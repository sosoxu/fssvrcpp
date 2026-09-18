// =============================================================================
//  C9.9：运维文档完整性 —— docs/operations.md 必须覆盖 config/fss.example.json 的
//        **每一个叶子键**，且不得写出不存在的配置键。
// =============================================================================
//  判据原文："operations.md 覆盖全部配置项（与 config/fss.example.json 一一对应，
//             有自动比对测试）"。
//
//  三条断言合起来才构成"完整"，缺任何一条都可能是**恒真**的假通过：
//    ① 正向：示例文件里的每个叶子路径都以 `路径` 形态出现在 operations.md 里；
//    ② 非空洞（R16）：叶子数与文档长度必须超过下限 —— 否则"全部出现"可以在
//       空文档/占位符上成立；
//    ③ 反向：operations.md 里出现的配置路径必须能被 CoreSchema().IsAllowedPath()
//       命中 —— 否则运维会照着一份写着不存在键的手册去配。
//
//  失败诊断：一次性把**全部**缺失/多余的键收集起来 INFO 出来（不是只报第一个 false）。
//  示例文件是"带注释的 JSON"，必须用 json::ParseWithComments（R15）。
// =============================================================================
#include <catch2/catch.hpp>

#include "common/config/config.h"
#include "common/json/json.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool ReadFile(const std::string& path, std::string& out) {
  std::ifstream in(path);
  if (!in.good()) return false;
  std::stringstream buffer;
  buffer << in.rdbuf();
  out = buffer.str();
  return true;
}

// 示例文件里所有叶子路径（与 tests/unit/test_config.cpp 的 walk 同源）
std::vector<std::string> LeafPaths(const fss::json::Value& root) {
  std::vector<std::string> leaves;
  std::function<void(const fss::json::Value&, const std::string&)> walk =
      [&](const fss::json::Value& value, const std::string& prefix) {
        if (!value.is_object()) {
          if (!prefix.empty()) leaves.push_back(prefix);
          return;
        }
        if (value.empty() && !prefix.empty()) {
          leaves.push_back(prefix);
          return;
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
          walk(it.value(), prefix.empty() ? it.key() : prefix + "." + it.key());
        }
      };
  walk(root, "");
  return leaves;
}

// 反向断言只认"配置路径形态"的行内代码：
//   [a-z][a-z0-9_@-]* ( '.' [a-z0-9_@-]+ )+
// 且**首段必须是配置的顶层命名空间**。
// 这样 `docs/02-design.md`（含 `/`）、`BASELINE.tsv`（含大写）、
// `service.file.editors`（角色名，首段 `service` 不是命名空间）都不会被误判。
const std::set<std::string>& ConfigNamespaces() {
  static const std::set<std::string> kNamespaces = {
      "deployment", "server",       "storage",   "self_signed", "expiry", "http",
      "metadata",   "location",     "leases",    "leader_election", "auth", "legal",
      "schema",     "events",       "partition", "gc",          "observability"};
  return kNamespaces;
}

bool LooksLikeConfigPath(const std::string& token) {
  if (token.size() < 3) return false;
  std::vector<std::string> segments;
  std::string current;
  for (const char ch : token) {
    if (ch == '.') {
      segments.push_back(current);
      current.clear();
      continue;
    }
    const bool allowed = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                         ch == '@' || ch == '-';
    if (!allowed) return false;
    current.push_back(ch);
  }
  segments.push_back(current);
  if (segments.size() < 2) return false;
  for (const auto& segment : segments) {
    if (segment.empty()) return false;
  }
  if (!(segments.front()[0] >= 'a' && segments.front()[0] <= 'z')) return false;
  return ConfigNamespaces().count(segments.front()) == 1;
}

// 文档里所有行内代码（`...`）的内容
std::vector<std::string> BacktickedTokens(const std::string& text) {
  std::vector<std::string> tokens;
  std::size_t pos = 0;
  while (true) {
    const auto open = text.find('`', pos);
    if (open == std::string::npos) break;
    const auto close = text.find('`', open + 1);
    if (close == std::string::npos) break;
    tokens.push_back(text.substr(open + 1, close - open - 1));
    pos = close + 1;
  }
  return tokens;
}

}  // namespace

TEST_CASE("★ C9.9 operations.md 覆盖 example 的全部叶子键且不写不存在的键",
          "[phase9][docs][c9.9]") {
  const std::string example_path = std::string(FSS_REPO_ROOT) + "/config/fss.example.json";
  const std::string doc_path = std::string(FSS_REPO_ROOT) + "/docs/operations.md";

  std::string example_text;
  std::string doc;
  INFO("示例文件: " << example_path);
  INFO("运维文档: " << doc_path);
  REQUIRE(ReadFile(example_path, example_text));
  REQUIRE(ReadFile(doc_path, doc));

  // 示例文件是"带注释的 JSON"——必须走 ParseWithComments（R15）
  const auto parsed = fss::json::ParseWithComments(example_text);
  INFO("示例文件必须可解析（带注释的 JSON）");
  REQUIRE(parsed.ok());

  const std::vector<std::string> leaves = LeafPaths(parsed.value());

  // ---------------------------------------------------------------------------
  // ② 非空洞性（R16）：先钉住"被检查的集合本身足够大"。
  //    下限取得比真实值（本例 156）低，但只要有人把示例文件删到只剩骨架，
  //    或 operations.md 退化成占位符，这里就会失败。
  // ---------------------------------------------------------------------------
  INFO("example 叶子键数: " << leaves.size() << "；operations.md 字节数: " << doc.size());
  REQUIRE(leaves.size() >= 40);
  REQUIRE(doc.size() > 5000);

  // ---------------------------------------------------------------------------
  // ① 正向：每个叶子路径都必须以 `路径` 形态出现在文档里
  // ---------------------------------------------------------------------------
  std::vector<std::string> missing;
  for (const auto& leaf : leaves) {
    if (doc.find("`" + leaf + "`") == std::string::npos) missing.push_back(leaf);
  }
  INFO("operations.md 未覆盖的配置键（共 " << missing.size() << " / " << leaves.size() << "）:");
  for (const auto& key : missing) INFO("  缺文档: " << key);
  REQUIRE(missing.empty());

  // ---------------------------------------------------------------------------
  // ③ 反向：文档里出现的配置路径必须真实存在（example ∪ CoreSchema）
  // ---------------------------------------------------------------------------
  const auto schema = fss::config::CoreSchema();

  // 自证（R1/R16）：抽取器与 schema 判定都必须能失败，否则反向断言恒真
  REQUIRE(LooksLikeConfigPath("storage.posix.root"));
  REQUIRE_FALSE(LooksLikeConfigPath("docs/02-design.md"));   // 含 `/`
  REQUIRE_FALSE(LooksLikeConfigPath("BASELINE.tsv"));        // 含大写
  REQUIRE_FALSE(LooksLikeConfigPath("service.file.editors"));// 首段不是配置命名空间
  REQUIRE(schema.IsAllowedPath("storage.posix.root"));
  REQUIRE_FALSE(schema.IsAllowedPath("no.such.config.key"));

  // 示例里的每个叶子键也都必须被 schema 认可（示例 ⇄ schema 不漂移）
  std::vector<std::string> not_in_schema;
  for (const auto& leaf : leaves) {
    if (!schema.IsAllowedPath(leaf)) not_in_schema.push_back(leaf);
  }
  INFO("example 有但 schema 未声明的键（共 " << not_in_schema.size() << "）:");
  for (const auto& key : not_in_schema) INFO("  schema 未知: " << key);
  REQUIRE(not_in_schema.empty());

  std::vector<std::string> unknown;
  for (const auto& token : BacktickedTokens(doc)) {
    if (!LooksLikeConfigPath(token)) continue;
    if (!schema.IsAllowedPath(token)) unknown.push_back(token);
  }
  std::sort(unknown.begin(), unknown.end());
  unknown.erase(std::unique(unknown.begin(), unknown.end()), unknown.end());
  INFO("operations.md 写了但 example/schema 中不存在的配置路径（共 " << unknown.size() << "）:");
  for (const auto& key : unknown) INFO("  不存在的键: " << key);
  REQUIRE(unknown.empty());
}
