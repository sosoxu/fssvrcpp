// =============================================================================
//  PostgresLeaseRepository（L2，`src/infra/location/postgres/`）—— 多实例在途租约
// =============================================================================
//  实现 `domain::ILeaseRepository`（ADR-009 §4.3：GC 只能回收"租约已过期且无元数据
//  记录"的对象；在途上传必须能被**任何实例**看见，因此租约必须放 PG）。
//  表结构取自 `db/migrations/001_init.sql` 的 `staging_leases`：
//
//      partition_id  TEXT        NOT NULL
//      file_source   TEXT        NOT NULL
//      owner         TEXT        NOT NULL        -- instance_id
//      acquired_at   TIMESTAMPTZ NOT NULL DEFAULT now()
//      renewed_at    TIMESTAMPTZ NOT NULL DEFAULT now()
//      expires_at    TIMESTAMPTZ NOT NULL        -- now() + ttl
//      PRIMARY KEY (partition_id, file_source)
//
//  ★★ 端口签名与 ADR-009 §4.3 的**阻抗匹配**（本切片已定案，不再更改）
//  ---------------------------------------------------------------------------
//  表的身份列是 `file_source`（ADR-009 §4.3 的 schema 就是 `(partition_id, file_source)`
//  主键 —— GC 要靠它与 `file_locations`/`file_metadata_records` 对齐），而端口
//  `ILeaseRepository` 的第一个参数**名字**叫 `file_id`（`ports.h`）。
//  本切片的裁决：
//      **端口的这个参数 = "租约键" = 被写入的 staging 对象的 `file_source`。**
//    即 `Acquire(partition, lease_key, owner, ttl)` 会写入/更新
//    `(partition_id, file_source = lease_key)` 这一行；`Renew` / `Release` 同理。
//  为什么不是别的解释：
//    · 表里根本没有 `file_id` 列 —— 端口参数名是历史命名，不能凭名字反推 schema；
//    · `ClaimExpired` 需要把 `Lease::file_id` 回填给 GC（`gc_task.cpp` 用
//      `lease.file_id` 去 `locations.Find`），所以 PG 实现在领取时用
//      `LEFT JOIN file_locations ON (partition_id, file_source)` 反解出真正的 `file_id`；
//      没有位置记录时留空 —— GC 对空 `file_id` 的表现是"查不到位置 → 跳过"（安全方向）。
//  ⚠️ 真实影响范围（如实登记）：**生产代码目前没有任何调用方 `Acquire` 租约**
//     （上传路径接租约是后续切片）。当前唯一使用者是 `GcTask::CollectExpiredLeases`
//     的"领取过期租约"路径，而它默认为空（`gc.require_lease_expiry=true` 时）。
//     因此本实现落地不会改变现有进程的可观测行为，组合根也**尚未**装配它。
//
//  ★ 时间基准：TTL/过期**只用数据库 `now()`**（ADR-009 §6.4）——
//    `expires_at = now() + ttl`、`renewed_at = now()`，绝不用实例本地时钟。
//
//  ★ `ClaimExpired` 必须**原子且对并发领取者非阻塞**：
//    `FOR UPDATE SKIP LOCKED` 在同一事务内挑选 + 改 owner + 推后 60 秒 +
//    `RETURNING`，一条语句完成（CTE）。两个 GC 实例同时领取时，同一条租约只会被
//    一个实例拿到（ADR-009 §4.3 的 M3 修复）。
#pragma once

#include "common/result/result.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/postgres/pg_connection.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fss::infra {

//  ★ C2（ADR-009 §6.4）：租约的时间基准。
//    · `kDatabase`（默认）= SQL `now()`：**跨实例同源**，消除实例间时钟偏移（推荐）。
//    · `kLocal`             = 注入的 `IClock`：`expires_at = to_timestamp(clock.NowEpochSeconds() + ttl)`。
//      为什么允许它：单实例 / 受控测试需要让租约判定跟随**可注入时钟**（ManualClock），
//      否则"到期/续租"只能靠 sleep 或改宿主时钟。代价是**放弃跨实例同源**——
//      多实例部署必须保持默认 `database`（`leases.time_source` 的语义见 operations.md §1.3）。
enum class LeaseTimeSource { kDatabase, kLocal };

//  ★ `pg` 必须是**第一个成员**（与 `location.postgres.*` 的配置字段顺序一致）。
struct PostgresLeaseRepositoryOptions {
  PgOptions pg;
  LeaseTimeSource time_source = LeaseTimeSource::kDatabase;
  //  `kLocal` 时必须非空（否则 `Open` 返回 kInvalidArgument，绝不静默回退到数据库钟）。
  const fss::IClock* clock = nullptr;
};

class PostgresLeaseRepository final : public domain::ILeaseRepository {
 public:
  //  建立连接池（DSN 不可达 → `Err(kUnavailable, <libpq 消息>)`）。
  static fss::Result<std::unique_ptr<PostgresLeaseRepository>> Open(
      PostgresLeaseRepositoryOptions options = {});

  ~PostgresLeaseRepository() override;
  PostgresLeaseRepository(const PostgresLeaseRepository&) = delete;
  PostgresLeaseRepository& operator=(const PostgresLeaseRepository&) = delete;

  //  `file_id` 参数 = `staging_leases.file_source`（"租约键"，见文件头）。
  fss::Result<Lease> Acquire(std::string_view partition, std::string_view file_id,
                             std::string_view owner_instance_id,
                             std::int64_t ttl_millis) override;
  fss::Result<void> Renew(std::string_view partition, std::string_view file_id,
                          std::string_view owner_instance_id, std::int64_t ttl_millis) override;
  fss::Result<void> Release(std::string_view partition, std::string_view file_id,
                            std::string_view owner_instance_id) override;
  fss::Result<std::vector<Lease>> ClaimExpired(std::string_view partition, int limit,
                                               std::string_view claimant_instance_id) override;

  //  诊断（测试/排障）
  PgPool& pool() const { return *pool_; }

 private:
  PostgresLeaseRepository() = default;

  //  `owner` 校验失败时区分"不存在"（kNotFound）与"不是自己的"（kPermissionDenied）。
  fss::Error OwnerMismatch(std::string_view partition, std::string_view lease_key,
                           PgConnection& connection);

  //  当前生效的时间基准（`kLocal` 时 `clock_` 必非空，由 `Open` 保证）。
  LeaseTimeSource time_source_ = LeaseTimeSource::kDatabase;
  const fss::IClock* clock_ = nullptr;
  std::unique_ptr<PgPool> pool_;
};

}  // namespace fss::infra
