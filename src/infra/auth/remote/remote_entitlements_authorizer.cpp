// RemoteEntitlementsAuthorizer 实现。语义与理由见头文件。
#include "infra/auth/remote/remote_entitlements_authorizer.h"

#include "common/json/json.h"

#include <curl/curl.h>

#include <algorithm>
#include <string>
#include <utility>

namespace fss::infra {

namespace {

constexpr std::string_view kMissingToken = "Missing authorization token";

fss::Error Unavailable(std::string message) {
  return fss::Error(fss::ErrorKind::kUnavailable, std::move(message));
}

std::size_t WriteToString(char* data, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(data, size * nmemb);
  return size * nmemb;
}

//  只把 roles 序列化进 body（不拼字符串 —— 角色名虽然来自常量，但拼接是坏习惯）
std::string RenderRequest(const std::vector<std::string>& roles) {
  json::Value body = json::Value::object();
  json::Value list = json::Value::array();
  for (const auto& role : roles) list.push_back(role);
  body["roles"] = std::move(list);
  return json::Dump(body);
}

}  // namespace

std::string RemoteEntitlementsAuthorizer::NotReadyReason() const {
  return "auth.mode=remote-entitlements 但没有配置 auth.remote_entitlements.base_url —— "
         "本实例会拒绝所有请求（fail-closed）。请配置 Entitlements 地址。";
}

fss::Result<bool> RemoteEntitlementsAuthorizer::AskEntitlements(
    const std::vector<std::string>& roles, std::string_view partition,
    std::string_view bearer_token) const {
  //  ① 没配地址：不发起请求，直接失败（fail-closed）
  if (options_.base_url.empty()) {
    return Unavailable("未配置 auth.remote_entitlements.base_url（fail-closed）");
  }

  CURL* curl = curl_easy_init();
  if (curl == nullptr) return Unavailable("curl_easy_init 失败");

  const std::string url = options_.base_url + options_.authorize_path;
  const std::string request_body = RenderRequest(roles);
  std::string response_body;
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  const std::string authorization = "Authorization: " + std::string(bearer_token);
  headers = curl_slist_append(headers, authorization.c_str());
  const std::string partition_header = "data-partition-id: " + std::string(partition);
  headers = curl_slist_append(headers, partition_header.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options_.connect_timeout_ms));
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(options_.timeout_ms));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request_body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  if (!options_.verify_tls) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  } else if (!options_.ca_bundle_path.empty()) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, options_.ca_bundle_path.c_str());
  }

  const CURLcode code = curl_easy_perform(curl);
  long status = 0;
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  //  ② 传输层失败（超时/连不上/DNS/TLS）→ 503，**不是**放行
  if (code != CURLE_OK) {
    return Unavailable(std::string("Entitlements 不可达或超时（fail-closed）：") +
                       curl_easy_strerror(code));
  }
  //  ③ 401 透传成"未认证"；其它非 200 一律 503
  if (status == 401) {
    return fss::Error(fss::ErrorKind::kUnauthenticated,
                      "Entitlements 拒绝了该凭证（401）");
  }
  if (status != 200) {
    return Unavailable("Entitlements 返回非 200（" + std::to_string(status) +
                       "，fail-closed）");
  }

  //  ④ 200 必须带**明确**的 `allowed`：缺字段/类型不对 = 我们读不懂依赖的回答 → 503
  const auto parsed = json::Parse(response_body);
  if (!parsed.ok() || !parsed.value().is_object()) {
    return Unavailable("Entitlements 的响应不是合法 JSON 对象（fail-closed）");
  }
  const auto allowed = parsed.value().find("allowed");
  if (allowed == parsed.value().end() || !allowed->is_boolean()) {
    return Unavailable("Entitlements 的响应缺少布尔字段 allowed（fail-closed）");
  }
  return allowed->get<bool>();
}

fss::Result<void> RemoteEntitlementsAuthorizer::Authorize(std::string_view required_role,
                                                         std::string_view partition,
                                                         std::string_view bearer_token) {
  const std::vector<std::string_view> roles{required_role};
  return AuthorizeAny(roles, partition, bearer_token);
}

fss::Result<void> RemoteEntitlementsAuthorizer::AuthorizeAny(
    std::span<const std::string_view> required_roles, std::string_view partition,
    std::string_view bearer_token) {
  //  "没有要求任何角色"如果被当成"放行"，就是一个静默的鉴权后门（三个实现同一条纪律）
  if (required_roles.empty()) {
    return fss::Error(fss::ErrorKind::kInternal, "AuthorizeAny 要求至少一个角色");
  }
  if (bearer_token.empty()) {
    return fss::Error(fss::ErrorKind::kUnauthenticated, std::string(kMissingToken));
  }
  std::vector<std::string> roles;
  roles.reserve(required_roles.size());
  for (const auto& role : required_roles) roles.emplace_back(role);

  FSS_TRY(allowed, AskEntitlements(roles, partition, bearer_token));
  if (!allowed) {
    std::string wanted;
    for (const auto& role : roles) {
      if (!wanted.empty()) wanted += ", ";
      wanted += role;
    }
    return fss::Error(fss::ErrorKind::kPermissionDenied,
                      "Entitlements 未授予任一必需角色：" + wanted);
  }
  return Ok();
}

}  // namespace fss::infra
