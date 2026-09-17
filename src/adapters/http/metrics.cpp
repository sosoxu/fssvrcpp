// HttpMetrics 实现（Prometheus 文本输出）。设计说明见头文件。
#include "adapters/http/metrics.h"

#include <cstdio>

namespace fss::adapters::http {

namespace {

//  `le` 标签的渲染：**固定 3 位小数**，避免 `%g` 在不同平台上给出
//  `0.001` / `1e-03` 两种写法（抓取器能接受，但黄金比对会漂）
std::string FormatBound(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.3f", value);
  return buffer;
}

std::string FormatSum(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.6f", value);
  return buffer;
}

}  // namespace

std::string EscapeLabelValue(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      default:
        out += ch;
    }
  }
  return out;
}

void HttpMetrics::Observe(std::string_view route, std::string_view method, int status,
                          double seconds) {
  const std::string route_key = route.empty() ? std::string("<unmatched>") : std::string(route);
  const std::string method_key = method.empty() ? std::string("<unknown>") : std::string(method);
  if (seconds < 0) seconds = 0;

  std::lock_guard<std::mutex> lock(mutex_);
  ++requests_[std::make_tuple(route_key, method_key, status)];

  auto& histogram = durations_[std::make_pair(route_key, method_key)];
  ++histogram.count;
  histogram.sum += seconds;
  std::size_t index = 0;
  for (; index < kDurationBuckets.size(); ++index) {
    if (seconds <= kDurationBuckets[index]) break;
  }
  //  累加式（Prometheus 的 bucket 语义是 `le` 累积），从命中的那一位开始全部 +1
  for (std::size_t i = index; i < histogram.buckets.size(); ++i) ++histogram.buckets[i];
}

std::uint64_t HttpMetrics::RequestCount(std::string_view route, std::string_view method,
                                         int status) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = requests_.find(std::make_tuple(std::string(route), std::string(method), status));
  return it == requests_.end() ? 0 : it->second;
}

std::uint64_t HttpMetrics::DurationCount(std::string_view route, std::string_view method) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = durations_.find(std::make_pair(std::string(route), std::string(method)));
  return it == durations_.end() ? 0 : it->second.count;
}

std::string HttpMetrics::RenderPrometheus(const fss::http::Stats& stats) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string out;
  out.reserve(2048);

  out += "# HELP fss_http_requests_total HTTP 请求总数（按路由/方法/状态码）\n";
  out += "# TYPE fss_http_requests_total counter\n";
  for (const auto& [key, count] : requests_) {
    const auto& [route, method, status] = key;
    out += "fss_http_requests_total{route=\"" + EscapeLabelValue(route) + "\",method=\"" +
           EscapeLabelValue(method) + "\",status=\"" + std::to_string(status) + "\"} " +
           std::to_string(count) + "\n";
  }

  out += "# HELP fss_http_request_duration_seconds HTTP 请求耗时（按路由/方法）\n";
  out += "# TYPE fss_http_request_duration_seconds histogram\n";
  for (const auto& [key, histogram] : durations_) {
    const auto& [route, method] = key;
    const std::string labels = "route=\"" + EscapeLabelValue(route) + "\",method=\"" +
                               EscapeLabelValue(method) + "\"";
    for (std::size_t i = 0; i < kDurationBuckets.size(); ++i) {
      out += "fss_http_request_duration_seconds_bucket{" + labels + ",le=\"" +
             FormatBound(kDurationBuckets[i]) + "\"} " + std::to_string(histogram.buckets[i]) +
             "\n";
    }
    out += "fss_http_request_duration_seconds_bucket{" + labels + ",le=\"+Inf\"} " +
           std::to_string(histogram.buckets.back()) + "\n";
    out += "fss_http_request_duration_seconds_sum{" + labels + "} " +
           FormatSum(histogram.sum) + "\n";
    out += "fss_http_request_duration_seconds_count{" + labels + "} " +
           std::to_string(histogram.count) + "\n";
  }

  out += "# HELP fss_transfer_token_rejected_total 自签传输 token 被拒次数\n";
  out += "# TYPE fss_transfer_token_rejected_total counter\n";
  out += "fss_transfer_token_rejected_total " + std::to_string(transfer_token_rejected_) + "\n";

  //  ---- 服务器自身的计数（背压/在途/拒绝；ADR-010 的"探测结果必须可见"）----
  out += "# HELP fss_http_in_flight_requests 当前在途请求数\n";
  out += "# TYPE fss_http_in_flight_requests gauge\n";
  out += "fss_http_in_flight_requests " + std::to_string(stats.in_flight) + "\n";
  out += "# HELP fss_http_peak_in_flight_requests 在途请求峰值\n";
  out += "# TYPE fss_http_peak_in_flight_requests gauge\n";
  out += "fss_http_peak_in_flight_requests " + std::to_string(stats.peak_in_flight) + "\n";
  out += "# HELP fss_http_worker_threads 工作线程数（= 并发连接上限）\n";
  out += "# TYPE fss_http_worker_threads gauge\n";
  out += "fss_http_worker_threads " + std::to_string(stats.worker_threads) + "\n";
  out += "# HELP fss_http_max_connections 允许的最大并发连接数（C1.12 不变量）\n";
  out += "# TYPE fss_http_max_connections gauge\n";
  out += "fss_http_max_connections " + std::to_string(stats.max_connections) + "\n";

  out += "# HELP fss_http_rejected_total 被拒绝的请求（按原因）\n";
  out += "# TYPE fss_http_rejected_total counter\n";
  out += "fss_http_rejected_total{reason=\"busy\"} " + std::to_string(stats.rejected_busy) + "\n";
  out += "fss_http_rejected_total{reason=\"too_large\"} " +
         std::to_string(stats.rejected_too_large) + "\n";
  out += "fss_http_rejected_total{reason=\"body_limit_aborted\"} " +
         std::to_string(stats.body_limit_aborted) + "\n";
  out += "fss_http_rejected_total{reason=\"length_mismatch\"} " +
         std::to_string(stats.body_length_mismatch) + "\n";
  out += "fss_http_rejected_total{reason=\"not_found\"} " + std::to_string(stats.not_found) + "\n";
  return out;
}

}  // namespace fss::adapters::http
