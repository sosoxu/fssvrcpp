// SqliteLocationRepository 实现。schema 与分区隔离的设计依据见头文件。
#include "infra/location/sqlite/sqlite_location_repository.h"

#include "common/json/json.h"
#include "infra/sqlite/sqlite_group_commit.h"

#include <sqlite3.h>

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace fss::infra {

namespace {

//  ---- SQL（全部写成 raw string，便于 C3.9 的护栏机械检索）----
//  ★ 每条**语句**都必须带 partition_id；这是契约级的租户隔离，不是可选项。
constexpr const char* kSchemaLocations = R"sql(
CREATE TABLE IF NOT EXISTS file_locations (
  partition_id TEXT    NOT NULL,
  file_id      TEXT    NOT NULL,
  file_source  TEXT    NOT NULL,
  container    TEXT    NOT NULL,
  object_key   TEXT    NOT NULL,
  zone         TEXT    NOT NULL,
  driver       TEXT    NOT NULL,
  created_by   TEXT    NOT NULL,
  created_at   INTEGER NOT NULL,
  updated_at   INTEGER NOT NULL,
  signed_url   TEXT    NOT NULL DEFAULT '',
  data         TEXT    NOT NULL,
  PRIMARY KEY (partition_id, file_id)
))sql";

constexpr const char* kSchemaSourceIndex = R"sql(
CREATE UNIQUE INDEX IF NOT EXISTS idx_fl_source ON file_locations(partition_id, file_source))sql";

constexpr const char* kSchemaPartitionCreatedIndex = R"sql(
CREATE INDEX IF NOT EXISTS idx_fl_partition_created ON file_locations(partition_id, created_at))sql";

constexpr const char* kSchemaZoneIndex = R"sql(
CREATE INDEX IF NOT EXISTS idx_fl_zone ON file_locations(partition_id, zone))sql";

constexpr const char* kSelectByFileId =
    R"sql(SELECT data FROM file_locations WHERE partition_id = ? AND file_id = ?)sql";

constexpr const char* kSelectByFileSource =
    R"sql(SELECT data FROM file_locations WHERE partition_id = ? AND file_source = ?)sql";

constexpr const char* kSelectListBase =
    R"sql(SELECT data FROM file_locations WHERE partition_id = ?)sql";

constexpr const char* kCountListBase =
    R"sql(SELECT COUNT(*) FROM file_locations WHERE partition_id = ?)sql";

constexpr const char* kUpsert = R"sql(
INSERT INTO file_locations
  (partition_id, file_id, file_source, container, object_key, zone, driver, created_by,
   created_at, updated_at, signed_url, data)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(partition_id, file_id) DO UPDATE SET
  file_source = excluded.file_source,
  container   = excluded.container,
  object_key  = excluded.object_key,
  zone        = excluded.zone,
  driver      = excluded.driver,
  created_by  = excluded.created_by,
  created_at  = excluded.created_at,
  updated_at  = excluded.updated_at,
  signed_url  = excluded.signed_url,
  data        = excluded.data)sql";

constexpr const char* kUpdateSignedUrl = R"sql(
UPDATE file_locations SET signed_url = ?, updated_at = ?, data = ?
WHERE partition_id = ? AND file_id = ?)sql";

constexpr const char* kDelete =
    R"sql(DELETE FROM file_locations WHERE partition_id = ? AND file_id = ?)sql";

//  ---- 错误 ----
fss::Error SqliteError(sqlite3* db, const std::string& what) {
  const int code = db != nullptr ? sqlite3_extended_errcode(db) : SQLITE_ERROR;
  const std::string message =
      db != nullptr ? sqlite3_errmsg(db) : std::string("sqlite 未初始化");
  fss::ErrorKind kind = fss::ErrorKind::kInternal;
  if (code == SQLITE_BUSY || code == SQLITE_LOCKED) {
    kind = fss::ErrorKind::kUnavailable;
  }
  return fss::Err(kind, what + "：" + message).With("sqlite_code", std::to_string(code));
}

fss::Error Invalid(const std::string& message) {
  return fss::Err(fss::ErrorKind::kInvalidArgument, message);
}
fss::Error NotFound(const std::string& message) {
  return fss::Err(fss::ErrorKind::kNotFound, message);
}

//  ---- 语句 RAII ----
class Statement {
 public:
  Statement(sqlite3* db, const char* sql) {
    rc_ = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
  }
  ~Statement() {
    if (stmt_ != nullptr) sqlite3_finalize(stmt_);
  }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  bool ok() const { return rc_ == SQLITE_OK && stmt_ != nullptr; }
  int rc() const { return rc_; }
  sqlite3_stmt* get() { return stmt_; }

 private:
  sqlite3_stmt* stmt_ = nullptr;
  int rc_ = SQLITE_ERROR;
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

//  ---- FileLocation ↔ JSON（`data` 列）----
//  已知字段写在顶层；`extra` 的键也写在顶层，读回时把不认识的键收回 `extra` ——
//  这样"读-改-写"不会丢失任何字段（前向兼容）。
//  ★ 这里**不能**收 `container` / `object_key`：它们在领域模型里属于
//    `FileLocation::extra`（物理引用的存放处，见 `LocationIssuer::kExtraContainer`），
//    只因为 `DumpLocation` 把 `extra` 的键**摊平**写到了顶层。若把它们算作"已知键"，
//    回读时就会既进不了 `extra`、也没有对应的领域字段接住 → 物理引用静默丢失，
//    随后 `ObjectRefFromLocation` 报"位置记录缺少物理引用"（P4-D09，500）。
const std::set<std::string>& KnownKeys() {
  static const std::set<std::string> keys = {
      "file_id", "driver", "zone", "file_source", "user_id", "signed_url",
      "created_at_epoch_seconds", "updated_at_epoch_seconds",
  };
  return keys;
}

std::string DumpLocation(const domain::FileLocation& location) {
  json::Value value = location.extra.is_object() ? location.extra : json::Value::object();
  value["file_id"] = location.file_id;
  value["driver"] = std::string(domain::StorageDriverName(location.driver));
  value["zone"] = std::string(domain::StorageZoneName(location.zone));
  value["file_source"] = location.file_source;
  value["user_id"] = location.user_id;
  value["signed_url"] = location.signed_url;
  value["created_at_epoch_seconds"] = location.created_at_epoch_seconds;
  value["updated_at_epoch_seconds"] = location.updated_at_epoch_seconds;
  return json::Dump(value);
}

fss::Result<domain::FileLocation> ParseLocation(const std::string& text) {
  FSS_TRY(value, json::ParseObject(text));
  domain::FileLocation location;
  json::Value extra = json::Value::object();
  for (auto it = value.begin(); it != value.end(); ++it) {
    if (KnownKeys().count(it.key()) == 0) extra[it.key()] = it.value();
  }
  location.extra = std::move(extra);

  location.file_id = value.value("file_id", std::string{});
  location.file_source = value.value("file_source", std::string{});
  location.user_id = value.value("user_id", std::string{});
  location.signed_url = value.value("signed_url", std::string{});
  location.created_at_epoch_seconds = value.value("created_at_epoch_seconds", std::int64_t{0});
  location.updated_at_epoch_seconds = value.value("updated_at_epoch_seconds", std::int64_t{0});
  location.driver = domain::ParseStorageDriver(value.value("driver", std::string{}))
                        .value_or(domain::StorageDriver::kPosix);
  location.zone = domain::ParseStorageZone(value.value("zone", std::string{}))
                      .value_or(domain::StorageZone::kStaging);
  return location;
}

std::string ExtraString(const domain::FileLocation& location, std::string_view key) {
  if (!location.extra.is_object() || !location.extra.contains(std::string(key))) return {};
  const auto& slot = location.extra[std::string(key)];
  return slot.is_string() ? slot.get<std::string>() : std::string{};
}

//  upsert 的**语句级**执行与错误映射（逐操作路径与批内路径共用一份）。
//  ★ 开了 `sqlite3_extended_result_codes` 之后 `rc` 是**扩展码**
//    （SQLITE_CONSTRAINT_UNIQUE = 2067），不是主码 SQLITE_CONSTRAINT(19)；
//    只比主码会漏判，症状是"唯一约束冲突被当成 500"（P3-D03）。
fss::Result<void> StepUpsert(sqlite3* db, sqlite3_stmt* stmt,
                             const domain::FileLocation& location) {
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) return Ok();
  if ((rc & 0xFF) == SQLITE_CONSTRAINT) {
    //  (partition, file_source) 唯一索引冲突（同 file_id 的 upsert 已被 ON CONFLICT 处理）
    return Err(fss::ErrorKind::kLocationAlreadyExists,
               "file_source 已被另一个 file_id 占用：" + location.file_source);
  }
  return SqliteError(db, "写入位置记录失败");
}

}  // namespace

fss::Result<std::unique_ptr<SqliteLocationRepository>> SqliteLocationRepository::Open(
    const std::string& path, SqliteLocationRepositoryOptions options) {
  if (options.max_write_concurrency <= 0) {
    return Err(fss::ErrorKind::kInvalidArgument, "max_write_concurrency 必须 > 0");
  }
  std::unique_ptr<SqliteLocationRepository> repository(new SqliteLocationRepository());
  repository->options_ = options;

  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  if (sqlite3_open_v2(path.c_str(), &repository->db_, flags, nullptr) != SQLITE_OK) {
    const auto error = SqliteError(repository->db_, "打开 SQLite 数据库失败");
    if (repository->db_ != nullptr) sqlite3_close(repository->db_);
    repository->db_ = nullptr;
    return error;
  }
  sqlite3_extended_result_codes(repository->db_, 1);

  if (options.wal) {
    char* message = nullptr;
    sqlite3_exec(repository->db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &message);
    if (message != nullptr) sqlite3_free(message);
  }
  //  ★ 阶段 10（切片 4）：`location.sqlite.synchronous` → `PRAGMA synchronous=<n>`。
  //    0 = OFF / 1 = NORMAL（默认）/ 2 = FULL；组合根在启动期拒绝白名单外的取值。
  //    ⚠ 不落盘，验证只能用同连接的 `AppliedPragma("synchronous")`。
  {
    const std::string synchronous_pragma =
        "PRAGMA synchronous=" + std::to_string(options.synchronous_level) + ";";
    char* pragma_error = nullptr;
    if (sqlite3_exec(repository->db_, synchronous_pragma.c_str(), nullptr, nullptr,
                     &pragma_error) != SQLITE_OK) {
      const std::string detail = pragma_error != nullptr ? pragma_error : "未知错误";
      if (pragma_error != nullptr) sqlite3_free(pragma_error);
      sqlite3_close(repository->db_);
      repository->db_ = nullptr;
      return Err(fss::ErrorKind::kInvalidArgument, "设置 PRAGMA synchronous 失败：" + detail);
    }
    if (pragma_error != nullptr) sqlite3_free(pragma_error);
  }
  sqlite3_busy_timeout(repository->db_, options.busy_timeout_millis);

  FSS_TRY(repository->EnsureSchema());
  //  ★ 本切片：组提交协调器（`group_commit=false` 时逐操作路径不碰它）。
  SqliteGroupCommitOptions group_options;
  group_options.group_commit = options.group_commit;
  group_options.group_commit_max_wait_ms = options.group_commit_max_wait_ms;
  group_options.group_commit_max_batch = options.group_commit_max_batch;
  group_options.batch_observer = options.batch_observer;
  group_options.batch_gate = options.batch_gate;
  group_options.commit_fault = options.commit_fault;
  group_options.metrics = options.metrics;
  group_options.metrics_repo = "location";
  repository->committer_ = std::make_unique<SqliteGroupCommitter>(
      repository->db_, repository->mutex_, group_options);
  return repository;
}

SqliteLocationRepository::~SqliteLocationRepository() {
  if (db_ != nullptr) sqlite3_close(db_);
}

fss::Result<void> SqliteLocationRepository::EnsureSchema() {
  for (const char* ddl : {kSchemaLocations, kSchemaSourceIndex, kSchemaPartitionCreatedIndex,
                          kSchemaZoneIndex}) {
    char* message = nullptr;
    if (sqlite3_exec(db_, ddl, nullptr, nullptr, &message) != SQLITE_OK) {
      const std::string detail = message != nullptr ? message : "";
      if (message != nullptr) sqlite3_free(message);
      return Err(fss::ErrorKind::kInternal, "创建 schema 失败：" + detail);
    }
  }
  return Ok();
}

fss::Result<domain::FileLocation> SqliteLocationRepository::LoadRow(
    sqlite3_stmt* statement) const {
  if (sqlite3_step(statement) != SQLITE_ROW) return NotFound("Record Not Found");
  return ParseLocation(ColumnText(statement, 0));
}

void SqliteLocationRepository::NotifyBatch(std::size_t ops_in_batch, bool committed) {
  if (options_.batch_observer != nullptr) {
    options_.batch_observer->OnBatchCommitted(ops_in_batch, committed);
  }
  if (options_.metrics != nullptr) {
    const fss::metrics::Labels labels{{"repo", "location"}};
    options_.metrics->Increment("fss_sqlite_group_commits_total", labels);
    options_.metrics->Increment("fss_sqlite_ops_total", labels,
                                static_cast<std::int64_t>(ops_in_batch));
  }
}

fss::Result<void> SqliteLocationRepository::Save(std::string_view partition,
                                                 const domain::FileLocation& location) {
  if (partition.empty()) return Invalid("partition 不能为空");
  if (location.file_id.empty()) return Invalid("file_id 不能为空");
  if (location.file_source.empty()) return Invalid("file_source 不能为空");
  if (options_.group_commit) {
    const std::string part(partition);
    return committer_->Submit<void>(
        [this, part, location](sqlite3* db) { return SaveInTransaction(db, part, location); });
  }
  std::lock_guard<std::mutex> guard(mutex_);
  return SaveLocked(partition, location);
}

fss::Result<void> SqliteLocationRepository::SaveLocked(
    std::string_view partition, const domain::FileLocation& location) {
  Statement statement(db_, kUpsert);
  if (!statement.ok()) return SqliteError(db_, "准备 upsert 失败");
  sqlite3_stmt* stmt = statement.get();

  BindText(stmt, 1, partition);
  BindText(stmt, 2, location.file_id);
  BindText(stmt, 3, location.file_source);
  BindText(stmt, 4, ExtraString(location, "container"));
  BindText(stmt, 5, ExtraString(location, "object_key"));
  BindText(stmt, 6, domain::StorageZoneName(location.zone));
  BindText(stmt, 7, domain::StorageDriverName(location.driver));
  BindText(stmt, 8, location.user_id);
  sqlite3_bind_int64(stmt, 9, location.created_at_epoch_seconds);
  sqlite3_bind_int64(stmt, 10, location.updated_at_epoch_seconds);
  BindText(stmt, 11, location.signed_url);
  const std::string data = DumpLocation(location);
  BindText(stmt, 12, data);

  const auto outcome = StepUpsert(db_, stmt, location);
  if (outcome.ok()) {
    //  单条自动提交语句 = 一次提交（与接线前一致）。
    NotifyBatch(1, true);
    return Ok();
  }
  NotifyBatch(1, false);
  return outcome.error();
}

fss::Result<void> SqliteLocationRepository::SaveInTransaction(
    sqlite3* db, std::string_view partition, const domain::FileLocation& location) {
  Statement statement(db, kUpsert);
  if (!statement.ok()) return SqliteError(db, "准备 upsert 失败");
  sqlite3_stmt* stmt = statement.get();

  BindText(stmt, 1, partition);
  BindText(stmt, 2, location.file_id);
  BindText(stmt, 3, location.file_source);
  BindText(stmt, 4, ExtraString(location, "container"));
  BindText(stmt, 5, ExtraString(location, "object_key"));
  BindText(stmt, 6, domain::StorageZoneName(location.zone));
  BindText(stmt, 7, domain::StorageDriverName(location.driver));
  BindText(stmt, 8, location.user_id);
  sqlite3_bind_int64(stmt, 9, location.created_at_epoch_seconds);
  sqlite3_bind_int64(stmt, 10, location.updated_at_epoch_seconds);
  BindText(stmt, 11, location.signed_url);
  const std::string data = DumpLocation(location);
  BindText(stmt, 12, data);

  return StepUpsert(db, stmt, location);
}

fss::Result<domain::FileLocation> SqliteLocationRepository::Find(std::string_view partition,
                                                                 std::string_view file_id) {
  std::lock_guard<std::mutex> guard(mutex_);
  return FindInternal(partition, file_id);
}

fss::Result<domain::FileLocation> SqliteLocationRepository::FindInternal(
    std::string_view partition, std::string_view file_id) {
  Statement statement(db_, kSelectByFileId);
  if (!statement.ok()) return SqliteError(db_, "准备查询失败");
  BindText(statement.get(), 1, partition);
  BindText(statement.get(), 2, file_id);
  const auto found = LoadRow(statement.get());
  if (!found.ok()) {
    return NotFound("Not found location for fileID : " + std::string(file_id));
  }
  return found;
}

fss::Result<domain::FileLocation> SqliteLocationRepository::FindByFileSource(
    std::string_view partition, std::string_view file_source) {
  std::lock_guard<std::mutex> guard(mutex_);
  Statement statement(db_, kSelectByFileSource);
  if (!statement.ok()) return SqliteError(db_, "准备查询失败");
  BindText(statement.get(), 1, partition);
  BindText(statement.get(), 2, file_source);
  const auto found = LoadRow(statement.get());
  if (!found.ok()) return NotFound("位置记录不存在（按 file_source）：" + std::string(file_source));
  return found;
}

fss::Result<void> SqliteLocationRepository::UpdateSignedUrl(
    std::string_view partition, std::string_view file_id, std::string_view signed_url,
    std::int64_t updated_at_epoch_seconds) {
  if (options_.group_commit) {
    const std::string part(partition);
    const std::string id(file_id);
    const std::string url(signed_url);
    return committer_->Submit<void>([this, part, id, url, updated_at_epoch_seconds](
                                        sqlite3* db) {
      return UpdateSignedUrlInTransaction(db, part, id, url, updated_at_epoch_seconds);
    });
  }
  std::lock_guard<std::mutex> guard(mutex_);
  return UpdateSignedUrlLocked(partition, file_id, signed_url, updated_at_epoch_seconds);
}

fss::Result<void> SqliteLocationRepository::UpdateSignedUrlLocked(
    std::string_view partition, std::string_view file_id, std::string_view signed_url,
    std::int64_t updated_at_epoch_seconds) {
  //  读-改-写 `data`：保留未知字段（否则一次 signed URL 更新就会丢字段）
  //  ★ 必须用 FindInternal：本方法已持锁，调用加锁的 Find 会自锁（非递归 mutex）
  const auto existing = FindInternal(partition, file_id);
  if (!existing.ok()) return existing.error();

  domain::FileLocation location = existing.value();
  location.signed_url = std::string(signed_url);
  location.updated_at_epoch_seconds = updated_at_epoch_seconds;
  const std::string data = DumpLocation(location);

  Statement statement(db_, kUpdateSignedUrl);
  if (!statement.ok()) return SqliteError(db_, "准备更新失败");
  sqlite3_stmt* stmt = statement.get();
  BindText(stmt, 1, location.signed_url);
  sqlite3_bind_int64(stmt, 2, location.updated_at_epoch_seconds);
  BindText(stmt, 3, data);
  BindText(stmt, 4, partition);
  BindText(stmt, 5, file_id);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    NotifyBatch(1, false);
    return SqliteError(db_, "更新 signed_url 失败");
  }
  if (sqlite3_changes(db_) == 0) {
    NotifyBatch(1, false);
    return NotFound("位置记录不存在：" + std::string(file_id));
  }
  NotifyBatch(1, true);
  return Ok();
}

fss::Result<void> SqliteLocationRepository::UpdateSignedUrlInTransaction(
    sqlite3* db, std::string_view partition, std::string_view file_id,
    std::string_view signed_url, std::int64_t updated_at_epoch_seconds) {
  const auto existing = FindInternal(partition, file_id);
  if (!existing.ok()) return existing.error();

  domain::FileLocation location = existing.value();
  location.signed_url = std::string(signed_url);
  location.updated_at_epoch_seconds = updated_at_epoch_seconds;
  const std::string data = DumpLocation(location);

  Statement statement(db, kUpdateSignedUrl);
  if (!statement.ok()) return SqliteError(db, "准备更新失败");
  sqlite3_stmt* stmt = statement.get();
  BindText(stmt, 1, location.signed_url);
  sqlite3_bind_int64(stmt, 2, location.updated_at_epoch_seconds);
  BindText(stmt, 3, data);
  BindText(stmt, 4, partition);
  BindText(stmt, 5, file_id);

  if (sqlite3_step(stmt) != SQLITE_DONE) return SqliteError(db, "更新 signed_url 失败");
  if (sqlite3_changes(db) == 0) return NotFound("位置记录不存在：" + std::string(file_id));
  return Ok();
}

fss::Result<void> SqliteLocationRepository::Delete(std::string_view partition,
                                                   std::string_view file_id) {
  if (options_.group_commit) {
    const std::string part(partition);
    const std::string id(file_id);
    return committer_->Submit<void>(
        [this, part, id](sqlite3* db) { return DeleteInTransaction(db, part, id); });
  }
  std::lock_guard<std::mutex> guard(mutex_);
  return DeleteLocked(partition, file_id);
}

fss::Result<void> SqliteLocationRepository::DeleteLocked(std::string_view partition,
                                                         std::string_view file_id) {
  Statement statement(db_, kDelete);
  if (!statement.ok()) return SqliteError(db_, "准备删除失败");
  BindText(statement.get(), 1, partition);
  BindText(statement.get(), 2, file_id);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    NotifyBatch(1, false);
    return SqliteError(db_, "删除失败");
  }
  if (sqlite3_changes(db_) == 0) {
    NotifyBatch(1, false);
    return NotFound("位置记录不存在：" + std::string(file_id));
  }
  NotifyBatch(1, true);
  return Ok();
}

fss::Result<void> SqliteLocationRepository::DeleteInTransaction(sqlite3* db,
                                                                 std::string_view partition,
                                                                 std::string_view file_id) {
  Statement statement(db, kDelete);
  if (!statement.ok()) return SqliteError(db, "准备删除失败");
  BindText(statement.get(), 1, partition);
  BindText(statement.get(), 2, file_id);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) return SqliteError(db, "删除失败");
  if (sqlite3_changes(db) == 0) return NotFound("位置记录不存在：" + std::string(file_id));
  return Ok();
}

fss::Result<domain::LocationPage> SqliteLocationRepository::List(
    std::string_view partition, const domain::LocationQuery& query) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (query.limit <= 0) return Invalid("limit 必须 > 0");
  if (query.offset < 0) return Invalid("offset 必须 >= 0");

  //  动态拼接过滤条件（基句永远带 partition_id；追加片段不含 SQL 关键字）
  std::string where = kSelectListBase;
  std::string count_where = kCountListBase;
  const bool by_user = !query.user_id.empty();
  const bool after = query.created_after_epoch_seconds >= 0;
  const bool before = query.created_before_epoch_seconds >= 0;
  if (by_user) {
    where += " AND created_by = ?";
    count_where += " AND created_by = ?";
  }
  if (after) {
    where += " AND created_at >= ?";
    count_where += " AND created_at >= ?";
  }
  if (before) {
    where += " AND created_at <= ?";
    count_where += " AND created_at <= ?";
  }
  where += " ORDER BY created_at ASC, file_id ASC LIMIT ? OFFSET ?";

  const auto bind_filters = [&](sqlite3_stmt* stmt) {
    int index = 2;
    if (by_user) BindText(stmt, index++, query.user_id);
    if (after) sqlite3_bind_int64(stmt, index++, query.created_after_epoch_seconds);
    if (before) sqlite3_bind_int64(stmt, index++, query.created_before_epoch_seconds);
    return index;
  };

  domain::LocationPage page;

  //  total = 过滤后、分页前的总数
  {
    Statement count_stmt(db_, count_where.c_str());
    if (!count_stmt.ok()) return SqliteError(db_, "准备计数失败");
    BindText(count_stmt.get(), 1, partition);
    bind_filters(count_stmt.get());
    if (sqlite3_step(count_stmt.get()) != SQLITE_ROW) return SqliteError(db_, "计数失败");
    page.total = sqlite3_column_int64(count_stmt.get(), 0);
  }

  Statement statement(db_, where.c_str());
  if (!statement.ok()) return SqliteError(db_, "准备列表查询失败");
  sqlite3_stmt* stmt = statement.get();
  BindText(stmt, 1, partition);
  const int next = bind_filters(stmt);
  sqlite3_bind_int(stmt, next, query.limit);
  sqlite3_bind_int(stmt, next + 1, query.offset);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    auto location = ParseLocation(ColumnText(stmt, 0));
    if (!location.ok()) return location.error();
    page.records.push_back(std::move(location.value()));
  }
  return page;
}

//  ★ 阶段 10（切片 4）：诊断访问器。为什么需要它（而不是"另开连接读回"）：
//    `PRAGMA synchronous` 是**连接级**设置，**不写进库文件**；另开一个 sqlite3 连接读回
//    只会得到那个新连接自己的默认值（FULL = 2），与仓储是否真的下发了 OFF/NORMAL/FULL
//    无关。因此必须在**同一个连接**上读回。PRAGMA 名不能参数化（不是绑定值），只放行
//    白名单里的名字以避免 SQL 注入。
fss::Result<std::string> SqliteLocationRepository::AppliedPragma(std::string_view name) const {
  if (name != "synchronous") {
    return Invalid("AppliedPragma 只支持 synchronous（收到 '" + std::string(name) + "'）");
  }
  std::lock_guard<std::mutex> guard(mutex_);
  Statement stmt(db_, "PRAGMA synchronous");
  if (!stmt.ok()) return SqliteError(db_, "准备 PRAGMA synchronous 失败");
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return SqliteError(db_, "读取 PRAGMA synchronous 失败");
  return ColumnText(stmt.get(), 0);
}

}  // namespace fss::infra
