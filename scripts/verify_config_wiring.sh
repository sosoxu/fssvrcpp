#!/usr/bin/env bash
# =============================================================================
#  scripts/verify_config_wiring.sh —— 阶段 10 切片 1/2：配置面接线 + GC/expiry/拒绝语义
# =============================================================================
#  判据：C10.1~C10.12（docs/04-implementation-plan.md「阶段 10：配置面接线」）。
#  与 `ctest -L phase10`（tests/integration/test_config_wiring.cpp）互补：
#    · C++ 用例覆盖断言的细节与正/反对照；
#    · 本脚本给运维一条**可复制粘贴**的证据链："配置真的生效" + "非法配置真的拒绝启动"。
#
#  断言（全部跑 `build/bin/fss_server` 真实二进制，配置来自**临时配置文件**）：
#    ① 配置生效：`server.http.port` → 该端口 readiness 200；`worker_threads` → /metrics；
#       `auth.mode=jwt` → `/v2/info` 的 authMode=jwt 且无 token 的 uploadURL = 401；
#    ② 优先级：`--set`（cli）覆盖配置文件；旧环境变量 `FSS_HTTP_PORT` 覆盖配置文件；
#    ③ `--print-config`：退出 0、打印来源、密钥打码（不出现明文）；
#    ④ 生产强校验：`deployment.environment=production` + `auth.mode=disabled` → **退出码 78**
#       且输出含 production；
#    ⑤ 未知键 → 退出码 78；
#    ⑥ `storage.io_engine=uring`（不可用）→ 退出码 78 且提示 scripts/check_io_uring.sh；
#    ⑦ C10.9 GC 周期调度（旧 .tmp.* 被清 + /metrics 的 fss_gc_* 动过 + 在途被保护）；
#    ⑧ C10.9 `--once`（exit 0 + GcReport 摘要）；
#    ⑨ C10.10 样例配置启动（readiness 200）与"改坏一个键 → 78"；
#    ⑩ C10.11 未实现能力的非默认值 → 78（含可读原因）；
#    ⑪ C10.12 expiry.default/max → 签发 URL 的 TTL（边界/夹紧/非法 400）。
#
#  用法：scripts/verify_config_wiring.sh [构建目录]
#  退出码：0 = 全部通过；1 = 任一断言失败
#  ★ 工作目录在 `build/` 下（**不用 /tmp**：本环境 /tmp 跨命令不共享）。
#  ★ 不用 `| head` 截断（`set -o pipefail` 下 head 会让上游收 SIGPIPE → 整条管道非 0，
#    见 AGENTS.md §4.3 的 P7-D06 教训）；需要前 N 行一律用 `sed -n '1,Np'`。
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build}"
SERVER_BIN="${BUILD_DIR}/bin/fss_server"
WORK="${BUILD_DIR}/config-wiring-$$"

C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
ok()   { printf '%s\n' "${C_GRN}  ✓${C_OFF} $*"; }
bad()  { printf '%s\n' "${C_RED}  ✗${C_OFF} $*"; }
info() { printf '%s\n' "${C_DIM}  ·${C_OFF} $*"; }

PASS=0
FAIL=0
SERVER_PID=""

cleanup() {
  if [[ -n "${SERVER_PID}" ]]; then
    kill "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  rm -rf "${WORK}"
}
trap cleanup EXIT

assert_eq() {  # $1=actual $2=expected $3=desc
  if [[ "$1" == "$2" ]]; then
    ok "$3（=$2）"; PASS=$((PASS + 1))
  else
    bad "$3：期望 '$2'，实际 '$1'"; FAIL=$((FAIL + 1))
  fi
}

assert_contains() {  # $1=text $2=needle $3=desc
  if grep -qF -- "$2" <<<"$1"; then
    ok "$3"; PASS=$((PASS + 1))
  else
    bad "$3：输出里找不到 '$2'"; FAIL=$((FAIL + 1))
  fi
}

assert_not_contains() {  # $1=text $2=needle $3=desc
  if grep -qF -- "$2" <<<"$1"; then
    bad "$3：输出里**不应**出现 '$2'"; FAIL=$((FAIL + 1))
  else
    ok "$3"; PASS=$((PASS + 1))
  fi
}

command -v curl >/dev/null 2>&1 || { bad "缺少 curl"; exit 1; }
command -v python3 >/dev/null 2>&1 || { bad "缺少 python3"; exit 1; }
[[ -x "${SERVER_BIN}" ]] || { bad "找不到 ${SERVER_BIN}，请先构建"; exit 1; }
mkdir -p "${WORK}"

free_port() {
  python3 -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()'
}

wait_ready() {  # $1=port
  local port="$1"
  for _ in $(seq 1 100); do
    if curl -fsS "http://127.0.0.1:${port}/api/file/v2/readiness_check" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

#  起服务并等到 readiness 200；`RUN_ENV` 是逐场景设置的环境变量数组
RUN_ENV=()
start_and_wait() {  # $1=log $2=port，其余为命令行参数
  local log="$1" port="$2"; shift 2
  if [[ ${#RUN_ENV[@]} -eq 0 ]]; then
    "${SERVER_BIN}" "$@" >"${log}" 2>&1 &
  else
    env "${RUN_ENV[@]}" "${SERVER_BIN}" "$@" >"${log}" 2>&1 &
  fi
  SERVER_PID=$!
  if ! wait_ready "${port}"; then
    bad "服务未就绪（端口 ${port}）"; sed -n '1,40p' "${log}"; exit 1
  fi
  ok "服务已在配置端口 ${port} 就绪（readiness 200）"; PASS=$((PASS + 1))
}

stop_server() {
  if [[ -n "${SERVER_PID}" ]]; then
    kill "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" 2>/dev/null || true
    SERVER_PID=""
  fi
}

echo "配置面接线验证（阶段 10 / C10.1~C10.12）    server_bin=${SERVER_BIN}"

# -----------------------------------------------------------------------------
#  ① 配置生效：端口 / worker_threads / auth.mode=jwt 都来自配置文件
# -----------------------------------------------------------------------------
PORT="$(free_port)"
CONFIG="${WORK}/config.json"
cat >"${CONFIG}" <<JSON
{
  // 带注释的 JSON（C10.1）：本文件是**真配置源**
  "server": {"http": {"bind": "127.0.0.1", "port": ${PORT}, "worker_threads": 9}},
  "auth": {"mode": "jwt", "jwt": {"hmac_secret": "wiring-secret", "verify_signature": true}},
  "observability": {"log_format": "text", "metrics_enabled": true, "metrics_path": "/metrics"}
}
JSON
mkdir -p "${WORK}/data"
RUN_ENV=(
  "FSS_STORAGE_ROOT=${WORK}/data"
  "FSS_SQLITE_PATH=${WORK}/data/location.db"
  "FSS_METADATA_SQLITE_PATH=${WORK}/data/metadata.db"
  "FSS_TRANSFER_SECRET=wiring-secret"
)
start_and_wait "${WORK}/s1.log" "${PORT}" --config "${CONFIG}"

BANNER="$(cat "${WORK}/s1.log")"
assert_contains "${BANNER}" "fss_server 配置来源 : ${CONFIG}" "启动横幅打印配置来源（文件路径）"
assert_contains "${BANNER}" "server.http.port [file]" "横幅逐键给出来源（file）"

INFO_JSON="$(curl -sS "http://127.0.0.1:${PORT}/api/file/v2/info")"
assert_contains "${INFO_JSON}" '"authMode":"jwt"' "配置文件 auth.mode=jwt 生效（/v2/info）"

METRICS="$(curl -sS "http://127.0.0.1:${PORT}/metrics")"
assert_contains "${METRICS}" "fss_http_worker_threads 9" \
  "配置文件 worker_threads=9 生效（/metrics 的 fss_http_worker_threads）"

CODE="$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:${PORT}/api/file/v2/files/uploadURL")"
assert_eq "${CODE}" "401" "jwt 模式下无 token 的 uploadURL"
CODE="$(curl -sS -o /dev/null -w '%{http_code}' \
  -H 'authorization: Bearer not-a-jwt' -H 'data-partition-id: opendes' \
  "http://127.0.0.1:${PORT}/api/file/v2/files/uploadURL")"
assert_eq "${CODE}" "401" "jwt 模式下伪造 token 的 uploadURL"
stop_server

# -----------------------------------------------------------------------------
#  ② 优先级：同一端口由 file / --set / 旧环境变量分别给出 → 高优先级生效
# -----------------------------------------------------------------------------
PORT_FILE="$(free_port)"
PORT_CLI="$(free_port)"
CONFIG_PRI="${WORK}/priority.json"
cat >"${CONFIG_PRI}" <<JSON
{ "server": {"http": {"bind": "127.0.0.1", "port": ${PORT_FILE}}} }
JSON
RUN_ENV=(
  "FSS_STORAGE_ROOT=${WORK}/data"
  "FSS_SQLITE_PATH=${WORK}/data/location.db"
  "FSS_TRANSFER_SECRET=wiring-secret"
)
start_and_wait "${WORK}/s2a.log" "${PORT_CLI}" --config "${CONFIG_PRI}" \
  --set "server.http.port=${PORT_CLI}"
CODE="$(curl -sS -o /dev/null -w '%{http_code}' \
  "http://127.0.0.1:${PORT_CLI}/api/file/v2/readiness_check")"
assert_eq "${CODE}" "200" "--set(cli) 覆盖配置文件（实际监听 ${PORT_CLI}）"
stop_server

PORT_ENV="$(free_port)"
RUN_ENV=(
  "FSS_STORAGE_ROOT=${WORK}/data"
  "FSS_SQLITE_PATH=${WORK}/data/location.db"
  "FSS_TRANSFER_SECRET=wiring-secret"
  "FSS_HTTP_PORT=${PORT_ENV}"
)
start_and_wait "${WORK}/s2b.log" "${PORT_ENV}" --config "${CONFIG_PRI}"
assert_contains "$(cat "${WORK}/s2b.log")" "server.http.port [env 别名 FSS_HTTP_PORT]" \
  "旧环境变量 FSS_HTTP_PORT 覆盖配置文件（来源标为别名）"
stop_server

# -----------------------------------------------------------------------------
#  ③ --print-config：脱敏 + 来源 + 成功退出
# -----------------------------------------------------------------------------
set +e
PRINT_OUT="$(env "FSS_STORAGE_ROOT=${WORK}/data" timeout 20 "${SERVER_BIN}" --config "${CONFIG}" \
  --print-config 2>&1)"
PRINT_RC=$?
set -e
assert_eq "${PRINT_RC}" "0" "--print-config 成功退出"
assert_contains "${PRINT_OUT}" "auth.jwt.hmac_secret = ***" "print-config 把密钥打成 ***"
assert_not_contains "${PRINT_OUT}" "wiring-secret" "print-config 不出现明文密钥"
assert_contains "${PRINT_OUT}" "server.http.port = ${PORT} [file]" \
  "print-config 给出该键的值与来源（file）"

# -----------------------------------------------------------------------------
#  ④ 生产强校验：production + auth.mode=disabled → 退出码 78
# -----------------------------------------------------------------------------
PROD_CONFIG="${WORK}/prod.json"
cat >"${PROD_CONFIG}" <<'JSON'
{
  // 生产环境：不允许 disabled（那等于没有鉴权）
  "deployment": {"environment": "production"},
  "auth": {"mode": "disabled", "jwt": {"hmac_secret": "x"}}
}
JSON
set +e
PROD_OUT="$(timeout 20 "${SERVER_BIN}" --config "${PROD_CONFIG}" 2>&1)"
PROD_RC=$?
set -e
assert_eq "${PROD_RC}" "78" "production + auth.mode=disabled 拒绝启动（EX_CONFIG=78）"
assert_contains "${PROD_OUT}" "production" "拒绝原因里明确写出 production"
assert_not_contains "${PROD_OUT}" "已启动" "被拒绝的进程没有进入服务状态"

# -----------------------------------------------------------------------------
#  ⑤ 未知键 → 退出码 78
# -----------------------------------------------------------------------------
UNKNOWN_CONFIG="${WORK}/unknown.json"
cat >"${UNKNOWN_CONFIG}" <<'JSON'
{ "server": {"http": {"prot": 8080}} }
JSON
set +e
UNKNOWN_OUT="$(timeout 20 "${SERVER_BIN}" --config "${UNKNOWN_CONFIG}" 2>&1)"
UNKNOWN_RC=$?
set -e
assert_eq "${UNKNOWN_RC}" "78" "配置文件里的未知键拒绝启动"
assert_contains "${UNKNOWN_OUT}" "server.http.prot" "错误信息逐条给出未知键路径"

# -----------------------------------------------------------------------------
#  ⑥ storage.io_engine=uring（不可用）→ 退出码 78 + 探测脚本提示
# -----------------------------------------------------------------------------
set +e
URING_OUT="$(timeout 20 "${SERVER_BIN}" --set storage.io_engine=uring 2>&1)"
URING_RC=$?
set -e
assert_eq "${URING_RC}" "78" "storage.io_engine=uring 不可用时拒绝启动"
assert_contains "${URING_OUT}" "check_io_uring.sh" "拒绝原因给出 scripts/check_io_uring.sh 提示"

# -----------------------------------------------------------------------------
#  ⑦ C10.9：GC 周期调度真的跑起来（旧 .tmp.* 被清 + /metrics 出现 fss_gc_*）
# -----------------------------------------------------------------------------
GC_ROOT="${WORK}/gcdata"
mkdir -p "${GC_ROOT}/blobs/opendes-staging"
printf 'partial-upload-bytes' >"${GC_ROOT}/blobs/opendes-staging/residue.bin.tmp.local.1.1"
touch -d '3 days ago' "${GC_ROOT}/blobs/opendes-staging/residue.bin.tmp.local.1.1"
printf 'partial-upload-bytes' >"${GC_ROOT}/blobs/opendes-staging/inflight.bin.tmp.local.1.2"
GC_PORT="$(free_port)"
GC_CONFIG="${WORK}/gc.json"
cat >"${GC_CONFIG}" <<JSON
{
  "server": {"http": {"bind": "127.0.0.1", "port": ${GC_PORT}}},
  "storage": {"posix": {"root": "${GC_ROOT}"}},
  "location": {"sqlite": {"path": "${GC_ROOT}/loc.db"}},
  "metadata": {"sqlite": {"path": "${GC_ROOT}/meta.db"}},
  "self_signed": {"signing_key": "wiring-gc"},
  "auth": {"mode": "disabled"},
  "gc": {"enabled": true, "dry_run": false, "require_lease_expiry": true,
         "staging_ttl_hours": 24, "orphan_grace_hours": 72, "interval_seconds": 1}
}
JSON
RUN_ENV=()
start_and_wait "${WORK}/s7.log" "${GC_PORT}" --config "${GC_CONFIG}"
assert_contains "$(cat "${WORK}/s7.log")" "gc             : 已启动（间隔 1s" \
  "横幅说明 GC 调度已启动（间隔 1s）"
GC_SWEEP=0
for _ in $(seq 1 100); do
  if [[ ! -f "${GC_ROOT}/blobs/opendes-staging/residue.bin.tmp.local.1.1" ]]; then
    if curl -sS "http://127.0.0.1:${GC_PORT}/metrics" | grep -qF 'fss_gc_tmp_removed_total 1'; then
      GC_SWEEP=1
      break
    fi
  fi
  sleep 0.1
done
assert_eq "${GC_SWEEP}" "1" "周期调度清理了够旧的 .tmp.*，且 /metrics 的 fss_gc_tmp_removed_total 动过"
if [[ -f "${GC_ROOT}/blobs/opendes-staging/inflight.bin.tmp.local.1.2" ]]; then
  ok "在途（mtime 太新）的 .tmp.* 被正确保护"; PASS=$((PASS + 1))
else
  bad "在途的 .tmp.* 被误删（TTL 判据失效）"; FAIL=$((FAIL + 1))
fi
stop_server

# -----------------------------------------------------------------------------
#  ⑧ C10.9：--once 跑一轮 GC 后退出 0（便于 cron）
# -----------------------------------------------------------------------------
printf 'partial-upload-bytes' >"${GC_ROOT}/blobs/opendes-staging/once.bin.tmp.local.2.1"
touch -d '3 days ago' "${GC_ROOT}/blobs/opendes-staging/once.bin.tmp.local.2.1"
set +e
ONCE_OUT="$(timeout 30 "${SERVER_BIN}" --once \
  --set "storage.posix.root=${GC_ROOT}" \
  --set "location.sqlite.path=${GC_ROOT}/loc.db" \
  --set "metadata.sqlite.path=${GC_ROOT}/meta.db" \
  --set "self_signed.signing_key=wiring-gc" \
  --set "gc.dry_run=false" --set "gc.staging_ttl_hours=24" 2>&1)"
ONCE_RC=$?
set -e
assert_eq "${ONCE_RC}" "0" "--once 退出码 0"
assert_contains "${ONCE_OUT}" "gc once : partition=opendes dry_run=false" "--once 打印 GcReport 摘要"
assert_contains "${ONCE_OUT}" "tmp_removed=1" "--once 真的清掉了旧的 .tmp.*"

# -----------------------------------------------------------------------------
#  ⑨ C10.10：样例配置真的能起来；改坏一个键 → 拒绝启动
# -----------------------------------------------------------------------------
EXAMPLE="${REPO_ROOT}/config/fss.example.json"
EXAMPLE_PORT="$(free_port)"
EXAMPLE_DATA="${WORK}/example-data"
mkdir -p "${EXAMPLE_DATA}"
RUN_ENV=(
  "FSS_S3_ACCESS_KEY=example-access"
  "FSS_S3_SECRET_KEY=example-secret"
  "FSS_TRANSFER_SIGNING_KEY=example-transfer"
  "FSS_PG_DSN=postgres://example"
  "FSS_JWT_HMAC_SECRET=example-jwt"
  "FSS_STORAGE_TOKEN=example-storage-token"
  "FSS_GRPC_PORT=0"
)
start_and_wait "${WORK}/s9.log" "${EXAMPLE_PORT}" --config "${EXAMPLE}" \
  --set "server.http.bind=127.0.0.1" \
  --set "server.http.port=${EXAMPLE_PORT}" \
  --set "storage.posix.root=${EXAMPLE_DATA}" \
  --set "location.sqlite.path=${EXAMPLE_DATA}/location.db" \
  --set "metadata.sqlite.path=${EXAMPLE_DATA}/metadata.db"
assert_contains "$(cat "${WORK}/s9.log")" "config/fss.example.json" \
  "样例配置作为 --config 真的被读了"
CODE="$(curl -sS -o /dev/null -w '%{http_code}' \
  "http://127.0.0.1:${EXAMPLE_PORT}/api/file/v2/readiness_check")"
assert_eq "${CODE}" "200" "样例配置启动后 readiness 200"
stop_server

BROKEN="${WORK}/example-broken.json"
sed 's/"port": 8080/"port": -1/' "${EXAMPLE}" >"${BROKEN}"
set +e
BROKEN_OUT="$(env "${RUN_ENV[@]}" timeout 20 "${SERVER_BIN}" --config "${BROKEN}" \
  --set "storage.posix.root=${EXAMPLE_DATA}" 2>&1)"
BROKEN_RC=$?
set -e
assert_eq "${BROKEN_RC}" "78" "样例配置改坏 server.http.port → 拒绝启动（exit 78）"
assert_contains "${BROKEN_OUT}" "server.http.port" "拒绝原因指向被改坏的键"
assert_not_contains "${BROKEN_OUT}" "已启动" "被拒绝的进程没有进入服务状态"

# -----------------------------------------------------------------------------
#  ⑩ C10.11：未实现能力的非默认值 → exit 78（"未实现 + 下一步"）
# -----------------------------------------------------------------------------
assert_reject() {  # $1=key=value $2=needle $3=desc
  set +e
  local out rc
  out="$(timeout 20 "${SERVER_BIN}" --set "$1" 2>&1)"
  rc=$?
  set -e
  assert_eq "${rc}" "78" "$3（exit 78）"
  assert_contains "${out}" "拒绝启动" "$3（有可读原因）"
  assert_contains "${out}" "$2" "$3（原因指向该键）"
}
assert_reject "leases.enabled=true" "leases.enabled=true" "leases.enabled=true"
#  ★ 切片 6b：`events.publisher=webhook` 已是**生效**能力，"没有 url 才拒绝"才是真实语义
#    （断言原因指向 `events.webhook.url`，而不是"未实现"）。
assert_reject "events.publisher=webhook" "events.webhook.url" \
  "events.publisher=webhook + 空 url"
assert_reject "self_signed.single_use_nonce=true" "single_use_nonce=true" \
  "self_signed.single_use_nonce=true"
assert_reject "server.http.large_file_plane.enabled=true" "large_file_plane" \
  "server.http.large_file_plane.enabled=true"
assert_reject "leader_election.enabled=true" "leader_election.enabled=true" \
  "leader_election.enabled=true"

# -----------------------------------------------------------------------------
#  ⑪ C10.12：expiry.default / expiry.max 作用于签发 URL 的 TTL
# -----------------------------------------------------------------------------
EXP_PORT="$(free_port)"
EXP_DATA="${WORK}/expiry-data"
mkdir -p "${EXP_DATA}"
EXP_CONFIG="${WORK}/expiry.json"
cat >"${EXP_CONFIG}" <<JSON
{
  "server": {"http": {"bind": "127.0.0.1", "port": ${EXP_PORT}}},
  "storage": {"posix": {"root": "${EXP_DATA}"}},
  "location": {"sqlite": {"path": "${EXP_DATA}/loc.db"}},
  "metadata": {"sqlite": {"path": "${EXP_DATA}/meta.db"}},
  "self_signed": {"signing_key": "wiring-expiry"},
  "auth": {"mode": "disabled"},
  "expiry": {"default": "5M", "max": "10M"}
}
JSON
RUN_ENV=()
start_and_wait "${WORK}/s11.log" "${EXP_PORT}" --config "${EXP_CONFIG}"
assert_contains "$(cat "${WORK}/s11.log")" "expiry         : default=5M（300s）max=10M（600s）" \
  "横幅打印配置后的缺省值与上限"
EXP_CLAMP_OK="$(python3 - "$EXP_PORT" <<'PYEXP'
import sys, time, urllib.request
port = sys.argv[1]
def exp_of(extra):
    req = urllib.request.Request(
        "http://127.0.0.1:%s/api/file/v2/files/uploadURL%s" % (port, extra),
        headers={"authorization": "Bearer t", "data-partition-id": "opendes"})
    with urllib.request.urlopen(req, timeout=10) as r:
        body = r.read().decode()
    i = body.find("exp=")
    return int(body[i + 4:].split("&")[0].split('"')[0]) if i >= 0 else -1
now = int(time.time())
d_default = exp_of("") - now
d_boundary = exp_of("?expiryTime=10M") - now
d_clamped = exp_of("?expiryTime=60M") - now
ok = 295 <= d_default <= 305 and 595 <= d_boundary <= 605 and 595 <= d_clamped <= 605
print("OK" if ok else "BAD default=%d boundary=%d clamped=%d" % (d_default, d_boundary, d_clamped))
PYEXP
)"
assert_eq "${EXP_CLAMP_OK}" "OK" "缺省 5M / 边界 10M / 超限 60M 被夹紧到 10M"
CODE="$(curl -sS -o /dev/null -w '%{http_code}' \
  -H 'authorization: Bearer t' -H 'data-partition-id: opendes' \
  "http://127.0.0.1:${EXP_PORT}/api/file/v2/files/uploadURL?expiryTime=5X")"
assert_eq "${CODE}" "400" "非法 expiryTime → 400（与「超限夹紧」严格区分）"
stop_server

# -----------------------------------------------------------------------------
hr() { printf '%s\n' "------------------------------------------------------------------------"; }
hr
if [[ ${FAIL} -eq 0 ]]; then
  printf '%s\n' "${C_GRN}配置面接线：全部通过（${PASS} 条断言）${C_OFF}"
  info "证据：配置文件里改端口/线程数/鉴权模式真的生效；production+disabled 与未知键真的以 78 拒绝启动；GC 调度与 --once 真的清理旧 .tmp.*；样例配置真的能启动；expiry TTL 真的按配置夹紧。"
  exit 0
fi
printf '%s\n' "${C_RED}配置面接线：${FAIL} 条断言失败（通过 ${PASS} 条）${C_OFF}"
exit 1
