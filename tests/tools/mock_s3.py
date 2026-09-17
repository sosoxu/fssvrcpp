#!/usr/bin/env python3
"""mock S3（path-style）—— **独立验签**的假 S3，用于证明我们的 SigV4 真的能被别人验过。

为什么必须是"独立实现"：如果 mock 复用我们 C++ 的签名器（或复用同一个思路的实现），
"自签自验"就什么也证明不了。这里用 `sigv4_reference.py`（纯 `hmac`/`hashlib` 重写）
按 AWS 规范重算 canonical request → string-to-sign → 签名，并逐字节比对。

支持（够 P5 用；不做完整 S3）：
  PUT    /<bucket>                        建桶
  HEAD   /<bucket>                        桶存在性（ensure_container）
  PUT    /<bucket>/<key>                  写入（或带 `x-amz-copy-source` 复制）
  GET    /<bucket>/<key>                  读取（支持 `Range`）
  HEAD   /<bucket>/<key>                  元数据（Content-Length/ETag/Content-Type）
  DELETE /<bucket>/<key>                  删除（幂等）
  GET    /<bucket>?list-type=2            列举（prefix / max-keys / continuation-token）

鉴权两种形态都验：
  ① **查询串预签名**：`X-Amz-Algorithm/Credential/Date/Expires/SignedHeaders/Signature`
  ② **头部签名**：`Authorization: AWS4-HMAC-SHA256 Credential=... SignedHeaders=... Signature=...`

错误可被触发（给 C5.7 用）：键名以 `.denied` 结尾 → 403 `AccessDenied`；
以 `.slowdown` 结尾 → 503 `SlowDown`；不存在的键 → 404 `NoSuchKey`。
所有拒绝都返回 S3 风格的 XML（`<Error><Code>..</Code>..</Error>`）。

用法：`python3 tests/tools/mock_s3.py --port 0 [--access-key K --secret-key S --region R]`
      `--port 0` 时向 stdout 打印一行 `LISTENING <port>`，便于测试确定性取端口。
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
import threading
import urllib.parse
import xml.sax.saxutils as xml_escape
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sigv4_reference as ref  # noqa: E402  （独立参照实现，只依赖标准库）

MAX_SKEW_SECONDS = 15 * 60  # 允许的时钟偏移（比 X-Amz-Expires 宽松，避免 CI 抖动）


class Store:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.buckets: set[str] = set()
        self.objects: dict[tuple[str, str], bytes] = {}
        self.content_types: dict[tuple[str, str], str] = {}

    def put(self, bucket: str, key: str, data: bytes, content_type: str) -> None:
        with self.lock:
            self.buckets.add(bucket)
            self.objects[(bucket, key)] = data
            self.content_types[(bucket, key)] = content_type or "application/octet-stream"

    def get(self, bucket: str, key: str):
        with self.lock:
            return self.objects.get((bucket, key))

    def delete(self, bucket: str, key: str) -> None:
        with self.lock:
            self.objects.pop((bucket, key), None)
            self.content_types.pop((bucket, key), None)

    def list(self, bucket: str, prefix: str):
        with self.lock:
            return sorted(k for (b, k) in self.objects if b == bucket and k.startswith(prefix))


class Config:
    def __init__(self, args) -> None:
        self.access_key = args.access_key
        self.secret_key = args.secret_key
        self.region = args.region
        self.service = args.service
        self.virtual_host_suffix = args.virtual_host_suffix
        self.store = Store()
        #  统计：测试用它断言"验签真的跑过"（不是靠"请求恰好成功"来推断）
        self.stats = {"verified_header": 0, "verified_query": 0, "rejected": 0}


def parse_query(raw: str) -> list[tuple[str, str]]:
    """把原始 query **按原样**拆成键值对（不重新编码：canonical query 由参照实现排序）。"""
    if not raw:
        return []
    pairs = []
    for part in raw.split("&"):
        if not part:
            continue
        key, _, value = part.partition("=")
        pairs.append((urllib.parse.unquote_plus(key), urllib.parse.unquote_plus(value)))
    return pairs


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "mock-s3/1.0"

    # ---- 基础 ----
    def log_message(self, fmt, *args):  # 静音（测试里不需要访问日志）
        pass

    def _config(self) -> Config:
        return self.server.config  # type: ignore[attr-defined]

    def _parse_target(self):
        parsed = urllib.parse.urlsplit(self.path)
        path = urllib.parse.unquote(parsed.path)
        raw_query = parsed.query
        #  ---- virtual-host 形态（C5.4）：桶名在 Host 的子域里，路径全是 key ----
        #  判定规则：`Host`（去端口）以 `<prefix>.<suffix>` 结尾且 prefix 非空。
        #  测试用 `curl --connect-to` 把 `bucket.s3.amazonaws.com:80` 指到本进程，
        #  所以这里能收到带桶前缀的 Host —— 不需要任何生产代码里的测试钩子。
        host = (self.headers.get("Host") or "").split(":")[0]
        suffix = getattr(self.server.config, "virtual_host_suffix", "")
        if suffix and host.endswith("." + suffix) and len(host) > len(suffix) + 1:
            bucket = host[: -(len(suffix) + 1)]
            return bucket, path.lstrip("/"), parse_query(raw_query), raw_query
        #  ---- path-style（默认） ----
        segments = path.lstrip("/").split("/", 1)
        bucket = segments[0] if segments and segments[0] else ""
        key = segments[1] if len(segments) > 1 else ""
        return bucket, key, parse_query(raw_query), raw_query

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def _send(self, status: int, body: bytes = b"", headers: dict | None = None) -> None:
        self.send_response(status)
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body and self.command != "HEAD":
            self.wfile.write(body)

    def _error(self, status: int, code: str, message: str, extra: str = "") -> None:
        body = (
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            f"<Error><Code>{code}</Code><Message>{xml_escape.escape(message)}</Message>"
            f"{extra}<RequestId>mock</RequestId></Error>"
        ).encode()
        self._send(status, body, {"Content-Type": "application/xml"})

    # ---- 验签（核心：独立实现）----
    def _verify(self, method: str, path: str, query: list[tuple[str, str]],
                body: bytes) -> tuple[bool, str]:
        config = self._config()
        host_header = self.headers.get("Host", "")
        query_dict = dict(query)

        if "X-Amz-Algorithm" in query_dict:
            #  ① 预签名 URL：签名在 query 里；payload 固定为 UNSIGNED-PAYLOAD
            if query_dict.get("X-Amz-Algorithm") != ref.ALGORITHM:
                return False, "algorithm"
            auth = ("AWS4-HMAC-SHA256 Credential=" + query_dict.get("X-Amz-Credential", "") +
                    ", SignedHeaders=" + query_dict.get("X-Amz-SignedHeaders", "") +
                    ", Signature=" + query_dict.get("X-Amz-Signature", ""))
            #  canonical query 里**不含** X-Amz-Signature（由 ref.verify 负责剔除）
            payload = ref.UNSIGNED_PAYLOAD
            headers = [("host", host_header)]
            #  ★ 过期判定：X-Amz-Date + X-Amz-Expires（S3 用 403，不用 401）
            import datetime
            try:
                stamp = datetime.datetime.strptime(query_dict["X-Amz-Date"], "%Y%m%dT%H%M%SZ")
                issued = stamp.replace(tzinfo=datetime.timezone.utc).timestamp()
            except (KeyError, ValueError):
                return False, "missing_x_amz_date"
            expires = int(query_dict.get("X-Amz-Expires", "0"))
            import time
            if issued - MAX_SKEW_SECONDS > time.time():
                return False, "request_not_yet_valid"
            if time.time() > issued + expires:
                return False, "request_expired"
            ok, reason = ref.verify(method, host_header, path, query, headers, payload, auth,
                                    config.secret_key, config.region, config.service)
            if ok:
                config.stats["verified_query"] += 1
            return ok, reason

        #  ② 头部签名
        authorization = self.headers.get("Authorization", "")
        if not authorization:
            return False, "missing_authorization"
        #  只把声明参与签名的头喂给参照实现（其余头不参与重算）
        pos = authorization.find("SignedHeaders=")
        if pos < 0:
            return False, "malformed_authorization"
        begin = pos + len("SignedHeaders=")
        end = authorization.find(",", begin)
        names = authorization[begin:end if end >= 0 else len(authorization)]
        headers = []
        for name in names.split(";"):
            if name == "host":
                headers.append(("host", host_header))
            elif name == "x-amz-content-sha256":
                headers.append(("x-amz-content-sha256", self.headers.get("x-amz-content-sha256", "")))
            else:
                value = self.headers.get(name)
                if value is None:
                    return False, f"signed_header_missing:{name}"
                headers.append((name, value))
        payload = self.headers.get("x-amz-content-sha256") or ref.EMPTY_SHA256
        ok, reason = ref.verify(method, host_header, path, query, headers, payload, authorization,
                                config.secret_key, config.region, config.service)
        if ok:
            config.stats["verified_header"] += 1
        return ok, reason

    def _handle(self, method: str) -> None:
        config = self._config()
        bucket, key, query, _raw_query = self._parse_target()
        body = self._read_body()
        path = urllib.parse.urlsplit(self.path).path

        ok, reason = self._verify(method, path, query, body)
        if not ok:
            config.stats["rejected"] += 1
            code = "AccessDenied" if reason in ("request_expired", "request_not_yet_valid") else \
                   "SignatureDoesNotMatch"
            if reason == "request_expired":
                self._error(403, code, "Request has expired")
            else:
                self._error(403, code, f"签名校验失败：{reason}")
            return

        if key.endswith(".denied"):
            self._error(403, "AccessDenied", "Access Denied")
            return
        if key.endswith(".slowdown"):
            self._error(503, "SlowDown", "Please reduce your request rate.")
            return

        #  ---- 桶级 ----
        if not key:
            if method == "PUT":
                config.store.buckets.add(bucket)
                self._send(200, b"", {"Content-Type": "application/xml"})
                return
            if method == "HEAD":
                if bucket in config.store.buckets:
                    self._send(200)
                else:
                    self._send(404)
                return
            if method == "GET":
                self._list(bucket, query)
                return
            self._error(405, "MethodNotAllowed", "不支持的方法")
            return

        #  ---- 对象级 ----
        #  ★ 桶必须**先**创建（与真实 S3 一致）：PUT /<bucket> 或 ensure_container。
        #    这样 "put 到不存在的容器 → kNotFound" 这条契约才真的被测到（C5.3）。
        #    注意：这条检查只在**对象级**请求上做，否则 PUT /<bucket>（建桶）会自己把自己挡住。
        if bucket not in config.store.buckets:
            self._error(404, "NoSuchBucket", "The specified bucket does not exist")
            return
        if method == "PUT":
            copy_source = self.headers.get("x-amz-copy-source")
            if copy_source:
                source = urllib.parse.unquote(copy_source).lstrip("/")
                src_bucket, _, src_key = source.partition("/")
                data = config.store.get(src_bucket, src_key)
                if data is None:
                    self._error(404, "NoSuchKey", "The specified key does not exist.")
                    return
                config.store.put(bucket, key, data,
                                 config.store.content_types.get((src_bucket, src_key), ""))
                etag = hashlib.md5(data).hexdigest()  # noqa: S324 （mock 用，不涉安全）
                body_xml = ('<?xml version="1.0" encoding="UTF-8"?>\n'
                            f"<CopyObjectResult><ETag>\"{etag}\"</ETag>"
                            f"<LastModified>2024-01-01T00:00:00.000Z</LastModified>"
                            "</CopyObjectResult>").encode()
                self._send(200, body_xml, {"Content-Type": "application/xml"})
                return
            #  `x-amz-checksum-sha256`（base64）由**我们独立重算**校验：错包必须被拒
            declared = self.headers.get("x-amz-checksum-sha256")
            if declared:
                import base64
                actual = base64.b64encode(hashlib.sha256(body).digest()).decode()
                if actual != declared:
                    self._error(400, "BadDigest",
                                "The Content-SHA256 you specified did not match what we received.")
                    return
            content_type = self.headers.get("Content-Type", "application/octet-stream")
            config.store.put(bucket, key, body, content_type)
            etag = hashlib.md5(body).hexdigest()  # noqa: S324
            self._send(200, b"", {"ETag": f'"{etag}"'})
            return

        data = config.store.get(bucket, key)
        if data is None:
            self._error(404, "NoSuchKey", "The specified key does not exist.")
            return
        if method in ("GET", "HEAD"):
            content_type = config.store.content_types.get((bucket, key), "application/octet-stream")
            etag = hashlib.md5(data).hexdigest()  # noqa: S324
            headers = {"Content-Type": content_type, "ETag": f'"{etag}"',
                       "Accept-Ranges": "bytes"}
            range_header = self.headers.get("Range")
            if range_header:
                matched = re.match(r"bytes=(\d*)-(\d*)$", range_header.strip())
                if matched:
                    first, last = matched.group(1), matched.group(2)
                    if first == "" and last != "":  # 后缀区间
                        start = max(0, len(data) - int(last))
                        end = len(data) - 1
                    else:
                        start = int(first)
                        end = int(last) if last else len(data) - 1
                    if start >= len(data):
                        self._error(416, "InvalidRange", "The requested range is not satisfiable")
                        return
                    end = min(end, len(data) - 1)
                    chunk = data[start:end + 1]
                    headers["Content-Range"] = f"bytes {start}-{end}/{len(data)}"
                    self._send(206, chunk, headers)
                    return
            self._send(200, data, headers)
            return
        if method == "DELETE":
            config.store.delete(bucket, key)  # 幂等：删不存在的键也返回 204
            self._send(204)
            return
        self._error(405, "MethodNotAllowed", "不支持的方法")

    def _list(self, bucket: str, query: list[tuple[str, str]]) -> None:
        config = self._config()
        #  ★ 与对象级一样：列举不存在的桶 → 404 NoSuchBucket（契约要求 kNotFound）
        if bucket not in config.store.buckets:
            self._error(404, "NoSuchBucket", "The specified bucket does not exist")
            return
        params = dict(query)
        prefix = params.get("prefix", "")
        max_keys = int(params.get("max-keys", "1000"))
        token = params.get("continuation-token", "")
        keys = config.store.list(bucket, prefix)
        #  token 对客户端是**不透明**的（S3 的语义）：这里就用"下一条的索引"。
        #  ⚠️ 曾用"上一页最后一个 key"当 token，结果把该 key 又返回一次（多一页 + 重复）——
        #     正是 C5.6"不重不漏"要抓的那种缺陷，只不过这次出在 mock 侧（P5-D04）。
        start = 0
        if token:
            try:
                start = max(0, min(int(token), len(keys)))
            except ValueError:
                start = 0
        page = keys[start:start + max_keys]
        truncated = start + len(page) < len(keys)
        contents = "".join(
            "<Contents>"
            f"<Key>{xml_escape.escape(k)}</Key>"
            f"<Size>{len(config.store.get(bucket, k) or b'')}</Size>"
            "<LastModified>2024-01-01T00:00:00.000Z</LastModified>"
            f"<ETag>\"{hashlib.md5(config.store.get(bucket, k) or b'').hexdigest()}\"</ETag>"
            "<StorageClass>STANDARD</StorageClass></Contents>"
            for k in page
        )
        next_token = ""
        if truncated and page:
            next_token = (f"<NextContinuationToken>{start + len(page)}"
                          "</NextContinuationToken>")
        body = ('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<ListBucketResult>'
                f"<Name>{xml_escape.escape(bucket)}</Name>"
                f"<Prefix>{xml_escape.escape(prefix)}</Prefix>"
                f"<KeyCount>{len(page)}</KeyCount>"
                f"<MaxKeys>{max_keys}</MaxKeys>"
                f"<IsTruncated>{'true' if truncated else 'false'}</IsTruncated>"
                f"{next_token}{contents}"
                "</ListBucketResult>").encode()
        self._send(200, body, {"Content-Type": "application/xml"})

    # ---- HTTP 动词 ----
    def do_GET(self):  # noqa: N802
        self._handle("GET")

    def do_PUT(self):  # noqa: N802
        self._handle("PUT")

    def do_HEAD(self):  # noqa: N802
        self._handle("HEAD")

    def do_DELETE(self):  # noqa: N802
        self._handle("DELETE")


def main() -> int:
    parser = argparse.ArgumentParser(description="mock S3（独立 SigV4 验签）")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--access-key", default="AKIDEXAMPLE")
    parser.add_argument("--secret-key", default="wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY")
    parser.add_argument("--region", default="us-east-1")
    #  virtual-host 形态的 Host 后缀（空 = 只支持 path-style）
    parser.add_argument("--virtual-host-suffix", default="s3.amazonaws.com")
    parser.add_argument("--service", default="s3")
    #  测试脚手架：预置 N 个对象（分页用例需要 >1000 个键；用 HTTP 逐个写太慢，
    #  而"分页"要测的是**客户端**如何翻页，不是"能不能写 1000 次"）
    parser.add_argument("--seed-count", type=int, default=0)
    parser.add_argument("--seed-prefix", default="page/")
    parser.add_argument("--seed-bucket", default="testbucket")
    args = parser.parse_args()

    config = Config(args)
    if args.seed_count > 0:
        config.store.buckets.add(args.seed_bucket)
        for index in range(args.seed_count):
            key = f"{args.seed_prefix}{index:05d}"
            config.store.put(args.seed_bucket, key, f"payload-{index}".encode(), "text/plain")
    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    httpd.config = config  # type: ignore[attr-defined]
    port = httpd.server_address[1]
    print(f"LISTENING {port}", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
