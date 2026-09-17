#include "common/time/clock.h"

#include <ctime>

namespace fss {
namespace {
std::int64_t ToEpoch(std::chrono::system_clock::time_point tp) {
  return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}
}  // namespace

std::int64_t SystemClock::NowEpochSeconds() const { return ToEpoch(std::chrono::system_clock::now()); }

std::int64_t SystemClock::NowEpochMillis() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::chrono::steady_clock::time_point SystemClock::NowSteady() const {
  return std::chrono::steady_clock::now();
}

}  // namespace fss
