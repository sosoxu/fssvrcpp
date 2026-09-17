// =============================================================================
//  LocalJwtAuthorizer（L2）—— 本地 JWT 校验 + 角色表 + 租户绑定
// =============================================================================
//  决策与理由见 `docs/adr/ADR-012-auth-and-tenant-binding.md`（已采纳）。要点：
//    · 默认 `auth.mode=jwt`：本服务**自己**校验 token，不假设前面一定有可信网关
//    · **fail-closed**：密钥缺失 / 算法不认识 / claim 缺失 / 验签失败 → 一律拒绝，
//      没有任何"跳过校验"的分支
//    · **租户由 token 绑定**：`partition_claim`（默认 `data-partition-id`）必须等于
//      请求头里的 `data-partition-id`，否则 403 —— 否则"用 A 的 token + 改头成 B"
//      就能读到 B 的数据（仓储按 partition 隔离，但 partition 来自请求头）
//
//  本文件是 L2：只依赖 L3 端口与 L1 通用件（crypto/json/time），不碰 HTTP/gRPC。
//  ★ 不在日志里输出 token 内容（连前缀都不打）。
// =============================================================================
#pragma once

#include "common/result/result.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fss::infra {

struct LocalJwtOptions {
  //  HS256 共享密钥。`verify_signature=true` 且为空 → **拒绝所有 token**（fail-closed）
  std::string hmac_secret;
  std::string issuer;    // 非空 → `iss` 必须相等
  std::string audience;  // 非空 → `aud` 必须包含它
  bool verify_signature = true;
  std::string roles_claim = "roles";
  std::string user_id_claim = "email";
  //  租户绑定（ADR-012 §3）：claim 名与"是否强制"
  std::string partition_claim = "data-partition-id";
  bool require_partition_claim = true;
  //  静态角色表：`user_id → 角色集合`（与 claim 里的角色取**并集**）
  std::map<std::string, std::vector<std::string>, std::less<>> local_roles;
  //  允许的时钟偏移（秒）：`nbf` 早到/`exp` 晚到都容忍这么多，避免实例间时钟抖动误判（C8.10）
  std::int64_t clock_skew_seconds = 5;
};

//  校验通过的 token 声明（只暴露本服务要用的字段；其余 claim 不透传）
struct JwtClaims {
  std::string subject;             // `sub`
  std::string user_id;             // `user_id_claim`（默认 email），空则由调用方兜底
  std::string partition;           // `partition_claim`
  std::vector<std::string> roles;  // claim ∪ 静态表
  std::int64_t expires_at_epoch_seconds = 0;
  std::int64_t issued_at_epoch_seconds = 0;
};

class LocalJwtAuthorizer final : public domain::IAuthorizer {
 public:
  LocalJwtAuthorizer(LocalJwtOptions options, const fss::IClock& clock);

  //  解析 + 校验（暴露给测试与"取 user_id"的调用方；不涉及角色判定）
  fss::Result<JwtClaims> Verify(std::string_view bearer_token,
                                std::string_view request_partition) const;

  //  角色判定：`required_role` ∈ claims.roles（任一角色由 `AuthorizeAny` 表达）
  fss::Result<void> Authorize(std::string_view required_role, std::string_view partition,
                              std::string_view bearer_token) override;
  fss::Result<void> AuthorizeAny(std::span<const std::string_view> required_roles,
                                 std::string_view partition,
                                 std::string_view bearer_token) override;

  //  该 token 是否能被本实例校验（密钥/算法配置是否自洽）——组合根用它做启动自检
  bool Ready() const { return !options_.verify_signature || !options_.hmac_secret.empty(); }
  std::string NotReadyReason() const;

 private:
  LocalJwtOptions options_;
  const fss::IClock& clock_;
};

}  // namespace fss::infra
