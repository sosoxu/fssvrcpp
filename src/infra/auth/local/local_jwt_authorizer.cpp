// LocalJwtAuthorizer 实现。校验顺序与理由见头文件与 ADR-012 §5.1。
#include "infra/auth/local/local_jwt_authorizer.h"

#include "common/crypto/crypto.h"
#include "common/json/json.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace fss::infra {

namespace {

constexpr std::string_view kMissingToken = "Missing authorization token";

fss::Error Unauthenticated(std::string message) {
  return fss::Error(fss::ErrorKind::kUnauthenticated, std::move(message));
}

//  去掉 "Bearer " 前缀（大小写不敏感；上游两种都出现过）
std::string StripBearer(std::string_view value) {
  constexpr std::string_view kPrefix = "bearer ";
  if (value.size() >= kPrefix.size()) {
    bool matches = true;
    for (std::size_t i = 0; i < kPrefix.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(value[i])) != kPrefix[i]) {
        matches = false;
        break;
      }
    }
    if (matches) return std::string(value.substr(kPrefix.size()));
  }
  return std::string(value);
}

std::string Trim(std::string_view value) {
  std::size_t begin = 0;
  std::size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
  return std::string(value.substr(begin, end - begin));
}

//  JSON 里的数字/字符串 claim → epoch 秒（拒绝非整数：fail-closed）
std::optional<std::int64_t> NumberClaim(const json::Value& payload, std::string_view name) {
  const auto it = payload.find(std::string(name));
  if (it == payload.end()) return std::nullopt;
  if (it->is_number_integer()) return it->get<std::int64_t>();
  if (it->is_number_float()) return static_cast<std::int64_t>(it->get<double>());
  return std::nullopt;  // 存在但类型不对 → 当作"非法"，由调用方报错
}

std::optional<std::string> StringClaim(const json::Value& payload, std::string_view name) {
  const auto it = payload.find(std::string(name));
  if (it == payload.end() || !it->is_string()) return std::nullopt;
  return it->get<std::string>();
}

//  `aud` 既可能是字符串也可能是数组（RFC 7519 §4.1.3）
bool AudienceMatches(const json::Value& payload, std::string_view expected) {
  const auto it = payload.find("aud");
  if (it == payload.end()) return false;
  if (it->is_string()) return it->get<std::string>() == expected;
  if (it->is_array()) {
    for (const auto& item : *it) {
      if (item.is_string() && item.get<std::string>() == expected) return true;
    }
  }
  return false;
}

std::vector<std::string> RolesFromClaim(const json::Value& payload, std::string_view claim) {
  std::vector<std::string> roles;
  const auto it = payload.find(std::string(claim));
  if (it == payload.end()) return roles;
  if (it->is_array()) {
    for (const auto& item : *it) {
      if (item.is_string()) roles.push_back(item.get<std::string>());
    }
  } else if (it->is_string()) {
    //  兼容"逗号分隔的单字符串"形态（部分 IdP 这样发）
    const std::string raw = it->get<std::string>();
    std::size_t begin = 0;
    while (begin <= raw.size() && !raw.empty()) {
      const auto comma = raw.find(',', begin);
      const auto piece = Trim(std::string_view(raw).substr(
          begin, comma == std::string::npos ? std::string::npos : comma - begin));
      if (!piece.empty()) roles.push_back(piece);
      if (comma == std::string::npos) break;
      begin = comma + 1;
    }
  }
  return roles;
}

bool Contains(const std::vector<std::string>& values, std::string_view needle) {
  return std::find(values.begin(), values.end(), needle) != values.end();
}

}  // namespace

LocalJwtAuthorizer::LocalJwtAuthorizer(LocalJwtOptions options, const fss::IClock& clock)
    : options_(std::move(options)), clock_(clock) {}

std::string LocalJwtAuthorizer::NotReadyReason() const {
  if (options_.verify_signature && options_.hmac_secret.empty()) {
    return "auth.jwt.verify_signature=true 但没有配置 auth.jwt.hmac_secret —— "
           "本实例会拒绝所有 token（fail-closed）。请配置共享密钥，或显式关闭验签（仅开发）。";
  }
  return "";
}

fss::Result<JwtClaims> LocalJwtAuthorizer::Verify(std::string_view bearer_token,
                                                 std::string_view request_partition) const {
  //  ①② 缺什么就报什么（消息逐字节对齐契约 §1.2）
  const std::string token = Trim(StripBearer(bearer_token));
  if (token.empty()) return Unauthenticated(std::string(kMissingToken));
  //  ★ "缺 partition → 401 Missing partitionID" 由**用例层**（`AuthorizeCaller` 的
  //    `require_partition`）判定，不在这里：`revokeURL` 按契约 §1.2 **不需要**
  //    `data-partition-id`，作者器若在这里一律拒绝就会把"合法的 revokeURL"判成 401。
  //    本层只负责：token 有效 + 角色 + （给了 partition 时）租户绑定一致。

  //  ③ fail-closed：验签公钥/密钥不可用时不是"跳过"，而是拒绝
  if (options_.verify_signature && options_.hmac_secret.empty()) {
    return Unauthenticated("本实例未配置 JWT 验签密钥，拒绝所有 token（fail-closed）");
  }

  //  ④ 三段式
  const auto first_dot = token.find('.');
  const auto second_dot = first_dot == std::string::npos ? std::string::npos
                                                         : token.find('.', first_dot + 1);
  if (first_dot == std::string::npos || second_dot == std::string::npos ||
      token.find('.', second_dot + 1) != std::string::npos) {
    return Unauthenticated("JWT 格式非法（期望 header.payload.signature 三段）");
  }
  const std::string_view header_b64(token.data(), first_dot);
  const std::string_view payload_b64(token.data() + first_dot + 1, second_dot - first_dot - 1);
  const std::string_view signature_b64(token.data() + second_dot + 1,
                                       token.size() - second_dot - 1);
  if (header_b64.empty() || payload_b64.empty() || signature_b64.empty()) {
    return Unauthenticated("JWT 格式非法（存在空段）");
  }

  const auto header_bytes = crypto::Base64UrlDecode(header_b64);
  const auto payload_bytes = crypto::Base64UrlDecode(payload_b64);
  if (!header_bytes.has_value() || !payload_bytes.has_value()) {
    return Unauthenticated("JWT 段不是合法的 base64url");
  }
  const auto header = json::Parse(std::string_view(
      reinterpret_cast<const char*>(header_bytes->data()), header_bytes->size()));
  const auto payload = json::Parse(std::string_view(
      reinterpret_cast<const char*>(payload_bytes->data()), payload_bytes->size()));
  if (!header.ok() || !payload.ok() || !header.value().is_object() ||
      !payload.value().is_object()) {
    return Unauthenticated("JWT 的 header/payload 不是合法 JSON 对象");
  }

  //  ⑤ 算法白名单：`none` 与未知算法一律拒绝（**不是**跳过验签）
  const auto algorithm = StringClaim(header.value(), "alg").value_or("");
  if (algorithm != "HS256") {
    return Unauthenticated("不支持的 JWT 算法（只接受 HS256）：" +
                               (algorithm.empty() ? std::string("<缺失>") : algorithm));
  }

  //  ⑥ 验签 + 常量时间比较
  if (options_.verify_signature) {
    const auto signature = crypto::Base64UrlDecode(signature_b64);
    if (!signature.has_value()) return Unauthenticated("JWT 签名不是合法的 base64url");
    const std::string signing_input(token.substr(0, second_dot));
    const auto expected = crypto::HmacSha256(options_.hmac_secret, signing_input);
    if (signature->size() != expected.size()) {
      return Unauthenticated("JWT 签名长度非法（HS256 期望 32 字节）");
    }
    crypto::Digest256 provided{};
    std::copy(signature->begin(), signature->end(), provided.begin());
    if (!crypto::ConstantTimeEquals(std::span<const std::uint8_t>(expected.data(), expected.size()),
                                   std::span<const std::uint8_t>(provided.data(),
                                                                 provided.size()))) {
      return Unauthenticated("JWT 验签失败");
    }
  }

  //  ⑦ 时间与发行方/受众
  const std::int64_t now = clock_.NowEpochSeconds();
  const auto expires = NumberClaim(payload.value(), "exp");
  if (!expires.has_value()) {
    //  ★ 没有 `exp` 的 token 等于"永久有效"：fail-closed 必须拒绝
    return Unauthenticated("JWT 缺少 exp（拒绝无过期时间的 token）");
  }
  if (now > *expires + options_.clock_skew_seconds) {
    return Unauthenticated("JWT 已过期");
  }
  if (const auto not_before = NumberClaim(payload.value(), "nbf"); not_before.has_value()) {
    if (now + options_.clock_skew_seconds < *not_before) {
      return Unauthenticated("JWT 尚未生效（nbf）");
    }
  }
  if (!options_.issuer.empty()) {
    const auto issuer = StringClaim(payload.value(), "iss").value_or("");
    if (issuer != options_.issuer) return Unauthenticated("JWT 的 iss 不匹配");
  }
  if (!options_.audience.empty() && !AudienceMatches(payload.value(), options_.audience)) {
    return Unauthenticated("JWT 的 aud 不匹配");
  }

  JwtClaims claims;
  claims.subject = StringClaim(payload.value(), "sub").value_or("");
  claims.user_id = StringClaim(payload.value(), options_.user_id_claim).value_or("");
  claims.partition = StringClaim(payload.value(), options_.partition_claim).value_or("");
  claims.expires_at_epoch_seconds = *expires;
  claims.issued_at_epoch_seconds = NumberClaim(payload.value(), "iat").value_or(0);
  claims.roles = RolesFromClaim(payload.value(), options_.roles_claim);
  //  静态角色表：与 claim 取并集（同一个用户可能在表里被额外授权）
  if (!claims.user_id.empty()) {
    if (const auto it = options_.local_roles.find(claims.user_id);
        it != options_.local_roles.end()) {
      for (const auto& role : it->second) {
        if (!Contains(claims.roles, role)) claims.roles.push_back(role);
      }
    }
  }

  //  ⑧ 租户绑定：token 说自己是哪个租户，必须与请求头一致（ADR-012 §3）
  //     ★ 请求没带 partition（如 `revokeURL`）时不比较 —— 没有"声明的租户"可比。
  if (options_.require_partition_claim && !request_partition.empty()) {
    if (claims.partition.empty()) {
      return Unauthenticated("JWT 缺少 " + options_.partition_claim +
                                 " claim（租户绑定的依据）");
    }
    if (claims.partition != request_partition) {
      return fss::Error(fss::ErrorKind::kPermissionDenied,
                      "JWT 的 " + options_.partition_claim +
                          " 与请求的 data-partition-id 不一致");
    }
  }
  return claims;
}

fss::Result<void> LocalJwtAuthorizer::Authorize(std::string_view required_role,
                                               std::string_view partition,
                                               std::string_view bearer_token) {
  FSS_TRY(claims, Verify(bearer_token, partition));
  if (!Contains(claims.roles, required_role)) {
    return fss::Error(fss::ErrorKind::kPermissionDenied,
                        "调用方缺少角色：" + std::string(required_role));
  }
  return Ok();
}

fss::Result<void> LocalJwtAuthorizer::AuthorizeAny(
    std::span<const std::string_view> required_roles, std::string_view partition,
    std::string_view bearer_token) {
  //  "没有要求任何角色"如果被当成"放行"，就是一个静默的鉴权后门（与用例层同一条纪律）
  if (required_roles.empty()) {
    return fss::Error(fss::ErrorKind::kInternal, "AuthorizeAny 要求至少一个角色");
  }
  FSS_TRY(claims, Verify(bearer_token, partition));
  for (const auto& role : required_roles) {
    if (Contains(claims.roles, role)) return Ok();
  }
  std::string wanted;
  for (const auto& role : required_roles) {
    if (!wanted.empty()) wanted += ", ";
    wanted += std::string(role);
  }
  return fss::Error(fss::ErrorKind::kPermissionDenied,
                      "调用方缺少任一必需角色：" + wanted);
}

}  // namespace fss::infra
