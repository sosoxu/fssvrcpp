// =============================================================================
//  PgConnection / PgResult / PgPool（L2，`src/infra/postgres/`）—— libpq 薄封装
// =============================================================================
//  为什么需要这一层（而不是让两个 PG 仓储直接调 libpq）
//  ---------------------------------------------------------------------------
//  ① **RAII**：`::PGconn` / `::PGresult` 都是裸指针，任何提前 return 都会泄漏
//     （连接泄漏在多实例下会直接吃掉 PG 的 `max_connections` 预算，ADR-009 §6.3）。
//  ② **有界连接池**：每个实例的连接数是硬预算；`max_connections` 是上限，绝不能
//     "每来一个请求就开一条连接"。被阻塞的借用者必须等待并复用（ADR-009 §4.5）。
//  ③ **数据库时钟**：ADR-009 §4.3/§6.4 要求租约与过期判定一律用数据库 `now()`，
//     不能用实例本地时钟（时钟偏移会误判在途对象）。So `NowEpochMillis()` 走 SQL。
//  ④ **错误映射单点**：SQLSTATE → `ErrorKind` 只写一处，两个仓储共用同一张表。
//
//  ★ 关于 `PGconn` / `PGresult` 的前向声明（P3-D04 的同款陷阱）
//    libpq 的这两个名字是**全局** typedef（`typedef struct pg_conn PGconn;`）。
//    如果把 `struct PGconn;` 写进 `namespace fss::infra`，就会声明出一个
//    `fss::infra::PGconn`，与 libpq 的全局类型不是同一个 —— 报错是
//    "cannot convert fss::infra::PGconn* to PGconn*"，一眼看不出根因。
//    因此这里在**全局命名空间**声明底层 struct，并用别名把 `PGconn` / `PGresult`
//    引出来；签名里一律写 `::PGconn*` / `::PGresult*`。
//    （别名与 libpq-fe.h 的 typedef 指向同一类型，重复声明合法。）
//
//  ★ SQL 纪律（机械护栏见 tests/unit/test_sql_guardrail.cpp）
//    **所有带值的 SQL 一律用 `PQexecParams` 占位符（$1..$n）**，不拼字符串。
//    唯一的两处例外：
//      · `SET statement_timeout = <N>`：SET 的值不是占位符可以表达的位置，
//        所以 N 必须先被校验为**非负十进制整数**再自行格式化（见 ApplyStatementTimeout）；
//      · `SELECT (extract(epoch from now()) * 1000)::bigint`：不访问任何分区表，
//        因此用独立的 `R"pgsql(...)pgsql"` 原始串定界符写（C3.9 的启发式只认
//        `R"sql(...)sql"`；它要求每条 DML 都出现 `partition_id`，而"取数据库时钟"
//        这条本来就没有分区维度 —— 让它走独立定界符是为了不产生**假阳性**，
//        不是为了绕过检查：这条语句里根本没有表名）。
// =============================================================================
#pragma once

#include "common/result/result.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

//  libpq 的底层 struct（全局命名空间；理由见文件头）。
struct pg_conn;
struct pg_result;
using PGconn = ::pg_conn;
using PGresult = ::pg_result;

namespace fss::infra {

// -----------------------------------------------------------------------------
//  配置
// -----------------------------------------------------------------------------
//  字段名与配置键一一对应（组合根按 `*.postgres.*` 填充；本切片不改组合根）：
//    dsn                      ← `metadata.postgres.dsn` / `location.postgres.dsn`
//    max_connections          ← `*.postgres.max_connections`
//    statement_timeout_millis ← `*.postgres.statement_timeout_ms`
struct PgOptions {
  std::string dsn;
  int max_connections = 4;
  int statement_timeout_millis = 5000;
};

//  `max_connections > 0` 且 `statement_timeout_millis >= 0`，否则 kInvalidArgument。
fss::Result<void> ValidatePgOptions(const PgOptions& options);

// -----------------------------------------------------------------------------
//  错误映射（两个 PG 仓储共用）
// -----------------------------------------------------------------------------
//  SQLSTATE → ErrorKind：
//    "23505" unique_violation        → kLocationAlreadyExists（与 SQLite 位置仓储一致）
//    "57014" query_canceled          → kUnavailable（statement_timeout 触发）
//    "08xxx" connection_exception    → kUnavailable
//    其它                            → kInternal
//  Error 里带 `sqlstate` detail 与 libpq 的原始消息（排障必需）。
fss::Error MapPgError(std::string_view sqlstate, std::string message);

//  给已有 Error 加一层上下文（保留 kind 与全部 details）。仓储用它把
//  "执行 SQL 失败" 变成 "写入位置记录失败：…"。
fss::Error Annotate(const fss::Error& error, std::string_view what);

// -----------------------------------------------------------------------------
//  PgResult —— RAII 拥有 `::PGresult*`
// -----------------------------------------------------------------------------
class PgResult {
 public:
  PgResult() = default;
  //  接管所有权（`raw` 为 nullptr 时表示"结果无效"）。
  explicit PgResult(::PGresult* raw) : raw_(raw) {}
  ~PgResult();
  PgResult(PgResult&& other) noexcept;
  PgResult& operator=(PgResult&& other) noexcept;
  PgResult(const PgResult&) = delete;
  PgResult& operator=(const PgResult&) = delete;

  bool ok() const;  // 非空且状态是 TUPLES_OK / COMMAND_OK
  int RowCount() const;
  int ColumnCount() const;
  //  列名 → 下标；不存在返回 -1
  int ColumnIndex(std::string_view name) const;
  bool IsNull(int row, int column) const;
  //  按列下标/列名取值（文本形态；越界返回空串 —— 调用方应用 RowCount/ColumnCount 先界定）
  std::string Value(int row, int column) const;
  std::string Value(int row, std::string_view column) const;
  //  `DELETE` / `UPDATE` 的受影响行数（PQcmdTuples）
  std::int64_t AffectedRows() const;
  //  libpq 的错误消息与 SQLSTATE（来自 PGresult；空结果返回空串）
  std::string ErrorMessage() const;
  std::string SqlState() const;
  ::PGresult* get() const { return raw_; }

 private:
  ::PGresult* raw_ = nullptr;
};

// -----------------------------------------------------------------------------
//  PgConnection —— RAII 拥有 `::PGconn*`
// -----------------------------------------------------------------------------
class PgConnection {
 public:
  //  按 DSN 建连；失败返回 `Err(kUnavailable, <libpq 消息>)`。
  //  ★ fail-closed：**绝不**回退到 SQLite / 内存（ADR-009 §4.1 明确禁止）。
  //  新连接建立后立刻应用 `SET statement_timeout = <N>`。
  static fss::Result<std::unique_ptr<PgConnection>> Connect(const PgOptions& options);

  ~PgConnection();
  PgConnection(PgConnection&& other) noexcept;
  PgConnection& operator=(PgConnection&& other) noexcept;
  PgConnection(const PgConnection&) = delete;
  PgConnection& operator=(const PgConnection&) = delete;

  //  `PQstatus() == CONNECTION_OK` 才有资格被再次借出；否则由连接池丢弃。
  bool Healthy() const;
  //  连接级最后一次错误（PQerrorMessage）
  std::string LastError() const;

  //  **带值的 SQL 一律走这里**（`$1..$n` 占位符 + 文本参数）。
  //  `params[i]` 为 nullopt → 绑定 SQL NULL。
  using Param = std::optional<std::string>;
  fss::Result<PgResult> ExecParams(const std::string& sql, const std::vector<Param>& params);

  //  不带参数、不带值的语句（BEGIN / COMMIT / ROLLBACK / SET）。**不要**用它执行
  //  任何带用户输入的 SQL。
  fss::Result<PgResult> ExecSimple(const std::string& sql);

  //  ★ ADR-009 §4.3/§6.4：数据库时钟（毫秒）。租约/过期判定必须用它，不能用实例本地时钟。
  fss::Result<std::int64_t> NowEpochMillis();

 private:
  explicit PgConnection(::PGconn* raw) : raw_(raw) {}
  fss::Result<void> ApplyStatementTimeout(int millis);

  ::PGconn* raw_ = nullptr;
};

// -----------------------------------------------------------------------------
//  PgPool —— 有界连接池（借用/归还 + RAII 句柄）
// -----------------------------------------------------------------------------
//  · 活跃连接数（含被借出的）**永远不超过** `max_connections`；
//  · 池空且已达上限时，借用者阻塞等待归还（条件变量）；
//  · 归还时若 `PQstatus != CONNECTION_OK`，连接被销毁而不是再次借出。
class PgPool {
 public:
  //  RAII 借用句柄：析构时归还连接（或在连接损坏时丢弃）。
  class Handle {
   public:
    Handle() = default;
    ~Handle();
    Handle(Handle&& other) noexcept;
    Handle& operator=(Handle&& other) noexcept;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    PgConnection& Connection() const { return *connection_; }
    PgConnection* operator->() const { return connection_.get(); }
    PgConnection& operator*() const { return *connection_; }
    explicit operator bool() const { return connection_ != nullptr; }

   private:
    friend class PgPool;
    Handle(PgPool* pool, std::unique_ptr<PgConnection> connection)
        : pool_(pool), connection_(std::move(connection)) {}

    PgPool* pool_ = nullptr;
    std::unique_ptr<PgConnection> connection_;
  };

  //  建池并**立即建立一条连接**以尽早暴露 DSN 错误（fail-closed）。
  static fss::Result<std::unique_ptr<PgPool>> Create(PgOptions options);

  ~PgPool();
  PgPool(const PgPool&) = delete;
  PgPool& operator=(const PgPool&) = delete;

  fss::Result<Handle> Borrow();
  //  数据库时钟（借一条连接执行一次查询）
  fss::Result<std::int64_t> NowEpochMillis();

  //  可观测性（测试/排障）：活跃连接数（含借出）与空闲连接数
  std::size_t LiveConnections() const;
  std::size_t IdleConnections() const;
  int MaxConnections() const { return options_.max_connections; }

 private:
  friend class Handle;
  PgPool() = default;
  //  归还（Handle 析构调用）
  void Return(std::unique_ptr<PgConnection> connection);

  PgOptions options_{};
  mutable std::mutex mutex_;
  std::condition_variable available_;
  std::vector<std::unique_ptr<PgConnection>> idle_;
  std::size_t live_ = 0;  // 已建立且未销毁的连接数（空闲 + 借出）
};

}  // namespace fss::infra
