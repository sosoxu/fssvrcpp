// RemoteSchemaValidator 实现。语义与理由见头文件（ADR-013）。
#include "infra/schema/remote_schema_validator.h"

#include "common/json/json.h"

#include <curl/curl.h>

#include <string>
#include <utility>

namespace fss::infra {

namespace {

fss::Error Unavailable(std::string message) {
  return fss::Error(fss::ErrorKind::kUnavailable, std::move(message));
}

std::size_t WriteToString(char* data, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(data, size * nmemb);
  return size * nmemb;
}

//  只把 kind / record 序列化进 body（record 原样承载，不做字段过滤 —— 过滤会
//  让远端看到的记录与我们落库的记录不是同一个东西）
std::string RenderRequest(std::string_view kind, const json::Value& record) {
  json::Value body = json::Value::object();
  body["kind"] = std::string(kind);
  body["record"] = record;
  return json::Dump(body);
}

}  // namespace

std::string RemoteSchemaValidator::NotReadyReason() const {
  return "schema.validator=remote 但没有配置 schema.remote.base_url —— "
         "本实例无法做任何 schema 校验（fail-closed）。请配置 Schema 校验端点"
         "（完整 URL，不追加路径）。";
}

fss::Result<void> RemoteSchemaValidator::Validate(std::string_view kind,
                                                  const json::Value& record) {
  //  ① 没配地址：不发起请求，直接失败（fail-closed；组合根本会拒绝启动，这里是双保险）
  if (options_.base_url.empty()) {
    return Unavailable("未配置 schema.remote.base_url（fail-closed）");
  }

  CURL* curl = curl_easy_init();
  if (curl == nullptr) return Unavailable("curl_easy_init 失败");

  const std::string request_body = RenderRequest(kind, record);
  std::string response_body;
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, options_.base_url.c_str());
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

  //  ② 传输层失败（超时/连不上/DNS/TLS）→ 503，**不是**"通过"也不是"不通过"
  if (code != CURLE_OK) {
    return Unavailable(std::string("Schema 校验器不可达或超时（fail-closed）：") +
                       curl_easy_strerror(code));
  }
  //  ③ 非 200（含 401/403/500）→ 503
  if (status != 200) {
    return Unavailable("Schema 校验器返回非 200（" + std::to_string(status) +
                       "，fail-closed）");
  }

  //  ④ 200 必须带**明确**的布尔 `valid`
  const auto parsed = json::Parse(response_body);
  if (!parsed.ok() || !parsed.value().is_object()) {
    return Unavailable("Schema 校验器的响应不是合法 JSON 对象（fail-closed）");
  }
  const auto& root = parsed.value();
  if (!root.contains("valid") || !root["valid"].is_boolean()) {
    return Unavailable("Schema 校验器的响应缺少布尔字段 valid（fail-closed）");
  }
  if (root["valid"].get<bool>()) return fss::Ok();

  //  ⑤ 远端明确说"不通过" → 400，带上远端给的 message
  std::string message = "Schema 校验未通过";
  if (root.contains("message") && root["message"].is_string()) {
    const std::string remote_message = root["message"].get<std::string>();
    if (!remote_message.empty()) message = remote_message;
  }
  return fss::Err(fss::ErrorKind::kInvalidArgument, std::move(message));
}

}  // namespace fss::infra
