// =============================================================================
//  test_multi_instance.cpp —— C6.11：多实例幂等（复现 ADR-009 M2）
// =============================================================================
//  风险 M2（ADR-009 §3 / AGENTS §4.4）：**幂等靠应用层 check-then-insert** 时，
//  两个实例同时提交同一个 `fileSource` 会各插一条 → 重复记录 + 重复对象。
//  正确做法：幂等键 `(partition, file_source)` 上的**唯一约束**（R5）+ 冲突后返回既有记录。
//
//  ★ 判据 C6.11 的两条：
//    ① 2 实例并发提交同一 `fileSource` → **只产生 1 条记录**、只存在 1 份 persistent 对象；
//    ② 对照测试：把约束换成"按随机主键 check-then-insert"时**必须**出现重复（自证检测器有效）。
//
//  本文件覆盖三种装配：
//    · 内存仓储（两个 `UseCasePorts` = 两个实例，共享同一个仓储对象）；
//    · **SQLite 两个连接**（两个仓储对象指向同一个库文件 = 真·2 实例，靠**唯一索引**兜底）；
//    · 对照：`NaiveIdKeyedMetadataRepository`（按 record id 建键、无幂等键约束）。
//
//  ★ "两个实例"用同一个 `SequentialIdGenerator` 种子：这是**最坏情况** ——
//    两个实例会生成**同一个记录 id**，因此幂等不能靠"id 不同"侥幸成立。
// =============================================================================
#include <catch2/catch.hpp>

#include "app_fixture.h"
#include "temp_dir.h"

#include "app/services/location_issuer.h"
#include "app/usecases/usecases.h"
#include "common/bytes/bytes.h"
#include "common/crypto/crypto.h"
#include "infra/location/memory/memory_location_repository.h"
#include "infra/metadata/memory/memory_metadata_repository.h"
#include "infra/metadata/sqlite/sqlite_metadata_repository.h"

#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using fss::app::CreateFileMetadata;
using fss::app::GetUploadLocation;
using fss::app::ObjectRefFromLocation;
using fss::app::UseCasePorts;
using fss::domain::FileMetadataRecord;
using fss::domain::IMetadataRepository;
using fss::domain::MetadataPage;
using fss::domain::MetadataQuery;
using fss::test::AllowAllAuthorizer;
using fss::test::AppFixture;
using fss::test::FakeBlobStoreFactory;
using fss::test::FakePartitionRegistry;
using fss::test::NoopLegalValidator;
using fss::test::NoopSchemaValidator;
using fss::test::RecordingAuditLogger;
using fss::test::RecordingEventPublisher;
using fss::test::RecordingSelfSignedCodec;

//  一个"实例"：自己的 id 生成器 + issuer + 端口集合（共享 store / 位置仓储 / 元数据仓储）。
//  ★ 事件/审计/作者器/自签 codec 都是**每个实例各一份**（真实世界里就是每个进程各一份），
//    不能让两个线程同时往同一个 vector 里 push_back —— 那是数据竞争（实测堆损坏）。
struct Instance {
  fss::SequentialIdGenerator ids{1};
  std::unique_ptr<RecordingSelfSignedCodec> codec = std::make_unique<RecordingSelfSignedCodec>();
  std::unique_ptr<AllowAllAuthorizer> authorizer = std::make_unique<AllowAllAuthorizer>();
  std::unique_ptr<RecordingEventPublisher> events = std::make_unique<RecordingEventPublisher>();
  std::unique_ptr<RecordingAuditLogger> audit = std::make_unique<RecordingAuditLogger>();
  //  ★ 工厂也必须**每实例一份**：`ForPartition` 会改 `calls`/`last_partition`（可变状态），
  //    两个线程共用同一个工厂对象 = 数据竞争（实测挂死/堆损坏）。
  std::unique_ptr<FakeBlobStoreFactory> factory;
  std::unique_ptr<fss::app::LocationIssuer> issuer;
  std::unique_ptr<UseCasePorts> ports;
  fss::app::CallerContext caller{"opendes", "osdu-user", "Bearer test-token"};
};

// =============================================================================
//  ★ C1：只统计"复制"次数的存储装饰器
// =============================================================================
//  判据"并发登记同一 fileSource 只发生一次 staging→persistent 复制"必须**可观测**，
//  否则只能靠"两边返回同一个 id"间接推断（那证明不了没重复复制）。装饰器只转发数据面，
//  不改任何语义（与 `CapabilityOverrideBlobStore` 同一纪律）。
class CountingBlobStore final : public fss::domain::IBlobStore {
 public:
  explicit CountingBlobStore(fss::domain::IBlobStore& inner) : inner_(inner) {}

  fss::domain::BlobCapabilities capabilities() const override { return inner_.capabilities(); }
  fss::Result<void> ensure_container(const std::string& container) override {
    return inner_.ensure_container(container);
  }
  fss::Result<fss::domain::SignedLocation> presign_put(
      const fss::domain::ObjectRef& ref, const fss::domain::PresignOptions& o) override {
    return inner_.presign_put(ref, o);
  }
  fss::Result<fss::domain::SignedLocation> presign_get(
      const fss::domain::ObjectRef& ref, const fss::domain::PresignOptions& o) override {
    return inner_.presign_get(ref, o);
  }
  fss::Result<void> put(const fss::domain::ObjectRef& ref, fss::bytes::ByteSource& source,
                        const fss::domain::PutOptions& options) override {
    puts_.fetch_add(1);
    return inner_.put(ref, source, options);
  }
  fss::Result<void> get(const fss::domain::ObjectRef& ref, fss::bytes::ByteSink& sink,
                        const fss::domain::ByteRange& range) override {
    return inner_.get(ref, sink, range);
  }
  fss::Result<fss::domain::ObjectStat> stat(const fss::domain::ObjectRef& ref) override {
    return inner_.stat(ref);
  }
  fss::Result<void> remove(const fss::domain::ObjectRef& ref) override {
    return inner_.remove(ref);
  }
  fss::Result<fss::domain::ObjectStat> copy(const fss::domain::ObjectRef& from,
                                            const fss::domain::ObjectRef& to) override {
    copies_.fetch_add(1);
    return inner_.copy(from, to);
  }
  fss::Result<fss::domain::ListPage> list(const std::string& container, const std::string& prefix,
                                          const std::string& continuation_token,
                                          int limit) override {
    return inner_.list(container, prefix, continuation_token, limit);
  }
  fss::Result<fss::domain::TempSweepResult> remove_temp_files(
      const std::string& container, std::int64_t older_than_epoch_seconds,
      bool dry_run) override {
    return inner_.remove_temp_files(container, older_than_epoch_seconds, dry_run);
  }

  int copies() const { return copies_.load(); }
  int puts() const { return puts_.load(); }

 private:
  fss::domain::IBlobStore& inner_;
  std::atomic<int> copies_{0};
  std::atomic<int> puts_{0};
};

struct MultiInstanceFixture {
  fss::test::TempDir dir{"multi_instance"};
  fss::ManualClock clock{1700000000};
  //  ---- 两个实例**共享**的部分（等价于"同一套存储 + 同一个库"）----
  fss::infra::InMemoryBlobStore blob{clock};
  fss::infra::InMemoryLocationRepository locations;
  fss::infra::InMemoryMetadataRepository metadata{clock};
  FakeBlobStoreFactory factory{blob};
  FakePartitionRegistry partitions;
  NoopLegalValidator legal;
  NoopSchemaValidator schema;
  fss::app::CallerContext caller{"opendes", "osdu-user", "Bearer test-token"};

  struct Seeded {
    std::string file_source;
    fss::domain::ObjectRef staging_ref;
  };

  //  ★ 实例必须**堆分配**：`issuer`/`ports` 内部持有对 `ids`/`codec` 等成员的引用，
  //    按值返回会让这些引用在移动后悬空。
  std::unique_ptr<Instance> Make(unsigned id_seed = 1) { return MakeWith(metadata, id_seed); }

  std::unique_ptr<Instance> Make(IMetadataRepository& repository, unsigned id_seed = 1) {
    return MakeWith(repository, id_seed);
  }

  std::unique_ptr<Instance> MakeWith(IMetadataRepository& repository, unsigned id_seed) {
    auto instance = std::make_unique<Instance>();
    //  ★ `SequentialIdGenerator` 的计数器是原子的（并发用例需要）→ 不可赋值；
    //    用 `Reset()` 设置起点（P9-D01）
    instance->ids.Reset(id_seed);
    instance->factory = std::make_unique<FakeBlobStoreFactory>(blob);
    instance->issuer = std::make_unique<fss::app::LocationIssuer>(
        *instance->factory, locations, *instance->codec, clock, instance->ids,
        "https://self.invalid");
    instance->ports = std::make_unique<UseCasePorts>(UseCasePorts{
        *instance->factory, locations, repository, *instance->authorizer, *instance->events,
        *instance->audit, partitions, legal, schema, *instance->issuer, clock, instance->ids});
    return instance;
  }

  //  在 staging 放一份数据并登记位置记录，返回 FileSource（两个实例随后提交同一个它）
  Seeded SeedUpload(const std::string& payload) {
    auto uploader = Make();
    GetUploadLocation upload(*uploader->ports);
    const auto up = upload.Execute(uploader->caller, std::nullopt, "1H");
    REQUIRE(up.ok());
    const auto location = locations.Find(uploader->caller.partition, up.value().file_id);
    REQUIRE(location.ok());
    const auto ref = ObjectRefFromLocation(location.value());
    REQUIRE(ref.ok());
    fss::bytes::StringSource source(payload);
    REQUIRE(blob.put(ref.value(), source, fss::domain::PutOptions{}).ok());
    return Seeded{up.value().file_source, ref.value()};
  }
};

//  两个实例的并发提交：用"起跑栅栏"让它们真的同时进入用例
struct RaceOutcome {
  std::optional<fss::Result<std::string>> first;
  std::optional<fss::Result<std::string>> second;
  const fss::Result<std::string>& A() const { return *first; }
  const fss::Result<std::string>& B() const { return *second; }
};

RaceOutcome RaceCreate(const std::unique_ptr<Instance>& a, const std::unique_ptr<Instance>& b,
                       const FileMetadataRecord& record) {
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  RaceOutcome out;
  const auto run = [&](Instance* instance, std::optional<fss::Result<std::string>>* slot) {
    ready.fetch_add(1);
    while (!go.load()) std::this_thread::yield();
    CreateFileMetadata usecase(*instance->ports);
    slot->emplace(usecase.Execute(instance->caller, record));
  };
  std::thread first(run, a.get(), &out.first);
  std::thread second(run, b.get(), &out.second);
  while (ready.load() < 2) std::this_thread::yield();
  go.store(true);
  first.join();
  second.join();
  return out;
}

// =============================================================================
//  对照用的"错误实现"：按 **record id** 建键 + check-then-insert，没有幂等键约束
// =============================================================================
//  这正是 ADR-009 M2 记录的形态（也是 R5 的教训：约束建在随机主键上 → 幂等失效）。
//  ★ C1：用例的写路径已改成 `ClaimForWrite`（复制之前原子领取），所以对照实现也必须
//    把**领取**做成 check-then-insert —— 否则"两条记录"的对照会因为"根本没并发领取"
//    而变成另一种东西。`before_insert` 是给测试用的同步点，把"两个实例都查完、都还没插"
//    这个窗口固定下来；没有它，并发用例会时灵时不灵（R4：不稳的实验不能当证据）。
class NaiveIdKeyedMetadataRepository final : public IMetadataRepository {
 public:
  std::function<void()> before_insert;

  fss::Result<FileMetadataRecord> Create(std::string_view partition,
                                        const FileMetadataRecord& record) override {
    //  ① 先查（应用层 check）
    {
      std::lock_guard<std::mutex> guard(mutex_);
      const auto it = Find(partition, record.data.dataset_properties.file_source_info.file_source);
      if (it != records_.end()) return it->second.record;  // 已存在 → 幂等返回
    }
    if (before_insert) before_insert();
    //  ② 后插（随机主键，没有任何唯一约束兜底）
    std::lock_guard<std::mutex> guard(mutex_);
    FileMetadataRecord stored = record;
    stored.version = 1;
    records_[Key(partition, record.id)] = Row{stored, fss::domain::MetadataState::kReady};
    return stored;
  }

  //  ★ C1 对照：领取也是 check-then-insert，两个语句之间可以被另一个实例插进来。
  fss::Result<fss::domain::MetadataClaim> ClaimForWrite(
      std::string_view partition, const FileMetadataRecord& record) override {
    const std::string file_source =
        record.data.dataset_properties.file_source_info.file_source;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      const auto it = Find(partition, file_source);
      if (it != records_.end()) {
        fss::domain::MetadataClaim out;
        out.claimed = false;
        out.record = it->second.record;
        out.state = it->second.state;
        return out;
      }
    }
    if (before_insert) before_insert();  // 把"两个实例都查完"的窗口固定下来
    std::lock_guard<std::mutex> guard(mutex_);
    FileMetadataRecord stored = record;
    stored.version = 1;
    records_[Key(partition, record.id)] = Row{stored, fss::domain::MetadataState::kClaiming};
    fss::domain::MetadataClaim out;
    out.claimed = true;
    out.record = stored;
    out.state = fss::domain::MetadataState::kClaiming;
    return out;
  }

  fss::Result<FileMetadataRecord> MarkReady(std::string_view partition,
                                           std::string_view record_id, std::int64_t version,
                                           const FileMetadataRecord& record) override {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = records_.find(Key(partition, record_id));
    if (it == records_.end() || it->second.record.version != version) {
      return Err(fss::ErrorKind::kNotFound, "记录不存在");
    }
    it->second.record = record;
    it->second.record.version = version;
    it->second.state = fss::domain::MetadataState::kReady;
    return it->second.record;
  }

  fss::Result<void> ReleaseClaim(std::string_view partition, std::string_view record_id,
                                 std::int64_t version) override {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = records_.find(Key(partition, record_id));
    if (it == records_.end() || it->second.record.version != version ||
        it->second.state != fss::domain::MetadataState::kClaiming) {
      return Err(fss::ErrorKind::kNotFound, "记录不存在或不是 claiming");
    }
    records_.erase(it);
    return fss::Ok();
  }

  //  ★ C2：本对照替身只模拟"按随机主键 check-then-insert"的幂等失效，
  //    不承载租约驱动的 claiming 回收（那条语义由三个**真实**实现各自验证）。
  //    显式返回 0 而不是"悄悄什么都不做"：调用方看到的是"没有行被回收"这个明确结果。
  fss::Result<std::int64_t> ReclaimStaleClaiming(
      std::string_view partition, std::int64_t older_than_epoch_seconds, int limit,
      const std::vector<std::string>& live_expired_sources) override {
    (void)partition;
    (void)older_than_epoch_seconds;
    (void)limit;
    (void)live_expired_sources;
    return std::int64_t{0};
  }

  fss::Result<FileMetadataRecord> GetById(std::string_view partition,
                                         std::string_view record_id) override {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = records_.find(Key(partition, record_id));
    if (it == records_.end() || it->second.state != fss::domain::MetadataState::kReady) {
      return Err(fss::ErrorKind::kNotFound, "记录不存在");
    }
    return it->second.record;
  }

  fss::Result<FileMetadataRecord> GetLatestByFileSource(std::string_view partition,
                                                       std::string_view file_source) override {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = Find(partition, file_source);
    if (it == records_.end() || it->second.state != fss::domain::MetadataState::kReady) {
      return Err(fss::ErrorKind::kNotFound, "记录不存在");
    }
    return it->second.record;
  }

  fss::Result<FileMetadataRecord> Update(std::string_view partition,
                                        const FileMetadataRecord& record) override {
    return Create(partition, record);
  }

  fss::Result<void> Delete(std::string_view partition, std::string_view record_id) override {
    std::lock_guard<std::mutex> guard(mutex_);
    records_.erase(Key(partition, record_id));
    return fss::Ok();
  }

  fss::Result<MetadataPage> List(std::string_view partition,
                                 const MetadataQuery& query) override {
    std::lock_guard<std::mutex> guard(mutex_);
    MetadataPage page;
    for (const auto& [key, row] : records_) {
      if (key.rfind(std::string(partition) + "\x1f", 0) != 0) continue;
      page.records.push_back(row.record);
    }
    page.total = static_cast<std::int64_t>(page.records.size());
    (void)query;  // 对照只需要"总数"，过滤逻辑不是本用例的对象
    return page;
  }

 private:
  struct Row {
    FileMetadataRecord record;
    fss::domain::MetadataState state = fss::domain::MetadataState::kReady;
  };

  static std::string Key(std::string_view partition, std::string_view id) {
    return std::string(partition) + "\x1f" + std::string(id);
  }

  std::map<std::string, Row>::iterator Find(std::string_view partition,
                                            std::string_view file_source) {
    for (auto it = records_.begin(); it != records_.end(); ++it) {
      if (it->first.rfind(std::string(partition) + "\x1f", 0) != 0) continue;
      if (it->second.record.data.dataset_properties.file_source_info.file_source == file_source) {
        return it;
      }
    }
    return records_.end();
  }

  std::mutex mutex_;
  std::map<std::string, Row> records_;
};

}  // namespace

TEST_CASE("★ C6.11 两个实例并发提交同一 fileSource → 恰 1 条记录、1 份对象",
          "[phase6][integration][c6.11]") {
  MultiInstanceFixture fx;
  for (int round = 0; round < 5; ++round) {
    INFO("第 " << round << " 轮");
    //  ★ 每轮推进时钟：否则 FileSource（含时间戳）会与上一轮相同，第二轮就走"幂等快路径"，
    //    测不到"并发提交一个新 fileSource"。同轮两个实例仍用**同一个 id 种子**（最坏情况）。
    fx.clock.AdvanceSeconds(1);
    const std::string payload = "multi-instance-" + std::to_string(round);
    const auto seeded = fx.SeedUpload(payload);
    auto a = fx.Make(static_cast<unsigned>(round + 1));
    auto b = fx.Make(static_cast<unsigned>(round + 1));
    auto record = AppFixture::MakeRecord(seeded.file_source, "multi.bin");

    const auto outcome = RaceCreate(a, b, record);
    INFO("A: " << (outcome.A().ok() ? outcome.A().value() : outcome.A().error().ToString()));
    INFO("B: " << (outcome.B().ok() ? outcome.B().value() : outcome.B().error().ToString()));
    REQUIRE(outcome.A().ok());
    REQUIRE(outcome.B().ok());
    REQUIRE(outcome.A().value() == outcome.B().value());

    //  ★ 每轮**恰好新增 1 条**记录（并发提交没有产生重复）
    MetadataQuery query;
    const auto page = fx.metadata.List(fx.caller.partition, query);
    REQUIRE(page.ok());
    REQUIRE(page.value().total == round + 1);

    //  ★ persistent 区恰好 1 个对象，且内容就是刚上传的那份
    const auto location = fx.locations.FindByFileSource(fx.caller.partition, seeded.file_source);
    REQUIRE(location.ok());
    REQUIRE(location.value().zone == fss::domain::StorageZone::kPersistent);
    const auto ref = ObjectRefFromLocation(location.value());
    REQUIRE(ref.ok());
    const auto stat = fx.blob.stat(ref.value());
    REQUIRE(stat.ok());
    REQUIRE(stat.value().exists);
    REQUIRE(stat.value().checksum == fss::crypto::Sha256Hex(payload));

    //  staging 已被清理（两个实例都执行了第 11 步，幂等）
    const auto staging_after = fx.blob.stat(seeded.staging_ref);
    REQUIRE(staging_after.ok());
    REQUIRE_FALSE(staging_after.value().exists);
  }
}

TEST_CASE("★ C6.11 SQLite 两个连接（真·2 实例）并发提交 → 幂等返回既有记录，而不是 500",
          "[phase6][integration][c6.11]") {
  MultiInstanceFixture fx;
  //  ★ 两个**仓储对象**指向同一个库文件 = 两个连接（各自的事务与锁）——这才是真·2 实例。
  //    内存仓储靠"一个对象里的互斥量"串行化，测不出这个场景。
  auto opened_a = fss::infra::SqliteMetadataRepository::Open(fx.dir.child("metadata.db"), fx.clock);
  REQUIRE(opened_a.ok());
  auto opened_b = fss::infra::SqliteMetadataRepository::Open(fx.dir.child("metadata.db"), fx.clock);
  REQUIRE(opened_b.ok());

  //  ★ 两种最坏情况都要覆盖：
  //    `same_id = true`  → 两个实例生成**同一个记录 id** → 撞**主键**
  //    `same_id = false` → 不同的 id → 撞**幂等键唯一索引** `ux_metadata_source`
  int expected_total = 0;   // 每次并发提交只允许新增 1 条
  unsigned id_counter = 0;  // ★ id 种子必须**全局唯一**（跨两种组合也不能重复，否则会撞"同 id 不同 source"）
  for (const bool same_id : {true, false}) {
    for (int round = 0; round < 3; ++round) {
    ++expected_total;
    INFO("same_id=" << same_id << " 第 " << round << " 轮");
    fx.clock.AdvanceSeconds(1);  // 每轮不同的 FileSource
    const std::string payload =
        std::string("sqlite-multi-") + (same_id ? "same-" : "diff-") + std::to_string(round);
    const auto seeded = fx.SeedUpload(payload);
    const unsigned seed_a = ++id_counter;
    const unsigned seed_b = same_id ? seed_a : ++id_counter;
    auto a = fx.Make(*opened_a.value(), seed_a);
    auto b = fx.Make(*opened_b.value(), seed_b);
    auto record = AppFixture::MakeRecord(seeded.file_source, "sqlite.bin");

    const auto outcome = RaceCreate(a, b, record);
    INFO("A: " << (outcome.A().ok() ? outcome.A().value() : outcome.A().error().ToString()));
    INFO("B: " << (outcome.B().ok() ? outcome.B().value() : outcome.B().error().ToString()));
    //  ★ 两个都不能失败：并发插入被唯一索引挡下时必须**回读既有记录**（R5/ADR-009 M2），
    //    不能把 UNIQUE 冲突当成 500 抛给客户端
    REQUIRE(outcome.A().ok());
    REQUIRE(outcome.B().ok());
    REQUIRE(outcome.A().value() == outcome.B().value());

    MetadataQuery query;
    const auto page = opened_a.value()->List(fx.caller.partition, query);
    REQUIRE(page.ok());
    REQUIRE(page.value().total == expected_total);
    }
  }
}

TEST_CASE("★ C6.11 对照：约束建在随机主键上（check-then-insert）→ 必然出现重复记录",
          "[phase6][integration][c6.11]") {
  //  ★ 这是"错误实现"的对照：没有幂等键唯一约束时，两个实例都会插进去。
  //    它证明"恰 1 条记录"这条断言**能失败**（R1）——否则"没有重复"分不清是
  //    "约束生效"还是"测试根本没并发"。
  MultiInstanceFixture fx;
  NaiveIdKeyedMetadataRepository naive;
  std::atomic<int> checked{0};
  std::atomic<bool> both_checked{false};
  //  把 check-then-insert 的窗口固定下来：两个实例都查完（都没查到）之后才允许插入
  naive.before_insert = [&]() {
    checked.fetch_add(1);
    while (checked.load() < 2) std::this_thread::yield();
    both_checked.store(true);
  };

  const std::string payload = "naive-repo";
  const auto seeded = fx.SeedUpload(payload);
  //  ★ 两个实例用**不同**的 id 种子：真实世界每个实例生成随机 id（R5 的原始缺陷形态
  //    "约束建在随机主键上"）。若用同一个 id，这个朴素仓储的 map 会把它覆盖成 1 条，
  //    反而掩盖了"应用层 check-then-insert 会插两条"的事实。
  auto a = fx.Make(naive, 1);
  auto b = fx.Make(naive, 1000);
  auto record = AppFixture::MakeRecord(seeded.file_source, "naive.bin");

  const auto outcome = RaceCreate(a, b, record);
  REQUIRE(outcome.A().ok());
  REQUIRE(outcome.B().ok());
  REQUIRE(both_checked.load());

  MetadataQuery query;
  const auto page = naive.List(fx.caller.partition, query);
  REQUIRE(page.ok());
  //  ★ 两条记录、两个不同的 id —— 这正是 M2 的现场
  REQUIRE(page.value().total == 2);
  REQUIRE(outcome.A().value() != outcome.B().value());
}

// =============================================================================
//  ★ C1（ADR-009 §4.2）：端到端"只复制一次"与"停滞 winner"
// =============================================================================
TEST_CASE("★ C1 并发登记同一 fileSource → 恰 1 次 staging→persistent 复制，两个调用者同一 id",
          "[phase6][integration][c6.11][c1]") {
  MultiInstanceFixture fx;
  CountingBlobStore counting(fx.blob);

  fx.clock.AdvanceSeconds(1);
  const auto seeded = fx.SeedUpload("only-one-copy");
  //  ★ 同一 id 种子 = 最坏情况（两个实例本来会生成同一个记录 id）
  auto a = fx.Make(1);
  auto b = fx.Make(1);
  //  ★ 每个实例有自己的 `FakeBlobStoreFactory`（`MakeWith` 里新建）→ 必须逐个指向计数装饰器，
  //    否则复制发生在未计数的内存 store 上，"恰 1 次复制"的判据会恒为 0==1 的假失败。
  for (auto* instance : {a.get(), b.get()}) {
    instance->factory->SetZoneStore(fss::domain::StorageZone::kStaging, counting);
    instance->factory->SetZoneStore(fss::domain::StorageZone::kPersistent, counting);
  }
  auto record = AppFixture::MakeRecord(seeded.file_source, "one-copy.bin");

  const auto outcome = RaceCreate(a, b, record);
  INFO("A: " << (outcome.A().ok() ? outcome.A().value() : outcome.A().error().ToString()));
  INFO("B: " << (outcome.B().ok() ? outcome.B().value() : outcome.B().error().ToString()));
  REQUIRE(outcome.A().ok());
  REQUIRE(outcome.B().ok());
  REQUIRE(outcome.A().value() == outcome.B().value());

  //  ★★ 本切片的核心判据：**恰好一次**复制（另一个实例领不到 claim，因此不复制）。
  CAPTURE(counting.copies(), counting.puts());
  REQUIRE(counting.copies() == 1);

  //  正控：记录真的登记成功且可见（否则"只复制一次"可能只是"两边都失败了"）
  const auto latest = fx.metadata.GetLatestByFileSource(fx.caller.partition, seeded.file_source);
  REQUIRE(latest.ok());
  REQUIRE(latest.value().id == outcome.A().value());
  const auto location = fx.locations.FindByFileSource(fx.caller.partition, seeded.file_source);
  REQUIRE(location.ok());
  REQUIRE(location.value().zone == fss::domain::StorageZone::kPersistent);
}

TEST_CASE("★ C1 停滞 winner：loser 见 claiming → 503 且**不复制**；winner ready 后重试返回既有 id 且仍不复制",
          "[phase6][integration][c6.11][c1]") {
  MultiInstanceFixture fx;
  CountingBlobStore counting(fx.blob);

  fx.clock.AdvanceSeconds(1);
  const auto seeded = fx.SeedUpload("stalled-winner");

  //  ★ 用**仓储层直接领取**把 winner 停在 `claiming`（等价于"winner 的复制/校验和很慢"）。
  //    这样 loser 观察 claiming 是**确定性**的，不靠 sleep 猜时序（AGENTS §4.3）。
  auto held = AppFixture::MakeRecord(seeded.file_source, "held.bin");
  held.id = fx.caller.partition + ":dataset--File.Generic:held";
  const auto claim = fx.metadata.ClaimForWrite(fx.caller.partition, held);
  REQUIRE(claim.ok());
  REQUIRE(claim.value().claimed);

  const int copies_before = counting.copies();
  auto loser = fx.Make(7);
  //  实例自己的工厂（`MakeWith` 新建）→ 指向计数装饰器，才能断言"loser 没复制"
  loser->factory->SetZoneStore(fss::domain::StorageZone::kStaging, counting);
  loser->factory->SetZoneStore(fss::domain::StorageZone::kPersistent, counting);
  CreateFileMetadata create(*loser->ports);
  const auto blocked =
      create.Execute(loser->caller, AppFixture::MakeRecord(seeded.file_source, "loser.bin"));
  INFO("loser: " << (blocked.ok() ? std::string("ok") : blocked.error().ToString()));
  REQUIRE_FALSE(blocked.ok());
  REQUIRE(blocked.error().kind() == fss::ErrorKind::kUnavailable);  // → 503
  //  ★ 可读的原因（客户端据此判断"重试"而不是"请求错了"）
  REQUIRE(blocked.error().message().find("claiming 中") != std::string::npos);
  //  ★★ loser 一次复制都没做 —— 这就是 claim-before-copy 的目的
  REQUIRE(counting.copies() == copies_before);

  //  ★ 正控：winner ready 之后，loser 的**新一次**调用返回既有 id，且仍然零复制
  const auto marked = fx.metadata.MarkReady(fx.caller.partition, held.id, 1, held);
  REQUIRE(marked.ok());
  const auto retry =
      create.Execute(loser->caller, AppFixture::MakeRecord(seeded.file_source, "loser2.bin"));
  INFO("retry: " << (retry.ok() ? retry.value() : retry.error().ToString()));
  REQUIRE(retry.ok());
  REQUIRE(retry.value() == held.id);
  REQUIRE(counting.copies() == copies_before);
}
