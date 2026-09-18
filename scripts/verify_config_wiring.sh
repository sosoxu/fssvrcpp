#!/usr/bin/env bash
# =============================================================================
#  scripts/verify_config_wiring.sh —— 阶段 10 切片 1：配置面接线的真实二进制证据
# =============================================================================
#  判据：C10.1~C10.8（docs/04-implementation-plan.md「阶段 10：配置面接线」）。
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
#    ⑥ `storage.io_engine=uring`（不可用）→ 退出码 78 且提示 scripts/check_io_uring.sh。
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

echo "配置面接线验证（阶段 10 / C10.1~C10.8）    server_bin=${SERVER_BIN}"

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
hr() { printf '%s\n' "------------------------------------------------------------------------"; }
hr
if [[ ${FAIL} -eq 0 ]]; then
  printf '%s\n' "${C_GRN}配置面接线：全部通过（${PASS} 条断言）${C_OFF}"
  info "证据：配置文件里改端口/线程数/鉴权模式真的生效；production+disabled 与未知键真的以 78 拒绝启动。"
  exit 0
fi
printf '%s\n' "${C_RED}配置面接线：${FAIL} 条断言失败（通过 ${PASS} 条）${C_OFF}"
exit 1
