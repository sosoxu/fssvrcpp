#include "common/crypto/crypto.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>

namespace fss::crypto {
namespace {

// OpenSSL 3.x 的非弃用路径：EVP_Digest + EVP_PKEY_HMAC。
// （SHA256()/HMAC() 在 3.0 已弃用；虽然可用，但避免依赖弃用 API。）

struct MdCtxDeleter {
  void operator()(EVP_MD_CTX* p) const { EVP_MD_CTX_free(p); }
};
struct PkeyDeleter {
  void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
};
struct MdDeleter {
  void operator()(EVP_MD* p) const { EVP_MD_free(p); }
};
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;

const EVP_MD* Sha256Md() {
  static MdDeleter d;
  static std::unique_ptr<EVP_MD, MdDeleter> md(EVP_MD_fetch(nullptr, "SHA256", nullptr), d);
  return md.get() != nullptr ? md.get() : EVP_sha256();
}

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr char kB64Std[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kB64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string Base64Impl(std::span<const std::uint8_t> data, const char* alphabet, bool pad) {
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 3 <= data.size(); i += 3) {
    const std::uint32_t v = (std::uint32_t(data[i]) << 16) | (std::uint32_t(data[i + 1]) << 8) |
                            std::uint32_t(data[i + 2]);
    out.push_back(alphabet[(v >> 18) & 0x3F]);
    out.push_back(alphabet[(v >> 12) & 0x3F]);
    out.push_back(alphabet[(v >> 6) & 0x3F]);
    out.push_back(alphabet[v & 0x3F]);
  }
  const std::size_t rem = data.size() - i;
  if (rem == 1) {
    const std::uint32_t v = std::uint32_t(data[i]) << 16;
    out.push_back(alphabet[(v >> 18) & 0x3F]);
    out.push_back(alphabet[(v >> 12) & 0x3F]);
    if (pad) out.append("==");
  } else if (rem == 2) {
    const std::uint32_t v = (std::uint32_t(data[i]) << 16) | (std::uint32_t(data[i + 1]) << 8);
    out.push_back(alphabet[(v >> 18) & 0x3F]);
    out.push_back(alphabet[(v >> 12) & 0x3F]);
    out.push_back(alphabet[(v >> 6) & 0x3F]);
    if (pad) out.push_back('=');
  }
  return out;
}

int B64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

}  // namespace

// -----------------------------------------------------------------------------
Digest256 Sha256(std::span<const std::uint8_t> data) {
  Digest256 out{};
  unsigned int len = 0;
  if (EVP_Digest(data.data(), data.size(), out.data(), &len, Sha256Md(), nullptr) != 1 ||
      len != kSha256Bytes) {
    // 只可能在极端资源耗尽时发生；返回零摘要并由调用方的上层处理，
    // 但绝不能静默产生"看起来正确"的结果 —— 因此抛异常（L1 内部，不跨越层边界）。
    throw std::runtime_error("fss::crypto::Sha256: EVP_Digest failed");
  }
  return out;
}

std::string ToHex(std::span<const std::uint8_t> bytes) {
  std::string out;
  out.reserve(bytes.size() * 2);
  for (std::uint8_t b : bytes) {
    out.push_back(kHexDigits[b >> 4]);
    out.push_back(kHexDigits[b & 0x0F]);
  }
  return out;
}

std::string Sha256Hex(std::string_view data) { return ToHex(Sha256(data)); }

// -----------------------------------------------------------------------------
//  增量哈希
// -----------------------------------------------------------------------------
//  实现细节：把 `EVP_MD_CTX*` 存成 `void*`（头文件不引入 OpenSSL 类型）。
//  构造失败时 ctx_ 为空，Update 变成空操作、Final 返回全零摘要 —— 与"不抛异常
//  跨越层边界"的约定一致（调用方拿到的只是错误的校验和，不会崩溃）。
Sha256Hasher::Sha256Hasher() : ctx_(EVP_MD_CTX_new()) {
  if (ctx_ != nullptr) {
    EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(ctx_), Sha256Md(), nullptr);
  }
}

Sha256Hasher::~Sha256Hasher() {
  if (ctx_ != nullptr) EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_));
}

Sha256Hasher::Sha256Hasher(Sha256Hasher&& other) noexcept : ctx_(other.ctx_) {
  other.ctx_ = nullptr;
}

Sha256Hasher& Sha256Hasher::operator=(Sha256Hasher&& other) noexcept {
  if (this != &other) {
    if (ctx_ != nullptr) EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_));
    ctx_ = other.ctx_;
    other.ctx_ = nullptr;
  }
  return *this;
}

void Sha256Hasher::Update(std::span<const std::uint8_t> data) {
  if (ctx_ == nullptr || data.empty()) return;
  EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(ctx_), data.data(), data.size());
}

void Sha256Hasher::Update(std::string_view data) {
  Update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()),
                                       data.size()));
}

Digest256 Sha256Hasher::Final() {
  Digest256 out{};
  if (ctx_ == nullptr) return out;
  unsigned int length = 0;
  EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(ctx_), out.data(), &length);
  // 重置成初始状态：同一对象可继续用于下一段内容
  EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(ctx_), Sha256Md(), nullptr);
  return out;
}

std::string Sha256Hasher::HexDigest() { return ToHex(Final()); }

void Sha256Hasher::Reset() {
  if (ctx_ == nullptr) return;
  EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(ctx_), Sha256Md(), nullptr);
}

// -----------------------------------------------------------------------------
Digest256 HmacSha256(std::span<const std::uint8_t> key,
                     std::span<const std::uint8_t> data) {
  // 空 key 在 EVP_PKEY_new_raw_private_key 下是合法的（会走 HMAC 的零填充路径）
  PkeyPtr pkey(EVP_PKEY_new_raw_private_key(EVP_PKEY_HMAC, nullptr, key.data(), key.size()));
  if (!pkey) throw std::runtime_error("fss::crypto::HmacSha256: EVP_PKEY_new_raw_private_key failed");

  MdCtxPtr ctx(EVP_MD_CTX_new());
  if (!ctx) throw std::runtime_error("fss::crypto::HmacSha256: EVP_MD_CTX_new failed");

  if (EVP_DigestSignInit(ctx.get(), nullptr, Sha256Md(), nullptr, pkey.get()) != 1) {
    throw std::runtime_error("fss::crypto::HmacSha256: EVP_DigestSignInit failed");
  }
  Digest256 out{};
  std::size_t out_len = out.size();
  if (EVP_DigestSign(ctx.get(), out.data(), &out_len, data.data(), data.size()) != 1 ||
      out_len != kSha256Bytes) {
    throw std::runtime_error("fss::crypto::HmacSha256: EVP_DigestSign failed");
  }
  return out;
}

Digest256 DeriveSigningKey(std::string_view secret,
                           std::string_view date_yyyymmdd,
                           std::string_view region,
                           std::string_view service) {
  // ★ 每一轮都以【原始 32 字节摘要】作为下一轮的 key（不是 hex 字符串）。
  //   这是阶段 0 抓到的真实缺陷（D-01），并有对齐 AWS 官方向量的回归测试。
  const std::string k0 = "AWS4" + std::string(secret);
  const auto as_span = [](const Digest256& d) {
    return std::span<const std::uint8_t>(d.data(), d.size());
  };
  const auto key_of = [](const Digest256& d) {
    return std::string(reinterpret_cast<const char*>(d.data()), d.size());
  };

  const Digest256 k_date = HmacSha256(k0, date_yyyymmdd);
  const Digest256 k_region = HmacSha256(key_of(k_date), region);
  const Digest256 k_service = HmacSha256(key_of(k_region), service);
  const Digest256 k_signing = HmacSha256(key_of(k_service), kSigV4Terminator);
  (void)as_span;
  return k_signing;
}

// -----------------------------------------------------------------------------
std::string Base64UrlEncode(std::span<const std::uint8_t> data) {
  return Base64Impl(data, kB64Url, /*pad=*/false);
}

std::string Base64Encode(std::span<const std::uint8_t> data) {
  return Base64Impl(data, kB64Std, /*pad=*/true);
}

std::optional<std::vector<std::uint8_t>> Base64UrlDecode(std::string_view text) {
  // 去掉可能的 padding（base64url 约定不带，但容忍输入带 '='）
  while (!text.empty() && text.back() == '=') text.remove_suffix(1);
  std::vector<std::uint8_t> out;
  out.reserve(text.size() * 3 / 4);
  std::uint32_t acc = 0;
  int bits = 0;
  for (char c : text) {
    const int v = B64Value(c);
    if (v < 0) return std::nullopt;  // 非法字符 → 明确失败，不静默截断
    acc = (acc << 6) | std::uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(std::uint8_t((acc >> bits) & 0xFF));
    }
  }
  // 剩余位必须是 0 填充，否则输入被篡改
  if (bits > 0 && ((acc & ((1u << bits) - 1u)) != 0)) return std::nullopt;
  return out;
}

// -----------------------------------------------------------------------------
bool ConstantTimeEquals(std::span<const std::uint8_t> a,
                        std::span<const std::uint8_t> b) {
  if (a.size() != b.size()) return false;
  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
  }
  // 用 volatile 防止编译器把循环优化成提前返回
  volatile std::uint8_t sink = diff;
  return sink == 0;
}

bool ConstantTimeEquals(std::string_view a, std::string_view b) {
  return ConstantTimeEquals(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(a.data()), a.size()),
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(b.data()), b.size()));
}

// -----------------------------------------------------------------------------
std::vector<std::uint8_t> RandomBytes(std::size_t n) {
  std::vector<std::uint8_t> out(n);
  if (n > 0 && RAND_bytes(out.data(), static_cast<int>(n)) != 1) {
    throw std::runtime_error("fss::crypto::RandomBytes: RAND_bytes failed");
  }
  return out;
}

std::string RandomHex(std::size_t bytes) { return ToHex(RandomBytes(bytes)); }

}  // namespace fss::crypto
