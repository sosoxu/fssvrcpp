// =============================================================================
//  test_checksum.cpp —— C6.4 的 L1 基座：算法解析 / 规范名 / hex 结构校验 / 增量摘要
// =============================================================================
//  为什么这些用例挂在 **phase6** 而不是 phase1
//    `crypto::ChecksumAlgorithm` / `Hasher` / `IsHexDigestOf` 是**为 C6.4 新增**的 L1 能力。
//    把它们与 C6.4 放进同一个门槛，才能让"判据 ↔ 证据"一一对应（phase1 的
//    `test_crypto.cpp` 仍然覆盖 SHA-256 一次性/增量、HMAC/SigV4、base64url 等）。
//
//  ★ 期望值的来源（R18：引用的常量必须有参照实现比对）
//    RFC 1321（MD5）/ RFC 3174（SHA-1）/ FIPS 180-4（SHA-256）的**公开测试向量**，
//    并且已用独立的 `openssl dgst -sha256|-sha1|-md5` 复算确认。
//    没有这一步，"实现与期望值一起错"是查不出来的。
//
//  ★ 为什么 MD5/SHA-1 也要真的算
//    校验和**跟随驱动**：上游 Azure 的 `storageUtil.getChecksum` 返回的是 **MD5**
//    （`docs/01-osdu-research.md` §2.1 第 7 条），记录里就得写 `MD5` 且值要真算得出来。
//    `ParseChecksumAlgorithm` 不认识的名字 → `nullopt` → 调用方必须回退到**流式回算 SHA-256**，
//    绝不能把一个不认识的算法名（或 `ETAG` 这种非 hex 值）写进记录（"看起来有值"的假象）。
// =============================================================================
#include <catch2/catch.hpp>

#include "common/crypto/crypto.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using fss::crypto::CanonicalChecksumName;
using fss::crypto::ChecksumAlgorithm;
using fss::crypto::ChecksumHexLength;
using fss::crypto::Hasher;
using fss::crypto::IsHexDigestOf;
using fss::crypto::ParseChecksumAlgorithm;

TEST_CASE("C6.4 算法名解析：大小写与分隔符不敏感，未知算法必须返回 nullopt",
          "[phase6][unit][c6.4]") {
  //  ★ 接受的写法（契约 §2.6 第 7 步的判定表）
  const std::vector<std::pair<std::string, ChecksumAlgorithm>> accepted = {
      {"SHA-256", ChecksumAlgorithm::kSha256}, {"SHA256", ChecksumAlgorithm::kSha256},
      {"sha256", ChecksumAlgorithm::kSha256},  {"sha_256", ChecksumAlgorithm::kSha256},
      {"Sha-256", ChecksumAlgorithm::kSha256}, {"SHA-1", ChecksumAlgorithm::kSha1},
      {"sha1", ChecksumAlgorithm::kSha1},      {"MD5", ChecksumAlgorithm::kMd5},
      {"md5", ChecksumAlgorithm::kMd5},
  };
  for (const auto& [name, expected] : accepted) {
    INFO("算法名：" << name);
    const auto parsed = ParseChecksumAlgorithm(name);
    REQUIRE(parsed.has_value());
    REQUIRE(*parsed == expected);
  }

  //  ★ 拒绝的写法：返回 nullopt，让调用方回退到**流式回算 SHA-256**（而不是把不认识的名字写进记录）
  const std::vector<std::string> rejected = {
      "",           // 空
      "CRC32",      // 契约里出现过客户端算法名，但不是我们支持的集合
      "SHA-512",    // 长度不同的 SHA-2
      "SHA3-256",   // 不同族
      "sha-256x",   // 多了字符
      "ETAG",       // S3 的 ETag 不是可比较的 hex 摘要
      "adler32",
  };
  for (const auto& name : rejected) {
    INFO("算法名：" << name);
    REQUIRE_FALSE(ParseChecksumAlgorithm(name).has_value());
  }
}

TEST_CASE("C6.4 规范名与 hex 长度必须自洽（写回记录用的是规范名）", "[phase6][unit][c6.4]") {
  struct Row {
    ChecksumAlgorithm algorithm;
    const char* canonical;
    std::size_t hex_length;
  };
  const std::vector<Row> rows = {
      {ChecksumAlgorithm::kSha256, "SHA256", 64},
      {ChecksumAlgorithm::kSha1, "SHA1", 40},
      {ChecksumAlgorithm::kMd5, "MD5", 32},
  };
  for (const auto& row : rows) {
    INFO("规范名：" << row.canonical);
    REQUIRE(std::string(CanonicalChecksumName(row.algorithm)) == row.canonical);
    REQUIRE(ChecksumHexLength(row.algorithm) == row.hex_length);
    //  自洽：规范名必须能被解析回同一个算法（否则写回记录后会读不回来）
    const auto round_trip = ParseChecksumAlgorithm(CanonicalChecksumName(row.algorithm));
    REQUIRE(round_trip.has_value());
    REQUIRE(*round_trip == row.algorithm);
  }
}

TEST_CASE("C6.4 hex 结构校验：长度与字符集（结构先于取值）", "[phase6][unit][c6.4]") {
  const std::string sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

  //  正例：真实摘要必须通过（R16：只断言"非法被拒"无法区分"校验正确"与"校验恒真"）
  REQUIRE(IsHexDigestOf(sha256, ChecksumAlgorithm::kSha256));
  REQUIRE(IsHexDigestOf(std::string(64, 'A'), ChecksumAlgorithm::kSha256));  // 大写 hex 合法
  REQUIRE(IsHexDigestOf(std::string(40, '0'), ChecksumAlgorithm::kSha1));
  REQUIRE(IsHexDigestOf(std::string(32, '9'), ChecksumAlgorithm::kMd5));

  //  反例一：长度不符（32 个字符声明 SHA-256，即 MD5 的长度）
  REQUIRE_FALSE(IsHexDigestOf(std::string(32, 'a'), ChecksumAlgorithm::kSha256));
  REQUIRE_FALSE(IsHexDigestOf(std::string(64, 'a'), ChecksumAlgorithm::kMd5));
  REQUIRE_FALSE(IsHexDigestOf(std::string(63, 'a'), ChecksumAlgorithm::kSha256));
  REQUIRE_FALSE(IsHexDigestOf(std::string(65, 'a'), ChecksumAlgorithm::kSha256));
  REQUIRE_FALSE(IsHexDigestOf("", ChecksumAlgorithm::kSha256));

  //  反例二：长度对但含非 hex 字符
  REQUIRE_FALSE(IsHexDigestOf(std::string(62, 'a') + "zz", ChecksumAlgorithm::kSha256));
  REQUIRE_FALSE(IsHexDigestOf(std::string(63, 'a') + " ", ChecksumAlgorithm::kSha256));
  REQUIRE_FALSE(IsHexDigestOf(std::string(63, 'a') + "g", ChecksumAlgorithm::kSha256));

  //  反例三：把 SHA-256 的摘要当 MD5 用（长度不符 → 必须拒，而不是"截断比较"）
  REQUIRE_FALSE(IsHexDigestOf(sha256, ChecksumAlgorithm::kMd5));
  REQUIRE_FALSE(IsHexDigestOf(sha256, ChecksumAlgorithm::kSha1));
}

TEST_CASE("C6.4 增量摘要对公开测试向量逐字节匹配（SHA-256 / SHA-1 / MD5）",
          "[phase6][unit][c6.4]") {
  struct Row {
    ChecksumAlgorithm algorithm;
    const char* input;
    const char* expected;
  };
  //  RFC 1321 §A.5 / RFC 3174 §7.3 / FIPS 180-4 的公开向量
  const std::vector<Row> rows = {
      {ChecksumAlgorithm::kSha256, "",
       "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {ChecksumAlgorithm::kSha256, "abc",
       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {ChecksumAlgorithm::kSha256, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
      {ChecksumAlgorithm::kSha1, "", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
      {ChecksumAlgorithm::kSha1, "abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
      {ChecksumAlgorithm::kSha1, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
      {ChecksumAlgorithm::kMd5, "", "d41d8cd98f00b204e9800998ecf8427e"},
      {ChecksumAlgorithm::kMd5, "abc", "900150983cd24fb0d6963f7d28e17f72"},
      {ChecksumAlgorithm::kMd5, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
       "d174ab98d277d9f5a5611c2c9f419d9f"},
  };
  for (const auto& row : rows) {
    INFO("算法 " << CanonicalChecksumName(row.algorithm) << " 输入长度 "
                 << std::string_view(row.input).size());
    Hasher hasher(row.algorithm);
    hasher.Update(row.input);
    const std::string digest = hasher.HexDigest();
    REQUIRE(digest == row.expected);
    //  自洽：真实摘要必须通过结构校验（否则实现里的长度/字符集判断会与它打架）
    REQUIRE(IsHexDigestOf(digest, row.algorithm));
    REQUIRE(digest.size() == ChecksumHexLength(row.algorithm));
  }
}

TEST_CASE("C6.4 增量摘要在任意分块下一致：一百万个 'a'（RFC 3174 §7.3 向量 #3）",
          "[phase6][unit][c6.4]") {
  //  ★ 一个**非 2 的幂**的总长度（1,000,000）与多种分块：只要实现里少喂/多喂一个字节，
  //    或者把"分块边界"当成"结束"，结果就会偏离权威向量。
  struct Row {
    ChecksumAlgorithm algorithm;
    const char* expected;
  };
  const std::vector<Row> rows = {
      {ChecksumAlgorithm::kSha256,
       "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
      {ChecksumAlgorithm::kSha1, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"},
      {ChecksumAlgorithm::kMd5, "7707d6ae4e027c70eea2a935c2296f21"},
  };
  const std::string block(4096, 'a');
  constexpr std::size_t kTotal = 1000000;

  for (const auto& row : rows) {
    //  分块刻意选"不整除 4096 也不整除 1e6"的值：跨界与尾部不足一块都会被覆盖
    for (const std::size_t chunk : {std::size_t{7}, std::size_t{64}, std::size_t{4096},
                                    std::size_t{65537}}) {
      INFO("算法 " << CanonicalChecksumName(row.algorithm) << " 分块 " << chunk);
      Hasher hasher(row.algorithm);
      std::size_t written = 0;
      while (written < kTotal) {
        const std::size_t take = std::min(chunk, kTotal - written);
        //  从固定缓冲里取 `take` 字节（间隔 feed，覆盖跨界与尾部不足一块）
        std::size_t left = take;
        while (left > 0) {
          const std::size_t piece = std::min(left, block.size());
          hasher.Update(std::string_view(block).substr(0, piece));
          left -= piece;
        }
        written += take;
      }
      REQUIRE(written == kTotal);
      REQUIRE(hasher.HexDigest() == row.expected);
    }
  }
}

TEST_CASE("C6.4 HexDigest 取完即重置：同一对象可继续算下一段", "[phase6][unit][c6.4]") {
  Hasher hasher(ChecksumAlgorithm::kSha1);
  hasher.Update("abc");
  REQUIRE(hasher.HexDigest() == "a9993e364706816aba3e25717850c26c9cd0d89d");
  //  上一段落**不应**污染下一段（与 `Sha256Hasher` 的语义一致）
  hasher.Update("abc");
  REQUIRE(hasher.HexDigest() == "a9993e364706816aba3e25717850c26c9cd0d89d");

  Hasher reset_me(ChecksumAlgorithm::kMd5);
  reset_me.Update("junk");
  reset_me.Reset();
  reset_me.Update("abc");
  REQUIRE(reset_me.HexDigest() == "900150983cd24fb0d6963f7d28e17f72");
  REQUIRE(reset_me.algorithm() == ChecksumAlgorithm::kMd5);
}
