#!/usr/bin/env bash
# =============================================================================
#  C9.8 —— 容器镜像可构建 / 可启动 / readiness_check 通过 / production(jwt) 生效
# =============================================================================
#  这个脚本是判据 C9.8 的**可复现验证**，全部步骤都在本机真实执行：
#    1. docker build（最小上下文 → build/docker-context/，tag 默认 fssvrcpp:verify）
#    2. docker run（随机宿主端口 → 容器 8080；数据目录绑到 build/image-verify/data）
#    3. **轮询** GET /api/file/v2/readiness_check 直到 200（超时打印容器日志后失败）
#    4. 断言 production 配置：/api/file/v2/info 的 authMode == jwt
#       且不带 Authorization 的 GET /api/file/v2/files/uploadURL 返回 401
#    5. 断言容器以非 root 运行（docker exec <id> id -u != 0）
#    6. 断言 HEALTHCHECK 最终为 healthy（docker inspect .State.Health.Status）
#    7. 清理：trap 里 docker stop + docker rm
#
#  额外（fail-closed 证据）：镜像**拒绝** auth.mode != jwt 与空密钥的启动。
#
#  ---------------------------------------------------------------------------
#  8) 容器硬化运行形态（本轮新增，`[H*]` 段；证据 phase9-image.md §10）
#     H1  --read-only（**不带** --tmpfs /tmp）：记录真实结果 + 根文件系统写入探测
#     H2  --read-only + --tmpfs /tmp:rw,size=64m,mode=1777
#     H3  --cap-drop=ALL --security-opt no-new-privileges（**Docker 默认 seccomp**）
#     H4a --memory=128m --pids-limit=64（任务原文形态：**C9.32 之后是断言** ——
#         启动期线程创建 EAGAIN 必须被顶层接住 → ExitCode==70 + 可读「未预期异常」，
#         **不是** terminate/139）；H4a2 = pids=66 的**下界正控**（必须正常启动）
#     H4b --memory=128m --pids-limit=256 下的 **1 GiB** 上传+读回 + 峰值内存
#         （★ 较慢，默认跳过；FSS_VERIFY_IMAGE_FULL=1 才跑，并附 pids 下界探测）
#     H5  --restart=on-failure + HEALTHCHECK healthy + SIGTERM 优雅退出
#         （经 FSS_CONFIG 打开 GC 周期调度 → 退出路径要 join GC 调度线程）
#     H6  健康检查**负控**：真实服务但探针指向错误地址 → 必须 unhealthy
#     H7  反向对照：①去掉 --read-only ②去掉 --cap-drop=ALL/no-new-privileges
#     H8  默认 seccomp（io_uring 不可用）下的端点行为 + 暴露面实测（C9.30 可测的一半）
#
#  ★ 环境约定：不使用 /tmp —— 本环境 /tmp 不跨命令共享；所有临时数据放仓库 build/ 下。
#
#  用法：
#    scripts/verify_image.sh                 # 默认 tag fssvrcpp:verify
#    FSS_IMAGE_TAG=my/tag scripts/verify_image.sh
#    FSS_VERIFY_SKIP_BUILD=1 scripts/verify_image.sh   # 复用已构建镜像
#    FSS_VERIFY_IMAGE_FULL=1 scripts/verify_image.sh   # 额外跑 H4b（1 GiB 流式，约 1 分钟）
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_TAG="${FSS_IMAGE_TAG:-fssvrcpp:verify}"
CTX_DIR="${REPO_ROOT}/build/docker-context"
RUN_DIR="${REPO_ROOT}/build/image-verify"
WAIT_SECONDS="${FSS_VERIFY_WAIT_SECONDS:-60}"
HEALTH_WAIT_SECONDS="${FSS_VERIFY_HEALTH_WAIT_SECONDS:-90}"

# 只用于本脚本验证的**测试**密钥（不是真实密钥，进程退出即作废）
TEST_JWT_SECRET="verify-image-jwt-$(date +%s)-not-a-real-secret"
TEST_TRANSFER_SECRET="verify-image-transfer-$(date +%s)-not-a-real-secret"

CONTAINER_ID=""
CONTAINER_NAME="fssvrcpp-verify-$$"
# 容器硬化场景（[H*] 段）用到的全局量；在 cleanup 之前声明，保证任何早退都能清理
HARD_CIDS=()
HARD_FAILURES=0
HARD_SUMMARY=()
HARD_SOFT=()
MEM_SAMPLER_PID=""

log()  { printf '\n== %s\n' "$*"; }
fail() { printf '\n[FAIL] %s\n' "$*" >&2; exit 1; }

cleanup() {
  if [ -n "${CONTAINER_ID}" ]; then
    docker stop "${CONTAINER_ID}" >/dev/null 2>&1 || true
    docker rm   "${CONTAINER_ID}" >/dev/null 2>&1 || true
  fi
  docker rm -f "${CONTAINER_NAME}-refuse" >/dev/null 2>&1 || true
  [ -z "${MEM_SAMPLER_PID}" ] || kill "${MEM_SAMPLER_PID}" >/dev/null 2>&1 || true
  local cid
  for cid in ${HARD_CIDS[@]+"${HARD_CIDS[@]}"}; do
    docker rm -f "${cid}" >/dev/null 2>&1 || true
  done
}
trap cleanup EXIT

command -v docker >/dev/null 2>&1 || fail "需要 docker（client/server 均可用）"
command -v python3 >/dev/null 2>&1 || fail "需要 python3（签发测试 JWT 与解析 JSON 响应）"
command -v curl >/dev/null 2>&1 || fail "需要 curl（宿主侧发请求）"
docker info >/dev/null 2>&1 || fail "docker daemon 不可用"

# 清空一个宿主数据目录。
# ★ 为什么不能只用 `rm -rf`：容器以 uid 10001 在 bind mount 里建的 `blobs/<container>`
#   目录属主是 10001、宿主用户不可写 → `rm -rf` 报 `Permission denied` 并返回非 0，
#   在 `set -e` 下会让**第二次运行直接中止**（本脚本实测踩到）。这里探测到残留就用一个
#   root 容器清掉（`--user 0:0`，不装任何东西、不联网）。
wipe_data_dir() {
  local dir="$1"
  [ -e "${dir}" ] || return 0
  rm -rf "${dir:?}" 2>/dev/null || true
  if [ -n "$(ls -A "${dir}" 2>/dev/null || true)" ]; then
    docker run --rm --user 0:0 --entrypoint /bin/bash -v "${dir}:/w" "${IMAGE_TAG}" \
      -c 'rm -rf /w/*' >/dev/null 2>&1 || true
  fi
}

# ★ buildx 默认把"builder 活动时间"写到 ~/.docker/buildx/activity/。本环境的沙箱
#   只允许写仓库工作区，那里是只读文件系统 → 直接报
#   `failed to update builder last activity time: ... read-only file system`。
#   把 DOCKER_CONFIG 指到仓库 build/ 下（不是 /tmp）即可绕开，且不影响可复现性。
export DOCKER_CONFIG="${FSS_DOCKER_CONFIG:-${REPO_ROOT}/build/docker-cli}"
mkdir -p "${DOCKER_CONFIG}"

# -----------------------------------------------------------------------------
#  0) 最小构建上下文（避免把 build/ 4.9G 与 build-asan/ 7.5G 送进 daemon）
# -----------------------------------------------------------------------------
log "[0/7] 准备最小构建上下文 ${CTX_DIR}"
rm -rf "${CTX_DIR}"
mkdir -p "${CTX_DIR}"
for path in CMakeLists.txt cmake src proto third_party bench tests config; do
  [ -e "${REPO_ROOT}/${path}" ] || fail "缺少构建输入 ${path}"
  cp -a "${REPO_ROOT}/${path}" "${CTX_DIR}/${path}"
done
du -sh "${CTX_DIR}"

# -----------------------------------------------------------------------------
#  1) docker build
# -----------------------------------------------------------------------------
if [ "${FSS_VERIFY_SKIP_BUILD:-0}" = "1" ]; then
  log "[1/7] 跳过构建（FSS_VERIFY_SKIP_BUILD=1），复用 ${IMAGE_TAG}"
  BUILD_SECONDS="(skipped)"
else
  log "[1/7] docker build -f Dockerfile -t ${IMAGE_TAG} (context=${CTX_DIR})"
  BUILD_STARTED="$(date +%s)"
  DOCKER_BUILDKIT=1 docker build -f "${REPO_ROOT}/Dockerfile" -t "${IMAGE_TAG}" "${CTX_DIR}"
  BUILD_SECONDS="$(( $(date +%s) - BUILD_STARTED ))"
  echo "构建耗时: ${BUILD_SECONDS}s"
fi
IMAGE_BYTES="$(docker image inspect -f '{{.Size}}' "${IMAGE_TAG}")"
IMAGE_SIZE_HUMAN="$(docker images --format '{{.Size}}' "${IMAGE_TAG}" | head -n1)"
echo "镜像大小: ${IMAGE_SIZE_HUMAN} (${IMAGE_BYTES} bytes)"

# -----------------------------------------------------------------------------
#  2) docker run
# -----------------------------------------------------------------------------
log "[2/7] docker run（随机宿主端口 → 容器 8080；数据目录 ${RUN_DIR}/data）"
wipe_data_dir "${RUN_DIR}"          # 见 wipe_data_dir 注释：容器 uid 10001 建的目录宿主删不掉
install -d -m 0777 "${RUN_DIR}/data"   # 容器内 uid 10001 需可写（宿主 bind mount 不继承镜像属主）
CONTAINER_ID="$(docker run -d --name "${CONTAINER_NAME}" \
  -p 127.0.0.1::8080 \
  -v "${RUN_DIR}/data:/data" \
  -e "FSS_JWT_HMAC_SECRET=${TEST_JWT_SECRET}" \
  -e "FSS_TRANSFER_SECRET=${TEST_TRANSFER_SECRET}" \
  "${IMAGE_TAG}")"
echo "container id: ${CONTAINER_ID}"

HOST_PORT="$(docker port "${CONTAINER_ID}" 8080/tcp | head -n1 | awk -F: '{print $NF}')"
[ -n "${HOST_PORT}" ] || { docker logs "${CONTAINER_ID}" || true; fail "无法解析映射的宿主端口"; }
BASE_URL="http://127.0.0.1:${HOST_PORT}/api/file"
echo "宿主端口: ${HOST_PORT}  base url: ${BASE_URL}"

# -----------------------------------------------------------------------------
#  3) 轮询 readiness_check（不要固定 sleep）
# -----------------------------------------------------------------------------
log "[3/7] 轮询 GET ${BASE_URL}/v2/readiness_check（最多 ${WAIT_SECONDS}s）"
READY_CODE=""
DEADLINE="$(( $(date +%s) + WAIT_SECONDS ))"
while :; do
  READY_CODE="$(curl -s -o "${RUN_DIR}/readiness.body" -w '%{http_code}' \
                  --max-time 2 "${BASE_URL}/v2/readiness_check" || true)"
  [ "${READY_CODE}" = "200" ] && break
  if [ "$(docker inspect -f '{{.State.Running}}' "${CONTAINER_ID}" 2>/dev/null)" != "true" ]; then
    docker logs "${CONTAINER_ID}" || true
    fail "容器已退出（readiness 最后状态码=${READY_CODE}）"
  fi
  if [ "$(date +%s)" -ge "${DEADLINE}" ]; then
    echo "--- 超时，容器日志 ---"
    docker logs "${CONTAINER_ID}" || true
    fail "readiness_check 在 ${WAIT_SECONDS}s 内未返回 200（最后状态码=${READY_CODE}）"
  fi
  sleep 1
done
echo "readiness: HTTP ${READY_CODE}  body='$(cat "${RUN_DIR}/readiness.body")'"

# -----------------------------------------------------------------------------
#  4) production 配置：authMode=jwt + 未授权访问必须 401
# -----------------------------------------------------------------------------
log "[4/7] 断言 production 配置（authMode=jwt 且缺 Authorization → 401）"
INFO_JSON="$(curl -s --max-time 5 "${BASE_URL}/v2/info")"
echo "GET /v2/info -> ${INFO_JSON}"
AUTH_MODE="$(printf '%s' "${INFO_JSON}" | grep -o '"authMode"[[:space:]]*:[[:space:]]*"[^"]*"' \
             | sed -E 's/.*"([^"]*)"$/\1/' || true)"
[ "${AUTH_MODE}" = "jwt" ] || fail "/v2/info 的 authMode='${AUTH_MODE}'（期望 jwt）"
echo "authMode = ${AUTH_MODE}  ✅"

UPLOAD_CODE="$(curl -s -o "${RUN_DIR}/upload.body" -w '%{http_code}' --max-time 5 \
                 -H 'data-partition-id: opendes' \
                 "${BASE_URL}/v2/files/uploadURL?partitionId=opendes")"
echo "GET /v2/files/uploadURL（有 partition 头、**无** Authorization）-> HTTP ${UPLOAD_CODE} body='$(cat "${RUN_DIR}/upload.body")'"
[ "${UPLOAD_CODE}" = "401" ] || fail "无凭证的 uploadURL 期望 401，实际 ${UPLOAD_CODE}（强制 jwt 失败）"

# 反向对照（R1）：伪造的 Bearer token 同样必须被拒 —— 否则"401"可能只证明
# "缺了某个头"，而非"JWT 校验真的在跑"。
BOGUS_CODE="$(curl -s -o "${RUN_DIR}/upload-bogus.body" -w '%{http_code}' --max-time 5 \
                -H 'data-partition-id: opendes' \
                -H 'Authorization: Bearer not.a.valid.jwt' \
                "${BASE_URL}/v2/files/uploadURL?partitionId=opendes")"
echo "GET /v2/files/uploadURL（伪造 Bearer token）-> HTTP ${BOGUS_CODE} body='$(cat "${RUN_DIR}/upload-bogus.body")'"
[ "${BOGUS_CODE}" = "401" ] || fail "伪造 token 的 uploadURL 期望 401，实际 ${BOGUS_CODE}（jwt 未真正校验）"
echo "强制 jwt（无 token → 401，伪 token → 401）✅"

# -----------------------------------------------------------------------------
#  5) 非 root
# -----------------------------------------------------------------------------
log "[5/7] 断言容器以非 root 运行"
RUN_UID="$(docker exec "${CONTAINER_ID}" id -u)"
RUN_USER="$(docker exec "${CONTAINER_ID}" id -un)"
echo "docker exec id -u = ${RUN_UID} (${RUN_USER})"
[ "${RUN_UID}" != "0" ] || fail "容器以 root 运行（uid=0）"
echo "非 root ✅"

# -----------------------------------------------------------------------------
#  6) HEALTHCHECK
# -----------------------------------------------------------------------------
log "[6/7] 等待 HEALTHCHECK 变为 healthy（最多 ${HEALTH_WAIT_SECONDS}s）"
HEALTH_STATUS="unknown"
HEALTH_DEADLINE="$(( $(date +%s) + HEALTH_WAIT_SECONDS ))"
while :; do
  HEALTH_STATUS="$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' \
                     "${CONTAINER_ID}" 2>/dev/null || echo unknown)"
  [ "${HEALTH_STATUS}" = "healthy" ] && break
  if [ "$(date +%s)" -ge "${HEALTH_DEADLINE}" ]; then
    docker inspect -f '{{json .State.Health}}' "${CONTAINER_ID}" || true
    fail "HEALTHCHECK 在 ${HEALTH_WAIT_SECONDS}s 内未变为 healthy（最后状态=${HEALTH_STATUS}）"
  fi
  sleep 2
done
echo "Health.Status = ${HEALTH_STATUS} ✅"

# -----------------------------------------------------------------------------
#  7) fail-closed：拒绝 auth.mode 降级 / 空密钥
# -----------------------------------------------------------------------------
log "[7/7] 断言镜像拒绝 production 配置降级（auth.mode=disabled / 空密钥）"
set +e
REFUSE_OUT="$(docker run --rm --name "${CONTAINER_NAME}-refuse" \
                -e FSS_JWT_HMAC_SECRET=x -e FSS_TRANSFER_SECRET=y \
                -e FSS_AUTH_MODE=disabled "${IMAGE_TAG}" 2>&1)"
REFUSE_RC=$?
EMPTY_OUT="$(docker run --rm \
                -e FSS_JWT_HMAC_SECRET= -e FSS_TRANSFER_SECRET=y \
                -e FSS_AUTH_MODE=jwt "${IMAGE_TAG}" 2>&1)"
EMPTY_RC=$?
set -e
echo "auth.mode=disabled -> exit ${REFUSE_RC}: ${REFUSE_OUT}"
echo "空 FSS_JWT_HMAC_SECRET -> exit ${EMPTY_RC}: ${EMPTY_OUT}"
[ "${REFUSE_RC}" -ne 0 ] || fail "auth.mode=disabled 竟然启动成功（强制 jwt 失败）"
printf '%s' "${REFUSE_OUT}" | grep -q "拒绝启动" || fail "降级启动的拒绝信息不含『拒绝启动』"
[ "${EMPTY_RC}" -ne 0 ] || fail "空 FSS_JWT_HMAC_SECRET 竟然启动成功（fail-closed 失败）"
printf '%s' "${EMPTY_OUT}" | grep -q "拒绝启动" || fail "空密钥的拒绝信息不含『拒绝启动』"
echo "fail-closed ✅"

# =============================================================================
#  8) 容器硬化运行形态（[H*] 段）—— 证据：docs/test-evidence/phase9-image.md §10
# =============================================================================
#  每个场景：自己起容器 → 断言 readiness/uid/上传读回/峰值内存 → 自己清理。
#  ★ H1 是"记录真实结果"的场景，不作为断言失败（它的失败本身就是发现）。
#  ★ H4a / H4a2（C9.32）是**断言**场景：pids=64 → ExitCode==70 + 可读原因；
#    pids=66 → 正常启动（下界正控）。两者一起才有区分力。
#  ★ H4b（1 GiB 流式）默认跳过：FSS_VERIFY_IMAGE_FULL=1 启用。
# =============================================================================
HARD_DIR="${REPO_ROOT}/build/image-verify-hardening"
HARD_FULL="${FSS_VERIFY_IMAGE_FULL:-0}"

hard_log()  { printf '\n-- [%s] %s\n' "$1" "$2"; }
hard_ok()   { printf '   ✅ %s\n' "$*"; }
hard_bad()  { printf '   ✗ %s\n' "$*"; HARD_FAILURES=$((HARD_FAILURES + 1)); }
hard_note() { printf '   ⚠ [真实发现] %s\n' "$*"; HARD_SOFT+=("$*"); }
record_scenario() { HARD_SUMMARY+=("$1|$2|$3|$4|$5|$6"); }

free_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

# 与被测代码**完全无关**地签发一枚 HS256 token（只用 python3 标准库；
# 覆盖容器里的 JWT **正向**路径 —— 这是 phase9-image.md §6.2 原先登记的未验证项）
mint_test_jwt() {
  python3 - "$1" <<'PY'
import base64, hashlib, hmac, json, sys, time
def b64(b): return base64.urlsafe_b64encode(b).rstrip(b"=")
now = int(time.time())
header = {"alg": "HS256", "typ": "JWT"}
payload = {"sub": "verify-image", "email": "verify@opendes.example.com",
           "data-partition-id": "opendes",
           "roles": ["service.file.editors", "service.file.viewers"],
           "exp": now + 3600, "iat": now}
signing = (b64(json.dumps(header, separators=(",", ":")).encode()) + b"." +
           b64(json.dumps(payload, separators=(",", ":")).encode()))
print((signing + b"." + b64(hmac.new(sys.argv[1].encode(), signing,
                                     hashlib.sha256).digest())).decode())
PY
}

start_hardened() {   # $1=label；其余 = 追加到 docker run 的参数
  local label="$1"; shift
  HARD_DATA="${HARD_DIR}/${label}/data"
  install -d -m 0777 "${HARD_DATA}"
  HARD_PORT="$(free_port)"
  # ★ 运维要点：自签数据面 URL 由 FSS_SELF_BASE_URL 决定。不把它指到**客户端可达**的
  #   宿主端口时，uploadURL 返回的 SignedURL 会是 http://127.0.0.1:8080/...（容器内网
  #   视图）→ 宿主的 curl 不可达。这里固定宿主端口并显式覆盖。
  HARD_CID="$(docker run -d --name "fss-hard-${label}-$$" \
      -p "127.0.0.1:${HARD_PORT}:8080" \
      -v "${HARD_DATA}:/data" \
      -e "FSS_SELF_BASE_URL=http://127.0.0.1:${HARD_PORT}/api/file" \
      -e "FSS_JWT_HMAC_SECRET=${TEST_JWT_SECRET}" \
      -e "FSS_TRANSFER_SECRET=${TEST_TRANSFER_SECRET}" \
      "$@" "${IMAGE_TAG}")"
  HARD_CIDS+=("${HARD_CID}")
  printf '   container=%s host_port=%s\n' "${HARD_CID:0:12}" "${HARD_PORT}"
}

wait_hard_readiness() {   # $1=port $2=秒数 → 打印最后看到的状态码
  # ★ 不能写成 `local port=... seconds=... deadline=$(( ... seconds ))`：整条 `local`
  #   的单词展开发生在赋值**之前** → set -u 下 `seconds` 会被判 unbound。
  local port="$1" seconds="${2:-30}" code=""
  local deadline=$(( $(date +%s) + seconds ))
  while :; do
    code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 \
             "http://127.0.0.1:${port}/api/file/v2/readiness_check" || true)"
    [ "${code}" = "200" ] && break
    [ "$(docker inspect -f '{{.State.Running}}' "${HARD_CID}" 2>/dev/null)" = "true" ] || break
    [ "$(date +%s)" -ge "${deadline}" ] && break
    sleep 0.5
  done
  printf '%s' "${code}"
}

# 端到端：uploadURL → PUT（SignedURL）→ POST metadata → downloadURL → GET → SHA-256 比对
# ★ 大文件必须用 `-T`（流式）：`--data-binary @file` 会让 curl 把**整个文件读进内存**
#   （1 GiB 时实测报 `option --data-binary: out of memory`）—— 那测的就不是服务端了。
hard_e2e() {   # $1=port $2=payload 文件 $3=可选 expected sha → 打印 "PUT=.. GET=.. sha=一致"
  local port="$1" payload="$2" want="${3:-}"
  local base="http://127.0.0.1:${port}/api/file"
  local hdr=(-H "authorization: Bearer ${HARD_TOKEN}" -H 'data-partition-id: opendes')
  [ -n "${want}" ] || want="$(sha256sum "${payload}" | awk '{print $1}')"
  local upload; upload="$(curl -fsS "${hdr[@]}" "${base}/v2/files/uploadURL")" || return 1
  local put_url file_source file_id
  read -r put_url file_source file_id < <(printf '%s' "${upload}" | python3 -c '
import json, sys
d = json.load(sys.stdin)
print(d["Location"]["SignedURL"], d["Location"]["FileSource"], d["FileID"])
') || return 1
  local put_code
  put_code="$(curl -sS -o /dev/null -w '%{http_code}' -X PUT -T "${payload}" \
               "${hdr[@]}" "${put_url}")" || return 1
  [ "${put_code}" = "200" ] || { printf 'PUT=%s' "${put_code}"; return 1; }
  local record; record="$(python3 - "${file_source}" <<'PY'
import json, sys
print(json.dumps({
  "kind": "opendes:wks:dataset--File.Generic:1.0.0",
  "acl": {"viewers": ["data.default.viewers@opendes.example.com"],
          "owners": ["data.default.owners@opendes.example.com"]},
  "legal": {"legaltags": ["opendes-public-1"], "otherRelevantDataCountries": ["US"],
            "status": "compliant"},
  "data": {"Name": "verify-image.bin", "Endian": "LITTLE",
           "DatasetProperties": {"FileSourceInfo": {"FileSource": sys.argv[1]}}},
}))
PY
)"
  curl -fsS "${hdr[@]}" -H 'content-type: application/json' -X POST \
       --data-binary "${record}" "${base}/v2/files/metadata" >/dev/null || return 1
  local download get_url got_sha
  download="$(curl -fsS "${hdr[@]}" "${base}/v2/files/${file_id}/downloadURL")" || return 1
  get_url="$(printf '%s' "${download}" | python3 -c \
             'import json,sys;print(json.load(sys.stdin)["SignedUrl"])')" || return 1
  got_sha="$(curl -fsS "${hdr[@]}" "${get_url}" | sha256sum | awk '{print $1}')" || return 1
  [ "${got_sha}" = "${want}" ] || { printf 'GET sha 不一致'; return 1; }
  printf 'PUT=200 GET=200 sha=一致'
}

hard_cgroup_dir() {   # $1=cid → 该容器在**宿主** cgroup v2 下的目录
  local pid rel
  pid="$(docker inspect -f '{{.State.Pid}}' "$1" 2>/dev/null || echo 0)"
  rel="$(awk -F: '/^0::/{print $3}' "/proc/${pid}/cgroup" 2>/dev/null || true)"
  printf '/sys/fs/cgroup%s' "${rel}"
}
hard_peak_mem() {   # 容器不在运行（或已退出）时返回 "-"（避免读到宿主 root cgroup 的数字）
  local pid; pid="$(docker inspect -f '{{.State.Pid}}' "$1" 2>/dev/null || echo 0)"
  if [ "${pid}" = "0" ] || [ ! -r "/proc/${pid}/cgroup" ]; then printf '-'; return; fi
  cat "$(hard_cgroup_dir "$1")/memory.peak" 2>/dev/null || printf '-'
}
hard_mib() {
  case "${1:-}" in ''|'-') printf '-'; return;; esac
  awk -v b="$1" 'BEGIN{printf "%.1f MiB", b/1048576}'
}
# 进程**峰值 RSS**（`/proc/1/status` 的 VmHWM）：这是"流式 vs 整体读入"的判据。
# 与 cgroup `memory.peak` 不同 —— 后者含记在容器 memcg 上的**写页缓存**。
hard_vmhwm_mib() {
  local kb
  kb="$(docker exec "$1" awk '/^VmHWM/{print $2}' /proc/1/status 2>/dev/null || true)"
  case "${kb}" in ''|*[!0-9]*) printf '-'; return;; esac
  awk -v k="${kb}" 'BEGIN{printf "%.1f MiB", k/1024}'
}

start_mem_sampler() {   # $1=cid $2=输出文件（每 0.25 s 采一次 memory.current）
  local cg; cg="$(hard_cgroup_dir "$1")"
  ( for _ in $(seq 1 4000); do
      cat "${cg}/memory.current" 2>/dev/null || break
      sleep 0.25
    done ) >"$2" 2>/dev/null &
  MEM_SAMPLER_PID=$!
}
stop_mem_sampler() {
  [ -z "${MEM_SAMPLER_PID}" ] || kill "${MEM_SAMPLER_PID}" >/dev/null 2>&1 || true
  MEM_SAMPLER_PID=""
}

# 每个场景**跑完立即** docker rm -f（不把容器留到脚本退出；trap 仍然兜底）
end_scenario() { docker rm -f "${HARD_CID}" >/dev/null 2>&1 || true; }

# --- 准备：清掉上次运行残留（容器以 uid 10001 建的子目录，宿主删不掉 → wipe_data_dir）---
mkdir -p "${HARD_DIR}"
wipe_data_dir "${HARD_DIR}"
install -d -m 0777 "${HARD_DIR}"
HARD_TOKEN="$(mint_test_jwt "${TEST_JWT_SECRET}")"
HARD_PAYLOAD_SMALL="${HARD_DIR}/payload-small.bin"
head -c 262144 /dev/urandom >"${HARD_PAYLOAD_SMALL}"
HARD_SMALL_SHA="$(sha256sum "${HARD_PAYLOAD_SMALL}" | awk '{print $1}')"
echo "测试 JWT 已签发（HS256，roles=editors/viewers，partition=opendes）；小载荷 256 KiB"

# -----------------------------------------------------------------------------
#  H1：只读根文件系统（**不带** --tmpfs /tmp）—— 先记录真实结果
# -----------------------------------------------------------------------------
hard_log H1 "只读根文件系统：docker run --read-only（不带 --tmpfs /tmp）"
start_hardened ro_notmpfs --read-only
H1_CODE="$(wait_hard_readiness "${HARD_PORT}" 30)"
H1_UID="$(docker exec "${HARD_CID}" id -u 2>/dev/null || echo NA)"
H1_EROFS="$(docker exec "${HARD_CID}" touch /probe-write 2>&1 || true)"
H1_TMP="$(docker exec "${HARD_CID}" touch /tmp/probe-write 2>&1 || true)"
printf '   readiness=%s  uid=%s\n' "${H1_CODE}" "${H1_UID}"
printf '   根文件系统写入探测: %s\n' "${H1_EROFS:-（成功——根文件系统可写！）}"
printf '   /tmp 写入探测     : %s\n' "${H1_TMP:-（成功——/tmp 可写）}"
if [ "${H1_CODE}" = "200" ]; then
  [ "${H1_UID}" = "10001" ] && hard_ok "uid=10001" || hard_bad "uid=${H1_UID}（期望 10001）"
  case "${H1_EROFS}" in
    *"Read-only file system"*) hard_ok "根文件系统确实只读（touch /probe-write 被 EROFS 拒绝）" ;;
    *) hard_bad "根文件系统写入未被拒：${H1_EROFS}" ;;
  esac
  if H1_E2E="$(hard_e2e "${HARD_PORT}" "${HARD_PAYLOAD_SMALL}" "${HARD_SMALL_SHA}")"; then
    hard_ok "上传+读回通过（${H1_E2E}）"
  else
    hard_bad "上传+读回失败（${H1_E2E:-无输出}）"; H1_E2E="${H1_E2E:-失败}"
  fi
  H1_VERDICT="通过（**不需要**可写 rootfs，也不需要可写 /tmp）"
else
  H1_E2E="未做（readiness=${H1_CODE}）"
  H1_VERDICT="**真实发现**：read-only 下起不来 → 需要可写路径"
  hard_note "H1 未通过（readiness=${H1_CODE}）：服务需要可写路径。容器日志尾部：$(docker logs "${HARD_CID}" 2>&1 | tail -3 | tr '\n' ' ')"
fi
record_scenario "H1 read-only（无 tmpfs）" "${H1_CODE}" "${H1_UID}" "${H1_E2E}" "$(hard_vmhwm_mib "${HARD_CID}")" "${H1_VERDICT}"
end_scenario

# -----------------------------------------------------------------------------
#  H2：只读根文件系统 + --tmpfs /tmp（要求 readiness 200 且上传+读回成功）
# -----------------------------------------------------------------------------
hard_log H2 "只读根文件系统 + tmpfs：--read-only --tmpfs /tmp:rw,size=64m,mode=1777"
start_hardened ro_tmpfs --read-only --tmpfs /tmp:rw,size=64m,mode=1777
H2_CODE="$(wait_hard_readiness "${HARD_PORT}" 30)"
H2_UID="$(docker exec "${HARD_CID}" id -u 2>/dev/null || echo NA)"
H2_TMP="$(docker exec "${HARD_CID}" touch /tmp/probe-write 2>&1 && echo OK || echo FAILED)"
H2_EROFS="$(docker exec "${HARD_CID}" touch /probe-write 2>&1 || true)"
printf '   readiness=%s  uid=%s  /tmp 写入=%s\n' "${H2_CODE}" "${H2_UID}" "${H2_TMP}"
printf '   根文件系统写入探测: %s\n' "${H2_EROFS}"
[ "${H2_CODE}" = "200" ] && hard_ok "readiness=200" || hard_bad "readiness=${H2_CODE}（期望 200）"
[ "${H2_UID}" = "10001" ] && hard_ok "uid=10001" || hard_bad "uid=${H2_UID}（期望 10001）"
[ "${H2_TMP}" = "OK" ] && hard_ok "tmpfs /tmp 可写（挂上了）" || hard_bad "/tmp 不可写"
case "${H2_EROFS}" in
  *"Read-only file system"*) hard_ok "根文件系统仍只读" ;;
  *) hard_bad "根文件系统写入未被拒：${H2_EROFS}" ;;
esac
if H2_E2E="$(hard_e2e "${HARD_PORT}" "${HARD_PAYLOAD_SMALL}" "${HARD_SMALL_SHA}")"; then
  hard_ok "上传+读回通过（${H2_E2E}）"
else
  hard_bad "上传+读回失败（${H2_E2E:-无输出}）"; H2_E2E="${H2_E2E:-失败}"
fi
H2_PEAK="$(hard_vmhwm_mib "${HARD_CID}")"
record_scenario "H2 read-only + tmpfs /tmp" "${H2_CODE}" "${H2_UID}" "${H2_E2E}" "${H2_PEAK}" "通过"
end_scenario

# -----------------------------------------------------------------------------
#  H3：最小权限（--cap-drop=ALL --security-opt no-new-privileges；Docker 默认 seccomp）
# -----------------------------------------------------------------------------
hard_log H3 "最小权限：--cap-drop=ALL --security-opt no-new-privileges（**默认 seccomp**）"
start_hardened minpriv --cap-drop=ALL --security-opt no-new-privileges
H3_CODE="$(wait_hard_readiness "${HARD_PORT}" 30)"
H3_UID="$(docker exec "${HARD_CID}" id -u 2>/dev/null || echo NA)"
H3_CAPS="$(docker exec "${HARD_CID}" grep -E '^(CapBnd|CapEff|NoNewPrivs|Seccomp)' /proc/1/status 2>/dev/null | tr -s '\t' ' ' | tr '\n' ';' || echo NA)"
H3_SECOPT="$(docker inspect -f '{{json .HostConfig.SecurityOpt}} {{json .HostConfig.CapDrop}}' "${HARD_CID}")"
printf '   readiness=%s  uid=%s\n   %s\n   HostConfig: %s\n' "${H3_CODE}" "${H3_UID}" "${H3_CAPS}" "${H3_SECOPT}"
[ "${H3_CODE}" = "200" ] && hard_ok "readiness=200" || hard_bad "readiness=${H3_CODE}（期望 200）"
[ "${H3_UID}" = "10001" ] && hard_ok "uid=10001" || hard_bad "uid=${H3_UID}（期望 10001）"
case "${H3_CAPS}" in
  *"CapBnd: 0000000000000000"*) hard_ok "CapBnd=0（cap-drop=ALL 生效）" ;;
  *) hard_bad "CapBnd 非零：${H3_CAPS}" ;;
esac
case "${H3_CAPS}" in
  *"NoNewPrivs: 1"*) hard_ok "NoNewPrivs=1" ;;
  *) hard_bad "NoNewPrivs!=1：${H3_CAPS}" ;;
esac
case "${H3_CAPS}" in
  *"Seccomp: 2"*) hard_ok "Seccomp=2（Docker **默认** profile 生效；未使用 seccomp=unconfined）" ;;
  *) hard_bad "Seccomp!=2（默认 seccomp 未生效）：${H3_CAPS}" ;;
esac
case "${H3_SECOPT}" in
  *unconfined*) hard_bad "SecurityOpt 里出现了 seccomp=unconfined（本场景不允许）：${H3_SECOPT}" ;;
  *) hard_ok "SecurityOpt 中无 seccomp=unconfined：${H3_SECOPT}" ;;
esac
if H3_E2E="$(hard_e2e "${HARD_PORT}" "${HARD_PAYLOAD_SMALL}" "${HARD_SMALL_SHA}")"; then
  hard_ok "上传+读回通过（${H3_E2E}）"
else
  hard_bad "上传+读回失败（${H3_E2E:-无输出}）"; H3_E2E="${H3_E2E:-失败}"
fi
record_scenario "H3 cap-drop=ALL + NNP" "${H3_CODE}" "${H3_UID}" "${H3_E2E}" "$(hard_vmhwm_mib "${HARD_CID}")" "通过（CapBnd=0/NNP=1）"
end_scenario

# -----------------------------------------------------------------------------
#  H4a：资源上限的**任务原文形态** --memory=128m --pids-limit=64
#  ★ C9.32 之后这里从"记录真实结果"**升级为断言**：pids=64 < 默认线程数 66 →
#    启动期 `pthread_create` 返回 EAGAIN → `std::thread` 抛 `std::system_error`
#    → 被 `main()` 顶层 catch 接住 → **ExitCode=70（EX_SOFTWARE）+ 可读原因**；
#    **不再是** `terminate called` / 139。
#  ★ 下界正控见紧随其后的 H4a2（pids=66 必须正常启动）—— 否则"pids=64 失败"可能
#    只是"容器根本没起来"（AGENTS §4.3：否定判据必须配正控）。
# -----------------------------------------------------------------------------
hard_log H4a "资源上限（任务原文形态）：--memory=128m --pids-limit=64 → 期望 exit 70 + 可读原因"
start_hardened limits_128m_64pids --memory=128m --pids-limit=64
H4A_CODE=""
for _ in $(seq 1 40); do
  H4A_CODE="$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 \
               "http://127.0.0.1:${HARD_PORT}/api/file/v2/readiness_check" || true)"
  [ "${H4A_CODE}" = "200" ] && break
  [ "$(docker inspect -f '{{.State.Running}}' "${HARD_CID}" 2>/dev/null)" = "false" ] && break
  sleep 0.5
done
H4A_EXIT="$(docker inspect -f '{{.State.ExitCode}}' "${HARD_CID}" 2>/dev/null || echo NA)"
H4A_OOM="$(docker inspect -f '{{.State.OOMKilled}}' "${HARD_CID}" 2>/dev/null || echo NA)"
H4A_LOG="$(docker logs "${HARD_CID}" 2>&1 || true)"
H4A_ERR="$(printf '%s' "${H4A_LOG}" | grep -m2 '未预期异常' | tr '\n' ' ' || true)"
H4A_TERM="$(printf '%s' "${H4A_LOG}" | grep -c 'terminate called' || true)"
printf '   readiness=%s  ExitCode=%s  OOMKilled=%s\n' "${H4A_CODE}" "${H4A_EXIT}" "${H4A_OOM}"
printf '   日志: %s\n' "${H4A_ERR:-（没有「未预期异常」行）}"
#  ① 退出码必须是 70（EX_SOFTWARE），不是 terminate 的 139/134
[ "${H4A_EXIT}" = "70" ] && hard_ok "ExitCode=70（EX_SOFTWARE；不再是 terminate/139）" \
                         || hard_bad "ExitCode=${H4A_EXIT}（期望 70）"
#  ② stderr 必须给出可读原因（只有退出码、没有原因 = 对运维仍不可读）
printf '%s' "${H4A_LOG}" | grep -q '未预期异常' \
  && hard_ok "stderr 含「未预期异常」（异常被顶层接住，原因可读）" \
  || hard_bad "stderr 没有「未预期异常」"
#  ③ 绝不能出现原始 terminate 文案
[ "${H4A_TERM}" -eq 0 ] && hard_ok "stderr **不含** terminate called" \
                        || hard_bad "stderr 出现 ${H4A_TERM} 处 terminate called"
#  ④ 不是 OOM（与 exit 70 一起排除"内存不足"这个错误归因）
[ "${H4A_OOM}" = "false" ] && hard_ok "OOMKilled=false（确实不是内存不足）" \
                           || hard_bad "OOMKilled=${H4A_OOM}"
#  ⑤ 该上限下服务**没有**起来（66 个任务装不进 64 的 pids cgroup）
[ "${H4A_CODE}" = "200" ] && hard_bad "pids=64 竟然 readiness=200（下界结论需修正）" \
                          || hard_ok "readiness=${H4A_CODE}（未进入服务状态，符合预期）"
H4A_UID="NA"
record_scenario "H4a 128m + pids=64" "${H4A_CODE}" "${H4A_UID}" "-" "-" \
  "**已修（C9.32）**：exit ${H4A_EXIT}（EX_SOFTWARE）+ 可读「未预期异常」；无 terminate/139（非 OOM）"
end_scenario

# -----------------------------------------------------------------------------
#  H4a2：下界**正控** —— pids=66 必须正常启动（默认线程数下界未变）
# -----------------------------------------------------------------------------
hard_log H4a2 "pids 下界正控：--pids-limit=66 必须正常启动（下界=66 未变）"
start_hardened limits_pids_66 --pids-limit=66
H4A2_CODE="$(wait_hard_readiness "${HARD_PORT}" 30)"
H4A2_EXIT="$(docker inspect -f '{{.State.ExitCode}}' "${HARD_CID}" 2>/dev/null || echo NA)"
H4A2_TERM="$(docker logs "${HARD_CID}" 2>&1 | grep -c 'terminate called' || true)"
printf '   pids=66 → readiness=%s  ExitCode=%s  terminate=%s\n' \
  "${H4A2_CODE}" "${H4A2_EXIT}" "${H4A2_TERM}"
[ "${H4A2_CODE}" = "200" ] && hard_ok "pids=66 readiness=200（下界=66 未变）" \
                           || hard_bad "pids=66 readiness=${H4A2_CODE}（下界结论需修正）"
record_scenario "H4a2 128m + pids=66" "${H4A2_CODE}" "10001" "-" "-" \
  "下界正控：pids=66 正常启动（下界=66 未变）"
end_scenario

# -----------------------------------------------------------------------------
#  H4b（FULL）：--memory=128m --pids-limit=256 下的 1 GiB 上传+读回 + 峰值内存
# -----------------------------------------------------------------------------
if [ "${HARD_FULL}" = "1" ]; then
  hard_log H4b "资源上限下的 1 GiB 流式：--memory=128m --pids-limit=256"
  HARD_PAYLOAD_BIG="${HARD_DIR}/payload-1g.bin"
  truncate -s 1073741824 "${HARD_PAYLOAD_BIG}"     # 稀疏文件：不占磁盘
  H4B_WANT="$(sha256sum "${HARD_PAYLOAD_BIG}" | awk '{print $1}')"
  echo "   1 GiB 载荷（稀疏，内容全 0）预期 SHA-256 = ${H4B_WANT}"
  start_hardened limits_1g --memory=128m --pids-limit=256
  H4B_CODE="$(wait_hard_readiness "${HARD_PORT}" 30)"
  H4B_UID="$(docker exec "${HARD_CID}" id -u 2>/dev/null || echo NA)"
  printf '   readiness=%s  uid=%s\n' "${H4B_CODE}" "${H4B_UID}"
  [ "${H4B_CODE}" = "200" ] && hard_ok "readiness=200" || hard_bad "readiness=${H4B_CODE}（期望 200）"
  [ "${H4B_UID}" = "10001" ] && hard_ok "uid=10001" || hard_bad "uid=${H4B_UID}（期望 10001）"
  start_mem_sampler "${HARD_CID}" "${HARD_DIR}/limits_1g.mem"
  H4B_RSS_BEFORE="$(docker exec "${HARD_CID}" awk '/^VmHWM/{print $2}' /proc/1/status 2>/dev/null || echo 0)"
  H4B_T0="$(date +%s.%N)"
  if H4B_E2E="$(hard_e2e "${HARD_PORT}" "${HARD_PAYLOAD_BIG}" "${H4B_WANT}")"; then
    hard_ok "1 GiB 上传+读回通过（${H4B_E2E}）"
  else
    hard_bad "1 GiB 上传+读回失败（${H4B_E2E:-无输出}）"; H4B_E2E="${H4B_E2E:-失败}"
  fi
  H4B_SECONDS="$(awk -v a="${H4B_T0}" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b-a}')"
  stop_mem_sampler
  # ★ 两个不同的"峰值"必须分开报，否则会得出错误结论：
  #   ① 进程峰值 RSS = /proc/1/status 的 VmHWM（KiB）—— 这才是"流式 vs 整体读入"的判据；
  #   ② cgroup memory.peak（字节）—— **包含写 1 GiB 时记在容器 memcg 上的页缓存**，
  #      因此会贴到 --memory 上限附近；它说明的是内存上限生效，不是进程占用了 128 MiB。
  H4B_VMHWM_KB="$(docker exec "${HARD_CID}" awk '/^VmHWM/{print $2}' /proc/1/status 2>/dev/null || echo 0)"
  H4B_PEAK="$(hard_peak_mem "${HARD_CID}")"
  H4B_SAMPLE_MAX="$(sort -n "${HARD_DIR}/limits_1g.mem" 2>/dev/null | tail -1 || echo 0)"
  H4B_STATS="$(docker stats --no-stream --format '{{.MemUsage}} {{.MemPerc}} pids={{.PIDs}}' "${HARD_CID}")"
  H4B_VMHWM_BYTES="$(( H4B_VMHWM_KB * 1024 ))"
  printf '   1 GiB 往返耗时=%ss\n' "${H4B_SECONDS}"
  printf '   进程峰值 RSS(VmHWM)=%s (%s KiB；传输前 %s KiB)  docker stats 当前=%s\n' \
    "$(hard_mib "${H4B_VMHWM_BYTES}")" "${H4B_VMHWM_KB}" "${H4B_RSS_BEFORE}" "${H4B_STATS}"
  printf '   cgroup memory.peak=%s（含写 1 GiB 的页缓存；--memory 上限=128.0 MiB） 采样峰值=%s\n' \
    "$(hard_mib "${H4B_PEAK}")" "$(hard_mib "${H4B_SAMPLE_MAX}")"
  printf '   OOMKilled=%s ExitCode=%s\n' "$(docker inspect -f '{{.State.OOMKilled}}' "${HARD_CID}")" \
    "$(docker inspect -f '{{.State.ExitCode}}' "${HARD_CID}")"
  # ★ 判据：1 GiB 载荷在 128 MiB 上限下**成功**本身即证明"流式而非整体读入"
  #   （整体读入需要 ≥1 GiB 内存，必然被 cgroup 上限拦下）；再断言**进程**峰值 RSS < 128 MiB。
  if [ "${H4B_VMHWM_KB}" -gt 0 ] && [ "${H4B_VMHWM_BYTES}" -lt 134217728 ]; then
    hard_ok "进程峰值 RSS $(hard_mib "${H4B_VMHWM_BYTES}") < 上限 128 MiB ≪ 载荷 1 GiB（流式）"
  else
    hard_bad "进程峰值 RSS 异常：VmHWM=${H4B_VMHWM_KB} KiB"
  fi
  if [ "${H4B_PEAK}" -le 138412032 ]; then
    hard_ok "cgroup memory.peak $(hard_mib "${H4B_PEAK}") 未超过上限 + 512 KiB 容差（页缓存被回收而非 OOM）"
  else
    hard_bad "cgroup memory.peak=${H4B_PEAK} 明显超过上限"
  fi
  record_scenario "H4b 1 GiB 流式（128m/256pids）" "${H4B_CODE}" "${H4B_UID}" "${H4B_E2E}" \
    "$(hard_mib "${H4B_VMHWM_BYTES}")" "通过（${H4B_SECONDS}s；进程峰值 ≪1 GiB）"
  end_scenario

  # ---- 下界探测：pids 上限的最小可行值（默认线程数 66）----
  hard_log H4b2 "pids 上限下界探测：--pids-limit=65（期望失败）与 66（期望成功）"
  for n in 65 66; do
    start_hardened "pids_${n}" "--pids-limit=${n}"
    local_code=""
    for _ in $(seq 1 40); do
      local_code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 \
                     "http://127.0.0.1:${HARD_PORT}/api/file/v2/readiness_check" || true)"
      [ "${local_code}" = "200" ] && break
      [ "$(docker inspect -f '{{.State.Running}}' "${HARD_CID}" 2>/dev/null)" = "false" ] && break
      sleep 0.5
    done
    printf '   --pids-limit=%s → readiness=%s ExitCode=%s\n' "${n}" "${local_code}" \
      "$(docker inspect -f '{{.State.ExitCode}}' "${HARD_CID}" 2>/dev/null || echo NA)"
    if [ "${n}" = "65" ]; then
      [ "${local_code}" = "200" ] && hard_bad "pids=65 竟然可用（下界结论需修正）" \
                                  || hard_ok "pids=65 不可用（与下界一致）"
    else
      [ "${local_code}" = "200" ] && hard_ok "pids=66 可用（= 默认线程数下界）" \
                                  || hard_bad "pids=66 不可用（下界结论需修正）"
    fi
    record_scenario "H4b2 pids=${n} 下界" "${local_code}" "-" "-" "-" \
      "$([ "${n}" = "65" ] && echo '预期失败（下界）' || echo '预期成功')"
    end_scenario
  done
else
  echo
  echo "-- [H4b] 跳过 1 GiB 流式场景与 pids 下界探测（设 FSS_VERIFY_IMAGE_FULL=1 启用）"
fi

# -----------------------------------------------------------------------------
#  H5：--restart=on-failure + HEALTHCHECK healthy + SIGTERM 优雅退出（GC 调度在跑）
# -----------------------------------------------------------------------------
hard_log H5 "健康检查 + --restart=on-failure + SIGTERM 优雅退出（GC 调度线程在跑）"
printf '{"gc":{"enabled":true,"interval_seconds":2}}\n' >"${HARD_DIR}/gc.json"
# ★ entrypoint **不转发**命令行参数，所以只能用环境变量 FSS_CONFIG 指定配置文件
start_hardened health_gc --restart=on-failure \
  -v "${HARD_DIR}/gc.json:/etc/fss/gc.json:ro" -e FSS_CONFIG=/etc/fss/gc.json
H5_CODE="$(wait_hard_readiness "${HARD_PORT}" 30)"
[ "${H5_CODE}" = "200" ] && hard_ok "readiness=200" || hard_bad "readiness=${H5_CODE}"
H5_GC_BANNER="$(docker logs "${HARD_CID}" 2>&1 | grep -m1 '^  gc ' || true)"
printf '   %s\n' "${H5_GC_BANNER:-（横幅里没有 gc 行）}"
case "${H5_GC_BANNER}" in
  *已启动*) hard_ok "GC 周期调度确实在跑（退出路径必须 join 它）" ;;
  *) hard_bad "GC 未启动：${H5_GC_BANNER}" ;;
esac
H5_GC_RUNS="$(curl -s "http://127.0.0.1:${HARD_PORT}/metrics" | grep -m1 '^fss_gc_runs_total' || true)"
printf '   %s\n' "${H5_GC_RUNS:-（/metrics 里没有 fss_gc_runs_total）}"
if H5_E2E="$(hard_e2e "${HARD_PORT}" "${HARD_PAYLOAD_SMALL}" "${HARD_SMALL_SHA}")"; then
  hard_ok "上传+读回通过（${H5_E2E}）"
else
  hard_bad "上传+读回失败（${H5_E2E:-无输出}）"; H5_E2E="${H5_E2E:-失败}"
fi
H5_HEALTH="unknown"
H5_HEALTH_DEADLINE="$(( $(date +%s) + HEALTH_WAIT_SECONDS ))"
while :; do
  H5_HEALTH="$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' \
                "${HARD_CID}" 2>/dev/null || echo unknown)"
  [ "${H5_HEALTH}" = "healthy" ] && break
  [ "$(date +%s)" -ge "${H5_HEALTH_DEADLINE}" ] && break
  sleep 1
done
H5_FAILSTREAK="$(docker inspect -f '{{if .State.Health}}{{.State.Health.FailingStreak}}{{else}}-1{{end}}' "${HARD_CID}")"
printf '   Health.Status=%s FailingStreak=%s RestartCount=%s\n' \
  "${H5_HEALTH}" "${H5_FAILSTREAK}" "$(docker inspect -f '{{.RestartCount}}' "${HARD_CID}")"
[ "${H5_HEALTH}" = "healthy" ] && hard_ok "HEALTHCHECK healthy" || hard_bad "HEALTHCHECK=${H5_HEALTH}"
# ---- SIGTERM（docker stop 默认 SIGTERM；-t 10 = 10s grace）----
H5_T0="$(date +%s.%N)"
docker stop -t 10 "${HARD_CID}" >/dev/null
H5_STOP_SECONDS="$(awk -v a="${H5_T0}" -v b="$(date +%s.%N)" 'BEGIN{printf "%.2f", b-a}')"
H5_EXIT="$(docker inspect -f '{{.State.ExitCode}}' "${HARD_CID}")"
H5_OOM="$(docker inspect -f '{{.State.OOMKilled}}' "${HARD_CID}")"
H5_RESTARTS="$(docker inspect -f '{{.RestartCount}}' "${HARD_CID}")"
H5_BAD_SIGNS="$(docker logs "${HARD_CID}" 2>&1 | grep -cE 'terminate called|core dumped|Aborted|deadlock' || true)"
printf '   docker stop -t 10: 耗时=%ss ExitCode=%s OOMKilled=%s RestartCount=%s 异常迹象行数=%s\n' \
  "${H5_STOP_SECONDS}" "${H5_EXIT}" "${H5_OOM}" "${H5_RESTARTS}" "${H5_BAD_SIGNS}"
# ① 退出码 0 = 信号处理器正常返回（若走默认处置会是 143；被 SIGKILL 是 137）
case "${H5_EXIT}" in
  0) hard_ok "ExitCode=0（SIGTERM 由处理器接管并正常退出，非 143/137）" ;;
  143) hard_note "ExitCode=143（进程未接管 SIGTERM，走默认处置）" ;;
  137) hard_bad "ExitCode=137：grace 期内未退出，被 SIGKILL 强杀" ;;
  *) hard_bad "ExitCode=${H5_EXIT}（非预期）" ;;
esac
# ② 耗时 < grace（10s）
if awk -v s="${H5_STOP_SECONDS}" 'BEGIN{exit !(s < 10)}'; then
  hard_ok "stop 耗时 ${H5_STOP_SECONDS}s < 10s grace（未被超时强杀）"
else
  hard_bad "stop 耗时 ${H5_STOP_SECONDS}s ≥ grace"
fi
[ "${H5_OOM}" = "false" ] && hard_ok "OOMKilled=false" || hard_bad "OOMKilled=${H5_OOM}"
[ "${H5_RESTARTS}" = "0" ] && hard_ok "RestartCount=0（--restart=on-failure 下干净退出不重启）" \
                          || hard_bad "RestartCount=${H5_RESTARTS}"
[ "${H5_BAD_SIGNS}" = "0" ] && hard_ok "退出路径无 terminate/core/abort 迹象（GC 调度线程已 join，无死锁）" \
                            || hard_bad "日志里有 ${H5_BAD_SIGNS} 行异常迹象"
record_scenario "H5 healthcheck+SIGTERM（GC 在跑）" "${H5_CODE}" 10001 "${H5_E2E:-失败}" \
  "$(hard_vmhwm_mib "${HARD_CID}")" \
  "优雅退出 exit=${H5_EXIT}，${H5_STOP_SECONDS}s < 10s grace，无残留线程迹象"
end_scenario

# -----------------------------------------------------------------------------
#  H6：健康检查**负控** —— 真实服务，但探针指向错误地址 → 必须 unhealthy
# -----------------------------------------------------------------------------
hard_log H6 "健康检查负控：FSS_BIND_ADDRESS=127.0.0.2（探针仍打 127.0.0.1:8080）"
start_hardened health_neg -e FSS_BIND_ADDRESS=127.0.0.2 \
  --health-interval=2s --health-retries=2 --health-start-period=1s
H6_BIND="$(docker logs "${HARD_CID}" 2>&1 | grep -m1 '^  bind ' || true)"
H6_DIRECT="$(docker exec "${HARD_CID}" /usr/local/bin/healthcheck.sh 2>&1 || true)"
printf '   %s\n   容器内直接执行探针 → %s\n' "${H6_BIND:-（无 bind 行）}" "${H6_DIRECT:-（无输出）}"
case "${H6_BIND}" in
  *127.0.0.2:8080*) hard_ok "服务**确实在跑**（只是绑定 127.0.0.2，不在探针地址上）" ;;
  *) hard_bad "未确认服务在跑：${H6_BIND}" ;;
esac
case "${H6_DIRECT}" in
  *"Connection refused"*|*无法连接*) hard_ok "探针在容器内直接执行即失败（证明确实去连了业务端口）" ;;
  *) hard_bad "探针竟然成功：${H6_DIRECT}" ;;
esac
H6_HEALTH="unknown"
H6_DEADLINE="$(( $(date +%s) + 20 ))"
while :; do
  H6_HEALTH="$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' \
                "${HARD_CID}" 2>/dev/null || echo unknown)"
  [ "${H6_HEALTH}" = "unhealthy" ] && break
  [ "$(date +%s)" -ge "${H6_DEADLINE}" ] && break
  sleep 1
done
H6_FAILSTREAK="$(docker inspect -f '{{if .State.Health}}{{.State.Health.FailingStreak}}{{else}}-1{{end}}' "${HARD_CID}")"
H6_LOG="$(docker inspect -f '{{if .State.Health}}{{range .State.Health.Log}}{{.Output}}{{end}}{{else}}none{{end}}' \
           "${HARD_CID}" 2>/dev/null | tr '\n' ' ' || true)"
printf '   Health.Status=%s FailingStreak=%s 探针输出=%s\n' "${H6_HEALTH}" "${H6_FAILSTREAK}" "${H6_LOG}"
[ "${H6_HEALTH}" = "unhealthy" ] && hard_ok "探针变 unhealthy（**不是恒 healthy**）" \
                                  || hard_bad "期望 unhealthy，实际 ${H6_HEALTH}"
[ "${H6_FAILSTREAK}" -ge 2 ] 2>/dev/null && hard_ok "FailingStreak=${H6_FAILSTREAK} ≥ 2" \
                                          || hard_bad "FailingStreak=${H6_FAILSTREAK}（期望 ≥2）"
record_scenario "H6 健康检查负控（错地址）" "unhealthy" "-" "-" "-" \
  "unhealthy（FailingStreak=${H6_FAILSTREAK}）→ 与 H5 的 healthy 构成区分"
end_scenario

# -----------------------------------------------------------------------------
#  H7：反向对照（R1）
# -----------------------------------------------------------------------------
hard_log H7a "反向对照①：去掉 --read-only（其余同 H1）→ 镜像自带 /tmp（mode 1777）应恢复可写"
start_hardened ctrl_writable
H7A_CODE="$(wait_hard_readiness "${HARD_PORT}" 30)"
H7A_UID="$(docker exec "${HARD_CID}" id -u 2>/dev/null || echo NA)"
# ★ 探测点必须选**非 root 也能写**的目录：镜像里的 `/` 属主 root、mode 0755，uid 10001 在
#   **不带** --read-only 时也写不进去（EACCES）。用它当"根文件系统可写"的判据会把 EROFS 与
#   EACCES 混为一谈。`/tmp`（1777）才是干净的探测点。
H7A_TOUCH_TMP="$(docker exec "${HARD_CID}" touch /tmp/probe-write 2>&1 && echo OK || echo FAILED)"
H7A_TOUCH_ROOT="$(docker exec "${HARD_CID}" touch /probe-write 2>&1 || true)"
printf '   readiness=%s uid=%s\n   touch /tmp/probe-write → %s\n   touch /probe-write → %s（非 root 在 / 上本就 EACCES，不具区分力）\n' \
  "${H7A_CODE}" "${H7A_UID}" "${H7A_TOUCH_TMP}" "${H7A_TOUCH_ROOT}"
[ "${H7A_CODE}" = "200" ] && hard_ok "去掉 --read-only 后 readiness 仍 200" || hard_bad "readiness=${H7A_CODE}"
[ "${H7A_TOUCH_TMP}" = "OK" ] && hard_ok "去掉 --read-only 后 /tmp 可写 → H1 的 EROFS 确实来自 --read-only" \
                              || hard_bad "去掉 --read-only 后 /tmp 仍不可写：${H7A_TOUCH_TMP}"
if H7A_E2E="$(hard_e2e "${HARD_PORT}" "${HARD_PAYLOAD_SMALL}" "${HARD_SMALL_SHA}")"; then
  printf '   上传+读回（对照）= %s\n' "${H7A_E2E}"
else
  printf '   上传+读回（对照）失败 = %s\n' "${H7A_E2E:-无输出}"; H7A_E2E="失败"
fi
hard_note "对照①的诚实结论：readiness 与上传读回在『带 / 不带 --read-only』两种形态下**都成功** ⇒ 这两条断言对『只读』**不具区分力**。有区分力的是『可写目录里的 touch』：H1（带 --read-only）在 /tmp 上是 EROFS『Read-only file system』、对照（不带）在 /tmp 上成功；而 /probe-write 在两种形态下都被拒（EROFS vs EACCES）—— 只看『被拒』会误判。"
record_scenario "H7a 对照①无 --read-only" "${H7A_CODE}" "${H7A_UID}" "${H7A_E2E:-失败}" \
  "$(hard_vmhwm_mib "${HARD_CID}")" "/tmp 可写 → 差异确实来自 --read-only"
end_scenario

hard_log H7b "反向对照②：不带 --cap-drop=ALL / no-new-privileges → CapBnd 应非零、NoNewPrivs=0"
start_hardened ctrl_caps
H7B_CODE="$(wait_hard_readiness "${HARD_PORT}" 30)"
H7B_CAPS="$(docker exec "${HARD_CID}" grep -E '^(CapBnd|CapEff|NoNewPrivs)' /proc/1/status 2>/dev/null | tr -s '\t' ' ' | tr '\n' ';' || echo NA)"
printf '   readiness=%s  %s\n' "${H7B_CODE}" "${H7B_CAPS}"
case "${H7B_CAPS}" in
  *"CapBnd: 0000000000000000"*) hard_bad "默认形态 CapBnd=0（对照失效）" ;;
  *) hard_ok "默认形态 CapBnd 非零（与 H3 的 0 构成对照）" ;;
esac
case "${H7B_CAPS}" in
  *"NoNewPrivs: 0"*) hard_ok "默认形态 NoNewPrivs=0（与 H3 的 1 构成对照）" ;;
  *) hard_bad "默认形态 NoNewPrivs!=0：${H7B_CAPS}" ;;
esac
case "${H7B_CAPS}" in
  *"CapEff: 0000000000000000"*) hard_note "对照②的诚实结论：本镜像以 **非 root** 运行，CapEff 在『默认』与『cap-drop=ALL』两种形态下**都是 0** ⇒ --cap-drop=ALL 不改变进程**当前**可用的能力，它缩小的是 CapBnd（加上 no-new-privileges 的 NNP 位）；该判据只在 CapBnd/NoNewPrivs 两个字段上有区分力。" ;;
esac
record_scenario "H7b 对照②默认 caps" "${H7B_CODE}" 10001 "-" "-" \
  "CapBnd 非零 / NNP=0 → H3 的 CapBnd=0 / NNP=1 确实来自那两个开关"
end_scenario

# -----------------------------------------------------------------------------
#  H8：默认 seccomp（io_uring 不可用）下的端点行为 + 暴露面实测（C9.30 可测的一半）
# -----------------------------------------------------------------------------
hard_log H8 "默认 seccomp（io_uring 不可用）下端点行为不变 + 暴露面实测（C9.30）"
start_hardened default_seccomp   # 不加任何安全参数 = Docker 默认 seccomp profile
H8_CODE="$(wait_hard_readiness "${HARD_PORT}" 30)"
H8_SECCOMP="$(docker exec "${HARD_CID}" grep -E '^Seccomp' /proc/1/status 2>/dev/null | tr -s '\t' ' ' || echo NA)"
printf '   readiness=%s  %s\n' "${H8_CODE}" "${H8_SECCOMP}"
[ "${H8_CODE}" = "200" ] && hard_ok "readiness=200" || hard_bad "readiness=${H8_CODE}"
case "${H8_SECCOMP}" in
  *"Seccomp: 2"*) hard_ok "默认 seccomp profile 已生效（Seccomp=2；ADR-010 实测其阻断 io_uring → EPERM）" ;;
  *) hard_bad "Seccomp!=2：${H8_SECCOMP}" ;;
esac
if H8_E2E="$(hard_e2e "${HARD_PORT}" "${HARD_PAYLOAD_SMALL}" "${HARD_SMALL_SHA}")"; then
  hard_ok "io_uring 不可用的部署里 OSDU 端点行为不变（${H8_E2E}）"
else
  hard_bad "端点行为异常：${H8_E2E:-无输出}"; H8_E2E="${H8_E2E:-失败}"
fi
H8_METRICS="$(curl -s "http://127.0.0.1:${HARD_PORT}/metrics" | grep -m1 '^fss_io_engine' || true)"
printf '   /metrics: %s\n' "${H8_METRICS:-（没有 fss_io_engine）}"
case "${H8_METRICS}" in
  *'fss_io_engine{engine="blocking",requested="blocking"} 1'*)
    hard_ok "指标确实暴露生效引擎（fss_io_engine{engine=\"blocking\"}=1）" ;;
  *) hard_bad "fss_io_engine 不符合实测形状：${H8_METRICS}" ;;
esac
H8_INFO="$(curl -s "http://127.0.0.1:${HARD_PORT}/api/file/v2/info")"
printf '   /v2/info: %s\n' "${H8_INFO}"
case "${H8_INFO}" in
  *'"version"'*) hard_ok "正控：/v2/info 含 version 字段（下面的『没有 ioEngine』不是空集恒真）" ;;
  *) hard_bad "/v2/info 缺 version（正控失败，否定判据可能恒真）" ;;
esac
case "${H8_INFO}" in
  *ioEngine*|*ioUringAvailable*) hard_bad "/v2/info 出现了 ioEngine/ioUringAvailable（与源码检索不符）" ;;
  *'"version"'*) hard_note "/v2/info **未**暴露 ioEngine/ioUringAvailable → C9.30 的『暴露』部分 = **未交付**（不是本次 Docker 硬化能补的；属契约面改动：会动 gRPC GetInfo 等价性，留给后续切片）" ;;
esac
record_scenario "H8 默认 seccomp + 暴露面" "${H8_CODE}" 10001 "${H8_E2E:-失败}" "-" \
  "端点行为不变 ✅；指标暴露 fss_io_engine ✅；/v2/info 字段=未交付"
end_scenario

# -----------------------------------------------------------------------------
#  汇总
# -----------------------------------------------------------------------------
log "C9.8 验证汇总"
cat <<SUMMARY
镜像                : ${IMAGE_TAG}
镜像大小            : ${IMAGE_SIZE_HUMAN} (${IMAGE_BYTES} bytes)
构建耗时            : ${BUILD_SECONDS}s
宿主端口            : ${HOST_PORT} → 容器 8080
readiness_check     : HTTP ${READY_CODE}
/v2/info authMode   : ${AUTH_MODE}
未授权 uploadURL    : HTTP ${UPLOAD_CODE}
伪造 token uploadURL: HTTP ${BOGUS_CODE}
容器用户            : ${RUN_USER} (uid=${RUN_UID})
HEALTHCHECK         : ${HEALTH_STATUS}
拒绝 auth 降级      : exit ${REFUSE_RC}
拒绝空密钥          : exit ${EMPTY_RC}
SUMMARY
echo
echo "C9.8 ✅ 全部断言通过（readiness / 强制 jwt / 非 root / HEALTHCHECK / 拒绝降级）"

# -----------------------------------------------------------------------------
#  容器硬化场景汇总表 + 真实发现清单
# -----------------------------------------------------------------------------
log "容器硬化实测汇总（证据：docs/test-evidence/phase9-image.md §10）"
printf '%-30s | %-9s | %-6s | %-22s | %-11s | %s\n' \
  "形态" "readiness" "uid" "上传读回" "进程峰值RSS" "结论"
printf -- '-------------------------------+-----------+--------+------------------------+-------------+--------\n'
for row in ${HARD_SUMMARY[@]+"${HARD_SUMMARY[@]}"}; do
  IFS='|' read -r f1 f2 f3 f4 f5 f6 <<<"${row}"
  printf '%-30s | %-9s | %-6s | %-22s | %-11s | %s\n' \
    "${f1}" "${f2}" "${f3}" "${f4}" "${f5}" "${f6}"
done
echo
if [ "${#HARD_SOFT[@]}" -gt 0 ]; then
  printf '[真实发现] %d 条（不是断言失败；已按 R4/§9 如实登记）：\n' "${#HARD_SOFT[@]}"
  for n in ${HARD_SOFT[@]+"${HARD_SOFT[@]}"}; do printf '  · %s\n' "${n}"; done
fi
if [ "${HARD_FULL}" = "1" ]; then
  echo "已跑 H4b（1 GiB 流式）；本机实测命令见上。"
else
  echo "未跑 H4b（1 GiB 流式 / pids 下界）：设 FSS_VERIFY_IMAGE_FULL=1 启用。"
fi
[ "${HARD_FAILURES}" -eq 0 ] || fail "容器硬化场景有 ${HARD_FAILURES} 条断言失败"
echo
echo "C9.8 + 容器硬化 ✅ 全部断言通过"
