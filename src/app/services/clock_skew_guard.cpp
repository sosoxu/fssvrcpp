// ClockSkewGuard 实现。策略与理由见头文件。
#include "app/services/clock_skew_guard.h"

#include <cstdlib>
#include <string>

namespace fss::app {

std::int64_t ClockSkewGuard::SkewSeconds() const {
  return local_.NowEpochSeconds() - reference_.NowEpochSeconds();
}

fss::Result<void> ClockSkewGuard::Check() const {
  const std::int64_t skew = SkewSeconds();
  if (std::llabs(skew) > tolerance_) {
    return Err(fss::ErrorKind::kUnavailable,
               "实例时钟与参考时钟（数据库 now()）偏差 " + std::to_string(skew) +
                   " 秒，超过容忍范围 " + std::to_string(tolerance_) +
                   " 秒 —— 拒绝服务（用本地钟做租约/过期判定会误删在途数据，ADR-009）");
  }
  return Ok();
}

}  // namespace fss::app
