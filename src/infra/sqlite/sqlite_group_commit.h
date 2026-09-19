// =============================================================================
//  SqliteGroupCommitter（L2 共用小工具）—— SQLite 的**数据库层组提交**
// =============================================================================
//  为什么要有它（本切片的判据：`*.sqlite.{group_commit,group_commit_max_wait_ms,
//  group_commit_max_batch}` 必须真的生效）
//    `SqliteMetadataRepository` 与 `SqliteLocationRepository` 都是"单连接 + 互斥"：
//    每次写操作各自 `BEGIN IMMEDIATE; … COMMIT;`（元数据）或各自一条自动提交语句
//    （位置），**每个操作一次 fsync**。本工具把并发到达的写操作**凑成一批**，
//    用一个事务一次提交（一次 fsync），从而让三个配置键有真实的语义。
//
//  协议（父代理定案；语义不在这里发明，照做）
//    1. `group_commit=false` → 仓储走**逐操作**路径（与接线前逐字一致），本工具不参与。
//    2. `group_commit=true` → 第一个到达的写操作成为**领队**：它等待
//       「批满 `group_commit_max_batch`」**或**「从第一个待处理操作起超过
//       `group_commit_max_wait_ms`」，然后开**一个** `BEGIN IMMEDIATE`，把批内所有
//       操作一次 `COMMIT`。
//    3. **每操作原子性**（关键）：批内每个操作前 `SAVEPOINT op_i;`，成功 `RELEASE`；
//       失败 `ROLLBACK TO op_i; RELEASE op_i;` ⇒ **只有该操作失败，其余照常提交**，
//       失败操作不留半成品。
//    4. **整批提交失败**（`COMMIT` 遇到 `SQLITE_FULL`/磁盘错误，或测试注入）→
//       批内**所有**操作都返回该错误。这是**有意的语义**：`COMMIT` 失败意味着整批
//       都没落盘，谎报其中某个操作成功比失败更糟。
//    5. **读路径不许看到未提交数据**：单连接 ⇒ 领队在**批事务期间持有同一把连接互斥
//       锁**（`connection_mutex`），因此同一仓储的读操作最坏多等一个批窗口
//       （≤ `group_commit_max_wait_ms` + 批执行时间）。**这是组提交的代价，不是免费收益**，
//       已写进 `docs/operations.md` 与 `docs/02-design.md`。
//
//  可观测/确定性接缝（照 `PosixBlobStore` 的 `IBatchCommitObserver`/`IBatchArrivalGate`）
//    · `ISqliteBatchObserver`：每个批次完成时回调 `(ops_in_batch, committed?)`，
//      以及每次保存点回滚 —— 测试用它断言"提交次数 == ceil(N/B)"。
//    · `ISqliteBatchGate`：`ShouldFlush` 在领队等待期间被**反复咨询**，返回 true 即
//      立即 flush ⇒ "批必定填满"是**确定性**事实（不靠 sleep 猜时序）；
//      `BeforeCommit` 在批事务内、`COMMIT` 之前调用，测试可在此阻塞 ⇒ "批事务开着"
//      成为可观察的确定性状态（"读不脏"判据用它）。
//    · `ISqliteCommitFault`：测试注入"整批 COMMIT 失败"（否则这条语义无法用真实环境
//      构造 —— 本环境没有 `SQLITE_FULL` / 磁盘故障注入）。
//  三者都**不是 L3 端口**（没有领域语义、不跨层），生产默认不注入。
// =============================================================================
#pragma once

#include "common/metrics/metrics.h"
#include "common/result/result.h"

#include <sqlite3.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fss::infra {

//  批次观察者（测试用）。生产默认为 null → 不观察。
class ISqliteBatchObserver {
 public:
  virtual ~ISqliteBatchObserver() = default;
  //  一个批次结束时调用一次。`ops_in_batch` = 批内操作数；
  //  `committed` = 整批 `COMMIT` 是否成功（false 时批内所有操作都已返回同一错误）。
  virtual void OnBatchCommitted(std::size_t ops_in_batch, bool committed) = 0;
  //  每个失败的写操作触发一次（即一次 `ROLLBACK TO SAVEPOINT`）。
  virtual void OnSavepointRollback() = 0;
};

//  批门控（测试用）。生产默认为 null → 只按"批满 / 窗口到期"决定 flush。
class ISqliteBatchGate {
 public:
  virtual ~ISqliteBatchGate() = default;
  //  领队在等待"批满或窗口到期"期间被反复咨询：返回 true → **立即** flush。
  //  测试实现成 `ops_in_batch >= N` 时返回 true ⇒ 批必定填满（确定性，不用 sleep）。
  virtual bool ShouldFlush(std::size_t ops_in_batch) = 0;
  //  领队在批事务**内**、`COMMIT` **之前**调用（默认 no-op）。测试可在此阻塞，
  //  把"批事务开着"变成可观察的确定性状态 —— "读不脏"判据（读必须等待）用它。
  virtual void BeforeCommit(std::size_t ops_in_batch) { (void)ops_in_batch; }
};

//  提交故障注入（测试用）：让整批 `COMMIT` 失败，验证"整批失败 → 所有操作返回该错误"。
class ISqliteCommitFault {
 public:
  virtual ~ISqliteCommitFault() = default;
  virtual bool FailCommit() = 0;
};

//  三个键的取值 + 可观测接缝。默认值 = `core_schema.cpp` / `config/fss.example.json`
//  的默认值（`group_commit=true` / `5ms` / `64`）—— 组合根逐键传入。
struct SqliteGroupCommitOptions {
  bool group_commit = true;
  int group_commit_max_wait_ms = 5;
  int group_commit_max_batch = 64;
  ISqliteBatchObserver* batch_observer = nullptr;
  ISqliteBatchGate* batch_gate = nullptr;
  ISqliteCommitFault* commit_fault = nullptr;
  //  可观测性（可选）：`fss_sqlite_{group_commits,ops}_total{repo=...}`。null → 不记。
  fss::metrics::Registry* metrics = nullptr;
  std::string metrics_repo;
};

class SqliteGroupCommitter {
 public:
  //  `connection_mutex` 必须是**该连接**的互斥锁（两个仓储各自的 `mutex_`）：
  //  批事务期间领队一直持有它，读操作因此在同一把锁上排队（读不到未提交数据）。
  SqliteGroupCommitter(sqlite3* db, std::mutex& connection_mutex,
                       SqliteGroupCommitOptions options)
      : db_(db), connection_mutex_(connection_mutex), options_(options) {
    if (options_.group_commit_max_batch <= 0) options_.group_commit_max_batch = 1;
    if (options_.group_commit_max_wait_ms < 0) options_.group_commit_max_wait_ms = 0;
  }

  SqliteGroupCommitter(const SqliteGroupCommitter&) = delete;
  SqliteGroupCommitter& operator=(const SqliteGroupCommitter&) = delete;

  //  提交一个写操作（`run` 在批事务内执行，可能由**领队线程**调用）。
  //  返回值是该操作的最终结果：自己的成败，或（整批 COMMIT 失败时的）批错误。
  //  ⚠️ `run` 只能执行**语句级**逻辑，**不得**自己 BEGIN/COMMIT/ROLLBACK
  //     （事务与保存点由本工具负责）。
  template <typename T>
  fss::Result<T> Submit(std::function<fss::Result<T>(sqlite3*)> run) {
    //  结果槽：类型擦除后由执行线程写入，调用线程在批完成后读取（同一把锁 / cv 建
    //  立 happens-before）。初值 = "未执行"，正常路径一定会被覆盖。
    auto result = std::make_shared<fss::Result<T>>(
        fss::Err(fss::ErrorKind::kInternal, "组提交：操作未执行（内部错误）"));
    PendingOp op;
    op.run = [run = std::move(run), result, db = db_]() mutable {
      *result = run(db);
      return result->ok();
    };
    op.poison = [result](const fss::Error& error) { *result = error; };
    Enqueue(std::move(op));
    return std::move(*result);
  }

 private:
  struct PendingOp {
    //  执行该操作；返回 true = 该操作自身成功（→ RELEASE 保存点）。
    std::function<bool()> run;
    //  整批 COMMIT 失败时把该操作的结果改写成批错误。
    std::function<void(const fss::Error&)> poison;
  };

  struct Batch {
    std::vector<PendingOp> ops;
    std::chrono::steady_clock::time_point deadline;
    bool sealed = false;  //  已封批（不再收新操作）
    bool done = false;    //  提交完成，跟随着可以返回
  };

  static bool ExecSql(sqlite3* db, const std::string& sql, std::string* error) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
      if (error != nullptr) {
        *error = message != nullptr ? std::string(message) : std::string(sqlite3_errstr(rc));
      }
      if (message != nullptr) sqlite3_free(message);
      return false;
    }
    if (message != nullptr) sqlite3_free(message);
    return true;
  }

  void Enqueue(PendingOp op) {
    std::unique_lock<std::mutex> lock(batch_mutex_);
    bool leader = false;
    if (pending_ == nullptr || pending_->sealed) {
      pending_ = std::make_shared<Batch>();
      //  窗口从**第一个待处理操作**起算（不是从"最后一次入批"）。
      pending_->deadline =
          std::chrono::steady_clock::now() +
          std::chrono::milliseconds(options_.group_commit_max_wait_ms);
      leader = true;
    }
    std::shared_ptr<Batch> batch = pending_;
    batch->ops.push_back(std::move(op));
    if (batch->ops.size() >= static_cast<std::size_t>(options_.group_commit_max_batch)) {
      batch->sealed = true;  //  批满：不再收新操作（批**不会**超过上限）
    }
    batch_cv_.notify_all();

    if (!leader) {
      //  跟随着：等领队提交完，读自己的结果槽。
      batch_cv_.wait(lock, [&] { return batch->done; });
      return;
    }

    //  领队：等"批满"或"窗口到期"（或门控放行）。
    while (!batch->sealed) {
      if (options_.batch_gate != nullptr &&
          options_.batch_gate->ShouldFlush(batch->ops.size())) {
        break;
      }
      //  ★ 超时必须 break：`wait_until` 到期返回 `timeout`，此时 deadline 已过，
      //    不 break 会变成"永远等下去"（或用已过期的 deadline 忙循环）。
      if (batch_cv_.wait_until(lock, batch->deadline) == std::cv_status::timeout) break;
    }
    batch->sealed = true;
    if (pending_ == batch) pending_.reset();  //  新写者可以开始凑下一批
    std::vector<PendingOp> ops;
    ops.swap(batch->ops);
    lock.unlock();  //  ★ 提交期间不持 `batch_mutex_`（否则新写者无法入下一批）

    ExecuteBatch(ops);

    lock.lock();
    batch->done = true;
    batch_cv_.notify_all();
  }

  void ExecuteBatch(const std::vector<PendingOp>& ops) {
    //  ★ 批事务期间持有**连接互斥**：同一连接上的读都在这里排队 ⇒ 读不到未提交数据。
    std::lock_guard<std::mutex> connection_lock(connection_mutex_);
    std::string error;
    if (!ExecSql(db_, "BEGIN IMMEDIATE;", &error)) {
      const fss::Error begin_error =
          fss::Err(fss::ErrorKind::kInternal, "组提交：开启事务失败：" + error);
      for (const auto& op : ops) op.poison(begin_error);
      Notify(ops.size(), false);
      return;
    }

    for (std::size_t i = 0; i < ops.size(); ++i) {
      const std::string savepoint = "fss_gc_op_" + std::to_string(i);
      std::string ignored;
      ExecSql(db_, "SAVEPOINT " + savepoint + ";", &ignored);
      const bool op_ok = ops[i].run();
      if (op_ok) {
        ExecSql(db_, "RELEASE " + savepoint + ";", &ignored);
      } else {
        //  ★ 每操作原子性：只回滚**这一个**操作；同批其它操作照常提交。
        ExecSql(db_, "ROLLBACK TO " + savepoint + ";", &ignored);
        ExecSql(db_, "RELEASE " + savepoint + ";", &ignored);
        if (options_.batch_observer != nullptr) options_.batch_observer->OnSavepointRollback();
      }
    }

    //  测试门控：批事务**开着**、COMMIT 之前的确定性可观察点（"读不脏"判据用）。
    if (options_.batch_gate != nullptr) options_.batch_gate->BeforeCommit(ops.size());

    bool committed = false;
    if (options_.commit_fault != nullptr && options_.commit_fault->FailCommit()) {
      std::string ignored;
      ExecSql(db_, "ROLLBACK;", &ignored);
      const fss::Error commit_error =
          fss::Err(fss::ErrorKind::kInternal, "组提交：COMMIT 失败（故障注入）");
      for (const auto& op : ops) op.poison(commit_error);
    } else if (!ExecSql(db_, "COMMIT;", &error)) {
      std::string ignored;
      ExecSql(db_, "ROLLBACK;", &ignored);
      //  ★ 有意的语义：整批 COMMIT 失败（SQLITE_FULL / 磁盘错误）→ 批内**所有**操作
      //    都返回该错误。谎报"其中某个操作成功"比整批失败更糟。
      const fss::Error commit_error =
          fss::Err(fss::ErrorKind::kInternal, "组提交：整批 COMMIT 失败：" + error);
      for (const auto& op : ops) op.poison(commit_error);
    } else {
      committed = true;
    }
    Notify(ops.size(), committed);
  }

  void Notify(std::size_t ops_in_batch, bool committed) {
    if (options_.batch_observer != nullptr) {
      options_.batch_observer->OnBatchCommitted(ops_in_batch, committed);
    }
    if (options_.metrics != nullptr) {
      const fss::metrics::Labels labels{{"repo", options_.metrics_repo}};
      //  批次数按"尝试提交的批"计（与 POSIX 的 `fss_posix_group_commits_total` 同口径）：
      //  `fss_sqlite_ops_total / fss_sqlite_group_commits_total` = 平均批大小。
      options_.metrics->Increment("fss_sqlite_group_commits_total", labels);
      options_.metrics->Increment("fss_sqlite_ops_total", labels,
                                  static_cast<std::int64_t>(ops_in_batch));
    }
  }

  sqlite3* db_ = nullptr;
  std::mutex& connection_mutex_;
  SqliteGroupCommitOptions options_;

  //  批状态（`batch_mutex_` 只保护"凑批"，不保护连接）。
  std::mutex batch_mutex_;
  std::condition_variable batch_cv_;
  std::shared_ptr<Batch> pending_;
};

}  // namespace fss::infra
