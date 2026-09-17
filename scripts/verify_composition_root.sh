#!/usr/bin/env bash
# =============================================================================
#  scripts/verify_composition_root.sh —— 组合根护栏（C4.9 / R12）的"自证"（R1）
# =============================================================================
#  一个永远通过的护栏等于没有护栏。本脚本证明 `C4.9` 的护栏**确实能失败**：
#
#    ① 基线：当前源码树 → 护栏必须通过
#    ② 注入真实违规（`src/app/usecases/` 下新建文件构造具体实现）→ 必须失败
#    ③ 移除注入 → 必须恢复通过
#    ④ 反向注入：只写**类型引用/前向声明**（没有构造）→ 必须通过
#       （证明护栏不是"看到类型名就报"的粗粒度关键字匹配）
#
#  护栏本体在 `tests/unit/test_composition_root_guard.cpp`（它按源码树扫描，因此
#  注入无需重新编译）。
#
#  用法：scripts/verify_composition_root.sh [构建目录]
#  退出码：0 = 护栏有效；1 = 护栏无效（其"通过"结论不可信）
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build}"
INJECT="${REPO_ROOT}/src/app/usecases/_composition_root_selftest.cpp"
GUARD_BIN="${BUILD_DIR}/bin/test_composition_root_guard"
CASE_NAME="★ C4.9 具体实现只在组合根装配（R12）"

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

echo "组合根护栏自证（AGENTS.md R1：护栏必须能失败）"

# ---------------------------------------------------------------- ① 基线
if run_guard; then ok "① 基线：当前源码树通过"; else
  bad "① 基线不通过 —— 源码树里本来就有越权装配，先修它"; exit 1
fi

# ---------------------------------------------------------------- ② 注入真实违规
cat > "${INJECT}" <<'CXX'
// 自证用临时文件（scripts/verify_composition_root.sh 生成，检查后删除）
// 违规点：在应用层（非组合根）构造具体的内存适配器
#include "infra/blob/memory/memory_blob_store.h"
#include "common/time/clock.h"

namespace {
inline fss::infra::InMemoryBlobStore MakeFallbackStore(const fss::IClock& clock) {
  fss::infra::InMemoryBlobStore store(clock);
  return store;
}
}  // namespace
CXX
if run_guard; then
  bad "② 在应用层构造具体实现后护栏**仍然通过** —— 护栏无效"
  exit 1
else
  ok "② 注入越权装配后护栏失败（符合预期）"
fi

# ---------------------------------------------------------------- ③ 恢复
rm -f "${INJECT}"
if run_guard; then ok "③ 移除注入后护栏恢复通过"; else
  bad "③ 移除注入后仍失败 —— 清理不干净或有误报"; exit 1
fi

# ---------------------------------------------------------------- ④ 反向：引用不算构造
cat > "${INJECT}" <<'CXX'
// 自证用临时文件：只有类型引用 / 前向声明 / 参数类型，**没有任何构造**
#include "domain/ports/ports.h"

namespace {
//  指针与引用形态：不是构造，必须放行
inline fss::domain::IBlobStore* Pick(fss::domain::IBlobStore* first,
                                    fss::domain::IBlobStore& second) {
  (void)second;
  return first;
}
class Holder {
 public:
  explicit Holder(fss::domain::IFileLocationRepository& repo) : repo_(repo) {}

 private:
  fss::domain::IFileLocationRepository& repo_;
};
}  // namespace
CXX
if run_guard; then
  ok "④ 纯引用/声明形态被正确放行（护栏不是粗粒度关键字匹配）"
else
  bad "④ 纯引用形态被误判为越权装配 —— 护栏会误报"; exit 1
fi
rm -f "${INJECT}"

echo
printf '%s\n' "${C_GRN}组合根护栏有效（自证 ①~④ 全部符合预期）${C_OFF}"
info "结论：C4.9 的'通过'可信 —— 它能失败，也确实会失败。"
