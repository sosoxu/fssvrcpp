// C2.3：ExpiryPolicy —— 合法 / 缺省 / 超限截断 / 非法（固定消息逐字节匹配）
//
// 这一条门槛的难点不在"算秒数"，而在**把三类输入严格区分**：
//   合法（原样）、超限（静默截断，**不报错**）、非法（400 + 固定消息）。
// 只测其中两类会得到"实现看起来对"的假象，所以三类都有断言，且非法分支逐一核对**整条消息**。
#include <catch2/catch.hpp>

#include "app/services/expiry_policy.h"

#include <string>
#include <vector>

using fss::app::ExpiryPolicy;
using fss::app::kDefaultExpirySeconds;
using fss::app::kExpiryInvalidMessage;
using fss::app::kMaxExpirySeconds;

namespace {
fss::Result<std::int64_t> Parse(const char* text) {
  return ExpiryPolicy::Parse(std::optional<std::string>(text));
}
}  // namespace

TEST_CASE("★ C2.3 合法输入：M / H / D 三种单位", "[phase2][expiry][contract]") {
  REQUIRE(Parse("5M").value() == 300);
  REQUIRE(Parse("1M").value() == 60);
  REQUIRE(Parse("2H").value() == 7200);
  REQUIRE(Parse("1H").value() == 3600);
  REQUIRE(Parse("1D").value() == 86400);
  REQUIRE(Parse("0M").value() == 0);          // 0 是合法语法，只是立刻过期
  REQUIRE(Parse("10080M").value() == kMaxExpirySeconds);  // 恰好 7 天（边界，不截断）
}

TEST_CASE("★ C2.3 缺省：未提供或空串 → 3600 秒", "[phase2][expiry][contract]") {
  // 上游 `@RequestParam(required=false)`：`?expiryTime=` 拿到空串 → 走缺省分支
  REQUIRE(ExpiryPolicy::Parse(std::nullopt).value() == kDefaultExpirySeconds);
  REQUIRE(Parse("").value() == kDefaultExpirySeconds);
  REQUIRE(kDefaultExpirySeconds == 3600);
}

TEST_CASE("★ C2.3 超限：静默截断为 7 天（**不报错**）", "[phase2][expiry][contract]") {
  // 这条与下面的"非法"必须严格区分：截断是成功路径，非法是错误路径
  const std::vector<std::pair<const char*, std::int64_t>> cases = {
      {"8D", 7 * 86400},
      {"30D", 7 * 86400},
      {"1000000D", 7 * 86400},
      {"169H", 7 * 86400},       // 169H = 7 天 + 1 小时
      {"10081M", 7 * 86400},     // 7 天 + 1 分钟
  };
  for (const auto& [text, expected] : cases) {
    INFO("expiryTime=" << text);
    const auto r = Parse(text);
    REQUIRE(r.ok());  // ★ 截断绝不报错
    REQUIRE(r.value() == expected);
  }
  REQUIRE(kMaxExpirySeconds == 604800);
  REQUIRE(ExpiryPolicy::Cap(999999999) == kMaxExpirySeconds);
}

TEST_CASE("★ C2.3 非法：400 + 固定消息（逐字节匹配）", "[phase2][expiry][contract]") {
  const std::vector<const char*> bad = {
      "5X",     // 单位非法
      "abc",    // 非数字
      "M",      // 只有单位
      "5",      // 只有数字
      "-5M",    // 负数（正则只允许数字）
      "+5M",    // 显式正号
      "5 M",    // 中间有空格
      " 5M",    // 前导空格
      "5M ",    // 尾随空格
      "5m",     // 小写单位
      "5H5",    // 多余字符
      "5.5H",   // 小数
      "0x10M",  // 十六进制形态
      "99999999999999999999D",  // 溢出（必须被拒，不能静默截断成一个奇怪的数）
  };
  for (const char* text : bad) {
    INFO("expiryTime=" << text);
    const auto r = Parse(text);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().kind() == fss::ErrorKind::kInvalidArgument);
    // ★ 固定消息：整条逐字节比对（客户端可能按文案断言）
    REQUIRE(r.error().message() == std::string(kExpiryInvalidMessage));
  }
  // 消息内容本身也要锁定（防止有人"顺手润色"）
  REQUIRE(std::string(kExpiryInvalidMessage) ==
          "expiryTime pattern isn't supported. Value should be one of these regex patterns "
          "^[0-9]+M$ , ^[0-9]+H$ , ^[0-9]+D$");
}

TEST_CASE("ExpiryPolicy::Format：反向格式化（自签 URL 参数用）", "[phase2][expiry]") {
  REQUIRE(ExpiryPolicy::Format(60) == "1M");
  REQUIRE(ExpiryPolicy::Format(300) == "5M");
  REQUIRE(ExpiryPolicy::Format(3600) == "1H");
  REQUIRE(ExpiryPolicy::Format(7200) == "2H");
  REQUIRE(ExpiryPolicy::Format(86400) == "1D");
  REQUIRE(ExpiryPolicy::Format(7 * 86400) == "7D");
  REQUIRE(ExpiryPolicy::Format(8 * 86400) == "7D");  // 截断后再格式化
  REQUIRE(ExpiryPolicy::Format(90) == "2M");         // 90s → 向上取整到分钟
  REQUIRE(ExpiryPolicy::Format(0) == "0M");

  SECTION("往返：Format→Parse 不改变秒数（在整除的前提下）") {
    for (const std::int64_t s : {60, 300, 3600, 7200, 86400, 604800}) {
      REQUIRE(ExpiryPolicy::Parse(std::optional<std::string>(ExpiryPolicy::Format(s))).value() == s);
    }
  }
}
