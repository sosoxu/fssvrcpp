// SqliteMetadataRepository 实现。schema 与语义的设计依据见头文件。
#include "infra/metadata/sqlite/sqlite_metadata_repository.h"

#include "common/json/json.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fss::infra {

namespace {

//  ---- SQL（raw string，便于 C3.9 的护栏机械检索：每条语句都必须带 partition_id）----
constexpr const char* kSchemaMetadata = R"sql(
CREATE TABLE IF NOT EXISTS metadata (
  partition_id TEXT    NOT NULL,
  id           TEXT    NOT NULL,
  version      INTEGER NOT NULL,
  is_latest    INTEGER NOT NULL,
  previous_version INTEGER,
  file_source  TEXT    NOT NULL,
  kind         TEXT    NOT NULL,
  name         TEXT    NOT NULL DEFAULT '',
  created_at   INTEGER NOT NULL,
  created_by   TEXT    NOT NULL DEFAULT '',
  data         TEXT    NOT NULL,
  PRIMARY KEY (partition_id, id, version)
))sql";

//  ★ R5/R6：幂等键的唯一约束必须是**部分唯一索引**，且谓词含业务语义（`is_latest = 1`）。
//    去掉谓词会阻断合法版本链（同一 file_source 的后续版本会被拒）。
constexpr const char* kSchemaSourceUnique = R"sql(
CREATE UNIQUE INDEX IF NOT EXISTS ux_metadata_source
  ON metadata (partition_id, file_source) WHERE is_latest = 1)sql";

constexpr const char* kSchemaLatestUnique = R"sql(
CREATE UNIQUE INDEX IF NOT EXISTS ux_metadata_latest
  ON metadata (partition_id, id) WHERE is_latest = 1)sql";

constexpr const char* kSchemaCreatedIndex = R"sql(
CREATE INDEX IF NOT EXISTS ix_metadata_created
  ON metadata (partition_id, created_at, id))sql";

constexpr const char* kSelectLatestById = R"sql(
SELECT version, previous_version, created_at, data FROM metadata
 WHERE partition_id = ? AND id = ? AND is_latest = 1)sql";

constexpr const char* kSelectLatestBySource = R"sql(
SELECT version, previous_version, created_at, data FROM metadata
 WHERE partition_id = ? AND file_source = ? AND is_latest = 1)sql";

constexpr const char* kSelectIdExists = R"sql(
SELECT 1 FROM metadata WHERE partition_id = ? AND id = ? LIMIT 1)sql";

constexpr const char* kSelectListLatest = R"sql(
SELECT version, previous_version, created_at, data FROM metadata
 WHERE partition_id = ? AND is_latest = 1)sql";

constexpr const char* kClearLatest = R"sql(
UPDATE metadata SET is_latest = 0
 WHERE partition_id = ? AND id = ? AND is_latest = 1)sql";

constexpr const char* kInsertVersion = R"sql(
INSERT INTO metadata
  (partition_id, id, version, is_latest, previous_version, file_source, kind, name,
   created_at, created_by, data)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?))sql";

constexpr const char* kDeleteAllVersions = R"sql(
DELETE FROM metadata WHERE partition_id = ? AND id = ?)sql";

constexpr const char* kCountVersions = R"sql(
SELECT COUNT(*) FROM metadata WHERE partition_id = ? AND id = ?)sql";

//  ---- 通用小工具 ----
fss::Error Invalid(const std::string& message) {
  return fss::Err(fss::ErrorKind::kInvalidArgument, message);
}
fss::Error NotFound(const std::string& message) {
  return fss::Err(fss::ErrorKind::kNotFound, message);
}

class Statement {
 public:
  Statement(sqlite3* db, const char* sql) : sql_(sql) {
    rc_ = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
  }
  ~Statement() {
    if (stmt_ != nullptr) sqlite3_finalize(stmt_);
  }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  bool ok() const { return rc_ == SQLITE_OK && stmt_ != nullptr; }
  int rc() const { return rc_; }
  const char* sql() const { return sql_; }
  sqlite3_stmt* get() { return stmt_; }

 private:
  sqlite3_stmt* stmt_ = nullptr;
  int rc_ = SQLITE_ERROR;
  const char* sql_ = nullptr;
};

void BindText(sqlite3_stmt* stmt, int index, std::string_view value) {
  sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

std::string ColumnText(sqlite3_stmt* stmt, int index) {
  const unsigned char* text = sqlite3_column_text(stmt, index);
  const int bytes = sqlite3_column_bytes(stmt, index);
  if (text == nullptr || bytes <= 0) return {};
  return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
}

//  行 → 记录（列顺序固定为 `version, previous_version, created_at, data`）
fss::Result<domain::FileMetadataRecord> RowToRecord(sqlite3_stmt* stmt) {
  FSS_TRY(value, json::ParseObject(ColumnText(stmt, 3)));
  FSS_TRY(record, domain::ParseFileMetadataRecord(value));
  //  ★ 版本以**列**为准（JSON 里的 version 只是同一份数据的拷贝）
  record.version = sqlite3_column_int64(stmt, 0);
  return record;
}

bool HasPrefix(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

//  校验参数（与内存实现逐条对齐；契约里有对应断言）
fss::Result<void> ValidateRecord(std::string_view partition,
                                 const domain::FileMetadataRecord& record) {
  if (partition.empty()) return Invalid("partition 不能为空");
  if (record.id.empty()) return Invalid("record.id 不能为空");
  if (!HasPrefix(record.id, std::string(partition) + ":")) {
    return Invalid("record.id 的前缀必须与 partition 一致");
  }
  const auto& file_source = record.data.dataset_properties.file_source_info.file_source;
  if (file_source.empty()) return Invalid("file_source 不能为空（幂等键缺失）");
  return Ok();
}

}  // namespace

// =============================================================================
//  Open / 析构
// =============================================================================
fss::Result<std::unique_ptr<SqliteMetadataRepository>> SqliteMetadataRepository::Open(
    const std::string& path, const fss::IClock& clock, SqliteMetadataRepositoryOptions options) {
  std::unique_ptr<SqliteMetadataRepository> repository(new SqliteMetadataRepository());
  repository->clock_ = &clock;
  repository->options_ = options;

  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  if (sqlite3_open_v2(path.c_str(), &repository->db_, flags, nullptr) != SQLITE_OK) {
    const std::string message = repository->db_ != nullptr ? sqlite3_errmsg(repository->db_)
                                                           : "sqlite3_open_v2 失败";
    return fss::Err(fss::ErrorKind::kInternal, "打开元数据仓储失败：" + message);
  }
  //  WAL：读不阻塞写；busy_timeout：并发写时等待而不是立刻 SQLITE_BUSY
  char* error = nullptr;
  if (options.wal) {
    sqlite3_exec(repository->db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &error);
    if (error != nullptr) sqlite3_free(error);
  }
  //  ★ 阶段 10（切片 4）：`metadata.sqlite.synchronous` → `PRAGMA synchronous=<n>`。
  //    0 = OFF / 1 = NORMAL（默认）/ 2 = FULL；组合根在启动期就拒绝白名单外的取值。
  //    ⚠ 这个 PRAGMA **不落盘**，验证只能用同连接的 `AppliedPragma("synchronous")`。
  {
    const std::string synchronous_pragma =
        "PRAGMA synchronous=" + std::to_string(options.synchronous_level) + ";";
    char* pragma_error = nullptr;
    if (sqlite3_exec(repository->db_, synchronous_pragma.c_str(), nullptr, nullptr,
                     &pragma_error) != SQLITE_OK) {
      const std::string message = pragma_error != nullptr ? pragma_error : "未知错误";
      if (pragma_error != nullptr) sqlite3_free(pragma_error);
      sqlite3_close(repository->db_);
      repository->db_ = nullptr;
      return fss::Err(fss::ErrorKind::kInvalidArgument,
                      "设置 PRAGMA synchronous 失败：" + message);
    }
    if (pragma_error != nullptr) sqlite3_free(pragma_error);
  }
  sqlite3_busy_timeout(repository->db_, options.busy_timeout_millis);

  for (const char* schema : {kSchemaMetadata, kSchemaSourceUnique, kSchemaLatestUnique,
                             kSchemaCreatedIndex}) {
    char* schema_error = nullptr;
    if (sqlite3_exec(repository->db_, schema, nullptr, nullptr, &schema_error) != SQLITE_OK) {
      const std::string message = schema_error != nullptr ? schema_error : "未知错误";
      if (schema_error != nullptr) sqlite3_free(schema_error);
      return fss::Err(fss::ErrorKind::kInternal, "初始化元数据 schema 失败：" + message);
    }
  }
  return repository;
}

SqliteMetadataRepository::~SqliteMetadataRepository() {
  if (db_ != nullptr) sqlite3_close(db_);
}

// =============================================================================
//  查询（调用方必须已持锁）
// =============================================================================
fss::Result<domain::FileMetadataRecord> SqliteMetadataRepository::ReadRow(sqlite3_stmt* stmt) {
  return RowToRecord(stmt);
}

fss::Result<domain::FileMetadataRecord> SqliteMetadataRepository::FindLatestById(
    sqlite3* db, std::string_view partition, std::string_view record_id) {
  Statement stmt(db, kSelectLatestById);
  if (!stmt.ok()) return fss::Err(fss::ErrorKind::kInternal, std::string("prepare 失败：") + stmt.sql());
  BindText(stmt.get(), 1, partition);
  BindText(stmt.get(), 2, record_id);
  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_DONE) return NotFound("Record Not Found");
  if (rc != SQLITE_ROW) {
    return fss::Err(fss::ErrorKind::kInternal, std::string("查询失败：") + sqlite3_errstr(rc));
  }
  return ReadRow(stmt.get());
}

fss::Result<domain::FileMetadataRecord> SqliteMetadataRepository::FindLatestBySource(
    sqlite3* db, std::string_view partition, std::string_view file_source) {
  Statement stmt(db, kSelectLatestBySource);
  if (!stmt.ok()) return fss::Err(fss::ErrorKind::kInternal, std::string("prepare 失败：") + stmt.sql());
  BindText(stmt.get(), 1, partition);
  BindText(stmt.get(), 2, file_source);
  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_DONE) return NotFound("Record Not Found");
  if (rc != SQLITE_ROW) {
    return fss::Err(fss::ErrorKind::kInternal, std::string("查询失败：") + sqlite3_errstr(rc));
  }
  return ReadRow(stmt.get());
}

// =============================================================================
//  写路径
// =============================================================================
fss::Result<domain::FileMetadataRecord> SqliteMetadataRepository::Create(
    std::string_view partition, const domain::FileMetadataRecord& record) {
  FSS_TRY(ValidateRecord(partition, record));
  const std::string file_source =
      record.data.dataset_properties.file_source_info.file_source;

  std::lock_guard<std::mutex> guard(mutex_);
  //  ★ 幂等键先查：同 (partition, file_source) 已存在 → 返回**第一次那条**（R5）。
  //    这里用"先查后插 + 事务"而不是只靠唯一索引报错，是因为契约要求返回的是
  //    **已存在的那条记录**（客户端重试要拿到同样的 id/version）。
  if (auto existing = FindLatestBySource(db_, partition, file_source); existing.ok()) {
    return existing.value();
  }

  //  同 id 已被别的 file_source 占用 → 拒绝（否则会破坏"一个 id 一条链"的不变量）
  {
    Statement stmt(db_, kSelectIdExists);
    if (!stmt.ok()) return fss::Err(fss::ErrorKind::kInternal, "prepare 失败");
    BindText(stmt.get(), 1, partition);
    BindText(stmt.get(), 2, record.id);
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
      //  ★ 并发实例可能刚刚插入**同一个 file_source** 的那条记录：先按幂等键回读，
      //    只有确实"同 id 但 file_source 不同"才拒绝（ADR-009 M2 / R5）
      if (auto existing = FindLatestBySource(db_, partition, file_source); existing.ok()) {
        return existing.value();
      }
      return Invalid("同 id 已存在且 file_source 不同：" + record.id);
    }
    if (rc != SQLITE_DONE) {
      return fss::Err(fss::ErrorKind::kInternal, std::string("查询失败：") + sqlite3_errstr(rc));
    }
  }

  domain::FileMetadataRecord stored = record;
  stored.version = 1;
  const std::int64_t created_at = clock_->NowEpochSeconds();

  char* error = nullptr;
  sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &error);
  if (error != nullptr) {
    sqlite3_free(error);
    return fss::Err(fss::ErrorKind::kInternal, "开启事务失败");
  }
  {
    Statement stmt(db_, kInsertVersion);
    if (!stmt.ok()) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return fss::Err(fss::ErrorKind::kInternal, "prepare insert 失败");
    }
    BindText(stmt.get(), 1, partition);
    BindText(stmt.get(), 2, stored.id);
    sqlite3_bind_int64(stmt.get(), 3, 1);
    sqlite3_bind_int(stmt.get(), 4, 1);
    sqlite3_bind_null(stmt.get(), 5);
    BindText(stmt.get(), 6, file_source);
    BindText(stmt.get(), 7, stored.kind);
    BindText(stmt.get(), 8, stored.data.name.value_or(""));
    sqlite3_bind_int64(stmt.get(), 9, created_at);
    BindText(stmt.get(), 10, "");
    BindText(stmt.get(), 11, json::Dump(domain::ToJson(stored)));
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      //  ★ 唯一约束挡下并发插入（`ux_metadata_source` / 主键）时，幂等语义要求
      //    返回**已存在的那条记录**，而不是把 UNIQUE 冲突当 500 抛给客户端。
      //    ⚠️ `sqlite3_extended_result_codes` 打开时 `rc` 是**扩展码**，必须按主码比较（P3-D03）。
      if ((rc & 0xFF) == SQLITE_CONSTRAINT) {
        if (auto existing = FindLatestBySource(db_, partition, file_source); existing.ok()) {
          return existing.value();
        }
        return Invalid("同 id 已存在且 file_source 不同：" + record.id);
      }
      return fss::Err(fss::ErrorKind::kInternal, std::string("插入元数据失败：") + sqlite3_errstr(rc));
    }
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return stored;
}

fss::Result<domain::FileMetadataRecord> SqliteMetadataRepository::GetById(
    std::string_view partition, std::string_view record_id) {
  if (partition.empty()) return Invalid("partition 不能为空");
  std::lock_guard<std::mutex> guard(mutex_);
  return FindLatestById(db_, partition, record_id);
}

fss::Result<domain::FileMetadataRecord> SqliteMetadataRepository::GetLatestByFileSource(
    std::string_view partition, std::string_view file_source) {
  if (partition.empty()) return Invalid("partition 不能为空");
  std::lock_guard<std::mutex> guard(mutex_);
  return FindLatestBySource(db_, partition, file_source);
}

fss::Result<domain::FileMetadataRecord> SqliteMetadataRepository::Update(
    std::string_view partition, const domain::FileMetadataRecord& record) {
  FSS_TRY(ValidateRecord(partition, record));
  const std::string file_source =
      record.data.dataset_properties.file_source_info.file_source;

  std::lock_guard<std::mutex> guard(mutex_);
  FSS_TRY(existing, FindLatestById(db_, partition, record.id));

  //  ★ 幂等键必须稳定：允许改内容，不允许改 (partition, file_source)
  const auto& existing_source =
      existing.data.dataset_properties.file_source_info.file_source;
  if (existing_source != file_source) {
    return Invalid("Update 不得改写 (partition, file_source) 幂等键");
  }

  domain::FileMetadataRecord stored = record;
  stored.version = existing.version + 1;
  const std::int64_t created_at = clock_->NowEpochSeconds();

  char* error = nullptr;
  sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &error);
  if (error != nullptr) {
    sqlite3_free(error);
    return fss::Err(fss::ErrorKind::kInternal, "开启事务失败");
  }
  {
    Statement stmt(db_, kClearLatest);
    if (!stmt.ok()) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return fss::Err(fss::ErrorKind::kInternal, "prepare update 失败");
    }
    BindText(stmt.get(), 1, partition);
    BindText(stmt.get(), 2, stored.id);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return fss::Err(fss::ErrorKind::kInternal, "清理旧 latest 标记失败");
    }
  }
  {
    Statement stmt(db_, kInsertVersion);
    if (!stmt.ok()) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return fss::Err(fss::ErrorKind::kInternal, "prepare insert 失败");
    }
    BindText(stmt.get(), 1, partition);
    BindText(stmt.get(), 2, stored.id);
    sqlite3_bind_int64(stmt.get(), 3, stored.version);
    sqlite3_bind_int(stmt.get(), 4, 1);
    sqlite3_bind_int64(stmt.get(), 5, existing.version);
    BindText(stmt.get(), 6, file_source);
    BindText(stmt.get(), 7, stored.kind);
    BindText(stmt.get(), 8, stored.data.name.value_or(""));
    sqlite3_bind_int64(stmt.get(), 9, created_at);
    BindText(stmt.get(), 10, "");
    BindText(stmt.get(), 11, json::Dump(domain::ToJson(stored)));
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return fss::Err(fss::ErrorKind::kInternal,
                      std::string("写入新版本失败：") + sqlite3_errstr(rc));
    }
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return stored;
}

fss::Result<void> SqliteMetadataRepository::Delete(std::string_view partition,
                                                   std::string_view record_id) {
  if (partition.empty()) return Invalid("partition 不能为空");
  std::lock_guard<std::mutex> guard(mutex_);
  //  契约：删除**全部版本**；缺失 → kNotFound（与幂等删除不同，元数据删除要报缺失）
  {
    Statement stmt(db_, kCountVersions);
    if (!stmt.ok()) return fss::Err(fss::ErrorKind::kInternal, "prepare count 失败");
    BindText(stmt.get(), 1, partition);
    BindText(stmt.get(), 2, record_id);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
      return fss::Err(fss::ErrorKind::kInternal, "统计版本失败");
    }
    if (sqlite3_column_int64(stmt.get(), 0) == 0) return NotFound("Record Not Found");
  }
  Statement stmt(db_, kDeleteAllVersions);
  if (!stmt.ok()) return fss::Err(fss::ErrorKind::kInternal, "prepare delete 失败");
  BindText(stmt.get(), 1, partition);
  BindText(stmt.get(), 2, record_id);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    return fss::Err(fss::ErrorKind::kInternal, "删除元数据失败");
  }
  return Ok();
}

fss::Result<domain::MetadataPage> SqliteMetadataRepository::List(
    std::string_view partition, const domain::MetadataQuery& query) {
  if (partition.empty()) return Invalid("partition 不能为空");
  if (query.limit <= 0) return Invalid("limit 必须 > 0");
  if (query.offset < 0) return Invalid("offset 必须 >= 0");

  std::lock_guard<std::mutex> guard(mutex_);
  Statement stmt(db_, kSelectListLatest);
  if (!stmt.ok()) return fss::Err(fss::ErrorKind::kInternal, "prepare list 失败");
  BindText(stmt.get(), 1, partition);

  struct Row {
    domain::FileMetadataRecord record;
    std::int64_t created_at = 0;
    std::string name;
  };
  std::vector<Row> matched;
  while (true) {
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) {
      return fss::Err(fss::ErrorKind::kInternal, std::string("遍历失败：") + sqlite3_errstr(rc));
    }
    FSS_TRY(record, ReadRow(stmt.get()));
    //  ★ 过滤在 C++ 侧做：`name_prefix` 需要读 JSON 里的 `data.Name`，而 SQLite 的 JSON1
    //    扩展在 3.37 上不保证编译进来（3.38 起才默认启用）。分区内记录数在本阶段可控，
    //    因此选择"SQL 只做分区隔离 + 内存过滤"，并把 name 单独抽成列以便将来下推。
    if (query.kind.has_value() && record.kind != *query.kind) continue;
    if (query.name_prefix.has_value()) {
      const auto& name = record.data.name;
      if (!name.has_value() || name->rfind(*query.name_prefix, 0) != 0) continue;
    }
    const std::int64_t created_at = sqlite3_column_int64(stmt.get(), 2);
    if (query.created_after_epoch_seconds >= 0 &&
        created_at < query.created_after_epoch_seconds) {
      continue;  // 含下界
    }
    if (query.created_before_epoch_seconds >= 0 &&
        created_at > query.created_before_epoch_seconds) {
      continue;  // 含上界
    }
    matched.push_back(Row{std::move(record), created_at, {}});
  }

  //  稳定全序：created_at 升序、同秒按 id 升序（分页不漏不重）
  std::sort(matched.begin(), matched.end(), [](const Row& a, const Row& b) {
    if (a.created_at != b.created_at) return a.created_at < b.created_at;
    return a.record.id < b.record.id;
  });

  domain::MetadataPage page;
  page.total = static_cast<std::int64_t>(matched.size());
  for (std::size_t i = static_cast<std::size_t>(query.offset); i < matched.size(); ++i) {
    if (static_cast<int>(page.records.size()) >= query.limit) break;
    page.records.push_back(std::move(matched[i].record));
  }
  return page;
}

std::size_t SqliteMetadataRepository::VersionCount(std::string_view partition,
                                                   std::string_view record_id) {
  std::lock_guard<std::mutex> guard(mutex_);
  Statement stmt(db_, kCountVersions);
  if (!stmt.ok()) return 0;
  BindText(stmt.get(), 1, partition);
  BindText(stmt.get(), 2, record_id);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return 0;
  return static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 0));
}

//  ★ 阶段 10（切片 4）：诊断访问器。为什么需要它（而不是"另开连接读回"）：
//    `PRAGMA synchronous` 是**连接级**设置，**不写进库文件**；另开一个 sqlite3 连接
//    读回只会得到那个新连接自己的默认值（FULL = 2），与被测仓储是否真的下发了 OFF /
//    NORMAL / FULL 完全无关。因此必须在**同一个连接**上读回。
//  PRAGMA 名不能参数化（不是绑定值），只放行白名单里的名字以避免 SQL 注入。
fss::Result<std::string> SqliteMetadataRepository::AppliedPragma(std::string_view name) const {
  if (name != "synchronous") {
    return fss::Err(fss::ErrorKind::kInvalidArgument,
                    "AppliedPragma 只支持 synchronous（收到 '" + std::string(name) + "'）");
  }
  std::lock_guard<std::mutex> guard(mutex_);
  Statement stmt(db_, "PRAGMA synchronous");
  if (!stmt.ok()) return fss::Err(fss::ErrorKind::kInternal, "准备 PRAGMA synchronous 失败");
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    return fss::Err(fss::ErrorKind::kInternal, "读取 PRAGMA synchronous 失败");
  }
  return ColumnText(stmt.get(), 0);
}

}  // namespace fss::infra
