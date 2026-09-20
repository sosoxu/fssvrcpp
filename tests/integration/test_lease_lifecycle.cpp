// =============================================================================
//  test_lease_lifecycle.cpp —— C2（ADR-009 §4.2/§4.3）：在途租约真的生效
// =============================================================================
//  本切片把 `leases.{ttl_seconds,renew_interval_seconds}` 从「已读但无效果」变成生效：
//    · 上传路径 `GetUploadLocation` 发地址时 `Acquire` 租约（`leases.enabled=true`）；
//    · 登记路径 `CreateFileMetadata` 在复制/校验和期间周期 `Renew`（`LeaseRenewer`）；
//    · 崩溃者留下的 `claiming` 行由 GC 用"已原子领取的过期租约"驱动回收
//      （`IMetadataRepository::ReclaimStaleClaiming`）。
//
//  ★ 判据全部用**可轮询的真实条件**（不是固定 sleep）：
//    · 续租：用会阻塞 `copy` 的存储替身把"复制正在进行"变成确定性时点；
//    · 崩溃：直接 `ClaimForWrite` 后**故意不** MarkReady/ReleaseClaim（模拟进程被杀）；
//    · 时间：`ManualClock` + 租约替身的 `SetNowMillis` 双侧一起推进（服务钟与租约钟）。
//
//  ★ 每条否定式判据都配正控（AGENTS §4.3）：
//    · "GC 没回收" → 先证明 claiming 行确实还在、对象确实还在；
//    · "默认不 Acquire" → 先证明开着 `leases.enabled` 时租约行确实被写出来。
// =============================================================================
#include <catch2/catch.hpp>

#include "app_fixture.h"

#include "app/tasks/gc_task.h"
#include "app/usecases/usecases.h"
#include "common/bytes/bytes.h"
#include "domain/model/types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using fss::app::CreateFileMetadata;
using fss::app::GcOptions;
using fss::app::GcTask;
using fss::app::GetUploadLocation;
using fss::app::ObjectRefFromLocation;
using fss::domain::ObjectRef;
using fss::domain::StorageZone;
using fss::test::AppFixture;
using fss::test::InMemoryLeaseRepository;

// =============================================================================
//  会阻塞 `copy` 的存储替身：把"复制正在进行"变成确定性时点（不是 sleep 猜时序）
// =============================================================================
class BlockingCopyBlobStore final : public fss::domain::IBlobStore {
 public:
  explicit BlockingCopyBlobStore(fss::domain::IBlobStore& inner) : inner_(inner) {}

  void BlockCopy() {
    std::lock_guard<std::mutex> guard(mutex_);
    block_ = true;
    entered_ = false;
  }
  bool CopyEntered() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return entered_;
  }
  void ReleaseCopy() {
    std::lock_guard<std::mutex> guard(mutex_);
    block_ = false;
    cv_.notify_all();
  }

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
    {
      std::unique_lock<std::mutex> lock(mutex_);
      entered_ = true;
      cv_.notify_all();
      cv_.wait(lock, [this] { return !block_; });
    }
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

 private:
  fss::domain::IBlobStore& inner_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool block_ = false;
  bool entered_ = false;
};

bool StillExists(AppFixture& fx, const ObjectRef& ref) {
  const auto stat = fx.blob.stat(ref);
  REQUIRE(stat.ok());
  return stat.value().exists;
}

//  双侧一起推进：服务钟（fx.clock）与租约钟（替身的 now_millis_）。
//  真实 PG 下"租约钟"是数据库 now()，测试里由替身模拟；两侧不同步会让 GC 的
//  年龄阈值与租约到期判定互相矛盾。
void AdvanceBoth(AppFixture& fx, InMemoryLeaseRepository& leases, std::int64_t seconds) {
  fx.clock.AdvanceSeconds(seconds);
  leases.SetNowMillis(fx.clock.NowEpochSeconds() * 1000);
}

//  打开租约（双闸门）并设置实例身份。返回后 `fx.ports` 会 Acquire/Renew/Release。
void EnableLeases(AppFixture& fx, InMemoryLeaseRepository& leases, const std::string& instance_id,
                  std::int64_t ttl_seconds, std::int64_t renew_seconds) {
  fx.ports->leases = &leases;
  fx.ports->leases_enabled = true;
  fx.ports->instance_id = instance_id;
  fx.ports->lease_ttl_seconds = ttl_seconds;
  fx.ports->lease_renew_interval_seconds = renew_seconds;
}

//  一条"已发地址 + 已落 staging 字节"的上传（供组合场景复用）
struct Staged {
  std::string file_id;
  std::string file_source;
  ObjectRef staging_ref;
};

Staged StageOne(AppFixture& fx, const std::string& payload) {
  GetUploadLocation upload(*fx.ports);
  const auto up = upload.Execute(fx.caller, std::nullopt, "1H");
  REQUIRE(up.ok());
  const auto location = fx.locations.Find(fx.caller.partition, up.value().file_id);
  REQUIRE(location.ok());
  const auto ref = ObjectRefFromLocation(location.value());
  REQUIRE(ref.ok());
  fss::bytes::StringSource source(payload);
  REQUIRE(fx.blob.put(ref.value(), source, fss::domain::PutOptions{}).ok());
  return Staged{up.value().file_id, up.value().file_source, ref.value()};
}

//  直接建立一条 claiming 行（模拟"领取后崩溃"，不经 CreateFileMetadata 的收尾）
void ClaimDirectly(AppFixture& fx, const std::string& file_source, const std::string& record_id) {
  auto record = AppFixture::MakeRecord(file_source, record_id + ".bin");
  record.id = fx.caller.partition + ":dataset--File.Generic:" + record_id;
  record.version = 1;
  const auto claim = fx.metadata.ClaimForWrite(fx.caller.partition, record);
  REQUIRE(claim.ok());
  REQUIRE(claim.value().claimed);
}

}  // namespace

// =============================================================================
//  ① 默认（leases.enabled=false）：上传/登记路径一个租约动作都不做
// =============================================================================
TEST_CASE("★ C2 默认不接租约：leases.enabled=false 时 GetUploadLocation 不写租约",
          "[phase6][integration][c2]") {
  AppFixture fx;
  InMemoryLeaseRepository leases;
  //  ★ 正控放在前面：开着租约时必须真的写出租约行（否则下面的 nullopt 无法区分
  //    "默认关闭"与"根本没接线"）。
  EnableLeases(fx, leases, "instance-A", 60, 20);
  {
    GetUploadLocation upload(*fx.ports);
    const auto up = upload.Execute(fx.caller, std::nullopt, "1H");
    REQUIRE(up.ok());
    REQUIRE(leases.ExpiresAtMillis(fx.caller.partition, up.value().file_source).has_value());
  }

  //  关闭 → 新的一次上传地址请求不得写任何租约行
  fx.ports->leases_enabled = false;
  GetUploadLocation upload(*fx.ports);
  const auto up = upload.Execute(fx.caller, std::nullopt, "1H");
  REQUIRE(up.ok());
  REQUIRE_FALSE(leases.ExpiresAtMillis(fx.caller.partition, up.value().file_source).has_value());
}

// =============================================================================
//  ② 跨实例冲突：A 持有未过期租约 → B 的 uploadURL 请求 503 且不发地址
// =============================================================================
TEST_CASE("★ C2 跨实例租约冲突：A 持有租约时 B 的 uploadURL → kUnavailable（503）",
          "[phase6][integration][c2]") {
  AppFixture fx;
  InMemoryLeaseRepository leases;
  constexpr std::uint64_t kSeed = 7;

  //  先以"关闭租约"跑一次，取得该 (clock, id) 组合下的确定性 file_source；
  //  然后删掉它的位置记录/对象 —— 我们要制造的是"另一个实例仅在途持有租约"，
  //  而不是"这个 fileID 已经登记过位置"（后者会在 IssueUploadLocation 里先撞 400）。
  fx.ids.Reset(kSeed);
  std::string file_id;
  std::string file_source;
  {
    GetUploadLocation upload(*fx.ports);
    const auto up = upload.Execute(fx.caller, std::nullopt, "1H");
    REQUIRE(up.ok());
    file_id = up.value().file_id;
    file_source = up.value().file_source;
  }
  {
    const auto location = fx.locations.Find(fx.caller.partition, file_id);
    REQUIRE(location.ok());
    const auto ref = ObjectRefFromLocation(location.value());
    REQUIRE(ref.ok());
    REQUIRE(fx.blob.remove(ref.value()).ok());
    REQUIRE(fx.locations.Delete(fx.caller.partition, file_id).ok());
  }

  //  实例 A 持有租约（模拟它在途上传尚未完成）
  REQUIRE(leases.Acquire(fx.caller.partition, file_source, "instance-A", 60000).ok());
  EnableLeases(fx, leases, "instance-B", 60, 20);

  //  实例 B 用同一个 (clock, id) 请求同一个 file_source → 必须 503，且不发地址
  fx.ids.Reset(kSeed);
  {
    GetUploadLocation upload(*fx.ports);
    const auto up = upload.Execute(fx.caller, std::nullopt, "1H");
    REQUIRE_FALSE(up.ok());
    REQUIRE(up.error().kind() == fss::ErrorKind::kUnavailable);
    REQUIRE(up.error().message().find("在途上传") != std::string::npos);
  }
  //  正控：B 的失败请求**撤销了副作用**（位置记录不存在 → 释放后重试不会被 400 挡住）
  REQUIRE_FALSE(fx.locations.Find(fx.caller.partition, file_id).ok());

  //  正控：A 释放后，B 必须成功，且拿到的正是同一个 file_source、租约归 B
  REQUIRE(leases.Release(fx.caller.partition, file_source, "instance-A").ok());
  fx.ids.Reset(kSeed);
  {
    GetUploadLocation upload(*fx.ports);
    const auto up = upload.Execute(fx.caller, std::nullopt, "1H");
    REQUIRE(up.ok());
    REQUIRE(up.value().file_source == file_source);
    const auto expiry = leases.ExpiresAtMillis(fx.caller.partition, file_source);
    REQUIRE(expiry.has_value());
    const std::int64_t now = leases.now_millis();
    REQUIRE(*expiry > now);
    REQUIRE(*expiry <= now + 60000 + 5000);  // ttl_seconds=60
  }
}

// =============================================================================
//  ③ renew_interval_seconds 可观测：慢复制期间续租把 expires_at 往后推，
//     并发 GC 不得回收在途对象（数据丢失护栏）
// =============================================================================
TEST_CASE("★ C2 慢复制期间续租保活：GC 不得回收在途 staging 对象",
          "[phase6][integration][c2]") {
  AppFixture fx;
  InMemoryLeaseRepository leases;
  BlockingCopyBlobStore slow_copy(fx.blob);
  //  两个 zone 都指向同一个替身 → `CopyBetweenZones` 走服务端 `copy` 并被阻塞
  fx.factory.SetZoneStore(StorageZone::kStaging, slow_copy);
  fx.factory.SetZoneStore(StorageZone::kPersistent, slow_copy);
  EnableLeases(fx, leases, "instance-A", /*ttl_seconds=*/3, /*renew_seconds=*/1);

  //  1. 发地址：Acquire 租约 + 建位置记录 + 空对象
  std::string file_source;
  ObjectRef staging_ref;
  {
    GetUploadLocation upload(*fx.ports);
    const auto up = upload.Execute(fx.caller, std::nullopt, "1H");
    REQUIRE(up.ok());
    file_source = up.value().file_source;
    const auto location = fx.locations.Find(fx.caller.partition, up.value().file_id);
    REQUIRE(location.ok());
    const auto ref = ObjectRefFromLocation(location.value());
    REQUIRE(ref.ok());
    staging_ref = ref.value();
  }
  //  2. 客户端写入 staging 字节
  {
    fss::bytes::StringSource source("slow-copy-payload");
    REQUIRE(fx.blob.put(staging_ref, source, fss::domain::PutOptions{}).ok());
    REQUIRE(StillExists(fx, staging_ref));
  }
  const auto initial_expiry = leases.ExpiresAtMillis(fx.caller.partition, file_source);
  REQUIRE(initial_expiry.has_value());

  //  3. 阻塞复制，后台跑 createMetadata（claim → 复制…）
  slow_copy.BlockCopy();
  auto record = AppFixture::MakeRecord(file_source, "slow.bin");
  record.id = fx.caller.partition + ":dataset--File.Generic:slow-1";
  std::optional<fss::Result<std::string>> create_result;
  std::thread creator([&] {
    CreateFileMetadata create(*fx.ports);
    create_result.emplace(create.Execute(fx.caller, record));
  });
  const auto enter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!slow_copy.CopyEntered() && std::chrono::steady_clock::now() < enter_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  REQUIRE(slow_copy.CopyEntered());  // 前置条件显式断言（R9）

  //  4. 轮询真实条件：租约钟推进 + 续租线程每 1s 一次 → expires_at 必须往后走。
  //     推进速度约 2x 实时（50ms / 25ms），保证续租追得上（ttl=3s）。
  bool renewed = false;
  const auto renew_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < renew_deadline) {
    leases.SetNowMillis(leases.now_millis() + 50);
    const auto expiry = leases.ExpiresAtMillis(fx.caller.partition, file_source);
    REQUIRE(expiry.has_value());
    //  在途对象必须一直处于"租约未过期"的保护下
    REQUIRE(*expiry > leases.now_millis());
    if (*expiry > *initial_expiry) {
      renewed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  REQUIRE(renewed);  // ★ 续租真的发生了（expires_at 往后走了）

  //  5. 并发 GC：租约活着 → 不得领取、不得回收 claiming 行、不得删对象
  GcOptions options;
  options.dry_run = false;
  options.require_lease_expiry = true;
  options.lease_ttl_seconds = 3;
  GcTask gc(*fx.ports, leases, "gc-instance");
  const auto report = gc.Run(fx.caller.partition, options);
  REQUIRE(report.ok());
  REQUIRE(report.value().expired_leases_claimed == 0);
  REQUIRE(report.value().reclaimed_claiming == 0);
  REQUIRE(report.value().deleted_objects == 0);
  REQUIRE(StillExists(fx, staging_ref));  // ★ 在途对象必须还在（数据丢失护栏）

  //  6. 放行复制 → createMetadata 成功，租约在 MarkReady 之后被释放
  slow_copy.ReleaseCopy();
  creator.join();
  REQUIRE(create_result.has_value());
  REQUIRE(create_result->ok());
  REQUIRE_FALSE(leases.ExpiresAtMillis(fx.caller.partition, file_source).has_value());
  const auto location = fx.locations.FindByFileSource(fx.caller.partition, file_source);
  REQUIRE(location.ok());
  REQUIRE(location.value().zone == StorageZone::kPersistent);
}

// =============================================================================
//  ④ 崩溃 → 回收 → 重试（ADR-009 §4.2 的缺口）
// =============================================================================
TEST_CASE("★ C2 崩溃者 claiming 行：活租约不回收 / 过期由 GC 回收 / 同 fileSource 可重试",
          "[phase6][integration][c2]") {
  AppFixture fx;
  InMemoryLeaseRepository leases;
  EnableLeases(fx, leases, "crasher", /*ttl_seconds=*/1, /*renew_seconds=*/20);

  //  1. 正常发地址（Acquire 租约）+ 客户端写入 staging 字节
  std::string file_id;
  std::string file_source;
  ObjectRef staging_ref;
  fss::domain::FileLocation saved_location;
  {
    GetUploadLocation upload(*fx.ports);
    const auto up = upload.Execute(fx.caller, std::nullopt, "1H");
    REQUIRE(up.ok());
    file_id = up.value().file_id;
    file_source = up.value().file_source;
    const auto location = fx.locations.Find(fx.caller.partition, file_id);
    REQUIRE(location.ok());
    saved_location = location.value();
    const auto ref = ObjectRefFromLocation(saved_location);
    REQUIRE(ref.ok());
    staging_ref = ref.value();
  }
  {
    fss::bytes::StringSource source("crash-payload");
    REQUIRE(fx.blob.put(staging_ref, source, fss::domain::PutOptions{}).ok());
  }

  //  2. 模拟"领取后进程被杀"：直接 ClaimForWrite，然后**不** MarkReady/ReleaseClaim/释放租约
  auto record = AppFixture::MakeRecord(file_source, "crashed.bin");
  record.id = fx.caller.partition + ":dataset--File.Generic:crashed-1";
  record.version = 1;
  {
    const auto claim = fx.metadata.ClaimForWrite(fx.caller.partition, record);
    REQUIRE(claim.ok());
    REQUIRE(claim.value().claimed);
    REQUIRE(claim.value().state == fss::domain::MetadataState::kClaiming);
  }
  //  正控：claiming 行确实存在（第二次领取返回 claimed=false + kClaiming）
  {
    const auto probe = fx.metadata.ClaimForWrite(fx.caller.partition, record);
    REQUIRE(probe.ok());
    REQUIRE_FALSE(probe.value().claimed);
    REQUIRE(probe.value().state == fss::domain::MetadataState::kClaiming);
  }

  //  3. 活租约阶段：推进时间超过年龄阈值（证明年龄护栏不是 0），但续租保持租约活着。
  //     → GC 一轮**不得**回收 claiming 行、不得删对象（R1 注入③的正控）。
  AdvanceBoth(fx, leases, 10);
  REQUIRE(leases.Renew(fx.caller.partition, file_source, "crasher", 1000).ok());
  GcOptions options;
  options.dry_run = false;
  options.require_lease_expiry = true;
  options.lease_ttl_seconds = 1;
  GcTask gc(*fx.ports, leases, "gc-instance");
  {
    const auto report = gc.Run(fx.caller.partition, options);
    REQUIRE(report.ok());
    REQUIRE(report.value().expired_leases_claimed == 0);
    REQUIRE(report.value().reclaimed_claiming == 0);
    REQUIRE(report.value().deleted_objects == 0);
    REQUIRE(StillExists(fx, staging_ref));
    REQUIRE(fx.locations.Find(fx.caller.partition, file_id).ok());
    //  正控：claiming 行仍在（否则"没回收"可能只是"行本来就没有"）
    const auto probe = fx.metadata.ClaimForWrite(fx.caller.partition, record);
    REQUIRE(probe.ok());
    REQUIRE_FALSE(probe.value().claimed);
    REQUIRE(probe.value().state == fss::domain::MetadataState::kClaiming);
  }

  //  4. 过期阶段：不再续租，推进时间越过 TTL → GC 必须回收 claiming 行 + 孤儿对象
  AdvanceBoth(fx, leases, 5);
  {
    const auto report = gc.Run(fx.caller.partition, options);
    REQUIRE(report.ok());
    REQUIRE(report.value().expired_leases_claimed == 1);
    REQUIRE(report.value().reclaimed_claiming == 1);  // ★ ADR-009 §4.2 的缺口被补上
    REQUIRE(report.value().deleted_objects == 1);
    REQUIRE(report.value().deleted_locations == 1);
    REQUIRE_FALSE(StillExists(fx, staging_ref));
    REQUIRE_FALSE(fx.locations.Find(fx.caller.partition, file_id).ok());
    REQUIRE_FALSE(fx.metadata.GetLatestByFileSource(fx.caller.partition, file_source).ok());
  }

  //  5. 重试：GC 领走的租约被推后 60s；推过它之后，**同一个 fileSource** 重新上传并登记
  //     （重新播种 staging 对象 + 位置记录，模拟客户端再次上传）必须成功。
  AdvanceBoth(fx, leases, 61);
  REQUIRE(fx.locations.Save(fx.caller.partition, saved_location).ok());
  {
    fss::bytes::StringSource source("retry-payload");
    REQUIRE(fx.blob.put(staging_ref, source, fss::domain::PutOptions{}).ok());
  }
  {
    CreateFileMetadata create(*fx.ports);
    const auto retried = create.Execute(fx.caller, record);
    REQUIRE(retried.ok());
    const auto ready = fx.metadata.GetById(fx.caller.partition, retried.value());
    REQUIRE(ready.ok());
    REQUIRE(ready.value().data.dataset_properties.file_source_info.file_source == file_source);
  }
  //  登记成功后租约已被释放（不再占着这个 fileSource）
  REQUIRE_FALSE(leases.ExpiresAtMillis(fx.caller.partition, file_source).has_value());
}

// =============================================================================
//  ⑤ 回收是**租约驱动**的：一轮 GC 里"一条过期租约 + 一条活租约"时，
//     只能回收过期那条的 claiming 行；活租约的 claiming 行必须留存
// =============================================================================
//  为什么需要这条：如果 GC 用 `if (!claimed.empty())` 把空集合短路掉，
//  "忽略集合、只按年龄回收"的错误实现**永远不会被触发** —— 正控就成了摆设。
//  本用例在同一轮 GC 里放入两条：A 的租约已过期（会被 ClaimExpired 领到，进入集合），
//  B 的租约被续租保活（不进集合）。只听年龄的错误实现会连 B 的行一起删掉 → 失败。
TEST_CASE("★ C2 reclaim 只回收'已领取的过期租约'对应的 claiming 行（活租约必须留存）",
          "[phase6][integration][c2]") {
  AppFixture fx;
  InMemoryLeaseRepository leases;
  EnableLeases(fx, leases, "instance-A", /*ttl_seconds=*/1, /*renew_seconds=*/20);

  const auto a = StageOne(fx, "reclaim-a");
  const auto b = StageOne(fx, "keep-b");
  REQUIRE(a.file_source != b.file_source);
  ClaimDirectly(fx, a.file_source, "reclaim-a-1");
  ClaimDirectly(fx, b.file_source, "keep-b-1");

  //  推进超过 TTL 与年龄阈值；只续租 B → A 过期、B 活着
  AdvanceBoth(fx, leases, 10);
  REQUIRE(leases.Renew(fx.caller.partition, b.file_source, "instance-A", 60000).ok());

  GcOptions options;
  options.dry_run = false;
  options.require_lease_expiry = true;
  options.lease_ttl_seconds = 1;
  GcTask gc(*fx.ports, leases, "gc-instance");
  const auto report = gc.Run(fx.caller.partition, options);
  REQUIRE(report.ok());
  //  只领到 A 的过期租约 → 只回收 A 的 claiming 行
  REQUIRE(report.value().expired_leases_claimed == 1);
  REQUIRE(report.value().reclaimed_claiming == 1);
  REQUIRE(report.value().deleted_objects == 1);

  //  ★ 正控①：B 的 claiming 行**必须留存**（第二次领取返回 claimed=false + kClaiming）
  {
    auto record_b = AppFixture::MakeRecord(b.file_source, "keep-b-probe.bin");
    record_b.id = fx.caller.partition + ":dataset--File.Generic:keep-b-probe";
    const auto probe = fx.metadata.ClaimForWrite(fx.caller.partition, record_b);
    REQUIRE(probe.ok());
    REQUIRE_FALSE(probe.value().claimed);
    REQUIRE(probe.value().state == fss::domain::MetadataState::kClaiming);
  }
  //  ★ 正控②：B 的在途对象与位置记录都还在（活租约保护）
  REQUIRE(StillExists(fx, b.staging_ref));
  REQUIRE(fx.locations.Find(fx.caller.partition, b.file_id).ok());
  //  A 的对象已被回收
  REQUIRE_FALSE(StillExists(fx, a.staging_ref));
  REQUIRE_FALSE(fx.locations.Find(fx.caller.partition, a.file_id).ok());
}
