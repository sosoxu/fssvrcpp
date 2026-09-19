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

//  原始字符串字面量的**定界符**（`R"<delim>(` 里的 `<delim>`）
struct RawDelimiter {
  std::string file;
  int line = 0;
  std::string delimiter;
};

//  提取文件里所有原始字符串字面量的定界符。
//  为什么需要它（护栏盲区的补丁，见下面的用例）：
//    `ExtractSqlLiterals` 只认 `R"sql(...)sql"`。若某条 SQL 写成 `R"pgsql(...)pgsql"`，
//    partition_id 扫描器就**看不见它** —— 于是"每条 DML 都必须带 partition_id"这条
//    机械规则对那份 SQL **恒为真**（与 P2-D07 的 `phase012` 空集合同类）。
//    所以仓储实现文件里必须禁止非 `sql` 定界符，而不是靠"大家都记得用 sql"。
std::vector<RawDelimiter> ExtractRawDelimiters(const std::string& relative,
                                               const std::string& source) {
  std::vector<RawDelimiter> out;
  std::size_t pos = 0;
  while (true) {
    const std::size_t at = source.find("R\"", pos);
    if (at == std::string::npos) break;
    const std::size_t delim_begin = at + 2;
    const std::size_t paren = source.find('(', delim_begin);
    //  定界符最长 16 字符、不含空白/括号/反斜杠（C++ 标准）；不像定界符就跳过
    if (paren == std::string::npos || paren - delim_begin > 16) {
      pos = delim_begin;
      continue;
    }
    const std::string delimiter = source.substr(delim_begin, paren - delim_begin);
    const bool plausible = std::all_of(delimiter.begin(), delimiter.end(), [](char c) {
      return c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\\' && c != ')';
    });
    if (!plausible) {
      pos = delim_begin;
      continue;
    }
    out.push_back(RawDelimiter{relative,
                               static_cast<int>(std::count(source.begin(), source.begin() + at,
                                                           '\n')) + 1,
                               delimiter});
    pos = paren + 1;
  }
  return out;
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

  //  定界符提取器自证：非 `sql` 定界符必须被识别出来。
  //  没有这条，"仓储文件不得用非 sql 定界符"的禁令就是恒真的（扫不到任何东西也叫通过）。
  const auto delimiters = ExtractRawDelimiters(
      "sample.cpp",
      "const char* a = R\"sql(SELECT 1 FROM t WHERE partition_id = ?)sql\";\n"
      "const char* b = R\"pgsql(SELECT 1 FROM t)pgsql\";\n");
  REQUIRE(delimiters.size() == 2);
  REQUIRE(delimiters[0].delimiter == "sql");
  REQUIRE(delimiters[0].line == 1);
  REQUIRE(delimiters[1].delimiter == "pgsql");
  REQUIRE(delimiters[1].line == 2);
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

// -----------------------------------------------------------------------------
//  补丁：仓储实现文件**不得**用非 `sql` 定界符写原始字符串
// -----------------------------------------------------------------------------
//  动机（本切片引入第一个 `R"pgsql(...)pgsql"` 时发现的盲区）：
//    上面的扫描器只认 `R"sql(...)sql"`。只要把 SQL 换个定界符，它就从扫描范围里
//    消失，而"每条 DML 都带 partition_id"这条规则**不会因此失败** —— 检查静默变成
//    只覆盖一部分 SQL。留一个"想绕过就绕过"的通道，比不检查更危险（它给的是
//    虚假的安全感）。因此把范围钉在**仓储实现文件**：
//      `src/**/*_repository.{h,cpp}` 里出现的每个原始字符串字面量都必须是 `R"sql(`。
//    数据库时钟那类"不访问任何表"的语句不在仓储文件里（在 L2 封装
//    `src/infra/postgres/pg_connection.*`），因此不受此约束 —— 那里用独立定界符
//    仍然写明了理由，且本用例不覆盖它。
//  ★ 非空洞性：必须真的扫到仓储文件、也真的扫到原始字符串，否则"通过"是空集合。
TEST_CASE("★ C3.9 仓储实现文件不得用非 sql 定界符藏 SQL（护栏盲区补丁）",
          "[phase3][guard][c3.9]") {
  const fs::path src_root = fs::path(FSS_REPO_ROOT) / "src";
  std::size_t repository_files = 0;
  std::size_t raw_literals = 0;
  std::vector<RawDelimiter> violations;

  for (const auto& file : CollectSources(src_root)) {
    if (file.filename().string().find("_repository.") == std::string::npos) continue;
    ++repository_files;
    const std::string relative = fs::relative(file, src_root).generic_string();
    for (const auto& delimiter : ExtractRawDelimiters(relative, ReadFile(file))) {
      ++raw_literals;
      if (delimiter.delimiter != "sql") violations.push_back(delimiter);
    }
  }

  REQUIRE(repository_files > 0);
  REQUIRE(raw_literals > 0);

  std::ostringstream report;
  for (const auto& v : violations) {
    report << "\n  src/" << v.file << ":" << v.line << " —— 定界符 R\"" << v.delimiter
           << "(\" 会让 C3.9 的 partition_id 扫描器看不见这条 SQL";
  }
  INFO("护栏盲区：" << report.str());
  REQUIRE(violations.empty());
}
