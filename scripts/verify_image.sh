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
#  ★ 环境约定：不使用 /tmp —— 本环境 /tmp 不跨命令共享；所有临时数据放仓库 build/ 下。
#
#  用法：
#    scripts/verify_image.sh                 # 默认 tag fssvrcpp:verify
#    FSS_IMAGE_TAG=my/tag scripts/verify_image.sh
#    FSS_VERIFY_SKIP_BUILD=1 scripts/verify_image.sh   # 复用已构建镜像
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

log()  { printf '\n== %s\n' "$*"; }
fail() { printf '\n[FAIL] %s\n' "$*" >&2; exit 1; }

cleanup() {
  if [ -n "${CONTAINER_ID}" ]; then
    docker stop "${CONTAINER_ID}" >/dev/null 2>&1 || true
    docker rm   "${CONTAINER_ID}" >/dev/null 2>&1 || true
  fi
  docker rm -f "${CONTAINER_NAME}-refuse" >/dev/null 2>&1 || true
}
trap cleanup EXIT

command -v docker >/dev/null 2>&1 || fail "需要 docker（client/server 均可用）"
docker info >/dev/null 2>&1 || fail "docker daemon 不可用"

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
rm -rf "${RUN_DIR}"
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
echo "C9.8 ✅ 全部断言通过"
