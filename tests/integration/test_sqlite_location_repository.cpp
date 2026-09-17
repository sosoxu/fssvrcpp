// =============================================================================
//  C2.10 跨实现 + C3.8：SqliteLocationRepository
// =============================================================================
//  ★ 这里实例化的是与**内存实现完全相同**的 `CheckLocationRepositoryContract`。
//    这一条同时闭合两件事：
//      · C2.10（"两种仓储实现共用同一套契约测试"）的 SQLite 一侧；
//      · C3.8（双租户不串数据 / 未知 JSON 字段往返不丢）。
// =============================================================================
#include <catch2/catch.hpp>

#include "port_contract.h"
#include "temp_dir.h"

#include "domain/ports/ports.h"
#include "infra/location/sqlite/sqlite_location_repository.h"

#include <memory>
#include <string>

using fss::domain::FileLocation;
using fss::domain::StorageZone;
using fss::infra::SqliteLocationRepository;

namespace {

std::unique_ptr<SqliteLocationRepository> OpenTemp(fss::test::TempDir& dir) {
  auto opened = SqliteLocationRepository::Open(dir.child("locations.db"));
  REQUIRE(opened.ok());
  return std::move(opened.value());
}

}  // namespace

TEST_CASE("★ C2.10/C3.8 SqliteLocationRepository 通过与内存实现共用的契约测试",
          "[phase3][sqlite][c2.10][c3.8]") {
  fss::test::TempDir dir("sqlite_location");
  auto repository = OpenTemp(dir);
  fss::test::CheckLocationRepositoryContract(*repository);
}

TEST_CASE("★ C3.8 双租户：同 file_id / 同 file_source 在两个 partition 各自独立",
          "[phase3][sqlite][c3.8]") {
  fss::test::TempDir dir("sqlite_tenants");
  auto repository = OpenTemp(dir);

  //  同一主键 (partition_id, file_id) 的两个租户必须能共存
  REQUIRE(repository->Save("tenant-a", fss::test::MakeLocation("shared", "/shared/src")).ok());
  REQUIRE(repository->Save("tenant-b", fss::test::MakeLocation("shared", "/shared/src")).ok());

  //  同 file_source 在两个租户各自成立
  const auto a = repository->FindByFileSource("tenant-a", "/shared/src");
  const auto b = repository->FindByFileSource("tenant-b", "/shared/src");
  REQUIRE(a.ok());
  REQUIRE(b.ok());

  //  跨租户按 id 查不到
  REQUIRE_FALSE(repository->Find("tenant-c", "shared").ok());

  //  第三个租户可以用同样的 file_source（唯一索引是 (partition_id, file_source)）
  REQUIRE(repository->Save("tenant-c", fss::test::MakeLocation("other", "/shared/src")).ok());

  //  删除 A 不影响 B
  REQUIRE(repository->Delete("tenant-a", "shared").ok());
  REQUIRE_FALSE(repository->Find("tenant-a", "shared").ok());
  REQUIRE(repository->Find("tenant-b", "shared").ok());

  //  List 也是租户级的
  const auto page_a = repository->List("tenant-a", fss::domain::LocationQuery{});
  const auto page_b = repository->List("tenant-b", fss::domain::LocationQuery{});
  REQUIRE(page_a.ok());
  REQUIRE(page_b.ok());
  REQUIRE(page_a.value().total == 0);
  REQUIRE(page_b.value().total == 1);
}

TEST_CASE("★ C3.8 未知 JSON 字段：读-改-写（UpdateSignedUrl）不得丢失",
          "[phase3][sqlite][c3.8]") {
  fss::test::TempDir dir("sqlite_unknown_fields");
  auto repository = OpenTemp(dir);

  FileLocation location = fss::test::MakeLocation("f-1", "/u/1-ts/f-1");
  location.extra["FutureTopLevelField"] = "keep-me";        // 模拟未来版本新增的字段
  location.extra["Nested"] = {{"a", 1}, {"b", "two"}};
  REQUIRE(repository->Save("opendes", location).ok());

  //  一次"只改 signed_url"的更新之后，未知字段必须原样还在
  REQUIRE(repository->UpdateSignedUrl("opendes", "f-1", "https://signed.invalid/x", 1234).ok());

  const auto found = repository->Find("opendes", "f-1");
  REQUIRE(found.ok());
  REQUIRE(found.value().signed_url == "https://signed.invalid/x");
  REQUIRE(found.value().updated_at_epoch_seconds == 1234);
  REQUIRE(found.value().extra["FutureTopLevelField"].get<std::string>() == "keep-me");
  REQUIRE(found.value().extra["Nested"]["b"].get<std::string>() == "two");
}

TEST_CASE("C3.8 (partition, file_source) 唯一索引：不同 file_id 抢同一来源必须被拒",
          "[phase3][sqlite][c3.8]") {
  fss::test::TempDir dir("sqlite_unique_source");
  auto repository = OpenTemp(dir);

  REQUIRE(repository->Save("opendes", fss::test::MakeLocation("f-1", "/u/1-ts/f-1")).ok());
  const auto conflict =
      repository->Save("opendes", fss::test::MakeLocation("f-2", "/u/1-ts/f-1"));
  REQUIRE_FALSE(conflict.ok());
  REQUIRE(conflict.error().kind() == fss::ErrorKind::kLocationAlreadyExists);
  //  冲突后原记录保持不变
  const auto original = repository->Find("opendes", "f-1");
  REQUIRE(original.ok());
  REQUIRE(original.value().file_id == "f-1");
}

TEST_CASE("C3.8 Save 是 upsert：同 file_id 再次保存更新 zone 与位置", "[phase3][sqlite][c3.8]") {
  fss::test::TempDir dir("sqlite_upsert");
  auto repository = OpenTemp(dir);

  FileLocation staging = fss::test::MakeLocation("f-9", "/u/9-ts/f-9", StorageZone::kStaging);
  REQUIRE(repository->Save("opendes", staging).ok());
  FileLocation persistent = staging;
  persistent.zone = StorageZone::kPersistent;
  persistent.updated_at_epoch_seconds = 999;
  REQUIRE(repository->Save("opendes", persistent).ok());

  const auto found = repository->Find("opendes", "f-9");
  REQUIRE(found.ok());
  REQUIRE(found.value().zone == StorageZone::kPersistent);
  REQUIRE(found.value().updated_at_epoch_seconds == 999);
}
