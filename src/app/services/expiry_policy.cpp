#include "app/services/expiry_policy.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace fss::app {
namespace {

//  严格解析：整串必须是"数字 + 单个单位字符"。
//  ★ 不用 strtoll 的"部分解析"：`5Mx`、` 5M`、`+5M`、`5 M` 必须是非法，
//    否则会把客户端的拼写错误静默当成合法值（这类宽松是契约里最贵的错误之一）。
bool ParseDigits(std::string_view text, std::int64_t* out) {
  if (text.empty() || text.size() > 18) return false;  // 上限防溢出
  std::int64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    const auto digit = static_cast<std::int64_t>(c - '0');
    if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) return false;
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

}  // namespace

std::int64_t ExpiryPolicy::Cap(std::int64_t seconds) {
  if (seconds < 0) return 0;
  return std::min(seconds, kMaxExpirySeconds);
}

Result<std::int64_t> ExpiryPolicy::Parse(const std::optional<std::string>& raw) {
  if (!raw.has_value() || raw->empty()) return kDefaultExpirySeconds;  // 缺省 1 小时

  const std::string_view text = *raw;
  const char unit = text.back();
  const std::string_view digits = text.substr(0, text.size() - 1);

  std::int64_t value = 0;
  if (!ParseDigits(digits, &value)) {
    return Err(ErrorKind::kInvalidArgument, std::string(kExpiryInvalidMessage));
  }

  std::int64_t seconds = 0;
  switch (unit) {
    case 'M':
      seconds = value * 60;
      break;
    case 'H':
      seconds = value * 3600;
      break;
    case 'D':
      seconds = value * 86400;
      break;
    default:
      return Err(ErrorKind::kInvalidArgument, std::string(kExpiryInvalidMessage));
  }
  //  ★ 超限**静默截断**（契约 §1.4）：不报错。这里必须与"非法"分开处理。
  return Cap(seconds);
}

std::string ExpiryPolicy::Format(std::int64_t seconds) {
  const auto capped = Cap(seconds);
  if (capped > 0 && capped % 86400 == 0) return std::to_string(capped / 86400) + "D";
  if (capped > 0 && capped % 3600 == 0) return std::to_string(capped / 3600) + "H";
  if (capped > 0 && capped % 60 == 0) return std::to_string(capped / 60) + "M";
  return std::to_string(capped / 60 + (capped % 60 != 0 ? 1 : 0)) + "M";  // 向上取整到分钟
}

}  // namespace fss::app
