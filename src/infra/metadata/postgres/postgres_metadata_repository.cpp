// PostgresMetadataRepository 实现。schema 映射裁决、与 SQLite 的语义对齐点见头文件。
#include "infra/metadata/postgres/postgres_metadata_repository.h"

#include "common/json/json.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace fss::infra {

namespace {

//  ---- SQL（全部写成 raw string，便于 C3.9 护栏机械检索；每条 DML 都带 partition_id）----
//  ★ 带值的 SQL 一律用 `PQexecParams` 占位符（$1..$n），**不拼字符串**（见 pg_connection.h）。
//  ★ 只用 PostgreSQL ≤12 语法（目标环境是 PG 12.6）：无扩展、无 INCLUDE、无 MERGE、
//    无 `NULLS NOT DISTINCT`、无 PG-14 专属 GUC。`to_timestamp(bigint)` 自 8.1 起可用。
constexpr const char* kSelectLatestById = R"sql(
SELECT version, data FROM file_metadata_records
 WHERE partition_id = $1 AND id = $2 AND is_latest AND state = 'ready')sql";

constexpr const char* kSelectLatestBySource = R"sql(
SELECT version, data FROM file_metadata_records
 WHERE partition_id = $1 AND file_source = $2 AND is_latest AND state = 'ready')sql";

//  ★ C1：领取路径按幂等键取**活动**行（claiming 或 ready），连带回传状态。
constexpr const char* kSelectClaimBySource = R"sql(
SELECT version, data, state FROM file_metadata_records
 WHERE partition_id = $1 AND file_source = $2 AND is_latest AND state <> 'deleted')sql";

//  ★ C1：MarkReady 0 行更新时的**歧义消解**（区分"版本不存在"与"幂等键不符"）。
constexpr const char* kSelectVersionState = R"sql(
SELECT state, file_source FROM file_metadata_records
 WHERE partition_id = $1 AND id = $2 AND version = $3::int)sql";

//  ★ ADR-009 §4.2 的**原子领取**（M2 修复），本切片把插入状态改成 `'claiming'`
//    （复制成功后再由 `kMarkReady` 翻到 `'ready'`）。一条语句完成"插入本版本 + 冲突则放弃"。
//    `ON CONFLICT (partition_id, file_source) WHERE state <> 'deleted' AND is_latest`
//    的谓词必须与 `db/migrations/001_init.sql` 的 `ux_mr_source` **逐字一致**，否则
//    PostgreSQL 无法推断出该部分唯一索引（实测两引擎报 42P10）；
//    返回 0 行 = 另一个实例已经赢了竞态 → 调用方按 file_source 回读并返回那条。
constexpr const char* kClaimInsert = R"sql(
INSERT INTO file_metadata_records
  (partition_id, id, version, kind, state, is_latest, created_at, created_by,
   acl_viewers, acl_owners, legal_tags, file_source, data)
VALUES ($1, $2, 1, $3, 'claiming', TRUE, to_timestamp($4::bigint), '',
        $5::jsonb, $6::jsonb, $7::jsonb, $8, $9::jsonb)
ON CONFLICT (partition_id, file_source) WHERE state <> 'deleted' AND is_latest DO NOTHING
RETURNING id, version)sql";

//  ★ C1：claiming → ready，并一次性把**最终**记录（含复制后才算出的 checksum）与镜像列同步。
//    `file_source = $4` 是幂等键守卫：不一致 → 0 行 → 由 `kSelectVersionState` 判为
//    kInvalidArgument，且行**保持 claiming**（调用方仍可 ReleaseClaim）。
constexpr const char* kMarkReady = R"sql(
UPDATE file_metadata_records
   SET state = 'ready', data = $5::jsonb, kind = $6,
       acl_viewers = $7::jsonb, acl_owners = $8::jsonb, legal_tags = $9::jsonb
 WHERE partition_id = $1 AND id = $2 AND version = $3::int
   AND state = 'claiming' AND file_source = $4
RETURNING version, data)sql";

//  ★ C1：放弃领取 —— 只删 claiming 行（ready 行删不到 → 0 行 → kNotFound）。
constexpr const char* kReleaseClaim = R"sql(
DELETE FROM file_metadata_records
 WHERE partition_id = $1 AND id = $2 AND version = $3::int AND state = 'claiming')sql";

//  ★ C2（ADR-009 §4.2/§4.3）：回收崩溃领取者留下的 claiming 行。
//    一条语句：CTE 选出候选（`partition_id` + `state='claiming'` + `created_at` 够旧 +
//    `file_source = ANY(已原子领取的过期租约键)`），`FOR UPDATE SKIP LOCKED` 保证并发 GC
//    互不阻塞且一行只被一个 GC 回收，`DELETE ... RETURNING` 返回实际回收行数。
//    ★ 由调用方给出 file_source 集合：领取是否"已死"只有租约知道（见 ports.h 的说明）。
//    ★ 全部是 PG ≤12 语法（`= ANY(array)`、CTE、`FOR UPDATE SKIP LOCKED` 自 9.5 起可用）。
constexpr const char* kReclaimStaleClaiming = R"sql(
WITH stale AS (
  SELECT id, version
    FROM file_metadata_records
   WHERE partition_id = $1 AND state = 'claiming'
     AND created_at <= to_timestamp($2::bigint)
     AND file_source = ANY($3::text[])
   ORDER BY created_at ASC, id ASC
   LIMIT $4::int
   FOR UPDATE SKIP LOCKED
)
DELETE FROM file_metadata_records target
 USING stale
 WHERE target.partition_id = $1
   AND target.id = stale.id
   AND target.version = stale.version
RETURNING target.id)sql";

//  ★ 版本链（R6）：先把旧的 is_latest 清零，再插入新版本 —— 两条语句必须在**同一事务**里
//    （`Update` 用 BEGIN/COMMIT/ROLLBACK 控制），否则中间态会短暂出现"没有 latest"或
//    "两个 latest"（后者会被 ux_mr_latest 拒绝）。
constexpr const char* kClearLatest = R"sql(
UPDATE file_metadata_records SET is_latest = FALSE
 WHERE partition_id = $1 AND id = $2 AND is_latest AND state = 'ready')sql";

constexpr const char* kInsertVersion = R"sql(
INSERT INTO file_metadata_records
  (partition_id, id, version, kind, state, is_latest, created_at, created_by,
   acl_viewers, acl_owners, legal_tags, file_source, data)
VALUES ($1, $2, $3::int, $4, 'ready', TRUE, to_timestamp($5::bigint), '',
        $6::jsonb, $7::jsonb, $8::jsonb, $9, $10::jsonb))sql";

//  ★ 契约：删除**全部版本**（硬删除）。含任何外部写入的 tombstone 行 —— 否则后续用
//    同一个 id 重新 `Create` 会撞上 `(partition_id, id, version)` 主键。
constexpr const char* kDeleteAllVersions = R"sql(
DELETE FROM file_metadata_records WHERE partition_id = $1 AND id = $2)sql";

constexpr const char* kCountVersions = R"sql(
SELECT COUNT(*) FROM file_metadata_records WHERE partition_id = $1 AND id = $2)sql";

//  `List` 的基句：只取 latest 且**已 ready** 的行；过滤/排序/分页在下面按需拼接（占位符编号连续）。
constexpr const char* kSelectListBase = R"sql(
SELECT version, data FROM file_metadata_records
 WHERE partition_id = $1 AND is_latest AND state = 'ready')sql";

constexpr const char* kCountListBase = R"sql(
SELECT COUNT(*) FROM file_metadata_records
 WHERE partition_id = $1 AND is_latest AND state = 'ready')sql";

//  ---- 通用小工具（消息与 SqliteMetadataRepository 保持一致）----
fss::Error Invalid(const std::string& message) {
  return fss::Err(fss::ErrorKind::kInvalidArgument, message);
}
fss::Error NotFound(const std::string& message) {
  return fss::Err(fss::ErrorKind::kNotFound, message);
}

bool HasPrefix(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::int64_t ParseInt64(const std::string& text) {
  if (text.empty()) return 0;
  return static_cast<std::int64_t>(std::strtoll(text.c_str(), nullptr, 10));
}

//  ★ C1：`state` 列 → 领域枚举（与 SqliteMetadataRepository 的映射逐字一致）。
domain::MetadataState ParseMetadataState(std::string_view text) {
  if (text == "claiming") return domain::MetadataState::kClaiming;
  if (text == "deleted") return domain::MetadataState::kDeleted;
  return domain::MetadataState::kReady;
}

std::string DumpStringArray(const std::vector<std::string>& items) {
  json::Value array = json::Value::array();
  for (const auto& item : items) array.push_back(item);
  return json::Dump(array);
}

//  ★ C2：`text[]` 参数的**数组字面量**（`PQexecParams` 的文本参数按 `$3::text[]` 解析）。
//    每个元素用双引号包裹并转义 `\` 与 `"`（PostgreSQL 数组输入语法）；空数组不会到这里
//    （调用方先短路），但仍写成合法的 `{}`。不用 `string_to_array` 是因为分隔符本身需要
//    转义，数组字面量是唯一无歧义的表达。
std::string PgTextArrayLiteral(const std::vector<std::string>& items) {
  std::string out = "{";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) out += ',';
    out += '"';
    for (const char c : items[i]) {
      if (c == '\\' || c == '"') out += '\\';
      out += c;
    }
    out += '"';
  }
  out += '}';
  return out;
}

//  校验参数（与内存 / SQLite 实现逐条对齐；契约里有对应断言）
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

//  行 → 记录（列顺序固定为 `version, data`）。
//  ★ 版本以**列**为准（JSON 里的 version 只是同一份数据的拷贝），与 SQLite 实现一致；
//    `previous_version` 在 PG schema 里没有列，领域侧的 `ancestry` 直接从 `data` 往返。
fss::Result<domain::FileMetadataRecord> RowToRecord(const PgResult& result, int row) {
  FSS_TRY(value, json::ParseObject(result.Value(row, 1)));
  FSS_TRY(record, domain::ParseFileMetadataRecord(value));
  record.version = ParseInt64(result.Value(row, 0));
  return record;
}

}  // namespace

// =============================================================================
//  Open / 析构
// =============================================================================
fss::Result<std::unique_ptr<PostgresMetadataRepository>> PostgresMetadataRepository::Open(
    PostgresMetadataRepositoryOptions options, const fss::IClock& clock) {
  //  ★ 建池即建连：DSN 不可达/权限不足在这里就返回 kUnavailable，绝不回退到别的存储。
  FSS_TRY(pool, PgPool::Create(std::move(options.pg)));
  std::unique_ptr<PostgresMetadataRepository> repository(new PostgresMetadataRepository());
  repository->clock_ = &clock;
  repository->pool_ = std::move(pool);
  return Ok(std::move(repository));
}

PostgresMetadataRepository::~PostgresMetadataRepository() = default;

// =============================================================================
//  查询 helper（调用方已借出连接）
// =============================================================================
fss::Result<domain::FileMetadataRecord> PostgresMetadataRepository::FindLatestById(
    PgConnection& connection, std::string_view partition, std::string_view record_id) {
  const auto result =
      connection.ExecParams(kSelectLatestById, {std::string(partition), std::string(record_id)});
  if (!result.ok()) return Annotate(result.error(), "查询元数据记录失败");
  if (result.value().RowCount() == 0) return NotFound("Record Not Found");
  return RowToRecord(result.value(), 0);
}

fss::Result<domain::FileMetadataRecord> PostgresMetadataRepository::FindLatestBySource(
    PgConnection& connection, std::string_view partition, std::string_view file_source) {
  const auto result = connection.ExecParams(kSelectLatestBySource,
                                            {std::string(partition), std::string(file_source)});
  if (!result.ok()) return Annotate(result.error(), "查询元数据记录失败");
  if (result.value().RowCount() == 0) return NotFound("Record Not Found");
  return RowToRecord(result.value(), 0);
}

fss::Result<domain::FileMetadataRecord> PostgresMetadataRepository::FindClaimBySource(
    PgConnection& connection, std::string_view partition, std::string_view file_source,
    domain::MetadataState* state) {
  const auto result = connection.ExecParams(kSelectClaimBySource,
                                            {std::string(partition), std::string(file_source)});
  if (!result.ok()) return Annotate(result.error(), "查询元数据记录失败");
  if (result.value().RowCount() == 0) return NotFound("Record Not Found");
  //  列顺序：version, data, state（与 RowToRecord 的 2 列布局不同，故单独解析）
  FSS_TRY(value, json::ParseObject(result.value().Value(0, 1)));
  FSS_TRY(record, domain::ParseFileMetadataRecord(value));
  record.version = ParseInt64(result.value().Value(0, 0));
  if (state != nullptr) *state = ParseMetadataState(result.value().Value(0, 2));
  return record;
}

// =============================================================================
//  Create / ClaimForWrite / MarkReady / ReleaseClaim（ADR-009 §4.2）
// =============================================================================
fss::Result<domain::FileMetadataRecord> PostgresMetadataRepository::Create(
    std::string_view partition, const domain::FileMetadataRecord& record) {
  FSS_TRY(ValidateRecord(partition, record));
  //  ★ C1：用新原语实现（claim → mark ready）。可观测语义与接线前逐字一致。
  FSS_TRY(claim, ClaimForWrite(partition, record));
  if (!claim.claimed) {
    //  既有记录（ready，或另一个实例正在 claiming）→ 返回既有 id（旧 Create 的幂等语义）。
    return claim.record;
  }
  return MarkReady(partition, claim.record.id, claim.record.version, claim.record);
}

fss::Result<domain::MetadataClaim> PostgresMetadataRepository::ClaimForWrite(
    std::string_view partition, const domain::FileMetadataRecord& record) {
  FSS_TRY(ValidateRecord(partition, record));
  const std::string file_source =
      record.data.dataset_properties.file_source_info.file_source;
  FSS_TRY(handle, pool_->Borrow());

  //  ★ 幂等键先查：同 (partition, file_source) 已有活动记录（ready/claiming）→ 返回它。
  //    claiming 也返回 claimed=false：**不复制**正是本切片的目的。
  {
    domain::MetadataState state = domain::MetadataState::kReady;
    if (auto existing = FindClaimBySource(*handle, partition, file_source, &state);
        existing.ok()) {
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

  //  ★★ 单语句原子领取（R1 注入点①：拆成"先查后插"两条语句 → 并发下会出现多个 winner）。
  const auto inserted = handle->ExecParams(
      kClaimInsert,
      {std::string(partition), stored.id, stored.kind, std::to_string(created_at),
       DumpStringArray(stored.acl.viewers), DumpStringArray(stored.acl.owners),
       DumpStringArray(stored.legal.legaltags), file_source,
       json::Dump(domain::ToJson(stored))});
  if (!inserted.ok()) {
    //  主键/ux_mr_latest 冲突（同 id 但不同 file_source）被 MapPgError 映射成
    //  kLocationAlreadyExists（23505）—— 按幂等语义恢复，不把 UNIQUE 冲突当 500。
    if (inserted.error().kind() == fss::ErrorKind::kLocationAlreadyExists) {
      domain::MetadataState state = domain::MetadataState::kReady;
      if (auto existing = FindClaimBySource(*handle, partition, file_source, &state);
          existing.ok()) {
        domain::MetadataClaim out;
        out.claimed = false;
        out.record = existing.value();
        out.state = state;
        return out;
      }
      return Invalid("同 id 已存在且 file_source 不同：" + record.id);
    }
    return Annotate(inserted.error(), "插入元数据失败");
  }
  //  ★ 原子领取失败（另一个实例赢了竞态）：0 行返回 → 按幂等键回读并返回那条。
  if (inserted.value().RowCount() == 0) {
    domain::MetadataState state = domain::MetadataState::kReady;
    if (auto existing = FindClaimBySource(*handle, partition, file_source, &state);
        existing.ok()) {
      domain::MetadataClaim out;
      out.claimed = false;
      out.record = existing.value();
      out.state = state;
      return out;
    }
    return fss::Err(fss::ErrorKind::kInternal,
                    "插入元数据失败：claim 未插入且按 file_source 回读为空");
  }
  domain::MetadataClaim out;
  out.claimed = true;
  out.record = stored;
  out.state = domain::MetadataState::kClaiming;
  return out;
}

fss::Result<domain::FileMetadataRecord> PostgresMetadataRepository::MarkReady(
    std::string_view partition, std::string_view record_id, std::int64_t version,
    const domain::FileMetadataRecord& record) {
  FSS_TRY(ValidateRecord(partition, record));
  //  ★ `record_id` / `version`（列语义）是权威的：不做"record.version 必须等于 version"的
  //    拒绝（否则"版本不存在 → kNotFound"的契约判据会变成 kInvalidArgument）。
  const std::string file_source =
      record.data.dataset_properties.file_source_info.file_source;
  domain::FileMetadataRecord stored = record;
  stored.id = std::string(record_id);
  stored.version = version;
  FSS_TRY(handle, pool_->Borrow());

  const auto updated = handle->ExecParams(
      kMarkReady,
      {std::string(partition), std::string(record_id), std::to_string(version), file_source,
       json::Dump(domain::ToJson(stored)), stored.kind, DumpStringArray(stored.acl.viewers),
       DumpStringArray(stored.acl.owners), DumpStringArray(stored.legal.legaltags)});
  if (!updated.ok()) return Annotate(updated.error(), "mark-ready 失败");
  if (updated.value().RowCount() > 0) return RowToRecord(updated.value(), 0);

  //  0 行更新：区分"版本不存在/不是 claiming"（kNotFound）与"幂等键不符"（kInvalidArgument，
  //  且行**保持 claiming**，调用方仍可 ReleaseClaim）。
  const auto probe = handle->ExecParams(
      kSelectVersionState,
      {std::string(partition), std::string(record_id), std::to_string(version)});
  if (!probe.ok()) return Annotate(probe.error(), "查询元数据版本失败");
  if (probe.value().RowCount() == 0) return NotFound("Record Not Found");
  if (probe.value().Value(0, 0) != "claiming") return NotFound("Record Not Found");
  if (probe.value().Value(0, 1) != file_source) {
    return Invalid("MarkReady 不得改写 (partition, file_source) 幂等键");
  }
  return NotFound("Record Not Found");
}

fss::Result<void> PostgresMetadataRepository::ReleaseClaim(std::string_view partition,
                                                           std::string_view record_id,
                                                           std::int64_t version) {
  if (partition.empty()) return Invalid("partition 不能为空");
  FSS_TRY(handle, pool_->Borrow());
  const auto result = handle->ExecParams(
      kReleaseClaim, {std::string(partition), std::string(record_id), std::to_string(version)});
  if (!result.ok()) return Annotate(result.error(), "放弃领取失败");
  if (result.value().AffectedRows() == 0) {
    //  只删 claiming 行 → 版本不存在 / 已是 ready 都归为 kNotFound。
    return NotFound("Record Not Found（该版本不是 claiming 状态）");
  }
  return Ok();
}

//  ★ C2：回收崩溃领取者留下的 claiming 行（见 kReclaimStaleClaiming 的注释）。
fss::Result<std::int64_t> PostgresMetadataRepository::ReclaimStaleClaiming(
    std::string_view partition, std::int64_t older_than_epoch_seconds, int limit,
    const std::vector<std::string>& live_expired_sources) {
  if (partition.empty()) return Invalid("partition 不能为空");
  //  ★ 空集合 = "没有任何被本 GC 原子领取的过期租约" → 一条都不回收（绝不凭年龄误删活 claim）。
  if (limit <= 0 || live_expired_sources.empty()) return std::int64_t{0};
  FSS_TRY(handle, pool_->Borrow());
  const auto result = handle->ExecParams(
      kReclaimStaleClaiming,
      {std::string(partition), std::to_string(older_than_epoch_seconds),
       PgTextArrayLiteral(live_expired_sources), std::to_string(limit)});
  if (!result.ok()) return Annotate(result.error(), "回收 claiming 记录失败");
  //  `DELETE … RETURNING` 的结果是 TUPLES_OK：返回行数 = 实际回收的行数。
  return static_cast<std::int64_t>(result.value().RowCount());
}

// =============================================================================
//  读取
// =============================================================================
fss::Result<domain::FileMetadataRecord> PostgresMetadataRepository::GetById(
    std::string_view partition, std::string_view record_id) {
  if (partition.empty()) return Invalid("partition 不能为空");
  FSS_TRY(handle, pool_->Borrow());
  return FindLatestById(*handle, partition, record_id);
}

fss::Result<domain::FileMetadataRecord> PostgresMetadataRepository::GetLatestByFileSource(
    std::string_view partition, std::string_view file_source) {
  if (partition.empty()) return Invalid("partition 不能为空");
  FSS_TRY(handle, pool_->Borrow());
  return FindLatestBySource(*handle, partition, file_source);
}

// =============================================================================
//  Update（版本链；两条语句一个事务）
// =============================================================================
fss::Result<domain::FileMetadataRecord> PostgresMetadataRepository::Update(
    std::string_view partition, const domain::FileMetadataRecord& record) {
  FSS_TRY(ValidateRecord(partition, record));
  const std::string file_source =
      record.data.dataset_properties.file_source_info.file_source;

  //  ★ 整个 Update 用**同一个借出的连接**做 BEGIN…COMMIT（连接池句柄保证同一会话）。
  FSS_TRY(handle, pool_->Borrow());
  FSS_TRY(existing, FindLatestById(*handle, partition, record.id));

  //  ★ 幂等键必须稳定：允许改内容，不允许改 (partition, file_source)
  const auto& existing_source =
      existing.data.dataset_properties.file_source_info.file_source;
  if (existing_source != file_source) {
    return Invalid("Update 不得改写 (partition, file_source) 幂等键");
  }

  domain::FileMetadataRecord stored = record;
  stored.version = existing.version + 1;
  const std::int64_t created_at = clock_->NowEpochSeconds();

  const auto begun = handle->ExecSimple("BEGIN");
  if (!begun.ok()) return Annotate(begun.error(), "开启事务失败");
  //  统一失败出口：任何一步失败都必须回滚，绝不把"半个操作"（旧 latest 已被清掉）留在库里。
  const auto fail = [&](const fss::Error& error) {
    (void)handle->ExecSimple("ROLLBACK");
    return error;
  };

  const auto cleared =
      handle->ExecParams(kClearLatest, {std::string(partition), stored.id});
  if (!cleared.ok()) return fail(Annotate(cleared.error(), "清理旧 latest 标记失败"));
  if (cleared.value().AffectedRows() == 0) {
    //  FindLatestById 刚刚命中过，这里却没清到任何行 = 不变量被破坏（R9：显式断言）
    return fail(fss::Err(fss::ErrorKind::kInternal,
                         "清理旧 latest 标记失败：未命中任何行"));
  }

  const auto inserted = handle->ExecParams(
      kInsertVersion,
      {std::string(partition), stored.id, std::to_string(stored.version), stored.kind,
       std::to_string(created_at), DumpStringArray(stored.acl.viewers),
       DumpStringArray(stored.acl.owners), DumpStringArray(stored.legal.legaltags),
       file_source, json::Dump(domain::ToJson(stored))});
  if (!inserted.ok()) return fail(Annotate(inserted.error(), "写入新版本失败"));

  const auto committed = handle->ExecSimple("COMMIT");
  if (!committed.ok()) return fail(Annotate(committed.error(), "提交事务失败"));
  return stored;
}

// =============================================================================
//  Delete（硬删除全部版本）
// =============================================================================
fss::Result<void> PostgresMetadataRepository::Delete(std::string_view partition,
                                                     std::string_view record_id) {
  if (partition.empty()) return Invalid("partition 不能为空");
  FSS_TRY(handle, pool_->Borrow());
  const auto result =
      handle->ExecParams(kDeleteAllVersions, {std::string(partition), std::string(record_id)});
  if (!result.ok()) return Annotate(result.error(), "删除元数据失败");
  if (result.value().AffectedRows() == 0) return NotFound("Record Not Found");
  return Ok();
}

// =============================================================================
//  List（过滤 + 稳定全序 + 分页 + total）
// =============================================================================
fss::Result<domain::MetadataPage> PostgresMetadataRepository::List(
    std::string_view partition, const domain::MetadataQuery& query) {
  if (partition.empty()) return Invalid("partition 不能为空");
  if (query.limit <= 0) return Invalid("limit 必须 > 0");
  if (query.offset < 0) return Invalid("offset 必须 >= 0");

  //  动态拼接过滤条件（基句永远带 partition_id；占位符编号连续）。
  std::string where = kSelectListBase;
  std::string count_where = kCountListBase;
  std::vector<PgConnection::Param> params{std::string(partition)};
  std::vector<PgConnection::Param> count_params{std::string(partition)};
  int index = 2;
  const auto append_text = [&](const std::string& predicate, const std::string& value) {
    const std::string placeholder =
        " AND " + predicate + " $" + std::to_string(index);
    where += placeholder;
    count_where += placeholder;
    params.emplace_back(value);
    count_params.emplace_back(value);
    ++index;
  };
  const auto append_epoch = [&](const std::string& op, std::int64_t value) {
    //  ★ 过滤用 `floor(extract(epoch from created_at))::bigint`：截断（不是四舍五入），
    //    与 SQLite 的整数 epoch 列语义一致；含端点（`>=` / `<=`）。
    const std::string placeholder =
        " AND floor(extract(epoch from created_at))::bigint " + op + " $" +
        std::to_string(index) + "::bigint";
    where += placeholder;
    count_where += placeholder;
    params.emplace_back(std::to_string(value));
    count_params.emplace_back(std::to_string(value));
    ++index;
  };

  if (query.kind.has_value()) append_text("kind =", *query.kind);
  if (query.name_prefix.has_value()) {
    //  ★ `name` 在 PG schema 里**没有列**：过滤 JSONB 的 `data -> 'data' ->> 'Name'`。
    //    SQLite 实现用的是 C++ 的**字面前缀**比较（`rfind(prefix,0)==0`），所以这里
    //    用 `left(name, length($n)) = $n` 而不是 `LIKE $n || '%'` —— `%` / `_` 在 LIKE
    //    里是通配符，会把 SQLite 当普通字符的名字前缀变成通配（两边语义不一致）。
    //    `left/length` 按字符计数，对合法 UTF-8 与逐字节前缀比较等价；
    //    缺失的 `Name` 是 NULL → 谓词为 NULL → 该行被排除（与 SQLite 一致）。
    append_text("left(data -> 'data' ->> 'Name', length($" + std::to_string(index) +
                    ")) =",
                *query.name_prefix);
  }
  if (query.created_after_epoch_seconds >= 0) {
    append_epoch(">=", query.created_after_epoch_seconds);
  }
  if (query.created_before_epoch_seconds >= 0) {
    append_epoch("<=", query.created_before_epoch_seconds);
  }

  //  稳定全序：created_at 升序、同秒按 id 升序（分页不漏不重，与 SQLite 一致）
  where += " ORDER BY created_at ASC, id ASC LIMIT $" + std::to_string(index) +
           "::int OFFSET $" + std::to_string(index + 1) + "::int";
  params.emplace_back(std::to_string(query.limit));
  params.emplace_back(std::to_string(query.offset));

  FSS_TRY(handle, pool_->Borrow());

  domain::MetadataPage page;
  {
    //  total = 过滤后、分页前的总数
    const auto counted = handle->ExecParams(count_where, count_params);
    if (!counted.ok()) return Annotate(counted.error(), "统计元数据记录失败");
    if (counted.value().RowCount() == 0) {
      return fss::Err(fss::ErrorKind::kInternal, "统计元数据记录失败：查询没有返回行");
    }
    page.total = ParseInt64(counted.value().Value(0, 0));
  }
  const auto rows = handle->ExecParams(where, params);
  if (!rows.ok()) return Annotate(rows.error(), "查询元数据记录列表失败");
  for (int row = 0; row < rows.value().RowCount(); ++row) {
    FSS_TRY(record, RowToRecord(rows.value(), row));
    page.records.push_back(std::move(record));
  }
  return page;
}

std::size_t PostgresMetadataRepository::VersionCount(std::string_view partition,
                                                     std::string_view record_id) {
  auto handle = pool_->Borrow();
  if (!handle.ok()) return 0;
  const auto result =
      handle.value()->ExecParams(kCountVersions, {std::string(partition), std::string(record_id)});
  if (!result.ok() || result.value().RowCount() == 0) return 0;
  return static_cast<std::size_t>(ParseInt64(result.value().Value(0, 0)));
}

}  // namespace fss::infra
