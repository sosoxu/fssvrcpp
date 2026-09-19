#!/usr/bin/env bash
# =============================================================================
#  scripts/check_io_uring.sh —— io_uring 可用性探测（宿主机 + 容器 + 回退语义）
# =============================================================================
#  用途
#  ---------------------------------------------------------------------------
#  ADR-010 决定"阻塞线程池为默认、io_uring 为可选加速引擎"，并且启用 io_uring 的
#  前置条件 U1 是"目标部署环境允许 io_uring"。本脚本就是 U1 的执行体：
#  它在**宿主机**与**容器（默认 seccomp / unconfined）**两个环境里分别探测
#  io_uring_setup，并给出明确的启用建议。
#
#  为什么需要单独探测：io_uring 被阻断通常不是内核问题，而是**容器运行时的
#  seccomp profile**（Docker 自 2023 起默认屏蔽 io_uring_* 系统调用），
#  返回 EPERM。这一点在宿主机上测不出来。
#
#  用法
#  ---------------------------------------------------------------------------
#     scripts/check_io_uring.sh                # 自动选择镜像做容器探测
#     IMAGE=ubuntu:22.04 scripts/check_io_uring.sh
#     scripts/check_io_uring.sh --host-only    # 跳过容器探测
#
#  退出码：0 = 至少在一个目标环境可用；1 = 全不可用（则不要启用 io_uring）
#         2 = 环境不具备探测条件（无编译器 / 无 docker）—— 不算失败
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "${WORKDIR}"' EXIT

HOST_ONLY=0
[[ "${1:-}" == "--host-only" ]] && HOST_ONLY=1

C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
ok()   { printf '%s\n' "${C_GRN}  ✓${C_OFF} $*"; }
no()   { printf '%s\n' "${C_RED}  ✗${C_OFF} $*"; }
warn() { printf '%s\n' "${C_YEL}  !${C_OFF} $*"; }
info() { printf '%s\n' "${C_DIM}  ·${C_OFF} $*"; }

# -----------------------------------------------------------------------------
#  探针：直接调 io_uring_setup，打印 errno（区分 EPERM 与 ENOSYS）
#  只依赖 libc，不链接 liburing —— 因此容器里不需要装任何东西
# -----------------------------------------------------------------------------
cat > "${WORKDIR}/probe.c" <<'C'
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>
int main(void) {
  struct io_uring_params p;
  memset(&p, 0, sizeof(p));
  long r = syscall(__NR_io_uring_setup, 8, &p);
  if (r < 0) {
    const char* verdict;
    if (errno == EPERM)       verdict = "EPERM:被 seccomp/权限策略阻断";
    else if (errno == ENOSYS) verdict = "ENOSYS:内核不支持";
    else                      verdict = "其他错误";
    printf("UNAVAILABLE errno=%d(%s) %s\n", errno, strerror(errno), verdict);
    return 1;
  }
  printf("AVAILABLE fd=%ld features=0x%x\n", r, p.features);
  close((int)r);
  return 0;
}
C

if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then
  warn "找不到 C 编译器，无法构建探针"; exit 2
fi
CC="$(command -v cc || command -v gcc)"
"${CC}" -O2 "${WORKDIR}/probe.c" -o "${WORKDIR}/probe" 2>/dev/null || { warn "探针编译失败"; exit 2; }

echo "io_uring 可用性探测（ADR-010 前置条件 U1）"
info "内核: $(uname -r)"
if [[ -r /proc/sys/kernel/io_uring_disabled ]]; then
  v="$(cat /proc/sys/kernel/io_uring_disabled)"
  case "$v" in
    0) info "kernel.io_uring_disabled=0（内核允许）";;
    1) warn "kernel.io_uring_disabled=1（仅特权可用）";;
    2) warn "kernel.io_uring_disabled=2（内核完全禁用）";;
  esac
else
  info "kernel.io_uring_disabled 不存在（旧内核，无该开关）"
fi

HOST_OK=0; CONT_OK=0; CONT_SECCOMP_BLOCKED=0; CONT_RAN=0

# 判断 docker run 的输出到底是"探针的结论"还是"运行时没跑起来"。
# 只有前者才算容器环境的 io_uring 证据；后者是**无结论**（R4）。
probe_verdict() { [[ "$1" == *AVAILABLE* || "$1" == *"UNAVAILABLE errno="* ]]; }

echo
echo "① 宿主机"
if out="$("${WORKDIR}/probe" 2>&1)"; then ok "宿主机: $out"; HOST_OK=1; else no "宿主机: $out"; fi

if [[ ${HOST_ONLY} -eq 1 ]]; then
  echo; echo "（--host-only：跳过容器探测）"
else
  echo
  echo "② 容器（★ 这一步才是关键：默认 seccomp 常阻断 io_uring）"
  if ! command -v docker >/dev/null 2>&1; then
    warn "无 docker，跳过容器探测（环境不具备，不算失败）"
  elif ! docker info >/dev/null 2>&1; then
    warn "docker daemon 不可用，跳过容器探测"
  else
    #  ★ `| head -1` 会让 `docker images` 收到 SIGPIPE → pipefail 下整条管道 141（P7-D06）
    IMAGE="${IMAGE:-$(docker images --format '{{.Repository}}:{{.Tag}}' 2>/dev/null \
             | grep -v '^<none>' | sed -n '1p')}"
    if [[ -z "${IMAGE}" ]]; then
      warn "本地无可用镜像且不可联网拉取，跳过容器探测"
    else
      info "镜像: ${IMAGE}"
      #  ★ 必须显式 --entrypoint：像 fssvrcpp:verify 这类镜像自带 ENTRYPOINT，
      #    否则 `/w/probe` 会被当作**entrypoint 的参数**吞掉 → 探针根本没执行，
      #    脚本只能报"docker/runc 错误 → 无结论"（实测踩到；R4 的"无结论"不该由
      #    脚本自身的调用方式造成）。
      if out="$(docker run --rm --entrypoint /w/probe -v "${WORKDIR}:/w" -w /w "${IMAGE}" 2>&1)"; then
        if probe_verdict "$out"; then
          ok "容器(默认 seccomp): $out"
          CONT_OK=1; CONT_RAN=1
        else
          warn "容器(默认 seccomp): 探针未执行（不是 io_uring 的结论）→ $(printf '%s' "$out" | tail -1)"
        fi
      elif probe_verdict "$out"; then
        no "容器(默认 seccomp): $(printf '%s' "$out" | tail -1)"
        CONT_RAN=1
        [[ "$out" == *EPERM* ]] && CONT_SECCOMP_BLOCKED=1
      else
        warn "容器(默认 seccomp): 探针未执行（docker/runc 错误）→ $(printf '%s' "$out" | tail -1)"
      fi
      if out="$(docker run --rm --security-opt seccomp=unconfined --entrypoint /w/probe \
                 -v "${WORKDIR}:/w" -w /w "${IMAGE}" 2>&1)"; then
        if probe_verdict "$out"; then
          ok "容器(seccomp=unconfined): $out"
          [[ ${CONT_SECCOMP_BLOCKED} -eq 1 ]] && \
            info "  → 确认是 seccomp 拦的：放行后即可用"
        else
          warn "容器(seccomp=unconfined): 探针未执行（不是 io_uring 的结论）"
        fi
      elif probe_verdict "$out"; then
        no "容器(seccomp=unconfined): $(printf '%s' "$out" | tail -1)"
      else
        warn "容器(seccomp=unconfined): 探针未执行（docker/runc 错误）"
      fi
    fi
  fi
fi

# -----------------------------------------------------------------------------
#  回退语义：默认 blocking 引擎必须可用（不依赖 io_uring）
# -----------------------------------------------------------------------------
echo
echo "③ 回退路径（默认引擎）"
info "blocking 引擎 = pread/pwrite + 有界线程池；不依赖任何 io_uring 能力"
info "→ 因此下面的结论不影响默认部署：默认部署在**任何**环境都能跑"

echo
echo "结论"
if [[ ${HOST_OK} -eq 1 && ${CONT_OK} -eq 1 ]]; then
  ok "宿主机与容器均可用 → 满足 U1；仍需 U2（目标存储上收益 ≥1.5x）才建议启用"
  echo "    启用方式：io_engine: uring（或 auto）；并请确认容器 seccomp profile 已放行"
  exit 0
elif [[ ${HOST_OK} -eq 1 && ${CONT_SECCOMP_BLOCKED} -eq 1 ]]; then
  warn "仅宿主机可用，容器内不可用 → **不满足 U1**，不要启用 io_uring"
  echo "    这与 ADR-010 的实测一致：Docker 默认 seccomp profile 屏蔽 io_uring_*"
  echo "    若确需启用，必须自定义 seccomp profile 放行 io_uring_setup/enter/register"
  echo "    （或 seccomp=unconfined，但会显著削弱隔离，多数安全策略不允许）"
  exit 1
elif [[ ${HOST_OK} -eq 1 && ${CONT_RAN} -eq 0 ]]; then
  #  ★ 空证据防护：容器探测**根本没跑起来**（docker/runc 报错，或探针没被 exec），
  #    此时任何"容器内可用/不可用"的结论都是编的。按 R4 如实标注为**无结论**，
  #    并用退出码 2（环境不具备探测条件）与"真的被 seccomp 拦住"区分开。
  warn "宿主机可用；但容器探测**未执行**（docker/runc 未能运行探针）→ U1 **无结论**"
  echo "    不要据此判断容器内可用或不可用；请在有可用 docker 的环境重跑本脚本"
  exit 2
elif [[ ${HOST_OK} -eq 1 ]]; then
  warn "宿主机可用；容器内探针执行了但**失败原因不是 EPERM** → 不满足 U1"
  echo "    这属于非 seccomp 的失败（如内核/cgroup/权限），需单独排查"
  exit 1
else
  no "宿主机也不可用 → 不满足 U1，不要启用 io_uring"
  exit 1
fi
