// =============================================================================
//  C3.12：有界写并发 —— 并发 32 个写入者不得出现 SQLITE_BUSY 失败
// =============================================================================
//  依据（实测）：8 线程 25,471 tx/s 为峰值，32 线程降到 13,475 tx/s。
//  因此写入并发必须是**有界的**（默认 8），超出部分排队，而不是让所有请求线程直抢写锁。
//
//  本测试的可观察判据：
//    · 32 个线程并发 Save，**零失败**（尤其不得出现映射为 `kUnavailable` 的 `SQLITE_BUSY`）；
//    · 写完后记录数正好等于写入次数（没有丢写/重复）；
//    · 两个连接（两个仓储实例）同时写同一文件也不报 busy（WAL + busy_timeout）。
// =============================================================================
#include <catch2/catch.hpp>

#include "port_contract.h"
#include "temp_dir.h"

#include "domain/ports/ports.h"
#include "infra/location/sqlite/sqlite_location_repository.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using fss::domain::FileLocation;
using fss::infra::SqliteLocationRepository;

namespace {

std::unique_ptr<SqliteLocationRepository> OpenDb(const std::string& path,
                                                 int max_write_concurrency = 8) {
  fss::infra::SqliteLocationRepositoryOptions options;
  options.max_write_concurrency = max_write_concurrency;
  auto opened = SqliteLocationRepository::Open(path, options);
  REQUIRE(opened.ok());
  return std::move(opened.value());
}

FileLocation Make(const std::string& file_id) {
  return fss::test::MakeLocation(file_id, "/u/" + file_id);
}

}  // namespace

TEST_CASE("★ C3.12 32 线程并发写同一个 SQLite 仓储：零失败、无丢失", "[phase3][sqlite][c3.12]") {
  fss::test::TempDir dir("sqlite_write_conc");
  auto repository = OpenDb(dir.child("locations.db"));

  constexpr int kThreads = 32;
  constexpr int kPerThread = 20;
  std::atomic<int> failures{0};
  std::atomic<int> busy_failures{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        const std::string file_id = "f-" + std::to_string(t) + "-" + std::to_string(i);
        const auto saved = repository->Save("opendes", Make(file_id));
        if (!saved.ok()) {
          ++failures;
          if (saved.error().kind() == fss::ErrorKind::kUnavailable) ++busy_failures;
          continue;
        }
        //  交织读，制造读写混合负载
        const auto found = repository->Find("opendes", file_id);
        if (!found.ok()) ++failures;
      }
    });
  }
  for (auto& thread : threads) thread.join();

  REQUIRE(failures.load() == 0);
  REQUIRE(busy_failures.load() == 0);  // 关键：不得有 SQLITE_BUSY

  fss::domain::LocationQuery query;
  query.limit = kThreads * kPerThread + 1;
  const auto page = repository->List("opendes", query);
  REQUIRE(page.ok());
  REQUIRE(page.value().total == kThreads * kPerThread);
}

TEST_CASE("★ C3.12 两个连接同时写同一文件：WAL + busy_timeout 下不报 busy",
          "[phase3][sqlite][c3.12]") {
  fss::test::TempDir dir("sqlite_two_conn");
  const std::string path = dir.child("locations.db");
  auto first = OpenDb(path);
  auto second = OpenDb(path);

  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  threads.emplace_back([&] {
    for (int i = 0; i < 50; ++i) {
      if (!first->Save("tenant-a", Make("a-" + std::to_string(i))).ok()) ++failures;
    }
  });
  threads.emplace_back([&] {
    for (int i = 0; i < 50; ++i) {
      if (!second->Save("tenant-b", Make("b-" + std::to_string(i))).ok()) ++failures;
    }
  });
  for (auto& thread : threads) thread.join();
  REQUIRE(failures.load() == 0);

  fss::domain::LocationQuery query;
  query.limit = 1000;
  const auto a = first->List("tenant-a", query);
  const auto b = second->List("tenant-b", query);
  REQUIRE(a.ok());
  REQUIRE(b.ok());
  REQUIRE(a.value().total == 50);
  REQUIRE(b.value().total == 50);
}

TEST_CASE("C3.12 并发写不同租户 + 唯一约束冲突都被正确串行化", "[phase3][sqlite][c3.12]") {
  fss::test::TempDir dir("sqlite_conflict_conc");
  auto repository = OpenDb(dir.child("locations.db"));

  std::atomic<int> ok_count{0};
  std::atomic<int> conflict_count{0};
  std::atomic<int> other{0};
  std::vector<std::thread> threads;
  //  16 个线程抢同一个 (partition, file_source)：应当恰好 1 个成功，其余 kLocationAlreadyExists
  for (int t = 0; t < 16; ++t) {
    threads.emplace_back([&, t] {
      FileLocation location = fss::test::MakeLocation("id-" + std::to_string(t), "/same/source");
      const auto saved = repository->Save("opendes", location);
      if (saved.ok()) {
        ++ok_count;
      } else if (saved.error().kind() == fss::ErrorKind::kLocationAlreadyExists) {
        ++conflict_count;
      } else {
        ++other;
      }
    });
  }
  for (auto& thread : threads) thread.join();

  REQUIRE(other.load() == 0);
  REQUIRE(ok_count.load() + conflict_count.load() == 16);
  REQUIRE(ok_count.load() == 1);        // 唯一约束由数据库保证，不靠应用层 check-then-insert（R5）
  REQUIRE(conflict_count.load() == 15);

  const auto by_source = repository->FindByFileSource("opendes", "/same/source");
  REQUIRE(by_source.ok());
}

TEST_CASE("C3.12 max_write_concurrency 必须 > 0（非法配置被拒）", "[phase3][sqlite][c3.12]") {
  fss::test::TempDir dir("sqlite_bad_option");
  fss::infra::SqliteLocationRepositoryOptions options;
  options.max_write_concurrency = 0;
  const auto opened = SqliteLocationRepository::Open(dir.child("bad.db"), options);
  REQUIRE_FALSE(opened.ok());
  REQUIRE(opened.error().kind() == fss::ErrorKind::kInvalidArgument);
}
