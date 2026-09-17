// C2.5：KindValidator —— 语法 + 语义 + 三条固定消息
#include <catch2/catch.hpp>

#include "app/services/kind_validator.h"

#include <string>
#include <string_view>
#include <vector>

using fss::app::KindValidator;
using fss::app::kInvalidKindEntityMessage;
using fss::app::kInvalidKindMessage;
using fss::app::kInvalidKindSourceMessage;

TEST_CASE("★ C2.5 合法 kind（≥6 例）", "[phase2][kind][contract]") {
  const std::vector<const char*> good = {
      "opendes:wks:dataset--File.Generic:1.0.0",
      "opendes:wks:dataset--File.Generic:1.0.0",  // 与黄金样例一致
      "tenant-a:wks:dataset--File.Generic:2.3.4",
      "osdu:wks:dataset--File.Generic:0.0.1",
      "A1:wks:dataset--File.Generic:10.20.30",
      "x.y:wks:dataset--File.Generic:1.0.0",      // 段内允许 '.' 与 '-'
      "part_1:wks:dataset--File.Generic:1.0.0",
  };
  for (const char* kind : good) {
    INFO("kind=" << kind);
    const auto r = KindValidator::Validate(kind);
    REQUIRE(r.ok());
    REQUIRE(r.value().entity == "dataset--File.Generic");
    REQUIRE(r.value().source == "wks");
    REQUIRE_FALSE(r.value().partition.empty());
    REQUIRE_FALSE(r.value().version.empty());
  }
  // 黄金样例的 kind 必须通过与解析
  auto golden = KindValidator::Validate("opendes:wks:dataset--File.Generic:1.0.0");
  REQUIRE(golden.ok());
  REQUIRE(golden.value().partition == "opendes");
  REQUIRE(golden.value().version == "1.0.0");
}

TEST_CASE("★ C2.5 非法 kind（≥6 例）与固定消息", "[phase2][kind][contract]") {
  // 期望消息分三类：语法/结构 → Invalid kind；source 错 → Invalid source in kind；
  //                entity 错 → Invalid entity in kind
  struct Case {
    const char* kind;
    std::string_view message;
  };
  const std::vector<Case> bad = {
      // 语法不匹配
      {"", kInvalidKindMessage},
      {"opendes", kInvalidKindMessage},
      {"opendes:wks:dataset--File.Generic", kInvalidKindMessage},          // 只有 3 段
      {"opendes:wks:dataset--File.Generic:1.0.0:extra", kInvalidKindMessage},  // 5 段
      {"opendes:wks:dataset--File.Generic:1.0", kInvalidKindMessage},      // 版本两段
      {"opendes:wks:dataset--File.Generic:v1.0.0", kInvalidKindMessage},   // 版本非数字
      {"opendes wks:dataset--File.Generic:1.0.0", kInvalidKindMessage},    // 空格
      {"opendes:wks:dataset--File.Generic:1.0.0\n", kInvalidKindMessage},  // 尾随换行
      // 语义错误（语法合法）
      {"opendes:notwks:dataset--File.Generic:1.0.0", kInvalidKindSourceMessage},
      {"opendes:wks:dataset--Something.Else:1.0.0", kInvalidKindEntityMessage},
  };
  for (const auto& c : bad) {
    INFO("kind=" << c.kind);
    const auto r = KindValidator::Validate(c.kind);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().kind() == fss::ErrorKind::kInvalidArgument);
    REQUIRE(r.error().message() == std::string(c.message));
  }
}

TEST_CASE("KindValidator：语法与语义两段式，行为一致", "[phase2][kind]") {
  REQUIRE(KindValidator::MatchesSyntax("opendes:wks:dataset--File.Generic:1.0.0"));
  REQUIRE_FALSE(KindValidator::MatchesSyntax("opendes:wks:dataset--File.Generic"));
  // 已知的宽松点：上游正则里版本段的 '.' 未转义 → `1x0x0` 也能过语法检查。
  // 这里**如实锁定**该行为（改了会让上游样例行为不同），并在注释里说明。
  const bool loose_but_expected = KindValidator::MatchesSyntax("opendes:wks:dataset--File.Generic:1x0x0");
  REQUIRE(loose_but_expected);
}
