// =============================================================================
//  IClock / SystemClock / ManualClock（L1）
// =============================================================================
//
//  ★ 关于它为什么在 L1 而不是 L3 的 ports/
//    `IClock` 没有任何依赖，也不含领域语义（"现在几点"不是业务规则）。
//    它与 `ILogger` 同类：是**技术抽象**，L1 自己的测试也需要它。
//    设计文档 §5.2 的端口表已标注它的声明位置（见该表说明）。
//    → 若把它放 L3，L2 的实现要为一个"零领域语义"的接口反向依赖 L3，收益为负。
//
//  为什么必须有可注入时钟
//    过期时间、租约到期、签名有效期、`getFileList` 的时间区间过滤，
//    全都依赖"现在"。若代码直接调 std::chrono，这些逻辑就只能靠 sleep 测，
//    既慢又不稳定。`ManualClock` 让它们在微秒级完成且完全确定。
#pragma once

#include <chrono>
#include <cstdint>

namespace fss {

class IClock {
 public:
  virtual ~IClock() = default;
  virtual std::int64_t NowEpochSeconds() const = 0;
  virtual std::int64_t NowEpochMillis() const = 0;
  // 单调时钟（用于测量耗时；不受系统时间被调整的影响）
  virtual std::chrono::steady_clock::time_point NowSteady() const = 0;
};

class SystemClock final : public IClock {
 public:
  std::int64_t NowEpochSeconds() const override;
  std::int64_t NowEpochMillis() const override;
  std::chrono::steady_clock::time_point NowSteady() const override;
};

// 测试用：时间完全由测试驱动。
//   ManualClock c(1700000000);
//   c.AdvanceSeconds(3600);
class ManualClock final : public IClock {
 public:
  explicit ManualClock(std::int64_t epoch_seconds = 1700000000)
      : seconds_(epoch_seconds) {}

  std::int64_t NowEpochSeconds() const override { return seconds_; }
  std::int64_t NowEpochMillis() const override { return seconds_ * 1000 + millis_; }
  std::chrono::steady_clock::time_point NowSteady() const override { return steady_; }

  void SetEpochSeconds(std::int64_t s) { seconds_ = s; }
  void AdvanceSeconds(std::int64_t s) { seconds_ += s; }
  void AdvanceMillis(std::int64_t ms) {
    millis_ += ms;
    seconds_ += millis_ / 1000;
    millis_ %= 1000;
  }
  void AdvanceSteady(std::chrono::milliseconds d) { steady_ += d; }

 private:
  std::int64_t seconds_;
  std::int64_t millis_ = 0;
  std::chrono::steady_clock::time_point steady_{};
};

}  // namespace fss
