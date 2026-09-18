#!/usr/bin/env bash
# =============================================================================
#  scripts/bench_baseline.sh —— 容量基线（C9.4 / C9.11 / C9.13 / C9.15）
# =============================================================================
#  铁律与本脚本的对应关系
#  ---------------------------------------------------------------------------
#  · R2「性能测量必须独立进程 + 绑核」：服务端与负载生成器是**两个进程**，
#    且被 `taskset` 绑到**不同的核集合**（见下面 SERVER_CPUS / CLIENT_CPUS）。
#    本脚本**从不**在服务进程内压测（历史上那条错误结论就是这么来的）。
#  · R3「性能数字必须标注协议与安全性」：所有点位都是 HTTP/1.1 + 真实自签 token
#    校验（没有"跳过签名"的快捷模式）；输出里同时记录协议、并发、绑核。
#  · R4「无法区分的实验必须标注无结论」：每个点位都报 **client_cpu_pct**
#    （负载生成器自己消耗的 CPU）。若它接近客户端核数 ×100，说明该点位受
#    **客户端**限制，不能当作服务端容量 —— 脚本会打印警示。
#
#  诚实声明（务必随数字一起引用）
#  ---------------------------------------------------------------------------
#  · 本机为 **WSL2 + 虚拟盘**：绝对数字只是**本机量级参考**，
#    生产容量必须在目标硬件/网卡上复测（C9.14，本环境不具备条件）。
#  · 相对比较（同一台机器、同一绑核、同一负载生成器）是有效的：回归 <20% 的判据
#    与 ADR-006 的 A/B 倍数都建立在这个前提上。
#
#  用法
#  ---------------------------------------------------------------------------
#    scripts/bench_baseline.sh                 # 跑一遍，打印表格 + 与基线对比
#    scripts/bench_baseline.sh --save          # 跑一遍并**写入**基线文件
#    scripts/bench_baseline.sh --check         # 跑一遍，回归 >20% 直接退出码 1
#    FSS_BENCH_QUICK=1 scripts/bench_baseline.sh     # 缩短时长（冒烟用）
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${FSS_BUILD_DIR:-${REPO_ROOT}/build}"
BASE_FILE="${FSS_BENCH_BASELINE:-${REPO_ROOT}/docs/appendix/capacity-baseline/BASELINE.tsv}"
WORK_DIR="${FSS_BENCH_WORK:-${BUILD_DIR}/bench-run}"
PORT="${FSS_BENCH_PORT:-18099}"
PARTITION="opendes"
SECRET="bench-secret"
#  绑核：服务端 4 个核、客户端 8 个核，且**互不重叠**（否则量的是"自己抢自己"）
SERVER_CPUS="${FSS_BENCH_SERVER_CPUS:-4-7}"
CLIENT_CPUS="${FSS_BENCH_CLIENT_CPUS:-8-15}"
CLIENT_CORES=8
#  ★ 每个点位重复几次取**中位数**：本机（WSL2 虚拟盘、16 核共享）在高并发点位上的
#    逐次散布可达 ±25%（实测 control_read_c16 的 rps = 10989 / 13767 / 14308），
#    单次测量不足以支撑"退化 >20%"的判据 —— 中位数是抗噪的最小代价（R4）。
REPEATS="${FSS_BENCH_REPEATS:-3}"
DURATION_MS="${FSS_BENCH_DURATION_MS:-4000}"
WARMUP_MS="${FSS_BENCH_WARMUP_MS:-800}"
LARGE_BYTES="${FSS_BENCH_LARGE_BYTES:-67108864}"   # 64 MiB
SMALL_BYTES="${FSS_BENCH_SMALL_BYTES:-4096}"

MODE="run"
for arg in "$@"; do
  case "${arg}" in
    --save) MODE="save" ;;
    --check) MODE="check" ;;
    -h|--help) sed -n '2,40p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "未知参数：${arg}" >&2; exit 2 ;;
  esac
done
if [[ -n "${FSS_BENCH_QUICK:-}" ]]; then
  DURATION_MS=800; WARMUP_MS=200; LARGE_BYTES=16777216; REPEATS=1
fi

C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
log()  { printf '%s\n' "$*"; }
dim()  { printf '%s%s%s\n' "${C_DIM}" "$*" "${C_OFF}"; }
die()  { printf '%s%s%s\n' "${C_RED}" "$*" "${C_OFF}" >&2; exit 1; }

command -v taskset >/dev/null 2>&1 || die "缺少 taskset：无法绑核（违反 R2）。请安装 util-linux。"
[[ -x "${BUILD_DIR}/bin/fss_server" ]] || die "找不到 ${BUILD_DIR}/bin/fss_server：先 cmake --build build"

for target in fss_bench_capacity fss_bench_sqlite_write; do
  if [[ ! -x "${BUILD_DIR}/bin/${target}" ]]; then
    log "构建 ${target} …"
    cmake --build "${BUILD_DIR}" --target "${target}" >/dev/null
  fi
done

#  ★ 每次运行用**独立目录**，并在**脚本末尾**才清理旧目录。
#    为什么：早先版本在启动时 `rm -rf` 上一次运行的目录，而那一批是几万个小文件
#    （每个上传链迭代 = 1 个对象 + 2 条 DB 记录）——删除与回写会和随后的**写入点位**
#    （PUT / 上传链）抢盘，实测把 p99 从 7~44 ms 抬到 293~890 ms，于是"检查"看起来
#    像是产品退化。测量期**不得**与批量删除并发（R4）。
mkdir -p "${WORK_DIR}"
RUN_DIR="${WORK_DIR}/run-$(date +%s)"
rm -rf "${RUN_DIR}"
mkdir -p "${RUN_DIR}"
RESULTS_RAW="${WORK_DIR}/results_raw.tsv"
RESULTS_TSV="${WORK_DIR}/results.tsv"
: > "${RESULTS_RAW}"
: > "${RESULTS_TSV}"
SERVER_PID=""
cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

#  ---------------------------------------------------------------------------
#  起服务（独立进程 + 绑核）。`FSS_POSIX_DURABILITY` 由调用方按档位覆盖（C9.15）。
#  ---------------------------------------------------------------------------
start_server() {
  local durability="$1"
  local root="${RUN_DIR}/data-${durability}"
  rm -rf "${root}"
  mkdir -p "${root}"
  FSS_STORAGE_ROOT="${root}" \
  FSS_SQLITE_PATH="${root}/location.db" \
  FSS_METADATA_SQLITE_PATH="${root}/metadata.db" \
  FSS_TRANSFER_SECRET="${SECRET}" \
  FSS_HTTP_PORT="${PORT}" \
  FSS_BIND_ADDRESS="127.0.0.1" \
  FSS_SELF_BASE_URL="http://127.0.0.1:${PORT}/api/file" \
  FSS_AUTH_MODE="disabled" \
  FSS_POSIX_DURABILITY="${durability}" \
  FSS_POSIX_FSYNC_THRESHOLD_BYTES="${FSS_BENCH_FSYNC_THRESHOLD:-1048576}" \
    taskset -c "${SERVER_CPUS}" "${BUILD_DIR}/bin/fss_server" > "${RUN_DIR}/server-${durability}.log" 2>&1 &
  SERVER_PID=$!   # ★ 不用 $(...)：命令替换会开子 shell，PID 会变成孤儿（AGENTS 陷阱清单）
  local waited=0
  while ! curl -sf -o /dev/null "http://127.0.0.1:${PORT}/api/file/v2/readiness_check"; do
    sleep 0.2
    waited=$((waited + 1))
    if [[ ${waited} -gt 100 ]]; then
      cat "${RUN_DIR}/server-${durability}.log" >&2
      die "服务端 20 秒内没有就绪"
    fi
  done
}

stop_server() {
  cleanup
  SERVER_PID=""
}

BASE_URL="http://127.0.0.1:${PORT}/api/file"

#  ---------------------------------------------------------------------------
#  记录一条结果（label, metric, value）—— 基线文件是 TSV，便于 diff 与对比
#  ---------------------------------------------------------------------------
record() { printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> "${RESULTS_RAW}"; }

#  解析 load/chain 的 RESULT 行并落盘
run_load() {   # run_load <label> <mode> <url> <conns> [extra args...]
  local label="$1" mode="$2" url="$3" conns="$4"; shift 4
  local out r
  for ((r = 1; r <= REPEATS; ++r)); do
    out="$(taskset -c "${CLIENT_CPUS}" "${BUILD_DIR}/bin/fss_bench_capacity" load \
          --label "${label}" --mode "${mode}" --url "${url}" --connections "${conns}" \
          --duration-ms "${DURATION_MS}" --warmup-ms "${WARMUP_MS}" "$@" 2>&1)" || true
    printf '%s\n' "${out}"
  awk -v label="${label}" '
    /^RESULT/ {
      for (i = 1; i <= NF; ++i) {
        split($i, kv, "=")
        v[kv[1]] = kv[2]
      }
      printf "%s\trps\t%s\n%s\tmibps\t%s\n%s\tp99_us\t%s\n%s\tp50_us\t%s\n%s\terrs\t%s\n%s\tclient_cpu_pct\t%s\n%s\treqs\t%s\n",
             label, v["rps"], label, v["mibps"], label, v["p99_us"], label, v["p50_us"],
             label, v["errs"], label, v["client_cpu_pct"], label, v["reqs"]
    }' <<<"${out}" >> "${RESULTS_RAW}"
  done
}

run_chain() {  # run_chain <label> <conns>
  local label="$1" conns="$2"
  local out r
  for ((r = 1; r <= REPEATS; ++r)); do
    out="$(taskset -c "${CLIENT_CPUS}" "${BUILD_DIR}/bin/fss_bench_capacity" chain \
          --label "${label}" --base-url "${BASE_URL}" --partition "${PARTITION}" \
          --auth-token "bench-token" --connections "${conns}" --body-bytes "${SMALL_BYTES}" \
          --duration-ms "${DURATION_MS}" --warmup-ms "${WARMUP_MS}" 2>&1)" || true
    printf '%s\n' "${out}"
  awk -v label="${label}" '
    /^RESULT/ {
      for (i = 1; i <= NF; ++i) { split($i, kv, "="); v[kv[1]] = kv[2] }
      printf "%s\tfiles_s\t%s\n%s\tp99_us\t%s\n%s\terrs\t%s\n%s\tclient_cpu_pct\t%s\n",
             label, v["files_s"], label, v["p99_us"], label, v["errs"], label, v["client_cpu_pct"]
    }' <<<"${out}" >> "${RESULTS_RAW}"
  done
}

#  ---------------------------------------------------------------------------
#  主流程
#  ---------------------------------------------------------------------------
log "容量基线（C9.4/C9.11/C9.13/C9.15）"
log "  协议        : HTTP/1.1 keep-alive + 真实自签 token 校验（无「跳过签名」模式）"
log "  服务端绑核  : ${SERVER_CPUS}    客户端绑核: ${CLIENT_CPUS}（客户端核数 ${CLIENT_CORES}）"
log "  每点位时长  : ${DURATION_MS} ms（预热 ${WARMUP_MS} ms 不计入）"
log "  ⚠️ WSL2 + 虚拟盘：绝对数字仅作本机量级参考（C9.14 需在目标硬件复测）"
log "  顺序        : 先量 SQLite（安静机器）→ 起服务 → 播种 → 控制面/数据面/上传链/大文件"
log "------------------------------------------------------------------------"

#  ---- ⑦ SQLite 写吞吐（独立进程；仓储层单独量）----
#  ★ 为什么**放在最前面**（在起服务与压测之前）：实测把它放在最后时会被"前面点位留下的
#    脏页回写"污染 —— 同一命令在安静机器上稳定 2014~2488 writes/s，而在压测末尾量到
#    537 writes/s（-77%），那是**替前面的写入收尾**，不是仓储变慢（R4：这种数字不许当证据）。
sync || true
for threads in 1 8; do
  for ((r = 1; r <= REPEATS; ++r)); do
    cpu_line="$(taskset -c "${CLIENT_CPUS}" "${BUILD_DIR}/bin/fss_bench_sqlite_write" 8000 \
                "${threads}" "${RUN_DIR}/sqlite-${threads}-${r}" 2>&1 | tail -1)"
    printf 'SQLITE threads=%s run=%s %s\n' "${threads}" "${r}" "${cpu_line}"
    value="$(printf '%s' "${cpu_line}" | sed -n 's/.*writes_per_sec=\([0-9]*\).*/\1/p')"
    record "sqlite_write_t${threads}" writes_per_sec "${value:-0}"
  done
done


start_server "per_file"

#  ---- 播种：一个小对象 + 一个大对象（走真实 API） ----
SEED_OUT="$(taskset -c "${CLIENT_CPUS}" "${BUILD_DIR}/bin/fss_bench_capacity" seed \
            --base-url "${BASE_URL}" --partition "${PARTITION}" \
            --large-bytes "${LARGE_BYTES}" 2>&1)" || { printf '%s\n' "${SEED_OUT}"; die "播种失败"; }
printf '%s\n' "${SEED_OUT}" | while IFS= read -r line; do dim "  ${line}"; done
#  ★ 用 `sed -n 1p` 而不是 `head -1`：head 提前退出会给上游发 SIGPIPE，
#    在 `set -euo pipefail` 下整条管道返回 141 → 脚本"因为不相干的原因"失败（P7-D06）。
field() { printf '%s\n' "${SEED_OUT}" | sed -n "s/.*$1=\([^ ]*\).*/\1/p" | sed -n 1p; }
SMALL_GET="$(field small_get_url)"
SMALL_PUT="$(field small_put_url)"
LARGE_GET="$(field large_get_url)"
RECORD_ID="$(field record_id)"
[[ -n "${SMALL_GET}" && -n "${SMALL_PUT}" && -n "${LARGE_GET}" && -n "${RECORD_ID}" ]] ||
  die "没有取到播种结果"

#  ★ 播种写入了大对象（几十~几百 MiB）。在测量前 `sync`：否则**上一个阶段留下的脏页
#    回写**会与后续写入点位（PUT / 上传链 / SQLite）抢盘，把数字压低——实测 SQLite 8 线程
#    出现过 654 writes/s（手动重复 7 次为 2014~2488）就是被播种回写干扰的离群点。
sync || true

AUTH_ARGS=(--header "authorization: Bearer bench-token" --header "data-partition-id: ${PARTITION}")

#  ---- ① 控制面小请求（含鉴权 + JSON + SQLite 读） ----
for conns in 1 4 16 64; do
  run_load "control_read_c${conns}" get \
    "${BASE_URL}/v2/files/${RECORD_ID}/metadata" "${conns}" "${AUTH_ARGS[@]}"
done
#  ---- ② 数据面小对象 GET（自签 token 校验 + POSIX 读 + 内容提供者） ----
for conns in 1 16; do
  run_load "data_small_get_c${conns}" get "${SMALL_GET}" "${conns}" \
    --header "data-partition-id: ${PARTITION}"
done
#  ---- ③ 数据面 PUT（4 KiB 覆盖同一个键；token 校验 + POSIX 写 + rename） ----
for conns in 1 16; do
  run_load "data_put_c${conns}" put "${SMALL_PUT}" "${conns}" \
    --header "data-partition-id: ${PARTITION}" --body-bytes "${SMALL_BYTES}"
done
#  ---- ④ 大文件单流吞吐（一次一个连接，反复整份下载） ----
run_load "large_stream_c1" get "${LARGE_GET}" 1 --header "data-partition-id: ${PARTITION}"
run_load "large_stream_c4" get "${LARGE_GET}" 4 --header "data-partition-id: ${PARTITION}"
#  ---- ⑤ 小文件上传端到端（C9.15）：uploadURL → PUT → POST metadata ----
for conns in 1 8; do
  run_chain "upload_chain_c${conns}" "${conns}"
done
#  ---- ⑥ 每档 fsync 策略的端到端上传吞吐（C9.15 的"各档位差异"） ----
for durability in per_file batch never; do
  stop_server
  start_server "${durability}"
  run_chain "upload_chain_${durability}_c4" 4
done
stop_server

#  ---- 聚合：每 (label, metric) 取**中位数**，并打印 >20% 的散布（R4：噪声不许当证据）----
python3 - "${RESULTS_RAW}" "${RESULTS_TSV}" <<'PY_AGG'
import statistics
import sys
from collections import defaultdict

raw, out = sys.argv[1], sys.argv[2]
values = defaultdict(list)
order = []
with open(raw, encoding="utf-8") as handle:
    for line in handle:
        line = line.rstrip("\n")
        if not line:
            continue
        label, metric, value = line.split("\t")
        key = (label, metric)
        if key not in values:
            order.append(key)
        try:
            values[key].append(float(value))
        except ValueError:
            values[key].append(0.0)
with open(out, "w", encoding="utf-8") as handle:
    for key in order:
        handle.write("%s\t%s\t%.1f\n" % (key[0], key[1], statistics.median(values[key])))
print("  聚合：%d 个 (label,metric)，各取 %d 次的中位数" % (len(order), len(values[order[0]])))
for key in order:
    samples = values[key]
    if len(samples) > 1 and key[1] in ("rps", "mibps", "files_s", "writes_per_sec"):
        median = statistics.median(samples)
        spread = (max(samples) - min(samples)) / max(1e-9, median) * 100.0
        if spread > 20.0:
            print("  ⚠ 逐次散布 >20%%：%s/%s %s（中位数 %.0f）" %
                  (key[0], key[1], ["%.0f" % s for s in samples], median))
PY_AGG

#  ---- 结果表 ----
log "------------------------------------------------------------------------"
column -t -s $'\t' "${RESULTS_TSV}" | sed 's/^/  /'
log "------------------------------------------------------------------------"

#  ---------------------------------------------------------------------------
#  ① p99 绝对阈值（C9.4）。阈值的作用是抓**病态**（例如 100 ms 级 p99 或长尾爆炸），
#    不是"生产 SLA"：本机是 WSL2 + 虚拟盘，生产阈值必须在目标硬件上重新推导（C9.14）。
#    阈值取基线最差点位的 ~2.5 倍余量，既不会被噪声打穿，也不会对退化成摆设。
#  ---------------------------------------------------------------------------
p99_limit_us() {
  case "$1" in
    control_read_*)   echo 50000 ;;   # 基线最差 19123 µs（c64）
    data_small_get_*) echo 20000 ;;   # 基线最差  3109 µs
    data_put_*)       echo 60000 ;;   # 基线最差 14228 µs
    large_stream_*)   echo 400000 ;;  # 基线最差 77289 µs
    upload_chain_*)   echo 200000 ;;  # 基线最差 49810 µs
    *)                echo 0 ;;
  esac
}
P99_FAIL=0
log "p99 阈值检查（C9.4）"
while IFS=$'\t' read -r label metric value; do
  [[ "${metric}" == "p99_us" ]] || continue
  limit="$(p99_limit_us "${label}")"
  [[ "${limit}" -gt 0 ]] || continue
  if awk -v v="${value}" -v l="${limit}" 'BEGIN { exit !(v > l) }'; then
    printf '  %s✗ %-26s p99=%s µs > 阈值 %s µs%s\n' "${C_RED}" "${label}" "${value}" "${limit}" "${C_OFF}"
    P99_FAIL=1
  else
    printf '  %s✓%s %-26s p99=%s µs ≤ %s µs\n' "${C_GRN}" "${C_OFF}" "${label}" "${value}" "${limit}"
  fi
done < "${RESULTS_TSV}"
[[ "${P99_FAIL}" -eq 0 ]] || die "p99 超过阈值（见上）"
log "------------------------------------------------------------------------"

if [[ ! -f "${BASE_FILE}" && "${MODE}" != "save" ]]; then
  dim "  （还没有基线文件 ${BASE_FILE}；用 --save 生成后再跑即可做回归对比）"
fi
if [[ -f "${BASE_FILE}" && "${MODE}" != "save" ]]; then
  log "与基线对比（${BASE_FILE}；判据：吞吐退化 >20% 即失败）"
  #  ★ 判据的**适用范围**（两次实测踩出来的，不是想当然）：
  #    ① 20% 退化判据只对**吞吐类**指标生效（rps / mibps / files_s / writes_per_sec）——
  #       C9.11 的原话就是"相对基线退化 >20%"，而这要求"越大越好"的极性。
  #    ② 延迟类指标（p50/p99）**不**用 20% 带宽判：本机上高并发点位的 p99 逐次散布就有
  #       ±25%（实测 c16 p99 = 4346 / 3185 µs）。延迟由 **C9.4 的绝对阈值**兜（带 2.5x 余量），
  #       这里只把差值**打印**出来供人看。
  #    ③ **fsync/磁盘类**吞吐点位（data_put_* / upload_chain_* / sqlite_write_*）在 WSL2
  #       虚拟盘上的散布 >20%（实测 per_file c4 = 78.6 / 96.4 files/s）→ 只告警不判失败；
  #       该判据要在真实存储上才具备判定条件（C9.14，已登记为未验证）。
  awk -F'\t' -v red="${C_RED}" -v grn="${C_GRN}" -v yel="${C_YEL}" -v off="${C_OFF}" '
    function higher_is_better(m) {
      return (m == "rps" || m == "mibps" || m == "files_s" || m == "writes_per_sec")
    }
    NR == FNR { base[$1 "\t" $2] = $3; next }
    {
      key = $1 "\t" $2
      if (!(key in base)) { printf "  %s新增点位%s %s = %s\n", off, off, $1 "/" $2, $3; next }
      b = base[key] + 0; c = $3 + 0
      if (b <= 0) next
      delta = (c - b) / b * 100.0
      noisy = ($1 ~ /^(data_put_|upload_chain_|sqlite_write_)/)
      #  报告型：本机**不可复现**的点位（单线程 SQLite 写实测散布 359~1482 writes/s，
      #  虚拟盘上的 fsync 路径在共享主机上无法稳定），只打印、不参与任何判定
      report_only = ($1 == "sqlite_write_t1")
      if (report_only) {
        printf "  %s·%s %-34s %-14s %+6.1f%%（基线 %.0f → 现在 %.0f；报告型：本机不可复现，不判定）\n",
               off, off, $1, $2, delta, b, c
        next
      }
      if (!higher_is_better($2)) {
        printf "  %s·%s %-34s %-14s %+6.1f%%（基线 %.0f → 现在 %.0f；延迟由 C9.4 绝对阈值判定）\n",
               off, off, $1, $2, delta, b, c
        next
      }
      if (delta < -20.0 && noisy) {
        printf "  %s⚠ %s/%s 退化 %.1f%%（基线 %.0f → 现在 %.0f）—— WSL2 上 fsync 类点位散布 >20%%，仅告警（C9.14 上可判定）%s\n",
               yel, $1, $2, delta, b, c, off
      } else if (delta < -20.0) {
        printf "  %s✗ %s/%s 退化 %.1f%%（基线 %.0f → 现在 %.0f）%s\n", red, $1, $2, delta, b, c, off
        fail = 1
      } else if (delta > 20.0) {
        printf "  %s↑%s %-34s %-14s %+6.1f%%（基线 %.0f → 现在 %.0f；提升 >20%%，建议重新 --save 基线）\n",
               grn, off, $1, $2, delta, b, c
      } else {
        printf "  %s✓%s %-34s %-14s %+6.1f%%（基线 %.0f → 现在 %.0f）\n", grn, off, $1, $2, delta, b, c
      }
    }
    END { exit (fail ? 3 : 0) }' "${BASE_FILE}" "${RESULTS_TSV}" || CHECK_RC=$?
  CHECK_RC="${CHECK_RC:-0}"
  if [[ "${CHECK_RC}" == "3" ]]; then
    die "容量基线回归超过 20%（见上）"
  fi
fi

if [[ "${MODE}" == "save" ]]; then
  mkdir -p "$(dirname "${BASE_FILE}")"
  {
    printf '# fssvrcpp 容量基线（C9.11）\n'
    printf '# 生成：scripts/bench_baseline.sh --save\n'
    printf '# 环境：%s；服务端绑核 %s；客户端绑核 %s；时长 %s ms × %s 次（每点位取中位数）\n' \
           "$(uname -sr)" "${SERVER_CPUS}" "${CLIENT_CPUS}" "${DURATION_MS}" "${REPEATS}"
    printf '# ⚠️ WSL2 + 虚拟盘：绝对数字仅作**本机量级参考**；相对回归（<20%%）与 A/B 倍数才有效\n'
    printf '# 协议：HTTP/1.1 keep-alive；数据面真实自签 token 校验（无跳过签名模式）\n'
    printf '# 列：label\tmetric\tvalue\n'
    cat "${RESULTS_TSV}"
  } > "${BASE_FILE}"
  log "${C_GRN}基线已写入${C_OFF} ${BASE_FILE}"
fi

if [[ "${MODE}" == "check" ]]; then
  log "${C_GRN}容量回归检查通过（无点位退化 >20%）${C_OFF}"
fi

#  ---- 清理历史运行目录（保留最近 1 个）----
#  放在**所有测量之后**：批量删除不得与测量并发（见文件上部 RUN_DIR 的说明）
mapfile -t old_runs < <(ls -1dt "${WORK_DIR}"/run-* 2>/dev/null | sed -n '2,$p')
if [[ ${#old_runs[@]} -gt 0 ]]; then
  printf '%s\n' "${old_runs[@]}" | while IFS= read -r old; do rm -rf "${old}"; done
  dim "  已清理 ${#old_runs[@]} 个历史运行目录（${WORK_DIR}）"
fi
