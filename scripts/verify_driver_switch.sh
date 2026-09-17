#!/usr/bin/env bash
# =============================================================================
#  scripts/verify_driver_switch.sh —— C5.9：切换驱动**只改配置**
# =============================================================================
#  判据原文：同一二进制、同一测试代码，`storage.driver: posix|s3` 两值各跑一遍全绿。
#
#  做法：
#    ① 记录**同一个** `fss_server` 二进制的 sha256（证明两次跑的是同一份代码）；
#    ② 用**同一段**端到端脚本（`run_e2e`，只在文件里写一次）分别驱动两种模式：
#         · POSIX：真实目录 + 自签数据面（`SignedURL` 指向**本服务**的 /v1/transfer）
#         · S3   ：mock-S3 + 原生预签名（`SignedURL` 指向**存储端点**）
#    ③ 额外断言两种模式的 `SignedURL` **确实不同**（否则"只改配置"这句话没有意义：
#       两个模式若都走自签数据面，就说明驱动根本没换）。
#
#  用法：scripts/verify_driver_switch.sh [构建目录]
#  退出码：0 = 两种模式都通过；1 = 任一模式失败
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build}"
SERVER_BIN="${BUILD_DIR}/bin/fss_server"

C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
ok()   { printf '%s\n' "${C_GRN}  ✓${C_OFF} $*"; }
bad()  { printf '%s\n' "${C_RED}  ✗${C_OFF} $*"; }
info() { printf '%s\n' "${C_DIM}  ·${C_OFF} $*"; }

[[ -x "${SERVER_BIN}" ]] || { bad "找不到 ${SERVER_BIN}，请先构建"; exit 1; }
command -v curl >/dev/null || { bad "缺少 curl"; exit 1; }
command -v python3 >/dev/null || { bad "缺少 python3"; exit 1; }

WORK="$(mktemp -d)"
SERVER_PID=""; MOCK_PID=""
cleanup() {
  [[ -n "${SERVER_PID}" ]] && { kill "${SERVER_PID}" >/dev/null 2>&1 || true; wait "${SERVER_PID}" 2>/dev/null || true; }
  [[ -n "${MOCK_PID}" ]] && kill "${MOCK_PID}" >/dev/null 2>&1 || true
  rm -rf "${WORK}"
}
trap cleanup EXIT

#  同一个二进制的指纹：两次跑必须一致（判据要求"同一二进制"）
BIN_SHA="$(sha256sum "${SERVER_BIN}" | cut -d' ' -f1)"
echo "驱动切换验证（C5.9）    server_bin sha256=${BIN_SHA:0:16}…"

# -----------------------------------------------------------------------------
#  起服务（两种模式共用一个函数，只改环境变量）
# -----------------------------------------------------------------------------
start_server() {  # $1=driver $2=port
  local driver="$1" port="$2"
  mkdir -p "${WORK}/${driver}"   # 日志目录必须先存在，否则重定向会失败（一句 set -e 就让脚本静默退出）
  if [[ "${driver}" == "s3" ]]; then
    #  mock-S3（Python 独立验签）；从 stdout 的 LISTENING 行拿端口
    python3 "${REPO_ROOT}/tests/tools/mock_s3.py" --port 0 \
      --access-key AKIDEXAMPLE --secret-key 'wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY' \
      >"${WORK}/mock.out" 2>/dev/null &
    MOCK_PID=$!
    local mock_port=""
    for _ in $(seq 1 100); do
      mock_port="$(awk '/LISTENING/{print $2}' "${WORK}/mock.out" 2>/dev/null || true)"
      [[ -n "${mock_port}" ]] && break
      sleep 0.1
    done
    [[ -n "${mock_port}" ]] || { bad "mock-S3 未就绪"; exit 1; }
    export FSS_STORAGE_S3_ENDPOINT="127.0.0.1:${mock_port}"
    export FSS_STORAGE_S3_REGION="us-east-1"
    export FSS_STORAGE_S3_ACCESS_KEY="AKIDEXAMPLE"
    export FSS_STORAGE_S3_SECRET_KEY="wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"
    export FSS_STORAGE_S3_FORCE_PATH_STYLE="true"
    export FSS_STORAGE_S3_VERIFY_TLS="false"
    STORAGE_ENDPOINT="127.0.0.1:${mock_port}"
  else
    export FSS_STORAGE_ROOT="${WORK}/posix-data"
    STORAGE_ENDPOINT="127.0.0.1:${port}"
  fi

  FSS_STORAGE_DRIVER="${driver}" \
  FSS_HTTP_PORT="${port}" \
  FSS_SQLITE_PATH="${WORK}/${driver}/location.db" \
  FSS_BIND_ADDRESS="127.0.0.1" \
  FSS_TRANSFER_SECRET="driver-switch-secret" \
  FSS_SELF_BASE_URL="http://127.0.0.1:${port}/api/file" \
    "${SERVER_BIN}" >"${WORK}/${driver}/server.log" 2>&1 &
  SERVER_PID=$!

  for _ in $(seq 1 100); do
    curl -fsS "http://127.0.0.1:${port}/api/file/v2/liveness_check" >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  bad "服务未就绪（${driver}）："; tail -20 "${WORK}/${driver}/server.log"; exit 1
}

# -----------------------------------------------------------------------------
#  同一段端到端脚本：uploadURL → PUT（打 SignedURL）→ metadata → downloadURL → GET → DELETE
# -----------------------------------------------------------------------------
run_e2e() {  # $1=driver $2=port   → 输出 "signed_url_host|sha256"
  local driver="$1" port="$2"
  local hdr=(-H 'authorization: Bearer driver-switch' -H 'data-partition-id: opendes')
  local base="http://127.0.0.1:${port}/api/file/v2"

  local payload="${WORK}/${driver}/payload.bin"
  head -c 65536 /dev/urandom >"${payload}"
  local src_sha; src_sha="$(sha256sum "${payload}" | cut -d' ' -f1)"

  #  ① uploadURL
  local upload; upload="$(curl -fsS "${hdr[@]}" "${base}/files/uploadURL")"
  local put_url file_id file_source
  read -r put_url file_source file_id < <(printf '%s' "${upload}" | python3 -c '
import json,sys
d=json.load(sys.stdin)
print(d["Location"]["SignedURL"], d["Location"]["FileSource"], d["FileID"])
')
  [[ -n "${put_url}" ]] || { bad "uploadURL 未返回 SignedURL"; exit 1; }
  local put_host; put_host="$(printf '%s' "${put_url}" | python3 -c 'import sys,urllib.parse;print(urllib.parse.urlsplit(sys.stdin.read()).netloc)')"

  #  ② 上传（**打到 SignedURL 上**：POSIX 模式下是自签代理，S3 模式下是存储端点）
  local put_code
  put_code="$(curl -sS -o /dev/null -w '%{http_code}' -X PUT --data-binary @"${payload}" "${hdr[@]}" "${put_url}")"
  [[ "${put_code}" == "200" ]] || { bad "${driver}: PUT ${put_code}"; exit 1; }

  #  ③ metadata
  local record; record="$(python3 - "${file_source}" <<'PY'
import json,sys
print(json.dumps({
  "kind": "opendes:wks:dataset--File.Generic:1.0.0",
  "acl": {"viewers": ["data.default.viewers@opendes.example.com"],
          "owners": ["data.default.owners@opendes.example.com"]},
  "legal": {"legaltags": ["opendes-public-1"], "otherRelevantDataCountries": ["US"],
            "status": "compliant"},
  "data": {"Name": "driver-switch.bin", "Endian": "LITTLE",
           "DatasetProperties": {"FileSourceInfo": {"FileSource": sys.argv[1]}}},
}))
PY
)"
  local created; created="$(curl -fsS "${hdr[@]}" -H 'content-type: application/json' \
      -X POST --data-binary "${record}" "${base}/files/metadata")"
  local record_id; record_id="$(printf '%s' "${created}" | python3 -c 'import json,sys;print(json.load(sys.stdin)["id"])')"

  #  ④ downloadURL → 下载 → SHA-256 比对
  local download; download="$(curl -fsS "${hdr[@]}" "${base}/files/${file_id}/downloadURL")"
  local get_url; get_url="$(printf '%s' "${download}" | python3 -c 'import json,sys;print(json.load(sys.stdin)["SignedUrl"])')"
  local got_file="${WORK}/${driver}/downloaded.bin"
  curl -fsS "${hdr[@]}" -o "${got_file}" "${get_url}"
  local dst_sha; dst_sha="$(sha256sum "${got_file}" | cut -d' ' -f1)"
  [[ "${src_sha}" == "${dst_sha}" ]] || { bad "${driver}: SHA-256 不一致"; exit 1; }

  #  ⑤ 删除 → 204；再读 → 404
  local del_code
  del_code="$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "${hdr[@]}" "${base}/files/${record_id}/metadata")"
  [[ "${del_code}" == "204" ]] || { bad "${driver}: DELETE ${del_code}"; exit 1; }
  local gone_code
  gone_code="$(curl -sS -o /dev/null -w '%{http_code}' "${hdr[@]}" "${base}/files/${record_id}/metadata")"
  [[ "${gone_code}" == "404" ]] || { bad "${driver}: 删除后再读应为 404，实际 ${gone_code}"; exit 1; }

  printf '%s|%s' "${put_host}" "${src_sha}"
}

# -----------------------------------------------------------------------------
#  ① POSIX 模式
# -----------------------------------------------------------------------------
info "① POSIX 模式（自签数据面）"
start_server posix 18401
POSIX_RESULT="$(run_e2e posix 18401)"
kill "${SERVER_PID}" >/dev/null 2>&1 || true; wait "${SERVER_PID}" 2>/dev/null || true; SERVER_PID=""
ok "POSIX 端到端通过（SignedURL host = ${POSIX_RESULT%%|*}）"

# -----------------------------------------------------------------------------
#  ② S3 模式（同一二进制、同一脚本）
# -----------------------------------------------------------------------------
info "② S3 模式（原生预签名 → 客户端直连存储）"
start_server s3 18402
S3_RESULT="$(run_e2e s3 18402)"
kill "${SERVER_PID}" >/dev/null 2>&1 || true; wait "${SERVER_PID}" 2>/dev/null || true; SERVER_PID=""
ok "S3 端到端通过（SignedURL host = ${S3_RESULT%%|*}）"

# -----------------------------------------------------------------------------
#  ③ 两种模式的"地址形态"必须**真的不同**（否则等于没换驱动）
# -----------------------------------------------------------------------------
POSIX_HOST="${POSIX_RESULT%%|*}"; S3_HOST="${S3_RESULT%%|*}"
info "POSIX SignedURL host = ${POSIX_HOST}（应为 127.0.0.1:18401 = 本服务）"
info "S3    SignedURL host = ${S3_HOST}（应为 ${STORAGE_ENDPOINT} = 存储端点）"
[[ "${POSIX_HOST}" == "127.0.0.1:18401" ]] || { bad "POSIX 模式的 SignedURL 应指向本服务"; exit 1; }
[[ "${S3_HOST}" == "${STORAGE_ENDPOINT}" ]] || { bad "S3 模式的 SignedURL 应指向存储端点"; exit 1; }
[[ "${POSIX_HOST}" != "${S3_HOST}" ]] || { bad "两种模式的 SignedURL 相同 = 驱动没换"; exit 1; }

#  两次跑的是同一个二进制（脚本开头记的指纹没有变）
BIN_SHA_AFTER="$(sha256sum "${SERVER_BIN}" | cut -d' ' -f1)"
[[ "${BIN_SHA}" == "${BIN_SHA_AFTER}" ]] || { bad "两次运行之间二进制被替换了"; exit 1; }

echo
printf '%s\n' "${C_GRN}C5.9 通过：同一二进制（sha256=${BIN_SHA:0:16}…）+ 同一段端到端脚本，posix/s3 两种模式各跑一遍全绿；且 SignedURL 分别指向本服务与存储端点${C_OFF}"
info "结论：切换驱动只改配置（FSS_STORAGE_DRIVER + 对应的存储配置），上层代码零改动。"
