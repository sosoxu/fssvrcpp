// =============================================================================
//  RemoteEntitlementsAuthorizer（L2）—— 远端 Entitlements 的 `authorizeAny`
// =============================================================================
//  决策见 `docs/adr/ADR-012-auth-and-tenant-binding.md`（备选方案 3）。要点：
//
//  · **fail-closed 是唯一允许的失败方向**：超时、连不上、非 200、坏 JSON、
//    缺 `allowed` 字段 → 一律 `kUnavailable`（503），**绝不**返回 Ok。
//    配置里的 `auth.remote_entitlements.fail_closed` 只是**声明**（schema 强制必须 true）；
//    本实现**不提供**"失败即放行"的开关 —— 那种开关迟早会被打开。
//  · 角色的权威源在 Entitlements：本服务不缓存（缓存会引入"撤权延迟"，
//    而撤权的正确性优先于每请求一次 RTT；缓存留给后续按需加）。
//  · 与项目约定的接口（契约 §4.5）：
//      POST {base_url}{authorize_path}
//      headers: Authorization: <bearer 原样>、data-partition-id: <partition>
//      body:    {"roles":[...]}
//      200 + {"allowed":true|false, ...}；401 → kUnauthenticated；其它 → kUnavailable
//
//  ⚠️ **未与真实 OSDU Entitlements 联调**（本机环境不可达）：路径与响应形状是
//     本项目与 Entitlements 的约定，`authorize_path` 可配置以便适配真实服务。
//     该限制已登记在 ADR-012 §5.3 与 `docs/test-evidence/phase8.md`。
//
//  本文件是 L2：只依赖 L3 端口与 L1（json/result/curl）。
// =============================================================================
#pragma once

#include "common/result/result.h"
#include "domain/ports/ports.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fss::infra {

struct RemoteEntitlementsOptions {
  std::string base_url;  // 空 → **拒绝一切**（fail-closed），不发起任何请求
  std::string authorize_path = "/api/entitlements/v2/authorizeAny";
  int timeout_ms = 3000;          // 整体超时
  int connect_timeout_ms = 1000;  // 连接超时
  bool verify_tls = true;
  std::string ca_bundle_path;
};

class RemoteEntitlementsAuthorizer final : public domain::IAuthorizer {
 public:
  explicit RemoteEntitlementsAuthorizer(RemoteEntitlementsOptions options)
      : options_(std::move(options)) {}

  fss::Result<void> Authorize(std::string_view required_role, std::string_view partition,
                              std::string_view bearer_token) override;
  fss::Result<void> AuthorizeAny(std::span<const std::string_view> required_roles,
                                 std::string_view partition,
                                 std::string_view bearer_token) override;

  //  组合根用它做启动自检（未配置地址时**拒绝启动**，而不是起来后拒绝一切）
  bool Ready() const { return !options_.base_url.empty(); }
  std::string NotReadyReason() const;

 private:
  //  真值 = 放行；`kUnavailable` = 依赖故障（调用方必须映射成 503，不得放行）
  fss::Result<bool> AskEntitlements(const std::vector<std::string>& roles,
                                    std::string_view partition,
                                    std::string_view bearer_token) const;

  RemoteEntitlementsOptions options_;
};

}  // namespace fss::infra
