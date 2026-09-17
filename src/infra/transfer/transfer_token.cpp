// HmacTransferTokenCodec 实现。协议与安全要求见头文件。
#include "infra/transfer/transfer_token.h"

#include "common/crypto/crypto.h"
#include "common/json/json.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fss::infra {

namespace {

fss::Error Invalid(const std::string& message) {
  return Err(fss::ErrorKind::kInvalidArgument, message);
}
fss::Error Unauthenticated(const std::string& message) {
  return Err(fss::ErrorKind::kUnauthenticated, message);
}

bool IsKnownOp(std::string_view op) { return op == "put" || op == "get"; }

std::string Base64Url(const std::string_view data) { return crypto::Base64UrlEncode(data); }

//  把 base64url 解成原始字节串；失败返回 nullopt
std::optional<std::string> Base64UrlDecodeToString(std::string_view text) {
  const auto bytes = crypto::Base64UrlDecode(text);
  if (!bytes.has_value()) return std::nullopt;
  return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

}  // namespace

std::string HmacTransferTokenCodec::SigningKey() const {
  //  ★ 独立密钥域：与 S3 SigV4 的派生链分开（ADR-003 §6.3）
  return crypto::ToHex(crypto::HmacSha256(secret_, kKeyDomain));
}

std::string HmacTransferTokenCodec::ComputeSignature(std::string_view token,
                                                     std::string_view expires) const {
  const std::string message = std::string(token) + "." + std::string(expires);
  return crypto::Base64UrlEncode(crypto::HmacSha256(SigningKey(), message));
}

fss::Result<std::string> HmacTransferTokenCodec::Encode(const domain::TransferToken& token,
                                                        std::string_view base_url) {
  if (secret_.empty()) return Invalid("transfer token 密钥未配置");
  if (token.partition.empty()) return Invalid("TransferToken.partition 不能为空");
  if (token.container.empty()) return Invalid("TransferToken.container 不能为空");
  if (token.object_key.empty()) return Invalid("TransferToken.object_key 不能为空");
  if (!IsKnownOp(token.op)) return Invalid("TransferToken.op 只能是 put / get");
  if (token.expires_at_epoch_seconds <= 0) {
    return Invalid("TransferToken.expires_at_epoch_seconds 必须为正");
  }

  json::Value payload = json::Value::object();
  payload["partition"] = token.partition;
  payload["file_id"] = token.file_id;
  payload["container"] = token.container;
  payload["object_key"] = token.object_key;
  payload["zone"] = std::string(domain::StorageZoneName(token.zone));
  payload["op"] = token.op;
  payload["exp"] = token.expires_at_epoch_seconds;
  //  nonce 只为将来的重放防护预留（默认不强制单次使用，见头文件）
  payload["nonce"] = crypto::RandomHex(8);

  const std::string encoded_token = Base64Url(json::Dump(payload));
  const std::string expires = std::to_string(token.expires_at_epoch_seconds);
  const std::string signature = ComputeSignature(encoded_token, expires);

  std::string base(base_url);
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base + "/v1/transfer/" + encoded_token + "?exp=" + expires + "&sig=" + signature;
}

fss::Result<domain::TransferToken> HmacTransferTokenCodec::Decode(
    std::string_view token, std::string_view expires, std::string_view signature) {
  if (secret_.empty()) return Invalid("transfer token 密钥未配置");
  if (token.empty()) return Unauthenticated("transfer token 为空");
  if (expires.empty() || signature.empty()) {
    return Unauthenticated("transfer token 缺少 exp 或 sig");
  }

  //  ① 先验签：常量时间比较，签名不符时**不解析**载荷（避免把未认证数据当真）
  const std::string expected = ComputeSignature(token, expires);
  if (!crypto::ConstantTimeEquals(expected, signature)) {
    return Unauthenticated("transfer token 签名不匹配");
  }

  //  ② 解析载荷
  const auto raw = Base64UrlDecodeToString(token);
  if (!raw.has_value()) return Unauthenticated("transfer token 不是合法的 base64url");
  const auto parsed = json::ParseObject(*raw);
  if (!parsed.ok()) return Unauthenticated("transfer token 载荷不是 JSON 对象");

  domain::TransferToken out;
  const json::Value& value = parsed.value();
  const auto require_string = [&](const char* key, std::string& target) -> bool {
    const auto it = value.find(key);
    if (it == value.end() || !it->is_string()) return false;
    target = it->get<std::string>();
    return true;
  };
  if (!require_string("partition", out.partition)) {
    return Unauthenticated("transfer token 缺少 partition");
  }
  if (!require_string("container", out.container)) {
    return Unauthenticated("transfer token 缺少 container");
  }
  if (!require_string("object_key", out.object_key)) {
    return Unauthenticated("transfer token 缺少 object_key");
  }
  if (!require_string("op", out.op) || !IsKnownOp(out.op)) {
    return Unauthenticated("transfer token 的 op 非法");
  }
  (void)require_string("file_id", out.file_id);

  const auto zone_it = value.find("zone");
  if (zone_it == value.end() || !zone_it->is_string()) {
    return Unauthenticated("transfer token 缺少 zone");
  }
  const auto zone = domain::ParseStorageZone(zone_it->get<std::string>());
  if (!zone.has_value()) return Unauthenticated("transfer token 的 zone 非法");
  out.zone = *zone;

  const auto exp_it = value.find("exp");
  if (exp_it == value.end() || !exp_it->is_number_integer()) {
    return Unauthenticated("transfer token 缺少 exp");
  }
  const std::int64_t payload_exp = exp_it->get<std::int64_t>();

  //  ③ `exp` 查询参数必须与载荷一致（签名已覆盖两者，这里再显式钉一次语义）
  std::int64_t query_exp = 0;
  try {
    query_exp = std::stoll(std::string(expires));
  } catch (...) {
    return Unauthenticated("transfer token 的 exp 不是整数");
  }
  if (query_exp != payload_exp) return Unauthenticated("transfer token 的 exp 与载荷不一致");

  //  ④ 过期判定（注入时钟 → 测试无需 sleep）
  if (clock_.NowEpochSeconds() >= payload_exp) {
    return Unauthenticated("transfer token 已过期");
  }
  out.expires_at_epoch_seconds = payload_exp;
  return out;
}

}  // namespace fss::infra
