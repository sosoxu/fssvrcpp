// =============================================================================
//  ExpiryPolicy（L4）：`expiryTime` 的解析与截断（契约 §1.4）
// =============================================================================
//  权威依据是**实测的上游行为**（`ExpiryTimeUtil`）：
//    语法    `^[0-9]+(M|H|D)$`
//    缺省    1 小时（DEFAULT_TTL = 1 HOURS）
//    上限    7 天（CAPPED_DEFAULT_TTL = 7 DAYS）
//    超限    **静默截断**为 7 天（不报错）
//    非法    400 + **固定消息**（逐字节匹配，客户端可能按文案断言）
//
//  ⚠️ "静默截断"这类宽容行为必须与"非法输入"严格区分：前者不能报错，后者必须报错。
//     测试对两者都有断言 —— 只测一边会得到"实现看起来对"的假象（R16）。
#pragma once

#include "common/result/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fss::app {

inline constexpr std::int64_t kDefaultExpirySeconds = 3600;      // 1 小时
inline constexpr std::int64_t kMaxExpirySeconds = 7 * 86400;     // 7 天

//  非法时返回的**固定消息**（契约 §1.4，逐字节匹配）
inline constexpr std::string_view kExpiryInvalidMessage =
    "expiryTime pattern isn't supported. Value should be one of these regex patterns "
    "^[0-9]+M$ , ^[0-9]+H$ , ^[0-9]+D$";

class ExpiryPolicy {
 public:
  //  解析 query 参数 `expiryTime`：
  //    · 未提供（nullopt）或空串 → 缺省 3600
  //    · 合法            → 对应秒数，且**超过 7 天时截断为 7 天**
  //    · 非法            → kInvalidArgument + kExpiryInvalidMessage
  //  ★ 为什么空串按"未提供"处理：上游用 `@RequestParam(required=false)`，
  //    `?expiryTime=` 在 Spring 里拿到的是空串，实测走的是缺省分支。
  static Result<std::int64_t> Parse(const std::optional<std::string>& raw);

  //  已经是秒数时的截断（供内部与测试使用）
  static std::int64_t Cap(std::int64_t seconds);

  //  反向：把秒数格式化为 `<n>M|H|D`（仅在能整除时；否则用秒→小时向上取整）
  //  用于自签 URL 的参数与日志。**不**用于回显给客户端（契约里没有这个回显）。
  static std::string Format(std::int64_t seconds);
};

}  // namespace fss::app
