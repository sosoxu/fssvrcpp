// =============================================================================
//  RemoteLegalValidator（L2）—— 远端**法务标签**校验（`legal.validator=remote`）
// =============================================================================
//  决策见 `docs/adr/ADR-013-file-service-extension-endpoints.md`。要点：
//
//  · **fail-closed 是唯一允许的失败方向**：连接失败 / 超时 / 非 200 / 非 JSON /
//    缺 `valid` 字段 / `valid` 不是 bool → 一律 `fss::ErrorKind::kUnavailable`
//    （HTTP **503**）。**绝不**把"依赖故障"当成"校验通过"或"校验不通过（400）"。
//  · 远端**明确**回答"不通过"（`200 + {"valid":false,"message":"..."}`）→
//    `kInvalidArgument`（HTTP **400**），消息里带上远端给的 `message`。
//  · `base_url` 为空且 `legal.validator=remote` → 组合根用 `Ready()` /
//    `NotReadyReason()` **拒绝启动**（exit 78，与 `RemoteEntitlementsAuthorizer` 一致）；
//    本实现自身在没配地址时也**不发起任何请求**（直接失败），双保险。
//
//  ★ **线协议是本项目的约定，不是上游路径**（必须如实标注）
//    上游 File Service **不直接调用** Legal 服务：`docs/01-osdu-research.md:110`
//    指出"legal tag 的合规性由 Storage Service 的 PUT /records 内部校验"。
//    因此本端点与 `/v1/transfer` 同类，是**平台外扩展**：
//      POST <legal.remote.base_url>            ← base_url 就是**完整端点 URL**，
//      Content-Type: application/json             **不追加任何路径**（ADR-013）
//      body: {"partition":"<p>","legaltags":["<t>", ...]}
//      200 + {"valid":true}                    → 通过
//      200 + {"valid":false,"message":"<原因>"} → 不通过（→ 400，带上 message）
//      其余一切                                 → 依赖故障（→ 503，fail-closed）
//
//  ⚠️ **未与真实 Legal 服务联调**（本环境没有该服务）：协议形状是本项目与运维方的
//     约定，已登记在 ADR-013 §5 与 `docs/test-evidence/phase10.md` §11。
//
//  ⚠️ **不透传调用方身份**：端口签名是 `Validate(partition, legal_tags)`
//     （`src/domain/ports/ports.h`），**没有** bearer token 参数，因此本适配器
//     无法（也不应该）转发调用方的凭证。这要求部署满足**端点必须允许无
//     per-request 认证访问**（集群内网 / mTLS 终结 / 网络策略白名单）。
//     该前提同时写在 `docs/operations.md` §1.2.11 与契约 §7。
//
//  本文件是 L2：只依赖 L3 端口与 L1（json/result/curl）。
// =============================================================================
#pragma once

#include "common/result/result.h"
#include "domain/ports/ports.h"

#include <string>
#include <string_view>
#include <vector>

namespace fss::infra {

struct RemoteLegalValidatorOptions {
  std::string base_url;           // 完整端点 URL（POST 到它，不追加路径）；空 → 不发起请求
  int timeout_ms = 3000;          // 整体超时
  int connect_timeout_ms = 1000;  // 连接超时
  bool verify_tls = true;
  std::string ca_bundle_path;
};

class RemoteLegalValidator final : public domain::ILegalValidator {
 public:
  explicit RemoteLegalValidator(RemoteLegalValidatorOptions options)
      : options_(std::move(options)) {}

  //  `legaltags` 为空 → 直接 `kInvalidArgument`（与 `NoopLegalValidator` 一致），
  //  **不发起请求**：空集合没有任何可校验的内容，把它发出去只是浪费一次 RTT。
  fss::Result<void> Validate(std::string_view partition,
                             const std::vector<std::string>& legal_tags) override;

  //  组合根用它做启动自检（未配置地址时**拒绝启动**，而不是起来后拒绝所有请求）
  bool Ready() const { return !options_.base_url.empty(); }
  std::string NotReadyReason() const;

 private:
  RemoteLegalValidatorOptions options_;
};

}  // namespace fss::infra
