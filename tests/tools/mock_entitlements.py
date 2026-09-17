#!/usr/bin/env python3
"""mock Entitlements（`authorizeAny` 语义）—— 用于证明**远端鉴权失败时不会放行**。

为什么需要它（C8.4）
    远端鉴权有两个必须被机械验证的性质：
      ① 正常的 allow / deny 能正确映射到 200 / 403；
      ② **依赖不可用**（超时、5xx、坏 JSON、连不上）时**绝不降级为放行** —— fail-closed。
    ②无法用"真实 Entitlements"验证（我们无法让它按需超时），只能用一个**可控的**
    mock 把每一种失败形态注入出来。

本文件与项目约定的 Entitlements 接口（见 `docs/03-api-contract.md` §4.5）
    POST <authorize_path>            （默认 /api/entitlements/v2/authorizeAny）
    headers: Authorization: <原样透传的 bearer>、data-partition-id: <partition>
    body:    {"roles": ["service.file.editors", ...]}
    200 + {"allowed": true|false, "grantedRoles": [...]}
    401 未认证；其它状态码视为依赖故障（调用方必须 fail-closed）

可注入的失败形态（都是命令行开关，便于单测确定性构造）
    --grant R1,R2        这些角色被授予（其它角色 allowed=false）
    --delay-ms N         响应前睡 N 毫秒（用于触发客户端超时）
    --status N           一律返回该状态码（如 500）
    --malformed          返回非 JSON
    --fail-file PATH     该文件**存在**时一律返回 500（删掉它 = 故障恢复，用于恢复时间测试）
    --require-partition P  收到的 data-partition-id 与 P 不符 → 400（证明客户端确实带了这个头）
    --require-role R     请求的 roles 里没有 R → 400

用法：`python3 tests/tools/mock_entitlements.py --port 0 [开关...]`
      `--port 0` 时向 stdout 打印一行 `LISTENING <port>`。
"""
from __future__ import annotations

import argparse
import json
import os
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_BODY = 64 * 1024


class Config:
    def __init__(self, args: argparse.Namespace) -> None:
        self.granted = {r.strip() for r in args.grant.split(",") if r.strip()}
        self.delay_ms = args.delay_ms
        self.force_status = args.status
        self.malformed = args.malformed
        self.require_partition = args.require_partition
        self.require_role = args.require_role
        self.fail_file = args.fail_file
        #  观测：最后一次请求（供测试断言"客户端发了什么"）
        self.last_headers: dict[str, str] = {}
        self.last_roles: list[str] = []


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
        #  ★ 故障控制文件：存在 = 依赖不可用；删除 = 恢复（测试用它测"解除故障后的恢复时间"）
        if config.fail_file and os.path.exists(config.fail_file):
            self._reply(500, b'{"error":"injected failure"}')
            return
        if config.force_status != 0:
            self._reply(config.force_status, b'{"error":"injected"}')
            return
        if config.malformed:
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
            self._reply(400, b'{"error":"bad json"}')
            return
        roles = payload.get("roles") or []
        if not isinstance(roles, list):
            self._reply(400, b'{"error":"roles must be a list"}')
            return

        partition = self.headers.get("data-partition-id", "")
        authorization = self.headers.get("Authorization", "")
        config.last_headers = {"data-partition-id": partition, "authorization": authorization}
        config.last_roles = [str(r) for r in roles]

        #  协议前置：缺认证头 → 401（调用方必须映射成 kUnauthenticated）
        if not authorization:
            self._reply(401, b'{"error":"missing authorization"}')
            return
        if config.require_partition and partition != config.require_partition:
            self._reply(400, b'{"error":"unexpected partition"}')
            return
        if config.require_role and config.require_role not in roles:
            self._reply(400, b'{"error":"missing required role"}')
            return

        granted = [r for r in roles if r in config.granted]
        body = json.dumps({"allowed": bool(granted), "grantedRoles": granted}).encode("utf-8")
        self._reply(200, body)


def main() -> int:
    parser = argparse.ArgumentParser(description="mock Entitlements（authorizeAny）")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--grant", default="", help="逗号分隔的被授予角色")
    parser.add_argument("--delay-ms", type=int, default=0)
    parser.add_argument("--status", type=int, default=0, help="非 0 时一律返回该状态码")
    parser.add_argument("--malformed", action="store_true")
    parser.add_argument("--require-partition", default="")
    parser.add_argument("--require-role", default="")
    parser.add_argument("--fail-file", default="", help="该文件存在时一律 500（可删除以恢复）")
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
