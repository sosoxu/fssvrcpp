// =============================================================================
//  PostgresMetadataRepository（L2，`src/infra/metadata/postgres/`）—— 多实例元数据仓储
// =============================================================================
//  实现 `domain::IMetadataRepository`（ADR-009 §4.2：多实例下元数据记录必须放 PG；
//  幂等键 `(partition_id, file_source)` 上的**原子领取**是 M2 缺陷的修复）。
//  表结构取自 `db/migrations/001_init.sql` 的 `file_metadata_records`：
//
//      partition_id  TEXT        NOT NULL
//      id            TEXT        NOT NULL   -- "<partition>:dataset--File.Generic:<uuid>"
//      version       INTEGER     NOT NULL
//      kind          TEXT        NOT NULL
//      state         TEXT        NOT NULL DEFAULT 'ready'   -- claiming | ready | deleted
//      is_latest     BOOLEAN     NOT NULL DEFAULT TRUE
//      created_at    TIMESTAMPTZ NOT NULL DEFAULT now()
//      created_by    TEXT        NOT NULL
//      acl_viewers   JSONB       NOT NULL DEFAULT '[]'
//      acl_owners    JSONB       NOT NULL DEFAULT '[]'
//      legal_tags    JSONB       NOT NULL DEFAULT '[]'
//      file_source   TEXT                    -- data.DatasetProperties.FileSourceInfo.FileSource
//      data          JSONB       NOT NULL    -- 完整记录 JSON（信封 + data）
//      PRIMARY KEY (partition_id, id, version)
//      UNIQUE INDEX ux_mr_source (partition_id, file_source)
//             WHERE state <> 'deleted' AND is_latest        ← 幂等键（ADR-009 §4.2 / R5/R6）
//      UNIQUE INDEX ux_mr_latest (partition_id, id) WHERE is_latest
//
//  ★ 与 `SqliteMetadataRepository` 的关系：**它是契约**
//    本实现的可观测语义（错误 kind 与消息、幂等返回、版本链、`List` 的过滤/排序/分页/
//    `total`、`data` 的无损往返）必须与 SQLite 实现逐条一致，并由
//    `tests/framework/port_contract.h::CheckMetadataRepositoryContract` 在**同一个**
//    断言集上钉住（C2.10 的元数据侧 / C6.11）。凡与 SQLite 不同的地方都在下面注明理由。
//
//  ★ SQLite ↔ PG 的列映射（两套 schema 不同，裁决如下；不再重新设计）
//    | SQLite `metadata` 列 | PG 列 | 规则 |
//    | --- | --- | --- |
//    | `version`            | `version`       | `Create` → `1`；`Update` → 现有 latest 的 `version + 1`。读回时**列**覆盖 JSON 里的 `version` |
//    | `is_latest`          | `is_latest`     | `Update` 先清旧 latest，再以 `is_latest = TRUE` 插入新版本（`ux_mr_latest` 保证每个 `(partition,id)` 只有一个 latest） |
//    | `previous_version`   | **无此列**      | **不建列、不加迁移**：领域侧的 `ancestry` 本来就在记录 JSON 里（`ToJson` 会输出 `"ancestry"`），靠 `data` 列无损往返 |
//    | `name`               | **无此列**      | **不建列**：`MetadataQuery::name_prefix` 过滤 JSONB 的 `data -> 'data' ->> 'Name'`；语义 = SQLite 的**字面前缀**（见 `List` 的注释） |
//    | `created_at INTEGER` | `created_at TIMESTAMPTZ` | **写**注入时钟的 epoch 秒（`to_timestamp($n::bigint)`）；**读/过滤**用 `floor(extract(epoch from created_at))::bigint`（`floor` 而非裸 `::bigint`：截断语义与 SQLite 的整数 epoch 一致，四舍五入会带来 ±1 s 漂移，破坏**含端点**的时间区间过滤） |
//    | `created_by`         | `created_by`    | SQLite 写 `""`；PG 的列 NOT NULL 且无默认值 → 同样写 `""`（逐字节一致） |
//    | `data TEXT`          | `data JSONB`    | 存 `json::Dump(domain::ToJson(record))` —— 与 SQLite 绑定的**是同一份完整信封**，未知/额外字段无损往返 |
//    | （无）               | `state`         | **所有读取都过滤 `state <> 'deleted'`**；`Create`/`Update` 写 `'ready'`。`Delete` 是**硬删除全部版本**（见下） |
//    | （无）               | `acl_viewers` / `acl_owners` / `legal_tags` | 写入时从记录填（`record.acl.viewers` / `record.acl.owners` / `record.legal.legaltags`），使这三列成为**忠实的反规范化镜像**而非永久为空；**读取只用 `data` JSON**（单一事实来源） |
//
//  ★ 时间基准：`created_at` 来自**注入的 `IClock`**（与 SQLite 逐字一致）。
//    ADR-009 §6.4 的"一律用数据库 `now()`"是针对**租约/过期判定**（`staging_leases`，
//    见 `PostgresLeaseRepository`）的 —— 元数据记录的 `created_at` 是应用时钟产生的时间戳，
//    端口契约用 `ManualClock` 钉住它（时间区间过滤因此可确定性测试），组合根将来注入真实时钟。
//
//  ★ 本切片**未实现**的部分（如实登记，ADR-009 §10 第 2 项，留给后续切片）：
//    · schema 里 `state = 'deleted'` 的**软删除**语义与 `claiming` → `ready` 状态机；
//    · `CreateFileMetadata` 的**跨步骤**原子领取（本仓储只提供"单条 INSERT 的原子领取"）。
//    读取路径已经过滤 `state <> 'deleted'`，因此**外部写入的 tombstone 行不会重新冒出来**；
//    `Delete` 仍然是硬删除（与 SQLite 一致）。
//
//  ★ 分区隔离是**每条语句**级的：所有 DML 都带 `partition_id` 条件
//    （机械护栏 tests/unit/test_sql_guardrail.cpp；组合根未接线前，跨租户串数据
//    不会有任何上层症状，只能靠这条护栏与契约测试兜住）。
#pragma once

#include "common/result/result.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/postgres/pg_connection.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace fss::infra {

//  ★ `pg` 必须是**第一个成员**：组合根将来按 `metadata.postgres.*` 直接填充，
//    保持"配置结构体 → 传输结构体"的字段顺序一致，避免位置初始化静默错位。
struct PostgresMetadataRepositoryOptions {
  PgOptions pg;
};

class PostgresMetadataRepository final : public domain::IMetadataRepository {
 public:
  //  建立连接池（DSN 不可达 → `Err(kUnavailable, <libpq 消息>)`，fail-closed）。
  //  `clock` 只用于 `Create` / `Update` 写入 `created_at` 的 epoch 秒（见文件头）。
  static fss::Result<std::unique_ptr<PostgresMetadataRepository>> Open(
      PostgresMetadataRepositoryOptions options, const fss::IClock& clock);

  ~PostgresMetadataRepository() override;
  PostgresMetadataRepository(const PostgresMetadataRepository&) = delete;
  PostgresMetadataRepository& operator=(const PostgresMetadataRepository&) = delete;

  fss::Result<domain::FileMetadataRecord> Create(std::string_view partition,
                                                 const domain::FileMetadataRecord& record) override;
  fss::Result<domain::FileMetadataRecord> GetById(std::string_view partition,
                                                  std::string_view record_id) override;
  fss::Result<domain::FileMetadataRecord> GetLatestByFileSource(
      std::string_view partition, std::string_view file_source) override;
  fss::Result<domain::FileMetadataRecord> Update(std::string_view partition,
                                                 const domain::FileMetadataRecord& record) override;
  fss::Result<void> Delete(std::string_view partition, std::string_view record_id) override;
  fss::Result<domain::MetadataPage> List(std::string_view partition,
                                         const domain::MetadataQuery& query) override;

  //  测试可观测性（与内存/SQLite 实现同名方法）：某个 record id 的**全部版本**数。
  std::size_t VersionCount(std::string_view partition, std::string_view record_id);

  //  诊断（测试/排障）：当前池的活跃/空闲连接数
  PgPool& pool() const { return *pool_; }

 private:
  PostgresMetadataRepository() = default;

  //  以下两个 helper 使用**调用方已借出的**连接（同一连接 = 同一会话/事务边界，P3-D10 的教训）。
  fss::Result<domain::FileMetadataRecord> FindLatestById(PgConnection& connection,
                                                         std::string_view partition,
                                                         std::string_view record_id);
  fss::Result<domain::FileMetadataRecord> FindLatestBySource(PgConnection& connection,
                                                             std::string_view partition,
                                                             std::string_view file_source);

  const fss::IClock* clock_ = nullptr;
  std::unique_ptr<PgPool> pool_;
};

}  // namespace fss::infra
