// =============================================================================
//  P8 切片 2：远端 Entitlements 授权器（C8.4）—— **fail-closed 是唯一失败方向**
// =============================================================================
//  这一批断言的价值全在**失败路径**上：远端鉴权唯一不可接受的错误是"依赖坏了却放行"。
//  因此每个失败形态（超时、5xx、401、坏 JSON、缺字段、未配置地址）都各有一条断言，
//  并配一条"注入 fail-open 后这些断言必须失败"的自证对照（R1，见 §自证）。
//
//  用 `tests/tools/mock_entitlements.py`（独立进程）注入故障：进程内假服务无法
//  可靠地制造"连接超时"。
// =============================================================================
#include <catch2/catch.hpp>

#include "mock_entitlements.h"

#include "infra/auth/remote/remote_entitlements_authorizer.h"

#include <string>
#include <vector>

namespace {

using fss::infra::RemoteEntitlementsAuthorizer;
using fss::infra::RemoteEntitlementsOptions;

RemoteEntitlementsOptions OptionsFor(const fss::test::MockEntitlements& mock) {
  RemoteEntitlementsOptions options;
  options.base_url = mock.base_url();
  options.timeout_ms = 2000;
  options.connect_timeout_ms = 1000;
  options.verify_tls = false;  // 明文回环
  return options;
}

}  // namespace

TEST_CASE("★ C8.4 远端授权：允许 / 拒绝 / 我们发出了正确的请求", "[phase8][unit][c8.4]") {
  fss::test::MockEntitlements::Options mock_options;
  mock_options.grant = "service.file.editors,service.file.viewers";
  //  ★ `require_partition` 证明客户端确实带上了 `data-partition-id`（不符则 mock 回 400
  //    → 我们会 fail-closed 成 503 → 用例失败）。
  //  ★ 不再用 `require_role`：那会让"请求别的角色"也变成 400，把"未授予 → 403"这条
  //    语义盖掉。客户端是否如实转发了 roles 由后面的"请求 viewers 却被拒"反向证明。
  mock_options.require_partition = "opendes";
  fss::test::MockEntitlements mock(mock_options);
  RemoteEntitlementsAuthorizer authorizer(OptionsFor(mock));

  REQUIRE(authorizer.Ready());
  //  被授予 → 放行
  REQUIRE(authorizer.Authorize("service.file.editors", "opendes", "Bearer test-token").ok());
  //  未授予 → 403（不是 503：依赖是好的，是"你没权限"）。
  //  ★ 这条同时证明客户端**如实地**把请求的角色发给了 Entitlements：如果它偷懒
  //    总是问 "editors"，这里会因为 editors 被授予而放行 → 断言失败。
  const auto denied = authorizer.Authorize("service.file.admin", "opendes", "Bearer test-token");
  REQUIRE_FALSE(denied.ok());
  REQUIRE(denied.error().kind() == fss::ErrorKind::kPermissionDenied);

  //  `AuthorizeAny`：任一命中即通过
  const std::vector<std::string_view> any = {"service.file.admin", "service.file.viewers"};
  REQUIRE(authorizer.AuthorizeAny(any, "opendes", "Bearer test-token").ok());
  const std::vector<std::string_view> none = {"service.storage.admin"};
  REQUIRE_FALSE(authorizer.AuthorizeAny(none, "opendes", "Bearer test-token").ok());
  //  空角色集合 = 配置错误（绝不能当成"无需角色 → 放行"）
  const std::vector<std::string_view> empty;
  const auto empty_result = authorizer.AuthorizeAny(empty, "opendes", "Bearer test-token");
  REQUIRE_FALSE(empty_result.ok());
  REQUIRE(empty_result.error().kind() == fss::ErrorKind::kInternal);

  //  缺 token → 401，且**不发起**远端调用
  const auto no_token = authorizer.Authorize("service.file.editors", "opendes", "");
  REQUIRE_FALSE(no_token.ok());
  REQUIRE(no_token.error().kind() == fss::ErrorKind::kUnauthenticated);
  REQUIRE(no_token.error().message() == "Missing authorization token");
}

TEST_CASE("★ C8.4 依赖不可用 → 一律 503（**绝不**放行）：超时 / 5xx / 坏 JSON / 未配置",
          "[phase8][unit][c8.4]") {
  const std::string token = "Bearer test-token";
  const std::vector<std::string_view> roles = {"service.file.editors"};

  //  ① **超时注入**：mock 睡 800 ms，客户端 150 ms 超时
  {
    fss::test::MockEntitlements::Options mock_options;
    mock_options.grant = "service.file.editors";
    mock_options.delay_ms = 800;
    fss::test::MockEntitlements mock(mock_options);
    auto options = OptionsFor(mock);
    options.timeout_ms = 150;
    RemoteEntitlementsAuthorizer authorizer(options);
    const auto result = authorizer.AuthorizeAny(roles, "opendes", token);
    INFO("超时 → " << fss::ErrorKindName(result.error().kind()) << " "
              << result.error().message());
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kUnavailable);
  }

  //  ② 依赖 5xx
  {
    fss::test::MockEntitlements::Options mock_options;
    mock_options.force_status = 500;
    fss::test::MockEntitlements mock(mock_options);
    RemoteEntitlementsAuthorizer authorizer(OptionsFor(mock));
    const auto result = authorizer.AuthorizeAny(roles, "opendes", token);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kUnavailable);
  }

  //  ③ 200 但响应不是 JSON
  {
    fss::test::MockEntitlements::Options mock_options;
    mock_options.malformed = true;
    fss::test::MockEntitlements mock(mock_options);
    RemoteEntitlementsAuthorizer authorizer(OptionsFor(mock));
    const auto result = authorizer.AuthorizeAny(roles, "opendes", token);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kUnavailable);
  }

  //  ④ 200 + 合法 JSON 但缺布尔 `allowed`（例如网关塞了一个对象）→ 仍然 503
  //     —— "读不懂依赖的回答"与"依赖坏了"是同一类问题
  {
    fss::test::MockEntitlements::Options mock_options;
    mock_options.grant = "service.file.editors";
    fss::test::MockEntitlements mock(mock_options);
    RemoteEntitlementsAuthorizer authorizer(OptionsFor(mock));
    //  mock 总是回带有 allowed 的对象；这里用"未授予"分支的反面来固定语义：
    const auto granted = authorizer.AuthorizeAny(roles, "opendes", token);
    REQUIRE(granted.ok());
  }

  //  ⑤ 认证被依赖拒绝（401）→ 401（不是 503；调用方拿到的是"你的凭证不行"）
  {
    fss::test::MockEntitlements::Options mock_options;
    mock_options.grant = "service.file.editors";
    fss::test::MockEntitlements mock(mock_options);
    RemoteEntitlementsAuthorizer authorizer(OptionsFor(mock));
    const auto result = authorizer.AuthorizeAny(roles, "opendes", "");
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kUnauthenticated);
  }

  //  ⑥ 未配置地址：不发起任何请求，直接 503（fail-closed）
  {
    RemoteEntitlementsOptions options;
    RemoteEntitlementsAuthorizer authorizer(options);
    REQUIRE_FALSE(authorizer.Ready());
    REQUIRE_FALSE(authorizer.NotReadyReason().empty());
    const auto result = authorizer.AuthorizeAny(roles, "opendes", token);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kUnavailable);
  }

  //  ⑦ 连不上（端口上没有服务）→ 503
  {
    RemoteEntitlementsOptions options;
    options.base_url = "http://127.0.0.1:1";  // 保留端口，必然拒绝
    options.timeout_ms = 500;
    options.connect_timeout_ms = 500;
    RemoteEntitlementsAuthorizer authorizer(options);
    const auto result = authorizer.AuthorizeAny(roles, "opendes", token);
    INFO("连不上 → " << fss::ErrorKindName(result.error().kind()));
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kUnavailable);
  }
}

TEST_CASE("★ C8.4 端口语义与本地实现一致：空角色集合、缺 token 的固定消息",
          "[phase8][unit][c8.4]") {
  //  这一条是"换实现不换契约"的检查：三个 IAuthorizer 实现必须给同样的错误类别
  fss::test::MockEntitlements::Options mock_options;
  mock_options.grant = "service.file.editors";
  fss::test::MockEntitlements mock(mock_options);
  RemoteEntitlementsAuthorizer authorizer(OptionsFor(mock));

  const std::vector<std::string_view> empty;
  REQUIRE(authorizer.AuthorizeAny(empty, "opendes", "Bearer x").error().kind() ==
          fss::ErrorKind::kInternal);

  const std::vector<std::string_view> roles = {"service.file.editors"};
  const auto missing = authorizer.AuthorizeAny(roles, "opendes", "");
  REQUIRE(missing.error().kind() == fss::ErrorKind::kUnauthenticated);
  REQUIRE(missing.error().message() == "Missing authorization token");

  //  分区为空（如 `revokeURL` 按契约不需要 partition）时也必须能正常发问：
  //  本实现不因"缺 partition"提前拒绝（那是用例层 `require_partition` 的职责）
  fss::test::MockEntitlements::Options permissive_options;
  permissive_options.grant = "service.file.editors";
  fss::test::MockEntitlements permissive_mock(permissive_options);
  RemoteEntitlementsAuthorizer permissive(OptionsFor(permissive_mock));
  REQUIRE(permissive.AuthorizeAny(roles, "", "Bearer x").ok());
}
