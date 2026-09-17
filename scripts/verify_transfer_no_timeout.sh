#!/usr/bin/env bash
# =============================================================================
#  scripts/verify_transfer_no_timeout.sh —— C4.11 的"≥5 分钟慢传输"实测
# =============================================================================
#  为什么单独一个脚本、而不放进日常门槛：
#    C4.11 要求"用刻意放慢的客户端把单次传输拉长到 **> 5 分钟**仍不被中断"。
#    5 分钟会让 `ctest -L phase4` 变得不可用（而日常门槛必须便宜到"每次都跑"）。
#    因此：
#      · 日常门槛：`test_transfer_timeouts` 用 **2 秒空闲超时 + 6 秒慢传输** 证明
#        "没有整体超时"这一**机制**；
#      · 本脚本：用**生产默认超时**（`transfer_idle_timeout_sec=120`）跑满 > 5 分钟，
#        证明"长时间慢传输"在真实配置下同样不被整体超时打断。输出记入阶段证据。
#
#  做法：起一个真实 `fss_server`（真实 POSIX 存储 + SQLite），用 `curl` 的
#        `--limit-rate` 把 3 MiB 压到 ≈ 8 KiB/s（≈ 6.4 分钟），全程无整体超时。
#        curl 自身不设 `--max-time`（只设 `--speed-limit/--speed-time` 用于**无进展**判定，
#        阈值远大于服务端空闲超时，因此不会先于服务端触发）。
#
#  用法：scripts/verify_transfer_no_timeout.sh [持续秒数] [构建目录]
#  退出码：0 = 传输在超时时间内完成且字节完全一致；1 = 被中断/字节不符/环境不满足
# =============================================================================
set -euo pipefail

DURATION_SEC="${1:-330}"          # 默认 > 5 分钟（C4.11 的字面要求）
BUILD_DIR="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_BIN="${BUILD_DIR}/bin/fss_server"

C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
ok()   { printf '%s\n' "${C_GRN}  ✓${C_OFF} $*"; }
bad()  { printf '%s\n' "${C_RED}  ✗${C_OFF} $*"; }
info() { printf '%s\n' "${C_DIM}  ·${C_OFF} $*"; }

for tool in curl python3; do
  command -v "${tool}" >/dev/null 2>&1 || { bad "缺少 ${tool}"; exit 1; }
done
[[ -x "${SERVER_BIN}" ]] || { bad "找不到 ${SERVER_BIN}，请先构建"; exit 1; }

WORK="$(mktemp -d)"
PORT="${FSS_SMOKE_PORT:-18131}"
SERVER_PID=""
cleanup() {
  if [[ -n "${SERVER_PID}" ]]; then kill "${SERVER_PID}" >/dev/null 2>&1 || true; wait "${SERVER_PID}" 2>/dev/null || true; fi
  rm -rf "${WORK}"
}
trap cleanup EXIT

# ---- ① 起真实服务（生产默认超时：transfer_idle=120s；没有整体超时）----
info "启动 fss_server（端口 ${PORT}，存储 ${WORK}/data）"
FSS_HTTP_PORT="${PORT}" \
FSS_STORAGE_ROOT="${WORK}/data" \
FSS_SQLITE_PATH="${WORK}/data/location.db" \
FSS_BIND_ADDRESS="127.0.0.1" \
FSS_TRANSFER_SECRET="long-run-secret" \
FSS_SELF_BASE_URL="http://127.0.0.1:${PORT}/api/file" \
  "${SERVER_BIN}" >"${WORK}/server.log" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 50); do
  if curl -fsS "http://127.0.0.1:${PORT}/api/file/v2/liveness_check" >/dev/null 2>&1; then break; fi
  sleep 0.1
done
curl -fsS "http://127.0.0.1:${PORT}/api/file/v2/liveness_check" >/dev/null \
  || { bad "服务未就绪，日志："; tail -20 "${WORK}/server.log"; exit 1; }
ok "服务就绪（PID ${SERVER_PID}）"

# ---- ② 取上传地址（自签 URL 指向 /v1/transfer）----
HDR=(-H 'authorization: Bearer long-run' -H 'data-partition-id: opendes')
UPLOAD_URL="http://127.0.0.1:${PORT}/api/file/v2/files/uploadURL"
UPLOAD_JSON="$(curl -fsS "${HDR[@]}" "${UPLOAD_URL}")"
read -r PUT_URL FILE_SOURCE FILE_ID <<<"$(printf '%s' "${UPLOAD_JSON}" | python3 -c '
import json,sys
d=json.load(sys.stdin)
print(d["Location"]["SignedURL"], d["Location"]["FileSource"], d["FileID"])
')"
[[ -n "${PUT_URL}" ]] || { bad "uploadURL 未返回 SignedURL"; exit 1; }
info "自签上传地址前缀：${PUT_URL%%\?*}"

# ---- ③ 造一份确定性载荷（大小按目标时长与限速推导）----
RATE_BYTES=$((8 * 1024))                      # curl --limit-rate 8k
PAYLOAD_BYTES=$((RATE_BYTES * DURATION_SEC))
info "载荷 ${PAYLOAD_BYTES} 字节，限速 ${RATE_BYTES} B/s → 预计 ≈ $((PAYLOAD_BYTES / RATE_BYTES)) 秒（目标 > ${DURATION_SEC} 秒的判据下限是 300）"
python3 - "$WORK/payload.bin" "$PAYLOAD_BYTES" <<'PY'
import sys
path, size = sys.argv[1], int(sys.argv[2])
chunk = bytes((i * 7) % 256 for i in range(65536))
with open(path, "wb") as fh:
    written = 0
    while written < size:
        take = min(len(chunk), size - written)
        fh.write(chunk[:take])
        written += take
PY
SRC_SHA="$(sha256sum "${WORK}/payload.bin" | cut -d' ' -f1)"
info "上行 SHA-256 ${SRC_SHA}"

# ---- ④ 以限速慢传（不设 --max-time；`--speed-limit` 只用于"完全无进展"）----
info "开始慢速上传（这一步会持续数分钟）"
STARTED=$(date +%s)
if ! curl -sS -o "${WORK}/put.out" -w '%{http_code}' \
      -X PUT --limit-rate "${RATE_BYTES}" \
      --speed-limit 1024 --speed-time 240 \
      -H 'authorization: Bearer long-run' -H 'data-partition-id: opendes' \
      --data-binary "@${WORK}/payload.bin" "${PUT_URL}" >"${WORK}/put.code" 2>"${WORK}/put.err"; then
  bad "PUT 失败（被中断？）：$(cat "${WORK}/put.err")"
  ELAPSED=$(( $(date +%s) - STARTED ))
  info "已耗时 ${ELAPSED}s"
  exit 1
fi
ELAPSED=$(( $(date +%s) - STARTED ))
PUT_CODE="$(cat "${WORK}/put.code")"
info "PUT 完成：status=${PUT_CODE}，耗时 ${ELAPSED}s"
[[ "${PUT_CODE}" == "200" ]] || { bad "PUT 状态码 ${PUT_CODE}（期望 200）"; exit 1; }
[[ "${ELAPSED}" -gt 300 ]] || { bad "本次只跑了 ${ELAPSED}s，未达到 C4.11 的 '> 5 分钟' 要求 → **无结论**"; exit 1; }
ok "单次传输持续 ${ELAPSED}s（> 5 分钟）未被整体超时打断"

# ---- ⑤ 登记元数据 → 下载 → 字节比对（证明不是"传了个空壳"）----

RECORD="$(python3 - "$FILE_SOURCE" <<'PY'
import json,sys
print(json.dumps({
  "kind": "opendes:wks:dataset--File.Generic:1.0.0",
  "acl": {"viewers": ["data.default.viewers@opendes.example.com"],
          "owners": ["data.default.owners@opendes.example.com"]},
  "legal": {"legaltags": ["opendes-public-1"], "otherRelevantDataCountries": ["US"],
            "status": "compliant"},
  "data": {"Name": "long-run.bin", "Endian": "LITTLE",
           "DatasetProperties": {"FileSourceInfo": {"FileSource": sys.argv[1]}}},
}))
PY
)"
CREATED="$(curl -fsS "${HDR[@]}" -H 'content-type: application/json' \
  -X POST --data-binary "${RECORD}" \
  "http://127.0.0.1:${PORT}/api/file/v2/files/metadata")"
RECORD_ID="$(printf '%s' "${CREATED}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')"
ok "元数据已登记：${RECORD_ID}"

DOWNLOAD_JSON="$(curl -fsS "${HDR[@]}" \
  "http://127.0.0.1:${PORT}/api/file/v2/files/${FILE_ID}/downloadURL")"
GET_URL="$(printf '%s' "${DOWNLOAD_JSON}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["SignedUrl"])')"
curl -fsS -H 'authorization: Bearer long-run' -H 'data-partition-id: opendes' \
  -o "${WORK}/downloaded.bin" "${GET_URL}"
DST_SHA="$(sha256sum "${WORK}/downloaded.bin" | cut -d' ' -f1)"
[[ "${SRC_SHA}" == "${DST_SHA}" ]] || { bad "SHA-256 不一致：${SRC_SHA} vs ${DST_SHA}"; exit 1; }
ok "下载后 SHA-256 一致（${DST_SHA}）"

# ---- ⑥ 空闲超时仍然生效（同一进程内、非数据面的对照）----
if grep -q '"msg":"auth.mode=disabled' "${WORK}/server.log"; then
  info "（服务端 auth 告警已在日志中可见）"
fi

echo
printf '%s\n' "${C_GRN}C4.11 长跑实测通过：${ELAPSED}s 的慢传输未被整体超时打断，且字节一致${C_OFF}"
info "结论：数据面确实**没有整体超时**；空闲超时（transfer_idle_timeout_sec=120）由 test_transfer_timeouts 覆盖。"
