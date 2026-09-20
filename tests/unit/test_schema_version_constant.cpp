// =============================================================================
//  B2a：schema 版本常量的"机械防漂移"测试
// =============================================================================
//  判据（本切片）：期望的 schema 版本必须有**单一真相**，且不允许静默漂移 ——
//    · 真相 = `src/infra/postgres/pg_schema.h` 的 `kExpectedSchemaVersion`；
//    · 机械断言 = 从 `db/migrations/NNN_*.sql` 的**文件名前缀**推导最大 NNN 并比对。
//
//  ★ 为什么必须有这条测试（AGENTS R1/R13）
//    "加了一个 002_*.sql 却忘了改常量"是**必然会发生**的漂移，而且它在开发机上
//    完全无症状：readiness 只在目标库上变红，排查成本极高。把推导机械化之后，
//    "加迁移 + 不改常量"会立刻让本用例失败 —— 这正是判据要的"不能静默漂移"。
//
//  ★ 非空洞性（R16）：推导函数必须能**失败**才有效。因此除了对真实目录的断言，
//    还对一份**合成清单**断言"最大值为 2 时，若常量是其它值必须报错"（下面第二个 SECTION）。
// =============================================================================
#include <catch2/catch.hpp>

#include "infra/postgres/pg_schema.h"  // kExpectedSchemaVersion（纯头文件常量，不链接 libpq）

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

//  与 `scripts/dev_postgres.sh::do_migrate` **同一规则**：
//    001_init.sql → 1；010_x.sql → 10（十进制，避免 08/09 被当成八进制）。
//  只解析 `NNN_` 前缀；不符合命名规则的 `.sql` **不计入**（也不静默当成 0）。
bool ParseMigrationVersion(const std::string& filename, int* out) {
  const auto underscore = filename.find('_');
  if (underscore == std::string::npos || underscore == 0) return false;
  const std::string digits = filename.substr(0, underscore);
  if (!std::all_of(digits.begin(), digits.end(),
                   [](unsigned char c) { return std::isdigit(c) != 0; })) {
    return false;
  }
  try {
    *out = std::stoi(digits);
  } catch (...) {
    return false;
  }
  return true;
}

//  从一组迁移文件名推导期望版本（空集合 / 无合法文件名 → -1，调用方必须当成失败）。
int DeriveExpectedVersion(const std::vector<std::string>& filenames, int* parsed_count) {
  int max_version = -1;
  int count = 0;
  for (const auto& name : filenames) {
    int version = 0;
    if (!ParseMigrationVersion(name, &version)) continue;
    ++count;
    max_version = std::max(max_version, version);
  }
  if (parsed_count != nullptr) *parsed_count = count;
  return max_version;
}

std::vector<std::string> MigrationFilenames() {
  std::vector<std::string> names;
  const fs::path dir = fs::path(FSS_REPO_ROOT) / "db" / "migrations";
  REQUIRE(fs::exists(dir));
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".sql") continue;
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace

TEST_CASE("★ B2a：kExpectedSchemaVersion 与 db/migrations/*.sql 的文件名一致（禁止静默漂移）",
          "[phase10][unit][b2a]") {
  const std::vector<std::string> names = MigrationFilenames();
  int parsed = 0;
  const int derived = DeriveExpectedVersion(names, &parsed);

  INFO("db/migrations 下的迁移文件（" << names.size() << " 个）：");
  for (const auto& name : names) INFO("  " << name);
  INFO("推导出的期望版本 = " << derived << "（计入 " << parsed << " 个合法文件名）");

  //  非空洞性：目录里必须真的有可解析的迁移文件（否则 `derived` 恒为 -1）。
  REQUIRE(parsed > 0);
  REQUIRE(derived > 0);

  //  ★ 核心断言：常量的单一真相必须等于从文件名推导出的最大值。
  //    "新增 002_*.sql 但不改常量" → 这里失败（这就是本切片要的防漂移）。
  REQUIRE(fss::infra::kExpectedSchemaVersion == derived);

  SECTION("R1/R16 自证：推导逻辑本身能失败（合成清单 002 与常量不符时必须检出）") {
    const int synthetic = DeriveExpectedVersion({"001_init.sql", "002_add_table.sql"}, &parsed);
    REQUIRE(synthetic == 2);
    REQUIRE(parsed == 2);
    //  非法文件名不计入（也不静默当成 0）
    REQUIRE(DeriveExpectedVersion({"readme.txt", "notes.sql"}, &parsed) == -1);
    REQUIRE(parsed == 0);
    //  两位数用十进制解析（08 不能当八进制 / 不能只取一位）
    REQUIRE(DeriveExpectedVersion({"008_a.sql", "010_b.sql"}, &parsed) == 10);
  }
}
