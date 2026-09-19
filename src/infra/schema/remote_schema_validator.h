// =============================================================================
//  RemoteSchemaValidator（L2）—— 远端 **schema** 校验（`schema.validator=remote`）
// =============================================================================
//  决策见 `docs/adr/ADR-013-file-service-extension-endpoints.md`。与
//  `RemoteLegalValidator` **同一条纪律**（这里只列与它不同之处）：
//
//  · fail-closed 矩阵逐条相同：连接失败 / 超时 / 非 200 / 非 JSON / 缺 `valid` /
//    `valid` 不是 bool → 一律 `kUnavailable`（HTTP **503**）；
//    `200 + {"valid":false,"message":"..."}` → `kInvalidArgument`（HTTP **400**），
//    消息带上远端 `message`。
//  · `base_url` 为空且 `schema.validator=remote` → 组合根用 `Ready()` /
//    `NotReadyReason()` **拒绝启动**（exit 78）；本实现自身也不发起请求。
//  · **没有**"空值防线"：`kind` / `record` 由用例层的前置校验（`KindValidator`、
//    `ValidateMetadataRecord`）保证，这里再发明一条只会变成第二份真相。
//
//  ★ **线协议是本项目的约定，不是上游路径**（必须如实标注）
//    上游 File Service **不直接调用** Schema 服务：`docs/01-osdu-research.md:110`
//    指出"legal tag 的合规性由 Storage Service 的 PUT /records 内部校验"，记录写入
//    的 schema 校验同样发生在 Storage Service 侧。因此本端点与 `/v1/transfer` 同类，
//    是**平台外扩展**：
//      POST <schema.remote.base_url>           ← base_url 就是**完整端点 URL**，
//      Content-Type: application/json             **不追加任何路径**（ADR-013）
//      body: {"kind":"<kind>","record":{ ...完整记录 JSON... }}
//      200 + {"valid":true}                    → 通过
//      200 + {"valid":false,"message":"<原因>"} → 不通过（→ 400，带上 message）
//      其余一切                                 → 依赖故障（→ 503，fail-closed）
//
//  ⚠️ **未与真实 Schema 服务联调**（本环境没有该服务）：协议形状是本项目与运维方的
//     约定，已登记在 ADR-013 §5 与 `docs/test-evidence/phase10.md` §11。
//
//  ⚠️ **不透传调用方身份**：端口签名是 `Validate(kind, record)`
//     （`src/domain/ports/ports.h`），**没有** bearer token 参数 → 端点必须允许
//     **无 per-request 认证**访问（集群内网 / mTLS 终结 / 网络策略白名单）。
//     该前提同时写在 `docs/operations.md` §1.2.11 与契约 §7。
//
//  本文件是 L2：只依赖 L3 端口与 L1（json/result/curl）。
// =============================================================================
#pragma once

#include "common/result/result.h"
#include "domain/ports/ports.h"

#include <string>
#include <string_view>

namespace fss::infra {

struct RemoteSchemaValidatorOptions {
  std::string base_url;           // 完整端点 URL（POST 到它，不追加路径）；空 → 不发起请求
  int timeout_ms = 3000;          // 整体超时
  int connect_timeout_ms = 1000;  // 连接超时
  bool verify_tls = true;
  std::string ca_bundle_path;
};

class RemoteSchemaValidator final : public domain::ISchemaValidator {
 public:
  explicit RemoteSchemaValidator(RemoteSchemaValidatorOptions options)
      : options_(std::move(options)) {}

  fss::Result<void> Validate(std::string_view kind, const json::Value& record) override;

  //  组合根用它做启动自检（未配置地址时**拒绝启动**）
  bool Ready() const { return !options_.base_url.empty(); }
  std::string NotReadyReason() const;

 private:
  RemoteSchemaValidatorOptions options_;
};

}  // namespace fss::infra
