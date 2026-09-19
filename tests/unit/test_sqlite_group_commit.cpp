// =============================================================================
//  C10.20：SQLite 组提交协调器（`infra/sqlite/sqlite_group_commit.h`）的**单元**判据
// =============================================================================
//  为什么单独测协调器（而不是只测两个仓储）：
//    1. **每操作原子性**的最强证据需要"一个操作先写成功、再在同一步失败"的场景 ——
//       用一张最小的真实表 + 真实主键冲突即可构造（"失败的 INSERT 不留半行"）。
//    2. **整批 COMMIT 失败 → 批内所有操作返回该错误** 这条语义在真实环境里无法构造
//       （本环境没有 `SQLITE_FULL` / 磁盘故障注入）→ 用 `ISqliteCommitFault` 接缝。
//    3. 仓储层的组提交判据（摊销 / 读不脏 / max_wait）在
//       `tests/integration/test_sqlite_group_commit.cpp`。
//
//  ★ 判据都能失败（R1）：删掉 SAVEPOINT → 用例 ① 必失败；不 poison 整批 → 用例 ② 必失败。
// =============================================================================
#include <catch2/catch.hpp>

#include "infra/sqlite/sqlite_group_commit.h"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using fss::infra::ISqliteBatchGate;
using fss::infra::ISqliteBatchObserver;
using fss::infra::ISqliteCommitFault;
using fss::infra::SqliteGroupCommitOptions;
using fss::infra::SqliteGroupCommitter;

namespace {

//  记录每批 `(ops_in_batch, committed?)` 与保存点回滚次数。
class RecordingObserver final : public ISqliteBatchObserver {
 public:
  void OnBatchCommitted(std::size_t ops_in_batch, bool committed) override {
    std::lock_guard<std::mutex> lock(mu_);
    batches_.emplace_back(ops_in_batch, committed);
  }
  void OnSavepointRollback() override {
    std::lock_guard<std::mutex> lock(mu_);
    ++rollbacks_;
  }

  std::vector<std::pair<std::size_t, bool>> Batches() const {
    std::lock_guard<std::mutex> lock(mu_);
    return batches_;
  }
  std::size_t OpsIn(size_t index) const {
    std::lock_guard<std::mutex> lock(mu_);
    return batches_.at(index).first;
  }
  bool CommittedIn(size_t index) const {
    std::lock_guard<std::mutex> lock(mu_);
    return batches_.at(index).second;
  }
  std::size_t CommitCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t n = 0;
    for (const auto& batch : batches_) {
      if (batch.second) ++n;
    }
    return n;
  }
  std::size_t Rollbacks() const {
    std::lock_guard<std::mutex> lock(mu_);
    return rollbacks_;
  }

 private:
  mutable std::mutex mu_;
  std::vector<std::pair<std::size_t, bool>> batches_;
  std::size_t rollbacks_ = 0;
};

//  门控：批内操作数达到 `expected` 才允许 flush ⇒ "批必定填满"是**确定性**事实
//  （不靠 sleep 猜时序；领队仍受 `max_wait_ms` 兜底）。
class CountGate final : public ISqliteBatchGate {
 public:
  CountGate(std::size_t expected, std::size_t* before_commit_calls = nullptr)
      : expected_(expected), before_commit_calls_(before_commit_calls) {}
  bool ShouldFlush(std::size_t ops_in_batch) override { return ops_in_batch >= expected_; }
  void BeforeCommit(std::size_t ops_in_batch) override {
    if (before_commit_calls_ != nullptr) ++(*before_commit_calls_);
    last_before_commit_ops_ = ops_in_batch;
  }
  std::size_t last_before_commit_ops() const { return last_before_commit_ops_; }

 private:
  std::size_t expected_;
  std::size_t* before_commit_calls_ = nullptr;
  std::size_t last_before_commit_ops_ = 0;
};

//  提交故障注入：让整批 `COMMIT` 失败（真实 SQLITE_FULL 无法在本环境构造）。
class CommitFault final : public ISqliteCommitFault {
 public:
  bool FailCommit() override { return true; }
};

fss::Result<void> Exec(sqlite3* db, const std::string& sql) {
  char* error = nullptr;
  const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
  if (rc != SQLITE_OK) {
    const std::string message = error != nullptr ? std::string(error) : std::string("sqlite 错误");
    if (error != nullptr) sqlite3_free(error);
    return fss::Err(fss::ErrorKind::kInternal, message);
  }
  if (error != nullptr) sqlite3_free(error);
  return fss::Ok();
}

//  一个最小真实表：`t(id INTEGER PRIMARY KEY, tag TEXT)`（主键冲突 = 真实约束冲突）。
struct Fixture {
  sqlite3* db = nullptr;
  std::mutex connection_mutex;

  Fixture() {
    REQUIRE(sqlite3_open_v2(":memory:", &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            nullptr) == SQLITE_OK);
    REQUIRE(Exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, tag TEXT)").ok());
  }
  ~Fixture() {
    if (db != nullptr) sqlite3_close(db);
  }

  std::vector<long long> Ids() {
    std::vector<long long> ids;
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "SELECT id FROM t ORDER BY id", -1, &stmt, nullptr) ==
            SQLITE_OK);
    while (sqlite3_step(stmt) == SQLITE_ROW) ids.push_back(sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt);
    return ids;
  }
};

}  // namespace

TEST_CASE("★ C10.20 每操作原子性：同批里一个操作失败 → 只有它回滚，其余照常提交，且不留半行",
          "[phase10][sqlite][c10.20]") {
  Fixture fx;
  RecordingObserver observer;
  SqliteGroupCommitOptions options;
  options.group_commit = true;
  options.group_commit_max_wait_ms = 5000;  // 门控负责 flush；窗口只作兜底
  options.group_commit_max_batch = 64;
  options.batch_observer = &observer;
  CountGate gate(3);
  options.batch_gate = &gate;
  SqliteGroupCommitter committer(fx.db, fx.connection_mutex, options);

  //  三个并发操作凑成**同一批**（门控：批内 3 个才放行）：
  //    ① 插入 (1,'a')                 → 成功
  //    ② 插入 (2,'b') 然后插入 (2,'x') → 第二步**自己**主键冲突 → **该操作失败**
  //    ③ 插入 (3,'c')                 → 成功
  //  ② 的"先插入 (2,'b')"是**真实的半成品**：没有 SAVEPOINT 回滚它就会留在库里。
  //  ★ 冲突必须是「操作 ② 自己内部」的（而不是与 ① 争同一个 id）：批内操作的**执行顺序
  //    是到达顺序**（并发下不确定）；用「① 与 ② 争同一个 id」会让判据随调度翻转
  //    （实测：② 先跑时它的第二次插入反而成功，① 才失败 —— 27/200 次翻转）。
  fss::Result<void> r1 = fss::Ok();
  fss::Result<void> r2 = fss::Ok();
  fss::Result<void> r3 = fss::Ok();
  std::vector<std::thread> threads;
  threads.emplace_back([&] { r1 = committer.Submit<void>([](sqlite3* db) { return Exec(db, "INSERT INTO t VALUES (1,'a')"); }); });
  threads.emplace_back([&] {
    r2 = committer.Submit<void>([](sqlite3* db) -> fss::Result<void> {
      FSS_TRY(Exec(db, "INSERT INTO t VALUES (2,'b')"));
      FSS_TRY(Exec(db, "INSERT INTO t VALUES (2,'x')"));  // 主键冲突（真实约束，与到达顺序无关）
      return fss::Ok();
    });
  });
  threads.emplace_back([&] { r3 = committer.Submit<void>([](sqlite3* db) { return Exec(db, "INSERT INTO t VALUES (3,'c')"); }); });
  for (auto& thread : threads) thread.join();

  //  ① 失败操作必须返回错误（不是整批成功、也不是整批失败）。
  //  INFO 的作用域是**所在块**：必须与 REQUIRE 同块，否则断言失败时不打印原因。
  if (!r1.ok()) {
    INFO("r1 意外失败：" << r1.error().message());
    REQUIRE(r1.ok());
  }
  if (r2.ok()) {
    INFO("r2 本应失败（主键冲突）却成功了");
    REQUIRE_FALSE(r2.ok());
  }
  if (!r3.ok()) {
    INFO("r3 意外失败：" << r3.error().message());
    REQUIRE(r3.ok());
  }

  //  ②③ 只有 id {1,3}：**失败的 ② 连它先插入的 (2,'b') 都没有留下**。
  const auto ids = fx.Ids();
  REQUIRE(ids == std::vector<long long>{1, 3});

  //  ④ 观察者：1 个批次（3 个操作，整批 COMMIT 成功）+ **1 次**保存点回滚。
  REQUIRE(observer.Batches().size() == 1);
  REQUIRE(observer.OpsIn(0) == 3);
  REQUIRE(observer.CommittedIn(0));
  REQUIRE(observer.Rollbacks() == 1);
  //  门控的 `BeforeCommit` 在批事务内被调用（"读不脏"判据依赖这个时点）。
  REQUIRE(gate.last_before_commit_ops() == 3);
}

TEST_CASE("★ C10.20 整批 COMMIT 失败 → 批内**所有**操作都返回该错误（有意的语义）",
          "[phase10][sqlite][c10.20]") {
  Fixture fx;
  RecordingObserver observer;
  CommitFault fault;
  SqliteGroupCommitOptions options;
  options.group_commit = true;
  options.group_commit_max_wait_ms = 5000;
  options.group_commit_max_batch = 64;
  options.batch_observer = &observer;
  options.commit_fault = &fault;
  CountGate gate(3);
  options.batch_gate = &gate;
  SqliteGroupCommitter committer(fx.db, fx.connection_mutex, options);

  std::vector<fss::Result<void>> results(3);
  std::vector<std::thread> threads;
  for (int i = 0; i < 3; ++i) {
    threads.emplace_back([&, i] {
      const std::string sql = "INSERT INTO t VALUES (" + std::to_string(i + 1) + ",'x')";
      results[static_cast<std::size_t>(i)] =
          committer.Submit<void>([sql](sqlite3* db) { return Exec(db, sql); });
    });
  }
  for (auto& thread : threads) thread.join();

  for (const auto& result : results) {
    REQUIRE_FALSE(result.ok());  //  整批失败：一个都不许谎报成功
    REQUIRE(result.error().kind() == fss::ErrorKind::kInternal);
  }
  REQUIRE(fx.Ids().empty());  //  批事务已回滚：一行都不留
  REQUIRE(observer.Batches().size() == 1);
  REQUIRE(observer.OpsIn(0) == 3);
  REQUIRE_FALSE(observer.CommittedIn(0));
  REQUIRE(observer.Rollbacks() == 0);
}

TEST_CASE("★ C10.20 门控是 flush 时机的确定性来源（不靠 sleep）",
          "[phase10][sqlite][c10.20]") {
  Fixture fx;
  RecordingObserver observer;
  SqliteGroupCommitOptions options;
  options.group_commit = true;
  options.group_commit_max_wait_ms = 5000;
  options.group_commit_max_batch = 64;  // 上限远大于 2 ⇒ 只能由门控决定"何时 flush"
  options.batch_observer = &observer;
  CountGate gate(2);
  options.batch_gate = &gate;
  SqliteGroupCommitter committer(fx.db, fx.connection_mutex, options);

  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 2; ++i) {
    threads.emplace_back([&, i] {
      const std::string sql = "INSERT INTO t VALUES (" + std::to_string(i + 1) + ",'x')";
      const auto submitted = committer.Submit<void>(
          [sql](sqlite3* db) { return Exec(db, sql); });
      if (!submitted.ok()) ++failures;
    });
  }
  for (auto& thread : threads) thread.join();
  REQUIRE(failures.load() == 0);

  //  2 个操作被合并成**一个**批次（若门控不生效，领队会等满 5s 窗口 —— 结果仍是 1 批，
  //  但下面的"批内 2 个"断言会失败：先到的那个会被单独 flush）。
  REQUIRE(observer.Batches().size() == 1);
  REQUIRE(observer.OpsIn(0) == 2);
  REQUIRE(observer.CommitCount() == 1);
  REQUIRE(fx.Ids() == std::vector<long long>{1, 2});
}
