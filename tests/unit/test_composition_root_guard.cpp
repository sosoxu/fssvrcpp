// =============================================================================
//  C4.9：组合根纪律护栏（R12）
// =============================================================================
//  `docs/02-design.md` §4 的纪律是：**具体实现只能在组合根 `src/main/` 创建**。
//  这条纪律最容易破的方式不是"有人故意违反"，而是**顺手**：
//    · 在某个用例里 `InMemoryBlobStore fallback(clock);` 兜个底；
//    · 在适配层 `auto server = std::make_unique<fss::http::Server>(...)` 图省事；
//    · 在 L2 里 `SqliteLocationRepository::Open(...)` 直接开库。
//  三者都不会破坏编译期分层图（类型在 L2/L5，谁都能 include 头文件），
//  但会让"换驱动/换实现"重新变成改多处 —— 那正是 R12 要防的东西。
//
//  ★ 护栏不是证明，它提高违规成本（与 C2.1/C2.7 同类）。已知边界：
//    ① 只认"构造形态"（`make_unique<T>` / `make_shared<T>` / `T name(` / `T(` /
//       白名单静态工厂 `T::Open(`），**不认**"通过中间引用传递后再构造"；
//    ② 无法识别 typedef/auto 掩盖的构造；
//    ③ 头文件里写"构造示例"的注释会被剥离（StripComments），所以文档不会误报。
//
//  自证：`scripts/verify_composition_root.sh`（R1）—— 注入一次真实违规必须失败，
//  注入一次"纯引用/声明"必须放行。
// =============================================================================
#include <catch2/catch.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifndef FSS_REPO_ROOT
#error "FSS_REPO_ROOT 未定义：组合根护栏需要知道源码树位置"
#endif

//  具体实现清单：{类型名, 所属目录（相对 src/，该目录内的构造是"自己的定义"，放行）,
//                 是否还有静态工厂}
struct ConcreteType {
  const char* name;
  const char* owner_dir;   // 允许在其中构造的目录（实现自己的家）
  bool static_factory;     // 额外认 `Type::`（如 `SqliteLocationRepository::Open`）
};

const std::vector<ConcreteType>& Inventory() {
  static const std::vector<ConcreteType> types = {
      // L2：存储 / 仓储 / 传输 / IO
      {"PosixBlobStore", "infra/blob/posix/", false},
      {"InMemoryBlobStore", "infra/blob/memory/", false},
      {"SqliteLocationRepository", "infra/location/sqlite/", true},
      {"InMemoryLocationRepository", "infra/location/memory/", false},
      {"InMemoryMetadataRepository", "infra/metadata/memory/", false},
      //  ★ P9：组合根按 ADR-004 改接内置 SQLite 元数据仓储（单实例语义：重启后记录还在）。
      //    清单必须同步 —— 否则"组合根里装配了几个具体实现"这个**非空洞性**断言会
      //    因为"类名换了"而误报（实测：6 < 7 直接失败）。这正是它该有的行为。
      {"SqliteMetadataRepository", "infra/metadata/sqlite/", true},
      {"HmacTransferTokenCodec", "infra/transfer/", false},
      {"BlobByteSource", "infra/transfer/", false},
      {"BlockingIoEngine", "infra/io/", false},
      {"UringIoEngine", "infra/io/", false},
      //  ★ P10 切片 6a（ADR-013）：远端 legal / schema 校验器也是"只能在组合根装配"的
      //    具体实现 —— 用例层只依赖 `ILegalValidator` / `ISchemaValidator` 端口。
      {"RemoteLegalValidator", "infra/legal/", false},
      {"RemoteSchemaValidator", "infra/schema/", false},
      //  ⚠️ 刻意**不**收录 L1 的注入点（`SystemClock` / `UuidGenerator` / `StreamLogger`）：
      //     C4.9 的判据范围是"基础设施实现（blob store / repository / driver）"，
      //     而 L1 的注入点与使用者在同一层。已知的既有偏离如实记录（不静默）：
      //     `src/common/http/server.cpp` 里 `UuidGenerator ids;` —— 相关性 ID 用的是
      //     具体实现而不是注入的 `IIdGenerator`。它不影响本判据，登记在 phase4 证据里。
      // L5：HTTP 服务器与路由器也必须在组合根装配
      {"Router", "adapters/http/", false},
      {"Server", "common/http/", false},
  };
  return types;
}

//  组合根本身（唯一允许装配具体实现的地方）+ 契约里明确豁免的目录
bool IsAllowedFile(const std::string& rel) {
  return rel.rfind("main/", 0) == 0;
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::vector<fs::path> CollectSources(const fs::path& root) {
  std::vector<fs::path> files;
  if (!fs::exists(root)) return files;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    const auto ext = entry.path().extension().string();
    if (ext == ".h" || ext == ".hpp" || ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

//  去掉注释与字符串字面量内容（保留换行 → 行号不变）
std::string StripComments(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  bool in_line_comment = false;
  bool in_block_comment = false;
  bool in_string = false;
  char quote = '\0';
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const char n = (i + 1 < text.size()) ? text[i + 1] : '\0';
    if (in_line_comment) {
      if (c == '\n') {
        in_line_comment = false;
        out.push_back(c);
      }
      continue;
    }
    if (in_block_comment) {
      if (c == '*' && n == '/') {
        in_block_comment = false;
        ++i;
      } else if (c == '\n') {
        out.push_back(c);
      }
      continue;
    }
    if (in_string) {
      if (c == '\\' && n != '\0') {
        ++i;
      } else if (c == quote) {
        in_string = false;
        out.push_back(c);
      } else if (c == '\n') {
        out.push_back(c);
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      in_string = true;
      quote = c;
      out.push_back(c);
      continue;
    }
    if (c == '/' && n == '/') {
      in_line_comment = true;
      continue;
    }
    if (c == '/' && n == '*') {
      in_block_comment = true;
      ++i;
      continue;
    }
    out.push_back(c);
  }
  return out;
}

struct Hit {
  std::string file;
  int line = 0;
  std::string token;
};

//  变量定义形态：`PosixBlobStore blob_store(root, clock);` / `SystemClock clock;`
//  —— 只匹配 `Type` 后面紧跟"标识符 + (` 或 `{` 或 `;`"，所以
//    `class Server;`（前向声明）、`ServerOptions options;`、`Type& ref` 都不会命中。
const std::regex& DefinitionPattern(const std::string& name) {
  static std::map<std::string, std::regex> cache;
  const auto it = cache.find(name);
  if (it != cache.end()) return it->second;
  //  变量定义：`[ns::]Type name(` / `{` / `;`
  const std::regex pattern("\\b(?:[A-Za-z_][A-Za-z0-9_]*::)*" + name +
                           "\\s+[A-Za-z_][A-Za-z0-9_]*\\s*[({;]");
  return cache.emplace(name, pattern).first->second;
}

//  构造形态：`make_unique<[ns::]Type>` / `make_shared<...>` / `new [ns::]Type`
//  ★ 必须支持限定名：真实代码写的是 `std::make_unique<fss::http::Server>(...)`
const std::regex& CreationPattern(const std::string& name) {
  static std::map<std::string, std::regex> cache;
  const auto it = cache.find(name);
  if (it != cache.end()) return it->second;
  const std::string qualified = "(?:[A-Za-z_][A-Za-z0-9_]*::)*" + name;
  const std::regex pattern("(make_unique|make_shared)<" + qualified + "[>,]|\\bnew\\s+" +
                           qualified + "\\b");
  return cache.emplace(name, pattern).first->second;
}

//  扫描逻辑独立成函数 → 可以先对**合成文本**自证（不依赖真实源码树）
std::vector<Hit> ScanText(const std::string& rel, const std::string& raw_text) {
  const std::string text = StripComments(raw_text);
  std::vector<Hit> hits;
  std::istringstream is(text);
  std::string line;
  int lineno = 0;
  while (std::getline(is, line)) {
    ++lineno;
    for (const auto& type : Inventory()) {
      const std::string name = type.name;
      //  "自己的家"里构造自己 → 放行
      if (rel.rfind(type.owner_dir, 0) == 0) continue;
      bool found = false;
      if (std::regex_search(line, CreationPattern(name))) {
        hits.push_back(Hit{rel, lineno, name + "（make_unique/make_shared/new）"});
        found = true;
      }
      if (!found && type.static_factory && line.find(name + "::") != std::string::npos) {
        hits.push_back(Hit{rel, lineno, name + "::（静态工厂）"});
        found = true;
      }
      if (!found && std::regex_search(line, DefinitionPattern(name))) {
        hits.push_back(Hit{rel, lineno, name + " <identifier> ..."});
      }
    }
  }
  return hits;
}

}  // namespace

TEST_CASE("★ C4.9 组合根护栏的扫描器本身有效（合成文本）", "[phase4][guard][c4.9]") {
  //  必须检出的构造形态
  REQUIRE_FALSE(ScanText("app/usecases/x.cpp", "fss::infra::InMemoryBlobStore store(clock);")
                    .empty());
  REQUIRE_FALSE(ScanText("app/usecases/x.cpp", "auto s = std::make_unique<PosixBlobStore>(a, b);")
                    .empty());
  REQUIRE_FALSE(ScanText("adapters/http/router.cpp",
                         "auto server = std::make_unique<fss::http::Server>(o, l, c);")
                    .empty());
  REQUIRE_FALSE(ScanText("app/usecases/x.cpp",
                         "auto repo = SqliteLocationRepository::Open(path);")
                    .empty());
  REQUIRE_FALSE(ScanText("app/services/other.cpp", "auto* s = new PosixBlobStore(a, b);").empty());
  //  ★ 限定名形态（真实代码就是这么写的）
  REQUIRE_FALSE(ScanText("app/usecases/x.cpp",
                         "auto s = std::make_unique<fss::infra::InMemoryBlobStore>(clock);")
                    .empty());
  REQUIRE_FALSE(ScanText("app/usecases/x.cpp",
                         "fss::infra::SqliteLocationRepository repo;")
                    .empty());

  //  不得误判：纯引用 / 声明 / 参数类型 / 注释 / 字符串
  REQUIRE(ScanText("app/usecases/x.cpp", "fss::domain::IBlobStore* store = nullptr;").empty());
  REQUIRE(ScanText("app/usecases/x.cpp", "class X { IBlobStore& store_; };").empty());
  REQUIRE(ScanText("app/usecases/x.cpp", "//   InMemoryBlobStore store(clock); 设计说明").empty());
  REQUIRE(ScanText("app/usecases/x.cpp", "const char* s = \"make_unique<Router>\";").empty());
  //  实现自己的目录里构造自己 → 放行（否则每个实现文件都会自报违规）
  REQUIRE(ScanText("infra/blob/posix/posix_blob_store.cpp", "PosixBlobStore::PosixBlobStore(...)")
              .empty());
  REQUIRE(ScanText("infra/blob/memory/memory_blob_store.cpp", "InMemoryBlobStore store(clock);")
              .empty());
  //  组合根里的构造能被检出（用于下面的非空洞性断言）
  REQUIRE(ScanText("main/server_main.cpp",
                   "auto metadata_repository = SqliteMetadataRepository::Open(path, clock);")
              .size() == 1);
}

TEST_CASE("★ C4.9 具体实现只在组合根装配（R12）", "[phase4][guard][c4.9]") {
  const fs::path src_root = fs::path(FSS_REPO_ROOT) / "src";
  const auto files = CollectSources(src_root);
  REQUIRE_FALSE(files.empty());

  std::vector<Hit> violations;
  std::vector<std::string> instantiated_in_main;
  for (const auto& file : files) {
    const std::string rel = fs::relative(file, src_root).generic_string();
    const auto hits = ScanText(rel, ReadFile(file));
    if (hits.empty()) continue;
    if (IsAllowedFile(rel)) {
      for (const auto& hit : hits) instantiated_in_main.push_back(hit.token);
      continue;
    }
    for (const auto& hit : hits) violations.push_back(hit);
  }

  //  ★ 非空洞性（R1）：如果组合根里根本没构造任何具体实现，这条护栏的"通过"是假象
  //    —— 与 C2.1 的"静态库 link.txt 是空证据"同类陷阱
  std::sort(instantiated_in_main.begin(), instantiated_in_main.end());
  instantiated_in_main.erase(
      std::unique(instantiated_in_main.begin(), instantiated_in_main.end()),
      instantiated_in_main.end());
  INFO("组合根里装配的具体实现类型数：" << instantiated_in_main.size());
  //  实测值 7（PosixBlobStore / SqliteLocationRepository / SqliteMetadataRepository /
  //  HmacTransferTokenCodec / BlobByteSource / Router / Server）。留 0 余量：一旦有人把
  //  装配从组合根挪走或删空，这条断言立刻失败
  REQUIRE(instantiated_in_main.size() >= 7);

  std::ostringstream report;
  for (const auto& v : violations) {
    report << "\n  src/" << v.file << ":" << v.line << "  出现 '" << v.token << "'";
  }
  INFO("越权装配具体实现：" << report.str());
  REQUIRE(violations.empty());
}
