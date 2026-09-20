#!/usr/bin/env python3
"""mock 远端 legal / schema 校验器 + **事件 webhook 接收端** —— 用于证明
**远端校验失败时既不建记录也不放行**，以及**事件发布失败绝不致命**。

为什么需要它（阶段 10 切片 6a）
    远端 legal/schema 校验有两个必须被机械验证的性质：
      ① 远端说"通过"→ 记录被建出来；远端说"不通过"→ 400 且**带上远端的 message**；
      ② **依赖不可用**（超时、5xx、坏 JSON、缺 `valid`、连不上）时**绝不**降级为
         "通过"或"不通过" —— 一律 503（fail-closed）。
    ②无法用"真实 Legal/Schema 服务"验证（我们无法让它按需超时），只能用一个**可控的**
    mock 把每一种失败形态注入出来。

为什么 webhook 也用它（阶段 10 切片 6b / ADR-013 §9）
    事件发布的方向与校验**相反**（`docs/03-api-contract.md` §2.6 第 4/10 步：上游只
    `log.warning("Failed to publish ...")`）：webhook 连不上/超时/非 2xx/坏响应
    **绝不能**让 HTTP 请求失败 —— 建记录请求必须照常 201，且记录必须真的落库。
    这需要同一个可控 mock 把"发布端故障"注入出来，同时**记录每一次收到的请求体**
    （`--mode webhook` 默认回 200 `{"ok":true}`）。

为什么 Storage Service 也用它（`metadata.repository=remote` / ADR-004）
    远端 Storage Service 元数据仓储需要三种可证事实，都只有**可控 mock** 才能造：
      ① 线协议形状（`PUT {base}/records` 的体是不是 `domain::ToJson` 的记录、幂等键
         是不是落在 id 上）—— `--observe-file` 的 `calls` 让"客户端确实按约定发了"可断言；
      ② **幂等**（同一 fileSource 的第二次 createMetadata 不得再发 PUT）；
      ③ fail-closed 的每一种形态（`--fail-put 500` / `--fail-get 404` / `--timeout-ms` /
         `--require-token` 401 / `--malformed` / `--delete-status 200`）。
    ★ 故障响应体故意是"看起来成功的记录 JSON（version=999）" —— 这样"非 2xx 必须
      fail-closed"只能靠**真的检查状态码**满足（与切片 6a 的 `--status` 同一教训）。
    ⚠️ **未与真实 Storage Service 联调**：响应形状是本项目与 mock 的约定；真实服务返回
      `{recordCount,recordIds,versions}` 信封时用 `--put-envelope` 模拟（适配器两种都收）。

⚠️ 本端点**没有上游路径依据**（这是本服务的**扩展**，与 `/v1/transfer` 同类）：
    `docs/01-osdu-research.md:110` 明确"legal tag 的合规性由 Storage Service 的
    PUT /records 内部校验"，上游 File Service **不直接调用** Legal/Schema 服务。
    线协议由 ADR-013 定案：配置里的 `*.remote.base_url` 就是**完整端点 URL**
    （POST 到该 URL，不追加任何路径）。

本文件与项目约定的线协议（ADR-013 / `docs/03-api-contract.md` §7）
    legal  —— POST <base_url>
              body: {"partition": "<partition>", "legaltags": ["<tag>", ...]}
    schema —— POST <base_url>
              body: {"kind": "<kind>", "record": { ...完整记录 JSON... }}
    200 + {"valid": true}                    → 通过
    200 + {"valid": false, "message": "<原因>"} → **不通过**（调用方映射成 400）
    其余一切（非 200 / 非 JSON / 缺 valid / valid 非 bool）→ 依赖故障（调用方 fail-closed → 503）

可注入的失败形态（都是命令行开关，便于单测确定性构造）
    --mode legal|schema|webhook
                         请求体形状（同时决定默认的响应语义，三者一致地"成功"）
                         · legal/schema：按下面的开关回答 `{"valid":...}`
                         · webhook：默认回 200 `{"ok":true}`（只认 2xx = 成功）
    --valid              一律返回 200 + {"valid": true}
    --invalid-message M  一律返回 200 + {"valid": false, "message": M}
    --delay-ms N         响应前睡 N 毫秒（用于触发客户端超时）
    --status N           一律返回该状态码（如 500）；响应体是**伪装成通过**的
                         `{"valid":true,"note":"..."}` —— 这样"非 200 必须
                         fail-closed"只能靠**真的检查状态码**满足（否则会与
                         "缺 valid"那条防线重合，判据失去区分力，见 phase10 §11.4.1）
    --malformed          返回非 JSON
    --no-valid-field     返回 200 + {}（合法 JSON 但缺 `valid`）
    --fail-file PATH     该文件**存在**时一律返回 500（删掉它 = 故障恢复）
    --observe-file PATH  每次请求后把 {"requests": N, "last_body": ..., "last_headers": ...,
                         "bodies": [...]} 写进该文件。`bodies` 是**全部**请求体
                         （按到达顺序）；一次 createMetadata 会产生 2~3 个事件，
                         只看 `last_body` 无法断言"两个都发了"（切片 6b 新增）。

用法：`python3 tests/tools/mock_validators.py --port 0 --mode legal [开关...]`
      `--port 0` 时向 stdout 打印一行 `LISTENING <port>`。
"""
from __future__ import annotations

import argparse
import json
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_BODY = 8 * 1024 * 1024


class Config:
    def __init__(self, args: argparse.Namespace) -> None:
        self.mode = args.mode
        self.valid = args.valid
        self.invalid_message = args.invalid_message
        #  `--timeout-ms`（storage 模式）与 `--delay-ms`（legal/schema）都是"响应前睡多久"。
        self.delay_ms = args.delay_ms or args.timeout_ms
        self.force_status = args.status
        self.malformed = args.malformed
        self.no_valid_field = args.no_valid_field
        self.fail_file = args.fail_file
        self.observe_file = args.observe_file
        #  ---- `--mode storage`（远端 Storage Service mock）的故障开关 ----
        self.fail_put = args.fail_put
        self.fail_get = args.fail_get
        self.require_token = args.require_token
        self.put_envelope = args.put_envelope
        self.delete_status = args.delete_status
        #  观测：请求计数 + 最后一次请求（供测试断言"客户端发了什么 / 有没有发"）
        self.requests = 0
        self.last_body: object = None
        self.last_headers: dict[str, str] = {}
        self.last_method = ""
        self.last_path = ""
        #  ★ 切片 6b：**全部**请求体（按到达顺序）。一次 createMetadata 会发
        #    statusChanged(IN_PROGRESS) / statusChanged(SUCCESS) / datasetDetails
        #    三个事件 —— 只看 `last_body` 无法断言"两个 kind 都发了"。
        self.bodies: list[object] = []
        #  ★ 本切片（metadata.repository=remote）：storage 模式的逐请求调用记录与计数。
        #    `calls` 让测试能断言"PUT 了几次 / 路径是什么 / Authorization 头是什么"，
        #    而 `puts`/`gets`/`deletes` 是最常用的计数。
        self.calls: list[dict[str, object]] = []
        self.puts = 0
        self.gets = 0
        self.deletes = 0
        self.records: dict[str, object] = {}
        self.versions: dict[str, int] = {}

    def observe(self, method: str, headers: dict[str, str], body: object,
                path: str = "") -> None:
        self.requests += 1
        self.last_method = method
        self.last_path = path
        self.last_headers = headers
        self.last_body = body
        if method == "PUT":
            self.puts += 1
        elif method == "GET":
            self.gets += 1
        elif method == "POST" and path.endswith(":delete"):
            self.deletes += 1
        self.calls.append({"method": method, "path": path, "headers": headers, "body": body})
        #  body 为 None 表示"这次请求的体没读到"（例如故障短路分支），不进 bodies
        if body is not None:
            self.bodies.append(body)
        if not self.observe_file:
            return
        #  ★ 原子落盘：测试侧轮询这个文件，绝不能让读到半个 JSON
        temp = self.observe_file + ".tmp"
        payload = {
            "requests": self.requests,
            "last_method": self.last_method,
            "last_path": self.last_path,
            "last_headers": self.last_headers,
            "last_body": self.last_body,
            "bodies": self.bodies,
            #  storage 模式专用
            "puts": self.puts,
            "gets": self.gets,
            "deletes": self.deletes,
            "calls": self.calls,
            "records": self.records,
        }
        with open(temp, "w", encoding="utf-8") as handle:
            json.dump(payload, handle)
        os.replace(temp, self.observe_file)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args) -> None:  # 静默（测试输出保持干净）
        return

    def _reply(self, status: int, body: bytes, content_type: str = "application/json") -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json_body(self):
        """读请求体并按 JSON 解析；返回 (raw, payload 或 None)。"""
        length = int(self.headers.get("Content-Length", "0") or "0")
        raw = self.rfile.read(length) if length > 0 else b"{}"
        if len(raw) > MAX_BODY:
            return raw, "TOO_LARGE"
        try:
            return raw, json.loads(raw.decode("utf-8") or "{}")
        except (UnicodeDecodeError, json.JSONDecodeError):
            return raw, None

    # -------------------------------------------------------------------------
    #  `--mode storage`：远端 Storage Service 的**契约 mock**
    # -------------------------------------------------------------------------
    #  路由基于路径**后缀**（`base_url` 前缀由调用方决定，例如 /api/storage/v2）：
    #    PUT  .../records              → upsert；按 id 递增版本；返回记录 JSON
    #    GET  .../records/{id}         → 200 记录 / 404
    #    POST .../records/{id}:delete  → 204（`--delete-status` 可改成 200 等）
    #  ★ 故障响应体故意是"看起来成功的记录 JSON（version=999）"：这样"非 2xx 必须
    #    fail-closed"只能靠**真的检查状态码**满足；若返回 `{"error":...}`，坏实现会
    #    因为"解析不出记录"而失败 —— 两条防线重合，判据失去区分力（与切片 6a 同一教训）。
    def _storage(self, method: str) -> None:
        config: Config = self.server.config  # type: ignore[attr-defined]
        raw, payload = self._read_json_body()
        if payload == "TOO_LARGE":
            self._reply(413, b'{"error":"too large"}')
            return
        path = self.path.split("?", 1)[0]
        config.observe(method, dict(self.headers), payload, path)

        #  鉴权：`--require-token T` → Authorization 必须是 `Bearer T`，否则 401
        if config.require_token:
            if self.headers.get("Authorization", "") != "Bearer " + config.require_token:
                self._reply(401, b'{"error":"unauthorized: bad or missing bearer token"}')
                return

        #  `--malformed`：storage 模式下一律回非 JSON（适配器必须 fail-closed → kUnavailable）
        if config.malformed:
            self._reply(200, b"this is not json")
            return

        lying_record = dict(payload) if isinstance(payload, dict) else {}
        lying_record.setdefault("id", "unknown")
        lying_record["version"] = 999

        if method == "PUT" and path.endswith("/records"):
            if config.fail_put:
                self._reply(config.fail_put, json.dumps(lying_record).encode("utf-8"))
                return
            record = dict(payload) if isinstance(payload, dict) else {}
            rid = record.get("id") or "unknown"
            version = config.versions.get(rid, 0) + 1
            config.versions[rid] = version
            record["version"] = version
            config.records[rid] = record
            if config.put_envelope:
                #  OSDU Storage 的 upsert 信封形状（**未与真实服务联调**；适配器两种都收）
                envelope = {"recordCount": 1, "recordIds": [rid], "versions": [str(version)]}
                self._reply(200, json.dumps(envelope).encode("utf-8"))
            else:
                self._reply(200, json.dumps(record).encode("utf-8"))
            return

        if method == "GET" and "/records/" in path:
            rid = path.rsplit("/records/", 1)[1]
            if config.fail_get:
                self._reply(config.fail_get, json.dumps(lying_record).encode("utf-8"))
                return
            record = config.records.get(rid)
            if record is None:
                self._reply(404, b'{"error":"record not found"}')
                return
            self._reply(200, json.dumps(record).encode("utf-8"))
            return

        if method == "POST" and path.endswith(":delete"):
            rid = path.rsplit("/records/", 1)[1][: -len(":delete")]
            if config.delete_status != 204:
                self._reply(config.delete_status, json.dumps({"deleted": rid}).encode("utf-8"))
                return
            config.records.pop(rid, None)
            self._reply(204, b"")
            return

        self._reply(404, b'{"error":"unknown storage route"}')

    def do_PUT(self):  # noqa: N802
        config: Config = self.server.config  # type: ignore[attr-defined]
        if config.delay_ms > 0:
            time.sleep(config.delay_ms / 1000.0)
        if config.mode != "storage":
            self._reply(405, b'{"error":"use POST"}')
            return
        self._storage("PUT")

    def do_POST(self):  # noqa: N802
        config: Config = self.server.config  # type: ignore[attr-defined]
        if config.delay_ms > 0:
            time.sleep(config.delay_ms / 1000.0)

        #  ★ 先把请求体读进来并**观测**，再做故障短路：切片 6b 需要断言
        #    "失败形态下 mock 也真的收到了事件体"（例如 `--status 500`）。
        length = int(self.headers.get("Content-Length", "0") or "0")
        raw = self.rfile.read(length) if length > 0 else b"{}"
        if len(raw) > MAX_BODY:
            self._reply(413, b'{"error":"too large"}')
            return
        try:
            payload = json.loads(raw.decode("utf-8") or "{}")
        except (UnicodeDecodeError, json.JSONDecodeError):
            #  ★ 请求体我们自己读不懂 = 客户端 bug；仍然按"依赖故障"记一次观测，
            #    测试不会依赖这条分支（它只证明我们确实收到了字节）。
            config.observe("POST", dict(self.headers), None, self.path)
            self._reply(400, b'{"error":"bad json"}')
            return

        #  ★ storage 模式由 `_storage` 自己观测与路由（它要区分 :delete 与 /records）。
        if config.mode == "storage":
            config.observe("POST", dict(self.headers), payload, self.path.split("?", 1)[0])
            if config.require_token:
                if self.headers.get("Authorization", "") != "Bearer " + config.require_token:
                    self._reply(401, b'{"error":"unauthorized: bad or missing bearer token"}')
                    return
            self._storage_post(payload)
            return

        config.observe("POST", dict(self.headers), payload, self.path)

        #  ★ 故障控制文件：存在 = 依赖不可用；删除 = 恢复
        if config.fail_file and os.path.exists(config.fail_file):
            self._reply(500, b'{"valid":true,"note":"injected failure"}')
            return
        if config.force_status != 0:
            #  ★ 关键：失败状态码配一个**伪装成通过**的体（`{"valid":true}`）。
            #    早前这里是 `{"error":"injected"}` → "非 200 必须 fail-closed"与
            #    "缺 valid 必须 fail-closed"两条防线**同时**触发，判据无法区分
            #    "状态码真的被检查了"与"只是恰好缺 valid"（父代理注入自证时抓到：
            #    把状态码检查改成 `if (false)`，用例照样全绿）。现在体是"谎报通过"，
            #    只有真的检查了状态码才可能 503。
            self._reply(config.force_status, b'{"valid":true,"note":"injected status"}')
            return
        if config.malformed:
            self._reply(200, b"this is not json")
            return

        #  ★ 切片 6b：webhook 接收端 —— 只认 2xx = 成功，体内容无关紧要
        if config.mode == "webhook":
            self._reply(200, b'{"ok":true}')
            return

        if config.no_valid_field:
            self._reply(200, b"{}")
            return
        if config.valid:
            self._reply(200, b'{"valid":true}')
            return
        if config.invalid_message:
            body = json.dumps({"valid": False, "message": config.invalid_message}).encode("utf-8")
            self._reply(200, body)
            return
        #  默认（未指定 --valid / --invalid-message）：按"不通过"回答，且带上形状断言用的 message
        self._reply(200, b'{"valid":false,"message":"mock default: invalid"}')

    def _storage_post(self, payload) -> None:
        """storage 模式下已观测过的 POST：只可能是 `.../records/{id}:delete`。"""
        config: Config = self.server.config  # type: ignore[attr-defined]
        path = self.path.split("?", 1)[0]
        if path.endswith(":delete"):
            rid = path.rsplit("/records/", 1)[1][: -len(":delete")]
            if config.delete_status != 204:
                self._reply(config.delete_status, json.dumps({"deleted": rid}).encode("utf-8"))
                return
            config.records.pop(rid, None)
            self._reply(204, b"")
            return
        self._reply(404, b'{"error":"unknown storage route"}')

    def do_GET(self):  # noqa: N802
        config: Config = self.server.config  # type: ignore[attr-defined]
        if config.mode == "storage":
            if config.delay_ms > 0:
                time.sleep(config.delay_ms / 1000.0)
            self._storage("GET")
            return
        #  观测端点：测试只读文件，不读 HTTP（避免"测试自己产生请求"污染计数）。
        self._reply(405, b'{"error":"use POST"}')


def main() -> int:
    parser = argparse.ArgumentParser(description="mock 远端 legal/schema 校验器 + webhook + Storage Service（ADR-013/ADR-004）")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--mode", choices=["legal", "schema", "webhook", "storage"], default="legal")
    parser.add_argument("--valid", action="store_true", help='返回 200 + {"valid":true}')
    parser.add_argument("--invalid-message", default="", help='返回 200 + {"valid":false,"message":M}')
    parser.add_argument("--delay-ms", type=int, default=0)
    parser.add_argument("--status", type=int, default=0, help="非 0 时一律返回该状态码")
    parser.add_argument("--malformed", action="store_true")
    parser.add_argument("--no-valid-field", action="store_true")
    parser.add_argument("--fail-file", default="", help="该文件存在时一律 500（可删除以恢复）")
    parser.add_argument("--observe-file", default="", help="每次请求后把观测写入该文件")
    #  ---- `--mode storage`（远端 Storage Service mock，ADR-004）----
    parser.add_argument("--timeout-ms", type=int, default=0,
                        help="storage 模式：响应前睡 N 毫秒（用于触发客户端超时）")
    parser.add_argument("--fail-put", type=int, default=0,
                        help="storage 模式：PUT /records 一律返回该状态码（体伪装成成功记录）")
    parser.add_argument("--fail-get", type=int, default=0,
                        help="storage 模式：GET /records/{id} 一律返回该状态码（体伪装成成功记录）")
    parser.add_argument("--require-token", default="",
                        help="storage 模式：要求 Authorization: Bearer <t>，否则 401")
    parser.add_argument("--put-envelope", action="store_true",
                        help="storage 模式：PUT 返回 OSDU 的 {recordCount,recordIds,versions} 信封")
    parser.add_argument("--delete-status", type=int, default=204,
                        help="storage 模式：POST /records/{id}:delete 返回的状态码（默认 204）")
    args = parser.parse_args()

    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    httpd.config = Config(args)  # type: ignore[attr-defined]
    port = httpd.server_address[1]
    print(f"LISTENING {port}", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
