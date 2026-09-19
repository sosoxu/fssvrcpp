// WebhookEventPublisher 实现。语义与理由见头文件（ADR-013 §9）。
#include "infra/event/webhook_event_publisher.h"

#include "common/json/json.h"

#include <curl/curl.h>

#include <cstdint>
#include <string>
#include <utility>

namespace fss::infra {

namespace {

//  ★ 与远端校验器不同：这里的 `kUnavailable` **不会**变成 HTTP 503 ——
//    用例层丢弃它（`(void)ports.events.Publish...`），请求照常成功。
fss::Error Unavailable(std::string message) {
  return fss::Error(fss::ErrorKind::kUnavailable, std::move(message));
}

std::size_t WriteToString(char* data, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(data, size * nmemb);
  return size * nmemb;
}

}  // namespace

std::string WebhookEventPublisher::NotReadyReason() const {
  return "events.publisher=webhook 但没有配置 events.webhook.url —— "
         "本实例无法发布任何事件（若要「关掉事件」，应当显式用 events.publisher=none）。"
         "请配置 webhook 端点（完整 URL，不追加路径），或改回 publisher=log。";
}

fss::Result<void> WebhookEventPublisher::PublishStatusChanged(
    std::string_view topic, const domain::StatusChangedEvent& event) {
  //  ★ 镜像上游 statusChanged 事件的形状（`src/domain/ports/ports.h`）：
  //    {"topic":T,"kind":"statusChanged","body":{...}}
  json::Value payload = json::Value::object();
  payload["topic"] = EffectiveTopic(topic);
  payload["kind"] = "statusChanged";
  json::Value body = json::Value::object();
  body["recordId"] = event.record_id;
  body["partition"] = event.partition;
  body["status"] = event.status;
  body["datasetSync"] = event.dataset_sync;
  body["version"] = event.version;
  payload["body"] = std::move(body);
  return Post("statusChanged", json::Dump(payload));
}

fss::Result<void> WebhookEventPublisher::PublishDatasetDetails(
    std::string_view topic, const domain::DatasetDetailsEvent& event) {
  //  ★ 镜像上游 `FileDatasetDetailsPublisher.java` 的形状：`body` 是**长度为 1 的数组**，
  //    元素是 `{"properties":{...}}`（`ports.h` 的注释是一手依据）。
  json::Value properties = json::Value::object();
  properties["correlationId"] = event.correlation_id;
  properties["datasetId"] = event.dataset_id;
  properties["datasetType"] = event.dataset_type;
  properties["datasetVersionId"] = event.dataset_version_id;
  properties["recordCount"] = event.record_count;
  properties["timestamp"] = event.timestamp_millis;

  json::Value element = json::Value::object();
  element["properties"] = std::move(properties);

  json::Value body = json::Value::array();
  body.push_back(std::move(element));

  json::Value payload = json::Value::object();
  payload["topic"] = EffectiveTopic(topic);
  payload["kind"] = "datasetDetails";
  payload["body"] = std::move(body);
  return Post("datasetDetails", json::Dump(payload));
}

fss::Result<void> WebhookEventPublisher::Post(const std::string& kind,
                                              const std::string& request_body) {
  //  没配 URL：不发起请求（组合根本会拒绝启动，这里是双保险）。
  //  ★ 仍然只返回 Err —— 调用方按非致命处理，绝不因此让请求失败。
  if (options_.url.empty()) {
    const std::string message = "未配置 events.webhook.url，事件未发布（kind=" + kind + "）";
    logging::Warn(logger_, "Failed to publish event: " + message,
                  {{"component", "event_publisher"}, {"kind", kind}});
    return Unavailable(message);
  }

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    const std::string message = "curl_easy_init 失败（kind=" + kind + "）";
    logging::Warn(logger_, "Failed to publish event: " + message,
                  {{"component", "event_publisher"}, {"kind", kind}});
    return Unavailable(message);
  }

  std::string response_body;
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, options_.url.c_str());
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  //  不跟随重定向：通知端点的 3xx 只可能是配置错误，跟随会把事件发到别处
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(options_.connect_timeout_ms));
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

  //  ① 传输层失败（超时/连不上/DNS/TLS）→ 可读告警
  if (code != CURLE_OK) {
    const std::string message = "事件发布失败（传输层，kind=" + kind +
                                "）：" + curl_easy_strerror(code) + "（端点 " + options_.url +
                                "，timeout=" + std::to_string(options_.timeout_ms) + "ms）";
    logging::Warn(logger_, "Failed to publish event: " + message,
                  {{"component", "event_publisher"}, {"kind", kind}});
    return Unavailable(message);
  }
  //  ② 只有 2xx 算成功；3xx/4xx/5xx 都是失败（但**非致命**）
  if (status < 200 || status >= 300) {
    const std::string message = "事件发布失败（非 2xx，kind=" + kind +
                                "）：HTTP " + std::to_string(status) + "（端点 " +
                                options_.url + "）";
    logging::Warn(logger_, "Failed to publish event: " + message,
                  {{"component", "event_publisher"}, {"kind", kind}});
    return Unavailable(message);
  }
  return fss::Ok();
}

}  // namespace fss::infra
