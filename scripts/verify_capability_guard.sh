#!/usr/bin/env bash
# =============================================================================
#  scripts/verify_capability_guard.sh —— capabilities() 护栏的"自证"（R1）
# =============================================================================
#  C2.7 要求"capabilities() 只能在 location_issuer.cpp / storage_instruction_service.cpp
#  被调用"。一个永远通过的护栏等于没有护栏，因此本脚本证明它能失败：
#
#    ① 基线：当前源码树 → 护栏必须**通过**
#    ② 注入越权调用（src/app/services/ 下新建文件调用 store->capabilities()）
#       → 护栏必须**失败**
#    ③ 移除注入 → 护栏必须**恢复通过**
#    ④ 反向注入：只写**声明**（没有点/箭头）→ 必须**通过**（证明不是粗粒度关键字匹配）
#
#  用法：scripts/verify_capability_guard.sh [构建目录]
#  退出码：0 = 护栏有效；1 = 护栏无效（其"通过"结论不可信）
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build}"
INJECT="${REPO_ROOT}/src/app/services/_capability_guard_selftest.cpp"
GUARD_BIN="${BUILD_DIR}/bin/test_capability_guard"
CASE_NAME="★ C2.7 capabilities() 的调用点只在白名单里（含非空洞性断言）"

C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
ok()   { printf '%s\n' "${C_GRN}  ✓${C_OFF} $*"; }
bad()  { printf '%s\n' "${C_RED}  ✗${C_OFF} $*"; }
info() { printf '%s\n' "${C_DIM}  ·${C_OFF} $*"; }

cleanup() { rm -f "${INJECT}"; }
trap cleanup EXIT

run_guard() { "${GUARD_BIN}" "${CASE_NAME}" >/dev/null 2>&1; }

[[ -x "${GUARD_BIN}" ]] || {
  bad "找不到 ${GUARD_BIN}，请先构建（cmake --build ${BUILD_DIR}）"
  exit 1
}

echo "能力护栏自证（AGENTS.md R1：护栏必须能失败）"

# ---------------------------------------------------------------- ① 基线
if run_guard; then ok "① 基线：当前源码树通过"; else
  bad "① 基线不通过 —— 说明源码树里本来就有越权的 capabilities() 调用，先修它"; exit 1
fi

# ---------------------------------------------------------------- ② 注入越权调用
cat > "${INJECT}" <<'CXX'
// 自证用临时文件（scripts/verify_capability_guard.sh 生成，检查后删除）
#include "domain/ports/ports.h"

namespace {
inline bool ProbeRangeRead(fss::domain::IBlobStore& store) {
  return store.capabilities().range_read;
}
}  // namespace
CXX
if run_guard; then
  bad "② 在非白名单文件里调用 capabilities() 后护栏**仍然通过** —— 护栏无效"
  exit 1
else
  ok "② 注入越权调用后护栏失败（符合预期）"
fi

# ---------------------------------------------------------------- ③ 恢复
rm -f "${INJECT}"
if run_guard; then ok "③ 移除注入后护栏恢复通过"; else
  bad "③ 移除注入后仍失败 —— 清理不干净或有误报"; exit 1
fi

# ---------------------------------------------------------------- ④ 反向：声明不算调用
cat > "${INJECT}" <<'CXX'
// 自证用临时文件：只有声明/定义，没有对**某个对象**的调用 → 必须放行
#include "domain/ports/ports.h"

namespace {
inline fss::domain::BlobCapabilities DeclareOnly() { return fss::domain::BlobCapabilities{}; }
}  // namespace
CXX
if run_guard; then
  ok "④ 纯声明形态被正确放行（护栏不是粗粒度关键字匹配）"
else
  bad "④ 纯声明形态被误判为越权调用 —— 护栏会误报"; exit 1
fi
rm -f "${INJECT}"

echo
printf '%s\n' "${C_GRN}能力护栏有效（自证 ①~④ 全部符合预期）${C_OFF}"
info "结论：C2.7 的'通过'可信 —— 它能失败，也确实会失败。"
