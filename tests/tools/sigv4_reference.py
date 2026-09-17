#!/usr/bin/env python3
"""AWS SigV4 的**独立参照实现**（只依赖 Python 标准库 hmac/hashlib）。

用途（两条，都要求"不复用被测实现"）：
  ① mock-S3（`tests/tools/mock_s3.py`）用它**验签**我们 C++ 侧签出来的请求（C5.2）；
  ② 单元测试做"等价比对"：同一请求分别由 C++ 与 Python 计算，签名必须逐字节相同（R18）。

⚠️ 本文件**不是**被测代码：它按 AWS 文档的算法独立重写（canonical request → string to sign
   → 派生密钥 → HMAC）。两边同时写错的可能性存在，因此还需要 libcurl `--aws-sigv4`
   这条第三方实现的交叉验证（见 `tests/integration/test_sigv4_crosscheck.cpp`）。
"""
from __future__ import annotations

import hashlib
import hmac
from typing import Iterable, Sequence
from urllib.parse import quote

ALGORITHM = "AWS4-HMAC-SHA256"
UNSIGNED_PAYLOAD = "UNSIGNED-PAYLOAD"
EMPTY_SHA256 = hashlib.sha256(b"").hexdigest()

_UNRESERVED = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~"


def encode(value: str, keep_slash: bool = False) -> str:
    """SigV4 的百分号编码：只保留 RFC 3986 unreserved（含 `~`），其余 `%XX`（大写）。

    ★ 不用 `urllib.parse.quote_plus`：它把空格编成 `+`，且默认保留 `/`。
    """
    safe = _UNRESERVED + ("/" if keep_slash else "")
    return quote(value, safe=safe, encoding="utf-8", errors="strict")


def canonical_query(query: Iterable[tuple[str, str]]) -> str:
    encoded = sorted((encode(k), encode(v)) for k, v in query)
    return "&".join(f"{k}={v}" for k, v in encoded)


def canonical_headers(headers: Iterable[tuple[str, str]]) -> tuple[str, str]:
    normalized = {}
    for name, value in headers:
        normalized[name.lower()] = " ".join(value.split())
    block = "".join(f"{name}:{normalized[name]}\n" for name in sorted(normalized))
    names = ";".join(sorted(normalized))
    return block, names


def derive_signing_key(secret: str, date: str, region: str, service: str) -> bytes:
    key = ("AWS4" + secret).encode("utf-8")
    for part in (date, region, service, "aws4_request"):
        key = hmac.new(key, part.encode("utf-8"), hashlib.sha256).digest()
    return key


def canonical_request(method: str, path: str, query: Sequence[tuple[str, str]],
                      headers: Sequence[tuple[str, str]], payload_hash: str) -> tuple[str, str]:
    block, names = canonical_headers(headers)
    creq = "\n".join([method, encode(path, keep_slash=True), canonical_query(query),
                      block, names, payload_hash])
    return creq, names


def string_to_sign(amz_date: str, scope: str, creq: str) -> str:
    return "\n".join([ALGORITHM, amz_date, scope, hashlib.sha256(creq.encode("utf-8")).hexdigest()])


def sign(method: str, host: str, path: str, query: Sequence[tuple[str, str]],
         headers: Sequence[tuple[str, str]], payload_hash: str, access_key: str,
         secret_key: str, region: str, service: str, amz_date: str) -> dict:
    """返回 {signature, authorization, canonical_request, string_to_sign, signed_headers}"""
    date = amz_date[:8]
    scope = f"{date}/{region}/{service}/aws4_request"
    creq, names = canonical_request(method, path, query, headers, payload_hash)
    sts = string_to_sign(amz_date, scope, creq)
    signature = hmac.new(derive_signing_key(secret_key, date, region, service),
                         sts.encode("utf-8"), hashlib.sha256).hexdigest()
    authorization = (f"{ALGORITHM} Credential={access_key}/{scope}, "
                     f"SignedHeaders={names}, Signature={signature}")
    return {"signature": signature, "authorization": authorization, "canonical_request": creq,
            "string_to_sign": sts, "signed_headers": names, "scope": scope}


def verify(method: str, host: str, path: str, query: Sequence[tuple[str, str]],
           headers: Sequence[tuple[str, str]], payload_hash: str, authorization: str,
           secret_key: str, region: str, service: str,
           session_token: str = "") -> tuple[bool, str]:
    """按 Authorization 头里声明的 scope/SignedHeaders 重算并比对。

    返回 (是否通过, 原因)。**不信任** Authorization 里的 region/service —— 用配置值，
    并校验它确实出现在 scope 里。
    """
    fields = {}
    if not authorization.startswith(ALGORITHM + " "):
        return False, "algorithm"
    for part in authorization[len(ALGORITHM) + 1:].split(","):
        part = part.strip()
        if "=" in part:
            key, _, value = part.partition("=")
            fields[key] = value
    credential = fields.get("Credential", "")
    signed_headers = fields.get("SignedHeaders", "")
    signature = fields.get("Signature", "")
    if "/" not in credential or not signed_headers or not signature:
        return False, "malformed_authorization"
    access_key, _, scope = credential.partition("/")
    if scope != f"{scope.split('/')[0]}/{region}/{service}/aws4_request":
        return False, "scope_mismatch"
    #  只把声明参与签名的头纳入重算（其余头（User-Agent 等）不参与）
    wanted = set(signed_headers.split(";"))
    used, _ = canonical_headers([(k, v) for k, v in headers if k.lower() in wanted])
    block, names = canonical_headers([(k, v) for k, v in headers if k.lower() in wanted])
    if names != signed_headers:
        return False, "signed_headers_mismatch"
    date = scope.split("/")[0]
    amz_date = ""
    for k, v in headers:
        if k.lower() == "x-amz-date":
            amz_date = v
    if len(amz_date) != 16:
        #  预签名形态：`X-Amz-Date` 在 query 里，不在 SignedHeaders 里
        for k, v in query:
            if k == "X-Amz-Date":
                amz_date = v
    if len(amz_date) != 16:
        return False, "missing_x_amz_date"
    #  ★ `X-Amz-Signature` **不参与**被签内容（否则永远验不过）——
    #    这正是"预签名请求全部被拒、且报成签名不匹配"的根因（P5-D03）。
    signing_query = [(k, v) for k, v in query if k != "X-Amz-Signature"]
    creq = "\n".join([method, encode(path, keep_slash=True), canonical_query(signing_query),
                      block, names, payload_hash])
    #  ★ 必须把**剔除 X-Amz-Signature 之后**的 query 交给 sign() ——
    #    sign() 会自己重算一遍 canonical query；传全量会把签名本身也算进去，
    #    于是所有预签名请求都被判为不匹配（P5-D03）。
    expected = sign(method, host, path, signing_query, headers, payload_hash, access_key,
                    secret_key, region, service, amz_date)["signature"]
    return (hmac.compare_digest(expected, signature), "ok" if expected == signature else "signature")


def presign_url(method: str, scheme: str, host: str, path: str,
                query: Sequence[tuple[str, str]], extra_headers: Sequence[tuple[str, str]],
                access_key: str, secret_key: str, region: str, service: str,
                amz_date: str, expires: int) -> str:
    """查询串预签名：产出的 URL 里带 X-Amz-* 与 X-Amz-Signature。"""
    date = amz_date[:8]
    scope = f"{date}/{region}/{service}/aws4_request"
    headers = list(extra_headers) or [("host", host)]
    if not any(k.lower() == "host" for k, _ in headers):
        headers = [("host", host)] + list(headers)
    block, names = canonical_headers(headers)
    params = list(query) + [
        ("X-Amz-Algorithm", ALGORITHM),
        ("X-Amz-Credential", f"{access_key}/{scope}"),
        ("X-Amz-Date", amz_date),
        ("X-Amz-Expires", str(expires)),
        ("X-Amz-SignedHeaders", names),
    ]
    creq = "\n".join([method, encode(path, keep_slash=True), canonical_query(params), block,
                      names, UNSIGNED_PAYLOAD])
    sts = string_to_sign(amz_date, scope, creq)
    signature = hmac.new(derive_signing_key(secret_key, date, region, service),
                         sts.encode("utf-8"), hashlib.sha256).hexdigest()
    params.append(("X-Amz-Signature", signature))
    return f"{scheme}://{host}{encode(path, keep_slash=True)}?{canonical_query(params)}"


def _vector_from_json(path: str) -> dict:
    import json
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


if __name__ == "__main__":
    import argparse
    import json

    parser = argparse.ArgumentParser(description="SigV4 独立参照实现")
    parser.add_argument("--sign-vector", help="对 JSON 向量文件求签，输出一行 JSON")
    parser.add_argument("--presign-vector", help="对 JSON 向量文件做预签名 URL，输出一行 JSON")
    args = parser.parse_args()

    if args.sign_vector or args.presign_vector:
        vector = _vector_from_json(args.sign_vector or args.presign_vector)
        query = [tuple(pair) for pair in vector.get("query", [])]
        headers = [tuple(pair) for pair in vector.get("headers", [])]
        if args.sign_vector:
            result = sign(vector["method"], vector["host"], vector["path"], query, headers,
                          vector.get("payload_sha256_hex", UNSIGNED_PAYLOAD),
                          vector["access_key"], vector["secret_key"],
                          vector.get("region", "us-east-1"), vector.get("service", "s3"),
                          vector["amz_date"])
        else:
            url = presign_url(vector["method"], vector.get("scheme", "https"), vector["host"],
                              vector["path"], query, headers, vector["access_key"],
                              vector["secret_key"], vector.get("region", "us-east-1"),
                              vector.get("service", "s3"), vector["amz_date"],
                              int(vector.get("expires", 3600)))
            result = {"url": url}
        print(json.dumps(result, sort_keys=True))
        raise SystemExit(0)

    demo = sign("GET", "examplebucket.s3.amazonaws.com", "/test.txt", [],
                [("host", "examplebucket.s3.amazonaws.com"),
                 ("x-amz-date", "20130524T000000Z")], EMPTY_SHA256,
                "AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", "us-east-1", "s3",
                "20130524T000000Z")
    print(demo["canonical_request"])
    print("---")
    print(demo["string_to_sign"])
    print("---")
    print(demo["authorization"])
