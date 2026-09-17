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
//  `before_insert` 是给测试用的同步点，把"两个实例都查完、都还没插"这个窗口固定下来；
//  没有它，并发用例会时灵时不灵（R4：不稳的实验不能当证据）。
class NaiveIdKeyedMetadataRepository final : public IMetadataRepository {
 public:
  std::function<void()> before_insert;

  fss::Result<FileMetadataRecord> Create(std::string_view partition,
                                        const FileMetadataRecord& record) override {
    //  ① 先查（应用层 check）
    {
      std::lock_guard<std::mutex> guard(mutex_);
      const auto it = Find(partition, record.data.dataset_properties.file_source_info.file_source);
      if (it != records_.end()) return it->second;  // 已存在 → 幂等返回
    }
    if (before_insert) before_insert();
    //  ② 后插（随机主键，没有任何唯一约束兜底）
    std::lock_guard<std::mutex> guard(mutex_);
    records_[Key(partition, record.id)] = record;
    return record;
  }

  fss::Result<FileMetadataRecord> GetById(std::string_view partition,
                                         std::string_view record_id) override {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = records_.find(Key(partition, record_id));
    if (it == records_.end()) return Err(fss::ErrorKind::kNotFound, "记录不存在");
    return it->second;
  }

  fss::Result<FileMetadataRecord> GetLatestByFileSource(std::string_view partition,
                                                       std::string_view file_source) override {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = Find(partition, file_source);
    if (it == records_.end()) return Err(fss::ErrorKind::kNotFound, "记录不存在");
    return it->second;
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
    for (const auto& [key, record] : records_) {
      if (key.rfind(std::string(partition) + "\x1f", 0) != 0) continue;
      page.records.push_back(record);
    }
    page.total = static_cast<std::int64_t>(page.records.size());
    (void)query;  // 对照只需要"总数"，过滤逻辑不是本用例的对象
    return page;
  }

 private:
  static std::string Key(std::string_view partition, std::string_view id) {
    return std::string(partition) + "\x1f" + std::string(id);
  }

  std::map<std::string, FileMetadataRecord>::iterator Find(std::string_view partition,
                                                           std::string_view file_source) {
    for (auto it = records_.begin(); it != records_.end(); ++it) {
      if (it->first.rfind(std::string(partition) + "\x1f", 0) != 0) continue;
      if (it->second.data.dataset_properties.file_source_info.file_source == file_source) {
        return it;
      }
    }
    return records_.end();
  }

  std::mutex mutex_;
  std::map<std::string, FileMetadataRecord> records_;
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
