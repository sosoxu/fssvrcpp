// =============================================================================
//  ClockSkewGuard（L4 纯策略）—— "哪个时钟说了算"的显式判决
// =============================================================================
//  C8.10 的要求：模拟快钟/慢钟实例 → 租约与 token 过期**不误判**（租约判定一律用
//  PG 的 `now()`）。落到实现上是一条纪律 + 一个判决：
//
//    · **参考时钟**（多实例下 = 数据库时钟）是唯一可用于 TTL/租约/过期判定的时间源；
//    · 本地时钟只用来**比对**：偏差超过容忍范围 → 拒绝启动/拒绝服务（fail-closed），
//      而不是"继续用本地时间" —— 用本地钟做租约判定会让快钟实例误删别人的在途上传
//      （ADR-009 §R-25）。
//
//  ★ 为什么单独成一层：把"用哪个钟"写成一条可测的策略，比散落在各处的
//    `clock_.Now()` 更可靠 —— 那些调用点无法回答"这是本地钟还是参考钟"。
// =============================================================================
#pragma once

#include "common/result/result.h"
#include "common/time/clock.h"

#include <cstdint>
#include <string>

namespace fss::app {

class ClockSkewGuard {
 public:
  //  `reference` 是权威时间源（PG 的 `now()` 或单实例下的同一时钟）；
  //  `tolerance_seconds` 来自 `deployment.max_clock_skew_seconds`。
  ClockSkewGuard(const fss::IClock& local, const fss::IClock& reference,
                 std::int64_t tolerance_seconds)
      : local_(local), reference_(reference), tolerance_(tolerance_seconds) {}

  //  偏差绝对值超过容忍范围 → `kUnavailable`（多实例下由组合根在启动时调用一次）
  fss::Result<void> Check() const;
  //  实际偏差（本地 - 参考，秒；正数 = 本地快）
  std::int64_t SkewSeconds() const;
  //  ★ TTL/租约/过期判定必须用它（参考时钟），不要用本地钟
  std::int64_t NowEpochSeconds() const { return reference_.NowEpochSeconds(); }
  std::int64_t ToleranceSeconds() const { return tolerance_; }

 private:
  const fss::IClock& local_;
  const fss::IClock& reference_;
  std::int64_t tolerance_ = 5;
};

}  // namespace fss::app
