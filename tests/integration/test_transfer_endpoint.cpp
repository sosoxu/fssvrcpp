// =============================================================================
//  C3.7：自签传输 token 与 `/v1/transfer` 内核
// =============================================================================
//  判据（原文）："正常用法通过；篡改 `op`/`container`/`key`/`partition`/`exp` 各一例
//  **全部拒绝**；过期拒绝；`get` token 用于 `put` 拒绝；跨 partition 拒绝"。
//
//  因此本文件分三层断言：
//    ① codec：签名覆盖整个载荷（逐字段篡改都要被拒）、exp 参数与载荷一致、过期拒绝；
//    ② 内核：合法签名的**越权使用**（跨操作 / 跨租户）也要被拒；
//    ③ 端到端：签发 URL → 用内核 PUT 上传 → 再签发 GET → 取回内容一致。
// =============================================================================
#include <catch2/catch.hpp>

#include "fake_ports.h"

#include "common/crypto/crypto.h"
#include "common/json/json.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/blob/memory/memory_blob_store.h"
#include "infra/transfer/transfer_endpoint.h"
#include "infra/transfer/transfer_token.h"

#include <functional>
#include <memory>
#include <string>

using fss::domain::StorageZone;
using fss::domain::TransferToken;
using fss::infra::HmacTransferTokenCodec;
using fss::infra::TransferEndpoint;
using fss::infra::TransferRequest;

namespace {

struct ParsedUrl {
  std::string path;
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
  parsed.path = url.substr(0, marker_pos);
  parsed.token = rest.substr(0, query_pos);
  const std::string query = rest.substr(query_pos + 1);
  std::size_t start = 0;
  while (start <= query.size()) {
    const auto amp = query.find('&', start);
    const std::string pair = query.substr(start, amp == std::string::npos ? std::string::npos
                                                                          : amp - start);
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

//  攻击者模型：能改载荷、但**签不出新的签名**（不知道密钥）。
//  重新编码一个"改过某个字段"的 token，但沿用原来的 `exp`/`sig`。
std::string TamperToken(const std::string& token,
                        const std::function<void(fss::json::Value&)>& mutate) {
  const auto bytes = fss::crypto::Base64UrlDecode(token);
  REQUIRE(bytes.has_value());
  const std::string payload(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  auto parsed = fss::json::ParseObject(payload);
  REQUIRE(parsed.ok());
  fss::json::Value value = parsed.value();
  mutate(value);
  return fss::crypto::Base64UrlEncode(fss::json::Dump(value));
}

struct Fixture {
  fss::ManualClock clock{1700000000};
  fss::infra::InMemoryBlobStore blob{clock};
  fss::test::FakeBlobStoreFactory factory{blob};
  HmacTransferTokenCodec codec{"unit-test-secret", clock};
  TransferEndpoint endpoint{codec, factory};

  TransferToken MakeToken(std::string op, std::string partition = "opendes",
                          std::string container = "opendes-staging",
                          std::string key = "k/obj.bin",
                          std::int64_t ttl = 3600) const {
    TransferToken token;
    token.partition = std::move(partition);
    token.file_id = "file-1";
    token.container = std::move(container);
    token.object_key = std::move(key);
    token.zone = StorageZone::kStaging;
    token.op = std::move(op);
    token.expires_at_epoch_seconds = clock.NowEpochSeconds() + ttl;
    return token;
  }
};

}  // namespace

TEST_CASE("★ C3.7 URL 形态与往返：/v1/transfer/{token}?exp=&sig=", "[phase3][transfer][c3.7]") {
  Fixture fixture;
  const auto token = fixture.MakeToken("put");
  const auto url = fixture.codec.Encode(token, "https://self.invalid/");
  REQUIRE(url.ok());
  //  尾斜杠归一化 + 路径与查询参数名
  REQUIRE(url.value().rfind("https://self.invalid/v1/transfer/", 0) == 0);
  REQUIRE(url.value().find("?exp=") != std::string::npos);
  REQUIRE(url.value().find("&sig=") != std::string::npos);

  const auto parsed = ParseTransferUrl(url.value());
  REQUIRE(parsed.path == "https://self.invalid");
  REQUIRE(parsed.expires == std::to_string(token.expires_at_epoch_seconds));

  const auto decoded = fixture.codec.Decode(parsed.token, parsed.expires, parsed.signature);
  REQUIRE(decoded.ok());
  REQUIRE(decoded.value().partition == token.partition);
  REQUIRE(decoded.value().container == token.container);
  REQUIRE(decoded.value().object_key == token.object_key);
  REQUIRE(decoded.value().op == token.op);
  REQUIRE(decoded.value().zone == token.zone);
  REQUIRE(decoded.value().expires_at_epoch_seconds == token.expires_at_epoch_seconds);
}

TEST_CASE("★ C3.7 篡改载荷任一字段 → 签名不匹配 → 拒绝", "[phase3][transfer][c3.7]") {
  Fixture fixture;
  const auto token = fixture.MakeToken("put");
  const auto url = fixture.codec.Encode(token, "https://self.invalid").value();
  const auto parsed = ParseTransferUrl(url);

  const std::vector<std::pair<std::string, std::function<void(fss::json::Value&)>>> mutations = {
      {"op", [](fss::json::Value& v) { v["op"] = "get"; }},
      {"container", [](fss::json::Value& v) { v["container"] = "opendes-persistent"; }},
      {"object_key", [](fss::json::Value& v) { v["object_key"] = "k/other.bin"; }},
      {"partition", [](fss::json::Value& v) { v["partition"] = "other-tenant"; }},
      {"exp", [](fss::json::Value& v) { v["exp"] = 4102444800; }},  // 2100 年
      {"zone", [](fss::json::Value& v) { v["zone"] = "PERSISTENT"; }},
      {"file_id", [](fss::json::Value& v) { v["file_id"] = "forged"; }},
  };

  for (const auto& [field, mutate] : mutations) {
    const std::string tampered = TamperToken(parsed.token, mutate);
    INFO("篡改字段：" << field);
    const auto decoded = fixture.codec.Decode(tampered, parsed.expires, parsed.signature);
    REQUIRE_FALSE(decoded.ok());
    REQUIRE(decoded.error().kind() == fss::ErrorKind::kUnauthenticated);
  }

  //  篡改签名本身
  std::string bad_sig = parsed.signature;
  bad_sig[0] = bad_sig[0] == 'A' ? 'B' : 'A';
  const auto bad = fixture.codec.Decode(parsed.token, parsed.expires, bad_sig);
  REQUIRE_FALSE(bad.ok());
  REQUIRE(bad.error().kind() == fss::ErrorKind::kUnauthenticated);

  //  篡改 exp 参数（签名覆盖 token + exp，因此也必须被拒）
  const auto bad_exp = fixture.codec.Decode(parsed.token, "4102444800", parsed.signature);
  REQUIRE_FALSE(bad_exp.ok());
  REQUIRE(bad_exp.error().kind() == fss::ErrorKind::kUnauthenticated);
}

TEST_CASE("★ C3.7 过期拒绝（用注入时钟，不 sleep）", "[phase3][transfer][c3.7]") {
  Fixture fixture;
  const auto token = fixture.MakeToken("put", "opendes", "opendes-staging", "k/x", /*ttl=*/10);
  const auto url = fixture.codec.Encode(token, "https://self.invalid").value();
  const auto parsed = ParseTransferUrl(url);

  REQUIRE(fixture.codec.Decode(parsed.token, parsed.expires, parsed.signature).ok());

  fixture.clock.AdvanceSeconds(10);  // 恰好到点：按 `>= exp` 判定为过期
  const auto expired = fixture.codec.Decode(parsed.token, parsed.expires, parsed.signature);
  REQUIRE_FALSE(expired.ok());
  REQUIRE(expired.error().kind() == fss::ErrorKind::kUnauthenticated);
}

TEST_CASE("★ C3.7 内核：get token 用于 put / 跨 partition → 拒绝", "[phase3][transfer][c3.7]") {
  Fixture fixture;

  //  get token 收到 PUT 上
  {
    const auto get_token = fixture.MakeToken("get");
    const auto url = fixture.codec.Encode(get_token, "https://self.invalid").value();
    const auto parsed = ParseTransferUrl(url);
    TransferRequest request{parsed.token, parsed.expires, parsed.signature, "put", "opendes"};
    fss::bytes::StringSource body("payload");
    const auto result = fixture.endpoint.Put(request, body);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kPermissionDenied);
  }

  //  put token 用于跨租户
  {
    const auto put_token = fixture.MakeToken("put");
    const auto url = fixture.codec.Encode(put_token, "https://self.invalid").value();
    const auto parsed = ParseTransferUrl(url);
    TransferRequest request{parsed.token, parsed.expires, parsed.signature, "put", "other-tenant"};
    fss::bytes::StringSource body("payload");
    const auto result = fixture.endpoint.Put(request, body);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kPermissionDenied);
  }
}

TEST_CASE("★ C3.7 端到端：签发 PUT URL → 内核上传 → 签发 GET URL → 取回一致",
          "[phase3][transfer][c3.7]") {
  Fixture fixture;
  const std::string payload = "hello transfer dataplane";

  //  上传
  {
    const auto put_token = fixture.MakeToken("put");
    const auto url = fixture.codec.Encode(put_token, "https://self.invalid").value();
    const auto parsed = ParseTransferUrl(url);
    TransferRequest request{parsed.token, parsed.expires, parsed.signature, "put", "opendes"};
    fss::bytes::StringSource body(payload);
    const auto result = fixture.endpoint.Put(request, body);
    REQUIRE(result.ok());
  }

  //  下载（同一个对象，换成 get token）
  {
    const auto get_token = fixture.MakeToken("get");
    const auto url = fixture.codec.Encode(get_token, "https://self.invalid").value();
    const auto parsed = ParseTransferUrl(url);
    TransferRequest request{parsed.token, parsed.expires, parsed.signature, "get", "opendes"};
    fss::bytes::StringSink sink;
    const auto result = fixture.endpoint.Get(request, sink, fss::domain::ByteRange{});
    REQUIRE(result.ok());
    REQUIRE(sink.str() == payload);
  }

  //  get token 也不能去读别的对象（载荷里的 key 决定目标）
  {
    const auto other = fixture.MakeToken("get", "opendes", "opendes-staging", "k/other.bin");
    const auto url = fixture.codec.Encode(other, "https://self.invalid").value();
    const auto parsed = ParseTransferUrl(url);
    TransferRequest request{parsed.token, parsed.expires, parsed.signature, "get", "opendes"};
    fss::bytes::StringSink sink;
    const auto result = fixture.endpoint.Get(request, sink, fss::domain::ByteRange{});
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kNotFound);
  }
}

TEST_CASE("C3.7 签发参数校验与缺失参数", "[phase3][transfer][c3.7]") {
  Fixture fixture;
  //  op 非法 / 缺 container / exp 非正
  {
    auto token = fixture.MakeToken("delete");
    REQUIRE_FALSE(fixture.codec.Encode(token, "https://self.invalid").ok());
    token = fixture.MakeToken("put");
    token.container.clear();
    REQUIRE_FALSE(fixture.codec.Encode(token, "https://self.invalid").ok());
    token = fixture.MakeToken("put");
    token.expires_at_epoch_seconds = 0;
    REQUIRE_FALSE(fixture.codec.Encode(token, "https://self.invalid").ok());
  }
  //  Decode 缺 exp / sig
  {
    const auto url =
        fixture.codec.Encode(fixture.MakeToken("put"), "https://self.invalid").value();
    const auto parsed = ParseTransferUrl(url);
    REQUIRE_FALSE(fixture.codec.Decode(parsed.token, "", parsed.signature).ok());
    REQUIRE_FALSE(fixture.codec.Decode(parsed.token, parsed.expires, "").ok());
    REQUIRE_FALSE(fixture.codec.Decode("", parsed.expires, parsed.signature).ok());
  }
}
