// PgConnection / PgResult / PgPool 实现。设计与 SQL 纪律见头文件。
#include "infra/postgres/pg_connection.h"

#include <libpq-fe.h>

#include <cstdlib>
#include <cstring>
#include <utility>

namespace fss::infra {

namespace {

std::string Trim(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                           text.back() == '\t')) {
    text.pop_back();
  }
  return text;
}

std::string FieldOrEmpty(::PGresult* result, int field_code) {
  if (result == nullptr) return {};
  const char* value = ::PQresultErrorField(result, field_code);
  return value != nullptr ? std::string(value) : std::string{};
}

}  // namespace

fss::Result<void> ValidatePgOptions(const PgOptions& options) {
  if (options.max_connections <= 0) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "postgres.max_connections 必须 > 0（收到 " +
                   std::to_string(options.max_connections) + "）");
  }
  if (options.statement_timeout_millis < 0) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "postgres.statement_timeout_ms 必须 >= 0（收到 " +
                   std::to_string(options.statement_timeout_millis) + "）");
  }
  return Ok();
}

fss::Error MapPgError(std::string_view sqlstate, std::string message) {
  fss::ErrorKind kind = fss::ErrorKind::kInternal;
  if (sqlstate == "23505") {
    //  unique_violation：与 SqliteLocationRepository 对重复位置的映射**同一种**
    //  （kLocationAlreadyExists → 400，不是 409；见 ports.h 与契约 §5）。
    kind = fss::ErrorKind::kLocationAlreadyExists;
  } else if (sqlstate == "57014") {
    //  query_canceled（statement_timeout）→ 依赖过载/被取消，按不可用处理
    kind = fss::ErrorKind::kUnavailable;
  } else if (sqlstate.size() >= 2 && sqlstate[0] == '0' && sqlstate[1] == '8') {
    //  Class 08 —— Connection Exception
    kind = fss::ErrorKind::kUnavailable;
  }
  fss::Error error = fss::Err(kind, Trim(std::move(message)));
  if (!sqlstate.empty()) error.With("sqlstate", std::string(sqlstate));
  return error;
}

fss::Error Annotate(const fss::Error& error, std::string_view what) {
  fss::Error annotated(error.kind(), std::string(what) + "：" + error.message());
  for (const auto& [key, value] : error.details()) annotated.With(key, value);
  return annotated;
}

// -----------------------------------------------------------------------------
//  PgResult
// -----------------------------------------------------------------------------
PgResult::~PgResult() {
  if (raw_ != nullptr) ::PQclear(raw_);
}

PgResult::PgResult(PgResult&& other) noexcept : raw_(other.raw_) { other.raw_ = nullptr; }

PgResult& PgResult::operator=(PgResult&& other) noexcept {
  if (this != &other) {
    if (raw_ != nullptr) ::PQclear(raw_);
    raw_ = other.raw_;
    other.raw_ = nullptr;
  }
  return *this;
}

bool PgResult::ok() const {
  if (raw_ == nullptr) return false;
  const ExecStatusType status = ::PQresultStatus(raw_);
  return status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK;
}

int PgResult::RowCount() const { return raw_ != nullptr ? ::PQntuples(raw_) : 0; }

int PgResult::ColumnCount() const { return raw_ != nullptr ? ::PQnfields(raw_) : 0; }

int PgResult::ColumnIndex(std::string_view name) const {
  if (raw_ == nullptr) return -1;
  const std::string owned(name);  // PQfnumber 要求 NUL 结尾
  return ::PQfnumber(raw_, owned.c_str());
}

bool PgResult::IsNull(int row, int column) const {
  if (raw_ == nullptr) return true;
  if (row < 0 || row >= ::PQntuples(raw_) || column < 0 || column >= ::PQnfields(raw_)) return true;
  return ::PQgetisnull(raw_, row, column) == 1;
}

std::string PgResult::Value(int row, int column) const {
  if (raw_ == nullptr) return {};
  if (row < 0 || row >= ::PQntuples(raw_) || column < 0 || column >= ::PQnfields(raw_)) return {};
  if (::PQgetisnull(raw_, row, column) == 1) return {};
  const char* value = ::PQgetvalue(raw_, row, column);
  const int bytes = ::PQgetlength(raw_, row, column);
  if (value == nullptr || bytes <= 0) return {};
  return std::string(value, static_cast<std::size_t>(bytes));
}

std::string PgResult::Value(int row, std::string_view column) const {
  return Value(row, ColumnIndex(column));
}

std::int64_t PgResult::AffectedRows() const {
  if (raw_ == nullptr) return 0;
  const char* tuples = ::PQcmdTuples(raw_);
  if (tuples == nullptr || *tuples == '\0') return 0;
  return static_cast<std::int64_t>(std::strtoll(tuples, nullptr, 10));
}

std::string PgResult::ErrorMessage() const {
  if (raw_ == nullptr) return {};
  const char* message = ::PQresultErrorMessage(raw_);
  return message != nullptr ? Trim(std::string(message)) : std::string{};
}

std::string PgResult::SqlState() const { return FieldOrEmpty(raw_, PG_DIAG_SQLSTATE); }

// -----------------------------------------------------------------------------
//  PgConnection
// -----------------------------------------------------------------------------
fss::Result<std::unique_ptr<PgConnection>> PgConnection::Connect(const PgOptions& options) {
  FSS_TRY(ValidatePgOptions(options));
  ::PGconn* raw = ::PQconnectdb(options.dsn.c_str());
  if (raw == nullptr) {
    return Err(fss::ErrorKind::kUnavailable,
               "连接 PostgreSQL 失败：PQconnectdb 返回空指针（DSN 无法解析）");
  }
  std::unique_ptr<PgConnection> connection(new PgConnection(raw));
  if (::PQstatus(raw) != CONNECTION_OK) {
    //  ★ fail-closed：连接失败就是 kUnavailable，绝不回退到 SQLite / 内存（ADR-009 §4.1）。
    return Err(fss::ErrorKind::kUnavailable,
               "连接 PostgreSQL 失败：" + connection->LastError());
  }
  FSS_TRY(connection->ApplyStatementTimeout(options.statement_timeout_millis));
  return Ok(std::move(connection));
}

PgConnection::~PgConnection() {
  if (raw_ != nullptr) {
    ::PQfinish(raw_);
    raw_ = nullptr;
  }
}

PgConnection::PgConnection(PgConnection&& other) noexcept : raw_(other.raw_) {
  other.raw_ = nullptr;
}

PgConnection& PgConnection::operator=(PgConnection&& other) noexcept {
  if (this != &other) {
    if (raw_ != nullptr) ::PQfinish(raw_);
    raw_ = other.raw_;
    other.raw_ = nullptr;
  }
  return *this;
}

bool PgConnection::Healthy() const {
  return raw_ != nullptr && ::PQstatus(raw_) == CONNECTION_OK;
}

std::string PgConnection::LastError() const {
  if (raw_ == nullptr) return "连接未初始化";
  const char* message = ::PQerrorMessage(raw_);
  return message != nullptr ? Trim(std::string(message)) : std::string{};
}

fss::Result<void> PgConnection::ApplyStatementTimeout(int millis) {
  if (millis < 0) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "statement_timeout_ms 必须 >= 0（收到 " + std::to_string(millis) + "）");
  }
  //  ★ 全项目**唯一**的非参数化 SQL：`SET` 的值不是占位符能表达的位置。
  //    因此 millis 必须先被校验为"非负十进制整数"（上面的分支），再用 std::to_string
  //    自行格式化 —— 不可能注入。除此之外所有带值的 SQL 都走 ExecParams 占位符。
  const std::string sql = "SET statement_timeout = " + std::to_string(millis);
  const auto result = ExecSimple(sql);
  if (!result.ok()) return Annotate(result.error(), "设置 statement_timeout 失败");
  return Ok();
}

fss::Result<PgResult> PgConnection::ExecParams(const std::string& sql,
                                               const std::vector<Param>& params) {
  if (raw_ == nullptr) return Err(fss::ErrorKind::kUnavailable, "连接未初始化");
  std::vector<const char*> values;
  values.reserve(params.size());
  for (const auto& param : params) {
    values.push_back(param.has_value() ? param->c_str() : nullptr);
  }
  ::PGresult* raw = ::PQexecParams(raw_, sql.c_str(), static_cast<int>(params.size()),
                                   /*paramTypes=*/nullptr, values.data(),
                                   /*paramLengths=*/nullptr, /*paramFormats=*/nullptr,
                                   /*resultFormat=*/0);
  if (raw == nullptr) {
    return Err(fss::ErrorKind::kUnavailable, "执行 SQL 失败：" + LastError());
  }
  PgResult result(raw);
  if (!result.ok()) {
    return MapPgError(result.SqlState(), result.ErrorMessage());
  }
  return Ok(std::move(result));
}

fss::Result<PgResult> PgConnection::ExecSimple(const std::string& sql) {
  if (raw_ == nullptr) return Err(fss::ErrorKind::kUnavailable, "连接未初始化");
  ::PGresult* raw = ::PQexec(raw_, sql.c_str());
  if (raw == nullptr) {
    return Err(fss::ErrorKind::kUnavailable, "执行 SQL 失败：" + LastError());
  }
  PgResult result(raw);
  if (!result.ok()) {
    return MapPgError(result.SqlState(), result.ErrorMessage());
  }
  return Ok(std::move(result));
}

fss::Result<std::int64_t> PgConnection::NowEpochMillis() {
  //  不访问任何分区表 → 用独立定界符（见文件头的 SQL 纪律说明）。
  const auto result = ExecParams(R"pgsql(SELECT (extract(epoch from now()) * 1000)::bigint)pgsql",
                                 {});
  if (!result.ok()) return Annotate(result.error(), "读取数据库时钟失败");
  if (result.value().RowCount() == 0) {
    return Err(fss::ErrorKind::kInternal, "读取数据库时钟失败：查询没有返回行");
  }
  const std::string text = result.value().Value(0, 0);
  if (text.empty()) return Err(fss::ErrorKind::kInternal, "读取数据库时钟失败：返回空值");
  return static_cast<std::int64_t>(std::strtoll(text.c_str(), nullptr, 10));
}

// -----------------------------------------------------------------------------
//  PgPool
// -----------------------------------------------------------------------------
fss::Result<std::unique_ptr<PgPool>> PgPool::Create(PgOptions options) {
  FSS_TRY(ValidatePgOptions(options));
  std::unique_ptr<PgPool> pool(new PgPool());
  pool->options_ = std::move(options);
  //  建池即建一条连接：让 DSN/权限/网络问题在 `Open()` 时暴露（fail-closed），
  //  而不是等到第一个请求才发现依赖不可用。
  auto connection = PgConnection::Connect(pool->options_);
  if (!connection.ok()) return connection.error();
  pool->idle_.push_back(std::move(connection.value()));
  pool->live_ = 1;
  return Ok(std::move(pool));
}

PgPool::~PgPool() {
  std::lock_guard<std::mutex> guard(mutex_);
  idle_.clear();
  live_ = 0;
}

fss::Result<PgPool::Handle> PgPool::Borrow() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    if (!idle_.empty()) {
      std::unique_ptr<PgConnection> connection = std::move(idle_.back());
      idle_.pop_back();
      if (connection->Healthy()) {
        return Handle(this, std::move(connection));
      }
      //  坏连接必须**丢弃**，绝不再次借出；释放一个名额后继续尝试。
      connection.reset();
      --live_;
      continue;
    }
    if (live_ < static_cast<std::size_t>(options_.max_connections)) {
      auto created = PgConnection::Connect(options_);
      if (!created.ok()) return created.error();
      ++live_;
      return Handle(this, std::move(created.value()));
    }
    //  已达上限：等待归还（有界池的背压）。
    available_.wait(lock);
  }
}

void PgPool::Return(std::unique_ptr<PgConnection> connection) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (connection != nullptr && connection->Healthy()) {
    idle_.push_back(std::move(connection));
  } else {
    connection.reset();
    if (live_ > 0) --live_;
  }
  available_.notify_one();
}

fss::Result<std::int64_t> PgPool::NowEpochMillis() {
  FSS_TRY(handle, Borrow());
  return handle->NowEpochMillis();
}

std::size_t PgPool::LiveConnections() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return live_;
}

std::size_t PgPool::IdleConnections() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return idle_.size();
}

// -----------------------------------------------------------------------------
//  PgPool::Handle
// -----------------------------------------------------------------------------
PgPool::Handle::~Handle() {
  if (pool_ != nullptr && connection_ != nullptr) {
    pool_->Return(std::move(connection_));
  }
}

PgPool::Handle::Handle(Handle&& other) noexcept
    : pool_(other.pool_), connection_(std::move(other.connection_)) {
  other.pool_ = nullptr;
}

PgPool::Handle& PgPool::Handle::operator=(Handle&& other) noexcept {
  if (this != &other) {
    if (pool_ != nullptr && connection_ != nullptr) {
      pool_->Return(std::move(connection_));
    }
    pool_ = other.pool_;
    connection_ = std::move(other.connection_);
    other.pool_ = nullptr;
  }
  return *this;
}

}  // namespace fss::infra
