// =============================================================================
//  KindValidator（L4）：OSDU `kind` 的语法与语义校验（契约 §1.5 / §3.4）
// =============================================================================
//  两条独立的检查，**顺序与文案都必须与上游一致**：
//    ① 语法：`^[\w\-\.]+:[\w\-\.]+:[\w\-\.]+:[0-9]+.[0-9]+.[0-9]+$` → 不符 → `Invalid kind`
//    ② 语义：按 `:` 切成**恰好 4 段**，且
//         [1] == "wks"                    否则 → `Invalid source in kind`
//         [2] == "dataset--File.Generic"  否则 → `Invalid entity in kind`
//
//  ⚠️ 注意正则里版本段的 `.` **没有转义**（上游原样如此）：这意味着
//     `1x0x0` 这种也能过语法检查。我们**如实保留**这个宽松行为（契约要求逐字对齐上游），
//     但把它记录在这里 —— 这不是我们可以"顺手修正"的地方（改了会让上游样例行为不同）。
#pragma once

#include "common/result/result.h"

#include <string>
#include <string_view>
#include <vector>

namespace fss::app {

// 契约里出现的三条固定消息（逐字节匹配）
inline constexpr std::string_view kInvalidKindMessage = "Invalid kind";
inline constexpr std::string_view kInvalidKindSourceMessage = "Invalid source in kind";
inline constexpr std::string_view kInvalidKindEntityMessage = "Invalid entity in kind";

struct KindParts {
  std::string partition;
  std::string source;   // 期望 "wks"
  std::string entity;   // 期望 "dataset--File.Generic"
  std::string version;  // `x.y.z`
};

class KindValidator {
 public:
  //  完整校验（语法 + 语义）。失败时 error().message() 是上述三条固定消息之一。
  static Result<KindParts> Validate(std::string_view kind);

  //  只做语法检查（供"先语法后语义"的两段式调用；行为与 Validate 一致）
  static bool MatchesSyntax(std::string_view kind);

  //  OSDU `dataset--File.Generic` 的固定实体名与来源
  static constexpr std::string_view kFileGenericEntity = "dataset--File.Generic";
  static constexpr std::string_view kWorkspaceSource = "wks";
};

}  // namespace fss::app
