// =============================================================================
//  C2.7：`capabilities()` 调用点护栏（+ C2.6 的源码级"无驱动类型分支"检查）
// =============================================================================
//  ADR-003 的承诺是"按能力编程"，而承诺最容易被破坏的方式不是改接口，而是
//  在某个用例里顺手写一句 `store->capabilities().native_presign` —— 源码里看不出
//  分层违规，接口也没变，但能力分支已经扩散了。因此用**源码检索**把它变成可执行约束：
//
//     `capabilities()` 只允许被 `app/services/location_issuer.cpp` 与
//     `app/services/storage_instruction_service.cpp` 调用。
//
//  ⚠️ 启发式边界（如实说明）：扫描的是 `.capabilities(` / `->capabilities(` 两种形态，
//     因此"通过一个中间引用再调用"（如 `auto s = &store; s->capabilities()`）仍然叫
//     `s->capabilities(`，能被抓到；但"把能力拷进别的结构再读"抓不到。护栏不是证明，
//     它提高违规成本。自证见 `scripts/verify_capability_guard.sh`（R1）。
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

#ifndef FSS_REPO_ROOT
#error "FSS_REPO_ROOT 未定义：能力护栏需要知道源码树位置"
#endif

//  白名单（相对 src/）。`storage_instruction_service.cpp` 属 P3/P4，先登记后实现。
const std::vector<std::string>& AllowedCallSites() {
  static const std::vector<std::string> allowed = {
      "app/services/location_issuer.cpp",
      "app/services/storage_instruction_service.cpp",
  };
  return allowed;
}

bool IsAllowed(const std::string& rel) {
  const auto& allowed = AllowedCallSites();
  return std::find(allowed.begin(), allowed.end(), rel) != allowed.end();
}

//  ★ 装饰器（decorator）：它**必须**实现 `capabilities()`，但只能是"原样转发" ——
//  那不是"按能力分支"，而是"不改变语义"。为了不因此给护栏开口子，白名单项必须满足
//  **更强的条件**：该文件里 `capabilities()` 的每一次出现都必须是 `return inner_...` 形态。
//  这样"把装饰器加进白名单"就等价于"声明它是一个纯转发器"，而不是"这里可以随便分支"。
const std::vector<std::string>& PassThroughCallSites() {
  static const std::vector<std::string> allowed = {
      "infra/blob/metered/metered_blob_store.h",
  };
  return allowed;
}

bool IsPassThrough(const std::string& rel) {
  const auto& allowed = PassThroughCallSites();
  return std::find(allowed.begin(), allowed.end(), rel) != allowed.end();
}

//  取源文件第 `line` 行（1 基；越界返回空串）
std::string LineOf(const std::string& text, int line) {
  std::istringstream is(text);
  std::string current;
  for (int i = 1; std::getline(is, current); ++i) {
    if (i == line) return current;
  }
  return {};
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

struct Hit {
  std::string file;
  int line = 0;
  std::string token;
};

//  去掉注释（保留换行，保证行号不变；字符串字面量内的 `//` 不当注释）。
//  ⚠️ 为什么要去注释：头文件的**设计说明**里就会写 `store->capabilities()`，
//     不过滤会把文档当成违规（第一次就误报了 location_issuer.h）。原始字符串字面量
//     未特殊处理 —— 如实记录为已知边界。
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
      // 字符串字面量的内容也要屏蔽：`"https://h/->capabilities("` 不是调用
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

//  扫描逻辑独立成函数 → 可以对**合成文本**自证，不依赖真实源码树
std::vector<Hit> ScanText(const std::string& rel, const std::string& raw_text) {
  static const std::vector<std::string> tokens = {".capabilities(", "->capabilities("};
  const std::string text = StripComments(raw_text);
  std::vector<Hit> hits;
  std::istringstream is(text);
  std::string line;
  int lineno = 0;
  while (std::getline(is, line)) {
    ++lineno;
    for (const auto& token : tokens) {
      if (line.find(token) != std::string::npos) {
        hits.push_back(Hit{rel, lineno, token});
      }
    }
  }
  return hits;
}

}  // namespace

TEST_CASE("护栏扫描器本身有效（合成的越权调用必须被检出）", "[phase2][guard][c2.7]") {
  //  必须检出的形态
  REQUIRE_FALSE(ScanText("app/usecases/x.cpp", "auto caps = store->capabilities();").empty());
  REQUIRE_FALSE(ScanText("app/usecases/x.cpp", "auto caps = store.capabilities();").empty());
  REQUIRE_FALSE(ScanText("infra/blob/posix/posix_blob_store.cpp",
                         "if (inner->capabilities().range_read) {}")
                  .empty());
  //  不得误判的形态：声明/定义行没有点或箭头
  REQUIRE(ScanText("domain/ports/ports.h", "virtual BlobCapabilities capabilities() const = 0;")
              .empty());
  REQUIRE(ScanText("app/services/location_issuer.cpp",
                   "domain::BlobCapabilities capabilities() const override { return caps_; }")
              .empty());
  //  ★ 注释与字符串里的写法不算"调用"（否则头文件的设计说明就会被当成违规）
  REQUIRE(ScanText("app/services/location_issuer.h",
                   "//   caps = store->capabilities()   // 设计说明，不是调用")
              .empty());
  REQUIRE(ScanText("app/services/location_issuer.h",
                   "/* 分支由 caps = store->capabilities() 决定 */\nint x = 0;")
              .empty());
  REQUIRE(ScanText("app/x.cpp", "const char* url = \"https://h/->capabilities(\";").empty());
  //  白名单文件里的调用能被检出（用于下面的非空洞性断言）
  REQUIRE(ScanText("app/services/location_issuer.cpp", "const auto caps = store->capabilities();")
              .size() == 1);
}

TEST_CASE("★ C2.7 capabilities() 的调用点只在白名单里（含非空洞性断言）",
          "[phase2][guard][c2.7]") {
  const fs::path src_root = fs::path(FSS_REPO_ROOT) / "src";
  const auto files = CollectSources(src_root);
  REQUIRE_FALSE(files.empty());

  std::vector<Hit> violations;
  bool allowed_file_actually_calls = false;
  bool pass_through_actually_forwards = false;
  for (const auto& file : files) {
    const std::string rel = fs::relative(file, src_root).generic_string();
    const std::string content = ReadFile(file);
    const auto hits = ScanText(rel, content);
    if (hits.empty()) continue;
    if (IsAllowed(rel)) {
      allowed_file_actually_calls = true;
      continue;
    }
    if (IsPassThrough(rel)) {
      for (const auto& hit : hits) {
        const std::string text = LineOf(content, hit.line);
        INFO("装饰器白名单项必须原样转发：src/" << rel << ":" << hit.line << "  " << text);
        REQUIRE(text.find("return inner_") != std::string::npos);
        REQUIRE(text.find(".capabilities()") != std::string::npos);
        pass_through_actually_forwards = true;
      }
      continue;
    }
    for (const auto& hit : hits) violations.push_back(hit);
  }

  //  ★ 非空洞性：如果白名单文件里根本没有调用，这条护栏的"通过"是假象
  //    （与 C2.1 的"静态库 link.txt 是空证据"同类陷阱）
  REQUIRE(allowed_file_actually_calls);
  //  非空洞性（同一纪律）：装饰器白名单里也必须真的有转发调用，否则多出来的是一条死规则
  REQUIRE(pass_through_actually_forwards);

  std::ostringstream report;
  for (const auto& v : violations) {
    report << "\n  src/" << v.file << ":" << v.line << "  出现 '" << v.token << "'";
  }
  INFO("越权调用 capabilities()：" << report.str());
  REQUIRE(violations.empty());
}

TEST_CASE("★ C2.6 源码级：LocationIssuer 里没有驱动类型分支",
          "[phase2][guard][c2.6]") {
  const fs::path file = fs::path(FSS_REPO_ROOT) / "src/app/services/location_issuer.cpp";
  REQUIRE(fs::exists(file));
  const std::string text = ReadFile(file);

  //  "按能力编程"的可执行判据：不得按驱动**枚举**取值分支
  REQUIRE(text.find("StorageDriver::kPosix") == std::string::npos);
  REQUIRE(text.find("StorageDriver::kS3") == std::string::npos);
  //  能力查询必须真的存在，且驱动名取自能力声明
  REQUIRE(text.find("->capabilities()") != std::string::npos);
  REQUIRE(text.find("driver_name") != std::string::npos);
}
