// =============================================================================
//  test_postgres_lease_lifecycle.cpp —— C2：PG 租约的 TTL/time_source 可观测性
// =============================================================================
//  只有真实 PostgreSQL 才能验证的东西：
//    · `leases.ttl_seconds` **真的有可观测效果**：发完 uploadURL 后 `staging_leases`
//      里有一行，且 `expires_at - now() ≈ ttl_seconds`（直接查表 —— 正控：行真的在）；
//    · `leases.time_source=local` 真的跟随注入的 `IClock`（与 `database` 模式形成对照）；
//    · `Acquire`/`Renew`/`ClaimExpired` 在**两种**时间基准下都互相一致（local 模式跑
//      与 database 模式相同的共享契约测试）；
//    · GC 的"崩溃→回收→重试"在 PG 元数据仓储 + PG 租约（local 钟）上端到端成立。
//
//  ★ 连不上就失败，绝不静默跳过（跳过的门槛是空证据）；DSN 走 `FSS_PG_DSN`，
//    因此同一份二进制可指向本机 14.24 或远端 12.6。
//  ★ 每个 TEST_CASE 用 `pgtest-<pid>-<n>-<tag>` 唯一 partition；RAII 清理自己的行。
// =============================================================================
#include <catch2/catch.hpp>

#include "app_fixture.h"
#include "port_contract.h"

#include "app/tasks/gc_task.h"
#include "app/usecases/usecases.h"
#include "common/bytes/bytes.h"
#include "infra/location/memory/memory_location_repository.h"
#include "infra/location/postgres/postgres_lease_repository.h"
#include "infra/metadata/postgres/postgres_metadata_repository.h"
#include "infra/postgres/pg_connection.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using fss::infra::LeaseTimeSource;
using fss::infra::PgConnection;
using fss::infra::PgOptions;
using fss::infra::PostgresLeaseRepository;
using fss::infra::PostgresLeaseRepositoryOptions;
using fss::infra::PostgresMetadataRepository;
using fss::infra::PostgresMetadataRepositoryOptions;

namespace domain = fss::domain;

namespace {

constexpr const char* kDefaultDsn = "postgresql://fss@127.0.0.1:15432/fss";

std::string TestDsn() {
  const char* env = std::getenv("FSS_PG_DSN");
  if (env != nullptr && *env != '\0') return std::string(env);
  return kDefaultDsn;
}

PgOptions TestPgOptions(int max_connections = 2) {
  PgOptions options;
  options.dsn = TestDsn();
  options.max_connections = max_connections;
  options.statement_timeout_millis = 5000;
  return options;
}

std::string UniquePrefix(const std::string& tag) {
  static std::atomic<int> counter{0};
  return "pgtest-" + std::to_string(static_cast<long>(::getpid())) + "-" +
         std::to_string(++counter) + "-" + tag;
}

void CleanupPrefix(const std::string& prefix) {
  auto connection = PgConnection::Connect(TestPgOptions(1));
  if (!connection.ok()) return;
  const std::string pattern = prefix + "%";
  (void)connection.value()->ExecParams(
      R"sql(DELETE FROM staging_leases WHERE partition_id LIKE $1)sql", {pattern});
  (void)connection.value()->ExecParams(
      R"sql(DELETE FROM file_locations WHERE partition_id LIKE $1)sql", {pattern});
  (void)connection.value()->ExecParams(
      R"sql(DELETE FROM file_metadata_records WHERE partition_id LIKE $1)sql", {pattern});
}

class PrefixGuard {
 public:
  explicit PrefixGuard(std::string prefix) : prefix_(std::move(prefix)) {
    CleanupPrefix(prefix_);
  }
  ~PrefixGuard() { CleanupPrefix(prefix_); }
  PrefixGuard(const PrefixGuard&) = delete;
  PrefixGuard& operator=(const PrefixGuard&) = delete;

 private:
  std::string prefix_;
};

//  直接查 `staging_leases`：返回 {行数, owner, expires_at epoch 秒, expires_at - now() 秒}。
struct LeaseRow {
  int rows = 0;
  std::string owner;
  std::int64_t expires_epoch_seconds = 0;
  std::int64_t ttl_from_db_now_seconds = 0;
  std::string file_source;
};

LeaseRow QueryLeaseRow(const std::string& partition, const std::string& file_source) {
  LeaseRow row;
  auto connection = PgConnection::Connect(TestPgOptions(1));
  REQUIRE(connection.ok());
  const auto result = connection.value()->ExecParams(
      R"sql(SELECT owner,
                   extract(epoch from expires_at)::bigint,
                   extract(epoch from (expires_at - now()))::bigint,
                   file_source
              FROM staging_leases
             WHERE partition_id = $1 AND file_source = $2)sql",
      {partition, file_source});
  REQUIRE(result.ok());
  row.rows = result.value().RowCount();
  if (row.rows > 0) {
    row.owner = result.value().Value(0, 0);
    row.expires_epoch_seconds =
        std::strtoll(result.value().Value(0, 1).c_str(), nullptr, 10);
    row.ttl_from_db_now_seconds =
        std::strtoll(result.value().Value(0, 2).c_str(), nullptr, 10);
    row.file_source = result.value().Value(0, 3);
  }
  return row;
}

std::int64_t DbNowEpochSeconds() {
  auto connection = PgConnection::Connect(TestPgOptions(1));
  REQUIRE(connection.ok());
  const auto result =
      connection.value()->ExecSimple(R"sql(SELECT extract(epoch from now())::bigint)sql");
  REQUIRE(result.ok());
  return std::strtoll(result.value().Value(0, 0).c_str(), nullptr, 10);
}

void AttachLeases(fss::test::AppFixture& fx, domain::ILeaseRepository& leases,
                  const std::string& instance_id, std::int64_t ttl_seconds,
                  std::int64_t renew_seconds) {
  fx.ports->leases = &leases;
  fx.ports->leases_enabled = true;
  fx.ports->instance_id = instance_id;
  fx.ports->lease_ttl_seconds = ttl_seconds;
  fx.ports->lease_renew_interval_seconds = renew_seconds;
}

}  // namespace

// =============================================================================
//  ① ttl_seconds 可观测（PG 直接查表）
// =============================================================================
TEST_CASE("★ C2 PG：发完 uploadURL 后 staging_leases 有行，expires_at-now ≈ leases.ttl_seconds",
          "[pg][infra][c2]") {
  static const std::string prefix = UniquePrefix("lease-ttl");
  PrefixGuard guard(prefix);
  const std::string partition = prefix + "-p";

  PostgresLeaseRepositoryOptions options;
  options.pg = TestPgOptions(2);
  auto opened = PostgresLeaseRepository::Open(options);
  CAPTURE(opened.ok() ? std::string("ok") : opened.error().message());
  REQUIRE(opened.ok());

  fss::test::AppFixture fx;
  fx.caller.partition = partition;
  constexpr std::int64_t kTtlSeconds = 120;
  AttachLeases(fx, *opened.value(), "instance-A", kTtlSeconds, 20);

  fss::app::GetUploadLocation upload(*fx.ports);
  const auto up = upload.Execute(fx.caller, std::nullopt, "1H");
  CAPTURE(up.ok() ? std::string("ok") : up.error().message());
  REQUIRE(up.ok());

  const auto row = QueryLeaseRow(partition, up.value().file_source);
  //  ★ 正控：行真的被写出来了（否则下面的数值断言可能是"读了一行不存在的空值"）
  REQUIRE(row.rows == 1);
  REQUIRE(row.owner == "instance-A");
  REQUIRE(row.file_source == up.value().file_source);
  //  ★ 可观测效果：expires_at - now() ≈ ttl_seconds（±5 s 容差覆盖一次网络往返）
  CAPTURE(row.ttl_from_db_now_seconds);
  REQUIRE(std::llabs(row.ttl_from_db_now_seconds - kTtlSeconds) <= 5);
}

// =============================================================================
//  ② time_source：local 跟随注入时钟；database 跟随服务器 —— 两者都与契约一致
// =============================================================================
TEST_CASE("★ C2 PG time_source=local 通过与 database 相同的租约契约测试",
          "[pg][infra][c2]") {
  static const std::string prefix = UniquePrefix("lease-local");
  PrefixGuard guard(prefix);

  //  手动钟故意远离墙钟：如果实现偷偷用 `now()`，契约里的"到期/领取"判据会失配。
  fss::ManualClock clock{1000000000};
  PostgresLeaseRepositoryOptions options;
  options.pg = TestPgOptions(4);
  options.time_source = LeaseTimeSource::kLocal;
  options.clock = &clock;
  auto opened = PostgresLeaseRepository::Open(options);
  CAPTURE(opened.ok() ? std::string("ok") : opened.error().message());
  REQUIRE(opened.ok());
  //  正控：local 模式必须真的接受注入时钟（与 database 模式跑同一套断言）
  fss::test::CheckLeaseContract(*opened.value(), clock, prefix);
}

TEST_CASE("★ C2 PG time_source：local 跟随 ManualClock / database 跟随服务器（对照）",
          "[pg][infra][c2]") {
  static const std::string prefix = UniquePrefix("lease-clock");
  PrefixGuard guard(prefix);
  const std::string p_local = prefix + "-local";
  const std::string p_db = prefix + "-db";
  constexpr std::int64_t kManualEpoch = 1000000000;  // 2001-09-09，远离墙钟
  constexpr std::int64_t kTtlSeconds = 60;

  fss::ManualClock clock{kManualEpoch};
  PostgresLeaseRepositoryOptions local_options;
  local_options.pg = TestPgOptions(2);
  local_options.time_source = LeaseTimeSource::kLocal;
  local_options.clock = &clock;
  auto local = PostgresLeaseRepository::Open(local_options);
  REQUIRE(local.ok());

  PostgresLeaseRepositoryOptions db_options;
  db_options.pg = TestPgOptions(2);
  db_options.time_source = LeaseTimeSource::kDatabase;
  auto db = PostgresLeaseRepository::Open(db_options);
  REQUIRE(db.ok());

  //  ---- local：expires_at = to_timestamp(ManualClock + ttl) ----
  REQUIRE(local.value()->Acquire(p_local, "/local/key", "A", kTtlSeconds * 1000).ok());
  const auto local_row = QueryLeaseRow(p_local, "/local/key");
  REQUIRE(local_row.rows == 1);  // 正控：行真的在
  CAPTURE(local_row.expires_epoch_seconds, kManualEpoch + kTtlSeconds);
  REQUIRE(std::llabs(local_row.expires_epoch_seconds - (kManualEpoch + kTtlSeconds)) <= 1);
  //  ★ 与墙钟相差极大 → 证明它用的是注入时钟，不是服务器 now()
  const std::int64_t wall_now = DbNowEpochSeconds();
  REQUIRE(std::llabs(local_row.expires_epoch_seconds - wall_now) > 100000000);

  //  ---- local 三原语一致：未到点不领、到点才领 ----
  {
    const auto early = local.value()->ClaimExpired(p_local, 10, "gc");
    REQUIRE(early.ok());
    REQUIRE(early.value().empty());  // now = T，expiry = T+60 → 不能领
  }
  clock.SetEpochSeconds(kManualEpoch + 30);
  REQUIRE(local.value()->Renew(p_local, "/local/key", "A", kTtlSeconds * 1000).ok());
  const auto renewed_row = QueryLeaseRow(p_local, "/local/key");
  REQUIRE(std::llabs(renewed_row.expires_epoch_seconds -
                     (kManualEpoch + 30 + kTtlSeconds)) <= 1);
  clock.SetEpochSeconds(kManualEpoch + 80);  // 未到 renew 后的到期点（T+90）
  {
    const auto early = local.value()->ClaimExpired(p_local, 10, "gc");
    REQUIRE(early.ok());
    REQUIRE(early.value().empty());
  }
  clock.SetEpochSeconds(kManualEpoch + 95);  // 越过 T+90
  {
    const auto claimed = local.value()->ClaimExpired(p_local, 10, "gc");
    REQUIRE(claimed.ok());
    REQUIRE(claimed.value().size() == 1);
    REQUIRE(claimed.value().front().owner_instance_id == "gc");
  }

  //  ---- database（对照）：expires_at 跟随服务器 now() ----
  REQUIRE(db.value()->Acquire(p_db, "/db/key", "A", kTtlSeconds * 1000).ok());
  const auto db_row = QueryLeaseRow(p_db, "/db/key");
  REQUIRE(db_row.rows == 1);  // 正控
  CAPTURE(db_row.ttl_from_db_now_seconds);
  REQUIRE(std::llabs(db_row.ttl_from_db_now_seconds - kTtlSeconds) <= 5);
  //  对照：database 模式的用**同一个** ManualClock 也解释不出它的 expires_at
  REQUIRE(std::llabs(db_row.expires_epoch_seconds - (kManualEpoch + kTtlSeconds)) >
          100000000);
}

// =============================================================================
//  ③ PG 端到端：崩溃 → GC 回收 claiming 行 + 孤儿对象 → 同 fileSource 可重试
// =============================================================================
TEST_CASE("★ C2 PG：GC 按'已领取的过期租约'回收 claiming 行（活租约不回收的正控）",
          "[pg][infra][c2]") {
  static const std::string prefix = UniquePrefix("lease-crash");
  PrefixGuard guard(prefix);
  const std::string partition = prefix + "-p";

  //  local 时间源 + 同一个 ManualClock 同时驱动 PG 租约与 PG 元数据的 created_at
  //  ⇒ 整个"崩溃→回收→重试"完全确定性（没有 sleep、没有墙钟竞态）。
  //  ★ 必须用**同一个**时钟对象：GC 的年龄阈值取自 `UseCasePorts::clock`；若租约/元数据
  //    用另一个 ManualClock，就会出现"租约已过期但年龄护栏挡住回收"的假失败。
  fss::test::AppFixture fx;
  fx.caller.partition = partition;
  fx.partitions.Add(fss::domain::PartitionConfig{partition});

  PostgresLeaseRepositoryOptions lease_options;
  lease_options.pg = TestPgOptions(2);
  lease_options.time_source = LeaseTimeSource::kLocal;
  lease_options.clock = &fx.clock;
  auto leases = PostgresLeaseRepository::Open(lease_options);
  REQUIRE(leases.ok());

  PostgresMetadataRepositoryOptions meta_options;
  meta_options.pg = TestPgOptions(2);
  auto metadata = PostgresMetadataRepository::Open(meta_options, fx.clock);
  REQUIRE(metadata.ok());

  fx.UseMetadata(*metadata.value());
  AttachLeases(fx, *leases.value(), "crasher", /*ttl_seconds=*/1, /*renew_seconds=*/20);

  //  1. 发地址（PG 租约 Acquire）+ 客户端写入 staging 字节
  std::string file_id;
  std::string file_source;
  fss::domain::ObjectRef staging_ref;
  fss::domain::FileLocation saved_location;
  {
    fss::app::GetUploadLocation upload(*fx.ports);
    const auto up = upload.Execute(fx.caller, std::nullopt, "1H");
    REQUIRE(up.ok());
    file_id = up.value().file_id;
    file_source = up.value().file_source;
    const auto location = fx.locations.Find(partition, file_id);
    REQUIRE(location.ok());
    saved_location = location.value();
    const auto ref = fss::app::ObjectRefFromLocation(saved_location);
    REQUIRE(ref.ok());
    staging_ref = ref.value();
  }
  {
    fss::bytes::StringSource source("pg-crash-payload");
    REQUIRE(fx.blob.put(staging_ref, source, fss::domain::PutOptions{}).ok());
  }

  //  2. 模拟领取后崩溃：直接 ClaimForWrite（PG 行 state='claiming'），什么都不收尾
  auto record = fss::test::AppFixture::MakeRecord(file_source, "pg-crashed.bin");
  record.id = partition + ":dataset--File.Generic:crashed-pg-1";
  record.version = 1;
  {
    const auto claim = metadata.value()->ClaimForWrite(partition, record);
    REQUIRE(claim.ok());
    REQUIRE(claim.value().claimed);
    REQUIRE(claim.value().state == domain::MetadataState::kClaiming);
  }

  fss::app::GcOptions options;
  options.dry_run = false;
  options.require_lease_expiry = true;
  options.lease_ttl_seconds = 1;
  fss::app::GcTask gc(*fx.ports, *leases.value(), "gc-instance");

  //  3. 活租约阶段：推进时间越过年龄阈值，但续租让租约活着 → 不得回收
  fx.clock.AdvanceSeconds(10);
  REQUIRE(leases.value()->Renew(partition, file_source, "crasher", 1000).ok());
  {
    const auto report = gc.Run(partition, options);
    REQUIRE(report.ok());
    REQUIRE(report.value().expired_leases_claimed == 0);
    REQUIRE(report.value().reclaimed_claiming == 0);
    REQUIRE(report.value().deleted_objects == 0);
    const auto stat = fx.blob.stat(staging_ref);
    REQUIRE(stat.ok());
    REQUIRE(stat.value().exists);
    //  正控：claiming 行仍在 PG 里
    const auto probe = metadata.value()->ClaimForWrite(partition, record);
    REQUIRE(probe.ok());
    REQUIRE_FALSE(probe.value().claimed);
    REQUIRE(probe.value().state == domain::MetadataState::kClaiming);
  }

  //  4. 过期阶段：不再续租，越过 TTL → GC 回收 claiming 行 + 孤儿对象
  fx.clock.AdvanceSeconds(5);
  {
    const auto report = gc.Run(partition, options);
    REQUIRE(report.ok());
    REQUIRE(report.value().expired_leases_claimed == 1);
    REQUIRE(report.value().reclaimed_claiming == 1);
    REQUIRE(report.value().deleted_objects == 1);
    const auto stat = fx.blob.stat(staging_ref);
    REQUIRE(stat.ok());
    REQUIRE_FALSE(stat.value().exists);
    REQUIRE_FALSE(metadata.value()->GetLatestByFileSource(partition, file_source).ok());
  }

  //  5. 重试：推过 GC 领取后推后的 60 s → 同一个 fileSource 重新上传并登记必须成功
  fx.clock.AdvanceSeconds(61);
  REQUIRE(fx.locations.Save(partition, saved_location).ok());
  {
    fss::bytes::StringSource source("pg-retry-payload");
    REQUIRE(fx.blob.put(staging_ref, source, fss::domain::PutOptions{}).ok());
  }
  {
    fss::app::CreateFileMetadata create(*fx.ports);
    const auto retried = create.Execute(fx.caller, record);
    CAPTURE(retried.ok() ? std::string("ok") : retried.error().message());
    REQUIRE(retried.ok());
    const auto ready = metadata.value()->GetById(partition, retried.value());
    REQUIRE(ready.ok());
    REQUIRE(ready.value().data.dataset_properties.file_source_info.file_source == file_source);
  }
}
