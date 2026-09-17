// C1.10（部分）：common/time —— 可注入时钟与三种时间格式
#include <catch2/catch.hpp>

#include "common/time/clock.h"
#include "common/time/time_format.h"

#include <string>

TEST_CASE("ManualClock：时间完全由测试驱动（过期/租约逻辑可精确单测）",
          "[phase1][time]") {
  fss::ManualClock clock(1700000000);
  REQUIRE(clock.NowEpochSeconds() == 1700000000);
  REQUIRE(clock.NowEpochMillis() == 1700000000000LL);

  clock.AdvanceSeconds(3600);
  REQUIRE(clock.NowEpochSeconds() == 1700003600);

  clock.SetEpochSeconds(42);
  REQUIRE(clock.NowEpochSeconds() == 42);

  // 毫秒进位必须正确（不能只加秒而丢掉毫秒）
  fss::ManualClock c2(0);
  c2.AdvanceMillis(1500);
  REQUIRE(c2.NowEpochSeconds() == 1);
  REQUIRE(c2.NowEpochMillis() == 1500);
  c2.AdvanceMillis(600);
  REQUIRE(c2.NowEpochSeconds() == 2);
  REQUIRE(c2.NowEpochMillis() == 2100);

  // 单调时钟可独立推进（用于测量耗时，不受系统时间调整影响）
  const auto before = c2.NowSteady();
  c2.AdvanceSteady(std::chrono::milliseconds(250));
  REQUIRE(c2.NowSteady() > before);
}

TEST_CASE("IClock 多态可用（实现可替换）", "[phase1][time]") {
  fss::SystemClock sys;
  REQUIRE(sys.NowEpochSeconds() > 1600000000);  // 晚于 2020-09
  REQUIRE(sys.NowEpochMillis() > 1600000000000LL);

  // 通过基类指针使用（这正是应用层依赖端口的方式）
  fss::IClock* clk = new fss::ManualClock(100);
  REQUIRE(clk->NowEpochSeconds() == 100);
  delete clk;
}

TEST_CASE("★ OSDU 的 CreatedAt 格式：末尾是 +0000，不是 Z（契约 §2.5）",
          "[phase1][time][contract]") {
  // 契约 §2.5 的黄金样例
  const std::int64_t epoch = 1614784413;  // 2021-03-03T15:13:33Z
  const std::string s = fss::time::ToOsduTimestamp(epoch, 120);
  REQUIRE(s == "2021-03-03T15:13:33.120+0000");
  REQUIRE(s.back() != 'Z');  // ★ 回归断言：写成 Z 会让对齐上游的客户端解析失败

  // 解析回同样的时刻
  auto back = fss::time::ParseOsduTimestamp(s);
  REQUIRE(back.ok());
  REQUIRE(back.value() == epoch);
}

TEST_CASE("ISO-8601 UTC：格式与解析往返", "[phase1][time]") {
  const std::int64_t epoch = 1614784413;
  const std::string s = fss::time::ToIso8601Utc(epoch, 120);
  REQUIRE(s == "2021-03-03T15:13:33.120Z");

  auto back = fss::time::ParseIso8601(s);
  REQUIRE(back.ok());
  REQUIRE(back.value() == epoch);

  // 不带毫秒也能解析
  auto nomillis = fss::time::ParseIso8601("2021-03-03T15:13:33Z");
  REQUIRE(nomillis.ok());
  REQUIRE(nomillis.value() == epoch);
}

TEST_CASE("时区偏移处理：+HH:MM、+HHMM、-HHMM 都归一到 UTC", "[phase1][time]") {
  const std::int64_t utc = 1614784413;  // 15:13:33Z
  // 东八区同一时刻的本地时间是 23:13:33
  auto plus8_colon = fss::time::ParseIso8601("2021-03-03T23:13:33+08:00");
  REQUIRE(plus8_colon.ok());
  REQUIRE(plus8_colon.value() == utc);

  auto plus8_plain = fss::time::ParseIso8601("2021-03-03T23:13:33+0800");
  REQUIRE(plus8_plain.ok());
  REQUIRE(plus8_plain.value() == utc);

  // 西五区同一时刻是 10:13:33
  auto minus5 = fss::time::ParseIso8601("2021-03-03T10:13:33-0500");
  REQUIRE(minus5.ok());
  REQUIRE(minus5.value() == utc);
}

TEST_CASE("非法时间输入必须明确失败（不静默返回 0）", "[phase1][time]") {
  for (const char* bad : {"", "not-a-time", "2021-03-03", "2021-03-03T15:13",
                          "2021/03/03T15:13:33Z", "2021-03-03T15:13:33X"}) {
    INFO("应被拒: " << bad);
    REQUIRE_FALSE(fss::time::ParseIso8601(bad).ok());
  }
}

TEST_CASE("★ 越界/形状不符的时间必须被拒（timegm 会静默归一化，P6-D10）", "[phase1][time]") {
  //  ⚠️ `timegm` 对越界字段是**归一化**而不是报错：月份 13 → 次年、2 月 30 → 3 月 2 日、
  //     小时 99 → +4 天。若不显式挡，`2020-13-45T99:99:99Z` 会变成一个"看似合理"的错误
  //     时刻，用它当过滤边界会**静默筛错数据**。
  for (const char* bad : {
           "2020-13-01T00:00:00Z",   // 月份越界
           "2020-00-01T00:00:00Z",   // 月份 0
           "2020-01-45T00:00:00Z",   // 日越界
           "2020-01-00T00:00:00Z",   // 日 0
           "2021-02-29T00:00:00Z",   // 2021 不是闰年
           "2020-01-01T99:00:00Z",   // 小时越界
           "2020-01-01T00:99:00Z",   // 分钟越界
           "2020-01-01T00:00:99Z",   // 秒越界
           "2020-1-01T00:00:00Z",    // 形状：月份必须 2 位
           "2020-01-1T00:00:00Z",    // 形状：日必须 2 位
           "2020-01-01T00:00:00+25:00",  // 时区偏移越界
       }) {
    INFO("应被拒: " << bad);
    REQUIRE_FALSE(fss::time::ParseIso8601(bad).ok());
  }

  //  正例对照（R16）：闰年的 2 月 29 日必须**能**解析 ——
  //  否则上面的"日期越界"断言无法区分"校验正确"与"校验恒真"
  const auto leap = fss::time::ParseIso8601("2020-02-29T00:00:00Z");
  REQUIRE(leap.ok());
  const auto normal = fss::time::ParseIso8601("2021-02-28T00:00:00Z");
  REQUIRE(normal.ok());
  //  2020-02-29 → 2021-02-28 恰好 365 天（证明闰日被真正接受，且没有偷偷归一化到 3 月）
  REQUIRE(normal.value() - leap.value() == 365LL * 86400);
}

TEST_CASE("RFC 1123（HTTP 日期）", "[phase1][time]") {
  REQUIRE(fss::time::ToRfc1123(1614784413) == "Wed, 03 Mar 2021 15:13:33 GMT");
}
