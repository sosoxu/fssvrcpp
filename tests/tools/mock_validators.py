#!/usr/bin/env python3
"""mock 远端 legal / schema 校验器 —— 用于证明**远端校验失败时既不建记录也不放行**。

为什么需要它（阶段 10 切片 6a）
    远端 legal/schema 校验有两个必须被机械验证的性质：
      ① 远端说"通过"→ 记录被建出来；远端说"不通过"→ 400 且**带上远端的 message**；
      ② **依赖不可用**（超时、5xx、坏 JSON、缺 `valid`、连不上）时**绝不**降级为
         "通过"或"不通过" —— 一律 503（fail-closed）。
    ②无法用"真实 Legal/Schema 服务"验证（我们无法让它按需超时），只能用一个**可控的**
    mock 把每一种失败形态注入出来。

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
    --mode legal|schema  请求体形状（同时决定默认的响应语义，二者一致）
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
    --observe-file PATH  每次请求后把 {"requests": N, "last_body": ..., "last_headers": ...}
                         写进该文件（供测试断言"客户端发了什么"与"有没有发请求"）

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
        self.delay_ms = args.delay_ms
        self.force_status = args.status
        self.malformed = args.malformed
        self.no_valid_field = args.no_valid_field
        self.fail_file = args.fail_file
        self.observe_file = args.observe_file
        #  观测：请求计数 + 最后一次请求（供测试断言"客户端发了什么 / 有没有发"）
        self.requests = 0
        self.last_body: object = None
        self.last_headers: dict[str, str] = {}
        self.last_method = ""

    def observe(self, method: str, headers: dict[str, str], body: object) -> None:
        self.requests += 1
        self.last_method = method
        self.last_headers = headers
        self.last_body = body
        if not self.observe_file:
            return
        #  ★ 原子落盘：测试侧轮询这个文件，绝不能让读到半个 JSON
        temp = self.observe_file + ".tmp"
        payload = {
            "requests": self.requests,
            "last_method": self.last_method,
            "last_headers": self.last_headers,
            "last_body": self.last_body,
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

    def do_POST(self):  # noqa: N802
        config: Config = self.server.config  # type: ignore[attr-defined]
        if config.delay_ms > 0:
            time.sleep(config.delay_ms / 1000.0)
        #  ★ 故障控制文件：存在 = 依赖不可用；删除 = 恢复
        if config.fail_file and os.path.exists(config.fail_file):
            config.observe("POST", {}, None)
            self._reply(500, b'{"valid":true,"note":"injected failure"}')
            return
        if config.force_status != 0:
            config.observe("POST", {}, None)
            #  ★ 关键：失败状态码配一个**伪装成通过**的体（`{"valid":true}`）。
            #    早前这里是 `{"error":"injected"}` → "非 200 必须 fail-closed"与
            #    "缺 valid 必须 fail-closed"两条防线**同时**触发，判据无法区分
            #    "状态码真的被检查了"与"只是恰好缺 valid"（父代理注入自证时抓到：
            #    把状态码检查改成 `if (false)`，用例照样全绿）。现在体是"谎报通过"，
            #    只有真的检查了状态码才可能 503。
            self._reply(config.force_status, b'{"valid":true,"note":"injected status"}')
            return
        if config.malformed:
            config.observe("POST", {}, None)
            self._reply(200, b"this is not json")
            return

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
            config.observe("POST", dict(self.headers), None)
            self._reply(400, b'{"error":"bad json"}')
            return

        config.observe("POST", dict(self.headers), payload)

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

    def do_GET(self):  # noqa: N802
        #  观测端点：测试只读文件，不读 HTTP（避免"测试自己产生请求"污染计数）。
        self._reply(405, b'{"error":"use POST"}')


def main() -> int:
    parser = argparse.ArgumentParser(description="mock 远端 legal/schema 校验器（ADR-013）")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--mode", choices=["legal", "schema"], default="legal")
    parser.add_argument("--valid", action="store_true", help='返回 200 + {"valid":true}')
    parser.add_argument("--invalid-message", default="", help='返回 200 + {"valid":false,"message":M}')
    parser.add_argument("--delay-ms", type=int, default=0)
    parser.add_argument("--status", type=int, default=0, help="非 0 时一律返回该状态码")
    parser.add_argument("--malformed", action="store_true")
    parser.add_argument("--no-valid-field", action="store_true")
    parser.add_argument("--fail-file", default="", help="该文件存在时一律 500（可删除以恢复）")
    parser.add_argument("--observe-file", default="", help="每次请求后把观测写入该文件")
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
