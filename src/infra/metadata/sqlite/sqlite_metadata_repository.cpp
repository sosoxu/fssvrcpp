// SqliteMetadataRepository 实现。schema 与语义的设计依据见头文件。
#include "infra/metadata/sqlite/sqlite_metadata_repository.h"

#include "common/json/json.h"
#include "infra/sqlite/sqlite_group_commit.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <optional>
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
  state        TEXT    NOT NULL DEFAULT 'ready',
  PRIMARY KEY (partition_id, id, version)
))sql";

//  ★ C1（ADR-009 §4.2）：`state` 是**仓储列**，不是 OSDU 记录的一部分。
//    旧库文件（本切片之前创建）没有该列 → `Open` 里做一次幂等迁移补默认值 'ready'。
constexpr const char* kPragmaTableInfo = "PRAGMA table_info(metadata)";
constexpr const char* kMigrateAddState =
    "ALTER TABLE metadata ADD COLUMN state TEXT NOT NULL DEFAULT 'ready';";

//  ★ R5/R6：幂等键的唯一约束必须是**部分唯一索引**，且谓词含业务语义（`is_latest = 1`）。
//    去掉谓词会阻断合法版本链（同一 file_source 的后续版本会被拒）。
//    ★ SQLite 的 `Delete` 是**硬删除**（不存在 state='deleted' 的行），所以谓词无需含 state。
constexpr const char* kSchemaSourceUnique = R"sql(
CREATE UNIQUE INDEX IF NOT EXISTS ux_metadata_source
  ON metadata (partition_id, file_source) WHERE is_latest = 1)sql";

constexpr const char* kSchemaLatestUnique = R"sql(
CREATE UNIQUE INDEX IF NOT EXISTS ux_metadata_latest
  ON metadata (partition_id, id) WHERE is_latest = 1)sql";

constexpr const char* kSchemaCreatedIndex = R"sql(
CREATE INDEX IF NOT EXISTS ix_metadata_created
  ON metadata (partition_id, created_at, id))sql";

//  ★ C1：所有读取路径都只认 `state = 'ready'` —— claiming 行绝不能交给客户端。
constexpr const char* kSelectLatestById = R"sql(
SELECT version, previous_version, created_at, data FROM metadata
 WHERE partition_id = ? AND id = ? AND is_latest = 1 AND state = 'ready')sql";

constexpr const char* kSelectLatestBySource = R"sql(
SELECT version, previous_version, created_at, data FROM metadata
 WHERE partition_id = ? AND file_source = ? AND is_latest = 1 AND state = 'ready')sql";

//  ★ C1：领取路径按幂等键取**活动**行（claiming 或 ready），连带回传状态。
constexpr const char* kSelectClaimBySource = R"sql(
SELECT version, state, data FROM metadata
 WHERE partition_id = ? AND file_source = ? AND is_latest = 1 AND state <> 'deleted')sql";

//  ★ C1：MarkReady 失败后的**歧义消解读**（0 行更新时用来区分"版本不存在"与"幂等键不符"）。
constexpr const char* kSelectVersionState = R"sql(
SELECT state, file_source FROM metadata
 WHERE partition_id = ? AND id = ? AND version = ?)sql";

constexpr const char* kSelectListLatest = R"sql(
SELECT version, previous_version, created_at, data FROM metadata
 WHERE partition_id = ? AND is_latest = 1 AND state = 'ready')sql";

constexpr const char* kClearLatest = R"sql(
UPDATE metadata SET is_latest = 0
 WHERE partition_id = ? AND id = ? AND is_latest = 1 AND state = 'ready')sql";

//  ★ C1：写入一行版本。`state` 由调用方绑定（`Update` 写 'ready'，领取写 'claiming'）——
//    `state` 是仓储列、不是记录的一部分，因此不能从 `data` JSON 里取。
constexpr const char* kInsertVersion = R"sql(
INSERT INTO metadata
  (partition_id, id, version, is_latest, previous_version, file_source, kind, name,
   created_at, created_by, data, state)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?))sql";

//  ★ C1：claiming → ready，并一次性落库**最终**记录（含复制后才算出的 checksum）。
//    `file_source = ?` 是幂等键守卫：不一致时 0 行更新 → 由 kSelectVersionState 判为
//    kInvalidArgument，且行**保持 claiming**（调用方仍可 ReleaseClaim）。
constexpr const char* kMarkReady = R"sql(
UPDATE metadata SET state = 'ready', data = ?, kind = ?, name = ?
 WHERE partition_id = ? AND id = ? AND version = ? AND state = 'claiming' AND file_source = ?)sql";

//  ★ C1：放弃领取 —— 只删 claiming 行（ready 行删不到 → 0 行 → kNotFound）。
constexpr const char* kReleaseClaim = R"sql(
DELETE FROM metadata
 WHERE partition_id = ? AND id = ? AND version = ? AND state = 'claiming')sql";

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

//  ★ C1：`state` 列 → 领域枚举。未知/空值按 `ready` 处理（列的 DEFAULT 就是 'ready'，
//    旧库迁移后也只有该值），与 PG 实现的字符串映射一致。
domain::MetadataState ParseMetadataState(std::string_view text) {
  if (text == "claiming") return domain::MetadataState::kClaiming;
  if (text == "deleted") return domain::MetadataState::kDeleted;
  return domain::MetadataState::kReady;
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

//  语句级插入一行版本（**无事务控制**：调用方负责 BEGIN/COMMIT 或 SAVEPOINT）。
//  返回的是 SQLite 的返回码而不是 `Result`，因为调用方要按**主码**做唯一约束恢复
//  （P3-D03：开了 `sqlite3_extended_result_codes` 时 rc 是扩展码，必须 `rc & 0xFF`）。
struct InsertOutcome {
  int rc = SQLITE_ERROR;
  bool ok() const { return rc == SQLITE_DONE; }
};

InsertOutcome InsertVersionRow(sqlite3* db, std::string_view partition,
                               const domain::FileMetadataRecord& stored,
                               std::string_view file_source, std::int64_t created_at,
                               std::optional<std::int64_t> previous_version,
                               std::string_view state) {
  Statement stmt(db, kInsertVersion);
  if (!stmt.ok()) return InsertOutcome{SQLITE_ERROR};
  BindText(stmt.get(), 1, partition);
  BindText(stmt.get(), 2, stored.id);
  sqlite3_bind_int64(stmt.get(), 3, static_cast<sqlite3_int64>(stored.version));
  sqlite3_bind_int(stmt.get(), 4, 1);
  if (previous_version.has_value()) {
    sqlite3_bind_int64(stmt.get(), 5, static_cast<sqlite3_int64>(*previous_version));
  } else {
    sqlite3_bind_null(stmt.get(), 5);
  }
  BindText(stmt.get(), 6, file_source);
  BindText(stmt.get(), 7, stored.kind);
  BindText(stmt.get(), 8, stored.data.name.value_or(""));
  sqlite3_bind_int64(stmt.get(), 9, static_cast<sqlite3_int64>(created_at));
  BindText(stmt.get(), 10, "");
  BindText(stmt.get(), 11, json::Dump(domain::ToJson(stored)));
  BindText(stmt.get(), 12, state);
  return InsertOutcome{sqlite3_step(stmt.get())};
}

//  ★ C1：`claiming → ready` 的**语句级**实现（无事务控制，调用方负责）。
//    成功时把**最终**记录（含复制后才算出的 checksum）一次写入 data/kind/name，
//    并返回该记录；0 行更新时做歧义消解：
//      · 版本不存在 / 不是 claiming → kNotFound
//      · 是 claiming 但 `file_source` 与调用方给的不一致 → kInvalidArgument
//        （**保持 claiming**，调用方仍可 ReleaseClaim —— 幂等键不允许在 mark-ready 时改写）
fss::Result<domain::FileMetadataRecord> MarkReadyRow(sqlite3* db, std::string_view partition,
                                                     std::string_view record_id,
                                                     std::int64_t version,
                                                     const domain::FileMetadataRecord& record) {
  const std::string file_source =
      record.data.dataset_properties.file_source_info.file_source;
  domain::FileMetadataRecord stored = record;
  stored.id = std::string(record_id);
  stored.version = version;

  {
    Statement stmt(db, kMarkReady);
    if (!stmt.ok()) return fss::Err(fss::ErrorKind::kInternal, "prepare mark-ready 失败");
    BindText(stmt.get(), 1, json::Dump(domain::ToJson(stored)));
    BindText(stmt.get(), 2, stored.kind);
    BindText(stmt.get(), 3, stored.data.name.value_or(""));
    BindText(stmt.get(), 4, partition);
    BindText(stmt.get(), 5, record_id);
    sqlite3_bind_int64(stmt.get(), 6, static_cast<sqlite3_int64>(version));
    BindText(stmt.get(), 7, file_source);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
      return fss::Err(fss::ErrorKind::kInternal, "mark-ready 失败");
    }
    if (sqlite3_changes(db) > 0) return stored;
  }

  Statement probe(db, kSelectVersionState);
  if (!probe.ok()) return fss::Err(fss::ErrorKind::kInternal, "prepare 查询失败");
  BindText(probe.get(), 1, partition);
  BindText(probe.get(), 2, record_id);
  sqlite3_bind_int64(probe.get(), 3, static_cast<sqlite3_int64>(version));
  const int rc = sqlite3_step(probe.get());
  if (rc == SQLITE_DONE) return NotFound("Record Not Found");
  if (rc != SQLITE_ROW) {
    return fss::Err(fss::ErrorKind::kInternal, std::string("查询失败：") + sqlite3_errstr(rc));
  }
  if (ColumnText(probe.get(), 0) != "claiming") return NotFound("Record Not Found");
  if (ColumnText(probe.get(), 1) != file_source) {
    return Invalid("MarkReady 不得改写 (partition, file_source) 幂等键");
  }
  return NotFound("Record Not Found");
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
  //  ★ C1 迁移（幂等）：旧库文件（本切片之前创建）没有 `state` 列，`CREATE TABLE IF NOT EXISTS`
  //    不会补列 —— 必须显式 `ALTER TABLE ... ADD COLUMN`，否则读取路径引用该列会报
  //    "no such column: state"。已有该列时不动作（所以可以反复打开同一个库文件）。
  {
    bool has_state = false;
    Statement stmt(repository->db_, kPragmaTableInfo);
    if (stmt.ok()) {
      while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        if (ColumnText(stmt.get(), 1) == "state") {
          has_state = true;
          break;
        }
      }
    }
    if (!has_state) {
      char* migration_error = nullptr;
      if (sqlite3_exec(repository->db_, kMigrateAddState, nullptr, nullptr, &migration_error) !=
          SQLITE_OK) {
        const std::string message = migration_error != nullptr ? migration_error : "未知错误";
        if (migration_error != nullptr) sqlite3_free(migration_error);
        return fss::Err(fss::ErrorKind::kInternal, "迁移 metadata.state 列失败：" + message);
      }
    }
  }
  //  ★ 本切片：组提交协调器（`group_commit=true` 时用；`false` 时逐操作路径根本不碰它，
  //    因此仍然创建 —— 让"逐操作路径"与"批路径"共享同一份选项/接缝）。
  SqliteGroupCommitOptions group_options;
  group_options.group_commit = options.group_commit;
  group_options.group_commit_max_wait_ms = options.group_commit_max_wait_ms;
  group_options.group_commit_max_batch = options.group_commit_max_batch;
  group_options.batch_observer = options.batch_observer;
  group_options.batch_gate = options.batch_gate;
  group_options.commit_fault = options.commit_fault;
  group_options.metrics = options.metrics;
  group_options.metrics_repo = "metadata";
  repository->committer_ = std::make_unique<SqliteGroupCommitter>(
      repository->db_, repository->mutex_, group_options);
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

fss::Result<domain::FileMetadataRecord> SqliteMetadataRepository::FindClaimBySource(
    sqlite3* db, std::string_view partition, std::string_view file_source,
    domain::MetadataState* state) {
  Statement stmt(db, kSelectClaimBySource);
  if (!stmt.ok()) return fss::Err(fss::ErrorKind::kInternal, std::string("prepare 失败：") + stmt.sql());
  BindText(stmt.get(), 1, partition);
  BindText(stmt.get(), 2, file_source);
  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_DONE) return NotFound("Record Not Found");
  if (rc != SQLITE_ROW) {
    return fss::Err(fss::ErrorKind::kInternal, std::string("查询失败：") + sqlite3_errstr(rc));
  }
  //  列顺序：version, state, data（与 RowToRecord 的 4 列布局不同，故单独解析）
  FSS_TRY(value, json::ParseObject(ColumnText(stmt.get(), 2)));
  FSS_TRY(record, domain::ParseFileMetadataRecord(value));
  record.version = sqlite3_column_int64(stmt.get(), 0);
  if (state != nullptr) *state = ParseMetadataState(ColumnText(stmt.get(), 1));
  return record;
}

// =============================================================================
//  写路径
// =============================================================================
//  ★ 本切片的结构：每个写操作有两个**语句级**实现（`...InTransaction`），
//    区别只在"谁负责事务/保存点"：
//      · `group_commit=false` → `...Locked`：自己 `BEGIN IMMEDIATE … COMMIT`，
//        与接线前逐字一致（每次调用一次事务）；
//      · `group_commit=true`  → 批协调器在**一个**事务里调用 `...InTransaction`，
//        每个操作外裹 `SAVEPOINT`（每操作原子性，见共用小工具的头注释）。
//    `...Locked` 要求调用方已持 `mutex_`；`...InTransaction` 由协调器的领队线程在
//    持锁的事务内调用。
// =============================================================================
void SqliteMetadataRepository::NotifyBatch(std::size_t ops_in_batch, bool committed) {
  if (options_.batch_observer != nullptr) {
    options_.batch_observer->OnBatchCommitted(ops_in_batch, committed);
  }
  if (options_.metrics != nullptr) {
    const fss::metrics::Labels labels{{"repo", "metadata"}};
    options_.metrics->Increment("fss_sqlite_group_commits_total", labels);
    options_.metrics->Increment("fss_sqlite_ops_total", labels,
                                static_cast<std::int64_t>(ops_in_batch));
  }
}

fss::Result<domain::FileMetadataRecord> SqliteMetadataRepository::Create(
    std::string_view partition, const domain::FileMetadataRecord& record) {
  FSS_TRY(ValidateRecord(partition, record));
  //  ★ C1：用新原语实现（claim → mark ready）。可观测语义与接线前逐字一致：
  //    同幂等键返回第一次那条；否则写入 v1 ready 记录。
  FSS_TRY(claim, ClaimForWrite(partition, record));
  if (!claim.claimed) {
    //  既有记录（ready，或另一个实例正在 claiming）→ 返回既有 id（旧 Create 的幂等语义）。
    return claim.record;
  }
  return MarkReady(partition, claim.record.id, claim.record.version, claim.record);
}

fss::Result<domain::MetadataClaim> SqliteMetadataRepository::ClaimForWrite(
    std::string_view partition, const domain::FileMetadataRecord& record) {
  FSS_TRY(ValidateRecord(partition, record));
  if (options_.group_commit) {
    //  批内路径：整个操作（事务内重查幂等键 + 插入 claiming 行）作为一个保存点单位。
    const std::string part(partition);
    return committer_->Submit<domain::MetadataClaim>([this, part, record](sqlite3* db) {
      return ClaimForWriteInTransaction(db, part, record);
    });
  }
  std::lock_guard<std::mutex> guard(mutex_);
  return ClaimForWriteLocked(partition, record);
}

fss::Result<domain::MetadataClaim> SqliteMetadataRepository::ClaimForWriteLocked(
    std::string_view partition, const domain::FileMetadataRecord& record) {
  const std::string file_source =
      record.data.dataset_properties.file_source_info.file_source;

  //  快路径：同幂等键已有活动记录 → 不开启事务（与接线前 CreateLocked 的幂等分支同形）。
  {
    domain::MetadataState state = domain::MetadataState::kReady;
    if (auto existing = FindClaimBySource(db_, partition, file_source, &state); existing.ok()) {
      domain::MetadataClaim out;
      out.claimed = false;
      out.record = existing.value();
      out.state = state;
      return out;
    }
  }

  char* error = nullptr;
  sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &error);
  if (error != nullptr) {
    sqlite3_free(error);
    NotifyBatch(1, false);
    return fss::Err(fss::ErrorKind::kInternal, "开启事务失败");
  }
  //  ★ 原子性来自这里的**事务内重查**：`BEGIN IMMEDIATE` 拿到写锁后，并发的第二个连接
  //    必须等到本事务结束才能进来，因此"重查 + 插入"之间不可能插入别的写入者
  //    （SQLite 语义等价于 PG 的单语句 `ON CONFLICT DO NOTHING`，见 R1 注入②的对照）。
  {
    domain::MetadataState state = domain::MetadataState::kReady;
    if (auto existing = FindClaimBySource(db_, partition, file_source, &state); existing.ok()) {
      sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
      NotifyBatch(1, false);  //  该操作成功，但**没有**写入任何行
      domain::MetadataClaim out;
      out.claimed = false;
      out.record = existing.value();
      out.state = state;
      return out;
    }
  }

  domain::FileMetadataRecord stored = record;
  stored.version = 1;
  const std::int64_t created_at = clock_->NowEpochSeconds();
  const auto inserted = InsertVersionRow(db_, partition, stored, file_source, created_at,
                                         std::nullopt, "claiming");
  if (!inserted.ok()) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    //  ★ 唯一约束挡下并发插入（`ux_metadata_source` / 主键）时按幂等语义回读既有记录，
    //    绝不把 UNIQUE 冲突当 500 抛给客户端（ADR-009 M2 / R5）。
    //    ⚠️ `sqlite3_extended_result_codes` 打开时 `rc` 是**扩展码**，必须按主码比较（P3-D03）。
    if ((inserted.rc & 0xFF) == SQLITE_CONSTRAINT) {
      domain::MetadataState state = domain::MetadataState::kReady;
      if (auto existing = FindClaimBySource(db_, partition, file_source, &state); existing.ok()) {
        NotifyBatch(1, false);
        domain::MetadataClaim out;
        out.claimed = false;
        out.record = existing.value();
        out.state = state;
        return out;
      }
      NotifyBatch(1, false);
      return Invalid("同 id 已存在且 file_source 不同：" + record.id);
    }
    NotifyBatch(1, false);
    return fss::Err(fss::ErrorKind::kInternal,
                    std::string("插入元数据失败：") + sqlite3_errstr(inserted.rc));
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  NotifyBatch(1, true);
  domain::MetadataClaim out;
  out.claimed = true;
  out.record = stored;
  out.state = domain::MetadataState::kClaiming;
  return out;
}

//  批内路径：同一段语句，但**不**自己 BEGIN/COMMIT（事务与保存点由协调器负责）。
fss::Result<domain::MetadataClaim> SqliteMetadataRepository::ClaimForWriteInTransaction(
    sqlite3* db, std::string_view partition, const domain::FileMetadataRecord& record) {
  const std::string file_source =
      record.data.dataset_properties.file_source_info.file_source;

  {
    domain::MetadataState state = domain::MetadataState::kReady;
    if (auto existing = FindClaimBySource(db, partition, file_source, &state); existing.ok()) {
      domain::MetadataClaim out;
      out.claimed = false;
      out.record = existing.value();
      out.state = state;
      return out;
    }
  }

  domain::FileMetadataRecord stored = record;
  stored.version = 1;
  const std::int64_t created_at = clock_->NowEpochSeconds();
  const auto inserted = InsertVersionRow(db, partition, stored, file_source, created_at,
                                         std::nullopt, "claiming");
  if (!inserted.ok()) {
    if ((inserted.rc & 0xFF) == SQLITE_CONSTRAINT) {
      domain::MetadataState state = domain::MetadataState::kReady;
      if (auto existing = FindClaimBySource(db, partition, file_source, &state); existing.ok()) {
        domain::MetadataClaim out;
        out.claimed = false;
        out.record = existing.value();
        out.state = state;
        return out;
      }
      return Invalid("同 id 已存在且 file_source 不同：" + record.id);
    }
    return fss::Err(fss::ErrorKind::kInternal,
                    std::string("插入元数据失败：") + sqlite3_errstr(inserted.rc));
  }
  domain::MetadataClaim out;
  out.claimed = true;
  out.record = stored;
  out.state = domain::MetadataState::kClaiming;
  return out;
}

// =============================================================================
//  MarkReady / ReleaseClaim（claiming → ready / 放弃领取）
// =============================================================================
fss::Result<domain::FileMetadataRecord> SqliteMetadataRepository::MarkReady(
    std::string_view partition, std::string_view record_id, std::int64_t version,
    const domain::FileMetadataRecord& record) {
  FSS_TRY(ValidateRecord(partition, record));
  //  ★ `record_id` / `version`（列语义）是权威的：不做"record.version 必须等于 version"的
  //    拒绝（否则"版本不存在 → kNotFound"的契约判据会变成 kInvalidArgument）。
  if (options_.group_commit) {
    const std::string part(partition);
    const std::string id(record_id);
    return committer_->Submit<domain::FileMetadataRecord>(
        [this, part, id, version, record](sqlite3* db) {
          return MarkReadyInTransaction(db, part, id, version, record);
        });
  }
  std::lock_guard<std::mutex> guard(mutex_);
  return MarkReadyLocked(partition, record_id, version, record);
}

fss::Result<domain::FileMetadataRecord> SqliteMetadataRepository::MarkReadyLocked(
    std::string_view partition, std::string_view record_id, std::int64_t version,
    const domain::FileMetadataRecord& record) {
  char* error = nullptr;
  sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &error);
  if (error != nullptr) {
    sqlite3_free(error);
    NotifyBatch(1, false);
    return fss::Err(fss::ErrorKind::kInternal, "开启事务失败");
  }
  const auto updated = MarkReadyRow(db_, partition, record_id, version, record);
  if (updated.ok()) {
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    NotifyBatch(1, true);
    return updated;
  }
  sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
  NotifyBatch(1, false);
  return updated;
}

fss::Result<domain::FileMetadataRecord> SqliteMetadataRepository::MarkReadyInTransaction(
    sqlite3* db, std::string_view partition, std::string_view record_id, std::int64_t version,
    const domain::FileMetadataRecord& record) {
  //  批内路径：语句级实现；失败时协调器回滚本操作的保存点，因此半成品不会留下。
  return MarkReadyRow(db, partition, record_id, version, record);
}

fss::Result<void> SqliteMetadataRepository::ReleaseClaim(std::string_view partition,
                                                         std::string_view record_id,
                                                         std::int64_t version) {
  if (partition.empty()) return Invalid("partition 不能为空");
  if (options_.group_commit) {
    const std::string part(partition);
    const std::string id(record_id);
    return committer_->Submit<void>(
        [this, part, id, version](sqlite3* db) {
          return ReleaseClaimInTransaction(db, part, id, version);
        });
  }
  std::lock_guard<std::mutex> guard(mutex_);
  return ReleaseClaimLocked(partition, record_id, version);
}

fss::Result<void> SqliteMetadataRepository::ReleaseClaimLocked(std::string_view partition,
                                                              std::string_view record_id,
                                                              std::int64_t version) {
  Statement stmt(db_, kReleaseClaim);
  if (!stmt.ok()) return fss::Err(fss::ErrorKind::kInternal, "prepare release 失败");
  BindText(stmt.get(), 1, partition);
  BindText(stmt.get(), 2, record_id);
  sqlite3_bind_int64(stmt.get(), 3, static_cast<sqlite3_int64>(version));
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    NotifyBatch(1, false);
    return fss::Err(fss::ErrorKind::kInternal, "放弃领取失败");
  }
  if (sqlite3_changes(db_) == 0) {
    //  只删 claiming 行 → 版本不存在 / 已是 ready 都归为 kNotFound（与 PG 实现一致）。
    return NotFound("Record Not Found（该版本不是 claiming 状态）");
  }
  NotifyBatch(1, true);  //  单语句自动提交 = 一次提交（与 DeleteLocked 同口径）
  return Ok();
}

fss::Result<void> SqliteMetadataRepository::ReleaseClaimInTransaction(
    sqlite3* db, std::string_view partition, std::string_view record_id, std::int64_t version) {
  Statement stmt(db, kReleaseClaim);
  if (!stmt.ok()) return fss::Err(fss::ErrorKind::kInternal, "prepare release 失败");
  BindText(stmt.get(), 1, partition);
  BindText(stmt.get(), 2, record_id);
  sqlite3_bind_int64(stmt.get(), 3, static_cast<sqlite3_int64>(version));
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    return fss::Err(fss::ErrorKind::kInternal, "放弃领取失败");
  }
  if (sqlite3_changes(db) == 0) {
    return NotFound("Record Not Found（该版本不是 claiming 状态）");
  }
  return Ok();
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
  if (options_.group_commit) {
    const std::string part(partition);
    return committer_->Submit<domain::FileMetadataRecord>(
        [this, part, record](sqlite3* db) { return UpdateInTransaction(db, part, record); });
  }
  std::lock_guard<std::mutex> guard(mutex_);
  return UpdateLocked(partition, record);
}

fss::Result<domain::FileMetadataRecord> SqliteMetadataRepository::UpdateLocked(
    std::string_view partition, const domain::FileMetadataRecord& record) {
  const std::string file_source =
      record.data.dataset_properties.file_source_info.file_source;

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
    NotifyBatch(1, false);
    return fss::Err(fss::ErrorKind::kInternal, "开启事务失败");
  }
  {
    Statement stmt(db_, kClearLatest);
    if (!stmt.ok()) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      NotifyBatch(1, false);
      return fss::Err(fss::ErrorKind::kInternal, "prepare update 失败");
    }
    BindText(stmt.get(), 1, partition);
    BindText(stmt.get(), 2, stored.id);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      NotifyBatch(1, false);
      return fss::Err(fss::ErrorKind::kInternal, "清理旧 latest 标记失败");
    }
  }
  const auto inserted = InsertVersionRow(db_, partition, stored, file_source, created_at,
                                         existing.version, "ready");
  if (!inserted.ok()) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    NotifyBatch(1, false);
    return fss::Err(fss::ErrorKind::kInternal,
                    std::string("写入新版本失败：") + sqlite3_errstr(inserted.rc));
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  NotifyBatch(1, true);
  return stored;
}

//  批内路径：`kClearLatest` + 插入新版本两条语句；失败时由协调器回滚保存点，
//  因此"清旧 latest 标记"这个**半个操作**不会留下来（每操作原子性）。
fss::Result<domain::FileMetadataRecord> SqliteMetadataRepository::UpdateInTransaction(
    sqlite3* db, std::string_view partition, const domain::FileMetadataRecord& record) {
  const std::string file_source =
      record.data.dataset_properties.file_source_info.file_source;

  FSS_TRY(existing, FindLatestById(db, partition, record.id));

  const auto& existing_source =
      existing.data.dataset_properties.file_source_info.file_source;
  if (existing_source != file_source) {
    return Invalid("Update 不得改写 (partition, file_source) 幂等键");
  }

  domain::FileMetadataRecord stored = record;
  stored.version = existing.version + 1;
  const std::int64_t created_at = clock_->NowEpochSeconds();

  {
    Statement stmt(db, kClearLatest);
    if (!stmt.ok()) return fss::Err(fss::ErrorKind::kInternal, "prepare update 失败");
    BindText(stmt.get(), 1, partition);
    BindText(stmt.get(), 2, stored.id);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
      return fss::Err(fss::ErrorKind::kInternal, "清理旧 latest 标记失败");
    }
  }
  const auto inserted = InsertVersionRow(db, partition, stored, file_source, created_at,
                                         existing.version, "ready");
  if (!inserted.ok()) {
    return fss::Err(fss::ErrorKind::kInternal,
                    std::string("写入新版本失败：") + sqlite3_errstr(inserted.rc));
  }
  return stored;
}


fss::Result<void> SqliteMetadataRepository::Delete(std::string_view partition,
                                                   std::string_view record_id) {
  if (partition.empty()) return Invalid("partition 不能为空");
  if (options_.group_commit) {
    const std::string part(partition);
    const std::string id(record_id);
    return committer_->Submit<void>(
        [this, part, id](sqlite3* db) { return DeleteInTransaction(db, part, id); });
  }
  std::lock_guard<std::mutex> guard(mutex_);
  return DeleteLocked(partition, record_id);
}

fss::Result<void> SqliteMetadataRepository::DeleteLocked(std::string_view partition,
                                                         std::string_view record_id) {
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
    NotifyBatch(1, false);
    return fss::Err(fss::ErrorKind::kInternal, "删除元数据失败");
  }
  //  单语句自动提交：一次语句 = 一次提交（与接线前一致）。
  NotifyBatch(1, true);
  return Ok();
}

fss::Result<void> SqliteMetadataRepository::DeleteInTransaction(sqlite3* db,
                                                                 std::string_view partition,
                                                                 std::string_view record_id) {
  {
    Statement stmt(db, kCountVersions);
    if (!stmt.ok()) return fss::Err(fss::ErrorKind::kInternal, "prepare count 失败");
    BindText(stmt.get(), 1, partition);
    BindText(stmt.get(), 2, record_id);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
      return fss::Err(fss::ErrorKind::kInternal, "统计版本失败");
    }
    if (sqlite3_column_int64(stmt.get(), 0) == 0) return NotFound("Record Not Found");
  }
  Statement stmt(db, kDeleteAllVersions);
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
