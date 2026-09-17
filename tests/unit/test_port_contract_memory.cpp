// =============================================================================
//  端口契约测试（C2.10）—— 内存实现是第一个使用者
// =============================================================================
//  这三个 TEST_CASE 本身几乎没有断言：真正的语义断言在
//  `tests/framework/port_contract.h`。这样以后 POSIX/S3/SQLite/PostgreSQL 实现
//  只要各写三行就能证明"满足同一份契约"，不会出现"每个后端一套语义"。
//
//  本文件另外覆盖**内存实现独有**的能力：故障注入（P6 的 C6.3 基石）与
//  partition 隔离的直接交叉断言。
// =============================================================================
#include <catch2/catch.hpp>

#include "port_contract.h"

#include "common/bytes/bytes.h"
#include "common/result/result.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/blob/memory/memory_blob_store.h"
#include "infra/location/memory/memory_location_repository.h"
#include "infra/metadata/memory/memory_metadata_repository.h"

#include <string>
#include <type_traits>

using fss::domain::ObjectRef;
using fss::domain::PutOptions;
using fss::infra::InMemoryBlobStore;
using fss::infra::InMemoryLocationRepository;
using fss::infra::InMemoryMetadataRepository;

// 编译期：三个适配器都必须**可实例化**并实现对应端口（签名变了这里立刻失败）
static_assert(std::is_base_of_v<fss::domain::IBlobStore, InMemoryBlobStore>);
static_assert(!std::is_abstract_v<InMemoryBlobStore>);
static_assert(std::is_base_of_v<fss::domain::IFileLocationRepository, InMemoryLocationRepository>);
static_assert(!std::is_abstract_v<InMemoryLocationRepository>);
static_assert(std::is_base_of_v<fss::domain::IMetadataRepository, InMemoryMetadataRepository>);
static_assert(!std::is_abstract_v<InMemoryMetadataRepository>);

TEST_CASE("★ IBlobStore 契约：InMemoryBlobStore（memory/POSIX/S3 共用同一套断言）",
          "[phase2][contract][c2.10]") {
  fss::ManualClock clock;  // C2.8：时间可注入，测试无真实睡眠
  InMemoryBlobStore store(clock);
  fss::test::CheckBlobStoreContract(store);
}

TEST_CASE("★ IFileLocationRepository 契约：InMemoryLocationRepository",
          "[phase2][contract][c2.10]") {
  InMemoryLocationRepository repo;
  fss::test::CheckLocationRepositoryContract(repo);
}

TEST_CASE("★ IMetadataRepository 契约：InMemoryMetadataRepository",
          "[phase2][contract][c2.10]") {
  fss::ManualClock clock;
  InMemoryMetadataRepository repo(clock);
  fss::test::CheckMetadataRepositoryContract(repo, clock);
}

TEST_CASE("InMemoryBlobStore 故障注入：错误 / 字节截断 / 延迟 / 校验和替换",
          "[phase2][infra][fault]") {
  using Op = InMemoryBlobStore::Op;
  using FaultPlan = InMemoryBlobStore::FaultPlan;

  fss::ManualClock clock;
  InMemoryBlobStore store(clock);
  const std::string container = "fault-container";
  REQUIRE(store.ensure_container(container).ok());

  SECTION("错误注入：一次性失败后自动恢复（且失败不产生副作用）") {
    FaultPlan plan;
    plan.op = Op::kPut;
    plan.fail_count = 1;
    plan.error = fss::ErrorKind::kUnavailable;
    plan.message = "injected put failure";
    store.Inject(plan);

    fss::bytes::StringSource first("payload");
    const auto r1 = store.put(fss::test::MakeRef(container, "fault/1.bin"), first, PutOptions{});
    REQUIRE_FALSE(r1.ok());
    REQUIRE(r1.error().kind() == fss::ErrorKind::kUnavailable);
    REQUIRE(store.object_count() == 0);  // 失败必须"什么都没写"

    fss::bytes::StringSource second("payload");
    REQUIRE(store.put(fss::test::MakeRef(container, "fault/1.bin"), second, PutOptions{}).ok());
    REQUIRE(store.OpCount(Op::kPut) == 2);  // 计数器证明"注入真的被消费了"
  }

  SECTION("字节截断注入：put 返回成功但对象比承诺短（P6 的'静默截断'对照）") {
    FaultPlan plan;
    plan.op = Op::kPut;
    plan.truncate_put_to_bytes = 3;
    store.Inject(plan);

    fss::bytes::StringSource source("abcdefgh");
    REQUIRE(store.put(fss::test::MakeRef(container, "trunc/1.bin"), source, PutOptions{}).ok());

    const auto st = store.stat(fss::test::MakeRef(container, "trunc/1.bin"));
    REQUIRE(st.ok());
    REQUIRE(st.value().size == 3);  // 成功返回 ≠ 数据完整 —— 这正是要防的缺陷

    fss::bytes::StringSink sink;
    REQUIRE(store.get(fss::test::MakeRef(container, "trunc/1.bin"), sink,
                       fss::domain::ByteRange{})
                .ok());
    REQUIRE(sink.str() == "abc");
  }

  SECTION("延迟注入：不真实 sleep（C2.8），但延迟量可观测") {
    FaultPlan plan;
    plan.op = Op::kGet;
    plan.latency_millis = 250;
    store.Inject(plan);

    fss::bytes::StringSource source("x");
    REQUIRE(store.put(fss::test::MakeRef(container, "lat/1.bin"), source, PutOptions{}).ok());
    fss::bytes::StringSink sink;
    REQUIRE(store.get(fss::test::MakeRef(container, "lat/1.bin"), sink, fss::domain::ByteRange{})
                .ok());
    REQUIRE(store.TakeInjectedLatencyMillis() == 250);
    REQUIRE(store.TakeInjectedLatencyMillis() == 0);  // 取走后清零
  }

  SECTION("校验和替换：模拟'存储侧校验和不可信'") {
    fss::bytes::StringSource source("data");
    REQUIRE(store.put(fss::test::MakeRef(container, "cs/1.bin"), source, PutOptions{}).ok());

    FaultPlan plan;
    plan.op = Op::kStat;
    plan.checksum_override = "0000000000000000000000000000000000000000000000000000000000000000";
    store.Inject(plan);

    const auto st = store.stat(fss::test::MakeRef(container, "cs/1.bin"));
    REQUIRE(st.ok());
    REQUIRE(st.value().checksum == plan.checksum_override);
  }
}

TEST_CASE("★ 内存适配器：partition_id 严格隔离（跨租户交叉访问必须 miss）",
          "[phase2][infra][c2.10]") {
  // 契约基类里每个端口各有一段隔离断言；这里再把两个仓储放到一起做交叉验证，
  // 因为"租户串数据"是本项目最重要的安全不变量之一（docs/02-design.md §9.1）。
  const std::string pa = "tenant-a";
  const std::string pb = "tenant-b";

  fss::ManualClock clock;
  InMemoryLocationRepository locations;
  InMemoryMetadataRepository metadata(clock);

  // 位置记录：同 file_id + 同 file_source 在两个租户各自独立
  REQUIRE(locations.Save(pa, fss::test::MakeLocation("same-id", "/same/source")).ok());
  REQUIRE(locations.Save(pb, fss::test::MakeLocation("same-id", "/same/source")).ok());
  REQUIRE(locations.size(pa) == 1);
  REQUIRE(locations.size(pb) == 1);
  REQUIRE_FALSE(locations.Find("tenant-c", "same-id").ok());

  // 元数据：同 file_source 在两个租户互不覆盖
  REQUIRE(metadata.Create(pa, fss::test::MakeRecord(pa, "iso1", "/same/fs", "in-a")).ok());
  REQUIRE(metadata.Create(pb, fss::test::MakeRecord(pb, "iso2", "/same/fs", "in-b")).ok());
  const auto a = metadata.GetLatestByFileSource(pa, "/same/fs");
  const auto b = metadata.GetLatestByFileSource(pb, "/same/fs");
  REQUIRE(a.ok());
  REQUIRE(b.ok());
  REQUIRE(*a.value().data.name == "in-a");
  REQUIRE(*b.value().data.name == "in-b");

  // 用 A 租户的 record id 到 B 租户查 → 必须 miss
  REQUIRE_FALSE(metadata.GetById(pb, pa + ":dataset--File.Generic:iso1").ok());
  // 用 B 租户的身份写 A 租户的 id → 必须被拒（不能借 id 绕过隔离）
  REQUIRE_FALSE(metadata.Create(pb, fss::test::MakeRecord(pa, "iso3", "/x", "sneaky")).ok());
}
