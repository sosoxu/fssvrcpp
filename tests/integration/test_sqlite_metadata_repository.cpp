// =============================================================================
//  C2.10（元数据侧）/ C6.1 / C6.5：SqliteMetadataRepository 跑**同一套**元数据契约
// =============================================================================
//  为什么必须复用 `port_contract.h` 的那一份断言：
//    内存实现是"契约基准"，SQLite/postgres/remote 都必须与它语义一致。各写一套测试
//    就等于把"语义一致"降级成口号 —— 上层迟早写出只在某个实现成立的分支。
//  这一遍同时闭合 C2.10 的**元数据侧**（位置侧在 P3 已闭合）。
// =============================================================================
#include <catch2/catch.hpp>

#include "port_contract.h"
#include "temp_dir.h"

#include "common/time/clock.h"
#include "infra/metadata/sqlite/sqlite_metadata_repository.h"

#include <sqlite3.h>

#include <memory>
#include <string>

using fss::infra::SqliteMetadataRepository;

namespace {

struct Fixture {
  fss::test::TempDir dir{"sqlite_metadata"};
  fss::ManualClock clock{1700000000};
  std::unique_ptr<SqliteMetadataRepository> repo;

  Fixture() {
    auto opened = SqliteMetadataRepository::Open(dir.child("metadata.db"), clock);
    REQUIRE(opened.ok());
    repo = std::move(opened.value());
  }
};

}  // namespace

TEST_CASE("★ C2.10/C6.5 SqliteMetadataRepository 通过与内存实现**同一套**契约",
          "[phase6][integration][sqlite][c2.10]") {
  Fixture fixture;
  fss::test::CheckMetadataRepositoryContract(*fixture.repo, fixture.clock);
}

TEST_CASE("★ C6.5 版本链在**数据库层面**的真实形态（列与唯一约束）",
          "[phase6][integration][sqlite][c6.5]") {
  Fixture fixture;
  auto& repo = *fixture.repo;
  const std::string partition = "version-part";
  auto record = fss::test::MakeRecord(partition, "v1", "/u/v/1", "n1");
  REQUIRE(repo.Create(partition, record).ok());

  record.data.name = "n2";
  REQUIRE(repo.Update(partition, record).ok());
  record.data.name = "n3";
  REQUIRE(repo.Update(partition, record).ok());

  //  三版都在库里（历史可查），但只有一个 is_latest
  REQUIRE(repo.VersionCount(partition, record.id) == 3);
  const auto latest = repo.GetById(partition, record.id);
  REQUIRE(latest.ok());
  REQUIRE(latest.value().version == 3);
  REQUIRE(*latest.value().data.name == "n3");

  //  ★ 自证（R1）：`is_latest` 的部分唯一索引**确实存在且有效** ——
  //    直接往库里插第二条 is_latest=1 必须被唯一约束拒绝。
  //    （若约束缺失，上面"只有一个 latest"的语义就只是巧合。）
  {
    //  用仓储自己再插一版会把旧的 is_latest 清零，所以这里绕开仓储：直接开库验证索引。
    const std::string path = fixture.dir.child("metadata.db");
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    const std::string sql =
        "INSERT INTO metadata (partition_id, id, version, is_latest, previous_version,"
        " file_source, kind, name, created_at, created_by, data)"
        " VALUES ('" + partition + "', '" + record.id + "', 99, 1, NULL, '/u/v/other',"
        " 'k', 'n', 0, '', '{}')";
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
    INFO("插入重复 latest 的返回码：" << rc << " 错误：" << (error != nullptr ? error : ""));
    if (error != nullptr) sqlite3_free(error);
    REQUIRE(rc != SQLITE_OK);  // 唯一约束必须拒绝它
    sqlite3_close(db);
  }
}

// =============================================================================
//  阶段 10 切片 4：`metadata.sqlite.synchronous` → `PRAGMA synchronous`
// =============================================================================
//  ★ 关键陷阱（`AppliedPragma` 存在的唯一原因）：`PRAGMA synchronous` 是**连接级**
//    设置，**不随库文件持久化**。`Open` 之后再另开一个 sqlite3 连接读回，得到的是
//    那个新连接自己的默认值（FULL = 2），与被测仓储是否真的下发了 OFF/NORMAL/FULL
//    无关。必须用仓储自己的 `AppliedPragma("synchronous")` 在**同一连接**上读回。
//    （`journal_mode` 相反：它写进库文件头，另开连接能读回，见
//    tests/integration/test_config_wiring.cpp 的 C10.14。）
TEST_CASE("★ 切片 4：SqliteMetadataRepository 的 synchronous 真的下发到**本连接**",
          "[phase10][sqlite][c6.1]") {
  fss::test::TempDir dir("sqlite_metadata_pragma");
  fss::ManualClock clock{1700000000};

  SECTION("默认 = NORMAL(1)（R16 正例：不能只测非默认值）") {
    auto opened = SqliteMetadataRepository::Open(dir.child("default.db"), clock);
    REQUIRE(opened.ok());
    const auto applied = opened.value()->AppliedPragma("synchronous");
    REQUIRE(applied.ok());
    REQUIRE(applied.value() == "1");
  }

  SECTION("synchronous_level=0（OFF）→ 读回 0（与默认值不同，排除恒真）") {
    fss::infra::SqliteMetadataRepositoryOptions options;
    options.synchronous_level = 0;
    auto opened = SqliteMetadataRepository::Open(dir.child("off.db"), clock, options);
    REQUIRE(opened.ok());
    const auto applied = opened.value()->AppliedPragma("synchronous");
    REQUIRE(applied.ok());
    REQUIRE(applied.value() == "0");
  }

  SECTION("synchronous_level=2（FULL）→ 读回 2") {
    fss::infra::SqliteMetadataRepositoryOptions options;
    options.synchronous_level = 2;
    auto opened = SqliteMetadataRepository::Open(dir.child("full.db"), clock, options);
    REQUIRE(opened.ok());
    const auto applied = opened.value()->AppliedPragma("synchronous");
    REQUIRE(applied.ok());
    REQUIRE(applied.value() == "2");
  }

  SECTION("访问器只放行白名单内的 PRAGMA 名（PRAGMA 名不能参数化）") {
    auto opened = SqliteMetadataRepository::Open(dir.child("guard.db"), clock);
    REQUIRE(opened.ok());
    const auto rejected = opened.value()->AppliedPragma("journal_mode; DROP TABLE metadata");
    REQUIRE_FALSE(rejected.ok());
    REQUIRE(rejected.error().kind() == fss::ErrorKind::kInvalidArgument);
    REQUIRE(opened.value()->AppliedPragma("synchronous").ok());
  }
}
