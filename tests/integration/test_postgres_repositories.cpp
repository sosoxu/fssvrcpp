// =============================================================================
//  A1（ADR-009 §10）：PostgreSQL 仓储的**共享契约测试** + PG 专属并发判据
// =============================================================================
//  这一份文件把「位置仓储」与「租约」两套**与内存/SQLite 实现完全相同**的契约断言
//  跑在真实 PostgreSQL 上（C2.10 / C6.11 / C6.12）。此外补三条只有 PG 才能验证的东西：
//    · `ClaimExpired` 的 `file_id` 由 `LEFT JOIN file_locations` 反解；
//    · `ClaimExpired` 对并发领取者**非阻塞**（`FOR UPDATE SKIP LOCKED`）—— 用一条
//      **独立连接**把行锁住再调用，判据与线程到达顺序无关（AGENTS §4.3 的并发测试陷阱）；
//    · 适配层本身：options 校验、连不上 → kUnavailable（fail-closed）、有界连接池、数据库时钟。
//
//  ★ 铁律：**连不上就失败**，绝不静默跳过（跳过的门槛是空证据）。
//    DSN 来自环境变量 `FSS_PG_DSN`（未设时用本机 dev PG 的默认 DSN），
//    因此同一份二进制可以指向本机 14.24 或远端 12.6（SQL 只用 ≤12 语法）。
//
//  ★ 可重跑/不打扰别人：每个 TEST_CASE 用 `pgtest-<pid>-<n>-<tag>` 的唯一 partition
//    前缀（前缀在 SECTION 重入之间必须稳定 → 一律 `static const std::string`），
//    并用 RAII 在每个 section 结束时删掉**只属于自己前缀**的行（绝不 TRUNCATE）。
// =============================================================================
#include <catch2/catch.hpp>

#include "port_contract.h"

#include "common/json/json.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/location/postgres/postgres_lease_repository.h"
#include "infra/location/postgres/postgres_location_repository.h"
#include "infra/metadata/postgres/postgres_metadata_repository.h"
#include "infra/postgres/pg_connection.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using fss::infra::PgConnection;
using fss::infra::PgOptions;
using fss::infra::PgPool;
using fss::infra::PostgresLeaseRepository;
using fss::infra::PostgresLeaseRepositoryOptions;
using fss::infra::PostgresLocationRepository;
using fss::infra::PostgresLocationRepositoryOptions;
using fss::infra::PostgresMetadataRepository;
using fss::infra::PostgresMetadataRepositoryOptions;

//  测试体在全局命名空间：给两个会被反复书写的命名空间起短别名（仅本文件）。
namespace domain = fss::domain;
namespace json = fss::json;

namespace {

//  本机 dev PG（scripts/dev_postgres.sh）的默认 DSN；远端/其它实例用 FSS_PG_DSN 覆盖。
constexpr const char* kDefaultDsn = "postgresql://fss@127.0.0.1:15432/fss";

std::string TestDsn() {
  const char* env = std::getenv("FSS_PG_DSN");
  if (env != nullptr && *env != '\0') return std::string(env);
  return kDefaultDsn;
}

PgOptions TestPgOptions(int max_connections = 4) {
  PgOptions options;
  options.dsn = TestDsn();
  options.max_connections = max_connections;
  options.statement_timeout_millis = 5000;
  return options;
}

//  每个 TEST_CASE 唯一的 partition 前缀（pid + 进程内计数 + 标签）。
std::string UniquePrefix(const std::string& tag) {
  static std::atomic<int> counter{0};
  return "pgtest-" + std::to_string(static_cast<long>(::getpid())) + "-" +
         std::to_string(++counter) + "-" + tag;
}

//  删除**本前缀下**的行（只动自己的 partition；可重跑、不留垃圾）。
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

//  独立连接上的一次标量查询（PG 专属判据用它回读数据库的**真实行数/时间戳**）。
std::int64_t ScalarQuery(const std::string& partition) {
  auto connection = PgConnection::Connect(TestPgOptions(1));
  if (!connection.ok()) return -1;
  const auto result = connection.value()->ExecParams(
      R"sql(SELECT COUNT(*) FROM file_metadata_records WHERE partition_id = $1)sql", {partition});
  if (!result.ok() || result.value().RowCount() == 0) return -1;
  return static_cast<std::int64_t>(std::strtoll(result.value().Value(0, 0).c_str(), nullptr, 10));
}

//  直接往 `file_metadata_records` 插一行（用于制造"外部写入的墓碑行"与受控的 created_at）。
void RawInsertMetadata(const std::string& partition, const std::string& id,
                       const std::string& file_source, const std::string& state,
                       std::int64_t created_at, const std::string& name) {
  auto connection = PgConnection::Connect(TestPgOptions(1));
  REQUIRE(connection.ok());
  const std::string data = "{\"kind\":\"k\",\"acl\":{\"viewers\":[\"v@x\"],\"owners\":[\"o@x\"]},"
                           "\"legal\":{\"legaltags\":[\"t\"]},\"data\":{\"Name\":\"" + name + "\"}}";
  const auto result = connection.value()->ExecParams(
      R"sql(INSERT INTO file_metadata_records
              (partition_id, id, version, kind, state, is_latest, created_at, created_by,
               acl_viewers, acl_owners, legal_tags, file_source, data)
            VALUES ($1, $2, 1, 'k', $3, TRUE, to_timestamp($4::bigint), '',
                    '[]'::jsonb, '[]'::jsonb, '[]'::jsonb, $5, $6::jsonb))sql",
      {partition, id, state, std::to_string(created_at), file_source, data});
  INFO("RawInsertMetadata: " << (result.ok() ? std::string("ok") : result.error().message()));
  REQUIRE(result.ok());
}

//  Catch2 每个 SECTION 都会重入 TEST_CASE 体（局部对象重新构造/析构）→ 用 RAII 在
//  **每次** section 前后各清理一次：先清上次崩溃的残留，再保证本轮结束后库是干净的。
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

}  // namespace

// =============================================================================
//  一、适配层（libpq 薄封装）
// =============================================================================
TEST_CASE("PgOptions 校验：max_connections<=0 / statement_timeout<0 → kInvalidArgument",
          "[pg][infra]") {
  PgOptions base = TestPgOptions();
  //  R16 正例：合法取值必须通过（否则"拒绝"无法与"校验恒假"区分）
  REQUIRE(fss::infra::ValidatePgOptions(base).ok());
  base.statement_timeout_millis = 0;  // 0 = 关闭超时，是合法边界
  REQUIRE(fss::infra::ValidatePgOptions(base).ok());

  PgOptions bad_connections = TestPgOptions();
  bad_connections.max_connections = 0;
  const auto connections = fss::infra::ValidatePgOptions(bad_connections);
  REQUIRE_FALSE(connections.ok());
  REQUIRE(connections.error().kind() == fss::ErrorKind::kInvalidArgument);
  INFO(connections.error().message());

  PgOptions bad_timeout = TestPgOptions();
  bad_timeout.statement_timeout_millis = -1;
  const auto timeout = fss::infra::ValidatePgOptions(bad_timeout);
  REQUIRE_FALSE(timeout.ok());
  REQUIRE(timeout.error().kind() == fss::ErrorKind::kInvalidArgument);
  INFO(timeout.error().message());

  //  建池时也必须走同一套校验（不能只在 PgConnection::Connect 里校验）
  bad_connections.dsn = TestDsn();
  const auto pool = PgPool::Create(bad_connections);
  REQUIRE_FALSE(pool.ok());
  REQUIRE(pool.error().kind() == fss::ErrorKind::kInvalidArgument);
}

TEST_CASE("PgConnection：DSN 连不上 → kUnavailable + libpq 消息（fail-closed，绝不回退）",
          "[pg][infra]") {
  PgOptions bad;
  bad.dsn = "postgresql://fss@127.0.0.1:1/fss?connect_timeout=2";
  bad.max_connections = 1;
  bad.statement_timeout_millis = 1000;
  const auto connection = PgConnection::Connect(bad);
  INFO("实际错误：" << (connection.ok() ? std::string("（连接成功了？）")
                                       : connection.error().message()));
  REQUIRE_FALSE(connection.ok());
  REQUIRE(connection.error().kind() == fss::ErrorKind::kUnavailable);
  REQUIRE_FALSE(connection.error().message().empty());

  //  正控：同一个封装连**真实** DSN 必须成功 —— 证明上面的失败不是"封装恒失败"
  const auto good = PgConnection::Connect(TestPgOptions(1));
  if (!good.ok()) INFO("open error: " << good.error().message());
  REQUIRE(good.ok());
}

TEST_CASE("PgPool：有界（<= max_connections）+ 借用者等待归还 + 数据库时钟", "[pg][infra]") {
  auto pool = PgPool::Create(TestPgOptions(2));
  CAPTURE(pool.ok() ? std::string("ok") : pool.error().message());
  REQUIRE(pool.ok());
  REQUIRE(pool.value()->MaxConnections() == 2);
  //  建池即建一条连接（让 DSN 错误在 Open/启动期暴露）
  REQUIRE(pool.value()->LiveConnections() == 1);
  REQUIRE(pool.value()->IdleConnections() == 1);

  //  ★ ADR-009 §4.3/§6.4：数据库时钟。与系统时钟同量级（±60 s），且两次调用不倒退。
  const auto first = pool.value()->NowEpochMillis();
  REQUIRE(first.ok());
  const std::int64_t system_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
  CAPTURE(first.value(), system_now);
  REQUIRE(std::llabs(first.value() - system_now) < 60000);
  const auto second = pool.value()->NowEpochMillis();
  REQUIRE(second.ok());
  REQUIRE(second.value() >= first.value());

  //  借满两条（用 unique_ptr<Handle> 持有，便于中途归还一条）
  const auto borrow = [&]() -> std::unique_ptr<PgPool::Handle> {
    auto handle = pool.value()->Borrow();
    if (!handle.ok()) return nullptr;
    return std::make_unique<PgPool::Handle>(std::move(handle).value());
  };
  auto held_a = borrow();
  auto held_b = borrow();
  REQUIRE(held_a != nullptr);
  REQUIRE(held_b != nullptr);
  REQUIRE(pool.value()->LiveConnections() == 2);

  //  第三个借用者：在有人归还之前**不得**拿到连接（上限生效；负断言）
  std::atomic<bool> started{false};
  std::atomic<bool> acquired{false};
  std::thread waiter([&] {
    started.store(true);
    auto handle = pool.value()->Borrow();
    if (handle.ok()) acquired.store(true);
  });
  while (!started.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  REQUIRE_FALSE(acquired.load());

  //  正控：归还一条后，等待者必须在 5 s 内拿到（证明"等待"不是死锁、上限不是恒拒绝）
  held_a.reset();  // 销毁句柄 = 归还
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!acquired.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(acquired.load());
  waiter.join();
  REQUIRE(pool.value()->LiveConnections() <= 2);
}

// =============================================================================
//  二、位置仓储：共享契约 + partition 隔离
// =============================================================================
TEST_CASE("★ C2.10/C6.11 PostgresLocationRepository 通过与内存/SQLite 共用的契约测试",
          "[pg][infra][c2.10]") {
  //  ★ static：Catch2 每个 SECTION 都重入本函数，前缀必须稳定（否则清理跟不上）。
  static const std::string prefix = UniquePrefix("loc");
  PrefixGuard guard(prefix);

  PostgresLocationRepositoryOptions options;
  options.pg = TestPgOptions(4);
  auto opened = PostgresLocationRepository::Open(options);
  INFO("DSN=" << TestDsn());
  //  ★ CAPTURE 必须**无条件**声明在 REQUIRE 之前的同一作用域：Catch2 的 INFO/CAPTURE
  //    是 RAII 作用域消息，放进 `if (!ok())` 分支里会在 REQUIRE 之前被销毁（失败信息丢失）。
  CAPTURE(opened.ok() ? std::string("ok") : opened.error().message());
  REQUIRE(opened.ok());  // 连不上 → 大声失败，绝不 skip
  fss::test::CheckLocationRepositoryContract(*opened.value(), prefix);
}

TEST_CASE("★ PG 位置仓储：partition 严格隔离（跨租户不得命中）", "[pg][infra][c2.10]") {
  static const std::string prefix = UniquePrefix("iso");
  PrefixGuard guard(prefix);

  PostgresLocationRepositoryOptions options;
  options.pg = TestPgOptions(2);
  auto opened = PostgresLocationRepository::Open(options);
  REQUIRE(opened.ok());
  auto& repo = *opened.value();

  const std::string pa = prefix + "-a";
  const std::string pb = prefix + "-b";
  const std::string pc = prefix + "-c";
  REQUIRE(repo.Save(pa, fss::test::MakeLocation("shared-id", "/shared/source")).ok());
  REQUIRE(repo.Save(pb, fss::test::MakeLocation("shared-id", "/shared/source")).ok());

  //  正控：A/B 各自都能按 (file_id) 与 (file_source) 找到自己的那条
  REQUIRE(repo.Find(pa, "shared-id").ok());
  REQUIRE(repo.Find(pb, "shared-id").ok());
  REQUIRE(repo.FindByFileSource(pa, "/shared/source").ok());
  REQUIRE(repo.FindByFileSource(pb, "/shared/source").ok());

  //  负断言：第三个租户用同样的 file_id / file_source 不得命中
  const auto by_id = repo.Find(pc, "shared-id");
  REQUIRE_FALSE(by_id.ok());
  REQUIRE(by_id.error().kind() == fss::ErrorKind::kNotFound);
  const auto by_source = repo.FindByFileSource(pc, "/shared/source");
  REQUIRE_FALSE(by_source.ok());
  REQUIRE(by_source.error().kind() == fss::ErrorKind::kNotFound);

  //  删除 A 不得影响 B（UpdateSignedUrl 同理由 Find/分区条件保证）
  REQUIRE(repo.Delete(pa, "shared-id").ok());
  REQUIRE_FALSE(repo.Find(pa, "shared-id").ok());
  REQUIRE(repo.Find(pb, "shared-id").ok());
}

// =============================================================================
//  三、租约：共享契约 + PG 专属判据
// =============================================================================
TEST_CASE("★ C6.12 PostgresLeaseRepository 通过与内存共用的租约契约测试",
          "[pg][infra][c6.12]") {
  static const std::string prefix = UniquePrefix("lease");
  PrefixGuard guard(prefix);

  PostgresLeaseRepositoryOptions options;
  options.pg = TestPgOptions(4);
  auto opened = PostgresLeaseRepository::Open(options);
  INFO("DSN=" << TestDsn());
  CAPTURE(opened.ok() ? std::string("ok") : opened.error().message());
  REQUIRE(opened.ok());  // 连不上 → 大声失败
  fss::SystemClock clock;  // PG 的 expiry 来自数据库 now()，本机系统时钟只用于上下界
  fss::test::CheckLeaseContract(*opened.value(), clock, prefix);
}

TEST_CASE("★ PG 租约仓储：ClaimExpired 用 file_locations 反解 file_id（无位置记录则留空）",
          "[pg][infra][c6.12]") {
  static const std::string prefix = UniquePrefix("resolve");
  PrefixGuard guard(prefix);
  const std::string p = prefix + "-leases";
  const std::string with_location = p + "/with-location";
  const std::string without_location = p + "/without-location";

  //  位置记录：file_source == 租约键（ADR-009 §4.3 的对齐方式）
  PostgresLocationRepositoryOptions location_options;
  location_options.pg = TestPgOptions(2);
  auto locations = PostgresLocationRepository::Open(location_options);
  REQUIRE(locations.ok());
  REQUIRE(locations.value()->Save(p, fss::test::MakeLocation("file-resolved", with_location)).ok());

  PostgresLeaseRepositoryOptions lease_options;
  lease_options.pg = TestPgOptions(2);
  auto leases = PostgresLeaseRepository::Open(lease_options);
  REQUIRE(leases.ok());
  REQUIRE(leases.value()->Acquire(p, with_location, "owner", 0).ok());
  REQUIRE(leases.value()->Acquire(p, without_location, "owner", 0).ok());

  const auto claimed = leases.value()->ClaimExpired(p, 10, "claimer");
  REQUIRE(claimed.ok());
  REQUIRE(claimed.value().size() == 2);
  bool checked_with = false;
  bool checked_without = false;
  for (const auto& lease : claimed.value()) {
    if (lease.file_source == with_location) {
      //  正控：有位置记录 → file_id 必须被反解出来
      REQUIRE(lease.file_id == "file-resolved");
      checked_with = true;
    } else if (lease.file_source == without_location) {
      //  无位置记录 → 留空（GC 会当作"查不到位置"跳过 —— 安全方向）
      REQUIRE(lease.file_id.empty());
      checked_without = true;
    }
  }
  REQUIRE(checked_with);      // 非空洞：两条都被检查过
  REQUIRE(checked_without);
}

// =============================================================================
//  ★ 关键并发判据（R1 注入点）：ClaimExpired 必须**非阻塞**（FOR UPDATE SKIP LOCKED）
// =============================================================================
//  判据与线程到达顺序**无关**：
//    ① 另一条**独立连接**开事务，把所有过期行 `FOR UPDATE` 锁住（模拟"另一个领取者
//       正在处理这批行"）；
//    ② 正确实现用 `SKIP LOCKED` 跳过被锁的行 → 立刻返回空；
//       非原子实现（先 plain SELECT 再 UPDATE）会**阻塞**在行锁上 → 5 s 后
//       `statement_timeout`（57014）→ kUnavailable → 本用例失败。
//    ③ 正控：ROLLBACK 释放锁后，同一条查询路径必须能把 3 条全部领到 —— 证明步骤 ②
//       的空结果不是"恒空"。
TEST_CASE("★ PG 租约仓储：ClaimExpired 对并发领取者非阻塞（SKIP LOCKED 确定性判据）",
          "[pg][infra][c6.12]") {
  static const std::string prefix = UniquePrefix("skiplock");
  PrefixGuard guard(prefix);
  const std::string p = prefix + "-leases";

  PostgresLeaseRepositoryOptions options;
  options.pg = TestPgOptions(2);
  auto leases = PostgresLeaseRepository::Open(options);
  REQUIRE(leases.ok());
  for (int i = 0; i < 3; ++i) {
    REQUIRE(leases.value()->Acquire(p, "k-" + std::to_string(i), "owner", 0).ok());
  }

  auto holder = PgConnection::Connect(TestPgOptions(1));
  REQUIRE(holder.ok());
  REQUIRE(holder.value()->ExecSimple("BEGIN").ok());
  const auto locked = holder.value()->ExecParams(
      R"sql(SELECT file_source FROM staging_leases WHERE partition_id = $1 AND expires_at <= now() FOR UPDATE)sql",
      {p});
  REQUIRE(locked.ok());
  //  正控：确实锁住了 3 行（否则下面的空结果可能只是"没有行"）
  REQUIRE(locked.value().RowCount() == 3);

  //  ① 正确实现：跳过被锁的行 → 立刻返回空（不阻塞、不重复领取）
  const auto claimed = leases.value()->ClaimExpired(p, 10, "claimer");
  //  失败时必须能看到 **SQLSTATE**（正确的实现是立即返回空；非原子实现会 57014 超时）
  CAPTURE(claimed.ok() ? std::string("ok") : claimed.error().message());
  REQUIRE(claimed.ok());
  REQUIRE(claimed.value().empty());

  //  ② 正控：释放锁后必须能把 3 条全领到
  REQUIRE(holder.value()->ExecSimple("ROLLBACK").ok());
  const auto after = leases.value()->ClaimExpired(p, 10, "claimer");
  REQUIRE(after.ok());
  REQUIRE(after.value().size() == 3);
}

// =============================================================================
//  四、元数据仓储：共享契约 + PG 专属语义 + **原子领取**（ADR-009 M2）
// =============================================================================
TEST_CASE("★ C2.10/C6.11 PostgresMetadataRepository 通过与内存/SQLite 共用的元数据契约测试",
          "[pg][infra][c2.10]") {
  //  ★ static：Catch2 每个 SECTION 都重入本函数，前缀必须稳定（否则清理跟不上）。
  static const std::string prefix = UniquePrefix("meta");
  PrefixGuard guard(prefix);

  //  ★ `created_at` 来自**注入的时钟**（与 SqliteMetadataRepository 逐字一致），
  //    因此契约里的时间区间过滤在 PG 上同样可确定性测试（ADR-009 §6.4 只管租约）。
  fss::ManualClock clock{1700000000};
  PostgresMetadataRepositoryOptions options;
  options.pg = TestPgOptions(4);
  auto opened = PostgresMetadataRepository::Open(options, clock);
  INFO("DSN=" << TestDsn());
  CAPTURE(opened.ok() ? std::string("ok") : opened.error().message());
  REQUIRE(opened.ok());  // 连不上 → 大声失败，绝不 skip
  fss::test::CheckMetadataRepositoryContract(*opened.value(), clock, prefix);
}

TEST_CASE("★ PG 元数据仓储：partition 严格隔离（跨租户不得命中）", "[pg][infra][c2.10]") {
  static const std::string prefix = UniquePrefix("meta-iso");
  PrefixGuard guard(prefix);

  fss::ManualClock clock{1700000000};
  PostgresMetadataRepositoryOptions options;
  options.pg = TestPgOptions(2);
  auto opened = PostgresMetadataRepository::Open(options, clock);
  REQUIRE(opened.ok());
  auto& repo = *opened.value();

  const std::string pa = prefix + "-a";
  const std::string pb = prefix + "-b";
  const std::string pc = prefix + "-c";
  const std::string a_id = pa + ":dataset--File.Generic:shared";
  const std::string b_id = pb + ":dataset--File.Generic:shared";
  REQUIRE(repo.Create(pa, fss::test::MakeRecord(pa, "shared", "/shared/source", "in-a")).ok());
  REQUIRE(repo.Create(pb, fss::test::MakeRecord(pb, "shared", "/shared/source", "in-b")).ok());

  //  正控：A/B 各自都能按 id 与 file_source 找到自己的那条
  REQUIRE(repo.GetById(pa, a_id).ok());
  REQUIRE(repo.GetLatestByFileSource(pa, "/shared/source").ok());
  REQUIRE(repo.GetById(pb, b_id).ok());
  REQUIRE(repo.GetLatestByFileSource(pb, "/shared/source").ok());

  //  负断言：第三个租户用同样的 id / file_source 不得命中
  const auto by_id = repo.GetById(pc, a_id);
  REQUIRE_FALSE(by_id.ok());
  REQUIRE(by_id.error().kind() == fss::ErrorKind::kNotFound);
  const auto by_source = repo.GetLatestByFileSource(pc, "/shared/source");
  REQUIRE_FALSE(by_source.ok());
  REQUIRE(by_source.error().kind() == fss::ErrorKind::kNotFound);
  domain::MetadataQuery query;
  const auto page = repo.List(pc, query);
  REQUIRE(page.ok());
  REQUIRE(page.value().total == 0);

  //  删除 A 不得影响 B
  REQUIRE(repo.Delete(pa, a_id).ok());
  REQUIRE_FALSE(repo.GetById(pa, a_id).ok());
  REQUIRE(repo.GetById(pb, b_id).ok());
}

TEST_CASE("★ PG 元数据仓储：state='deleted' 的墓碑行对所有读取路径不可见（含正控）",
          "[pg][infra][c6.11]") {
  static const std::string prefix = UniquePrefix("meta-tomb");
  PrefixGuard guard(prefix);

  fss::ManualClock clock{1700000000};
  PostgresMetadataRepositoryOptions options;
  options.pg = TestPgOptions(2);
  auto opened = PostgresMetadataRepository::Open(options, clock);
  REQUIRE(opened.ok());
  auto& repo = *opened.value();

  const std::string p = prefix + "-p";
  const std::string live_id = p + ":dataset--File.Generic:live";
  const std::string dead_id = p + ":dataset--File.Generic:dead";

  //  ★ 正控：同一条查找路径对 live 行必须真的有效 —— 否则下面的 REQUIRE_FALSE
  //    可能只是"路径写错了所以恒真"（AGENTS §4.3：否定式判据必须配正控）。
  RawInsertMetadata(p, live_id, "/live/source", "ready", 1700000000, "live-name");
  REQUIRE(repo.GetById(p, live_id).ok());
  REQUIRE(repo.GetLatestByFileSource(p, "/live/source").ok());
  {
    domain::MetadataQuery query;
    const auto page = repo.List(p, query);
    REQUIRE(page.ok());
    REQUIRE(page.value().total == 1);  // 正控：live 行在 List 里确实可见
  }

  //  外部写入的软删除（tombstone）行：三条读取路径都不得返回它
  RawInsertMetadata(p, dead_id, "/dead/source", "deleted", 1700000000, "dead-name");
  const auto by_id = repo.GetById(p, dead_id);
  REQUIRE_FALSE(by_id.ok());
  REQUIRE(by_id.error().kind() == fss::ErrorKind::kNotFound);
  const auto by_source = repo.GetLatestByFileSource(p, "/dead/source");
  REQUIRE_FALSE(by_source.ok());
  REQUIRE(by_source.error().kind() == fss::ErrorKind::kNotFound);
  {
    domain::MetadataQuery query;
    const auto page = repo.List(p, query);
    REQUIRE(page.ok());
    REQUIRE(page.value().total == 1);  // 仍然只有 live 那一条
    for (const auto& record : page.value().records) {
      REQUIRE(record.id != dead_id);
    }
  }
  //  正控仍在：墓碑没有影响 live 行
  REQUIRE(repo.GetById(p, live_id).ok());
}

TEST_CASE("★ PG 元数据仓储：JSONB 无损往返未知字段（信封 / data / acl / legal 各一层）",
          "[pg][infra][c6.11]") {
  static const std::string prefix = UniquePrefix("meta-json");
  PrefixGuard guard(prefix);

  fss::ManualClock clock{1700000000};
  PostgresMetadataRepositoryOptions options;
  options.pg = TestPgOptions(2);
  auto opened = PostgresMetadataRepository::Open(options, clock);
  REQUIRE(opened.ok());
  auto& repo = *opened.value();

  const std::string p = prefix + "-p";
  auto record = fss::test::MakeRecord(p, "unknown", "/u/unknown/1", "unknown-name");
  record.extra["CustomTopLevel"] = "keep-me";
  record.extra["CustomNumber"] = 42;
  record.data.extra["CustomDataField"] = {{"deep", true}};
  record.acl.extra["CustomAclField"] = "acl-keep";
  record.legal.extra["CustomLegalField"] = 7;
  record.data.dataset_properties.file_source_info.extra["CustomSourceField"] = "src-keep";

  const auto created = repo.Create(p, record);
  REQUIRE(created.ok());
  const auto got = repo.GetById(p, record.id);
  REQUIRE(got.ok());
  REQUIRE(json::Dump(got.value().extra) == json::Dump(record.extra));
  REQUIRE(json::Dump(got.value().data.extra) == json::Dump(record.data.extra));
  REQUIRE(json::Dump(got.value().acl.extra) == json::Dump(record.acl.extra));
  REQUIRE(json::Dump(got.value().legal.extra) == json::Dump(record.legal.extra));
  REQUIRE(json::Dump(got.value().data.dataset_properties.file_source_info.extra) ==
          json::Dump(record.data.dataset_properties.file_source_info.extra));

  //  且确实写进了 **JSONB 列**（不是只在返回对象里）—— 直接回读列上的 JSON 路径
  auto connection = PgConnection::Connect(TestPgOptions(1));
  REQUIRE(connection.ok());
  const auto raw = connection.value()->ExecParams(
      R"sql(SELECT data -> 'CustomTopLevel',
                   data -> 'data' -> 'CustomDataField' -> 'deep'
              FROM file_metadata_records
             WHERE partition_id = $1 AND id = $2)sql",
      {p, record.id});
  REQUIRE(raw.ok());
  REQUIRE(raw.value().RowCount() == 1);
  REQUIRE(raw.value().Value(0, 0) == "\"keep-me\"");
  REQUIRE(raw.value().Value(0, 1) == "true");
}

TEST_CASE("★ PG 元数据仓储：created_at 由注入时钟**精确**写入 / 读回（无舍入漂移）+ 含端点区间",
          "[pg][infra][c6.11]") {
  static const std::string prefix = UniquePrefix("meta-clock");
  PrefixGuard guard(prefix);

  fss::ManualClock clock{1700000000};
  PostgresMetadataRepositoryOptions options;
  options.pg = TestPgOptions(2);
  auto opened = PostgresMetadataRepository::Open(options, clock);
  REQUIRE(opened.ok());
  auto& repo = *opened.value();

  const std::string p = prefix + "-p";
  const std::string kind_a = "part:dataset--File.Generic:1.0.0";
  const std::string kind_b = "part:dataset--File.Other:1.0.0";
  const auto r1 = fss::test::MakeRecord(p, "t1", "/t/1", "alpha", kind_a);
  const auto r2 = fss::test::MakeRecord(p, "t2", "/t/2", "alphabet", kind_a);
  const auto r3 = fss::test::MakeRecord(p, "t3", "/t/3", "beta", kind_b);
  clock.SetEpochSeconds(1700000001);
  REQUIRE(repo.Create(p, r1).ok());
  clock.SetEpochSeconds(1700000002);
  REQUIRE(repo.Create(p, r2).ok());
  clock.SetEpochSeconds(1700000003);
  REQUIRE(repo.Create(p, r3).ok());

  //  ★ 精确往返：写进去的 epoch 秒必须**逐字**读回（floor 截断，不能有四舍五入漂移）。
  //    这条同时是"time-range 过滤的含端点语义"的前提。
  auto connection = PgConnection::Connect(TestPgOptions(1));
  REQUIRE(connection.ok());
  const auto raw = connection.value()->ExecParams(
      R"sql(SELECT id, floor(extract(epoch from created_at))::bigint
              FROM file_metadata_records
             WHERE partition_id = $1
             ORDER BY created_at ASC, id ASC)sql",
      {p});
  REQUIRE(raw.ok());
  REQUIRE(raw.value().RowCount() == 3);
  REQUIRE(raw.value().Value(0, 1) == "1700000001");
  REQUIRE(raw.value().Value(1, 1) == "1700000002");
  REQUIRE(raw.value().Value(2, 1) == "1700000003");

  //  含下界
  domain::MetadataQuery after;
  after.created_after_epoch_seconds = 1700000002;
  const auto after_page = repo.List(p, after);
  REQUIRE(after_page.ok());
  REQUIRE(after_page.value().total == 2);
  //  含上界
  domain::MetadataQuery before;
  before.created_before_epoch_seconds = 1700000002;
  const auto before_page = repo.List(p, before);
  REQUIRE(before_page.ok());
  REQUIRE(before_page.value().total == 2);
  //  单点窗口（含两个端点）→ 恰好一条
  domain::MetadataQuery window;
  window.created_after_epoch_seconds = 1700000002;
  window.created_before_epoch_seconds = 1700000002;
  const auto window_page = repo.List(p, window);
  REQUIRE(window_page.ok());
  REQUIRE(window_page.value().total == 1);
  REQUIRE(window_page.value().records.size() == 1);
  REQUIRE(window_page.value().records.front().data.name.has_value());
  REQUIRE(*window_page.value().records.front().data.name == "alphabet");
  //  正控：窗口外的区间必须为空（证明过滤真的在起作用，而不是恒返回全部）
  domain::MetadataQuery outside;
  outside.created_after_epoch_seconds = 1700000010;
  outside.created_before_epoch_seconds = 1700000020;
  const auto outside_page = repo.List(p, outside);
  REQUIRE(outside_page.ok());
  REQUIRE(outside_page.value().total == 0);
}

// =============================================================================
//  ★★ 关键并发判据（R1 注入点）：ADR-009 M2 —— 幂等键上的**原子领取**
// =============================================================================
//  判据与线程到达顺序**无关**（AGENTS §4.3）：
//    · N 个并发 `Create` 用**不同的 id**、**相同的 (partition, file_source)`；
//    · 正确实现（`ON CONFLICT ... WHERE state<>'deleted' AND is_latest DO NOTHING`）
//      只有一个赢家 → 库里**恰好 1 行**，且**所有**调用者拿到同一条（同 id / version 1）；
//    · 正控：N 个**不同 file_source** → 恰好 N 行，每个调用者拿到自己那条。
//      正控是必需的：否则"恰好 1 行"可能只是"并发调用全被丢弃"。
TEST_CASE("★ C6.11/ADR-009 M2：并发 Create 同一 file_source 只产生 1 条（原子领取）",
          "[pg][infra][c6.11]") {
  static const std::string prefix = UniquePrefix("meta-claim");
  PrefixGuard guard(prefix);

  constexpr int kConcurrency = 8;
  fss::ManualClock clock{1700000000};
  PostgresMetadataRepositoryOptions options;
  options.pg = TestPgOptions(kConcurrency + 2);
  auto opened = PostgresMetadataRepository::Open(options, clock);
  REQUIRE(opened.ok());
  auto& repo = *opened.value();

  //  并发起跑栅栏：所有线程先就位，再一起调 Create（最大化真实竞态）。
  //  ★ 结果不放进 `std::vector<Result<T>>`：`Result<T>` 没有默认构造函数（它只有
  //    "有值" 与 "有错" 两个状态），无法 `resize`。这里按"成功记录 / 错误消息"分开存。
  struct Outcome {
    std::vector<domain::FileMetadataRecord> records;
    std::vector<std::string> errors;
  };
  const auto run_concurrent = [&](const std::string& partition,
                                  const std::vector<domain::FileMetadataRecord>& records) {
    Outcome outcome;
    outcome.records.resize(records.size());
    outcome.errors.resize(records.size());
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
      threads.emplace_back([&, i] {
        ready.fetch_add(1);
        while (!go.load()) std::this_thread::yield();
        auto created = repo.Create(partition, records[i]);
        if (created.ok()) {
          outcome.records[i] = std::move(created).value();
        } else {
          outcome.errors[i] = created.error().message();
        }
      });
    }
    while (ready.load() < static_cast<int>(records.size())) std::this_thread::yield();
    go.store(true);
    for (auto& thread : threads) thread.join();
    return outcome;
  };

  //  ---- 竞态：N 个不同 id、同一 file_source ----
  const std::string race_partition = prefix + "-race";
  const std::string race_source = "/race/source";
  std::vector<domain::FileMetadataRecord> race_records;
  for (int i = 0; i < kConcurrency; ++i) {
    race_records.push_back(fss::test::MakeRecord(
        race_partition, "race-" + std::to_string(i), race_source, "name-" + std::to_string(i)));
  }
  const auto results = run_concurrent(race_partition, race_records);
  for (int i = 0; i < kConcurrency; ++i) {
    INFO("caller " << i << " : " << results.errors[i]);
    REQUIRE(results.errors[i].empty());
    REQUIRE(results.records[i].version == 1);
  }
  const std::string winner = results.records[0].id;
  for (int i = 1; i < kConcurrency; ++i) {
    REQUIRE(results.records[i].id == winner);
  }
  //  ★ 库里**恰好 1 行**（不是 8 行、也不是 0 行）—— 判据由实现决定，与线程顺序无关
  REQUIRE(ScalarQuery(race_partition) == 1);
  const auto latest = repo.GetLatestByFileSource(race_partition, race_source);
  REQUIRE(latest.ok());
  REQUIRE(latest.value().id == winner);
  REQUIRE(latest.value().version == 1);

  //  ---- 正控：N 个**不同 file_source** → 恰好 N 行，每个调用者拿到自己那条 ----
  const std::string distinct_partition = prefix + "-distinct";
  std::vector<domain::FileMetadataRecord> distinct_records;
  for (int i = 0; i < kConcurrency; ++i) {
    distinct_records.push_back(fss::test::MakeRecord(
        distinct_partition, "distinct-" + std::to_string(i), "/distinct/" + std::to_string(i),
        "distinct-name-" + std::to_string(i)));
  }
  const auto distinct_results = run_concurrent(distinct_partition, distinct_records);
  for (int i = 0; i < kConcurrency; ++i) {
    INFO("distinct caller " << i << " : " << distinct_results.errors[i]);
    REQUIRE(distinct_results.errors[i].empty());
    REQUIRE(distinct_results.records[i].version == 1);
    REQUIRE(distinct_results.records[i].id == distinct_records[i].id);
  }
  REQUIRE(ScalarQuery(distinct_partition) == kConcurrency);
}
