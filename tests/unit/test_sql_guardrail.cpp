// =============================================================================
//  C3.9：「无 partition_id 条件的 SQL」护栏
// =============================================================================
//  分区隔离是**契约级**的（`ports.h`：每个仓储方法都带 partition）。
//  但在 SQL 里漏一个 `WHERE partition_id = ?` 不会编译报错，只会静默串租户 ——
//  这是最难在代码评审里看出来的缺陷类型。因此把它机械化：
//
//    扫描 `src/` 下所有 `R"sql( ... )sql"` 原始字符串里的 DML 语句，
//    每条都必须出现 `partition_id`。
//
//  ⚠️ 启发式边界（如实说明）：只覆盖"按约定写成 R"sql(...)sql" 的 SQL"。
//     内联拼接的 SQL 抓不到 —— 因此本仓库的约定是**所有 SQL 都写原始字符串**，
//     并由"非空洞性断言"保证扫描到的语句数 > 0。自证见本文件第一个用例。
// =============================================================================
#include <catch2/catch.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifndef FSS_REPO_ROOT
#error "FSS_REPO_ROOT 未定义：SQL 护栏需要知道源码树位置"
#endif

struct SqlLiteral {
  std::string file;
  int line = 0;
  std::string text;
};

//  提取 `R"sql( ... )sql"` 的**内容**（不含定界符）
std::vector<SqlLiteral> ExtractSqlLiterals(const std::string& relative,
                                           const std::string& source) {
  static const std::string kOpen = "R\"sql(";
  static const std::string kClose = ")sql\"";
  std::vector<SqlLiteral> out;
  std::size_t pos = 0;
  while (true) {
    const std::size_t begin = source.find(kOpen, pos);
    if (begin == std::string::npos) break;
    const std::size_t content = begin + kOpen.size();
    const std::size_t end = source.find(kClose, content);
    if (end == std::string::npos) break;
    const int line = static_cast<int>(std::count(source.begin(), source.begin() + begin, '\n')) + 1;
    out.push_back(SqlLiteral{relative, line, source.substr(content, end - content)});
    pos = end + kClose.size();
  }
  return out;
}

bool HasDmlKeyword(const std::string& sql) {
  const std::string upper = [&] {
    std::string out(sql);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
  }();
  for (const char* keyword : {"SELECT ", "INSERT ", "UPDATE ", "DELETE "}) {
    if (upper.find(keyword) != std::string::npos) return true;
  }
  return false;
}

std::vector<fs::path> CollectSources(const fs::path& root) {
  std::vector<fs::path> files;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    const auto ext = entry.path().extension().string();
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".h" || ext == ".hpp") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

TEST_CASE("SQL 扫描器本身有效（合成语句必须被判定）", "[phase3][guard][c3.9]") {
  const auto guard = [](const std::string& sql) {
    return HasDmlKeyword(sql) && sql.find("partition_id") == std::string::npos;
  };
  //  违规形态
  REQUIRE(guard("SELECT data FROM file_locations WHERE file_id = ?"));
  REQUIRE(guard("DELETE FROM file_locations WHERE file_id = ?"));
  //  合规形态
  REQUIRE_FALSE(guard("SELECT data FROM file_locations WHERE partition_id = ? AND file_id = ?"));
  //  DDL / 非 DML 不算（它们要么本来就有 partition_id，要么不按行隔离）
  REQUIRE_FALSE(HasDmlKeyword("PRAGMA journal_mode=WAL"));

  //  提取器自证
  const std::string sample =
      "const char* a = R\"sql(SELECT 1 FROM t WHERE partition_id = ?)sql\";\n"
      "const char* b = R\"sql(DELETE FROM t WHERE x = ?)sql\";";
  const auto literals = ExtractSqlLiterals("sample.cpp", sample);
  REQUIRE(literals.size() == 2);
  REQUIRE(literals[0].line == 1);
  REQUIRE(literals[1].line == 2);
}

TEST_CASE("★ C3.9 源码树里不存在缺少 partition_id 的 DML 语句",
          "[phase3][guard][c3.9]") {
  const fs::path src_root = fs::path(FSS_REPO_ROOT) / "src";
  std::size_t dml_total = 0;
  std::vector<SqlLiteral> violations;

  for (const auto& file : CollectSources(src_root)) {
    const std::string relative = fs::relative(file, src_root).generic_string();
    for (const auto& literal : ExtractSqlLiterals(relative, ReadFile(file))) {
      if (!HasDmlKeyword(literal.text)) continue;
      ++dml_total;
      if (literal.text.find("partition_id") == std::string::npos) violations.push_back(literal);
    }
  }

  //  ★ 非空洞性：如果一条 SQL 都没扫到，"通过"毫无意义（与 C2.1 的空证据同类）
  REQUIRE(dml_total > 0);

  std::ostringstream report;
  for (const auto& v : violations) {
    report << "\n  src/" << v.file << ":" << v.line << " —— 该 DML 缺少 partition_id";
  }
  INFO("越权 SQL：" << report.str());
  REQUIRE(violations.empty());
}
