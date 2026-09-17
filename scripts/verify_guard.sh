#!/usr/bin/env bash
# =============================================================================
#  scripts/verify_guard.sh —— 分层护栏的"自证"（AGENTS.md 铁律 R1）
# =============================================================================
#  一个永远通过的护栏测试等于没有护栏。本脚本证明 test_layering_guard 能失败：
#
#    ① 基线：当前源码树 → 护栏必须**通过**
#    ② 注入违规（src/domain/ 里 include <grpcpp/grpcpp.h>）→ 护栏必须**失败**
#    ③ 移除违规 → 护栏必须**恢复通过**
#    ④ 反向注入（src/common/http/ 里 include httplib.h，属**允许**）→ 必须**通过**
#       —— 证明护栏不是"见 httplib 就报错"的粗粒度检查
#
#  用法：scripts/verify_guard.sh [构建目录]
#  退出码：0 = 护栏有效；1 = 护栏无效（其"通过"结论不可信）
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build}"
INJECT_DOMAIN="${REPO_ROOT}/src/domain/_guard_selftest_injected.h"
INJECT_HTTP_DIR="${REPO_ROOT}/src/common/http"
INJECT_HTTP="${INJECT_HTTP_DIR}/_guard_selftest_injected.h"
GUARD_BIN="${BUILD_DIR}/bin/test_layering_guard"

C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
ok()   { printf '%s\n' "${C_GRN}  ✓${C_OFF} $*"; }
bad()  { printf '%s\n' "${C_RED}  ✗${C_OFF} $*"; }
info() { printf '%s\n' "${C_DIM}  ·${C_OFF} $*"; }

cleanup() {
  rm -f "${INJECT_DOMAIN}" "${INJECT_HTTP}"
  # 若 common/http 是本次为自证而创建的，移除空目录
  [[ -d "${INJECT_HTTP_DIR}" ]] && rmdir "${INJECT_HTTP_DIR}" 2>/dev/null || true
}
trap cleanup EXIT

# 只跑"源码树中不存在分层违规"这一个用例
run_guard() {
  "${GUARD_BIN}" "源码树中不存在分层违规" >/dev/null 2>&1
}

[[ -x "${GUARD_BIN}" ]] || {
  bad "找不到 ${GUARD_BIN}，请先构建（cmake --build ${BUILD_DIR}）"
  exit 1
}

echo "分层护栏自证（AGENTS.md R1：护栏必须能失败）"

# ---------------------------------------------------------------- ① 基线
if run_guard; then ok "① 基线：当前源码树通过"; else
  bad "① 基线不通过 —— 源码树里本来就存在分层违规，先修它"; exit 1
fi

# ---------------------------------------------------------------- ② 注入违规
cat > "${INJECT_DOMAIN}" <<'H'
// 自证用临时文件（scripts/verify_guard.sh 生成，检查后删除）
#pragma once
#include <grpcpp/grpcpp.h>
H
if run_guard; then
  bad "② 注入 'domain/ 里 include <grpcpp/grpcpp.h>' 后护栏**仍然通过** —— 护栏无效"
  echo "     护栏无法发现违规，其结论不可信。"
  exit 1
else
  ok "② 注入领域层违规后护栏失败（符合预期）"
fi

# 再验证 httplib 规则（这是 ADR-002 明确要求的一条）
cat > "${INJECT_DOMAIN}" <<'H'
// 自证用临时文件
#pragma once
#include <httplib.h>
H
if run_guard; then
  bad "②b 注入 'domain/ 里 include <httplib.h>' 后护栏仍然通过 —— 护栏无效"; exit 1
else
  ok "②b 注入领域层 httplib 违规后护栏失败（符合预期）"
fi
rm -f "${INJECT_DOMAIN}"

# ---------------------------------------------------------------- ③ 恢复
if run_guard; then ok "③ 移除违规后护栏恢复通过"; else
  bad "③ 移除违规后护栏仍失败 —— 清理不干净或有误报"; exit 1
fi

# ---------------------------------------------------------------- ④ 反向：豁免位置必须放行
mkdir -p "${INJECT_HTTP_DIR}"
cat > "${INJECT_HTTP}" <<'H'
// 自证用临时文件：httplib 在 fss_http 包装层内是**允许**的
#pragma once
#include <httplib.h>
H
if run_guard; then
  ok "④ common/http/ 内 include httplib 被正确放行（护栏不是粗粒度关键字匹配）"
else
  bad "④ common/http/ 内 include httplib 被误判为违规 —— 护栏豁免规则失效"; exit 1
fi
rm -f "${INJECT_HTTP}"

# ---------------------------------------------------------------- ⑤ 符号泄漏
cat > "${INJECT_DOMAIN}" <<'H'
// 自证用临时文件：不经 include 而直接使用第三方符号也必须被检出
#pragma once
inline void f() { httplib::Server s; }
H
if run_guard; then
  bad "⑤ 领域层直接使用 httplib:: 符号却未被检出 —— 符号级检查失效"; exit 1
else
  ok "⑤ 领域层直接使用 httplib:: 符号被检出（符合预期）"
fi
rm -f "${INJECT_DOMAIN}"

echo
printf '%s\n' "${C_GRN}分层护栏有效（自证 ①~⑤ 全部符合预期）${C_OFF}"
echo "  结论：护栏的'通过'是可信的 —— 它能失败，也确实会失败。"
