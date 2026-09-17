// =============================================================================
//  P8 切片 3：时钟偏移（C8.10）—— 快钟/慢钟实例不得误判租约与过期
// =============================================================================
//  判据要求"模拟快钟/慢钟实例 → 租约与 token 过期**不误判**"。
//  可机械验证的部分（本文件）：
//    · 参考时钟（多实例下 = 数据库 `now()`）是 TTL/租约/过期判定的**唯一**时间源；
//    · 本地钟与参考钟的偏差超过容忍范围 → fail-closed（而不是"继续用本地时间"）；
//    · token 的 `exp` 判定带容忍范围：快/慢 3 秒仍接受、10 秒拒绝（`test_jwt_authorizer`）。
//  ⚠️ 端到端形态（两个实例 + PG `now()`）需要 PG 版租约仓储 —— 尚未交付（P9），
//     已在 ADR-009 / 证据文件登记。
// =============================================================================
#include <catch2/catch.hpp>

#include "app/services/clock_skew_guard.h"
#include "common/time/clock.h"

namespace {

using fss::app::ClockSkewGuard;

constexpr std::int64_t kReference = 1700000000;

}  // namespace

TEST_CASE("★ C8.10 参考时钟是 TTL/过期判定的唯一时间源（本地钟只用来比对）",
          "[phase8][unit][c8.10]") {
  fss::ManualClock local(kReference + 2);      // 快 2 秒
  fss::ManualClock reference(kReference);
  ClockSkewGuard guard(local, reference, /*tolerance_seconds=*/5);

  REQUIRE(guard.SkewSeconds() == 2);
  REQUIRE(guard.Check().ok());
  //  ★ 关键性质：判定用的时间是**参考钟**，不是本地钟（否则快钟实例会提前判过期）
  REQUIRE(guard.NowEpochSeconds() == reference.NowEpochSeconds());
  REQUIRE(guard.NowEpochSeconds() != local.NowEpochSeconds());

  //  参考钟前进，本地钟不动 → 判定时间跟着参考钟走
  reference.SetEpochSeconds(kReference + 100);
  REQUIRE(guard.NowEpochSeconds() == kReference + 100);
  REQUIRE(guard.SkewSeconds() == -98);
}

TEST_CASE("★ C8.10 快钟/慢钟超出容忍范围 → 拒绝服务（fail-closed）", "[phase8][unit][c8.10]") {
  fss::ManualClock reference(kReference);

  //  ① 快钟：+30 秒 > 5 秒
  {
    fss::ManualClock local(kReference + 30);
    ClockSkewGuard guard(local, reference, 5);
    const auto result = guard.Check();
    INFO("快钟：" << result.error().message());
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kUnavailable);
    REQUIRE(result.error().message().find("30") != std::string::npos);
  }
  //  ② 慢钟：-30 秒
  {
    fss::ManualClock local(kReference - 30);
    ClockSkewGuard guard(local, reference, 5);
    REQUIRE_FALSE(guard.Check().ok());
    REQUIRE(guard.SkewSeconds() == -30);
  }
  //  ③ 边界：恰好等于容忍范围 → 接受；多 1 秒 → 拒绝
  {
    fss::ManualClock local(kReference + 5);
    REQUIRE(ClockSkewGuard(local, reference, 5).Check().ok());
    fss::ManualClock local_over(kReference + 6);
    REQUIRE_FALSE(ClockSkewGuard(local_over, reference, 5).Check().ok());
  }
  //  ④ 容忍范围放宽到 60 秒 → 30 秒的偏差就通过了（容忍范围是显式配置，不是魔数）
  {
    fss::ManualClock local(kReference + 30);
    REQUIRE(ClockSkewGuard(local, reference, 60).Check().ok());
  }
}
