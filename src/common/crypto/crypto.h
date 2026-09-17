// =============================================================================
//  fss::crypto —— 哈希 / HMAC / base64url / 常量时间比较（L1）
// =============================================================================
//
//  用途（docs/adr/ADR-003、ADR-008）：
//    * S3 SigV4 预签名（派生签名密钥 + 构造签名）
//    * 集中存储的自签传输 token（HMAC + 常量时间校验）
//    * 校验和计算（SHA-256；MD5/SHA-1 由 checksum 模块按 OSDU 需求另行提供）
//
//  ★ 关键教训（阶段 0 实测固化，见 docs/test-evidence/phase0.md D-01）
//    SigV4 的密钥派生是**链式**的，每一轮必须用**上一轮结果的原始 32 字节摘要**
//    作为 key —— 不是它的十六进制字符串。用 hex 字符串会得到错误签名，
//    且症状是"签名不匹配"，极难定位。因此：
//      - 内部一律用 std::array<uint8_t,32> 传递摘要
//      - 只有面向展示/比较时才转 hex
//    DeriveSigningKey() 就是这条链，并有对齐 AWS 官方向量的单元测试。
// =============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fss::crypto {

inline constexpr std::size_t kSha256Bytes = 32;
using Digest256 = std::array<std::uint8_t, kSha256Bytes>;

// -----------------------------------------------------------------------------
//  哈希
// -----------------------------------------------------------------------------
Digest256 Sha256(std::span<const std::uint8_t> data);
inline Digest256 Sha256(std::string_view data) {
  return Sha256(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

// 小写十六进制（用于 ETag、checksum、日志）
std::string ToHex(std::span<const std::uint8_t> bytes);
inline std::string ToHex(const Digest256& d) {
  return ToHex(std::span<const std::uint8_t>(d.data(), d.size()));
}
std::string Sha256Hex(std::string_view data);

// -----------------------------------------------------------------------------
//  增量哈希（流式大对象的校验和）
// -----------------------------------------------------------------------------
//  为什么必须有它：`put` 要"边收流边算校验和"。若只能对完整缓冲调用 `Sha256()`，
//  就不得不把整个对象读进内存 —— 直接违反 C1.3/C3.4 的"RSS 与对象大小无关"。
//
//  用法：`Update(...)` 多次 → `HexDigest()`（或 `Final()`）一次。
//  `Final()`/`HexDigest()` 之后哈希会重置为初始状态（同一对象可继续复用）。
class Sha256Hasher {
 public:
  Sha256Hasher();
  ~Sha256Hasher();
  Sha256Hasher(const Sha256Hasher&) = delete;
  Sha256Hasher& operator=(const Sha256Hasher&) = delete;
  Sha256Hasher(Sha256Hasher&& other) noexcept;
  Sha256Hasher& operator=(Sha256Hasher&& other) noexcept;

  void Update(std::string_view data);
  void Update(std::span<const std::uint8_t> data);
  Digest256 Final();
  std::string HexDigest();
  void Reset();

 private:
  void* ctx_ = nullptr;  // EVP_MD_CTX*（不把 OpenSSL 类型带进头文件）
};

// -----------------------------------------------------------------------------
//  HMAC-SHA256
// -----------------------------------------------------------------------------
// 返回**原始摘要**（32 字节）。链式派生必须用这个，不要用 hex 版本。
Digest256 HmacSha256(std::span<const std::uint8_t> key,
                     std::span<const std::uint8_t> data);
inline Digest256 HmacSha256(std::string_view key, std::string_view data) {
  return HmacSha256(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()),
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

// SigV4 密钥派生链：kDate → kRegion → kService → kSigning
//   secret 传原始 secret（本函数内部会加 "AWS4" 前缀）
Digest256 DeriveSigningKey(std::string_view secret,
                           std::string_view date_yyyymmdd,
                           std::string_view region,
                           std::string_view service);
constexpr std::string_view kSigV4Terminator = "aws4_request";

// -----------------------------------------------------------------------------
//  base64 / base64url
// -----------------------------------------------------------------------------
// base64url：使用 '-' 与 '_'，**不带** padding（SigV4 与 JWT 风格）
std::string Base64UrlEncode(std::span<const std::uint8_t> data);
inline std::string Base64UrlEncode(std::string_view data) {
  return Base64UrlEncode(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}
// 解析失败返回 std::nullopt（不要用异常跨越层边界）
std::optional<std::vector<std::uint8_t>> Base64UrlDecode(std::string_view text);

// 标准 base64（带 padding），用于需要标准形式的场景
std::string Base64Encode(std::span<const std::uint8_t> data);

// -----------------------------------------------------------------------------
//  常量时间比较（防止签名校验的时序侧信道）
// -----------------------------------------------------------------------------
// 长度不同直接返回 false（长度本身不是秘密）；长度相同时逐字节累积异或。
bool ConstantTimeEquals(std::string_view a, std::string_view b);
bool ConstantTimeEquals(std::span<const std::uint8_t> a,
                        std::span<const std::uint8_t> b);

// -----------------------------------------------------------------------------
//  随机数（nonce、临时密钥、UUID 熵源）
// -----------------------------------------------------------------------------
std::vector<std::uint8_t> RandomBytes(std::size_t n);
// 十六进制随机串（便于放进 URL / 日志）
std::string RandomHex(std::size_t bytes);

}  // namespace fss::crypto
