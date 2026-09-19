// PostgresLocationRepository 实现。schema 依据、与 SQLite 的语义对齐点见头文件。
#include "infra/location/postgres/postgres_location_repository.h"

#include "common/json/json.h"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fss::infra {

namespace {

//  ---- SQL（全部写成 raw string，便于 C3.9 护栏机械检索；每条 DML 都带 partition_id）----
//  ★ 带值的 SQL 一律用 `PQexecParams` 占位符（$1..$n），**不拼字符串**（见 pg_connection.h）。
constexpr const char* kUpsert = R"sql(
INSERT INTO file_locations
  (partition_id, file_id, file_source, container, object_key, zone, driver,
   created_by, created_at, migrated_at, data)
VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9::bigint, $10::bigint, $11::jsonb)
ON CONFLICT (partition_id, file_id) DO UPDATE SET
  file_source = EXCLUDED.file_source,
  container   = EXCLUDED.container,
  object_key  = EXCLUDED.object_key,
  zone        = EXCLUDED.zone,
  driver      = EXCLUDED.driver,
  created_by  = EXCLUDED.created_by,
  created_at  = EXCLUDED.created_at,
  migrated_at = EXCLUDED.migrated_at,
  data        = EXCLUDED.data)sql";

constexpr const char* kSelectByFileId =
    R"sql(SELECT data FROM file_locations WHERE partition_id = $1 AND file_id = $2)sql";

constexpr const char* kSelectByFileSource =
    R"sql(SELECT data FROM file_locations WHERE partition_id = $1 AND file_source = $2)sql";

constexpr const char* kSelectListBase =
    R"sql(SELECT data FROM file_locations WHERE partition_id = $1)sql";

constexpr const char* kCountListBase =
    R"sql(SELECT COUNT(*) FROM file_locations WHERE partition_id = $1)sql";

//  ★ 用 `jsonb_set` 一条语句原子地改 `data` 里的两个键：
//    · 只动 signed_url / updated_at_epoch_seconds，created_at 与**未知字段原样保留**；
//    · 不读-改-写 → 并发下不会互相覆盖对方写进去的字段（SQLite 靠单连接互斥达成同样效果）。
constexpr const char* kUpdateSignedUrl = R"sql(
UPDATE file_locations
   SET data = jsonb_set(
                jsonb_set(data, '{signed_url}', to_jsonb($1::text), true),
                '{updated_at_epoch_seconds}', to_jsonb($2::bigint), true)
 WHERE partition_id = $3 AND file_id = $4)sql";

constexpr const char* kDelete =
    R"sql(DELETE FROM file_locations WHERE partition_id = $1 AND file_id = $2)sql";

//  ---- 参数校验与错误（消息与 SqliteLocationRepository 保持一致）----
fss::Error Invalid(const std::string& message) {
  return fss::Err(fss::ErrorKind::kInvalidArgument, message);
}
fss::Error NotFound(const std::string& message) {
  return fss::Err(fss::ErrorKind::kNotFound, message);
}

//  ---- FileLocation ↔ JSON（`data` 列）----
//  ★ 这段映射与 SqliteLocationRepository 的 DumpLocation/ParseLocation **逐字对应**；
//    契约测试（`CheckLocationRepositoryContract`）会同时跑两边，任何漂移都会红。
//    已知字段写在顶层；`extra` 的键也写在顶层，读回时把不认识的键收回 `extra`。
//  ★ 这里**不能**把 `container` / `object_key` 当成已知字段：它们在领域模型里属于
//    `FileLocation::extra`（物理引用的存放处），只因为 `extra` 的键被摊平写到顶层。
//    若把它们算作已知键，回读时既进不了 `extra`、也没有领域字段接住 → 物理引用静默丢失
//    （P4-D09，同一坑在 SQLite 实现里踩过）。
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

}  // namespace

fss::Result<std::unique_ptr<PostgresLocationRepository>> PostgresLocationRepository::Open(
    PostgresLocationRepositoryOptions options) {
  //  ★ 建池即建连：DSN 不可达/权限不足在这里就返回 kUnavailable，绝不回退到别的存储。
  FSS_TRY(pool, PgPool::Create(std::move(options.pg)));
  std::unique_ptr<PostgresLocationRepository> repository(new PostgresLocationRepository());
  repository->pool_ = std::move(pool);
  return Ok(std::move(repository));
}

PostgresLocationRepository::~PostgresLocationRepository() = default;

fss::Result<void> PostgresLocationRepository::Save(std::string_view partition,
                                                   const domain::FileLocation& location) {
  if (partition.empty()) return Invalid("partition 不能为空");
  if (location.file_id.empty()) return Invalid("file_id 不能为空");
  if (location.file_source.empty()) return Invalid("file_source 不能为空");

  FSS_TRY(handle, pool_->Borrow());
  //  `migrated_at` 的语义（SQLite 没有这一列）：迁移到 persistent 时记下时间。
  PgConnection::Param migrated_at = std::nullopt;
  if (location.zone == domain::StorageZone::kPersistent) {
    migrated_at = std::to_string(location.updated_at_epoch_seconds);
  }
  const std::string data = DumpLocation(location);
  const auto result = handle->ExecParams(
      kUpsert, {std::string(partition), location.file_id, location.file_source,
                ExtraString(location, "container"), ExtraString(location, "object_key"),
                std::string(domain::StorageZoneName(location.zone)),
                std::string(domain::StorageDriverName(location.driver)), location.user_id,
                std::to_string(location.created_at_epoch_seconds), migrated_at, data});
  if (!result.ok()) {
    //  23505 已被 MapPgError 映射成 kLocationAlreadyExists（(partition, file_source) 冲突），
    //  与 SQLite 的 StepUpsert 同一语义。
    return Annotate(result.error(), "写入位置记录失败");
  }
  return Ok();
}

fss::Result<domain::FileLocation> PostgresLocationRepository::Find(std::string_view partition,
                                                                   std::string_view file_id) {
  FSS_TRY(handle, pool_->Borrow());
  const auto result =
      handle->ExecParams(kSelectByFileId, {std::string(partition), std::string(file_id)});
  if (!result.ok()) return Annotate(result.error(), "查询位置记录失败");
  if (result.value().RowCount() == 0) {
    return NotFound("Not found location for fileID : " + std::string(file_id));
  }
  return ParseLocation(result.value().Value(0, 0));
}

fss::Result<domain::FileLocation> PostgresLocationRepository::FindByFileSource(
    std::string_view partition, std::string_view file_source) {
  FSS_TRY(handle, pool_->Borrow());
  const auto result = handle->ExecParams(kSelectByFileSource,
                                         {std::string(partition), std::string(file_source)});
  if (!result.ok()) return Annotate(result.error(), "查询位置记录失败");
  if (result.value().RowCount() == 0) {
    return NotFound("位置记录不存在（按 file_source）：" + std::string(file_source));
  }
  return ParseLocation(result.value().Value(0, 0));
}

fss::Result<void> PostgresLocationRepository::UpdateSignedUrl(
    std::string_view partition, std::string_view file_id, std::string_view signed_url,
    std::int64_t updated_at_epoch_seconds) {
  FSS_TRY(handle, pool_->Borrow());
  const auto result = handle->ExecParams(
      kUpdateSignedUrl, {std::string(signed_url), std::to_string(updated_at_epoch_seconds),
                         std::string(partition), std::string(file_id)});
  if (!result.ok()) return Annotate(result.error(), "更新 signed_url 失败");
  if (result.value().AffectedRows() == 0) {
    return NotFound("位置记录不存在：" + std::string(file_id));
  }
  return Ok();
}

fss::Result<void> PostgresLocationRepository::Delete(std::string_view partition,
                                                     std::string_view file_id) {
  FSS_TRY(handle, pool_->Borrow());
  const auto result =
      handle->ExecParams(kDelete, {std::string(partition), std::string(file_id)});
  if (!result.ok()) return Annotate(result.error(), "删除位置记录失败");
  if (result.value().AffectedRows() == 0) {
    return NotFound("位置记录不存在：" + std::string(file_id));
  }
  return Ok();
}

fss::Result<domain::LocationPage> PostgresLocationRepository::List(
    std::string_view partition, const domain::LocationQuery& query) {
  if (query.limit <= 0) return Invalid("limit 必须 > 0");
  if (query.offset < 0) return Invalid("offset 必须 >= 0");

  //  动态拼接过滤条件（基句永远带 partition_id；占位符编号连续）。
  std::string where = kSelectListBase;
  std::string count_where = kCountListBase;
  std::vector<PgConnection::Param> params{std::string(partition)};
  std::vector<PgConnection::Param> count_params{std::string(partition)};
  const bool by_user = !query.user_id.empty();
  const bool after = query.created_after_epoch_seconds >= 0;
  const bool before = query.created_before_epoch_seconds >= 0;
  int index = 2;
  const auto append = [&](const std::string& predicate, PgConnection::Param value) {
    //  `predicate` 自带比较运算符（如 "created_at >="），这里只补占位符与类型转换。
    const std::string placeholder =
        " AND " + predicate + " $" + std::to_string(index) + "::bigint";
    where += placeholder;
    count_where += placeholder;
    params.push_back(value);
    count_params.push_back(value);
    ++index;
  };
  if (by_user) {
    //  created_by 是 TEXT，不能用 ::bigint 后缀
    const std::string placeholder = " AND created_by = $" + std::to_string(index);
    where += placeholder;
    count_where += placeholder;
    params.push_back(query.user_id);
    count_params.push_back(query.user_id);
    ++index;
  }
  if (after) append("created_at >=", std::to_string(query.created_after_epoch_seconds));
  if (before) append("created_at <=", std::to_string(query.created_before_epoch_seconds));
  //  稳定全序：created_at 升序，同秒按 file_id 升序（端口注释 §ports.h；offset 分页必须稳定）
  where += " ORDER BY created_at ASC, file_id ASC LIMIT $" + std::to_string(index) +
           "::int OFFSET $" + std::to_string(index + 1) + "::int";
  params.push_back(std::to_string(query.limit));
  params.push_back(std::to_string(query.offset));

  FSS_TRY(handle, pool_->Borrow());

  domain::LocationPage page;
  {
    //  total = 过滤后、分页前的总数
    const auto counted = handle->ExecParams(count_where, count_params);
    if (!counted.ok()) return Annotate(counted.error(), "统计位置记录失败");
    if (counted.value().RowCount() == 0) {
      return Err(fss::ErrorKind::kInternal, "统计位置记录失败：查询没有返回行");
    }
    const std::string total = counted.value().Value(0, 0);
    page.total = total.empty() ? 0 : std::stoll(total);
  }
  const auto rows = handle->ExecParams(where, params);
  if (!rows.ok()) return Annotate(rows.error(), "查询位置记录列表失败");
  for (int row = 0; row < rows.value().RowCount(); ++row) {
    const auto location = ParseLocation(rows.value().Value(row, 0));
    if (!location.ok()) return location.error();
    page.records.push_back(std::move(location.value()));
  }
  return page;
}

}  // namespace fss::infra
