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
namespace {

//  该年该月的天数（含闰年规则：4 年一闰、100 年不闰、400 年再闰）
int DaysInMonth(int year, int month) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return leap ? 29 : 28;
  }
  return kDays[month - 1];
}

}  // namespace

Result<std::int64_t> ParseCommon(std::string_view text) {
  if (text.size() < 19) return Err(ErrorKind::kInvalidArgument, "时间字符串过短");
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
  int consumed = 0;
  if (::sscanf(std::string(text).c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &s,
               &consumed) != 6) {
    return Err(ErrorKind::kInvalidArgument, "时间格式不符合 ISO-8601");
  }
  //  ★ 形状校验：必须**恰好**是 `yyyy-MM-ddTHH:mm:ss`（19 个字符）。
  //    `%2d` 对 "2020-1-01" 只吃 1 个字符 → `consumed` 会 ≠ 19，据此拒绝。
  if (consumed != 19) {
    return Err(ErrorKind::kInvalidArgument, "时间格式不符合 ISO-8601（要求 yyyy-MM-ddTHH:mm:ss）");
  }
  //  ★ 范围校验：`timegm` 会**静默归一化**越界字段（月份 13 → 次年、小时 99 → +4 天），
  //    于是 `2020-13-45T99:99:99Z` 会变成一个"看似合理"的错误时刻 —— 用它做过过滤边界
  //    会静默筛错数据（P6-D10）。这里必须先挡住。
  if (mo < 1 || mo > 12 || d < 1 || d > DaysInMonth(y, mo) || h < 0 || h > 23 || mi < 0 ||
      mi > 59 || s < 0 || s > 59) {
    return Err(ErrorKind::kInvalidArgument, "时间字段越界");
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
      if (oh > 23 || om > 59) {
        return Err(ErrorKind::kInvalidArgument, "时区偏移越界");
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
