// PG schema 版本与 readiness 探针实现。设计与 SQL 纪律见头文件。
#include "infra/postgres/pg_schema.h"

#include "infra/postgres/pg_connection.h"

#include <cstdlib>
#include <string>

namespace fss::infra {

namespace {

//  从 `MapPgError` 挂在 Error 上的 details 里取 SQLSTATE（不存在则空串）。
std::string SqlStateOf(const fss::Error& error) {
  for (const auto& [key, value] : error.details()) {
    if (key == "sqlstate") return value;
  }
  return {};
}

}  // namespace

fss::Result<void> ProbePgLiveness(PgPool& pool) {
  FSS_TRY(handle, pool.Borrow());
  //  不访问分区表 → 独立定界符（见头文件的说明）。
  const auto result = handle->ExecParams(R"pgsql(SELECT 1)pgsql", {});
  if (!result.ok()) {
    return Annotate(result.error(), "readiness 的 PG 存活探针失败（SELECT 1）");
  }
  if (result.value().RowCount() == 0) {
    return Err(fss::ErrorKind::kUnavailable,
               "readiness 的 PG 存活探针失败：SELECT 1 没有返回行");
  }
  return Ok();
}

fss::Result<int> ReadAppliedSchemaVersion(PgPool& pool) {
  FSS_TRY(handle, pool.Borrow());
  const auto result =
      handle->ExecParams(R"pgsql(SELECT max(version) FROM schema_migrations)pgsql", {});
  if (!result.ok()) {
    //  42P01 = undefined_table：库里的迁移从未应用（表由 001_init.sql 创建）。
    //  目标库（PG 12.6）的错误消息可能被本地化，因此**按 SQLSTATE 判定**，不匹配文本。
    if (SqlStateOf(result.error()) == "42P01") {
      return Err(fss::ErrorKind::kUnavailable,
                 "schema_migrations 表不存在：该数据库从未应用迁移"
                 "（请按顺序执行 db/migrations/*.sql）");
    }
    return Annotate(result.error(), "readiness 读取 schema_migrations 版本失败");
  }
  if (result.value().RowCount() == 0 || result.value().IsNull(0, 0)) {
    return Err(fss::ErrorKind::kUnavailable,
               "schema_migrations 为空：该数据库存在迁移表，但没有任何已记录的迁移版本"
               "（迁移应用流程没有把版本写进表）");
  }
  const std::string text = result.value().Value(0, 0);
  if (text.empty()) {
    return Err(fss::ErrorKind::kInternal,
               "readiness 读取 schema_migrations 版本失败：返回空值");
  }
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0') {
    return Err(fss::ErrorKind::kInternal,
               "readiness 读取 schema_migrations 版本失败：max(version)=\"" + text +
                   "\" 不是十进制整数");
  }
  return static_cast<int>(parsed);
}

fss::Result<void> ProbePgSharedState(PgPool& pool, bool check_schema_version) {
  //  ① 存活：无论版本检查是否开启都必须做（`schema_version_check=false` 只跳过版本比对）。
  FSS_TRY(ProbePgLiveness(pool));
  if (!check_schema_version) return Ok();
  //  ② 迁移版本：单一真相 = `kExpectedSchemaVersion`（机械测试从迁移文件名推导）。
  FSS_TRY(applied, ReadAppliedSchemaVersion(pool));
  if (applied != kExpectedSchemaVersion) {
    const std::string direction =
        applied < kExpectedSchemaVersion
            ? "落后：请按顺序应用 db/migrations/ 下尚未应用的迁移"
            : "超前：该数据库由更新的迁移版本创建，本二进制不认识它的 schema（请升级二进制）";
    return Err(fss::ErrorKind::kUnavailable,
               "schema 版本不符：数据库已应用 max(version)=" + std::to_string(applied) +
                   "，本二进制期望 " + std::to_string(kExpectedSchemaVersion) + "（" +
                   direction + "）");
  }
  return Ok();
}

fss::Result<int> ReadPgMaxConnections(PgPool& pool) {
  FSS_TRY(handle, pool.Borrow());
  //  不访问分区表 → 独立定界符（见头文件）。
  const auto result = handle->ExecParams(
      R"pgsql(SELECT setting::int FROM pg_settings WHERE name = 'max_connections')pgsql", {});
  if (!result.ok()) {
    return Annotate(result.error(), "读取 PG max_connections 失败（连接预算校验）");
  }
  if (result.value().RowCount() == 0 || result.value().IsNull(0, 0)) {
    return Err(fss::ErrorKind::kInternal,
               "读取 PG max_connections 失败：pg_settings 没有返回行"
               "（连接预算无法校验，fail-closed）");
  }
  const std::string text = result.value().Value(0, 0);
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || parsed <= 0) {
    return Err(fss::ErrorKind::kInternal,
               "读取 PG max_connections 失败：取值 \"" + text + "\" 不是正整数");
  }
  return static_cast<int>(parsed);
}

}  // namespace fss::infra
