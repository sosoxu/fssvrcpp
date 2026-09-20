#!/usr/bin/env bash
# =============================================================================
#  scripts/check_nfs_semantics.sh —— C9.27：**目标共享存储**上的语义探针
# =============================================================================
#  为什么需要它：ADR-009 §6.2 把"共享 POSIX 存储的语义假设"列为**上生产前的硬前提**——
#  多实例设计依赖三件事，而它们**在 NFS 上与本地盘不同**：
#    ① `rename` 的原子性（ADR-008 的两阶段批提交靠它把 tmp 变成正式对象）；
#    ② close-to-open 一致性（一个实例写完关闭后，**另一个实例**立刻能读到新内容）；
#    ③ `syncfs` 的**文件系统级**范围（ADR-008 的第二阶段；共盘时会牵连他人）。
#  本机（开发环境）没有 NFS 挂载、无 root ⇒ **无法**在这里得出结论。因此本脚本是
#  "把判据交给目标环境执行"的那一半：在任何**真实共享挂载**上跑，输出即证据。
#
#  用法：  scripts/check_nfs_semantics.sh <共享挂载上的目录>
#          FSS_NFS_PROBE_DIR=/mnt/shared scripts/check_nfs_semantics.sh
#  退出码：0 = 全部通过 / 1 = 至少一条 FAIL / 2 = 环境不具备（**无结论**，不是通过）
#          ★ 与 scripts/check_io_uring.sh 同一约定（AGENTS §5）。
#
#  ★ 本脚本**只报告它能测的**：真实的断电耐久性、多主机并发、NFS 版本差异不在范围内，
#    会明确打印"（无结论）"，绝不折算成 PASS（R4）。
# =============================================================================
set -uo pipefail

C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
fails=0
unknowns=0
pass() { printf '  %s✓%s %s\n' "${C_GRN}" "${C_OFF}" "$*"; }
fail() { printf '  %s✗%s %s\n' "${C_RED}" "${C_OFF}" "$*" >&2; fails=$((fails + 1)); }
unk()  { unknowns=$((unknowns + 1)); printf '  %s?%s %s（无结论）\n' "${C_YEL}" "${C_OFF}" "$*"; }
# note()：**信息性**说明（本探针原理上无法测的项）—— 不计入"无结论"计数器，
# 否则一条永远测不了的项会让脚本永远退 2，"退出码"就失去意义（R4 的正确用法）。
note() { printf '  %s·%s %s\n' "${C_DIM}" "${C_OFF}" "$*"; }

TARGET="${1:-${FSS_NFS_PROBE_DIR:-}}"
if [[ -z "${TARGET}" ]]; then
  printf '用法: %s <共享挂载上的目录>\n' "$0" >&2
  exit 2
fi
if [[ ! -d "${TARGET}" ]]; then
  printf '目录不存在或不是目录: %s\n' "${TARGET}" >&2
  exit 2
fi

WORK="$(mktemp -d "${TARGET%/}/.fss_nfs_probe.XXXXXX" 2>/dev/null)" || {
  printf '无法在 %s 下创建临时目录（只读/无权限？）\n' "${TARGET}" >&2
  exit 2
}
cleanup() { rm -rf "${WORK}" 2>/dev/null; }
trap cleanup EXIT

printf '目标目录      : %s\n' "${TARGET}"
printf '文件系统类型  : %s\n' "$(stat -f -c %T "${TARGET}" 2>/dev/null || echo 未知)"
printf '挂载信息      : %s\n' "$(df -P "${TARGET}" 2>/dev/null | tail -1)"
if command -v findmnt >/dev/null 2>&1; then
  printf '挂载选项      : %s\n' "$(findmnt -no FSTYPE,OPTIONS --target "${TARGET}" 2>/dev/null || echo 未知)"
fi
printf '\n'

# -----------------------------------------------------------------------------
#  N1  rename 原子性：写者反复 `tmp → rename` 覆盖，读者必须**只**看到旧内容或新内容，
#      绝不允许"文件不存在"或"半截内容"（那正是 ADR-008 两阶段提交与 M1 的前提）。
# -----------------------------------------------------------------------------
printf '[N1] rename 原子性（读者轮询 vs 写者 200 轮 tmp→rename 覆盖）\n'
printf 'A' > "${WORK}/target.txt"
(
  i=0
  while [[ ${i} -lt 200 ]]; do
    printf 'A' > "${WORK}/.tmp.$$"
    mv -f "${WORK}/.tmp.$$" "${WORK}/target.txt"
    printf 'BBBB' > "${WORK}/.tmp.$$"
    mv -f "${WORK}/.tmp.$$" "${WORK}/target.txt"
    i=$((i + 1))
  done
) &
writer=$!
bad=0; seen=0; missing=0
while kill -0 "${writer}" 2>/dev/null; do
  if [[ -e "${WORK}/target.txt" ]]; then
    content="$(cat "${WORK}/target.txt" 2>/dev/null)"
    case "${content}" in
      A|BBBB) seen=$((seen + 1)) ;;
      *) bad=$((bad + 1)) ;;
    esac
  else
    missing=$((missing + 1))
  fi
done
wait "${writer}" 2>/dev/null
if [[ "${bad}" -eq 0 && "${missing}" -eq 0 && "${seen}" -gt 100 ]]; then
  pass "无缺失、无半截内容（有效采样 ${seen} 次）"
elif [[ "${seen}" -le 100 ]]; then
  unk "采样次数过少（${seen}）—— 读者没跑起来，判据不成立"
else
  fail "观察到 缺失 ${missing} 次 / 非期望内容 ${bad} 次（有效采样 ${seen}）"
fi

# -----------------------------------------------------------------------------
#  N2  close-to-open：**另一个进程**写完关闭后，本进程必须能读到新内容（NFS 属性缓存
#      可能让它延迟可见；这里给 10 秒上限并**报告实测延迟**）。
# -----------------------------------------------------------------------------
printf '[N2] close-to-open 一致性（独立进程写+关 → 本进程读）\n'
for gen in 1 2 3 4 5; do
  ( umask 022; printf 'gen-%s' "${gen}" > "${WORK}/cto.txt" )
  expect="gen-${gen}"
  t0="$(date +%s%3N)"; ok=0
  for _ in $(seq 1 200); do
    if [[ "$(cat "${WORK}/cto.txt" 2>/dev/null)" == "${expect}" ]]; then ok=1; break; fi
    sleep 0.05
  done
  t1="$(date +%s%3N)"
  if [[ "${ok}" -eq 1 ]]; then
    pass "第 ${gen} 代内容在 $((t1 - t0)) ms 内被另一进程看到"
  else
    fail "第 ${gen} 代内容 10 s 内未被另一进程看到（close-to-open 不成立）"
  fi
done

# -----------------------------------------------------------------------------
#  N3  `syncfs(fd)` 的范围：在**同一个文件系统**上，对 A 调 syncfs 之后，先前写的 B
#      必须能被**另一个进程**读到（这就是 ADR-008 第二阶段依赖的语义）。
#      ⚠️ 真正的"断电后仍在"无法在无 root 的环境验证 ⇒ 单独打印无结论（R4）。
# -----------------------------------------------------------------------------
printf '[N3] syncfs 的文件系统级范围（需 cc 编译一个小助手；无 cc 则无结论）\n'
if command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1; then
  CC="$(command -v cc || command -v gcc)"
  cat > "${WORK}/syncfs_probe.c" <<'EOF'
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
int main(int argc, char** argv) {
  if (argc < 2) return 2;
  int fd = open(argv[1], O_RDONLY);
  if (fd < 0) { perror("open"); return 3; }
  if (syncfs(fd) != 0) { perror("syncfs"); close(fd); return 4; }
  close(fd);
  return 0;
}
EOF
  if "${CC}" -O0 -o "${WORK}/syncfs_probe" "${WORK}/syncfs_probe.c" 2>"${WORK}/cc.log"; then
    printf 'B-payload' > "${WORK}/b_file"
    : > "${WORK}/a_file"
    if "${WORK}/syncfs_probe" "${WORK}/a_file"; then
      got="$(cat "${WORK}/b_file" 2>/dev/null)"
      [[ "${got}" == "B-payload" ]] \
        && pass "对 a_file 调 syncfs 后，独立可见 b_file 的内容（syncfs 覆盖整文件系统）" \
        || fail "syncfs 之后 b_file 内容异常：'${got}'"
    else
      fail "syncfs(fd) 调用失败（见上）"
    fi
    note "断电后仍在（耐久性）—— 需 root/断电控制，本探针原理上无法判定"
  else
    unk "cc 无法编译 syncfs 助手：$(tail -1 "${WORK}/cc.log" 2>/dev/null)"
  fi
else
  unk "没有 cc/gcc，无法测试 syncfs(fd) 的范围"
fi

# -----------------------------------------------------------------------------
#  N4  对照（**只报告，不判定**）：ADR-009 明确**不**用 NFS 文件锁做互斥。这里把
#      "跨进程 flock 到底像不像样"如实打印出来，供运维判断它有多不可靠。
# -----------------------------------------------------------------------------
printf '[N4] 对照：跨进程 flock 行为（仅报告；设计**不**依赖它）\n'
if command -v flock >/dev/null 2>&1; then
  ( flock -n 9 || exit 1; sleep 1 ) 9>"${WORK}/lock" &
  holder=$!
  sleep 0.2
  if flock -n 9 9>"${WORK}/lock"; then
    note "第二个进程**拿到**了锁 —— 本挂载上 flock 互斥不成立（设计已规避，故不判定）"
  else
    note "第二个进程被阻塞 —— flock 看起来有效（设计仍不依赖它，故不判定）"
  fi
  wait "${holder}" 2>/dev/null
else
  unk "没有 flock 命令"
fi

# -----------------------------------------------------------------------------
printf '\n'
if [[ "${fails}" -gt 0 ]]; then
  printf '%s结论：%d 条 FAIL%s（该挂载**不满足** ADR-009 的存储语义假设，不要用于多实例生产）\n' \
    "${C_RED}" "${fails}" "${C_OFF}"
  exit 1
fi
if [[ "${unknowns}" -gt 0 ]]; then
  printf '%s结论：无 FAIL，但有 %d 项无结论%s（不能当作通过；请人工确认后登记）\n' \
    "${C_YEL}" "${unknowns}" "${C_OFF}"
  exit 2
fi
printf '%s结论：全部通过%s（仍请在 docs/test-evidence/ 里登记实测输出）\n' "${C_GRN}" "${C_OFF}"
exit 0
