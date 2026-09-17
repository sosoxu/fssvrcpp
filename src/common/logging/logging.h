// =============================================================================
//  结构化日志 + 敏感字段脱敏（L1）
// =============================================================================
//
//  为什么在 L1
//    与 `IClock` / `IIdGenerator` 同理：日志是**技术抽象**，没有领域语义，
//    而 L1/L2 自己（HTTP 传输层、存储驱动）就必须能打日志，不能反向依赖 L3。
//
//  为什么必须有脱敏（T5，见 docs/02-design.md §15）
//    S3 的 `access_key` / `secret_key`、SigV4 的签名与派生密钥、Bearer token
//    都可能被拼进日志——尤其是"把请求头 dump 出来排障"这种最常见的做法。
//    因此脱敏分两层，缺一不可：
//      ① **结构化字段层**：字段名命中 redact_keys（大小写不敏感子串）→ 整个值打码；
//      ② **自由文本层**：`ScrubText()` 在消息/字符串值里找 `key: value` / `key=value`，
//         把值打码。否则 `Info("Authorization: Bearer " + token)` 仍然泄漏。
//
//  ⚠️ 匹配是**子串**语义，方向是"宁可多打码"：
//     `sig` 会连带命中 `design`、`assignment`。这会让个别字段被无谓打码，
//     但反过来（只匹配完整词）会漏掉 `X-Amz-Signature`、`signing_key` 这类真实键名。
//     在"日志可读性"和"密钥泄漏"之间，本仓库一律选后者。
//
//  日志格式
//    `observability.log_format=json`（默认）：**每行一条 JSON**（JSON Lines），
//      便于 filebeat/fluent-bit 直接采集；nlohmann 会转义控制字符，
//      因此调用方传入含 `\n` 的消息**无法伪造出一条日志**。
//    `observability.log_format=text`：本地开发用，换行同样被转义。
#pragma once

#include "common/json/json.h"

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fss {

class IClock;

namespace logging {

// -----------------------------------------------------------------------------
//  级别
// -----------------------------------------------------------------------------
enum class Level { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

std::string_view LevelName(Level level);          // debug / info / warn / error
std::string LevelNameUpper(Level level);          // DEBUG / INFO / WARN / ERROR
std::optional<Level> ParseLevel(std::string_view text);

// -----------------------------------------------------------------------------
//  JSON 字符串转义原语
// -----------------------------------------------------------------------------
//  把 s 作为一个 JSON 字符串（含两侧引号）追加到 out。**不抛异常**：
//    · ASCII：`"` `\` 与控制字符按 RFC 8259 转义；
//    · 多字节：校验 UTF-8，合法则原样透传，非法字节按 U+FFFD 替换；
//    · 非法 UTF-8 的替换语义与 nlohmann `error_handler_t::replace` **逐字节等价**
//      （`test_logging.cpp` 用穷举比对证明这一点）。
//  为什么自备而不用 `json::Dump`：nlohmann 的默认 dump 在遇到非法 UTF-8 时
//  **抛异常**（P1-D12）—— 日志的输入是文件名/请求头，日志不能因为记录坏输入而失败。
void AppendJsonString(std::string& out, std::string_view s);
std::string EscapeJsonString(std::string_view s);

// -----------------------------------------------------------------------------
//  脱敏
// -----------------------------------------------------------------------------
inline constexpr std::string_view kDefaultMask = "***";

class Redactor {
 public:
  Redactor() = default;
  explicit Redactor(std::vector<std::string> keys);

  // 逗号分隔（`observability.redact_keys` 的取值形态）；空串 → 空规则集
  static Redactor FromCommaSeparated(std::string_view csv);

  // 大小写不敏感、**忽略 `_`/`-`** 的子串匹配（见文件头的取舍说明）
  //   命中示例：`secret_key` / `secretKey` / `SECRET-KEY` 三条规则都能命中彼此
  bool Matches(std::string_view field_name) const;

  const std::vector<std::string>& keys() const { return keys_lower_; }
  bool empty() const { return keys_lower_.empty(); }

  void set_mask(std::string mask) { mask_ = std::move(mask); }
  const std::string& mask() const { return mask_; }

  // ① 结构化层：递归处理 object/array，命中键名的值整体替换为 mask
  json::Value Apply(const json::Value& value) const;

  // ② 自由文本层：把 `key: value` / `key=value` / `"key":"value"` 的值替换为 mask
  std::string ScrubText(std::string_view text) const;

 private:
  std::vector<std::string> keys_lower_;  // 原拼写的小写形式（用于展示/排障）
  std::vector<std::string> keys_norm_;   // 归一化形式（丢弃 `_`/`-`），用于匹配
  std::size_t min_key_len_ = 0;          // 最短规则长度（早退用；P1-D11）
  std::string mask_{kDefaultMask};
};

// -----------------------------------------------------------------------------
//  字段
// -----------------------------------------------------------------------------
//  用 vector 而不是 map：日志字段是有序的，且字段数极少（线性查找更快）
using Fields = std::vector<std::pair<std::string, json::Value>>;

//  链路上下文（correlation-id 透传；P4 的中间件会填充它们）
struct Context {
  std::string correlation_id;
  std::string partition_id;
  std::string user_id;
};

// 只输出非空字段（空字段打出来只会淹没有用信息）
Fields ContextFields(const Context& ctx);

struct LogOptions {
  Level min_level = Level::kInfo;
  std::string format = "json";  // json | text（observability.log_format）
  std::string service = "file-service";
  std::vector<std::string> redact_keys;
  std::string mask{std::string(kDefaultMask)};
};

// -----------------------------------------------------------------------------
//  ILogger
// -----------------------------------------------------------------------------
class ILogger {
 public:
  virtual ~ILogger() = default;

  // ★ 调用方**必须**先问 Enabled：这样即使参数构造有开销，热路径也不会白算
  virtual bool Enabled(Level level) const = 0;
  // const：日志是旁路输出，不改变 logger 的身份语义；因此可以拿 const 引用到处传
  // noexcept：★ 日志不许把异常抛回调用方。日志的输入来自文件名/路径/请求头，
  //   里面可能含非法 UTF-8 等"坏数据"—— 记录坏输入绝不能反过来打挂请求。
  virtual void Log(Level level, std::string_view msg, const Fields& fields) const noexcept = 0;
};

void Debug(const ILogger& logger, std::string_view msg, const Fields& fields = {});
void Info(const ILogger& logger, std::string_view msg, const Fields& fields = {});
void Warn(const ILogger& logger, std::string_view msg, const Fields& fields = {});
void Error(const ILogger& logger, std::string_view msg, const Fields& fields = {});

// -----------------------------------------------------------------------------
//  StreamLogger —— 写向 std::ostream（默认 std::clog，即 stderr）
// -----------------------------------------------------------------------------
//  为什么默认 stderr：stdout 在容器里常被用作协议/数据输出；日志混进去会破坏管道。
class StreamLogger final : public ILogger {
 public:
  StreamLogger(LogOptions options, const IClock& clock, std::ostream& out);
  // 便捷：stderr
  StreamLogger(LogOptions options, const IClock& clock);

  bool Enabled(Level level) const override;
  void Log(Level level, std::string_view msg, const Fields& fields) const noexcept override;

  const LogOptions& options() const { return options_; }
  const Redactor& redactor() const { return redactor_; }
  // 已写出的记录数（用于自证"确实写了"）
  std::uint64_t written() const;

  // 渲染一条记录（不加换行）。测试直接断言它的输出，不必先落盘。
  std::string Render(Level level, std::string_view msg, const Fields& fields) const;

 private:
  LogOptions options_;
  Redactor redactor_;
  const IClock* clock_;
  std::ostream* out_;
  mutable std::mutex mu_;
  mutable std::uint64_t written_ = 0;
};

// -----------------------------------------------------------------------------
//  MemoryLogger —— 测试与"审计兜底"用：把记录留在内存里
// -----------------------------------------------------------------------------
class MemoryLogger final : public ILogger {
 public:
  explicit MemoryLogger(LogOptions options = {});

  bool Enabled(Level level) const override;
  void Log(Level level, std::string_view msg, const Fields& fields) const noexcept override;

  // 解析后的记录（JSON 形态，便于测试按字段断言）
  std::vector<json::Value> records() const;
  std::size_t size() const;
  void Clear();

  const LogOptions& options() const { return options_; }
  const Redactor& redactor() const { return redactor_; }
  void SetMinLevel(Level level) { options_.min_level = level; }

 private:
  void LogImpl(Level level, std::string_view msg, const Fields& fields) const;

  LogOptions options_;
  Redactor redactor_;
  mutable std::mutex mu_;
  mutable std::vector<json::Value> records_;
};

// 便捷：从配置值（`observability.*`）构造 LogOptions
LogOptions OptionsFromConfig(Level min_level, std::string_view format, std::string_view service,
                             std::string_view redact_keys_csv);

}  // namespace logging
}  // namespace fss
