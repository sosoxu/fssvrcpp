#!/usr/bin/env bash
# =============================================================================
#  scripts/bench_sendfile_ab.sh —— ADR-006 受控复核（判据 C9.12）
# =============================================================================
#  问题：产品数据面走 cpp-httplib 的 `set_content_provider`（64 KiB 一块的
#  `pread` + `write`）。是否值得为集中存储再实现一条 `sendfile(2)` 数据面？
#
#  判据（docs/04-implementation-plan.md 的 C9.12）：
#    用**同一个负载生成器**对比两条路径，得出可复现的倍数；
#    倍数 ≥ 1.5x → 值得实现 sendfile 数据面；< 1.5x → 放弃，统一用 httplib。
#
#  铁律与本脚本的对应关系
#  ---------------------------------------------------------------------------
#  · R2「性能测量必须独立进程 + 绑核」：服务端（fss_bench_sendfile_ab）与客户端
#    （fss_bench_capacity load）是**两个进程**，且被 `taskset` 绑到**互不重叠**的
#    核集合（SERVER_CPUS / CLIENT_CPUS，脚本启动时做重叠断言）。
#  · R3「性能数字必须标注协议与安全性」：两侧都是 HTTP/1.1 + 显式 Content-Length、
#    **无 chunked**、keep-alive、双端 TCP_NODELAY。sendfile 侧是**原型**：
#    只支持 `GET /blob`，无 Range/鉴权/限流/日志/超时/连接上限 →
#    它测的是 **sendfile 路径的上界**，不是一个可上线的数据面。
#  · R4「无法区分的实验必须标注无结论」：报告每个点位的 `client_cpu_pct`；
#    若它接近"客户端核数 × 100"，该点位标注 **[受客户端限制]**。
#
#  用法
#  ---------------------------------------------------------------------------
#    scripts/bench_sendfile_ab.sh              # 完整跑（256 MiB / 4 000 ms / 1,4,16 连接）
#    scripts/bench_sendfile_ab.sh --quick      # 冒烟：64 MiB / 2 000 ms（**不用作最终数字**）
#    FSS_AB_SIZE_BYTES=... FSS_AB_SERVER_CPUS=... FSS_AB_CLIENT_CPUS=... 覆盖默认值
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${FSS_BUILD_DIR:-${REPO_ROOT}/build}"
BIN_DIR="${BUILD_DIR}/bin"
WORK_DIR="${FSS_AB_WORK_DIR:-${BUILD_DIR}/ab-run}"

#  绑核：服务端 4 个核、客户端 8 个核，且**互不重叠**（否则量的是"自己抢自己"）
SERVER_CPUS="${FSS_AB_SERVER_CPUS:-4-7}"
CLIENT_CPUS="${FSS_AB_CLIENT_CPUS:-8-15}"
THREADS="${FSS_AB_THREADS:-4}"            # 两侧相同的并发上限（httplib: workers=max_conn）
SIZE_BYTES="${FSS_AB_SIZE_BYTES:-268435456}"   # 256 MiB
DURATION_MS="${FSS_AB_DURATION_MS:-4000}"
WARMUP_MS="${FSS_AB_WARMUP_MS:-800}"
CONN_LIST="${FSS_AB_CONNS:-1 4 16}"
RATIO_THRESHOLD="1.5"

for arg in "$@"; do
  case "${arg}" in
    --quick) SIZE_BYTES=67108864; DURATION_MS=2000; WARMUP_MS=500 ;;  # 只用于调试
    -h|--help) sed -n '2,36p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "未知参数：${arg}" >&2; exit 2 ;;
  esac
done

C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
log() { printf '%s\n' "$*"; }
dim() { printf '%s%s%s\n' "${C_DIM}" "$*" "${C_OFF}"; }
die() { printf '%s%s%s\n' "${C_RED}" "$*" "${C_OFF}" >&2; exit 1; }

#  ---------------------------------------------------------------------------
#  前置检查
#  ---------------------------------------------------------------------------
command -v taskset >/dev/null 2>&1 || die "缺少 taskset：无法绑核（违反 R2）"
command -v dd >/dev/null 2>&1 || die "缺少 dd：无法预热页缓存"

#  核集合展开与"不重叠"断言 —— R2 的机械保证
expand_cpus() {
  local spec="$1" part a b
  for part in ${spec//,/ }; do
    if [[ "${part}" == *-* ]]; then
      a="${part%-*}"; b="${part#*-}"
      seq "${a}" "${b}"
    else
      printf '%s\n' "${part}"
    fi
  done
}
#  ★ comm 要求两个输入按**同一字典序**排序（不能用 sort -n：8,9,10 不是字典序）→ 用 sort -u
OVERLAP="$(comm -12 <(expand_cpus "${SERVER_CPUS}" | sort -u) \
                    <(expand_cpus "${CLIENT_CPUS}" | sort -u) | tr '\n' ' ')"
[[ -z "${OVERLAP}" ]] || die "服务端与客户端绑核重叠（${OVERLAP}）—— 违反 R2"
taskset -c "${SERVER_CPUS}" true || die "SERVER_CPUS=${SERVER_CPUS} 不可用"
taskset -c "${CLIENT_CPUS}" true || die "CLIENT_CPUS=${CLIENT_CPUS} 不可用"
CLIENT_CORES="$(expand_cpus "${CLIENT_CPUS}" | sort -n -u | wc -l | tr -d ' ')"

#  目标二进制（EXCLUDE_FROM_ALL → 不存在时显式构建）
for target in fss_bench_sendfile_ab fss_bench_capacity; do
  if [[ ! -x "${BIN_DIR}/${target}" ]]; then
    log "构建 ${target} …"
    cmake --build "${BUILD_DIR}" --target "${target}" >/dev/null
  fi
done
[[ -x "${BIN_DIR}/fss_bench_sendfile_ab" ]] || die "缺少 ${BIN_DIR}/fss_bench_sendfile_ab"
[[ -x "${BIN_DIR}/fss_bench_capacity" ]] || die "缺少 ${BIN_DIR}/fss_bench_capacity"

mkdir -p "${WORK_DIR}"
BLOB_FILE="${WORK_DIR}/ab-blob-${SIZE_BYTES}.bin"
RESULTS_TSV="${WORK_DIR}/results.tsv"
: > "${RESULTS_TSV}"

#  ---------------------------------------------------------------------------
#  生命周期：全局变量保存后台 PID（★ 不用 $(...) 取 PID，见 AGENTS 陷阱清单）
#  ---------------------------------------------------------------------------
HTTPLIB_PID=""
SENDFILE_PID=""
PORT=""
KEEP_BLOB="${FSS_AB_KEEP_BLOB:-0}"

stop_one() {
  local pid="$1"
  if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
    kill -TERM "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
  fi
}
cleanup() {
  stop_one "${HTTPLIB_PID}"
  stop_one "${SENDFILE_PID}"
  HTTPLIB_PID=""
  SENDFILE_PID=""
  # 临时文件清理：删掉生成的 256 MiB 负载文件（日志与 results.tsv 保留，便于复核）
  if [[ "${KEEP_BLOB}" != "1" ]]; then
    rm -f "${BLOB_FILE}"
  fi
}
trap cleanup EXIT INT TERM

#  ---------------------------------------------------------------------------
#  起服务端（独立进程 + 绑核）。PORT 是第一行 stdout。
#  ---------------------------------------------------------------------------
start_server() {   # start_server <mode>
  local mode="$1"
  local out="${WORK_DIR}/server-${mode}.out"
  local err="${WORK_DIR}/server-${mode}.err"
  : > "${out}"
  : > "${err}"
  taskset -c "${SERVER_CPUS}" "${BIN_DIR}/fss_bench_sendfile_ab" \
      --mode "${mode}" --size "${SIZE_BYTES}" --dir "${WORK_DIR}" --threads "${THREADS}" \
      > "${out}" 2> "${err}" &
  local pid=$!                      # ★ 直接取 $!（不是命令替换）
  if [[ "${mode}" == "httplib" ]]; then HTTPLIB_PID="${pid}"; else SENDFILE_PID="${pid}"; fi
  PORT=""
  local waited=0
  while [[ -z "${PORT}" ]]; do
    if ! kill -0 "${pid}" 2>/dev/null; then
      cat "${err}" >&2 || true
      die "服务端（${mode}）启动即退出"
    fi
    PORT="$(sed -n 's/^PORT \([0-9][0-9]*\).*/\1/p' "${out}" | sed -n 1p)"
    if [[ -z "${PORT}" ]]; then
      sleep 0.1
      waited=$((waited + 1))
      if [[ ${waited} -gt 300 ]]; then
        cat "${err}" >&2 || true
        die "服务端（${mode}）30 秒内没有打印 PORT"
      fi
    fi
  done
}

#  ---------------------------------------------------------------------------
#  负载生成器（独立进程 + 绑核；只用官方生成器，不另写进程内压测）
#  ---------------------------------------------------------------------------
warm_cache() {   # 让两种模式都在"页缓存已热"的同一状态起跑
  taskset -c "${CLIENT_CPUS}" dd if="${BLOB_FILE}" of=/dev/null bs=1M status=none
}

run_load() {   # run_load <label> <mode> <conns> <port>
  local label="$1" mode="$2" conns="$3" port="$4"
  local out parsed
  out="$(taskset -c "${CLIENT_CPUS}" "${BIN_DIR}/fss_bench_capacity" load \
        --label "${label}" --mode get --url "http://127.0.0.1:${port}/blob" \
        --connections "${conns}" --duration-ms "${DURATION_MS}" --warmup-ms "${WARMUP_MS}" \
        2>&1)" || true
  printf '%s\n' "${out}" > "${WORK_DIR}/load-${label}.log"
  printf '%s\n' "${out}" | sed -n 's/^RESULT /  /p'
  parsed="$(printf '%s\n' "${out}" | awk -v mode="${mode}" -v conns="${conns}" '
    /^RESULT/ {
      for (i = 1; i <= NF; ++i) { split($i, kv, "="); v[kv[1]] = kv[2] }
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", mode, conns, v["mibps"], v["p99_us"],
             v["errs"], v["client_cpu_pct"], v["rps"], v["reqs"]
      found = 1
    }
    END { exit(found ? 0 : 1) }')" || true
  if [[ -z "${parsed}" ]]; then
    printf '%s\n' "${out}" >&2
    die "点位 ${label} 没有解析出 RESULT 行"
  fi
  #  ★ R9：判据必须是 `errs==0` **且** `reqs>0` —— 只看"errs=0"会把"请求全部 4xx/0 个请求"
  #    这种"跑完了、有表格"的假结果当成正常（AGENTS 陷阱清单里的播种器事故）。
  if [[ "$(printf '%s\n' "${parsed}" | awk -F'\t' '{ print $8 }')" -le 0 ]]; then
    printf '%s\n' "${out}" >&2
    die "点位 ${label} 的 reqs<=0：没有成功的请求，数字不可用"
  fi
  printf '%s\n' "${parsed}" >> "${RESULTS_TSV}"
}

#  ---------------------------------------------------------------------------
#  主流程
#  ---------------------------------------------------------------------------
log "ADR-006 受控复核（C9.12）：httplib 内容提供者  vs  裸 socket + sendfile(2)"
log "  环境        : $(uname -sr)"
log "  服务端绑核  : ${SERVER_CPUS}    客户端绑核: ${CLIENT_CPUS}（客户端核数 ${CLIENT_CORES}，不重叠）"
log "  负载文件    : ${SIZE_BYTES} 字节（非稀疏；落在 ${WORK_DIR} 内）"
log "  每点位      : ${DURATION_MS} ms 计时（预热 ${WARMUP_MS} ms 不计入）；连接数 ${CONN_LIST}"
log "  协议        : HTTP/1.1 keep-alive + 显式 Content-Length，无 chunked，双端 TCP_NODELAY"
log "  并发上限    : 两侧都是 ${THREADS} 线程 / ${THREADS} 连接（httplib worker=max_conn=${THREADS}）"
log "  ⚠️ WSL2 + 虚拟盘 + loopback：绝对数字只作本机量级参考；且 sendfile 侧是**原型**（只 GET /blob，"
log "     无 Range/鉴权/限流/超时/连接上限）→ 这是「上界对照」，不是「实现对比」。"
log "------------------------------------------------------------------------"

rm -f "${BLOB_FILE}"
start_server "httplib"
HTTPLIB_PORT="${PORT}"
start_server "sendfile"      # --size 复用 httplib 侧刚生成的那份文件
SENDFILE_PORT="${PORT}"
log "  httplib  : 127.0.0.1:${HTTPLIB_PORT}（日志 ${WORK_DIR}/server-httplib.{out,err}）"
log "  sendfile : 127.0.0.1:${SENDFILE_PORT}（日志 ${WORK_DIR}/server-sendfile.{out,err}）"

for conns in ${CONN_LIST}; do
  for mode in httplib sendfile; do
    if [[ "${mode}" == "httplib" ]]; then run_port="${HTTPLIB_PORT}"; else run_port="${SENDFILE_PORT}"; fi
    warm_cache
    run_load "${mode}_c${conns}" "${mode}" "${conns}" "${run_port}"
  done
done

stop_one "${SENDFILE_PID}"; SENDFILE_PID=""
stop_one "${HTTPLIB_PID}"; HTTPLIB_PID=""

#  ---------------------------------------------------------------------------
#  结果表 + 比值 + 结论
#  ---------------------------------------------------------------------------
log "------------------------------------------------------------------------"
log "原始数字（同一负载生成器 fss_bench_capacity load；mibps 为响应体吞吐）"
awk -F'\t' -v conn_list="${CONN_LIST}" -v client_cores="${CLIENT_CORES}" -v thr="${RATIO_THRESHOLD}" '
  {
    key = $1 "\t" $2
    mib[key] = $3; p99[key] = $4; errs[key] = $5; cpu[key] = $6; rps[key] = $7; reqs[key] = $8
  }
  function row(mode, c,    key, ratio, flag) {
    key = mode "\t" c
    if (!(key in mib)) return
    ratio = 0
    if (mode == "sendfile" && (("httplib\t" c) in mib) && (mib["httplib\t" c] + 0 > 0))
      ratio = mib[key] / mib["httplib\t" c]
    flag = ""
    if ((cpu[key] + 0) >= 0.9 * client_cores * 100) flag = "[受客户端限制]"
    printf "  %-9s %-5s %10.1f %10.0f %6d %9.1f %8.1f %8d %9s %s\n", mode, c, mib[key], p99[key],
           errs[key], cpu[key], rps[key], reqs[key],
           (mode == "sendfile" ? sprintf("%.2fx", ratio) : "-"), flag
  }
  END {
    printf "  %-9s %-5s %10s %10s %6s %9s %8s %8s %9s\n", "mode", "conns", "mibps", "p99_us",
           "errs", "cpu_pct", "rps", "reqs", "sf/hl"
    n = split(conn_list, cs, " ")
    for (i = 1; i <= n; ++i) {
      row("httplib", cs[i])
      row("sendfile", cs[i])
    }
  }' "${RESULTS_TSV}"

RATIO="$(awk -F'\t' -v conn_list="${CONN_LIST}" '
  { mib[$1 "\t" $2] = $3 }
  END {
    n = split(conn_list, cs, " ")
    s = 0; k = 0
    for (i = 1; i <= n; ++i) {
      h = mib["httplib\t" cs[i]] + 0
      f = mib["sendfile\t" cs[i]] + 0
      if (h > 0 && f > 0) { s += log(f / h); k++ }
    }
    if (k == 0) { print "NA"; exit 1 }
    printf "%.3f\n", exp(s / k)
  }' "${RESULTS_TSV}")" || RATIO="NA"

log "------------------------------------------------------------------------"
log "总倍数（连接数 ${CONN_LIST} 各点位 sendfile/httplib 的 mibps 比值几何平均）= ${RATIO}x"
if [[ "${RATIO}" == "NA" ]]; then
  die "无法计算倍数（缺某个点位的数字）"
fi

ERRS_TOTAL="$(awk -F'\t' '{ s += $5 } END { print s + 0 }' "${RESULTS_TSV}")"
if awk -v r="${RATIO}" -v t="${RATIO_THRESHOLD}" 'BEGIN { exit !(r + 0 >= t + 0) }'; then
  log "${C_GRN}结论：RATIO=${RATIO} >= ${RATIO_THRESHOLD} → 值得实现 sendfile 数据面${C_OFF}"
else
  log "${C_GRN}结论：RATIO=${RATIO} < ${RATIO_THRESHOLD} → 放弃，统一用 httplib${C_OFF}"
fi
dim "  （协议：HTTP/1.1 + Content-Length + keep-alive，无 chunked；两侧同并发上限；详见脚本头部 R3 声明）"
log "  服务器日志：${WORK_DIR}/server-httplib.{out,err}、${WORK_DIR}/server-sendfile.{out,err}"
log "  原始 TSV  ：${RESULTS_TSV}"

if [[ "${ERRS_TOTAL}" != "0" ]]; then
  die "存在 errs=${ERRS_TOTAL} 的点位：先修两侧响应格式，不得把有错误的数字写进报告"
fi
log "  所有点位 errs=0 ✓"
