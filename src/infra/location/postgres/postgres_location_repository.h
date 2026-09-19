// =============================================================================
//  PostgresLocationRepository（L2，`src/infra/location/postgres/`）—— 多实例位置仓储
// =============================================================================
//  实现 `domain::IFileLocationRepository`（ADR-009 §4.1：多实例的位置记录必须放 PG）。
//  表结构取自 `db/migrations/001_init.sql` 的 `file_locations`：
//
//      partition_id  TEXT    NOT NULL
//      file_id       TEXT    NOT NULL
//      file_source   TEXT    NOT NULL
//      container     TEXT    NOT NULL      ← 物理容器（POSIX 目录 / S3 bucket）
//      object_key    TEXT    NOT NULL      ← 物理键
//      zone          TEXT    NOT NULL      ← 'staging' | 'persistent'
//      driver        TEXT    NOT NULL      ← 'posix' | 's3'
//      created_by    TEXT    NOT NULL      ← FileLocation::user_id
//      created_at    BIGINT  NOT NULL      ← epoch 秒
//      migrated_at   BIGINT                ← 迁移到 persistent 的时间（可空）
//      data          JSONB   NOT NULL      ← 完整 JSON（未知字段原样保留）
//      PRIMARY KEY (partition_id, file_id)
//      UNIQUE (partition_id, file_source)  ← 幂等键（ADR-009 §4.2 / R5）
//
//  ★ 与 `SqliteLocationRepository` 的关系：**它是契约**
//    本实现的可观测语义（错误 kind、upsert、`List` 的过滤/排序/分页/total、
//    `data` 的无损往返）必须与 SQLite 实现逐条一致，并由
//    `tests/framework/port_contract.h::CheckLocationRepositoryContract` 在**同一个**
//    断言集上钉住（C2.10）。凡与 SQLite 不同的地方都在下面注明理由。
//
//  ★ 与 SQLite 的两处结构性差异（不影响可观测语义）
//    ① PG 没有 `updated_at` / `signed_url` 两个**独立列**：它们本来就在 `data` JSON 里
//       （SQLite 建了冗余列，但权威在 `data`）。因此 `UpdateSignedUrl` 用 `jsonb_set`
//       在一条语句里原子改 `data` 的两个键 —— 既避免读-改-写丢失并发字段，也天然是
//       "只改这两个键、created_at 与未知字段不动"。
//    ② PG 有 `migrated_at` 列（SQLite 没有）。本实现按 ADR-004 §9.1 的语义写：
//       `zone = persistent` 时记 `updated_at_epoch_seconds`，否则 NULL；读回时**忽略**
//       （端口没有对应的领域字段，写进 extra 会破坏未知字段的无损往返）。
//
//  ★ 分区隔离是**每条语句**级的：所有 DML 都带 `partition_id` 条件
//    （机械护栏 tests/unit/test_sql_guardrail.cpp；组合根未接线前，跨租户串数据
//    不会有任何上层症状，只能靠这条护栏与契约测试兜住）。
#pragma once

#include "common/result/result.h"
#include "domain/ports/ports.h"
#include "infra/postgres/pg_connection.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace fss::infra {

//  ★ `pg` 必须是**第一个成员**：组合根将来按 `location.postgres.*` 直接填充，
//    保持"配置结构体 → 传输结构体"的字段顺序一致，避免位置初始化静默错位。
struct PostgresLocationRepositoryOptions {
  PgOptions pg;
};

class PostgresLocationRepository final : public domain::IFileLocationRepository {
 public:
  //  建立连接池（并在 DSN 不可达时返回 `Err(kUnavailable, <libpq 消息>)`）。
  static fss::Result<std::unique_ptr<PostgresLocationRepository>> Open(
      PostgresLocationRepositoryOptions options = {});

  ~PostgresLocationRepository() override;
  PostgresLocationRepository(const PostgresLocationRepository&) = delete;
  PostgresLocationRepository& operator=(const PostgresLocationRepository&) = delete;

  fss::Result<void> Save(std::string_view partition,
                         const domain::FileLocation& location) override;
  fss::Result<domain::FileLocation> Find(std::string_view partition,
                                         std::string_view file_id) override;
  fss::Result<void> UpdateSignedUrl(std::string_view partition, std::string_view file_id,
                                    std::string_view signed_url,
                                    std::int64_t updated_at_epoch_seconds) override;
  fss::Result<void> Delete(std::string_view partition, std::string_view file_id) override;
  fss::Result<domain::FileLocation> FindByFileSource(std::string_view partition,
                                                     std::string_view file_source) override;
  fss::Result<domain::LocationPage> List(std::string_view partition,
                                         const domain::LocationQuery& query) override;

  //  诊断（测试/排障）：当前池的活跃/空闲连接数
  PgPool& pool() const { return *pool_; }

 private:
  PostgresLocationRepository() = default;

  std::unique_ptr<PgPool> pool_;
};

}  // namespace fss::infra
