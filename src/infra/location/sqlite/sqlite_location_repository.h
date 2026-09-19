// =============================================================================
//  SqliteLocationRepository（L2）—— 单实例的位置记录仓储（ADR-004）
// =============================================================================
//  ★ 分区隔离是 schema 级的
//    `PRIMARY KEY (partition_id, file_id)`：**不是** `file_id` 单列主键。
//    `docs/02-design.md` §9.1 初版 SQL 把 `file_id` 写成 PRIMARY KEY —— 那会让
//    两个租户无法拥有同名 fileID，与契约级的"分区隔离"直接矛盾。这里按
//    "每个方法都带 partition" 的端口契约修正（设计文档已同步）。
//
//  ★ 两套表示
//    · 抽出列（partition_id/file_source/zone/created_by/created_at/…）用于**索引与查询**；
//    · `data` 列存**完整 JSON**（前向兼容：不认识的字段原样保留，读-改-写不丢数据）。
//    这与 ADR-004 借鉴的 OSDU bareKV 模型一致。
//
//  ★ 并发
//    `journal_mode=WAL` + `busy_timeout` + **单连接互斥**：多线程调用者安全排队，
//    不会因为并发写而拿到 `SQLITE_BUSY`（C3.12 的"有界写并发"）。
//    `max_write_concurrency`（默认 8，来自实测：8 线程为吞吐峰值，32 线程反而下降）
//    是写入并发的**上限**；当前实现的实际并发是 1（单连接），因此必然 ≤ 上限。
//    将来引入连接池时该值才会真正起作用。
//    多进程共享状态请改用 PostgreSQL（ADR-009：SQLite 不支持在 NFS 上并发写）。
#pragma once

#include "common/result/result.h"
#include "domain/ports/ports.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

//  ★ 必须在**全局命名空间**前向声明：写在 `namespace fss::infra` 里会声明出
//    `fss::infra::sqlite3`，与 sqlite3.h 的全局 `::sqlite3` 不是同一个类型
//    （报错是"cannot convert fss::infra::sqlite3_stmt* to sqlite3_stmt*"，很难一眼看出）。
//    同理，成员签名里也不能写 `struct sqlite3_stmt*` —— 那会**就地**声明一个命名空间内的
//    新类型。要么用 `::sqlite3_stmt*`，要么像这里一样先在全局声明。
struct sqlite3;
struct sqlite3_stmt;

namespace fss::metrics {
class Registry;
}  // namespace fss::metrics

namespace fss::infra {

//  ★ 组提交的接缝/协调器声明在 `infra/sqlite/sqlite_group_commit.h`（L2 共用小工具）。
class ISqliteBatchObserver;
class ISqliteBatchGate;
class ISqliteCommitFault;
class SqliteGroupCommitter;

struct SqliteLocationRepositoryOptions {
  int busy_timeout_millis = 5000;
  bool wal = true;
  //  写入并发的**上限**（`docs/02-design.md` §13.4：实测 8 线程为峰值，32 线程反而下降）。
  //  当前实现是"单连接 + 互斥"（实际并发 1 ≤ 上限），因此不会出现 `SQLITE_BUSY`；
  //  将来的连接池可以真正用起这个值。必须 > 0。
  int max_write_concurrency = 8;
  //  ★ 阶段 10（切片 4）：`PRAGMA synchronous` 的取值（0 = OFF / 1 = NORMAL / 2 = FULL）。
  //    ⚠ 与 `journal_mode` 不同，该 PRAGMA **不随库文件持久化**：另开一个 sqlite3 连接
  //    读回的是新连接的默认值（FULL = 2），不能用来证明生效。可观测入口是
  //    `AppliedPragma("synchronous")`（在**本连接**上读回）。放在结构体**最后**，避免
  //    既有的聚合初始化按位置取值时静默错位。
  int synchronous_level = 1;
  //  ★ 本切片：数据库层组提交（`location.sqlite.{group_commit,group_commit_max_wait_ms,
  //    group_commit_max_batch}`）。默认值 = `core_schema.cpp` / `config/fss.example.json`
  //    的默认值（`true` / `5` / `64`）。协议见 `infra/sqlite/sqlite_group_commit.h`：
  //    `false` → 逐操作提交（与接线前逐字一致的单条自动提交语句）；
  //    `true`  → 并发写操作凑成一批，一个事务一次提交。
  bool group_commit = true;
  int group_commit_max_wait_ms = 5;
  int group_commit_max_batch = 64;
  //  可观测/确定性接缝（测试用；生产为 null）。
  ISqliteBatchObserver* batch_observer = nullptr;
  ISqliteBatchGate* batch_gate = nullptr;
  ISqliteCommitFault* commit_fault = nullptr;
  //  可观测性（可选）：`fss_sqlite_{group_commits,ops}_total{repo="location"}`。
  fss::metrics::Registry* metrics = nullptr;
};

class SqliteLocationRepository final : public domain::IFileLocationRepository {
 public:
  //  `path` 为数据库文件；":memory:" 表示内存库（测试用）。
  static fss::Result<std::unique_ptr<SqliteLocationRepository>> Open(
      const std::string& path, SqliteLocationRepositoryOptions options = {});

  ~SqliteLocationRepository() override;
  SqliteLocationRepository(const SqliteLocationRepository&) = delete;
  SqliteLocationRepository& operator=(const SqliteLocationRepository&) = delete;

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

  //  ★ 阶段 10（切片 4）诊断访问器：读回**本连接**上 `PRAGMA <name>` 的实际值。
  //    存在的唯一原因：`PRAGMA synchronous` **不落盘**，另开一个 sqlite3 连接读回得到的是
  //    该新连接自己的默认值（FULL = 2），与仓储是否真的下发了 OFF/NORMAL/FULL 无关。
  //    白名单只放行 `synchronous`（PRAGMA 名无法参数化，白名单是防注入手段）。
  fss::Result<std::string> AppliedPragma(std::string_view name) const;

 private:
  SqliteLocationRepository() = default;
  fss::Result<void> EnsureSchema();
  fss::Result<domain::FileLocation> LoadRow(::sqlite3_stmt* statement) const;
  //  不加锁的查询（供已持锁的方法内部复用，避免同一把非递归锁自锁）
  fss::Result<domain::FileLocation> FindInternal(std::string_view partition,
                                                 std::string_view file_id);

  //  ---- 本切片：逐操作路径（`group_commit=false`，与接线前逐字一致）与
  //       批内路径（`group_commit=true`，由批协调器在事务/保存点内调用） ----
  fss::Result<void> SaveLocked(std::string_view partition, const domain::FileLocation& location);
  fss::Result<void> SaveInTransaction(::sqlite3* db, std::string_view partition,
                                      const domain::FileLocation& location);
  fss::Result<void> UpdateSignedUrlLocked(std::string_view partition, std::string_view file_id,
                                          std::string_view signed_url,
                                          std::int64_t updated_at_epoch_seconds);
  fss::Result<void> UpdateSignedUrlInTransaction(::sqlite3* db, std::string_view partition,
                                                 std::string_view file_id,
                                                 std::string_view signed_url,
                                                 std::int64_t updated_at_epoch_seconds);
  fss::Result<void> DeleteLocked(std::string_view partition, std::string_view file_id);
  fss::Result<void> DeleteInTransaction(::sqlite3* db, std::string_view partition,
                                        std::string_view file_id);
  //  逐操作路径的观察者/指标通知（`group_commit=false` 时每个操作一批）。
  void NotifyBatch(std::size_t ops_in_batch, bool committed);

  sqlite3* db_ = nullptr;
  SqliteLocationRepositoryOptions options_{};
  //  组提交协调器（`group_commit=true` 时使用）；持有一份 `mutex_` 的引用。
  std::unique_ptr<SqliteGroupCommitter> committer_;
  //  `mutable`：允许 const 诊断访问器 `AppliedPragma` 也走同一把锁（DB 连接不可并发访问）。
  mutable std::mutex mutex_;  // 单连接串行化：让并发调用者排队（C3.12）
};

}  // namespace fss::infra
