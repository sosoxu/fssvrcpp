// =============================================================================
//  test_gc_lease.cpp —— C6.12：GC 租约（复现 ADR-009 M3）
// =============================================================================
//  风险 M3（ADR-009 §3 / AGENTS §4.4）：GC 按"**没有元数据记录**"判定可回收对象 →
//  多实例下实测 **20/20 误删在途上传**（上传已落盘、元数据还没写，这是每个并发上传
//  的必经窗口）。正确路径只能是"**租约到期 + 原子领取**"，而且领到之后还要**再看一次
//  元数据记录**（纵深防御）。
//
//  ★ 判据 C6.12 的三条，本文件逐条断言：
//    ① 在途对象在**租约有效期内不被删**（哪怕 staging TTL 已过）；
//    ② **租约过期且无记录**时被回收；
//    ③ 两个 GC **并发**时靠原子领取**不重复删**（每个对象恰好被删一次）。
//  另外两条重要的"反向测试"（R1/R16）：
//    · `require_lease_expiry=true` 且**完全没有租约** → 不许删（这就是 M3 的护栏）；
//    · **有元数据记录的对象永不删**（哪怕有一条过期租约指着它）。
//
//  ⚠️ 未覆盖（如实登记）：`.tmp_*` 残留清理与 transfer token 清理属于 P9（C9.25）；
//     调度/领导者选举/指标暴露同样属 P9。
// =============================================================================
#include <catch2/catch.hpp>

#include "app_fixture.h"

#include "app/tasks/gc_task.h"
#include "app/usecases/usecases.h"
#include "common/bytes/bytes.h"

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using fss::app::CreateFileMetadata;
using fss::app::GcOptions;
using fss::app::GcReport;
using fss::app::GcTask;
using fss::app::GetUploadLocation;
using fss::app::ObjectRefFromLocation;
using fss::domain::ObjectRef;
using fss::domain::StorageZone;
using fss::test::AppFixture;
using fss::test::InMemoryLeaseRepository;

//  放一份 staging 数据 + 位置记录（**不**登记元数据）—— 模拟"上传已落盘、记录还没写"
struct StagedUpload {
  std::string file_id;
  std::string file_source;
  ObjectRef staging_ref;
};

StagedUpload Stage(AppFixture& fx, const std::string& payload) {
  GetUploadLocation upload(*fx.ports);
  const auto up = upload.Execute(fx.caller, std::nullopt, "1H");
  REQUIRE(up.ok());
  const auto location = fx.locations.Find(fx.caller.partition, up.value().file_id);
  REQUIRE(location.ok());
  const auto ref = ObjectRefFromLocation(location.value());
  REQUIRE(ref.ok());
  fss::bytes::StringSource source(payload);
  REQUIRE(fx.blob.put(ref.value(), source, fss::domain::PutOptions{}).ok());
  return StagedUpload{up.value().file_id, up.value().file_source, ref.value()};
}

//  推进时间。★ 必须**同时**推进租约仓储的时钟：租约的到期判定用的不是服务时钟，
//  而是"数据库时钟"（ADR-009 §6.4：一律用数据库 `now()`，否则时钟偏移会误判）。
//  内存租约替身用 `SetNowMillis` 代表那一侧；真实实现（PG）由 `now()` 提供。
void AdvanceHours(AppFixture& fx, InMemoryLeaseRepository& leases, std::int64_t hours) {
  fx.clock.AdvanceSeconds(hours * 3600);
  leases.SetNowMillis(fx.clock.NowEpochSeconds() * 1000);
}

bool StillExists(AppFixture& fx, const ObjectRef& ref) {
  const auto stat = fx.blob.stat(ref);
  REQUIRE(stat.ok());
  return stat.value().exists;
}

}  // namespace

TEST_CASE("★ C6.12 dry-run（默认）：只报告候选、一个对象都不删", "[phase6][integration][c6.12]") {
  AppFixture fx;
  InMemoryLeaseRepository leases;
  const auto staged = Stage(fx, "dry-run-payload");
  //  在途租约 + 让它过期（模拟实例崩溃后留下过期租约）
  REQUIRE(leases.Acquire(fx.caller.partition, staged.file_id, "instance-1", 1000).ok());
  AdvanceHours(fx, leases, 48);

  GcTask gc(*fx.ports, leases, "gc-instance");
  GcOptions options;  // dry_run = true（默认）
  const auto report = gc.Run(fx.caller.partition, options);
  INFO(report.value().deleted_file_ids.size());
  REQUIRE(report.ok());
  REQUIRE(report.value().dry_run);
  REQUIRE(report.value().expired_leases_claimed == 1);
  REQUIRE(report.value().deleted_objects == 1);  // 候选
  //  ★ 但**什么都没删**：对象、位置记录、租约都还在
  REQUIRE(StillExists(fx, staged.staging_ref));
  REQUIRE(fx.locations.Find(fx.caller.partition, staged.file_id).ok());
}

TEST_CASE("★ C6.12 ① 租约有效期内不删（哪怕 staging TTL 早已过去）",
          "[phase6][integration][c6.12]") {
  AppFixture fx;
  InMemoryLeaseRepository leases;
  const auto staged = Stage(fx, "in-flight-payload");
  //  在途上传：租约有效期 1 小时，而 staging TTL 只有 24 小时 → 先把时钟推到 48 小时后
  REQUIRE(leases.Acquire(fx.caller.partition, staged.file_id, "instance-1", 3600 * 1000).ok());
  AdvanceHours(fx, leases, 48);
  //  续租（模拟"上传还在进行"）
  REQUIRE(leases.Renew(fx.caller.partition, staged.file_id, "instance-1", 3600 * 1000).ok());

  GcTask gc(*fx.ports, leases, "gc-instance");
  GcOptions options;
  options.dry_run = false;  // 真删
  const auto report = gc.Run(fx.caller.partition, options);
  REQUIRE(report.ok());
  REQUIRE(report.value().expired_leases_claimed == 0);  // 没有过期租约可领
  REQUIRE(report.value().deleted_objects == 0);
  //  ★ 在途对象必须还在（M3 的反面）
  REQUIRE(StillExists(fx, staged.staging_ref));
}

TEST_CASE("★ C6.12 ② 租约过期且无元数据记录 → 被回收（对象 + 位置记录）",
          "[phase6][integration][c6.12]") {
  AppFixture fx;
  InMemoryLeaseRepository leases;
  const auto staged = Stage(fx, "reclaim-me");
  REQUIRE(leases.Acquire(fx.caller.partition, staged.file_id, "dead-instance", 1000).ok());
  AdvanceHours(fx, leases, 2);  // 租约过期（TTL 1 秒）

  GcTask gc(*fx.ports, leases, "gc-instance");
  GcOptions options;
  options.dry_run = false;
  const auto report = gc.Run(fx.caller.partition, options);
  REQUIRE(report.ok());
  REQUIRE(report.value().expired_leases_claimed == 1);
  REQUIRE(report.value().deleted_objects == 1);
  REQUIRE(report.value().deleted_locations == 1);
  REQUIRE(report.value().deleted_file_ids == std::vector<std::string>{staged.file_id});
  //  ★ 对象与位置记录都被回收
  REQUIRE_FALSE(StillExists(fx, staged.staging_ref));
  REQUIRE_FALSE(fx.locations.Find(fx.caller.partition, staged.file_id).ok());

  //  再跑一轮：已经没有过期租约可领 → 不重复删（幂等）
  const auto second = gc.Run(fx.caller.partition, options);
  REQUIRE(second.ok());
  REQUIRE(second.value().expired_leases_claimed == 0);
  REQUIRE(second.value().deleted_objects == 0);
}

TEST_CASE("★ C6.12 反向测试：完全没有租约时**不许**按'没有记录'回收（M3 的护栏）",
          "[phase6][integration][c6.12]") {
  AppFixture fx;
  InMemoryLeaseRepository leases;
  const auto staged = Stage(fx, "no-lease-at-all");
  AdvanceHours(fx, leases, 48);  // 远超 staging TTL

  GcTask gc(*fx.ports, leases, "gc-instance");
  GcOptions options;  // require_lease_expiry = true（多实例默认）
  options.dry_run = false;
  const auto report = gc.Run(fx.caller.partition, options);
  REQUIRE(report.ok());
  REQUIRE(report.value().expired_leases_claimed == 0);
  //  ★ 一个都没删：多实例下"没有记录"绝不等于"可以回收"
  REQUIRE(report.value().deleted_objects == 0);
  REQUIRE(StillExists(fx, staged.staging_ref));

  //  正例对照（R16）：显式降级成单实例模式（`require_lease_expiry=false`）后，
  //  同一个对象**必须**被按 TTL 回收 —— 否则上面那条"没删"无法区分
  //  "护栏生效"与"GC 根本没扫到它"。
  GcOptions single_instance;
  single_instance.dry_run = false;
  single_instance.require_lease_expiry = false;
  const auto second = gc.Run(fx.caller.partition, single_instance);
  REQUIRE(second.ok());
  REQUIRE(second.value().deleted_objects == 1);
  REQUIRE_FALSE(StillExists(fx, staged.staging_ref));
}

TEST_CASE("★ C6.12 反向测试：有元数据记录的对象**永不删**（哪怕过期租约指着它）",
          "[phase6][integration][c6.12]") {
  AppFixture fx;
  InMemoryLeaseRepository leases;
  //  走完整流程：上传 → 登记元数据（对象搬到 persistent、记录已写）
  const auto staged = Stage(fx, "has-record");
  auto record = AppFixture::MakeRecord(staged.file_source, "gc-record.bin");
  CreateFileMetadata create(*fx.ports);
  const auto id = create.Execute(fx.caller, record);
  REQUIRE(id.ok());
  const auto location = fx.locations.FindByFileSource(fx.caller.partition, staged.file_source);
  REQUIRE(location.ok());
  REQUIRE(location.value().zone == StorageZone::kPersistent);
  const auto persistent_ref = ObjectRefFromLocation(location.value());
  REQUIRE(persistent_ref.ok());

  //  人为造一条"指着这条记录"的过期租约（模拟崩溃的实例）
  REQUIRE(leases.Acquire(fx.caller.partition, location.value().file_id, "dead", 1000).ok());
  AdvanceHours(fx, leases, 100);  // 租约过期、且超过 orphan 宽限期

  GcTask gc(*fx.ports, leases, "gc-instance");
  GcOptions options;
  options.dry_run = false;
  const auto report = gc.Run(fx.caller.partition, options);
  REQUIRE(report.ok());
  REQUIRE(report.value().expired_leases_claimed == 1);
  REQUIRE(report.value().skipped_has_record == 1);  // ★ 被记录挡住
  REQUIRE(report.value().deleted_objects == 0);
  //  对象、位置记录、元数据记录都必须在
  REQUIRE(StillExists(fx, persistent_ref.value()));
  REQUIRE(fx.locations.FindByFileSource(fx.caller.partition, staged.file_source).ok());
  REQUIRE(fx.metadata.GetById(fx.caller.partition, id.value()).ok());
}

TEST_CASE("★ C6.12 ③ 两个 GC 并发：靠原子领取**每个对象恰好删一次**",
          "[phase6][integration][c6.12]") {
  AppFixture fx;
  InMemoryLeaseRepository leases;
  constexpr int kObjects = 8;
  std::set<std::string> file_ids;
  std::vector<ObjectRef> refs;
  for (int i = 0; i < kObjects; ++i) {
    const auto staged = Stage(fx, "concurrent-" + std::to_string(i));
    REQUIRE(leases.Acquire(fx.caller.partition, staged.file_id, "dead", 1000).ok());
    file_ids.insert(staged.file_id);
    refs.push_back(staged.staging_ref);
  }
  AdvanceHours(fx, leases, 2);  // 全部租约过期
  REQUIRE(file_ids.size() == kObjects);

  GcOptions options;
  options.dry_run = false;
  GcReport first;
  GcReport second;
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  const auto run = [&](GcTask& gc, GcReport* out) {
    ready.fetch_add(1);
    while (!go.load()) std::this_thread::yield();
    const auto report = gc.Run(fx.caller.partition, options);
    REQUIRE(report.ok());
    *out = report.value();
  };
  //  ★ 两个**不同的实例 id**：`ClaimExpired` 的原子性保证同一条租约只被一个 GC 领走
  GcTask gc_a(*fx.ports, leases, "gc-A");
  GcTask gc_b(*fx.ports, leases, "gc-B");
  std::thread thread_a(run, std::ref(gc_a), &first);
  std::thread thread_b(run, std::ref(gc_b), &second);
  while (ready.load() < 2) std::this_thread::yield();
  go.store(true);
  thread_a.join();
  thread_b.join();

  INFO("A 删了 " << first.deleted_objects << "，B 删了 " << second.deleted_objects);
  //  ★ 合计恰好 kObjects：没有对象被删两次
  REQUIRE(first.expired_leases_claimed + second.expired_leases_claimed == kObjects);
  REQUIRE(first.deleted_objects + second.deleted_objects == kObjects);
  std::set<std::string> deleted(first.deleted_file_ids.begin(), first.deleted_file_ids.end());
  for (const auto& id : second.deleted_file_ids) {
    REQUIRE(deleted.insert(id).second);  // 两个报告的交集必须为空
  }
  REQUIRE(deleted == file_ids);
  //  所有对象都真的没了
  for (const auto& ref : refs) REQUIRE_FALSE(StillExists(fx, ref));
}
