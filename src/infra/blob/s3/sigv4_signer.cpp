// SigV4Signer 实现。算法与编码规则的依据见头文件与 ADR-005。
#include "infra/blob/s3/sigv4_signer.h"

#include "common/crypto/crypto.h"

#include <algorithm>
#include <map>
#include <cctype>
#include <cstdint>
#include <span>
#include <tuple>

namespace fss::infra {

namespace {

constexpr std::string_view kAlgorithm = "AWS4-HMAC-SHA256";
constexpr std::string_view kUnsignedPayload = "UNSIGNED-PAYLOAD";
//  S3 的预签名上限：7 天（超过会被服务端拒签）
constexpr std::int64_t kMaxPresignSeconds = 7 * 24 * 3600;

//  `crypto::HmacSha256` 的两个重载是 (span, span) 与 (string_view, string_view)；
//  用 `Digest256` 当 key 时必须显式转 span（数组 → span 的隐式转换对 const 元素不成立）
std::string HmacHex(const crypto::Digest256& key, std::string_view data) {
  const auto digest = crypto::HmacSha256(
      std::span<const std::uint8_t>(key.data(), key.size()),
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()),
                                    data.size()));
  return crypto::ToHex(digest);
}

bool IsUnreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '_' || c == '.' || c == '~';  // ★ `~` 不编码
}

std::string Trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
  return std::string(text.substr(begin, end - begin));
}

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

//  压缩连续空白（SigV4 对头值的规范化要求）
std::string CollapseSpaces(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool in_space = false;
  for (const char c : text) {
    const bool is_space = (c == ' ' || c == '\t');
    if (is_space) {
      if (!in_space && !out.empty()) out.push_back(' ');
      in_space = true;
      continue;
    }
    out.push_back(c);
    in_space = false;
  }
  if (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

//  canonical query string：键/值都按 query 规则编码，按 (key, value) 字典序排序
std::string CanonicalQuery(const std::vector<std::pair<std::string, std::string>>& query) {
  std::vector<std::pair<std::string, std::string>> encoded;
  encoded.reserve(query.size());
  for (const auto& [key, value] : query) {
    encoded.emplace_back(SigV4EncodeQuery(key), SigV4EncodeQuery(value));
  }
  std::sort(encoded.begin(), encoded.end());
  std::string out;
  for (const auto& [key, value] : encoded) {
    if (!out.empty()) out.push_back('&');
    out += key;
    out.push_back('=');
    out += value;
  }
  return out;
}

struct CanonicalHeaders {
  std::string block;   // 每行 "<name>:<value>\n"
  //  ⚠️ 不能叫 `signed`（C++ 关键字）；这是中间变量，不是 SigV4Signature 的同名字段
  std::string names;   // "<name>;<name>"
};

CanonicalHeaders BuildCanonicalHeaders(
    const std::vector<std::pair<std::string, std::string>>& headers) {
  //  ★ 同名头必须**用逗号连接**（AWS 规范：duplicate headers 合并为逗号分隔的列表），
  //    而且顺序是"先连接、再去多余空白/首尾空白"（对齐 AWS SDK 的 stripExcessSpaces）。
  //    曾经写成"后值覆盖前值"，被 AWS 官方向量抓出（`TestSignRequest` 里
  //    `X-Amz-Meta-Other-Header_With_Underscore` 被 Add 了两次 —— P5-D08）。
  std::map<std::string, std::vector<std::string>> collected;
  for (const auto& [name, value] : headers) {
    collected[Lower(name)].push_back(value);
  }
  CanonicalHeaders out;
  for (auto& [name, values] : collected) {
    std::string joined;
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (i > 0) joined.push_back(',');
      joined += values[i];
    }
    out.block += name + ":" + CollapseSpaces(Trim(joined)) + "\n";
    if (!out.names.empty()) out.names.push_back(';');
    out.names += name;
  }
  return out;
}

bool HasHeader(const std::vector<std::pair<std::string, std::string>>& headers,
               std::string_view name) {
  for (const auto& [key, value] : headers) {
    (void)value;
    if (Lower(key) == name) return true;
  }
  return false;
}

Result<void> ValidateRequest(const SigV4Request& request) {
  if (request.method.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "SigV4：method 不能为空");
  }
  if (request.host.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "SigV4：host 不能为空");
  }
  if (!HasHeader(request.headers, "host")) {
    return Err(fss::ErrorKind::kInvalidArgument, "SigV4：SignedHeaders 必须包含 host");
  }
  if (!HasHeader(request.headers, "x-amz-date")) {
    return Err(fss::ErrorKind::kInvalidArgument, "SigV4：SignedHeaders 必须包含 x-amz-date");
  }
  return Ok();
}

}  // namespace

std::string SigV4Encode(std::string_view value, bool keep_slash) {
  static const char* kHexDigits = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size());
  for (const char raw : value) {
    const auto c = static_cast<unsigned char>(raw);
    if (IsUnreserved(c) || (keep_slash && c == '/')) {
      out.push_back(static_cast<char>(c));
      continue;
    }
    out.push_back('%');
    out.push_back(kHexDigits[(c >> 4) & 0x0F]);
    out.push_back(kHexDigits[c & 0x0F]);
  }
  return out;
}

SigV4Signer::SigV4Signer(AwsCredentials credentials, SigV4Options options)
    : credentials_(std::move(credentials)), options_(std::move(options)) {}

std::string SigV4Signer::Scope(std::string_view amz_date) const {
  //  amz_date = "20130524T000000Z" → 日期部分 = 前 8 位
  const std::string date(amz_date.substr(0, 8));
  return date + "/" + options_.region + "/" + options_.service + "/" +
         std::string(crypto::kSigV4Terminator);
}

std::string SigV4Signer::HostFor(std::string_view bucket) const {
  if (options_.force_path_style || bucket.empty()) return options_.endpoint;
  return std::string(bucket) + "." + options_.endpoint;
}

fss::Result<std::string> SigV4Signer::BuildUrl(
    std::string_view bucket, std::string_view key,
    const std::vector<std::pair<std::string, std::string>>& query, bool include_query) const {
  if (options_.endpoint.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "SigV4：endpoint 未配置，无法拼 URL");
  }
  std::string url = options_.scheme + "://" + HostFor(bucket);
  if (options_.force_path_style && !bucket.empty()) url += "/" + std::string(bucket);
  if (!key.empty()) url += "/" + SigV4EncodePath(key);
  if (include_query && !query.empty()) url += "?" + CanonicalQuery(query);
  return url;
}

fss::Result<SigV4Signature> SigV4Signer::SignRequest(const SigV4Request& request,
                                                     std::string_view amz_date) const {
  FSS_TRY(ValidateRequest(request));
  if (amz_date.size() != 16) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "SigV4：amz_date 必须形如 20130524T000000Z（16 字符）");
  }

  const auto canonical_headers = BuildCanonicalHeaders(request.headers);
  const std::string canonical_request =
      request.method + "\n" + SigV4EncodePath(request.path) + "\n" +
      CanonicalQuery(request.query) + "\n" + canonical_headers.block + "\n" +
      canonical_headers.names + "\n" + std::string(request.payload_sha256_hex);

  std::string string_to_sign = std::string(kAlgorithm) + "\n" + std::string(amz_date) + "\n" +
                               Scope(amz_date) + "\n" + crypto::Sha256Hex(canonical_request);

  const auto signing_key =
      crypto::DeriveSigningKey(credentials_.secret_key, std::string(amz_date.substr(0, 8)),
                               options_.region, options_.service);
  SigV4Signature out;
  out.canonical_request = canonical_request;
  out.string_to_sign = std::move(string_to_sign);
  //  ★ 必须在**移动之后**用 `out.string_to_sign` 计算签名：写成
  //    `out.signature = HmacHex(signing_key, string_to_sign)`（移动前那个变量）会对着
  //    已搬空的字符串求 HMAC —— 签名变成**与输入无关的常量**（P5-D01，
  //    "签名覆盖性"用例一上手就抓到；只断言"签名是 64 位 hex"的测试会放过它）。
  out.signature = HmacHex(signing_key, out.string_to_sign);
  out.signed_headers = canonical_headers.names;
  out.amz_date = std::string(amz_date);
  out.authorization = std::string(kAlgorithm) + " Credential=" + credentials_.access_key + "/" +
                      Scope(amz_date) + ", SignedHeaders=" + out.signed_headers +
                      ", Signature=" + out.signature;
  return out;
}

fss::Result<std::string> SigV4Signer::PresignUrl(const SigV4Request& request,
                                                std::string_view amz_date,
                                                std::int64_t expires_in_seconds) const {
  if (credentials_.access_key.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "SigV4：预签名需要 access_key");
  }
  if (expires_in_seconds <= 0 || expires_in_seconds > kMaxPresignSeconds) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "SigV4：X-Amz-Expires 必须在 1..604800 秒之间（S3 上限 7 天）");
  }

  //  ★ 预签名把"认证信息"放进 query：先构造除 X-Amz-Signature 之外的参数，
  //    再对**包含它们的** canonical query 签名，最后把 Signature 追加到 URL。
  std::vector<std::pair<std::string, std::string>> query = request.query;
  query.emplace_back("X-Amz-Algorithm", std::string(kAlgorithm));
  query.emplace_back("X-Amz-Credential", credentials_.access_key + "/" + Scope(amz_date));
  query.emplace_back("X-Amz-Date", std::string(amz_date));
  query.emplace_back("X-Amz-Expires", std::to_string(expires_in_seconds));

  //  SignedHeaders 至少要含 host；调用方给的头（如 content-type）一并签进去
  std::vector<std::pair<std::string, std::string>> headers = request.headers;
  if (headers.empty()) headers.emplace_back("host", request.host);
  const auto canonical_headers = BuildCanonicalHeaders(headers);
  query.emplace_back("X-Amz-SignedHeaders", canonical_headers.names);
  if (!credentials_.session_token.empty()) {
    query.emplace_back("X-Amz-Security-Token", credentials_.session_token);
  }

  //  ★ payload 哈希**由调用方决定**：S3 的预签名用 `UNSIGNED-PAYLOAD`（URL 无法预知体），
  //    但其它服务（如 DynamoDB）的预签名要签真实体的哈希 —— 曾硬编码成 UNSIGNED-PAYLOAD，
  //    被 AWS 官方向量 `TestPresignRequest` 抓出（P5-D08）。
  const std::string canonical_request =
      request.method + "\n" + SigV4EncodePath(request.path) + "\n" + CanonicalQuery(query) +
      "\n" + canonical_headers.block + "\n" + canonical_headers.names + "\n" +
      std::string(request.payload_sha256_hex);
  const std::string string_to_sign = std::string(kAlgorithm) + "\n" + std::string(amz_date) +
                                     "\n" + Scope(amz_date) + "\n" +
                                     crypto::Sha256Hex(canonical_request);

  const auto signing_key =
      crypto::DeriveSigningKey(credentials_.secret_key, std::string(amz_date.substr(0, 8)),
                               options_.region, options_.service);
  const std::string signature = HmacHex(signing_key, string_to_sign);

  query.emplace_back("X-Amz-Signature", signature);
  std::string url = options_.scheme + "://" + request.host + SigV4EncodePath(request.path);
  url += "?" + CanonicalQuery(query);
  return url;
}

SigV4Request MakeSigV4Request(const SigV4Options& options, std::string_view method,
                              std::string_view bucket, std::string_view key,
                              std::vector<std::pair<std::string, std::string>> query,
                              std::vector<std::pair<std::string, std::string>> extra_headers,
                              std::string_view payload_sha256_hex, std::string_view amz_date) {
  SigV4Request request;
  request.method = std::string(method);
  request.host = options.force_path_style || bucket.empty()
                     ? options.endpoint
                     : std::string(bucket) + "." + options.endpoint;
  if (options.force_path_style && !bucket.empty()) {
    request.path = "/" + std::string(bucket);
    if (!key.empty()) request.path += "/" + std::string(key);
  } else {
    request.path = "/" + std::string(key);
  }
  request.query = std::move(query);
  request.payload_sha256_hex = std::string(payload_sha256_hex);

  request.headers.emplace_back("host", request.host);
  if (!amz_date.empty()) request.headers.emplace_back("x-amz-date", std::string(amz_date));
  request.headers.emplace_back("x-amz-content-sha256", std::string(payload_sha256_hex));
  for (auto& [name, value] : extra_headers) {
    request.headers.emplace_back(Lower(name), std::move(value));
  }
  return request;
}

}  // namespace fss::infra
