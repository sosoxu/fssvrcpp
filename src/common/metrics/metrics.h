// =============================================================================
//  MetricsRegistry（L1）—— 与协议无关的计数器/仪表（Prometheus 文本渲染）
// =============================================================================
//  为什么要有它（P9 的 C9.6）
//    HTTP 适配层自己有一套 `HttpMetrics`（请求计数/延迟直方图），但"存储操作""字节数"
//    "GC 删除数"这些发生在**更下层**（L2/L4）。让它们各自往 HTTP 适配层里塞计数会
//    把依赖方向搞反，于是抽出一个 L1 的通用注册表：
//      · L2 的 `MeteredBlobStore` 记存储操作与字节；
//      · L4 的 `GcTask` 记扫描/删除/跳过；
//      · L5 的 `/metrics` 把两者渲染进同一份文本。
//
//  ★ 只做"计数 + 仪表"，不做直方图（延迟直方图留在 HttpMetrics：那里才有路由标签）。
//  ★ 线程安全：多服务线程会并发 `Increment`（P9-D01 的教训）。
//  ★ 指标名与 help 必须**显式注册**：这样 `/metrics` 的 HELP/TYPE 行是稳定的，
//    测试可以断言"每个 family 都有 HELP/TYPE"（Prometheus 抓取器的要求）。
// =============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fss::metrics {

//  标签集合：用 `std::map` 保证渲染顺序稳定（测试逐行比对）
using Labels = std::map<std::string, std::string>;

//  Prometheus label 值转义（`\` → `\\`、`"` → `\"`、换行 → `\n`）。
//  ★ 手写转义必须有参照实现（R18）：`tests/unit/test_metrics.cpp` 用穷举比对钉住它。
std::string EscapeLabelValue(std::string_view value);

class Registry {
 public:
  enum class Kind { kCounter, kGauge };

  //  注册（幂等）：`name` + `help` + 类型。未注册就使用的名字仍能被记录，
  //  但渲染时**没有** HELP/TYPE —— 测试会因此失败（防止"静默新增指标"）。
  void Register(std::string_view name, Kind kind, std::string_view help);

  void Increment(std::string_view name, const Labels& labels = {}, std::int64_t delta = 1);
  void SetGauge(std::string_view name, std::int64_t value, const Labels& labels = {});

  //  当前值（测试用：让"内存里的值"与"渲染出的文本"互为印证，R1）
  std::int64_t Value(std::string_view name, const Labels& labels = {}) const;

  //  Prometheus 文本格式（含 HELP/TYPE；空注册表返回空串）
  std::string RenderPrometheus() const;

  void Reset();

 private:
  struct Family {
    Kind kind = Kind::kCounter;
    std::string help;
  };
  struct Sample {
    std::int64_t value = 0;
  };

  static std::string SampleKey(std::string_view name, const Labels& labels);

  mutable std::mutex mutex_;
  std::map<std::string, Family> families_;
  std::map<std::string, Sample> samples_;
  //  渲染顺序：(name, labels) 的稳定序 —— 直接用 `samples_` 的 key 排序即可
  std::map<std::string, std::pair<std::string, Labels>> sample_index_;
};

}  // namespace fss::metrics
