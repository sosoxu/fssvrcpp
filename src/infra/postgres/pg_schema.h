// =============================================================================
//  PG schema 版本与 readiness 探针（L2，`src/infra/postgres/`）
// =============================================================================
//  为什么需要这个模块（ADR-009 §5.3 / §6.4）
//  ---------------------------------------------------------------------------
//  `deployment.mode=multi` 下所有实例共享同一个数据库。若某个实例连到了一个
//  **迁移版本不同**的库（少跑了一次迁移、或库比二进制更新），它就会用错误的列/约束
//  读写共享状态 —— 这类错误不会在启动时报错，只会在运行期表现为"偶发丢数据/约束冲突"，
//  是最难排查的一类多实例事故。因此 readiness 必须校验**共享状态可用**：
//    ① PG 存活（`SELECT 1` 通过**既有连接池**）；
//    ② 迁移版本（`SELECT max(version) FROM schema_migrations`，表由
//       `db/migrations/001_init.sql` 创建）。
//
//  ★ `kExpectedSchemaVersion` 是**唯一真相**，且不允许静默漂移
//    新增 `db/migrations/002_*.sql` 时，如果忘了改这个常量，"二进制期望的 schema"
//    与"仓库里的迁移"就会分叉 —— 而分叉只有在生产 readiness 变红时才被发现。
//    因此 `tests/unit/test_schema_version_constant.cpp` 从 `db/migrations/*.sql`
//    的**文件名前缀**推导期望值并断言它等于本常量：**加迁移不改常量 → 测试失败**。
//
//  ★ SQL 定界符：本文件里的两条语句都**不访问任何分区表**（`SELECT 1` 与读
//    `schema_migrations` 都没有 partition 维度），因此按 `pg_connection.cpp` 的既有约定
//    用独立的 `R"pgsql(...)pgsql"` 原始串 —— C3.9 的 partition_id 扫描器只认
//    `R"sql(...)sql"`，让它看见这两条会产生**假阳性**（它们在语义上本来就没有分区条件）。
//    （`tests/unit/test_sql_guardrail.cpp` 的"盲区"用例解释了为什么仓库文件必须用 `sql`；
//    非 `*_repository.*` 的 L2 工具文件沿用 pg_connection.cpp 的先例。）
// =============================================================================
#pragma once

#include "common/result/result.h"

#include <cstdint>
#include <string>

namespace fss::infra {

class PgPool;

// -----------------------------------------------------------------------------
//  ★ 期望的 schema 版本（单一真相；机械测试从 db/migrations/*.sql 推导并比对）
// -----------------------------------------------------------------------------
//  取值规则：`db/migrations/NNN_name.sql` 里最大的 `NNN`（十进制）。
//  当前仓库只有 `001_init.sql` ⇒ 1。
inline constexpr int kExpectedSchemaVersion = 1;

// -----------------------------------------------------------------------------
//  探针（组合根在 readiness 路径上调用；错误消息面向运维，必须可读）
// -----------------------------------------------------------------------------

//  `SELECT 1`：通过**既有连接池**验证 PG 可达（借一条连接执行一次查询）。
fss::Result<void> ProbePgLiveness(PgPool& pool);

//  `SELECT max(version) FROM schema_migrations`。
//  · 表不存在（SQLSTATE 42P01）→ 可读原因「迁移从未应用」；
//  · 表存在但没有任何行（`max` 为 NULL）→ 可读原因「迁移从未记录」；
//  · 返回解析失败 → `kInternal`。
fss::Result<int> ReadAppliedSchemaVersion(PgPool& pool);

//  readiness 的"共享状态可用"综合探针：
//  · **总是**做存活探针（`SELECT 1`）；
//  · `check_schema_version=true` 时再比对 `max(version) == kExpectedSchemaVersion`。
//  任何一步失败 → 非 ok（消息可直接展示给运维）。
fss::Result<void> ProbePgSharedState(PgPool& pool, bool check_schema_version);

//  C9.28：读取**服务端**的 `max_connections`（`pg_settings`），用于启动期连接预算校验。
//  `setting::int`：目标 PG 12.6 的该 GUC 是整数型，`::int` 在 12/14 上都成立。
fss::Result<int> ReadPgMaxConnections(PgPool& pool);

}  // namespace fss::infra
