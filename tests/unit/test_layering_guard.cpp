// =============================================================================
//  分层护栏（源码级）—— 证明"低耦合"是约束，不是口号
// =============================================================================
//
//  为什么有了 CMake 目标图还要这个测试：
//    目标图能捕获"链接到了不该链接的库"，但捕获不了：
//      * 头文件里的间接引入（例如领域层的头文件 include 了 common/http 的头）
//      * 通过模板/内联代码引入的依赖
//      * 新加的源文件尚未被 CMake 的源列表收录（因此根本没参与编译）
//    本测试直接**扫描源码树**，与 CMake 构建无关 —— 因此对上述情况都有效。
//
//  ★ 按 AGENTS.md 铁律 R1：本测试自身必须能失败。
//    由 scripts/verify_guard.sh 注入违规文件并断言本测试失败来验证。
//    一个永远通过的护栏测试等于没有护栏。
// =============================================================================

#include <catch2/catch.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// 由 CMake 注入（tests/CMakeLists.txt）
#ifndef FSS_REPO_ROOT
#error "FSS_REPO_ROOT 未定义：分层护栏需要知道源码树位置"
#endif

struct Rule {
  std::string name;
  // 相对 src/ 的路径前缀（空串 = 全树）
  std::vector<std::string> scopes;
  // 不允许出现的字面量
  std::vector<std::string> forbidden;
  // 例外：相对 src/ 的路径前缀（该前缀下的文件豁免）
  std::vector<std::string> exempt;
  // true = 只检查 #include 行（避免把注释/文档里的词误判为违规）
  bool include_only = true;
  std::string why;
};

std::vector<Rule> BuildRules() {
  return {
      Rule{"L3 领域层不得依赖基础设施",
           {"domain/"},
           {"<grpc", "<sqlite3.h>", "<curl/", "<openssl/", "httplib", "common/http"},
           {},
           true,
           "领域层必须能在无 gRPC/DB/HTTP/OpenSSL 的环境下编译与单测（docs/02-design.md §4）"},

      Rule{"L4 应用层不得依赖基础设施",
           {"app/"},
           {"<grpc", "<sqlite3.h>", "<curl/", "<openssl/", "httplib", "common/http"},
           {},
           true,
           "应用层只依赖领域类型与端口；IO 细节属于 L2/L5"},

      Rule{"httplib 的 include 只能出现在 fss_http 包装层内",
           {""},                       // 全树
           {"httplib"},                // 命中 #include ... httplib ...
           {"common/http/"},           // 唯一豁免
           true,
           "换库不应触碰领域/应用/适配层（ADR-002 §5.1）"},

      Rule{"httplib 的符号只能出现在 fss_http 包装层内",
           {""},
           {"httplib::"},
           {"common/http/"},
           false,                      // 检查所有行（符号使用）
           "包装层之外不得泄漏第三方库类型（ADR-002 §5）"},

      Rule{"HTTP 适配层不得直接使用 protobuf 类型",
           {"adapters/http/"},
           {"osdu/file/v1", "<grpcpp/", "<google/protobuf"},
           {},
           true,
           "两个协议适配器必须互相独立（docs/02-design.md §3.2）"},

      Rule{"gRPC 适配层不得使用 HTTP 内核",
           {"adapters/grpc/"},
           {"httplib", "common/http"},
           {},
           true,
           "两个协议适配器必须互相独立"},

      Rule{"L1 通用库不得反向依赖上层",
           {"common/"},
           {"#include \"domain/", "#include \"app/", "#include \"adapters/",
            "#include \"infra/", "#include \"main/"},
           {},
           true,
           "依赖方向只能向下；L1 必须可被任何上层复用"},
  };
}

std::string ReadFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool UnderAnyPrefix(const std::string& rel, const std::vector<std::string>& prefixes) {
  if (prefixes.empty()) return false;
  for (const auto& pre : prefixes) {
    if (pre.empty()) return true;  // 空串 = 全树
    if (rel.rfind(pre, 0) == 0) return true;
  }
  return false;
}

std::vector<fs::path> CollectSources(const fs::path& src_root) {
  std::vector<fs::path> files;
  if (!fs::exists(src_root)) return files;
  for (const auto& entry : fs::recursive_directory_iterator(src_root)) {
    if (!entry.is_regular_file()) continue;
    const auto ext = entry.path().extension().string();
    if (ext == ".h" || ext == ".hpp" || ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

struct Violation {
  std::string rule;
  std::string file;
  int line;
  std::string token;
  std::string why;
};

std::vector<Violation> ScanAll() {
  const fs::path src_root = fs::path(FSS_REPO_ROOT) / "src";
  const auto files = CollectSources(src_root);
  const auto rules = BuildRules();

  std::vector<Violation> out;
  for (const auto& f : files) {
    const std::string rel = fs::relative(f, src_root).generic_string();
    const std::string text = ReadFile(f);
    std::istringstream is(text);
    std::string line;
    int lineno = 0;
    while (std::getline(is, line)) {
      ++lineno;
      // 判定"这是不是一个 include 行"：第一个非空白字符处必须正好是 "#include"。
      // （不要写成 find("#include") < find_first_not_of(...) —— 那样第 0 列的
      //   真正 include 会被判为 false，护栏就形同虚设。）
      const bool is_include = line.find_first_not_of(" \t") == line.find("#include");
      for (const auto& rule : rules) {
        if (!rule.scopes.empty() && !UnderAnyPrefix(rel, rule.scopes)) continue;
        if (UnderAnyPrefix(rel, rule.exempt)) continue;
        if (rule.include_only && !is_include) continue;
        for (const auto& tok : rule.forbidden) {
          if (line.find(tok) != std::string::npos) {
            out.push_back({rule.name, rel, lineno, tok, rule.why});
          }
        }
      }
    }
  }
  return out;
}

}  // namespace

TEST_CASE("护栏扫描器本身是有效的（对已知违规必须报错）", "[phase1][guard]") {
  // 构造式自证：直接调用扫描逻辑对一段合成文本的判断能力。
  // 目的：确保 test_layering_guard 的"通过"不是因为扫描器坏了。
  // 端到端自证（注入真实文件）见 scripts/verify_guard.sh。
  const auto rules = BuildRules();
  auto would_flag = [&](const std::string& scope, const std::string& text) {
    const bool is_include = text.find_first_not_of(" \t") == text.find("#include");
    for (const auto& r : rules) {
      if (!r.scopes.empty() && !UnderAnyPrefix(scope, r.scopes)) continue;
      if (UnderAnyPrefix(scope, r.exempt)) continue;
      if (r.include_only && !is_include) continue;
      for (const auto& tok : r.forbidden) {
        if (text.find(tok) != std::string::npos) return true;
      }
    }
    return false;
  };

  // 必须报错的几种真实违规形态
  REQUIRE(would_flag("domain/foo.cpp", "#include <grpcpp/grpcpp.h>"));
  REQUIRE(would_flag("domain/foo.cpp", "#include <sqlite3.h>"));
  REQUIRE(would_flag("app/foo.cpp", "#include \"common/http/server.h\""));
  REQUIRE(would_flag("adapters/http/foo.cpp", "#include \"osdu/file/v1/file_service.pb.h\""));
  REQUIRE(would_flag("adapters/grpc/foo.cpp", "#include <httplib.h>"));
  REQUIRE(would_flag("common/crypto/x.cpp", "#include \"domain/foo.h\""));
  // 符号使用（非 include）
  REQUIRE(would_flag("app/foo.cpp", "httplib::Server s;"));

  // 不得误报的合规形态
  REQUIRE_FALSE(would_flag("domain/foo.cpp", "#include \"common/result/result.h\""));
  REQUIRE_FALSE(would_flag("common/crypto/x.cpp", "#include <openssl/evp.h>"));
  REQUIRE_FALSE(would_flag("common/http/server.cpp", "#include <httplib.h>"));
  // ★ 回归断言：位于第 0 列的真正 include 必须被检出
  //   （曾写成 find("#include") < find_first_not_of(...)，导致首列 include 漏检）
  REQUIRE(would_flag("domain/foo.cpp", "#include <grpcpp/grpcpp.h>"));
  REQUIRE(would_flag("domain/foo.cpp", "  #include <grpcpp/grpcpp.h>"));   // 缩进也算
  // 注释里提到这些词不算违规（include_only 的作用）
  REQUIRE_FALSE(would_flag("domain/foo.cpp", "// 禁止 #include <grpcpp/grpcpp.h>"));
  REQUIRE_FALSE(would_flag("domain/foo.cpp", "// 本层禁止 include <grpcpp/grpcpp.h> 与 httplib"));
}

TEST_CASE("源码树中不存在分层违规", "[phase1][guard]") {
  const auto violations = ScanAll();

  std::ostringstream report;
  for (const auto& v : violations) {
    report << "\n  [" << v.rule << "] src/" << v.file << ":" << v.line << "  出现 '" << v.token
           << "'\n      原因：" << v.why;
  }

  INFO("分层违规：" << report.str());
  REQUIRE(violations.empty());
}

TEST_CASE("护栏覆盖了关键目录（防止规则被误删而测试仍通过）",
          "[phase1][guard]") {
  const fs::path src_root = fs::path(FSS_REPO_ROOT) / "src";
  const auto files = CollectSources(src_root);

  // 如果扫描不到文件，说明路径注入错了 —— 那时"无违规"是假象
  REQUIRE_FALSE(files.empty());

  // 规则表必须包含这些关键约束（防止有人"顺手"删掉一条规则让测试变绿）
  const auto rules = BuildRules();
  auto has_rule_for = [&](const std::string& scope, const std::string& token) {
    for (const auto& r : rules) {
      if (!UnderAnyPrefix(scope, r.scopes)) continue;
      if (std::find(r.forbidden.begin(), r.forbidden.end(), token) != r.forbidden.end()) {
        return true;
      }
    }
    return false;
  };
  REQUIRE(has_rule_for("domain/x.cpp", "<grpc"));
  REQUIRE(has_rule_for("app/x.cpp", "<sqlite3.h>"));
  REQUIRE(has_rule_for("adapters/http/x.cpp", "osdu/file/v1"));
  REQUIRE(has_rule_for("common/foo/x.cpp", "httplib"));
}
