// =============================================================================
//  fss::Result / fss::Error —— 显式的错误值类型（L1）
// =============================================================================
//
//  设计约束（docs/02-design.md §3.1、docs/03-api-contract.md §5）
//  ---------------------------------------------------------------------------
//  * **不使用异常跨越层边界**。所有可能失败的调用返回 `Result<T>`。
//  * `ErrorKind` 的取值**就是**契约里那张错误码映射表的行 —— REST 状态码、
//    `reason` 文案与 gRPC `StatusCode` 都由它单点派生（见 adapters 层的 mapper）。
//    新增 ErrorKind 必须同时更新契约表与等价性测试（否则测试因空缺失败）。
//  * `Error` 只携带**结构化信息**，不携带 HTTP/gRPC 细节 —— 协议细节属于适配层。
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace fss {

// -----------------------------------------------------------------------------
//  ErrorKind —— 与 docs/03-api-contract.md §5「错误码双向映射」逐行对应
// -----------------------------------------------------------------------------
enum class ErrorKind {
  kOk = 0,

  // 400 Bad Request 系
  kInvalidArgument,       // 校验失败、非法 expiryTime、kind 非法
  kFileSourceEmpty,       // 消息固定为 "FileSource can not be empty"
  kInvalidSourcePath,     // 消息固定为 "Invalid source file path to copy from <path>"
  kLocationAlreadyExists, // ★ 上游映射到 400（不是 409）
  kChecksumMismatch,      // 校验和不符（存储 put 的 expected_checksum / 数据面校验）

  // 401 / 403
  kUnauthenticated,       // 缺 token / 缺 partition → "Missing authorization token" / "Missing partitionID"
  kPermissionDenied,      // **调用方**角色不足（是本服务拒绝了这个用户）
  //  ★ 与 kPermissionDenied 必须分开：这是**存储侧**拒绝了本服务（凭证错/桶策略不允许），
  //    对客户端而言是依赖故障而不是"你没权限"。P5 的 S3 错误映射需要它（AccessDenied/
  //    SignatureDoesNotMatch → 403），契约 §5 已登记。
  kStorageAccessDenied,

  // 404
  kNotFound,              // "Record Not Found" / "Not found location for fileID : <id>"

  // 501
  kUnimplemented,         // 扩展端点未启用

  // 5xx
  kInternal,              // 未预期异常 → 500
  kBadGateway,            // 依赖服务异常 → 502
  kUnavailable,           // 存储后端不可用/过载 → 503
};

// 稳定的机器可读名字（用于日志、指标标签与测试断言；不要用于对外响应体）
std::string_view ErrorKindName(ErrorKind kind) noexcept;

// -----------------------------------------------------------------------------
//  Error —— 结构化错误
// -----------------------------------------------------------------------------
class Error {
 public:
  Error() = default;
  Error(ErrorKind kind, std::string message)
      : kind_(kind), message_(std::move(message)) {}

  ErrorKind kind() const noexcept { return kind_; }
  const std::string& message() const noexcept { return message_; }

  // 附加信息（例如 checksum 的期望/实际值）。键值对形式，便于映射层按需输出。
  Error& With(std::string key, std::string value) {
    details_.emplace_back(std::move(key), std::move(value));
    return *this;
  }
  const std::vector<std::pair<std::string, std::string>>& details() const noexcept {
    return details_;
  }
  std::optional<std::string> Find(std::string_view key) const;

  // 便于测试与日志的紧凑表示；**不**作为对外响应体的格式。
  std::string ToString() const;

 private:
  ErrorKind kind_ = ErrorKind::kInternal;
  std::string message_;
  std::vector<std::pair<std::string, std::string>> details_;
};

// -----------------------------------------------------------------------------
//  Result<T>
// -----------------------------------------------------------------------------
template <typename T>
class [[nodiscard]] Result {
 public:
  // 隐式构造：允许 `return value;` 与 `return error;`
  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

  bool ok() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  // 仅当 ok() 时可用；否则行为未定义（调用方应先判断）
  T& value() & { return std::get<0>(storage_); }
  const T& value() const& { return std::get<0>(storage_); }
  T&& value() && { return std::move(std::get<0>(storage_)); }

  Error& error() & { return std::get<1>(storage_); }
  const Error& error() const& { return std::get<1>(storage_); }

  // 便捷取值：失败时返回 fallback（避免到处写 if(!r)）
  template <typename U>
  T value_or(U&& fallback) const {
    return ok() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  std::variant<T, Error> storage_;
};

// -----------------------------------------------------------------------------
//  Result<void>
// -----------------------------------------------------------------------------
template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;  // 成功
  Result(Error error) : error_(std::move(error)), ok_(false) {}

  bool ok() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }

  Error& error() & { return error_; }
  const Error& error() const& { return error_; }

 private:
  Error error_{};
  bool ok_ = true;
};

// -----------------------------------------------------------------------------
//  构造函数：Ok() / Err()
// -----------------------------------------------------------------------------
inline Result<void> Ok() { return Result<void>{}; }

template <typename T>
Result<std::decay_t<T>> Ok(T&& value) {
  return Result<std::decay_t<T>>(std::forward<T>(value));
}

inline Error Err(ErrorKind kind, std::string message) {
  return Error(kind, std::move(message));
}

// -----------------------------------------------------------------------------
//  错误传播
// -----------------------------------------------------------------------------
//  用法：
//     FSS_TRY(store, blobs_->ForPartition(partition));   // 绑定成功值并传播错误
//     FSS_TRY(ref.Save());                               // 只传播错误（Result<void>）
//
//  传播时**保留原始 ErrorKind 与 details**（不做类型转换）—— 错误到协议的映射
//  只发生在适配层，避免在这里丢信息。
#define FSS_TRY_PROPAGATE(expr)                    \
  do {                                             \
    auto&& _fss_r = (expr);                        \
    if (!_fss_r.ok()) return _fss_r.error();       \
  } while (0)

#define FSS_TRY_BIND(var, expr)                              \
  auto&& _fss_r_##var = (expr);                              \
  if (!_fss_r_##var.ok()) return _fss_r_##var.error();       \
  auto var = std::move(_fss_r_##var).value()

#define FSS_TRY_PICK(_1, _2, NAME, ...) NAME
#define FSS_TRY(...) \
  FSS_TRY_PICK(__VA_ARGS__, FSS_TRY_BIND, FSS_TRY_PROPAGATE, )(__VA_ARGS__)

}  // namespace fss
