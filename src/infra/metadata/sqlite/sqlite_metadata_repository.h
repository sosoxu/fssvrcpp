// =============================================================================
//  SqliteMetadataRepository（L2）—— 元数据记录仓储的 SQLite 实现
// =============================================================================
//  ★ 与内存实现**共用同一份契约**（`tests/framework/port_contract.h` 的
//    `CheckMetadataRepositoryContract`，C2.10）：语义一致是可执行的断言，不是口头承诺。
//
//  语义要点（每条都在契约里有对应断言）：
//    · **幂等键 = `(partition_id, file_source)`**（R5）：`Create` 重复调用返回**第一次**那条，
//      既不新建记录也不新增版本。数据库层面用**部分唯一索引**保证
//      （`ux_metadata_source ... WHERE is_latest = 1`）。
//    · **版本链**（R6）：主键是 `(partition_id, id, version)`；`Update` 追加一版并把
//      `is_latest` 从旧版移到新版。`ux_metadata_latest ... WHERE is_latest = 1` 保证
//      "每个 id 只有一个最新版"，而**谓词必须含 `is_latest`** —— 否则会阻断合法版本链
//      （ADR-008/R6 踩过的坑）。
//    · **partition 严格隔离**：每条 SQL 都带 `partition_id`（C3.9 的源码护栏会机械检查）。
//    · `created_at` 来自**注入的时钟**（记录模型里没有时间字段），因此时间区间过滤与
//      分页顺序都是可确定性测试的。
//    · 排序：`created_at` 升序、同秒 `id` 升序 —— 分页必须有稳定全序，否则 offset 会漏/重。
#pragma once

#include "common/result/result.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

//  ★ 第三方类型的前向声明必须放**全局**命名空间：写进 `namespace fss::infra` 会声明出
//    `fss::infra::sqlite3`，成员签名里的 `sqlite3*` 也就跟着变成另一个类型（P3-D04）。
struct sqlite3;
struct sqlite3_stmt;

namespace fss::metrics {
class Registry;
}  // namespace fss::metrics

namespace fss::infra {

//  ★ 组提交的接缝/协调器声明在 `infra/sqlite/sqlite_group_commit.h`（L2 共用小工具）。
//    这里只前向声明（成员是指针 + unique_ptr，析构在 .cpp 里定义 ⇒ 不需要完整类型）。
class ISqliteBatchObserver;
class ISqliteBatchGate;
class ISqliteCommitFault;
class SqliteGroupCommitter;

struct SqliteMetadataRepositoryOptions {
  //  忙等超时：并发写时让 SQLite 自己等锁，而不是立刻返回 SQLITE_BUSY（C3.12 的同一策略）
  int busy_timeout_millis = 5000;
  //  ★ 阶段 10（切片 4）：journal 模式。`true`（默认）= `PRAGMA journal_mode=WAL`
  //    （与接线前逐字一致）；`false` = 不执行 PRAGMA → SQLite 默认 `DELETE`。
  //    与 `SqliteLocationRepositoryOptions::wal` 同构；组合根从
  //    `metadata.sqlite.journal_mode`（WAL|DELETE）映射，TRUNCATE → 拒绝启动。
  bool wal = true;
  //  ★ 阶段 10（切片 4）：`PRAGMA synchronous` 的取值（0 = OFF / 1 = NORMAL / 2 = FULL）。
  //    ⚠ 该 PRAGMA **不随库文件持久化**（`journal_mode` 会），因此**不能**用"另开一个
  //    sqlite3 连接读回"证明生效（新连接拿到的是它自己的默认值 FULL=2）。
  //    可观测入口是本类新增的 `AppliedPragma("synchronous")`（在**本连接**上读回）。
  int synchronous_level = 1;
  //  ★ 本切片：数据库层组提交（`metadata.sqlite.{group_commit,group_commit_max_wait_ms,
  //    group_commit_max_batch}`）。默认值 = `core_schema.cpp` / `config/fss.example.json`
  //    的默认值（`true` / `5` / `64`），由组合根逐键传入。
  //    · `group_commit=false` → **逐操作**提交（与接线前逐字一致：一次
  //      `BEGIN IMMEDIATE…COMMIT`）；
  //    · `true` → 并发到达的写操作凑成一批，一个事务一次提交（协议见
  //      `infra/sqlite/sqlite_group_commit.h`）。
  bool group_commit = true;
  int group_commit_max_wait_ms = 5;
  int group_commit_max_batch = 64;
  //  可观测/确定性接缝（测试用；生产为 null）。理由与语义见共用小工具的头注释。
  ISqliteBatchObserver* batch_observer = nullptr;
  ISqliteBatchGate* batch_gate = nullptr;
  ISqliteCommitFault* commit_fault = nullptr;
  //  可观测性（可选）：`fss_sqlite_{group_commits,ops}_total{repo="metadata"}`。
  fss::metrics::Registry* metrics = nullptr;
};

class SqliteMetadataRepository final : public domain::IMetadataRepository {
 public:
  static fss::Result<std::unique_ptr<SqliteMetadataRepository>> Open(
      const std::string& path, const fss::IClock& clock,
      SqliteMetadataRepositoryOptions options = {});
  ~SqliteMetadataRepository() override;

  fss::Result<domain::FileMetadataRecord> Create(std::string_view partition,
                                                 const domain::FileMetadataRecord& record) override;
  fss::Result<domain::MetadataClaim> ClaimForWrite(
      std::string_view partition, const domain::FileMetadataRecord& record) override;
  fss::Result<domain::FileMetadataRecord> MarkReady(
      std::string_view partition, std::string_view record_id, std::int64_t version,
      const domain::FileMetadataRecord& record) override;
  fss::Result<void> ReleaseClaim(std::string_view partition, std::string_view record_id,
                                 std::int64_t version) override;
  fss::Result<domain::FileMetadataRecord> GetById(std::string_view partition,
                                                  std::string_view record_id) override;
  fss::Result<domain::FileMetadataRecord> GetLatestByFileSource(
      std::string_view partition, std::string_view file_source) override;
  fss::Result<domain::FileMetadataRecord> Update(std::string_view partition,
                                                 const domain::FileMetadataRecord& record) override;
  fss::Result<void> Delete(std::string_view partition, std::string_view record_id) override;
  fss::Result<domain::MetadataPage> List(std::string_view partition,
                                         const domain::MetadataQuery& query) override;

  //  测试可观测性（与内存实现同名方法）：某个 record id 的版本数
  std::size_t VersionCount(std::string_view partition, std::string_view record_id);

  //  ★ 阶段 10（切片 4）诊断访问器：读回**本连接**上 `PRAGMA <name>` 的实际值。
  //    存在的唯一原因：`PRAGMA synchronous` **不落盘** —— 另开一个 sqlite3 连接读回得到
  //    的是该新连接自己的默认值（FULL = 2），无法证明本仓储真的下发了 OFF/NORMAL/FULL。
  //    `journal_mode` 会写进库文件头，用 `python3 -c "import sqlite3..."` 另开连接读回即可，
  //    不需要本访问器。白名单只放行 `synchronous`（PRAGMA 名无法参数化，白名单是防注入手段）。
  fss::Result<std::string> AppliedPragma(std::string_view name) const;

 private:
  SqliteMetadataRepository() = default;

  //  以下三个都在**已持锁**的前提下调用（自锁 = 死锁，P3-D10 的教训）
  fss::Result<domain::FileMetadataRecord> FindLatestById(sqlite3* db, std::string_view partition,
                                                         std::string_view record_id);
  fss::Result<domain::FileMetadataRecord> FindLatestBySource(sqlite3* db,
                                                             std::string_view partition,
                                                             std::string_view file_source);
  //  ★ C1：按幂等键取**活动**行（claiming 或 ready），并通过 `state` 回传它的状态。
  fss::Result<domain::FileMetadataRecord> FindClaimBySource(sqlite3* db, std::string_view partition,
                                                            std::string_view file_source,
                                                            domain::MetadataState* state);
  fss::Result<domain::FileMetadataRecord> ReadRow(sqlite3_stmt* stmt);

  //  ---- C1（ADR-009 §4.2）：原子领取 / claiming→ready / 放弃领取 ----
  //  ★ 与 Update/Delete 同构：`...Locked` 自己 `BEGIN IMMEDIATE…COMMIT`；`...InTransaction`
  //    由批协调器在**同一连接**的事务内调用（组提交开启时）。两种路径的 SQL 与语义完全一致。
  //    ★ SQLite 的写原子性来自 `BEGIN IMMEDIATE`（写锁串行化并发连接）：事务内**重新检查**
  //      幂等键后再插入，因此不需要 PG 那条单语句 `ON CONFLICT`（守卫语义等价）。
  fss::Result<domain::MetadataClaim> ClaimForWriteLocked(
      std::string_view partition, const domain::FileMetadataRecord& record);
  fss::Result<domain::MetadataClaim> ClaimForWriteInTransaction(
      sqlite3* db, std::string_view partition, const domain::FileMetadataRecord& record);
  fss::Result<domain::FileMetadataRecord> MarkReadyLocked(
      std::string_view partition, std::string_view record_id, std::int64_t version,
      const domain::FileMetadataRecord& record);
  fss::Result<domain::FileMetadataRecord> MarkReadyInTransaction(
      sqlite3* db, std::string_view partition, std::string_view record_id, std::int64_t version,
      const domain::FileMetadataRecord& record);
  fss::Result<void> ReleaseClaimLocked(std::string_view partition, std::string_view record_id,
                                       std::int64_t version);
  fss::Result<void> ReleaseClaimInTransaction(sqlite3* db, std::string_view partition,
                                              std::string_view record_id, std::int64_t version);

  fss::Result<domain::FileMetadataRecord> UpdateLocked(std::string_view partition,
                                                       const domain::FileMetadataRecord& record);
  fss::Result<domain::FileMetadataRecord> UpdateInTransaction(
      sqlite3* db, std::string_view partition, const domain::FileMetadataRecord& record);
  fss::Result<void> DeleteLocked(std::string_view partition, std::string_view record_id);
  fss::Result<void> DeleteInTransaction(sqlite3* db, std::string_view partition,
                                        std::string_view record_id);
  //  逐操作路径的观察者/指标通知（`group_commit=false` 时每个操作一批）。
  void NotifyBatch(std::size_t ops_in_batch, bool committed);

  sqlite3* db_ = nullptr;
  const fss::IClock* clock_ = nullptr;
  SqliteMetadataRepositoryOptions options_{};
  //  组提交协调器（`group_commit=true` 时创建）；持有一份 `mutex_` 的引用。
  std::unique_ptr<SqliteGroupCommitter> committer_;
  //  `mutable`：允许 const 诊断访问器 `AppliedPragma` 也走同一把锁（DB 连接不可并发访问）。
  mutable std::mutex mutex_;
};

}  // namespace fss::infra
