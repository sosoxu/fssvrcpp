#include "common/logging/logging.h"

#include "common/time/clock.h"
#include "common/time/time_format.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <sstream>

namespace fss::logging {
namespace {

std::string ToLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

// -----------------------------------------------------------------------------
//  JSON 字符串转义（**不抛异常**）
// -----------------------------------------------------------------------------
//  ★ P1-D12（血泪）：原来直接用 `json::Dump(json::Value(s))`，而 nlohmann 的
//    `dump()` 默认 error_handler 是 **strict** —— 遇到非法 UTF-8 字节会抛
//    `type_error.316`。日志的输入是文件名/路径/请求头，完全可能是非 UTF-8 字节，
//    于是"用日志记录一个坏输入"反而把请求打挂。
//    日志是旁路，**绝不能因为记录内容而失败**，所以这里自备转义：
//      · ASCII：`"` `\` 与控制字符按 RFC 8259 转义（`\u00XX`）；
//      · 多字节：先校验 UTF-8，合法则原样透传，非法则替换为 U+FFFD；
//      · 与 nlohmann `error_handler_t::replace` 同语义（测试里用它做 oracle 比对）。
inline constexpr char kHex[] = "0123456789abcdef";
inline constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD

// 返回该首字节指示的序列长度；0 = 不可能是合法 UTF-8 首字节
std::size_t Utf8SeqLen(unsigned char c) {
  if (c < 0x80) return 1;
  if (c >= 0xC2 && c <= 0xDF) return 2;
  if (c >= 0xE0 && c <= 0xEF) return 3;
  if (c >= 0xF0 && c <= 0xF4) return 4;
  return 0;  // 0x80..0xC1（续字节/过长编码）、0xF5..0xFF
}

// 每个位置允许的续字节范围。UTF-8 的合法区间不是"一律 80..BF"：
//   · 3 字节 0xE0 要求 b1 >= 0xA0（否则过长编码）；0xED 要求 b1 <= 0x9F（否则代理区）
//   · 4 字节 0xF0 要求 b1 >= 0x90；0xF4 要求 b1 <= 0x8F（否则超过 U+10FFFF）
// ⚠️ 实测 nlohmann 是**逐字节按范围校验**的：某个位置越界就立刻判定序列非法，
//    并从**越界那个字节**重新开始；只有已经通过校验的续字节才被跳过。
//    第一版把"结构不完整"与"范围越界"混为一谈，穷举比对立刻抓出两处不一致。
void ContinuationRange(unsigned char b0, std::size_t len, std::size_t k, unsigned char* lo,
                       unsigned char* hi) {
  *lo = 0x80;
  *hi = 0xBF;
  if (k != 1) return;
  if (len == 3 && b0 == 0xE0) *lo = 0xA0;
  if (len == 3 && b0 == 0xED) *hi = 0x9F;
  if (len == 4 && b0 == 0xF0) *lo = 0x90;
  if (len == 4 && b0 == 0xF4) *hi = 0x8F;
}

}  // namespace

void AppendJsonString(std::string& out, std::string_view s) {
  out += '"';
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) {
            out += "\\u00";
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
          } else {
            out += static_cast<char>(c);
          }
      }
      ++i;
      continue;
    }
    const std::size_t len = Utf8SeqLen(c);
    if (len == 0) {  // 非法首字节（孤立续字节 / 过长首字节 / 0xF5 及以上）
      out += kReplacement;
      ++i;
      continue;
    }

    // 逐字节按范围校验，同时记录"已通过校验的续字节数"
    std::size_t consumed = 0;
    bool ok = true;
    for (std::size_t k = 1; k < len; ++k) {
      if (i + k >= s.size()) {  // 输入被截断
        ok = false;
        break;
      }
      unsigned char lo = 0;
      unsigned char hi = 0;
      ContinuationRange(c, len, k, &lo, &hi);
      const auto b = static_cast<unsigned char>(s[i + k]);
      if (b < lo || b > hi) {
        ok = false;
        break;
      }
      ++consumed;
    }

    if (ok) {
      out.append(s.data() + i, len);  // 合法序列原样透传（与 nlohmann 默认 dump 一致）
      i += len;
      continue;
    }
    // 非法：整段只算**一个**替换，然后从"越界的那个字节"继续（已通过校验的续字节被跳过）
    out += kReplacement;
    i += 1 + consumed;
  }
  out += '"';
}

// 快路径标量追加；object/array/float 回退到 nlohmann（罕见，正确性优先）
void AppendJsonValue(std::string& out, const json::Value& v) {
  if (v.is_string()) {
    AppendJsonString(out, v.get_ref<const std::string&>());
    return;
  }
  if (v.is_boolean()) {
    out += v.get<bool>() ? "true" : "false";
    return;
  }
  if (v.is_number_integer()) {
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, v.get<std::int64_t>());
    out.append(buf, r.ptr);
    return;
  }
  if (v.is_number_unsigned()) {
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, v.get<std::uint64_t>());
    out.append(buf, r.ptr);
    return;
  }
  if (v.is_null()) {
    out += "null";
    return;
  }
  // 浮点/嵌套结构：交给 nlohmann（已自带转义）。它可能因极端输入抛异常，
  // 但 ILogger::Log 的 noexcept 兜底会接住，不会传出去。
  out += json::Dump(v);
}


// -----------------------------------------------------------------------------
//  键名归一化
// -----------------------------------------------------------------------------
//  大小写不敏感 + **忽略 `_` 与 `-`**：这样 `secret_key` / `secretKey` / `SECRET-KEY`
//  三种写法会命中同一条规则。只做大小写不敏感是不够的——真实世界里同一个概念
//  在不同来源的 JSON/头部里就是这三种拼法，漏掉一种就是漏掉一处泄漏。
//
//  `Normalized` 额外保留"归一化后的第 i 个字符在原文中的下标"，
//  否则在原文里定位命中位置就得再做一次反向映射（曾因此漏掉 `Authorization:` 的值尾部）。
struct Normalized {
  std::string norm;
  std::vector<std::size_t> orig_index;
};

Normalized Normalize(std::string_view text) {
  Normalized out;
  out.norm.reserve(text.size());
  out.orig_index.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '_' || c == '-') continue;  // 归一化时丢掉分隔符
    out.norm += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    out.orig_index.push_back(i);
  }
  return out;
}

// `key: value` —— 值可能含空格（`Authorization: Bearer xxx`），掩到行尾/引号/结构符为止
bool IsColonValueEnd(char c) {
  switch (c) {
    case '\n': case '\r': case '"': case '\'': case ',': case ';': case '}': case ']':
      return true;
    default:
      return false;
  }
}

// `key=value` —— 查询串形态，到 `&`/空白为止（`X-Amz-Date` 之类不能被牵连）
bool IsEqualsValueEnd(char c) {
  switch (c) {
    case ' ': case '\t': case '\n': case '\r': case '"': case '\'': case ',': case ';':
    case '&': case '}': case ']': case ')':
      return true;
    default:
      return false;
  }
}

// text 格式下把换行转义 —— 否则调用方传入含换行的消息就能**伪造日志行**
// （日志伪造/注入，T5 的邻居）。JSON 格式由 AppendJsonString 转义。
void AppendTextEscaped(std::string& out, std::string_view s) {
  for (const char c : s) {
    switch (c) {
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\\': out += "\\\\"; break;
      default: out += c; break;
    }
  }
}

int MillisOf(const IClock& clock) {
  const auto ms = clock.NowEpochMillis();
  const auto rem = ms % 1000;
  return static_cast<int>(rem < 0 ? rem + 1000 : rem);
}


std::string EscapeJsonString(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  AppendJsonString(out, s);
  return out;
}

// -----------------------------------------------------------------------------
//  Level
// -----------------------------------------------------------------------------
std::string_view LevelName(Level level) {
  switch (level) {
    case Level::kDebug: return "debug";
    case Level::kInfo: return "info";
    case Level::kWarn: return "warn";
    case Level::kError: return "error";
  }
  return "info";
}

std::string LevelNameUpper(Level level) {
  std::string s(LevelName(level));
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return s;
}

std::optional<Level> ParseLevel(std::string_view text) {
  const std::string low = ToLower(text);
  if (low == "debug") return Level::kDebug;
  if (low == "info") return Level::kInfo;
  if (low == "warn" || low == "warning") return Level::kWarn;
  if (low == "error" || low == "err") return Level::kError;
  return std::nullopt;
}

// -----------------------------------------------------------------------------
//  Redactor
// -----------------------------------------------------------------------------
Redactor::Redactor(std::vector<std::string> keys) {
  for (auto& k : keys) {
    if (k.empty()) continue;
    const std::string lower = ToLower(k);
    const std::string norm = Normalize(lower).norm;
    if (norm.empty()) continue;  // 例如规则写成 "---" → 归一化后为空
    keys_lower_.push_back(lower);
    keys_norm_.push_back(norm);
    if (min_key_len_ == 0 || norm.size() < min_key_len_) min_key_len_ = norm.size();
  }
}

Redactor Redactor::FromCommaSeparated(std::string_view csv) {
  std::vector<std::string> keys;
  std::size_t start = 0;
  while (start <= csv.size()) {
    const auto comma = csv.find(',', start);
    const auto end = comma == std::string_view::npos ? csv.size() : comma;
    std::string piece(csv.substr(start, end - start));
    // 去掉首尾空白（配置里人写的逗号列表常带空格）
    const auto b = piece.find_first_not_of(" \t\r\n");
    const auto e = piece.find_last_not_of(" \t\r\n");
    piece = (b == std::string::npos) ? std::string() : piece.substr(b, e - b + 1);
    if (!piece.empty()) keys.push_back(piece);
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return Redactor(std::move(keys));
}

bool Redactor::Matches(std::string_view field_name) const {
  if (keys_norm_.empty() || field_name.empty()) return false;
  if (field_name.size() < min_key_len_) return false;  // 早退：比最短规则还短，不可能命中

  // ★ P1-D11：这里过去调 `Normalize()`，而它每次都要 push_back 一个
  //   `std::vector<size_t>`（**一次堆分配**）。字段名很短，绝大多数落在栈上即可：
  //   用栈上缓冲做归一化，超过 kStackNormalize 的极长字段名才回退到堆。
  constexpr std::size_t kStackNormalize = 256;
  char stack_buf[kStackNormalize];
  std::string heap_buf;
  std::string_view norm;
  if (field_name.size() <= kStackNormalize) {
    std::size_t n = 0;
    for (const char ch : field_name) {
      if (ch == '_' || ch == '-') continue;
      stack_buf[n++] = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    norm = std::string_view(stack_buf, n);
  } else {
    heap_buf = Normalize(field_name).norm;
    norm = heap_buf;
  }

  for (const auto& k : keys_norm_) {
    if (norm.find(k) != std::string_view::npos) return true;
  }
  return false;
}

json::Value Redactor::Apply(const json::Value& value) const {
  if (keys_lower_.empty()) return value;
  if (value.is_object()) {
    json::Value out = json::Value::object();
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (Matches(it.key())) {
        out[it.key()] = mask_;  // ★ ① 键名命中 → 整个值打码（不递归、不留长度信息）
      } else {
        out[it.key()] = Apply(it.value());
      }
    }
    return out;
  }
  if (value.is_array()) {
    json::Value out = json::Value::array();
    for (const auto& item : value) out.push_back(Apply(item));
    return out;
  }
  if (value.is_string()) {
    // ★ ② 字符串值里可能内嵌 `token=xxx`，即使在没命中的字段里也要扫一遍
    return json::Value(ScrubText(value.get<std::string>()));
  }
  return value;
}

std::string Redactor::ScrubText(std::string_view text) const {
  if (keys_norm_.empty() || text.empty()) return std::string(text);
  if (text.size() < min_key_len_) return std::string(text);  // 早退

  // ★ P1-D11：过去是"每条规则各重建一次归一化视图"（7 条规则 = 7 次堆分配 + 7 遍扫描），
  //   现在**只归一化一次**，在其中找出所有规则的命中，再从后往前统一替换
  //   （从后往前是为了让下标不被前面的替换打乱）。
  const Normalized n = Normalize(text);
  if (n.norm.empty()) return std::string(text);

  std::vector<std::pair<std::size_t, std::size_t>> ranges;  // 原文中的 [begin, end)
  for (const auto& key : keys_norm_) {
    std::size_t from = 0;
    while (true) {
      const auto hit = n.norm.find(key, from);
      if (hit == std::string::npos) break;
      from = hit + key.size();
      // 归一化下标 → 原文下标（key 内无分隔符，故首尾都落在 orig_index 上）
      std::size_t i = n.orig_index[hit + key.size() - 1] + 1;

      if (i < text.size() && text[i] == '"') ++i;  // `"key":"value"` 的键后引号
      while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;

      char sep = '\0';
      if (i < text.size() && (text[i] == ':' || text[i] == '=')) {
        sep = text[i];
        ++i;
      } else {
        continue;  // 命中词后面没有分隔符（自然语言里出现规则名）→ 不是键值对
      }
      while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
      if (i < text.size() && text[i] == '"') ++i;

      const std::size_t value_begin = i;
      std::size_t value_end = value_begin;
      bool (*is_end)(char) = (sep == ':') ? IsColonValueEnd : IsEqualsValueEnd;
      while (value_end < text.size() && !is_end(text[value_end])) ++value_end;
      if (value_end > value_begin) ranges.emplace_back(value_begin, value_end);
    }
  }
  if (ranges.empty()) return std::string(text);

  // 合并重叠/相邻区间（例如 `sig` 与 `signature` 命中同一处），避免重复替换
  std::sort(ranges.begin(), ranges.end());
  std::vector<std::pair<std::size_t, std::size_t>> merged;
  merged.reserve(ranges.size());
  for (const auto& r : ranges) {
    if (!merged.empty() && r.first <= merged.back().second) {
      merged.back().second = std::max(merged.back().second, r.second);
    } else {
      merged.push_back(r);
    }
  }

  std::string work(text);
  for (auto it = merged.rbegin(); it != merged.rend(); ++it) {
    work.replace(it->first, it->second - it->first, mask_);
  }
  return work;
}

// -----------------------------------------------------------------------------
//  Context
// -----------------------------------------------------------------------------
Fields ContextFields(const Context& ctx) {
  Fields fields;
  if (!ctx.correlation_id.empty()) fields.emplace_back("correlation_id", ctx.correlation_id);
  if (!ctx.partition_id.empty()) fields.emplace_back("partition_id", ctx.partition_id);
  if (!ctx.user_id.empty()) fields.emplace_back("user_id", ctx.user_id);
  return fields;
}

// -----------------------------------------------------------------------------
//  便捷函数
// -----------------------------------------------------------------------------
namespace {
void Emit(const ILogger& logger, Level level, std::string_view msg, const Fields& fields) {
  if (!logger.Enabled(level)) return;
  logger.Log(level, msg, fields);
}
}  // namespace

void Debug(const ILogger& l, std::string_view msg, const Fields& f) { Emit(l, Level::kDebug, msg, f); }
void Info(const ILogger& l, std::string_view msg, const Fields& f) { Emit(l, Level::kInfo, msg, f); }
void Warn(const ILogger& l, std::string_view msg, const Fields& f) { Emit(l, Level::kWarn, msg, f); }
void Error(const ILogger& l, std::string_view msg, const Fields& f) { Emit(l, Level::kError, msg, f); }

// -----------------------------------------------------------------------------
//  StreamLogger
// -----------------------------------------------------------------------------
StreamLogger::StreamLogger(LogOptions options, const IClock& clock, std::ostream& out)
    : options_(std::move(options)),
      redactor_(options_.redact_keys),
      clock_(&clock),
      out_(&out) {
  redactor_.set_mask(options_.mask);
}

StreamLogger::StreamLogger(LogOptions options, const IClock& clock)
    : StreamLogger(std::move(options), clock, std::clog) {}

bool StreamLogger::Enabled(Level level) const {
  return static_cast<int>(level) >= static_cast<int>(options_.min_level);
}

std::uint64_t StreamLogger::written() const {
  std::lock_guard<std::mutex> lock(mu_);
  return written_;
}

std::string StreamLogger::Render(Level level, std::string_view msg, const Fields& fields) const {
  const bool json_format = (options_.format != "text");
  const std::string ts = time::ToIso8601Utc(clock_->NowEpochSeconds(), MillisOf(*clock_));

  // ★ P1-D11：预留缓冲 + 直接追加，不再"每条记录新建 nlohmann 对象再 dump()"。
  //   实测（独立进程、绑核、/dev/null sink，3 轮最好值）：
  //     nlohmann 组装 + dump  ~1100 ns/条 ；手写 append + to_chars  ~45 ns/条
  //   注意：字符串转义仍由 AppendJsonString 负责（它是安全边界，不许省）。
  std::string out;
  out.reserve(256);

  if (json_format) {
    out += R"({"ts":)";
    AppendJsonString(out, ts);
    out += R"(,"level":)";
    AppendJsonString(out, LevelName(level));
    out += R"(,"logger":)";
    AppendJsonString(out, options_.service);
    out += R"(,"msg":)";
    AppendJsonString(out, redactor_.ScrubText(msg));
    for (const auto& [key, value] : fields) {
      if (key.empty()) continue;
      out += ',';
      AppendJsonString(out, key);
      out += ':';
      if (redactor_.Matches(key)) {
        AppendJsonString(out, options_.mask);  // (1) 键名命中 -> 整个值打码
      } else {
        AppendJsonValue(out, redactor_.Apply(value));  // (2) 值内可能内嵌密钥
      }
    }
    out += '}';
    return out;
  }

  // text：本地开发用。同样不用 ostringstream（每条约 200 ns）
  out += ts;
  out += ' ';
  out += LevelNameUpper(level);
  out += ' ';
  AppendTextEscaped(out, redactor_.ScrubText(msg));
  for (const auto& [key, value] : fields) {
    if (key.empty()) continue;
    out += ' ';
    AppendTextEscaped(out, key);
    out += '=';
    if (redactor_.Matches(key)) {
      out += options_.mask;
    } else {
      const json::Value masked = redactor_.Apply(value);
      if (masked.is_string()) {
        AppendTextEscaped(out, masked.get_ref<const std::string&>());
      } else {
        out += json::Dump(masked);
      }
    }
  }
  return out;
}

void StreamLogger::Log(Level level, std::string_view msg, const Fields& fields) const noexcept {
  // ★ 日志是旁路：**任何**情况下都不许把异常抛回调用方（P1-D12 的推广）。
  //   日志的输入来自文件名/路径/请求头，可能含非法 UTF-8 等坏数据；
  //   真出了意外就退化成一条最小可用记录 —— 记录失败不能变成请求失败。
  std::string line;
  try {
    line = Render(level, msg, fields);
  } catch (const std::exception& e) {
    line = R"({"level":"error","logger":")" + options_.service +
           R"(","msg":"log render failed","error":)";
    AppendJsonString(line, e.what());
    line += '}';
  } catch (...) {
    line = R"({"level":"error","logger":")" + options_.service +
           R"(","msg":"log render failed","error":"unknown"})";
  }

  // 整行在同一把锁内写出：否则并发写会**交错**，JSON Lines 就不再是一行一条，
  //   采集端会解析失败（这比丢一条日志更难排查）。
  std::lock_guard<std::mutex> lock(mu_);
  try {
    *out_ << line << '\n';
    out_->flush();
  } catch (...) {
    // 输出流本身失败（磁盘满/管道断）也不能抛
  }
  ++written_;
}

// -----------------------------------------------------------------------------
//  MemoryLogger
// -----------------------------------------------------------------------------
MemoryLogger::MemoryLogger(LogOptions options)
    : options_(std::move(options)), redactor_(options_.redact_keys) {
  redactor_.set_mask(options_.mask);
}

bool MemoryLogger::Enabled(Level level) const {
  return static_cast<int>(level) >= static_cast<int>(options_.min_level);
}

void MemoryLogger::Log(Level level, std::string_view msg, const Fields& fields) const noexcept {
  try {
    LogImpl(level, msg, fields);
  } catch (...) {
    // 同 StreamLogger：内存 sink 也不许把异常抛给调用方
  }
}

void MemoryLogger::LogImpl(Level level, std::string_view msg, const Fields& fields) const {
  json::Value rec = json::Value::object();
  rec["ts"] = time::ToIso8601Utc(1700000000, 0);
  rec["level"] = std::string(LevelName(level));
  rec["logger"] = options_.service;
  rec["msg"] = redactor_.ScrubText(msg);
  for (const auto& [key, value] : fields) {
    if (key.empty()) continue;
    rec[key] = redactor_.Matches(key) ? json::Value(options_.mask) : redactor_.Apply(value);
  }
  std::lock_guard<std::mutex> lock(mu_);
  records_.push_back(std::move(rec));
}

std::vector<json::Value> MemoryLogger::records() const {
  std::lock_guard<std::mutex> lock(mu_);
  return records_;
}

std::size_t MemoryLogger::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return records_.size();
}

void MemoryLogger::Clear() {
  std::lock_guard<std::mutex> lock(mu_);
  records_.clear();
}

// -----------------------------------------------------------------------------
//  配置桥接
// -----------------------------------------------------------------------------
LogOptions OptionsFromConfig(Level min_level, std::string_view format, std::string_view service,
                             std::string_view redact_keys_csv) {
  LogOptions options;
  options.min_level = min_level;
  options.format = format.empty() ? "json" : std::string(format);
  if (!service.empty()) options.service = std::string(service);
  options.redact_keys = Redactor::FromCommaSeparated(redact_keys_csv).keys();
  return options;
}

}  // namespace fss::logging
