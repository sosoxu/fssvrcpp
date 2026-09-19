// RemoteLegalValidator 实现。语义与理由见头文件（ADR-013）。
#include "infra/legal/remote_legal_validator.h"

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

//  只把 partition / legaltags 序列化进 body（用 JSON 构造器，不拼字符串）
std::string RenderRequest(std::string_view partition, const std::vector<std::string>& tags) {
  json::Value body = json::Value::object();
  body["partition"] = std::string(partition);
  json::Value list = json::Value::array();
  for (const auto& tag : tags) list.push_back(tag);
  body["legaltags"] = std::move(list);
  return json::Dump(body);
}

}  // namespace

std::string RemoteLegalValidator::NotReadyReason() const {
  return "legal.validator=remote 但没有配置 legal.remote.base_url —— "
         "本实例无法做任何法务校验（fail-closed）。请配置 Legal 校验端点（完整 URL，"
         "不追加路径）。";
}

fss::Result<void> RemoteLegalValidator::Validate(
    std::string_view partition, const std::vector<std::string>& legal_tags) {
  //  ① 本地空值防线（与 `NoopLegalValidator` 逐字同义；**不发起请求**）
  if (legal_tags.empty()) {
    return fss::Err(fss::ErrorKind::kInvalidArgument, "legal tags 不能为空");
  }
  //  ② 没配地址：不发起请求，直接失败（fail-closed；组合根本会拒绝启动，这里是双保险）
  if (options_.base_url.empty()) {
    return Unavailable("未配置 legal.remote.base_url（fail-closed）");
  }

  CURL* curl = curl_easy_init();
  if (curl == nullptr) return Unavailable("curl_easy_init 失败");

  const std::string request_body = RenderRequest(partition, legal_tags);
  std::string response_body;
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, options_.base_url.c_str());
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  //  不跟随重定向：校验端点的 3xx 只可能是配置错误，跟随会把请求带到别处
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

  //  ③ 传输层失败（超时/连不上/DNS/TLS）→ 503，**不是**"通过"也不是"不通过"
  if (code != CURLE_OK) {
    return Unavailable(std::string("Legal 校验器不可达或超时（fail-closed）：") +
                       curl_easy_strerror(code));
  }
  //  ④ 非 200（含 401/403/500）→ 503：我们不知道远端是否真的校验过
  if (status != 200) {
    return Unavailable("Legal 校验器返回非 200（" + std::to_string(status) +
                       "，fail-closed）");
  }

  //  ⑤ 200 必须带**明确**的布尔 `valid`：缺字段/类型不对 = 我们读不懂依赖的回答 → 503
  const auto parsed = json::Parse(response_body);
  if (!parsed.ok() || !parsed.value().is_object()) {
    return Unavailable("Legal 校验器的响应不是合法 JSON 对象（fail-closed）");
  }
  const auto& root = parsed.value();
  if (!root.contains("valid") || !root["valid"].is_boolean()) {
    return Unavailable("Legal 校验器的响应缺少布尔字段 valid（fail-closed）");
  }
  if (root["valid"].get<bool>()) return fss::Ok();

  //  ⑥ 远端明确说"不通过" → 400，带上远端给的 message（没有 message 时给可读占位）
  std::string message = "Legal 校验未通过";
  if (root.contains("message") && root["message"].is_string()) {
    const std::string remote_message = root["message"].get<std::string>();
    if (!remote_message.empty()) message = remote_message;
  }
  return fss::Err(fss::ErrorKind::kInvalidArgument, std::move(message));
}

}  // namespace fss::infra
