// fss::crypto 单元测试。
// ★ 最重要的一组是 SigV4 密钥派生链（对齐 AWS 官方向量）—— 阶段 0 抓到的
//   "用 hex 字符串而不是原始摘要做链式 key" 缺陷就在这里被固化（见 phase0.md D-01）。
#include <catch2/catch.hpp>

#include "common/crypto/crypto.h"

#include <string>

using namespace std::string_literals;

TEST_CASE("SHA-256 对齐已知向量", "[phase1][crypto]") {
  // 空输入
  REQUIRE(fss::crypto::Sha256Hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  // "abc"
  REQUIRE(fss::crypto::Sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // 一个 448 bit 的输入（跨块边界）
  REQUIRE(fss::crypto::Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("★ SigV4 密钥派生链对齐 AWS 官方向量（原始字节链式派生）",
          "[phase1][crypto][sigv4]") {
  // AWS 文档 "Deriving the signing key" 示例
  const std::string secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
  const auto k = fss::crypto::DeriveSigningKey(secret, "20120215", "us-east-1", "iam");
  REQUIRE(fss::crypto::ToHex(k) ==
          "f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d");
  // 期望值是 32 字节原始摘要，不是 64 字符 hex
  REQUIRE(k.size() == 32);
}

TEST_CASE("HMAC-SHA256 对齐 RFC 4231 测试向量", "[phase1][crypto]") {
  // RFC 4231 Test Case 1：key = 20 × 0x0b, data = "Hi There"
  const std::string key(20, '\x0b');
  const auto mac = fss::crypto::HmacSha256(key, "Hi There");
  REQUIRE(fss::crypto::ToHex(mac) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

  // Test Case 2：key = "Jefe", data = "what do ya want for nothing?"
  const auto mac2 = fss::crypto::HmacSha256("Jefe", "what do ya want for nothing?");
  REQUIRE(fss::crypto::ToHex(mac2) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("base64url 编解码：无 padding、且能检出非法输入", "[phase1][crypto]") {
  // RFC 4648 向量（base64url 去掉 padding）
  auto enc = [](const std::string& s) {
    return fss::crypto::Base64UrlEncode(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
  };
  REQUIRE(enc("") == "");
  REQUIRE(enc("f") == "Zg");
  REQUIRE(enc("fo") == "Zm8");
  REQUIRE(enc("foo") == "Zm9v");
  REQUIRE(enc("foob") == "Zm9vYg");
  REQUIRE(enc("fooba") == "Zm9vYmE");
  REQUIRE(enc("foobar") == "Zm9vYmFy");
  // URL 安全字符：0xFB 0xFF 会编码出 '-' 与 '_'
  const std::uint8_t raw[] = {0xFB, 0xFF, 0xBF};
  REQUIRE(fss::crypto::Base64UrlEncode(std::span<const std::uint8_t>(raw, 3)) == "-_-_");

  // 往返
  for (const std::string& s : {""s, "f"s, "fo"s, "foo"s, "foob"s, "fooba"s, "foobar"s,
                               "\x00\x01\x02\xfd\xfe\xff"s}) {
    auto dec = fss::crypto::Base64UrlDecode(enc(s));
    REQUIRE(dec.has_value());
    REQUIRE(std::string(dec->begin(), dec->end()) == s);
  }

  // 非法字符必须明确失败（不能静默截断）
  REQUIRE_FALSE(fss::crypto::Base64UrlDecode("Zm9v!").has_value());
  // 单字符输入：剩余位非零 → 属于被篡改的输入，必须明确失败
  REQUIRE_FALSE(fss::crypto::Base64UrlDecode("a").has_value());
}

TEST_CASE("常量时间比较：相等/首字节/中间/末字节/长度不同", "[phase1][crypto]") {
  REQUIRE(fss::crypto::ConstantTimeEquals("abc", "abc"));
  REQUIRE_FALSE(fss::crypto::ConstantTimeEquals("abc", "abd"));   // 末字节
  REQUIRE_FALSE(fss::crypto::ConstantTimeEquals("abc", "xbc"));   // 首字节
  REQUIRE_FALSE(fss::crypto::ConstantTimeEquals("abc", "axc"));   // 中间
  REQUIRE_FALSE(fss::crypto::ConstantTimeEquals("abc", "ab"));    // 长度不同
  REQUIRE_FALSE(fss::crypto::ConstantTimeEquals("", "a"));
  REQUIRE(fss::crypto::ConstantTimeEquals("", ""));
  REQUIRE(fss::crypto::ConstantTimeEquals(std::string(4096, 'k'), std::string(4096, 'k')));
}

TEST_CASE("随机数：长度正确且不是常量", "[phase1][crypto]") {
  auto a = fss::crypto::RandomBytes(32);
  auto b = fss::crypto::RandomBytes(32);
  REQUIRE(a.size() == 32);
  REQUIRE(b.size() == 32);
  REQUIRE(a != b);  // 概率性断言：2^-256 的碰撞概率，可接受

  auto h = fss::crypto::RandomHex(8);
  REQUIRE(h.size() == 16);
  REQUIRE(h.find_first_not_of("0123456789abcdef") == std::string::npos);

  REQUIRE(fss::crypto::RandomBytes(0).empty());
}

TEST_CASE("增量 SHA-256：与一次性计算在任意分块下一致（R1：必须配参照实现）",
          "[phase1][crypto]") {
  //  参照实现是一次性 `Sha256Hex`；增量必须在任何分块方式下都等于它。
  //  （"边写边算"是 C3.4 流式 put 的前提，错了就会静默写入错误的校验和。）
  const std::string data = "The quick brown fox jumps over the lazy dog. 0123456789";
  const std::string expected = fss::crypto::Sha256Hex(data);

  //  分块大小覆盖：空、1、2、3、7、64、大于数据、恰好等于数据
  for (const std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7},
                                  std::size_t{64}, std::size_t{1000}, data.size()}) {
    fss::crypto::Sha256Hasher hasher;
    for (std::size_t i = 0; i < data.size(); i += chunk) {
      hasher.Update(std::string_view(data).substr(i, chunk));
    }
    INFO("chunk=" << chunk);
    REQUIRE(hasher.HexDigest() == expected);
  }

  //  空输入
  fss::crypto::Sha256Hasher empty;
  REQUIRE(empty.HexDigest() == fss::crypto::Sha256Hex(""));

  //  Final 之后自动重置：同一对象可以直接算下一段
  fss::crypto::Sha256Hasher reusable;
  reusable.Update("hello ");
  REQUIRE(reusable.HexDigest() == fss::crypto::Sha256Hex("hello "));
  reusable.Update("world");
  REQUIRE(reusable.HexDigest() == fss::crypto::Sha256Hex("world"));

  //  显式 Reset 与"新对象"等价
  fss::crypto::Sha256Hasher reset_me;
  reset_me.Update("junk");
  reset_me.Reset();
  reset_me.Update(data);
  REQUIRE(reset_me.HexDigest() == expected);
}
