// =============================================================================
//  HmacTransferTokenCodec（L2）—— 集中存储的自签传输 token（ADR-003 §6.3）
// =============================================================================
//  集中存储没有原生预签名 URL，由本服务签发**自包含**的传输 token：
//
//      <base_url>/v1/transfer/{token}?exp={epoch}&sig={base64url(HMAC)}
//
//      token = base64url(JSON{partition, file_id, object_key, zone, op, exp, nonce})
//      sig   = base64url(HMAC-SHA256(k, token + "." + exp))     k = HMAC(secret, "fss-transfer-token-v1")
//
//  ★ 三件事必须同时成立（C3.7）
//    ① **完整性**：签名覆盖 `token` 与 `exp` 两侧，篡改 payload 任何一个字段都会让签名不匹配；
//    ② **过期**：`exp` 是 epoch 秒，由注入的 `IClock` 判定（可确定性测试，不用 sleep）；
//    ③ **不跨用途**：签名密钥用**独立域**（前缀 `fss-transfer-token-v1`）从主密钥派生，
//       与 S3 SigV4 的派生链互不复用（ADR-003 §6.3 的明确要求）。
//
//  ★ 操作/租户绑定由**内核**执行（见 `transfer_endpoint.h`）
//    codec 只保证"载荷没被改"；"这个 token 能不能用来做这次 PUT / 访问这个 partition"
//    是对请求上下文（HTTP 方法、`data-partition-id`）的判断，属于内核的职责。
//
//  ⚠️ `nonce` 目前只进入载荷、**不强制单次使用**：`self_signed.single_use_nonce`
//     的默认值是 `false`（ADR-009 M5：本地 nonce 表在多实例下不成立）。要做重放防护
//     必须把 nonce 放进共享存储（P8/P9）。本文件如实保留该字段与默认语义。
#pragma once

#include "common/result/result.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"

#include <string>
#include <string_view>

namespace fss::infra {

class HmacTransferTokenCodec final : public domain::ISelfSignedUrlCodec {
 public:
  HmacTransferTokenCodec(std::string secret, const fss::IClock& clock)
      : secret_(std::move(secret)), clock_(clock) {}

  //  产出 `<base_url>/v1/transfer/{token}?exp=<epoch>&sig=<sig>`
  fss::Result<std::string> Encode(const domain::TransferToken& token,
                                  std::string_view base_url) override;

  //  校验签名（常量时间）→ 解析载荷 → 校验 exp 参数一致 → 校验未过期
  fss::Result<domain::TransferToken> Decode(std::string_view token, std::string_view expires,
                                            std::string_view signature) override;

  //  独立密钥域（也便于测试与自证）
  static constexpr std::string_view kKeyDomain = "fss-transfer-token-v1";

 private:
  std::string SigningKey() const;
  std::string ComputeSignature(std::string_view token, std::string_view expires) const;

  std::string secret_;
  const fss::IClock& clock_;
};

}  // namespace fss::infra
