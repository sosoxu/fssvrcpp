#!/usr/bin/env bash
# =============================================================================
#  scripts/verify_link_graph.sh —— C2.1：分层目标图的**链接行**校验
# =============================================================================
#  为什么不能只靠"编译期护栏"
#    分层违规有两种形态：
#      ① 源码级：`#include <grpcpp/...>` → 由 test_layering_guard 抓（已有）
#      ② **链接级**：某天有人给 `fss_app` 加上 `fss_http`（因为"顺手方便"），
#         源码里看不出任何违规 include，但依赖方向已经被破坏
#    本脚本读 CMake 生成的 link.txt，把**实际链接行**打印出来并与白名单比对。
#
#  白名单（docs/02-design.md §4）
#    fss_domain → fss_result fss_json fss_time fss_bytes fss_ids（+ 系统库）
#    fss_app    → fss_domain（+ 其传递依赖）
#  ★ 每新增一个 L1 模块都要在这里登记（`fss_metrics` 是 P9/C9.6 加的）：
#    白名单是"允许的依赖方向"的**唯一**机械表达，漏登记会让新模块被误判成越层依赖；
#    而随便加通配（如 `fss_*`）会让这条护栏变成恒真 —— 宁可每次多改一行。
#
#  退出码：0 = 通过；1 = 出现白名单外的依赖
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${FSS_BUILD_DIR:-${REPO_ROOT}/build}}"

C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
ok()   { printf '%s\n' "${C_GRN}  ✓${C_OFF} $*"; }
no()   { printf '%s\n' "${C_RED}  ✗${C_OFF} $*"; }
info() { printf '%s\n' "${C_DIM}  ·${C_OFF} $*"; }

FAILED=0

# 允许出现在各自链接行里的 fss_* 目标
ALLOWED_DOMAIN="fss_result fss_json fss_time fss_bytes fss_ids fss_crypto"
ALLOWED_APP="fss_domain fss_result fss_json fss_time fss_bytes fss_ids fss_crypto fss_metrics"

#  证据来源的选择（踩过一次坑）
#    ❌ 静态库的 `link.txt` 只包含 `ar qc libX.a *.o` —— **里没有任何依赖信息**，
#       用它做校验会"永远通过"，是**空证据**。
#    ✅ `CMakeFiles/<target>.dir/DependInfo.cmake` 里有该目标声明的 LINK_LIBRARIES（权威）。
#    ✅ 另外用"链接了 fss_domain 的可执行文件"的 link.txt 做**传递闭包**佐证：
#       若 fss_app 偷偷依赖 fss_http，那么连 fss_domain 的测试也会把它拉进来。
declared_deps() {
  local target="$1"
  local info
  #  ★ 不要用 `| head -1`：`head` 提前退出会给上游 `find` 发 SIGPIPE，
  #    在 `set -euo pipefail` 下整条管道返回 141 → 门槛**误报失败**（P7-D06）。
  #    取"第一行"用 `sed -n 1p`（读完全部输入，不提前关闭管道）。
  local candidates
  candidates="$(find "${BUILD_DIR}" -name DependInfo.cmake -path "*${target}.dir*" 2>/dev/null || true)"
  info="$(printf '%s\n' "${candidates}" | sed -n '1p')"
  [[ -f "${info}" ]] || return 1
  grep -oE 'fss_[a-z_]+' "${info}" | sort -u | grep -v "^${target}$" || true
}

check_target() {
  local target="$1"; shift
  local allowed="$1"; shift
  local deps
  if ! deps="$(declared_deps "${target}")"; then
    no "找不到 ${target} 的 DependInfo.cmake（build 目录是否已构建？）"
    FAILED=1
    return
  fi
  info "${target} 声明的 fss 依赖： $(echo ${deps} | tr '\n' ' ')"
  for dep in ${deps}; do
    local found=0
    for a in ${allowed}; do [[ "${dep}" == "${a}" ]] && found=1; done
    if [[ ${found} -eq 0 ]]; then
      no "${target} 依赖了白名单外的目标： ${dep}（依赖方向被破坏）"
      FAILED=1
    fi
  done
}

#  传递闭包佐证：**所有"不含传输适配器"的可执行文件**都不得把传输层/基础设施拉进来。
#
#  ★ 不要退化成"随便挑一个链接了 fss_domain 的可执行文件"：phase4 起的端到端测试
#    本来就链接 fss_http_adapter，一旦被挑中就会误报"依赖链被污染"。
#    判据必须**排除**合法链接适配器的二进制，再要求剩下的"纯分层"二进制干净。
closure_evidence() {
  local entry libs checked=0 violations=0 sample=""
  local -a links=()
  #  ★ 不用 `| head`：`head` 提前退出会给上游发 SIGPIPE → pipefail 下 141（P7-D06）
  mapfile -t links < <(find "${BUILD_DIR}/tests" -name link.txt -print 2>/dev/null | sort)
  for entry in "${links[@]}"; do
    libs="$(grep -oE 'libfss_[a-z_]+\.a' "${entry}" | sort -u | sed 's/^lib//; s/\.a$//' || true)"
    #  只看"确实链接了 L3/L4 且没有链接任一传输适配器"的可执行文件
    grep -qE '^(fss_domain|fss_app)$' <<<"${libs}" || continue
    grep -qE '^(fss_http_adapter|fss_grpc_adapter)$' <<<"${libs}" && continue
    checked=$((checked + 1))
    [[ -n "${sample}" ]] || sample="$(basename "$(dirname "${entry}")")"
    for forbidden in fss_http fss_proto; do
      if grep -qx "${forbidden}" <<<"${libs}"; then
        no "纯分层测试 ${sample} 链接了 ${forbidden} —— 说明 L3/L4 的依赖链被污染"
        violations=$((violations + 1))
      fi
    done
  done
  if [[ ${checked} -eq 0 ]]; then
    info "（未找到「纯分层」的可执行文件，跳过传递闭包佐证）"
    return
  fi
  if [[ ${violations} -eq 0 ]]; then
    info "传递闭包佐证：检查了 ${checked} 个纯分层可执行文件（如 ${sample}），均不含 fss_http/fss_proto"
  else
    info "传递闭包佐证：检查了 ${checked} 个纯分层可执行文件（如 ${sample}），其中 ${violations} 个被污染"
    FAILED=1
  fi
}

echo "分层链接图校验（C2.1，build=${BUILD_DIR}）"
check_target fss_domain "${ALLOWED_DOMAIN}"
check_target fss_app "${ALLOWED_APP}"
closure_evidence

[[ ${FAILED} -eq 0 ]] && ok "链接行只含允许的目标（L3→L1、L4→L3）"

if [[ ${FAILED} -ne 0 ]]; then
  echo "修复：检查 src/CMakeLists.txt 里 target_link_libraries(...) 是否引入了越层依赖"
  exit 1
fi
