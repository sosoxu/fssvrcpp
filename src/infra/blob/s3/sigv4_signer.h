// =============================================================================
//  SigV4Signer（L2）—— AWS Signature Version 4：签名与预签名（无 AWS SDK）
// =============================================================================
//  为什么自研：ADR-002/ADR-005 已定案"不引入 aws-sdk-cpp"（镜像里没有，且只需
//  S3 的 5 个动词 + 预签名）。OpenSSL 提供的 SHA-256/HMAC 已经足够（P0 Q1.3 已实测）。
//
//  ★ 三条不能弄错的规则（C5.1/C5.5，每一条都有边界用例）
//    ① 百分号编码：**未保留字符 `A-Za-z0-9-_.~` 不编码**（`~` 是 RFC 3986 的
//       unreserved —— 编成 `%7E` 会让签名与真实 S3 不一致）；
//       `/` 在 **canonical URI** 里保留、在 **query 的键/值** 里必须编成 `%2F`；
//       空格 → `%20`（**不是** `+`）；非 ASCII 走 UTF-8 字节再逐字节 `%XX`（大写十六进制）。
//    ② 派生密钥必须用**上一轮的原始 32 字节摘要**作 key（crypto.h 已明确），
//       用 hex 字符串当 key 会得到完全不同的签名（P0 已踩）。
//    ③ 签名的覆盖范围由 `SignedHeaders` 显式声明；预签名 URL 的 payload 是
//       `UNSIGNED-PAYLOAD`（客户端流式上传时服务端不会重算）。
//
//  ★ 独立性：本文件**不**参与校验自己产出的签名（自签自验等于没验）。
//    交叉验证有两条独立路径：
//      · `tests/integration/test_sigv4_crosscheck.cpp` —— 用 **libcurl 的 `--aws-sigv4`**
//        （第三方实现）对同一个请求签名并逐字节比对 `Authorization` 头；
//      · `tests/tools/mock_s3.py` —— Python 用 `hmac`/`hashlib` 独立重算（C5.2）。
#pragma once

#include "common/result/result.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fss::infra {

// -----------------------------------------------------------------------------
//  编码（C5.5 的边界：`~` 空格 `+` `%` 非 ASCII `/`）
// -----------------------------------------------------------------------------
//  SigV4 的编码是 RFC 3986 的 **unreserved** 集合：A-Z a-z 0-9 - _ . ~
//  `keep_slash` 为真时保留 `/`（canonical URI 用），否则编码为 `%2F`（query 用）。
std::string SigV4Encode(std::string_view value, bool keep_slash);
inline std::string SigV4EncodePath(std::string_view path) { return SigV4Encode(path, true); }
inline std::string SigV4EncodeQuery(std::string_view value) { return SigV4Encode(value, false); }

// -----------------------------------------------------------------------------
//  请求描述（只描述"参与签名的东西"，不含 socket/IO）
// -----------------------------------------------------------------------------
struct SigV4Request {
  std::string method;       // "GET" / "PUT" / "HEAD" / "DELETE" / "POST"
  std::string host;         // 参与签名的 Host（path-style / virtual-host 都由调用方决定）
  std::string path;         // **未编码**的路径，如 "/bucket/dir/key"（`/` 会被保留）
  //  查询串（**未编码**的键值对；空值也必须出现在 canonical query 里）
  std::vector<std::pair<std::string, std::string>> query;
  //  参与签名的头（**名字小写**；`host` 与 `x-amz-date`、`x-amz-content-sha256` 必须在内）
  std::vector<std::pair<std::string, std::string>> headers;
  std::string payload_sha256_hex = "UNSIGNED-PAYLOAD";
};

struct AwsCredentials {
  std::string access_key;
  std::string secret_key;
  std::string session_token;  // 可选（STS）；非空时必须进 SignedHeaders
};

struct SigV4Options {
  std::string region = "us-east-1";
  std::string service = "s3";
  //  `true` → `http://endpoint/bucket/key`；`false` → `http://bucket.endpoint/key`
  bool force_path_style = true;
  //  仅用于拼 URL（不参与签名）；形如 "s3.amazonaws.com" 或 "127.0.0.1:9000"
  std::string endpoint;
  //  预签名 URL 的 scheme（"http" / "https"）
  std::string scheme = "https";
};

struct SigV4Signature {
  std::string canonical_request;  // 便于排障与"逐字节比对"（不参与线上协议）
  std::string string_to_sign;
  std::string signature;          // hex
  std::string authorization;      // 完整的 Authorization 头值
  std::string amz_date;           // 实际使用的 x-amz-date（yyyyMMdd'T'HHmmss'Z'）
  std::string signed_headers;     // ";" 连接的有序头名
};

class SigV4Signer {
 public:
  SigV4Signer(AwsCredentials credentials, SigV4Options options);

  //  ---- 头部签名（数据面：PUT/GET/HEAD/DELETE + libcurl）----
  //  `amz_date` 形如 "20130524T000000Z"（由调用方从时钟取，便于确定性测试）
  Result<SigV4Signature> SignRequest(const SigV4Request& request,
                                     std::string_view amz_date) const;

  //  ---- 查询串预签名（上传/下载地址，服务不代理字节；C5.8）----
  //  产出完整 URL（含 X-Amz-* 与 X-Amz-Signature）。`expires_in_seconds` 上限 7 天。
  Result<std::string> PresignUrl(const SigV4Request& request, std::string_view amz_date,
                                 std::int64_t expires_in_seconds) const;

  //  ---- URL 组装（path-style / virtual-host 两种形态，C5.4）----
  Result<std::string> BuildUrl(std::string_view bucket, std::string_view key,
                               const std::vector<std::pair<std::string, std::string>>& query,
                               bool include_query) const;
  //  与 BuildUrl 配套的 Host（必须与请求里签的 host 完全一致）
  std::string HostFor(std::string_view bucket) const;

  const SigV4Options& options() const { return options_; }

 private:
  std::string Scope(std::string_view amz_date) const;  // "<yyyymmdd>/<region>/<service>/aws4_request"
  AwsCredentials credentials_;
  SigV4Options options_;
};

//  典型请求的便捷构造：给 method/host/path/query/bucket 与可选的额外头，
//  自动补 `host` / `x-amz-date` / `x-amz-content-sha256`（三者都必须参与签名）。
SigV4Request MakeSigV4Request(const SigV4Options& options, std::string_view method,
                              std::string_view bucket, std::string_view key,
                              std::vector<std::pair<std::string, std::string>> query = {},
                              std::vector<std::pair<std::string, std::string>> extra_headers = {},
                              std::string_view payload_sha256_hex = "UNSIGNED-PAYLOAD",
                              std::string_view amz_date = {});

}  // namespace fss::infra
