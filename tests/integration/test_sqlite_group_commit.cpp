// =============================================================================
//  C10.20：SQLite **数据库层组提交**（两个仓储的判据）
// =============================================================================
//  判据（父代理定案）：
//    1. **摊销（确定性）**：同一仓储上并发 N 个写操作、批上限 B（用门控保证批填满）
//       → 提交次数 == ceil(N/B)；`B=1` → == N；`group_commit=false` → == N 且每操作
//       一个事务（逐字回归）。
//    2. **每操作原子性**：批里一个操作必然失败（用**真实的约束冲突**：位置仓储的
//       `(partition, file_source)` 唯一索引）→ ① 该操作返回错误；② 同批其它操作
//       全部成功且可读；③ 失败操作没有留下任何行；④ 观察者看到 **1 次**保存点回滚。
//    3. **`max_wait_ms` 真的生效**：只发 1 个写操作（批永远不满）→ 在 `max_wait_ms`
//       量级内提交；`0` 与较大值各一次，断言"等待窗口"的行为差异（都有界、不挂死）。
//    4. **读不脏**：批事务开着时发起读 → 读**等待**批结束，返回批提交后的值。
//    5. 真实进程侧的可观测性见 `tests/integration/test_config_wiring.cpp` 的 C10.20。
//  ★ 全部时序都由注入的门控/观察者决定，**不用 sleep 猜批大小**。
// =============================================================================
#include <catch2/catch.hpp>

#include "port_contract.h"
#include "temp_dir.h"

#include "common/time/clock.h"

#include "domain/ports/ports.h"
#include "infra/location/sqlite/sqlite_location_repository.h"
#include "infra/metadata/sqlite/sqlite_metadata_repository.h"
#include "infra/sqlite/sqlite_group_commit.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using fss::domain::FileLocation;
using fss::domain::FileMetadataRecord;
using fss::infra::ISqliteBatchGate;
using fss::infra::ISqliteBatchObserver;
using fss::infra::SqliteLocationRepository;
using fss::infra::SqliteLocationRepositoryOptions;
using fss::infra::SqliteMetadataRepository;
using fss::infra::SqliteMetadataRepositoryOptions;

namespace {

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
  std::size_t BatchCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return batches_.size();
  }
  std::size_t CommitCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t n = 0;
    for (const auto& batch : batches_) {
      if (batch.second) ++n;
    }
    return n;
  }
  std::vector<std::size_t> Ops() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::size_t> out;
    for (const auto& batch : batches_) out.push_back(batch.first);
    return out;
  }
  bool AllCommitted() const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& batch : batches_) {
      if (!batch.second) return false;
    }
    return !batches_.empty();
  }
  std::size_t Rollbacks() const {
    std::lock_guard<std::mutex> lock(mu_);
    return rollbacks_;
  }
  //  只用于"把准备步骤（如 seed 写入）从判据里排除掉"—— 判据只统计被测的那一批。
  void Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    batches_.clear();
    rollbacks_ = 0;
  }

 private:
  mutable std::mutex mu_;
  std::vector<std::pair<std::size_t, bool>> batches_;
  std::size_t rollbacks_ = 0;
};

//  门控之一：批内操作数达到 `expected` 即放行 ⇒ "批必定填满"是**确定性**事实。
class CountGate final : public ISqliteBatchGate {
 public:
  explicit CountGate(std::size_t expected) : expected_(expected) {}
  bool ShouldFlush(std::size_t ops_in_batch) override { return ops_in_batch >= expected_; }

 private:
  std::size_t expected_;
};

//  门控之二：**永不**因"批满"放行（模拟"批永远不满"）—— 只能靠 `max_wait_ms` 兜底。
class NeverFlushGate final : public ISqliteBatchGate {
 public:
  bool ShouldFlush(std::size_t ops_in_batch) override {
    (void)ops_in_batch;
    return false;
  }
};

//  门控之三：立刻放行，并在**批事务内、COMMIT 之前**阻塞 —— 把"事务开着"变成
//  可观察、可控的确定性状态（"读不脏"判据用它）。
class BlockingCommitGate final : public ISqliteBatchGate {
 public:
  bool ShouldFlush(std::size_t ops_in_batch) override {
    (void)ops_in_batch;
    return true;
  }
  void BeforeCommit(std::size_t ops_in_batch) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      tx_open_ = true;
      ops_in_transaction_ = ops_in_batch;
    }
    cv_.notify_all();
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [&] { return released_; });
  }
  bool WaitUntilOpen(int timeout_ms = 5000) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return tx_open_; });
  }
  void Release() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      released_ = true;
    }
    cv_.notify_all();
  }
  std::size_t ops_in_transaction() const { return ops_in_transaction_; }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool tx_open_ = false;
  bool released_ = false;
  std::size_t ops_in_transaction_ = 0;
};

struct LocationHarness {
  fss::test::TempDir dir{"sqlite_group_commit_loc"};
  RecordingObserver observer;
  std::unique_ptr<SqliteLocationRepository> repo;

  LocationHarness(bool group_commit, int max_wait_ms, int max_batch, ISqliteBatchGate* gate,
                  const std::string& db_name = "locations.db") {
    SqliteLocationRepositoryOptions options;
    options.group_commit = group_commit;
    options.group_commit_max_wait_ms = max_wait_ms;
    options.group_commit_max_batch = max_batch;
    options.batch_observer = &observer;
    options.batch_gate = gate;
    auto opened = SqliteLocationRepository::Open(dir.child(db_name), options);
    REQUIRE(opened.ok());
    repo = std::move(opened.value());
  }
};

//  并发发起 N 个 `Save`（全部应成功；观察者会记录批次）。
void ConcurrentSaves(SqliteLocationRepository& repo, int n) {
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < n; ++i) {
    threads.emplace_back([&repo, &failures, i] {
      const std::string id = "f-" + std::to_string(i);
      if (!repo.Save("opendes", fss::test::MakeLocation(id, "/u/" + id)).ok()) ++failures;
    });
  }
  for (auto& thread : threads) thread.join();
  REQUIRE(failures.load() == 0);
}

}  // namespace

// =============================================================================
//  判据 1：摊销 == ceil(N/B)（确定性；门控让批必定填满）
// =============================================================================
TEST_CASE("★ C10.20 摊销：并发 N=8、批上限 B → 提交次数 == ceil(N/B)（位置仓储）",
          "[phase10][integration][sqlite][c10.20]") {
  SECTION("B=4 → 2 次提交，每批 4 个操作") {
    CountGate gate(4);
    LocationHarness harness(/*group_commit=*/true, /*max_wait_ms=*/2000, /*max_batch=*/4, &gate);
    ConcurrentSaves(*harness.repo, 8);
    REQUIRE(harness.observer.BatchCount() == 2);         // ceil(8/4) == 2
    REQUIRE(harness.observer.CommitCount() == 2);
    REQUIRE(harness.observer.Ops() == std::vector<std::size_t>{4, 4});
    REQUIRE(harness.observer.AllCommitted());
  }

  SECTION("B=2 → 4 次提交；B=8 → 1 次提交") {
    CountGate gate2(2);
    LocationHarness harness2(/*group_commit=*/true, /*max_wait_ms=*/2000, /*max_batch=*/2, &gate2,
                             "b2.db");
    ConcurrentSaves(*harness2.repo, 8);
    REQUIRE(harness2.observer.CommitCount() == 4);  // ceil(8/2) == 4

    CountGate gate8(8);
    LocationHarness harness8(/*group_commit=*/true, /*max_wait_ms=*/2000, /*max_batch=*/8, &gate8,
                             "b8.db");
    ConcurrentSaves(*harness8.repo, 8);
    REQUIRE(harness8.observer.CommitCount() == 1);  // ceil(8/8) == 1
    REQUIRE(harness8.observer.Ops() == std::vector<std::size_t>{8});
  }

  SECTION("B=1 → 8 次提交（退化即「每操作一次」）") {
    CountGate gate1(1);
    LocationHarness harness(/*group_commit=*/true, /*max_wait_ms=*/2000, /*max_batch=*/1, &gate1);
    ConcurrentSaves(*harness.repo, 8);
    REQUIRE(harness.observer.CommitCount() == 8);  // ceil(8/1) == 8
    REQUIRE(harness.observer.Ops() == std::vector<std::size_t>(8, 1));
  }
}

TEST_CASE("★ C10.20 摊销：元数据仓储也走同一套组提交（N=6、B=3 → 2 次提交）",
          "[phase10][integration][sqlite][c10.20]") {
  fss::test::TempDir dir("sqlite_group_commit_meta");
  fss::ManualClock clock{1700000000};
  RecordingObserver observer;
  CountGate gate(3);
  SqliteMetadataRepositoryOptions options;
  options.group_commit = true;
  options.group_commit_max_wait_ms = 2000;
  options.group_commit_max_batch = 3;
  options.batch_observer = &observer;
  options.batch_gate = &gate;
  auto opened = SqliteMetadataRepository::Open(dir.child("meta.db"), clock, options);
  REQUIRE(opened.ok());
  auto& repo = *opened.value();

  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 6; ++i) {
    threads.emplace_back([&repo, &failures, i] {
      const std::string suffix = "m" + std::to_string(i);
      const auto record = fss::test::MakeRecord("opendes", suffix, "/u/" + suffix, suffix);
      //  ★ C1：`ClaimForWrite`（原子领取）是**一个**批操作 —— 用它钉住"摊销 == ceil(N/B)"。
      if (!repo.ClaimForWrite("opendes", record).ok()) ++failures;
    });
  }
  for (auto& thread : threads) thread.join();
  REQUIRE(failures.load() == 0);
  REQUIRE(observer.CommitCount() == 2);  // ceil(6/3) == 2
  REQUIRE(observer.Ops() == std::vector<std::size_t>{3, 3});
  REQUIRE(observer.Rollbacks() == 0);

  //  ★ C1：`Create` 现在是 claim → mark-ready 的**复合**路径（每次登记 2 个批操作）。
  //    单独钉住它：6 次登记 = 12 个批操作、B=3 → 4 次提交（摊销比仍由 B 决定）。
  //    这条替代的只是"用哪个写原语计数"——`ceil(N/B)` 这条判据本身没有被放宽。
  observer.Reset();
  std::atomic<int> create_failures{0};
  std::vector<std::thread> create_threads;
  for (int i = 0; i < 6; ++i) {
    create_threads.emplace_back([&repo, &create_failures, i] {
      const std::string suffix = "c" + std::to_string(i);
      const auto record = fss::test::MakeRecord("opendes", suffix, "/u/" + suffix, suffix);
      if (!repo.Create("opendes", record).ok()) ++create_failures;
    });
  }
  for (auto& thread : create_threads) thread.join();
  REQUIRE(create_failures.load() == 0);
  REQUIRE(observer.CommitCount() == 4);  // ceil(12/3) == 4（ClaimForWrite + MarkReady 各一个操作）
  REQUIRE(observer.Ops() == std::vector<std::size_t>{3, 3, 3, 3});
  REQUIRE(observer.Rollbacks() == 0);
}

TEST_CASE("★ C10.20 group_commit=false：逐操作提交（== N，每操作一个事务，逐字回归）",
          "[phase10][integration][sqlite][c10.20]") {
  LocationHarness harness(/*group_commit=*/false, /*max_wait_ms=*/2000, /*max_batch=*/64,
                          /*gate=*/nullptr);
  ConcurrentSaves(*harness.repo, 8);
  //  ★ 接线前的行为：N 个操作 = N 次事务；观察者按"每操作一批"记录。
  REQUIRE(harness.observer.BatchCount() == 8);
  REQUIRE(harness.observer.CommitCount() == 8);
  REQUIRE(harness.observer.Ops() == std::vector<std::size_t>(8, 1));
  REQUIRE(harness.observer.Rollbacks() == 0);
}

// =============================================================================
//  判据 2：每操作原子性（真实的 (partition, file_source) 唯一索引冲突）
// =============================================================================
TEST_CASE("★ C10.20 每操作原子性：同批里一个 Save 撞唯一索引 → 只有它失败，其余照常提交",
          "[phase10][integration][sqlite][c10.20]") {
  CountGate gate(3);  // 3 个操作必须进**同一批**（max_batch 远大于 3）
  LocationHarness harness(/*group_commit=*/true, /*max_wait_ms=*/2000, /*max_batch=*/64, &gate);
  auto& repo = *harness.repo;

  //  先占住一个 file_source（唯一索引 `idx_fl_source` 是 (partition_id, file_source)）。
  REQUIRE(repo.Save("opendes", fss::test::MakeLocation("seed", "/u/seed")).ok());
  harness.observer.Reset();  //  seed 是准备步骤，不属于被测的那一批

  std::atomic<int> ok_count{0};
  fss::Error conflict_error;
  std::mutex error_mutex;
  std::vector<std::thread> threads;
  //  ① 正常；② **必然失败**（不同 file_id 抢同一个 file_source → SQLITE_CONSTRAINT）；
  //  ③ 正常。
  threads.emplace_back([&] {
    if (repo.Save("opendes", fss::test::MakeLocation("a", "/u/a")).ok()) ++ok_count;
  });
  threads.emplace_back([&] {
    const auto conflict = repo.Save("opendes", fss::test::MakeLocation("b", "/u/seed"));
    std::lock_guard<std::mutex> lock(error_mutex);
    if (!conflict.ok()) conflict_error = conflict.error();
  });
  threads.emplace_back([&] {
    if (repo.Save("opendes", fss::test::MakeLocation("c", "/u/c")).ok()) ++ok_count;
  });
  for (auto& thread : threads) thread.join();

  //  ① 失败操作返回的是"位置已存在"（不是 500、也不是整批失败）。
  REQUIRE(ok_count.load() == 2);
  REQUIRE(conflict_error.kind() == fss::ErrorKind::kLocationAlreadyExists);

  //  ② 同批其它操作全部成功且**数据可读**。
  const auto a = repo.Find("opendes", "a");
  const auto c = repo.Find("opendes", "c");
  REQUIRE(a.ok());
  REQUIRE(c.ok());
  REQUIRE(a.value().file_source == "/u/a");
  REQUIRE(c.value().file_source == "/u/c");

  //  ③ 失败操作**没有留下任何行**：id=b 不存在；被抢的 file_source 仍属于 seed。
  REQUIRE_FALSE(repo.Find("opendes", "b").ok());
  const auto by_source = repo.FindByFileSource("opendes", "/u/seed");
  REQUIRE(by_source.ok());
  REQUIRE(by_source.value().file_id == "seed");

  //  ④ 观察者：1 个批次（3 个操作，整批提交成功）+ **1 次**保存点回滚。
  REQUIRE(harness.observer.BatchCount() == 1);
  REQUIRE(harness.observer.Ops() == std::vector<std::size_t>{3});
  REQUIRE(harness.observer.AllCommitted());
  REQUIRE(harness.observer.Rollbacks() == 1);
}

// =============================================================================
//  判据 3：`max_wait_ms` 真的生效（批永远不满时，窗口是唯一的 flush 依据）
// =============================================================================
TEST_CASE("★ C10.20 max_wait_ms：批永远不满 → 在窗口量级内提交；0 与较大值行为不同",
          "[phase10][integration][sqlite][c10.20]") {
  SECTION("max_wait_ms=0 → 立刻提交（仍成功、不挂死）") {
    NeverFlushGate gate;  // 门控保证"批永远不满"
    LocationHarness harness(/*group_commit=*/true, /*max_wait_ms=*/0, /*max_batch=*/64, &gate);
    const auto begin = std::chrono::steady_clock::now();
    REQUIRE(harness.repo->Save("opendes", fss::test::MakeLocation("w0", "/u/w0")).ok());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - begin)
                             .count();
    INFO("elapsed(ms) = " << elapsed);
    REQUIRE(elapsed < 200);  // 窗口 0 ⇒ 不能等出几百毫秒
    REQUIRE(harness.observer.CommitCount() == 1);
  }

  SECTION("max_wait_ms=400 → 确实等满窗口才提交（差异可观测）") {
    NeverFlushGate gate;
    LocationHarness harness(/*group_commit=*/true, /*max_wait_ms=*/400, /*max_batch=*/64, &gate);
    const auto begin = std::chrono::steady_clock::now();
    REQUIRE(harness.repo->Save("opendes", fss::test::MakeLocation("w400", "/u/w400")).ok());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - begin)
                             .count();
    INFO("elapsed(ms) = " << elapsed);
    REQUIRE(elapsed >= 350);   // 窗口真的生效（不是"读了配置但忽略"）
    REQUIRE(elapsed < 5000);   // 且有界（不挂死）
    REQUIRE(harness.observer.CommitCount() == 1);
  }
}

// =============================================================================
//  判据 4：读不脏 —— 批事务开着时，读必须等到批结束，且看到的是提交后的值
// =============================================================================
TEST_CASE("★ C10.20 读不脏：批事务开着时 GetById 必须等待，返回批提交后的值",
          "[phase10][integration][sqlite][c10.20]") {
  fss::test::TempDir dir("sqlite_group_commit_dirty");
  fss::ManualClock clock{1700000000};
  RecordingObserver observer;
  BlockingCommitGate gate;
  SqliteMetadataRepositoryOptions options;
  options.group_commit = true;
  options.group_commit_max_wait_ms = 5000;
  options.group_commit_max_batch = 64;
  options.batch_observer = &observer;
  options.batch_gate = &gate;
  auto opened = SqliteMetadataRepository::Open(dir.child("meta.db"), clock, options);
  REQUIRE(opened.ok());
  auto& repo = *opened.value();

  const std::string partition = "opendes";
  const auto record = fss::test::MakeRecord(partition, "dirty1", "/u/dirty1", "after-commit");

  //  ★ C1：`Create` 现在是 claim → mark-ready 两次写（两个批操作）。要让"读必须等到批结束、
  //    且返回**提交后的值**"这条判据仍然成立，这里把**可见性翻转**（`MarkReady`）作为被
  //    门控卡住的那**一个**批操作：只有它提交之后，`GetById` 才应该看到 ready 记录。
  //    （若仍用 `Create` 当写者，被卡住的是 claiming 批；那批提交后读到的正确结果就是
  //     kNotFound —— "返回提交后的值"在那条路径上无从断言，这是新顺序的真实语义。）
  //  领取是**准备步骤**：用一个不带门控的第二个连接完成，避免它先撞上 `BeforeCommit` 的门。
  {
    SqliteMetadataRepositoryOptions plain_options;
    plain_options.group_commit = false;  // 逐操作路径，不经过门控
    auto plain_opened = SqliteMetadataRepository::Open(dir.child("meta.db"), clock, plain_options);
    REQUIRE(plain_opened.ok());
    const auto claimed = plain_opened.value()->ClaimForWrite(partition, record);
    REQUIRE(claimed.ok());
    REQUIRE(claimed.value().claimed);
  }
  observer.Reset();  // 领取不属于被测的那一批

  std::atomic<bool> writer_ok{false};
  std::thread writer([&] { writer_ok = repo.MarkReady(partition, record.id, 1, record).ok(); });

  //  领队在批事务内、COMMIT 之前被门控卡住（确定性时点，不用 sleep 猜）。
  REQUIRE(gate.WaitUntilOpen());
  REQUIRE(gate.ops_in_transaction() == 1);

  std::atomic<bool> reader_started{false};
  std::atomic<bool> reader_done{false};
  std::optional<FileMetadataRecord> read_value;
  std::thread reader([&] {
    reader_started = true;
    const auto found = repo.GetById(partition, record.id);
    if (found.ok()) read_value = found.value();
    reader_done = true;
  });

  //  ★ 读在批事务开着时**必须等待**：给它 150ms 也不能返回（返回了就是读到了未提交数据）。
  for (int i = 0; i < 100 && !reader_started.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(reader_started.load());
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  REQUIRE_FALSE(reader_done.load());  //  脏读会在这里失败

  gate.Release();
  writer.join();
  reader.join();

  REQUIRE(writer_ok.load());
  REQUIRE(reader_done.load());
  REQUIRE(read_value.has_value());
  REQUIRE(read_value->data.name.value_or("") == "after-commit");  //  提交后的值
  REQUIRE(observer.CommitCount() == 1);
}
