#include "common/time/time_format.h"

#include <ctime>

namespace fss::time {
namespace {

std::string Format(std::int64_t epoch_seconds, int millis, const char* fmt) {
  const std::time_t t = static_cast<std::time_t>(epoch_seconds);
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  char buf[64];
  const std::size_t n = ::strftime(buf, sizeof(buf), fmt, &tm);
  std::string out(buf, n);
  // 把 "%f" 占位替换成毫秒（strftime 不支持 %f）
  const auto pos = out.find("%f");
  if (pos != std::string::npos) {
    char ms[8];
    ::snprintf(ms, sizeof(ms), "%03d", millis % 1000);
    out.replace(pos, 2, ms);
  }
  return out;
}

// 解析 `YYYY-MM-DDTHH:MM:SS[.fff][Z|±HH:MM|±HHMM]`
Result<std::int64_t> ParseCommon(std::string_view text) {
  if (text.size() < 19) return Err(ErrorKind::kInvalidArgument, "时间字符串过短");
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
  int consumed = 0;
  if (::sscanf(std::string(text).c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &s,
               &consumed) != 6) {
    return Err(ErrorKind::kInvalidArgument, "时间格式不符合 ISO-8601");
  }
  std::size_t idx = static_cast<std::size_t>(consumed);
  if (idx < text.size() && text[idx] == '.') {
    ++idx;
    int digits = 0;
    int frac = 0;
    while (idx < text.size() && text[idx] >= '0' && text[idx] <= '9') {
      if (digits < 3) frac = frac * 10 + (text[idx] - '0');
      ++digits;
      ++idx;
    }
    ms = frac;
    for (int i = digits; i < 3; ++i) ms *= 10;
  }
  int offset_minutes = 0;
  if (idx < text.size()) {
    const char c = text[idx];
    if (c == 'Z') {
      offset_minutes = 0;
    } else if (c == '+' || c == '-') {
      int oh = 0, om = 0;
      const std::string rest(text.substr(idx + 1));
      if (rest.size() == 5 && rest[2] == ':') {
        if (::sscanf(rest.c_str(), "%2d:%2d", &oh, &om) != 2)
          return Err(ErrorKind::kInvalidArgument, "时区偏移格式错误");
      } else if (rest.size() >= 4) {
        if (::sscanf(rest.c_str(), "%2d%2d", &oh, &om) != 2)
          return Err(ErrorKind::kInvalidArgument, "时区偏移格式错误");
      } else {
        return Err(ErrorKind::kInvalidArgument, "时区偏移格式错误");
      }
      offset_minutes = oh * 60 + om;
      if (c == '-') offset_minutes = -offset_minutes;
    } else {
      return Err(ErrorKind::kInvalidArgument, "无法识别的时区标记");
    }
  }

  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
  const std::time_t utc = ::timegm(&tm);
  if (utc == static_cast<std::time_t>(-1) && !(y == 1969 && mo == 12)) {
    return Err(ErrorKind::kInvalidArgument, "时间字段越界");
  }
  (void)ms;  // 秒级返回；毫秒由调用方按需另取
  return static_cast<std::int64_t>(utc) - offset_minutes * 60;
}

}  // namespace

std::string ToIso8601Utc(std::int64_t epoch_seconds, int millis) {
  return Format(epoch_seconds, millis, "%Y-%m-%dT%H:%M:%S.%fZ");
}

std::string ToOsduTimestamp(std::int64_t epoch_seconds, int millis) {
  // 契约 §2.5：`2021-03-03T15:13:33.120+0000` —— 末尾是 %z（+0000），不是 Z
  return Format(epoch_seconds, millis, "%Y-%m-%dT%H:%M:%S.%f+0000");
}

std::string ToRfc1123(std::int64_t epoch_seconds) {
  return Format(epoch_seconds, 0, "%a, %d %b %Y %H:%M:%S GMT");
}

Result<std::int64_t> ParseIso8601(std::string_view text) { return ParseCommon(text); }
Result<std::int64_t> ParseOsduTimestamp(std::string_view text) { return ParseCommon(text); }

}  // namespace fss::time
