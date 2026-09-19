// =============================================================================
//  ADR-008 的 P4（两阶段批提交）：`durability=batch` 的真实语义
// =============================================================================
//  判据（父代理定案；也见 ADR-008 §5 的 R1/R2 与 C9.23）：
//    1. 摊销：并发 N 个 put、批上限 C → `syncfs` 次数 == ceil(N/C)（**确定性**，
//       用门控/屏障让批必定填满，不靠 sleep 猜时序）；C=1 → == N；
//       `durability=per_file` → 0 次 `syncfs`（走 `data_sync`）。
//    2. 顺序不变量（C9.23）：逐批断言「所有 rename > 该批的 syncfs > 所有 write_tmp」、
//       「fsync_dir > 所有 rename」。**必须能失败**（注入见 phase9 证据 §13）。
//    3. 数据正确性：批提交后每个对象**逐字节可读**，且**没有 `.tmp_*` 残留**
//       （★ 否定式判据配正控：提交前先断言 tmp 真的存在）。
//    4. 阈值例外：`>= fsync_threshold_bytes` 的对象走单独 `data_sync`（不靠批摊销）。
//    5. `atomic_write=false` → 不入批（没有 `syncfs`/`rename`）。
//    6. 失败路径：批内某个 rename 失败 → 该对象不成为正式对象（返回错误给它的调用者），
//       同批其它对象仍按协议提交。
//
//  ★ 为什么把"顺序"做成接缝：真实系统调用上不可观察（ADR-008 §4.5 记录 O_DIRECT
//    对照**无结论**）。注入记录器后事件序列是可断言的证据。
//  ★ 本环境**无法**验证断电语义（无 root / 不能 mount / 无电源故障注入）：
//    这里只验证"顺序不变量 + 摊销 + 数据正确性 + 失败路径"。power-loss durability
//    仍记为**未验证**（ADR-008 §6 的 R4/R8 与新增的"本实现未验证项"）。
// =============================================================================
#include <catch2/catch.hpp>

#include "temp_dir.h"

#include "common/bytes/bytes.h"
#include "common/fs/fs.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/blob/posix/posix_blob_store.h"
#include "infra/io/file_sync.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using fss::domain::ObjectRef;
using fss::domain::PutOptions;
using fss::infra::FsyncPolicy;
using fss::infra::IBatchArrivalGate;
using fss::infra::IBatchCommitObserver;
using fss::infra::IFileSync;
using fss::infra::PosixBlobStore;
using fss::infra::PosixBlobStoreOptions;

namespace {

ObjectRef Ref(const std::string& key) {
  ObjectRef ref;
  ref.container = "c";
  ref.key = key;
  return ref;
}

struct Event {
  std::string op;   // write_tmp / data_sync / syncfs / rename / fsync_dir
  std::string path; // 事件对象（tmp 路径 / 目录 / 改名目标）
  std::size_t batch = 0;  // 仅 syncfs 有意义：本批对象数
};

//  记录器：同时实现「操作事件观察者」与「落盘接缝」。
//  IFileSync 侧是 no-op（顺序判据与摊销计数只看事件，真实 syncfs 由生产实现负责）；
//  观察者侧把事件序列记下来 —— 这就是 R1/R2 顺序不变量的证据来源。
class RecordingSink final : public IBatchCommitObserver, public IFileSync {
 public:
  // ---- IBatchCommitObserver ----
  void OnWriteTmp(const std::string& path) override { Push("write_tmp", path); }
  void OnDataSync(const std::string& path) override { Push("data_sync", path); }
  void OnSyncFilesystem(const std::string& directory, std::size_t batch_size) override {
    Push("syncfs", directory, batch_size);
  }
  void OnRename(const std::string& from, const std::string& to) override {
    Push("rename", from + " -> " + to);
  }
  void OnDirectorySync(const std::string& directory) override { Push("fsync_dir", directory); }

  // ---- IFileSync（no-op；真实实现见 RealFileSync） ----
  fss::Result<void> DataSync(int fd) override {
    (void)fd;
    return fss::Ok();
  }
  fss::Result<void> SyncDirectory(const std::string& directory) override {
    (void)directory;
    return fss::Ok();
  }
  fss::Result<void> SyncFilesystem(const std::string& directory) override {
    (void)directory;
    return fss::Ok();
  }

  std::vector<Event> Snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return events_;
  }
  std::size_t Count(const std::string& op) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t n = 0;
    for (const auto& e : events_) {
      if (e.op == op) ++n;
    }
    return n;
  }

 private:
  void Push(std::string op, std::string path, std::size_t batch = 0) {
    std::lock_guard<std::mutex> lock(mu_);
    events_.push_back(Event{std::move(op), std::move(path), batch});
  }

  mutable std::mutex mu_;
  std::vector<Event> events_;
};

//  入批门控：让 N 个并发 put 在「tmp 已写好、尚未入批」处对齐。
//  ★ 这是"批必定填满"的**确定性**来源（不是 sleep）：所有线程都在门里，
//    于是 `batch_writers_ >= N`，领队必然等到批满（或窗口到期）。
class ProbingGate final : public IBatchArrivalGate {
 public:
  ProbingGate(std::size_t n, std::function<void()> on_all_arrived = {})
      : n_(n), on_all_arrived_(std::move(on_all_arrived)) {}

  void ArriveAndWait() override {
    std::unique_lock<std::mutex> lock(mu_);
    ++arrived_;
    if (arrived_ == n_) {
      if (on_all_arrived_) on_all_arrived_();  // 正控探针：此刻 tmp 应当真的都在
      released_ = true;
      cv_.notify_all();
      return;
    }
    cv_.wait(lock, [&] { return released_; });
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::size_t n_;
  std::size_t arrived_ = 0;
  bool released_ = false;
  std::function<void()> on_all_arrived_;
};

std::size_t CountTmpResidue(const std::string& root) {
  std::size_t n = 0;
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end;
       it.increment(ec)) {
    if (ec) break;
    if (it->path().filename().string().find(".tmp.") != std::string::npos) ++n;
  }
  return n;
}

PosixBlobStoreOptions BatchOptions(RecordingSink& sink, std::size_t max_batch,
                                   IBatchArrivalGate* gate = nullptr,
                                   std::int64_t threshold = 1024 * 1024) {
  PosixBlobStoreOptions options;
  options.batch_commit = true;
  options.fsync_policy = FsyncPolicy::kBySize;  // 只表达"≥ 阈值 → 单独 fdatasync"
  options.fsync_threshold_bytes = threshold;
  options.group_commit_max_batch = max_batch;
  options.sync_dir_after_batch = true;
  options.file_sync = &sink;
  options.batch_observer = &sink;
  options.batch_gate = gate;
  return options;
}

}  // namespace

// =============================================================================
//  1) 摊销：syncfs 次数 == ceil(N / C)，且确定性（门控/屏障，不靠 sleep）
// =============================================================================
TEST_CASE("★ C9.23 摊销：N=8 并发 put、C=4 → syncfs == 2；C=1 → == 8",
          "[phase3][posix][batch][c9.23]") {
  SECTION("N=8, C=4 → 恰好 2 批（每批 4 个对象）") {
    fss::test::TempDir dir("batch_amortize_4");
    fss::ManualClock clock;
    RecordingSink sink;
    constexpr int kN = 8;
    ProbingGate gate(kN);
    PosixBlobStore store(dir.str(), clock, BatchOptions(sink, /*max_batch=*/4, &gate));
    REQUIRE(store.ensure_container("c").ok());

    std::vector<std::thread> threads;
    std::vector<int> ok(kN, 0);
    for (int i = 0; i < kN; ++i) {
      threads.emplace_back([&, i] {
        fss::bytes::StringSource source(std::string(64, static_cast<char>('a' + i)));
        ok[i] = store.put(Ref("obj_" + std::to_string(i) + ".bin"), source, PutOptions{}).ok() ? 1
                                                                                              : 0;
      });
    }
    for (auto& t : threads) t.join();
    for (int i = 0; i < kN; ++i) REQUIRE(ok[i] == 1);

    //  摊销判据：syncfs 次数 = ceil(8 / 4) = 2（不是 8，也不是 1）
    REQUIRE(sink.Count("syncfs") == 2);
    //  ★ 正控：确实发生了 rename（每个对象 = 本体 + sidecar 两条）
    REQUIRE(sink.Count("rename") == 2 * kN);
    //  每批各有一次 fsync_dir（R2）
    REQUIRE(sink.Count("fsync_dir") == 2);
    //  小对象不单独落盘（摊销的前提）
    REQUIRE(sink.Count("data_sync") == 0);
    REQUIRE(CountTmpResidue(dir.str()) == 0);
  }

  SECTION("N=8, C=1 → 退化即「每文件一次提交」：syncfs == 8") {
    fss::test::TempDir dir("batch_amortize_1");
    fss::ManualClock clock;
    RecordingSink sink;
    constexpr int kN = 8;
    //  C=1 时批永远只有一个成员：门控仍然对齐，但每人都必须自己提交。
    ProbingGate gate(kN);
    PosixBlobStore store(dir.str(), clock, BatchOptions(sink, /*max_batch=*/1, &gate));
    REQUIRE(store.ensure_container("c").ok());

    std::vector<std::thread> threads;
    std::vector<int> ok(kN, 0);
    for (int i = 0; i < kN; ++i) {
      threads.emplace_back([&, i] {
        fss::bytes::StringSource source(std::string(32, 'x'));
        ok[i] = store.put(Ref("s_" + std::to_string(i) + ".bin"), source, PutOptions{}).ok() ? 1 : 0;
      });
    }
    for (auto& t : threads) t.join();
    for (int i = 0; i < kN; ++i) REQUIRE(ok[i] == 1);
    REQUIRE(sink.Count("syncfs") == kN);  // 退化：每文件一次提交
    REQUIRE(sink.Count("fsync_dir") == kN);
  }

  SECTION("durability=per_file → 0 次 syncfs（每对象一次 data_sync）") {
    fss::test::TempDir dir("batch_per_file");
    fss::ManualClock clock;
    RecordingSink sink;
    PosixBlobStoreOptions options;
    options.fsync_policy = FsyncPolicy::kAlways;  // per_file（batch_commit 保持 false）
    options.file_sync = &sink;
    options.batch_observer = &sink;
    PosixBlobStore store(dir.str(), clock, options);
    REQUIRE(store.ensure_container("c").ok());
    for (int i = 0; i < 3; ++i) {
      fss::bytes::StringSource source(std::string(48, 'p'));
      REQUIRE(store.put(Ref("pf_" + std::to_string(i) + ".bin"), source, PutOptions{}).ok());
    }
    REQUIRE(sink.Count("syncfs") == 0);
    REQUIRE(sink.Count("data_sync") == 3);
    //  正控：rename 仍然发生（没有 batch 也有原子写）
    REQUIRE(sink.Count("rename") == 3);
  }
}

// =============================================================================
//  2) 顺序不变量（C9.23 的核心）：
//     所有 rename > 该批的 syncfs > 所有 write_tmp；fsync_dir > 所有 rename
// =============================================================================
namespace {

//  逐批检查事件序列。失败时给出可读的序列，便于 R1 自证时对照。
void CheckOrdering(const std::vector<Event>& events) {
  std::size_t first_syncfs = events.size();
  std::size_t last_write_tmp = events.size();
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (events[i].op == "syncfs" && first_syncfs == events.size()) first_syncfs = i;
    if (events[i].op == "write_tmp") last_write_tmp = i;
  }
  INFO("事件序列:");
  for (std::size_t i = 0; i < events.size(); ++i) {
    INFO("  " << i << " " << events[i].op << " " << events[i].path);
  }
  REQUIRE(first_syncfs != events.size());  // 至少发生过一次 syncfs
  //  ① 本批（以及所有批）的 write_tmp 都必须早于**第一次** syncfs
  REQUIRE(last_write_tmp < first_syncfs);
  //  ①b ★ R1 的直接编码：**任何 rename 都不允许出现在第一次 syncfs 之前**。
  //     （R1 注入"把 syncfs 挪到 rename 之后"就是在这里失败的。）
  for (std::size_t i = 0; i < first_syncfs; ++i) {
    CAPTURE(i, events[i].op, events[i].path);
    REQUIRE(events[i].op != "rename");
  }

  //  ② 按 syncfs 切批：每批的 rename 在 syncfs 之后、fsync_dir 之前
  std::vector<std::size_t> syncfs_idx;
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (events[i].op == "syncfs") syncfs_idx.push_back(i);
  }
  for (std::size_t b = 0; b < syncfs_idx.size(); ++b) {
    const std::size_t start = syncfs_idx[b];
    const std::size_t end = (b + 1 < syncfs_idx.size()) ? syncfs_idx[b + 1] : events.size();
    std::size_t renames = 0;
    std::size_t fsync_dirs = 0;
    for (std::size_t i = start + 1; i < end; ++i) {
      if (events[i].op == "rename") ++renames;
      if (events[i].op == "fsync_dir") ++fsync_dirs;
    }
    INFO("批 #" << b << "：syncfs@" << start << " renames=" << renames
                << " fsync_dir=" << fsync_dirs);
    REQUIRE(renames > 0);        // 空批没有意义
    REQUIRE(fsync_dirs == 1);    // sync_dir_after_batch=true：每批恰好一次目录 fsync
    //  rename 与 fsync_dir 都在本批 syncfs 之后（由窗口切分天然保证），
    //  且 fsync_dir 必须是本批**最后**一个事件（在所有 rename 之后）。
    REQUIRE(events[end - 1].op == "fsync_dir");
  }
}

}  // namespace

TEST_CASE("★ C9.23 顺序不变量：rename 全在 syncfs 之后；fsync_dir 全在 rename 之后",
          "[phase3][posix][batch][c9.23]") {
  fss::test::TempDir dir("batch_order");
  fss::ManualClock clock;
  RecordingSink sink;
  constexpr int kN = 8;
  ProbingGate gate(kN);
  PosixBlobStore store(dir.str(), clock, BatchOptions(sink, /*max_batch=*/4, &gate));
  REQUIRE(store.ensure_container("c").ok());

  std::vector<std::thread> threads;
  std::vector<int> ok(kN, 0);
  for (int i = 0; i < kN; ++i) {
    threads.emplace_back([&, i] {
      fss::bytes::StringSource source(std::string(24, 'q'));
      ok[i] = store.put(Ref("o_" + std::to_string(i) + ".bin"), source, PutOptions{}).ok() ? 1 : 0;
    });
  }
  for (auto& t : threads) t.join();
  for (int i = 0; i < kN; ++i) REQUIRE(ok[i] == 1);

  CheckOrdering(sink.Snapshot());
  REQUIRE(sink.Count("syncfs") == 2);
}

// =============================================================================
//  3) 数据正确性 + 无 `.tmp_*` 残留（★ 否定式判据配正控）
// =============================================================================
TEST_CASE("★ C9.23 批提交后逐字节可读，且无 .tmp_ 残留（含正控）",
          "[phase3][posix][batch][c9.23]") {
  fss::test::TempDir dir("batch_content");
  fss::ManualClock clock;
  RecordingSink sink;
  constexpr int kN = 4;
  std::atomic<std::size_t> tmp_files_at_gate{0};
  //  正控：所有线程都停在门里时（tmp 已写好、尚未提交），扫描目录必须真的看到 tmp。
  ProbingGate gate(kN, [&] { tmp_files_at_gate.store(CountTmpResidue(dir.str())); });
  PosixBlobStore store(dir.str(), clock, BatchOptions(sink, /*max_batch=*/kN, &gate));
  REQUIRE(store.ensure_container("c").ok());

  std::vector<std::string> payload(kN);
  for (int i = 0; i < kN; ++i) {
    payload[i] = std::string(200, static_cast<char>('A' + i)) + std::to_string(i);
  }
  std::vector<std::thread> threads;
  std::vector<int> ok(kN, 0);
  for (int i = 0; i < kN; ++i) {
    threads.emplace_back([&, i] {
      fss::bytes::StringSource source(payload[i]);
      ok[i] = store.put(Ref("c_" + std::to_string(i) + ".bin"), source, PutOptions{}).ok() ? 1 : 0;
    });
  }
  for (auto& t : threads) t.join();
  for (int i = 0; i < kN; ++i) REQUIRE(ok[i] == 1);

  //  ★ 正控：提交**之前**目录里确实有 2*N 个 tmp（对象本体 + sidecar）。
  //    没有这一条，"提交后没有残留"可能因为路径写错而恒真。
  INFO("门控时刻的 tmp 文件数: " << tmp_files_at_gate.load());
  REQUIRE(tmp_files_at_gate.load() == 2 * kN);

  //  ① 每个对象**逐字节**可读（不是只看"文件存在"）
  for (int i = 0; i < kN; ++i) {
    const std::string key = "c_" + std::to_string(i) + ".bin";
    fss::bytes::StringSink out;
    const auto read = store.get(Ref(key), out, fss::domain::ByteRange{});
    REQUIRE(read.ok());
    REQUIRE(out.str() == payload[i]);
    //  副作用：sidecar 也在（checksum 可回读）
    const auto st = store.stat(Ref(key));
    REQUIRE(st.ok());
    REQUIRE(st.value().exists);
    REQUIRE(st.value().size == static_cast<std::int64_t>(payload[i].size()));
  }

  //  ② 提交完成后**没有** `.tmp_*` 残留（现在这条判据有牙齿：正控证明了扫描是真的）
  REQUIRE(CountTmpResidue(dir.str()) == 0);
}

// =============================================================================
//  4) 阈值例外：>= fsync_threshold_bytes 的对象单独 data_sync（不靠批摊销）
// =============================================================================
TEST_CASE("★ C9.23 阈值例外：大对象单独 data_sync；小对象走批（无 data_sync）",
          "[phase3][posix][batch][c9.23]") {
  fss::test::TempDir dir("batch_threshold");
  fss::ManualClock clock;
  RecordingSink sink;
  PosixBlobStore store(dir.str(), clock, BatchOptions(sink, /*max_batch=*/4, nullptr,
                                                      /*threshold=*/1024));
  REQUIRE(store.ensure_container("c").ok());

  //  小对象（< 1024）：入批 → 只有 syncfs，没有 data_sync
  fss::bytes::StringSource small(std::string(100, 's'));
  REQUIRE(store.put(Ref("small.bin"), small, PutOptions{}).ok());
  REQUIRE(sink.Count("syncfs") == 1);
  REQUIRE(sink.Count("data_sync") == 0);

  //  大对象（== 1024，边界取 >=）：**不**入批，单独 fdatasync + rename + fsync_dir
  fss::bytes::StringSource large(std::string(1024, 'L'));
  REQUIRE(store.put(Ref("large.bin"), large, PutOptions{}).ok());
  REQUIRE(sink.Count("data_sync") == 1);
  REQUIRE(sink.Count("syncfs") == 1);  // 大对象没有新增 syncfs
  const auto events = sink.Snapshot();
  bool large_synced = false;
  for (const auto& e : events) {
    if (e.op == "data_sync" && e.path.find("large.bin") != std::string::npos) large_synced = true;
  }
  REQUIRE(large_synced);  // 事件序列里能看到"是那个大对象"
  //  正控：小对象确实进了批（size 100 的对象没有 data_sync 事件）
  for (const auto& e : events) {
    if (e.op == "data_sync") REQUIRE(e.path.find("small.bin") == std::string::npos);
  }
}

// =============================================================================
//  5) atomic_write=false → 不入批（逐字保持旧直写语义：没有 syncfs / rename）
// =============================================================================
TEST_CASE("★ C9.23 atomic_write=false 不入批：没有 syncfs / rename",
          "[phase3][posix][batch][c9.23]") {
  fss::test::TempDir dir("batch_direct");
  fss::ManualClock clock;
  RecordingSink sink;
  PosixBlobStoreOptions options = BatchOptions(sink, /*max_batch=*/4);
  options.atomic_write = false;
  PosixBlobStore store(dir.str(), clock, options);
  REQUIRE(store.ensure_container("c").ok());

  const std::string content = std::string(80, 'd');
  fss::bytes::StringSource source(content);
  REQUIRE(store.put(Ref("direct.bin"), source, PutOptions{}).ok());
  REQUIRE(sink.Count("syncfs") == 0);
  REQUIRE(sink.Count("rename") == 0);
  REQUIRE(sink.Count("data_sync") == 0);  // 小对象，kBySize 阈值下不单独落盘

  //  正控：内容仍然真的写进了目标路径并可读回
  fss::bytes::StringSink out;
  REQUIRE(store.get(Ref("direct.bin"), out, fss::domain::ByteRange{}).ok());
  REQUIRE(out.str() == content);
  REQUIRE(fss::fs::Exists(dir.str() + "/c/direct.bin"));
}

// =============================================================================
//  6) 失败路径：批内某个对象的 rename 失败
// =============================================================================
//  规则（先定义再断言）：
//    · 失败对象**不得**被 rename 成正式对象，`put` 返回错误给它的调用者；
//    · 同批其它对象仍按协议提交（syncfs → rename → fsync_dir）；
//    · 失败对象的 tmp 全部清理（不留半个对象）。
//  注入方式（真实、无需额外生产代码）：在最终对象路径上预先建一个**目录** ——
//  `rename(file, dir)` 必然失败（EISDIR），而 tmp 的创建不受影响。
TEST_CASE("★ C9.23 失败路径：一个 rename 失败不影响同批其它对象的提交",
          "[phase3][posix][batch][c9.23]") {
  fss::test::TempDir dir("batch_failure");
  fss::ManualClock clock;
  RecordingSink sink;
  constexpr int kN = 4;
  ProbingGate gate(kN);
  PosixBlobStore store(dir.str(), clock, BatchOptions(sink, /*max_batch=*/kN, &gate));
  REQUIRE(store.ensure_container("c").ok());

  //  注入：bad.bin 的最终路径是一个目录 → rename 必失败
  REQUIRE(fss::fs::EnsureDir(dir.str() + "/c/bad.bin").ok());

  std::vector<std::thread> threads;
  std::vector<int> ok(kN, 0);
  std::vector<std::string> errors(kN);
  const std::string good = std::string(64, 'g');
  for (int i = 0; i < kN; ++i) {
    const std::string key = (i == 0) ? "bad.bin" : ("good_" + std::to_string(i) + ".bin");
    threads.emplace_back([&, i, key] {
      fss::bytes::StringSource source(good);
      const auto result = store.put(Ref(key), source, PutOptions{});
      ok[i] = result.ok() ? 1 : 0;
      if (!result.ok()) errors[i] = result.error().ToString();
    });
  }
  for (auto& t : threads) t.join();

  //  ① 失败的那个必须返回错误（正控：错误真的被观察到）
  REQUIRE(ok[0] == 0);
  REQUIRE_FALSE(errors[0].empty());
  //  ② 同批其它三个照常提交
  for (int i = 1; i < kN; ++i) {
    CAPTURE(i, errors[i]);
    REQUIRE(ok[i] == 1);
    fss::bytes::StringSink out;
    const std::string key = "good_" + std::to_string(i) + ".bin";
    REQUIRE(store.get(Ref(key), out, fss::domain::ByteRange{}).ok());
    REQUIRE(out.str() == good);
  }
  //  ③ 协议真的走完了（不是整批失败）：一次 syncfs + 一次 fsync_dir；
  //     成功的 3 个对象各有 2 条 rename（本体 + sidecar）→ 共 6 条
  REQUIRE(sink.Count("syncfs") == 1);
  REQUIRE(sink.Count("fsync_dir") == 1);
  REQUIRE(sink.Count("rename") == 6);
  //  ④ 失败对象没有被 rename 成正式**文件**（它仍是注入的目录），且无 tmp 残留
  REQUIRE(std::filesystem::is_directory(dir.str() + "/c/bad.bin"));
  REQUIRE(CountTmpResidue(dir.str()) == 0);
}
