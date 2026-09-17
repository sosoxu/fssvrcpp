#!/usr/bin/env bash
# =============================================================================
#  scripts/verify_http_hardening.sh —— H-2 防护的**生效性自证**（plan C1.2b / R1）
# =============================================================================
#  为什么需要它
#    `test_httplib_hardening` 全绿只能说明"当前代码通过"，**不能**说明这些用例真的
#    在防 H-2。所以本脚本故意把包装层的两道防护拆掉，然后要求测试**必须失败**，
#    并且失败症状必须是 H-2 的特征（超限却 201 / 413 变成别的码）。
#    这与 `scripts/verify_guard.sh` 是同一套思路：护栏/防线必须能失败。
#
#  做法：临时补丁 → 只重建该测试 → 断言失败 → 恢复 → 断言通过。
#       文件在退出时**必定**恢复（trap，含 Ctrl-C）。
#
#  退出码：0 = 自证成立；1 = 自证失败（防线拆掉后测试竟然还通过 → 用例是无效的）
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${FSS_BUILD_DIR:-${REPO_ROOT}/build}"
TARGET="${REPO_ROOT}/src/common/http/server.cpp"
BACKUP="$(mktemp)"

C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
ok()   { printf '%s\n' "${C_GRN}  ✓${C_OFF} $*"; }
no()   { printf '%s\n' "${C_RED}  ✗${C_OFF} $*"; }
info() { printf '%s\n' "${C_DIM}  ·${C_OFF} $*"; }

cleanup() {
  cp "${BACKUP}" "${TARGET}"
  rm -f "${BACKUP}"
}
trap cleanup EXIT INT TERM

echo "H-2 防护生效性自证（AGENTS.md R1：防线必须能失败）"

if [[ ! -f "${TARGET}" ]]; then
  no "找不到包装层源码：${TARGET}"
  exit 1
fi
cp "${TARGET}" "${BACKUP}"

# ---------------------------------------------------------------------------
#  ① 基线：现状必须全绿
# ---------------------------------------------------------------------------
if ! cmake --build "${BUILD_DIR}" -j"$(nproc)" --target test_httplib_hardening >/dev/null 2>&1; then
  no "基线构建失败"
  exit 1
fi
if ! "${BUILD_DIR}/bin/test_httplib_hardening" >/dev/null 2>&1; then
  no "基线测试未通过 —— 请先修好再谈'自证'"
  exit 1
fi
ok "① 基线：当前源码树通过"

# ---------------------------------------------------------------------------
#  ② 注入：拆掉 H-2 的两道防护
#     · 防护①（按 Content-Length 前置拒绝）→ 条件改成永假
#     · 防护②（计数 reader 超限立即中止）→ 改成"读完再说"
# ---------------------------------------------------------------------------
python3 - "${TARGET}" <<'PY'
import sys
path = sys.argv[1]
src = open(path, encoding='utf-8').read()

a = """    if (reader != nullptr && req.content_length >= 0 && route.options.max_body_bytes > 0 &&
        req.content_length > route.options.max_body_bytes) {"""
b = """    if (false && reader != nullptr && req.content_length >= 0 && route.options.max_body_bytes > 0 &&
        req.content_length > route.options.max_body_bytes) {"""
assert a in src, "找不到 H-2① 的前置拒绝分支（源码已改动？请同步更新本脚本）"
src = src.replace(a, b, 1)

c = """      exceeded = true;
      return false;  // ★ H-2②：立即中止，绝不是"读完再判断\""""
d = """      return true;  // 自证注入：故意禁用计数 reader 的中止"""
assert c in src, "找不到 H-2② 的中止分支（源码已改动？请同步更新本脚本）"
src = src.replace(c, d, 1)

open(path, 'w', encoding='utf-8').write(src)
PY
info "  → 已注入：H-2① 前置拒绝失效、H-2② 计数 reader 不再中止"

# ---------------------------------------------------------------------------
#  ③ 重建并运行：测试**必须**失败
# ---------------------------------------------------------------------------
if ! cmake --build "${BUILD_DIR}" -j"$(nproc)" --target test_httplib_hardening >/dev/null 2>&1; then
  no "注入后构建失败 —— 无法完成自证（请检查脚本的补丁是否仍匹配源码）"
  exit 1
fi

set +e
OUTPUT="$("${BUILD_DIR}/bin/test_httplib_hardening" "[phase1][http][c1.2]" 2>&1)"
RC=$?
set -e

if [[ ${RC} -eq 0 ]]; then
  no "② 拆掉 H-2 防护后测试**仍然通过** —— 说明这些用例防不住 H-2（R1 未满足）"
  exit 1
fi
ok "② 拆掉 H-2 防护后测试失败（符合预期）"

# 症状必须与 H-2 的特征一致：超限却成功（201）或状态码不对
if grep -qE '201 == 400|400 \(0x190\) == 413|201 == 413' <<<"${OUTPUT}"; then
  ok "③ 失败症状与 H-2 特征一致（超限却 201 / 413 退化），不是无关的偶发失败"
else
  no "③ 测试虽然失败，但症状与 H-2 无关 —— 请检查失败原因："
  #  ★ 同上：`head` 会 SIGPIPE 上游；用 sed 读满 20 行（P7-D06）
  grep -E "FAILED|with expansion:" -A 2 <<<"${OUTPUT}" | sed -n "1,20p"
  exit 1
fi

# ---------------------------------------------------------------------------
#  ④ 恢复：必须重新全绿
# ---------------------------------------------------------------------------
cp "${BACKUP}" "${TARGET}"
if ! cmake --build "${BUILD_DIR}" -j"$(nproc)" --target test_httplib_hardening >/dev/null 2>&1; then
  no "④ 恢复后构建失败"
  exit 1
fi
if ! "${BUILD_DIR}/bin/test_httplib_hardening" >/dev/null 2>&1; then
  no "④ 恢复后测试未通过 —— 源码可能没被正确还原"
  exit 1
fi
ok "④ 恢复防护后重新全绿"

echo
ok "H-2 防护有效（自证 ①~④ 全部符合预期）"
echo "  结论：test_httplib_hardening 的'通过'是可信的 —— 它能失败，也确实会失败。"
