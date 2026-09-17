// =============================================================================
//  HttpMetrics（L5 适配层）—— `/metrics` 的最小可用实现（契约 §7 扩展）
// =============================================================================
//  设计依据：`docs/02-design.md` §"指标"（按端点/状态码的请求计数与延迟直方图）。
//  本阶段只做**最小可用**：请求计数 + 延迟直方图 + 传输 token 拒绝数 + 服务器
//  自身的在途/背压计数；存储操作计数、GC 删除数等属于 P9（见证据文件的未验证项）。
//
//  ★ 输出格式：**Prometheus 文本**（`text/plain; version=0.0.4`）。
//    选它的理由：生态标准、可被现有抓取器直接消费；不引入任何依赖。
//
//  ★ 为什么 `/metrics` 不在 `/api/file` 下：契约 §7 要求扩展必须与 OSDU 命名空间
//    隔离（`/metrics` 是独立路径），否则会污染 OSDU 的路径空间。
//
//  ⚠️ label 值必须**转义**（`\` `"` 换行）：路由名/方法来自请求侧，虽然当前都来自
//     我们自己的代码，但"手写转义必须有参照实现并逐字节验证"是仓库铁律（R18）。
#pragma once

#include "common/http/http.h"

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace fss::adapters::http {

//  直方图上界（秒）。与 `kBucketLabels` 一一对应，末尾隐含 `+Inf`。
inline constexpr std::array<double, 9> kDurationBuckets = {0.001, 0.005, 0.010, 0.050,
                                                           0.100, 0.500, 1.000, 5.000,
                                                           10.000};

class HttpMetrics {
 public:
  //  记一次已完成请求。`route` 为空时用 `"<unmatched>"`（不静默丢计数）
  void Observe(std::string_view route, std::string_view method, int status, double seconds);

  //  自签传输 token 校验失败（签名错/过期/用途不符）
  void RecordTransferTokenRejected() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++transfer_token_rejected_;
  }

  //  渲染 Prometheus 文本。`stats` 是 HTTP 服务器自身的计数（在途/背压/拒绝）
  std::string RenderPrometheus(const fss::http::Stats& stats) const;

  //  某个 (route, method, status) 的计数。给测试用：让"内存里的计数"与"渲染出的文本"
  //  成为两条可互相印证的观测路径（R1）
  std::uint64_t RequestCount(std::string_view route, std::string_view method,
                             int status) const;
  //  (route, method) 的直方图样本数
  std::uint64_t DurationCount(std::string_view route, std::string_view method) const;

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.clear();
    durations_.clear();
    transfer_token_rejected_ = 0;
  }

 private:
  struct Histogram {
    std::array<std::uint64_t, kDurationBuckets.size() + 1> buckets{};  // 末位 = +Inf
    double sum = 0;
    std::uint64_t count = 0;
  };

  mutable std::mutex mutex_;
  //  用有序容器：输出顺序稳定，黄金比对不需要"忽略顺序"
  std::map<std::tuple<std::string, std::string, int>, std::uint64_t> requests_;
  std::map<std::pair<std::string, std::string>, Histogram> durations_;
  std::uint64_t transfer_token_rejected_ = 0;
};

//  Prometheus label 值转义（`\` → `\\`、`"` → `\"`、换行 → `\n`）
std::string EscapeLabelValue(std::string_view value);

}  // namespace fss::adapters::http
