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

TEST_CASE("RFC 1123（HTTP 日期）", "[phase1][time]") {
  REQUIRE(fss::time::ToRfc1123(1614784413) == "Wed, 03 Mar 2021 15:13:33 GMT");
}
