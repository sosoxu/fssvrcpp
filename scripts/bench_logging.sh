#!/usr/bin/env bash
# =============================================================================
#  日志热路径基准（AGENTS.md R2 / R3 / R19）
# =============================================================================
#  独立进程 + taskset 绑核；每档取 3 轮最好值。
#  输出里同时给出"峰值负载下的单核占比"，用于回答"还值不值得继续优化"。
#
#  用法：scripts/bench_logging.sh [迭代数] [cpu]
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${FSS_BUILD_DIR:-${REPO_ROOT}/build}"
ITERS="${1:-200000}"
CPU="${2:-3}"
PEAK_RPS="${FSS_PEAK_RPS:-21062}"   # 实测峰值 req/s（docs/05；TCP_NODELAY=on）

BIN="${BUILD_DIR}/bench_logging"

if [[ ! -f "${BUILD_DIR}/lib/libfss_logging.a" ]]; then
  echo "错误：找不到 ${BUILD_DIR}/lib/libfss_logging.a" >&2
  echo "修复：cmake -S . -B build && cmake --build build -j\"\$(nproc)\"" >&2
  exit 1
fi

mkdir -p "${BUILD_DIR}"
g++ -std=c++20 -O2 -I "${REPO_ROOT}/src" -I "${REPO_ROOT}/third_party" \
    "${REPO_ROOT}/bench/logging_bench.cpp" -o "${BIN}" \
    "${BUILD_DIR}/lib/libfss_logging.a" "${BUILD_DIR}/lib/libfss_json.a" \
    "${BUILD_DIR}/lib/libfss_time.a" "${BUILD_DIR}/lib/libfss_result.a"

if ! command -v taskset >/dev/null 2>&1; then
  echo "警告：没有 taskset，无法绑核；本次数字只能作粗略参考（违反 R2）" >&2
  RUN=("${BIN}")
else
  RUN=(taskset -c "${CPU}" "${BIN}")
fi

echo "日志热路径基准（独立进程 + 绑核 cpu=${CPU}；每档 ${ITERS} 条 × 3 轮取最好）"
echo "sink=/dev/null；⚠️ 与 spdlog 的对照项不做 JSON/脱敏/逐行 flush，仅标出框架本身量级"
echo "------------------------------------------------------------------------"
RESULTS="$("${RUN[@]}" "${ITERS}")"
printf '%s\n' "${RESULTS}"
echo "------------------------------------------------------------------------"
# 只取"json + 脱敏 + 落盘"那一档换算预算占比。
# ⚠️ 不要用固定列号：标签本身含空格，$2 会是 "+"（写错过一次，算出来是 0）
printf '%s\n' "${RESULTS}" | awk -v peak="${PEAK_RPS}" '
  /落盘/ && /脱敏/ {
    for (i = 1; i <= NF; ++i) {
      if ($i == "ns/rec") { ns = $(i-1) + 0; break }
    }
    if (ns > 0) {
      printf "峰值负载下的单核占比（峰值 %d req/s）:\n", peak;
      printf "  %-10s %.0f ns/条 → 1 条/请求 %5.2f%% 单核 ；5 条/请求 %5.2f%% 单核\n",
             "本实现", ns, ns*1e-9*peak*100, ns*1e-9*peak*5*100;
      exit;
    }
  }'
echo
echo "判据（ADR-011 §2.3）：日志框架本身只占约 3% 的成本，故不值得为此引依赖。"
echo "回归阈值：若"json + 脱敏 + 落盘"相比 docs/test-evidence/phase1.md 记录恶化 > 2 倍，先查明原因再提交。"
