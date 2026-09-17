#!/usr/bin/env bash
# =============================================================================
#  scripts/run_sanitizers.sh —— C1.7：ASan + UBSan 全量构建下测试全绿
# =============================================================================
#  为什么单独一个构建目录
#    sanitizer 需要**全量重编译**（含 Catch2/httplib 所在的 TU），不能和普通构建混用，
#    否则一半带插桩一半不带，报告会漏。
#
#  一处**有范围**的例外
#    gRPC 1.30 头文件内部对未初始化 bool 的读取会命中 UBSan 的 bool 检查；GCC 11 的 libubsan
#    不支持按文件抑制，因此只对 test_phase0_toolchain **关闭 bool 检查**
#    （见 tests/CMakeLists.txt 里的说明）。ASan 与其它 UBSan 检查仍然全部生效。
#
#  判据（plan C1.7）：无内存/资源泄漏。因此：
#    · `-fno-sanitize-recover=all` → UBSan 命中即 abort（不"报告后继续"，否则会被忽略）
#    · LeakSanitizer 打开（ASan 在 Linux 上默认启用）→ 退出时报告泄漏并使进程非零退出
#
#  ⚠️ 为什么缩小大文件测试
#    ASan 的影子内存与 redzone 会显著抬高 RSS 基线，"1 GiB 流式 + RSS < 64 MiB"这条
#    断言在插桩下不再度量同一件事。因此本脚本用 FSS_TEST_BIG_BYTES 缩小到 64 MiB，
#    并放宽 RSS 上限；**1 GiB 的完整用例仍在普通构建的门槛里**（那是它的正确位置）。
#    这是刻意的取舍，不是"绕过失败"。
#
#  用法：scripts/run_sanitizers.sh            # 配置+构建+跑测试
#        FSS_ASAN_BUILD_DIR=build-asan2 ...
#  退出码：0 = 全绿；1 = 有失败
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${FSS_ASAN_BUILD_DIR:-${REPO_ROOT}/build-asan}"
# ⚠️ 并行度必须降低：带 sanitizer 插桩的 Catch2/httplib 单 TU 编译可占 1 GB+ 内存，
#    本机 16 核 / 7.7 GB 用 -j16 会把把 cc1plus 送进 OOM killer
#    （实测：`c++: fatal error: Killed signal terminated program cc1plus`）。
#    4 是本机验证过的安全值；内存更大的机器可以调高。
JOBS="${FSS_ASAN_JOBS:-4}"
BIG_BYTES="${FSS_ASAN_BIG_BYTES:-67108864}"      # 64 MiB
RSS_LIMIT_KIB="${FSS_ASAN_RSS_LIMIT_KIB:-524288}" # 512 MiB（插桩下的基线抬升）

C_GRN=$'\033[32m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
ok()   { printf '%s\n' "${C_GRN}  ✓${C_OFF} $*"; }
info() { printf '%s\n' "${C_DIM}  ·${C_OFF} $*"; }

SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"

echo "C1.7：ASan + UBSan 构建（${BUILD_DIR}）"
info "编译选项： ${SAN_FLAGS}"
info "并行度： -j${JOBS}（sanitizer 下单个 TU 可达 1 GB+，并行过高会 OOM）"

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="${SAN_FLAGS}" \
  -DCMAKE_C_FLAGS="${SAN_FLAGS}" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DFSS_WITH_PG=OFF >/dev/null

cmake --build "${BUILD_DIR}" -j"${JOBS}" >/dev/null
ok "构建完成"

export ASAN_OPTIONS="detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1:abort_on_error=0"
export UBSAN_OPTIONS="print_stacktrace=1"
export FSS_TEST_BIG_BYTES="${BIG_BYTES}"
export FSS_TEST_RSS_LIMIT_KIB="${RSS_LIMIT_KIB}"

#  标签集合从 run_all_gates.sh 的 IMPLEMENTED_PHASES 推导 —— 避免"加了阶段却忘了跑 sanitizer"
#
#  ⚠️ 这里踩过一次坑（P2-D07），与 C2.1 的"静态库 link.txt 是空证据"同类：
#     原实现是 `sed ... | tr -d ' '` 之后再 `for p in ${IMPLEMENTED}`。
#     `IMPLEMENTED_PHASES=(0 1 2)` 会先被压成单个词 `012`，于是标签变成
#     `phase012`（**根本不存在**），"全量 sanitizer"实际只跑了 phase0，
#     却照样打印"ASan + UBSan 全绿"。空集合上的通过 = 假证据。
#     修法：① 按数字切词；② 每个标签先核对测试数 > 0，为 0 直接判失败。
IMPLEMENTED_NUMS="$(sed -n 's/^IMPLEMENTED_PHASES=(\(.*\))/\1/p' "${REPO_ROOT}/scripts/run_all_gates.sh" | tr -cs '0-9' ' ')"
CTEST_LABEL_RE=""
for p in ${IMPLEMENTED_NUMS}; do
  CTEST_LABEL_RE="${CTEST_LABEL_RE}${CTEST_LABEL_RE:+|}phase${p}"
done
[[ -n "${CTEST_LABEL_RE}" ]] || CTEST_LABEL_RE="phase0"
info "覆盖标签： ${CTEST_LABEL_RE}"
info "阶段编号： ${IMPLEMENTED_NUMS:-（未解析到）}"

FAILED=0
for label in ${CTEST_LABEL_RE//|/ }; do
  # ★ 空证据防护：标签匹配到 0 个测试时，"通过"没有任何意义 → 直接判失败
  test_count="$(ctest --test-dir "${BUILD_DIR}" -N -L "${label}" 2>/dev/null | grep -cE '^[[:space:]]*Test +#')"
  if [[ "${test_count}" -eq 0 ]]; then
    printf '%s\n' "  ✗ ${label} 匹配到 0 个测试 —— 标签推导有误（空证据）"
    FAILED=1
    continue
  fi
  echo "--- ctest -L ${label}（sanitizer 构建，${test_count} 个测试）"
  if ctest --test-dir "${BUILD_DIR}" -L "${label}" --output-on-failure; then
    ok "${label} 在 sanitizer 下通过"
  else
    FAILED=1
  fi
done

if [[ ${FAILED} -ne 0 ]]; then
  echo "❌ sanitizer 构建下有失败 —— 定位方式："
  echo "   ctest --test-dir ${BUILD_DIR} -L phase1 -R <测试名> --output-on-failure"
  exit 1
fi
echo
ok "ASan + UBSan 全绿：无内存错误、无未定义行为、无泄漏（C1.7）"
