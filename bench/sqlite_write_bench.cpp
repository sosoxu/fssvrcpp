// =============================================================================
//  bench/sqlite_write_bench.cpp —— SQLite 写吞吐（C9.11 的第四项）
// =============================================================================
//  为什么单独一个进程量：C9.11 要求记录"SQLite 写吞吐"，而进程内的数字会被
//  "客户端与服务端抢同一批核"污染（R2 的由来）。这里**只量仓储本身**：
//  同一进程内串行/并发调用两个 SQLite 仓储的写方法，客户端不存在，因此不存在
//  "谁抢谁"的问题；数字的用途是回答"元数据/位置写入是不是容量瓶颈"。
//
//  诚实声明（R3/R4）
//  ---------------------------------------------------------------------------
//  · 数据落在 **WSL2 虚拟盘**上，绝对数字只能作本机量级参考（C9.14）。
//  · 事务/耐久档位用仓储的默认值（与组合根一致）：`SqliteLocationRepository`/
//    `SqliteMetadataRepository` 的内部策略见各自头文件；本基准**不**调 PRAGMA
//    去"提速"，否则数字就不是生产路径的数字了。
//
//  用法：fss_bench_sqlite_write [记录数] [并发线程数] [临时目录]
// =============================================================================
#include "infra/location/sqlite/sqlite_location_repository.h"
#include "infra/metadata/sqlite/sqlite_metadata_repository.h"

#include "common/time/clock.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using fss::domain::FileLocation;
using fss::domain::FileMetadataRecord;

using Clock = std::chrono::steady_clock;

fss::domain::FileMetadataRecord MakeRecord(const std::string& id, const std::string& file_source) {
  FileMetadataRecord record;
  record.id = id;
  record.kind = "opendes:wks:dataset--File.Generic:1.0.0";
  record.acl.viewers = {"data.default.viewers@opendes.example.com"};
  record.acl.owners = {"data.default.owners@opendes.example.com"};
  record.legal.legaltags = {"opendes-public-1"};
  record.legal.other_relevant_data_countries = {"US"};
  record.legal.status = fss::domain::LegalStatus::kCompliant;
  record.data.name = "bench.bin";
  record.data.endian = "LITTLE";
  record.data.dataset_properties.present = true;
  record.data.dataset_properties.file_source_info.file_source = file_source;
  return record;
}

fss::domain::FileLocation MakeLocation(const std::string& file_id, const std::string& file_source) {
  FileLocation location;
  location.file_id = file_id;
  location.file_source = file_source;
  location.zone = fss::domain::StorageZone::kStaging;
  location.user_id = "bench-user";
  location.created_at_epoch_seconds = 1700000000;
  location.updated_at_epoch_seconds = 1700000000;
  location.extra = fss::json::Value::object();
  location.extra["container"] = "opendes-staging";
  location.extra["object_key"] = file_source;
  return location;
}

}  // namespace

int main(int argc, char** argv) {
  const int records = argc > 1 ? std::atoi(argv[1]) : 5000;
  const int threads = argc > 2 ? std::atoi(argv[2]) : 1;
  if (records <= 0 || threads <= 0) {
    std::cerr << "用法：fss_bench_sqlite_write [记录数] [并发线程数] [临时目录]\n";
    return 2;
  }
  const std::string dir = argc > 3 ? argv[3] : "/tmp/fss-bench-sqlite";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    std::cerr << "创建目录失败：" << ec.message() << "\n";
    return 1;
  }
  fss::SystemClock clock;

  auto locations = fss::infra::SqliteLocationRepository::Open(dir + "/location.db");
  if (!locations.ok()) {
    std::cerr << "打开位置仓储失败：" << locations.error().ToString() << "\n";
    return 1;
  }
  auto metadata = fss::infra::SqliteMetadataRepository::Open(dir + "/metadata.db", clock);
  if (!metadata.ok()) {
    std::cerr << "打开元数据仓储失败：" << metadata.error().ToString() << "\n";
    return 1;
  }

  const int per_thread = records / threads;
  std::atomic<std::uint64_t> ok{0};
  std::atomic<std::uint64_t> failed{0};
  const auto t0 = Clock::now();
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      for (int i = 0; i < per_thread; ++i) {
        const std::string id = "opendes:dataset--File.Generic:bench-" + std::to_string(t) + "-" +
                               std::to_string(i);
        const std::string source =
            "/bench-user/" + std::to_string(t) + "-" + std::to_string(i) + "/bench.bin";
        //  两张表各写一次：位置记录 + 元数据记录（真实上传路径的两处写）
        if (!locations.value()->Save("opendes", MakeLocation(id, source)).ok()) {
          ++failed;
          continue;
        }
        if (!metadata.value()->Create("opendes", MakeRecord(id, source)).ok()) {
          ++failed;
          continue;
        }
        ++ok;
      }
    });
  }
  for (auto& w : workers) w.join();
  const double seconds =
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count() / 1e6;
  const double writes = static_cast<double>(ok.load()) * 2;  // 每个循环 2 次写
  std::cout.setf(std::ios::fixed);
  std::cout.precision(1);
  std::cout << "SQLITE records=" << ok.load() << " failed=" << failed.load()
            << " threads=" << threads << " writes=" << writes << " seconds=" << std::setprecision(2)
            << seconds << " writes_per_sec=" << std::setprecision(0)
            << (seconds > 0 ? writes / seconds : 0.0) << " us_per_record="
            << (ok.load() > 0 ? seconds * 1e6 / static_cast<double>(ok.load()) : 0.0) << std::endl;
  std::filesystem::remove_all(dir, ec);
  return failed.load() == 0 ? 0 : 1;
}
