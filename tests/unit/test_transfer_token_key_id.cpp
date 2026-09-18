// =============================================================================
//  C10.17（阶段 10 切片 5）：`self_signed.key_id` 的编码/解码边界
// =============================================================================
//  语义（`docs/operations.md` §1.2.5、`src/infra/transfer/transfer_token.h`）：
//    · 配置 `key_id` 为空 → 载荷**不含** `key_id`，解码侧**不校验**
//      （既有测试夹具的用法，行为与接线前逐字一致）；
//    · 配置 `key_id` 非空 → `Encode` 把它写进**被签名的载荷**，`Decode` 在验签通过后
//      要求载荷里的 `key_id` 与配置**完全相等**；**缺失**或不匹配 → `kUnauthenticated`。
//
//  ★ 为什么"缺失"也必须拒绝：把"缺失"当成"匹配"，等价于"旧 token（不带 key_id）
//    永远继续可用"，绑定就成了恒真命题 —— 正是 AGENTS R16 与 §4.3 点名的那类形状。
//  ★ 这里只做**标识绑定**，不是密钥轮换：签名密钥仍是 `secret`（多 key 并存未交付）。
// =============================================================================
#include <catch2/catch.hpp>

#include "common/crypto/crypto.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/transfer/transfer_token.h"

#include <string>

using fss::domain::StorageZone;
using fss::domain::TransferToken;
using fss::infra::HmacTransferTokenCodec;

namespace {

struct ParsedUrl {
  std::string token;
  std::string expires;
  std::string signature;
};

ParsedUrl ParseTransferUrl(const std::string& url) {
  static const std::string marker = "/v1/transfer/";
  const auto marker_pos = url.find(marker);
  REQUIRE(marker_pos != std::string::npos);
  const std::string rest = url.substr(marker_pos + marker.size());
  const auto query_pos = rest.find('?');
  REQUIRE(query_pos != std::string::npos);

  ParsedUrl parsed;
  parsed.token = rest.substr(0, query_pos);
  const std::string query = rest.substr(query_pos + 1);
  std::size_t start = 0;
  while (start <= query.size()) {
    const auto amp = query.find('&', start);
    const std::string pair =
        query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
    const auto eq = pair.find('=');
    if (eq != std::string::npos) {
      const std::string key = pair.substr(0, eq);
      const std::string value = pair.substr(eq + 1);
      if (key == "exp") parsed.expires = value;
      if (key == "sig") parsed.signature = value;
    }
    if (amp == std::string::npos) break;
    start = amp + 1;
  }
  return parsed;
}

TransferToken MakeToken(const fss::ManualClock& clock) {
  TransferToken token;
  token.partition = "opendes";
  token.file_id = "file-1";
  token.container = "opendes-staging";
  token.object_key = "u/2021/k1/file-1";
  token.zone = StorageZone::kStaging;
  token.op = "put";
  token.expires_at_epoch_seconds = clock.NowEpochSeconds() + 3600;
  return token;
}

//  载荷原文（不验签；只用于断言 `key_id` 是否真的进了**被签名的载荷**）
std::string PayloadOf(const std::string& token) {
  const auto bytes = fss::crypto::Base64UrlDecode(token);
  REQUIRE(bytes.has_value());
  return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

}  // namespace

TEST_CASE("★ C10.17 key_id 为空：载荷不含 key_id，解码侧不校验（接线前逐字一致）",
          "[phase10][transfer][c10.17]") {
  fss::ManualClock clock{1700000000};
  HmacTransferTokenCodec codec{"c10-17-secret", clock};  // 第 3 参数默认空

  const auto url = codec.Encode(MakeToken(clock), "https://self.invalid");
  REQUIRE(url.ok());
  const auto parsed = ParseTransferUrl(url.value());
  REQUIRE(PayloadOf(parsed.token).find("key_id") == std::string::npos);

  const auto decoded = codec.Decode(parsed.token, parsed.expires, parsed.signature);
  REQUIRE(decoded.ok());
  REQUIRE(decoded.value().key_id.empty());
}

TEST_CASE("★ C10.17 key_id 非空：写进被签名载荷，同配置解码回读一致（R16 正例）",
          "[phase10][transfer][c10.17]") {
  fss::ManualClock clock{1700000000};
  HmacTransferTokenCodec codec{"c10-17-secret", clock, "k1"};

  const auto url = codec.Encode(MakeToken(clock), "https://self.invalid");
  REQUIRE(url.ok());
  const auto parsed = ParseTransferUrl(url.value());
  REQUIRE(PayloadOf(parsed.token).find("\"key_id\":\"k1\"") != std::string::npos);

  const auto decoded = codec.Decode(parsed.token, parsed.expires, parsed.signature);
  REQUIRE(decoded.ok());
  REQUIRE(decoded.value().key_id == "k1");
}

TEST_CASE("★ C10.17 key_id 不匹配（k1 签发 / k2 解码）→ kUnauthenticated",
          "[phase10][transfer][c10.17]") {
  fss::ManualClock clock{1700000000};
  //  同一个签名密钥（否则拒绝可能只是"签名不符"，而不是 key_id 判定）
  HmacTransferTokenCodec signer{"shared-secret", clock, "k1"};
  HmacTransferTokenCodec verifier{"shared-secret", clock, "k2"};

  const auto url = signer.Encode(MakeToken(clock), "https://self.invalid");
  REQUIRE(url.ok());
  const auto parsed = ParseTransferUrl(url.value());

  //  自证前提：同一份载荷用 k1 配置解码必须成功（证明"拒绝来自 key_id 判定"）
  REQUIRE(signer.Decode(parsed.token, parsed.expires, parsed.signature).ok());

  const auto decoded = verifier.Decode(parsed.token, parsed.expires, parsed.signature);
  REQUIRE_FALSE(decoded.ok());
  REQUIRE(decoded.error().kind() == fss::ErrorKind::kUnauthenticated);
  REQUIRE(decoded.error().message().find("key_id") != std::string::npos);
}

TEST_CASE("★ C10.17 载荷缺 key_id 而配置非空 → kUnauthenticated（缺失 ≠ 匹配）",
          "[phase10][transfer][c10.17]") {
  fss::ManualClock clock{1700000000};
  HmacTransferTokenCodec unbound{"shared-secret", clock};      // 不写 key_id
  HmacTransferTokenCodec bound{"shared-secret", clock, "k1"};  // 要求 key_id=k1

  const auto url = unbound.Encode(MakeToken(clock), "https://self.invalid");
  REQUIRE(url.ok());
  const auto parsed = ParseTransferUrl(url.value());
  REQUIRE(PayloadOf(parsed.token).find("key_id") == std::string::npos);

  const auto decoded = bound.Decode(parsed.token, parsed.expires, parsed.signature);
  REQUIRE_FALSE(decoded.ok());
  REQUIRE(decoded.error().kind() == fss::ErrorKind::kUnauthenticated);
  REQUIRE(decoded.error().message().find("key_id") != std::string::npos);
}
