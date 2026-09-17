// =============================================================================
//  P8 切片 1：本地 JWT 授权器（C8.3 + 租户绑定 + fail-closed）
// =============================================================================
//  为什么这些断言必须逐条写出来：JWT 校验的每个分支都是一个**安全边界**。
//  "整体能通过"不能说明任何事 —— 过期 token 能过、`alg=none` 能过、
//  验签密钥缺失时降级为放行……任何一种都是完整的鉴权绕过。
//
//  期望值全部由测试**自己签发**（`MintJwt`），不依赖被测代码的任何常量。
// =============================================================================
#include <catch2/catch.hpp>

#include "common/crypto/crypto.h"
#include "common/json/json.h"
#include "common/time/clock.h"
#include "infra/auth/local/local_jwt_authorizer.h"

#include <map>
#include <string>
#include <vector>

namespace {

using fss::infra::LocalJwtAuthorizer;
using fss::infra::LocalJwtOptions;

constexpr char kSecret[] = "test-hs256-secret";
constexpr std::int64_t kNow = 1700000000;

//  ---- 自己签发 token（HS256），完全不使用被测代码 ----
std::string MintJwt(const std::string& secret, const fss::json::Value& payload,
                    const std::string& algorithm = "HS256") {
  fss::json::Value header;
  header["alg"] = algorithm;
  header["typ"] = "JWT";
  const std::string header_b64 = fss::crypto::Base64UrlEncode(fss::json::Dump(header));
  const std::string payload_b64 = fss::crypto::Base64UrlEncode(fss::json::Dump(payload));
  const std::string signing_input = header_b64 + "." + payload_b64;
  const auto digest = fss::crypto::HmacSha256(secret, signing_input);
  return signing_input + "." + fss::crypto::Base64UrlEncode(digest);
}

//  默认载荷：一个合法的 editor token
fss::json::Value Payload(std::string partition = "opendes", std::string email = "editor@example.com") {
  fss::json::Value payload;
  payload["sub"] = "user-1";
  payload["email"] = email;
  payload["data-partition-id"] = partition;
  payload["roles"] = fss::json::Value::array({"service.file.editors", "service.file.viewers"});
  payload["exp"] = kNow + 3600;
  payload["iat"] = kNow - 10;
  return payload;
}

LocalJwtOptions Options() {
  LocalJwtOptions options;
  options.hmac_secret = kSecret;
  return options;
}

}  // namespace

TEST_CASE("★ C8.3 合法 token：角色判定 + 任意角色 + 无 Bearer 前缀也可", "[phase8][unit][c8.3]") {
  fss::ManualClock clock(kNow);
  LocalJwtAuthorizer authorizer(Options(), clock);
  const std::string token = MintJwt(kSecret, Payload());

  REQUIRE(authorizer.Ready());
  //  ★ 两种前缀形态都要接受（上游网关注入的与直连客户端给的都出现过）
  REQUIRE(authorizer.Authorize("service.file.editors", "opendes", token).ok());
  REQUIRE(authorizer.Authorize("service.file.editors", "opendes", "Bearer " + token).ok());
  REQUIRE(authorizer.Authorize("service.file.editors", "opendes", "bearer " + token).ok());

  //  声明解析：user_id / partition / 角色
  const auto claims = authorizer.Verify("Bearer " + token, "opendes");
  REQUIRE(claims.ok());
  REQUIRE(claims.value().user_id == "editor@example.com");
  REQUIRE(claims.value().partition == "opendes");
  REQUIRE(claims.value().roles.size() == 2);

  //  ---- 角色不足 → 403（固定语义，不是 401）----
  const auto denied = authorizer.Authorize("service.file.admin", "opendes", token);
  REQUIRE_FALSE(denied.ok());
  REQUIRE(denied.error().kind() == fss::ErrorKind::kPermissionDenied);

  //  ---- AuthorizeAny：任一命中即通过 ----
  const std::vector<std::string_view> any_admin = {"service.file.admin", "service.file.editors"};
  REQUIRE(authorizer.AuthorizeAny(any_admin, "opendes", token).ok());
  const std::vector<std::string_view> none = {"service.storage.creator"};
  const auto any_denied = authorizer.AuthorizeAny(none, "opendes", token);
  REQUIRE_FALSE(any_denied.ok());
  REQUIRE(any_denied.error().kind() == fss::ErrorKind::kPermissionDenied);
  //  ★ 空角色集合必须报"内部错误"，绝不能当成"无需角色 → 放行"
  const std::vector<std::string_view> empty_roles;
  const auto empty_denied = authorizer.AuthorizeAny(empty_roles, "opendes", token);
  REQUIRE_FALSE(empty_denied.ok());
  REQUIRE(empty_denied.error().kind() == fss::ErrorKind::kInternal);
}

TEST_CASE("★ C8.3 缺 token → 401（固定消息）；缺 partition 由**用例层**判定",
          "[phase8][unit][c8.3]") {
  fss::ManualClock clock(kNow);
  LocalJwtAuthorizer authorizer(Options(), clock);
  const std::string token = MintJwt(kSecret, Payload());

  const auto no_token = authorizer.Authorize("service.file.editors", "opendes", "");
  REQUIRE_FALSE(no_token.ok());
  REQUIRE(no_token.error().kind() == fss::ErrorKind::kUnauthenticated);
  REQUIRE(no_token.error().message() == "Missing authorization token");

  //  ★ 职责划分：`Missing partitionID` 由用例入口（`AuthorizeCaller` 的
  //    `require_partition`）产出 —— `revokeURL` 按契约 §1.2 不需要 partition，
  //    作者器若在这里一律拒绝，就会把合法的 revokeURL 判成 401。
  //    这一条在用例层由 `tests/unit/test_roles.cpp` 与 phase4 的契约用例覆盖。
  REQUIRE(authorizer.Authorize("service.file.editors", "", token).ok());
  //  没有请求 partition 时不比较租户（没有"声明的租户"可比）
  auto no_claim = Payload();
  no_claim.erase("data-partition-id");
  REQUIRE(authorizer.Authorize("service.file.editors", "", MintJwt(kSecret, no_claim)).ok());
}

TEST_CASE("★ C8.3 时间边界：过期 / 缺 exp / nbf 未到 / 允许的时钟偏移",
          "[phase8][unit][c8.3]") {
  fss::ManualClock clock(kNow);
  LocalJwtAuthorizer authorizer(Options(), clock);

  //  ① 已过期（超过 skew）
  auto expired = Payload();
  expired["exp"] = kNow - 60;
  const auto expired_result = authorizer.Authorize("service.file.editors", "opendes",
                                                   MintJwt(kSecret, expired));
  REQUIRE_FALSE(expired_result.ok());
  REQUIRE(expired_result.error().message().find("过期") != std::string::npos);

  //  ② 没有 `exp` = 永久有效 → 必须拒绝（fail-closed）
  auto no_exp = Payload();
  no_exp.erase("exp");
  const auto no_exp_result =
      authorizer.Authorize("service.file.editors", "opendes", MintJwt(kSecret, no_exp));
  REQUIRE_FALSE(no_exp_result.ok());
  REQUIRE(no_exp_result.error().message().find("exp") != std::string::npos);

  //  ③ `nbf` 还没到
  auto early = Payload();
  early["nbf"] = kNow + 300;
  REQUIRE_FALSE(authorizer.Authorize("service.file.editors", "opendes", MintJwt(kSecret, early)).ok());

  //  ④ 边界：`exp` 刚过 3 秒（skew=5）→ 仍然接受（时钟抖动容忍，C8.10）
  auto just_expired = Payload();
  just_expired["exp"] = kNow - 3;
  REQUIRE(authorizer.Authorize("service.file.editors", "opendes",
                               MintJwt(kSecret, just_expired)).ok());
  //  而过了 10 秒 → 拒绝
  auto expired_long = Payload();
  expired_long["exp"] = kNow - 10;
  REQUIRE_FALSE(
      authorizer.Authorize("service.file.editors", "opendes", MintJwt(kSecret, expired_long)).ok());
}

TEST_CASE("★ C8.3 iss / aud / 签名 / 算法 / 畸形 token → 全部 401", "[phase8][unit][c8.3]") {
  fss::ManualClock clock(kNow);
  auto options = Options();
  options.issuer = "https://issuer.example.com";
  options.audience = "file-service";
  LocalJwtAuthorizer authorizer(options, clock);

  auto payload = Payload();
  payload["iss"] = "https://issuer.example.com";
  payload["aud"] = "file-service";
  REQUIRE(authorizer.Authorize("service.file.editors", "opendes", MintJwt(kSecret, payload)).ok());

  //  `aud` 是数组形态也必须接受（RFC 7519 §4.1.3）
  auto aud_array = payload;
  aud_array["aud"] = fss::json::Value::array({"other", "file-service"});
  REQUIRE(authorizer.Authorize("service.file.editors", "opendes", MintJwt(kSecret, aud_array)).ok());

  auto wrong_iss = payload;
  wrong_iss["iss"] = "https://evil.example.com";
  REQUIRE_FALSE(authorizer.Authorize("service.file.editors", "opendes",
                                     MintJwt(kSecret, wrong_iss)).ok());

  auto wrong_aud = payload;
  wrong_aud["aud"] = "another-service";
  REQUIRE_FALSE(authorizer.Authorize("service.file.editors", "opendes",
                                     MintJwt(kSecret, wrong_aud)).ok());

  //  ---- 签名：换密钥签的 token 必须失败 ----
  REQUIRE_FALSE(authorizer.Authorize("service.file.editors", "opendes",
                                     MintJwt("another-secret", payload)).ok());

  //  ---- 算法：`alg=none` 是最经典的绕过手法，必须拒绝 ----
  REQUIRE_FALSE(authorizer.Authorize("service.file.editors", "opendes",
                                     MintJwt(kSecret, payload, "none")).ok());
  REQUIRE_FALSE(authorizer.Authorize("service.file.editors", "opendes",
                                     MintJwt(kSecret, payload, "RS256")).ok());

  //  ---- 畸形形态 ----
  const std::vector<std::string> malformed = {
      "not-a-jwt",
      "only.two",
      "a.b.c.d",
      "..",
      "!!!.???.***",
      fss::crypto::Base64UrlEncode("{\"alg\":\"HS256\"}") + ".{}.x",  // payload 不是 JSON
      MintJwt(kSecret, payload) + "tamper",                            // 末尾被改
  };
  for (const auto& token : malformed) {
    CAPTURE(token);
    const auto result = authorizer.Authorize("service.file.editors", "opendes", token);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kUnauthenticated);
  }

  //  ---- 载荷被改（签名不匹配）----
  auto tampered = Payload();
  tampered["roles"] = fss::json::Value::array({"service.file.admin"});
  const std::string honest = MintJwt(kSecret, payload);
  const std::string evil = MintJwt(kSecret, tampered);
  //  拼一个"签名取自合法 token、载荷来自提权 token"的混合体
  const std::string mixed = evil.substr(0, evil.rfind('.')) + honest.substr(honest.rfind('.'));
  REQUIRE_FALSE(authorizer.Authorize("service.file.admin", "opendes", mixed).ok());
}

TEST_CASE("★ ADR-012 租户绑定：token 的 partition claim 必须与请求头一致",
          "[phase8][unit][c8.2]") {
  fss::ManualClock clock(kNow);
  LocalJwtAuthorizer authorizer(Options(), clock);

  //  ① token 属于 opendes，但请求头写成另一个租户 → 403（不是 401：token 本身合法）
  const std::string opendes_token = MintJwt(kSecret, Payload("opendes"));
  const auto cross = authorizer.Authorize("service.file.editors", "tenant-b", opendes_token);
  REQUIRE_FALSE(cross.ok());
  REQUIRE(cross.error().kind() == fss::ErrorKind::kPermissionDenied);
  INFO("跨租户消息：" << cross.error().message());
  REQUIRE(cross.error().message().find("data-partition-id") != std::string::npos);

  //  ② token 属于 tenant-b、请求头也是 tenant-b → 通过（同一个用户可以有多个租户的 token）
  const std::string tenant_b_token = MintJwt(kSecret, Payload("tenant-b"));
  REQUIRE(authorizer.Authorize("service.file.editors", "tenant-b", tenant_b_token).ok());

  //  ③ token 里没有 partition claim → 拒绝（默认强制）
  auto no_partition = Payload();
  no_partition.erase("data-partition-id");
  const auto missing = authorizer.Authorize("service.file.editors", "opendes",
                                            MintJwt(kSecret, no_partition));
  REQUIRE_FALSE(missing.ok());
  REQUIRE(missing.error().kind() == fss::ErrorKind::kUnauthenticated);

  //  ④ 显式关掉强制（"网关已可信"的部署形态）→ 只看请求头
  auto relaxed = Options();
  relaxed.require_partition_claim = false;
  LocalJwtAuthorizer relaxed_authorizer(relaxed, clock);
  REQUIRE(relaxed_authorizer
              .Authorize("service.file.editors", "opendes", MintJwt(kSecret, no_partition))
              .ok());
}

TEST_CASE("★ ADR-012 fail-closed：密钥缺失 / 关掉验签 / 静态角色表",
          "[phase8][unit][c8.5]") {
  fss::ManualClock clock(kNow);
  const std::string token = MintJwt(kSecret, Payload());

  //  ① `verify_signature=true` 但没有密钥 → **拒绝一切**（绝不降级为放行）
  LocalJwtOptions no_secret;
  LocalJwtAuthorizer broken(no_secret, clock);
  REQUIRE_FALSE(broken.Ready());
  REQUIRE_FALSE(broken.NotReadyReason().empty());
  const auto broken_result = broken.Authorize("service.file.editors", "opendes", token);
  REQUIRE_FALSE(broken_result.ok());
  REQUIRE(broken_result.error().kind() == fss::ErrorKind::kUnauthenticated);

  //  ② 显式关掉验签（仅开发）：不校验签名，但**仍然**要求 exp 与 partition claim
  auto unsigned_options = Options();
  unsigned_options.verify_signature = false;
  unsigned_options.hmac_secret.clear();
  LocalJwtAuthorizer unsigned_authorizer(unsigned_options, clock);
  REQUIRE(unsigned_authorizer.Ready());
  auto unsigned_payload = Payload();
  unsigned_payload["exp"] = kNow + 60;
  REQUIRE(unsigned_authorizer
              .Authorize("service.file.editors", "opendes",
                         MintJwt("any-secret", unsigned_payload))
              .ok());
  auto no_exp_unsigned = unsigned_payload;
  no_exp_unsigned.erase("exp");
  REQUIRE_FALSE(unsigned_authorizer
                    .Authorize("service.file.editors", "opendes",
                               MintJwt("any-secret", no_exp_unsigned))
                    .ok());

  //  ③ 静态角色表：claim 里没有角色，但表里有 → 授权通过；表与 claim 取**并集**
  auto options = Options();
  options.local_roles["viewer@example.com"] = {"service.file.viewers"};
  LocalJwtAuthorizer with_table(options, clock);
  auto table_payload = Payload("opendes", "viewer@example.com");
  table_payload.erase("roles");
  const std::string table_token = MintJwt(kSecret, table_payload);
  REQUIRE(with_table.Authorize("service.file.viewers", "opendes", table_token).ok());
  REQUIRE_FALSE(with_table.Authorize("service.file.editors", "opendes", table_token).ok());

  //  并集：claim 有 editors、表里再加 admin
  auto union_options = Options();
  union_options.local_roles["editor@example.com"] = {"service.file.admin"};
  LocalJwtAuthorizer union_authorizer(union_options, clock);
  const auto claims = union_authorizer.Verify(MintJwt(kSecret, Payload()), "opendes");
  REQUIRE(claims.ok());
  REQUIRE(claims.value().roles.size() == 3);
  REQUIRE(union_authorizer.Authorize("service.file.admin", "opendes",
                                     MintJwt(kSecret, Payload())).ok());
}
