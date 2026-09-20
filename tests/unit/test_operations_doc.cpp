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
#include <cstddef>
#include <fstream>
#include <functional>
#include <regex>
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
  //    下限取得比真实值（本例 157）低，但只要有人把示例文件删到只剩骨架，
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

// =============================================================================
//  C10.11（阶段 10 切片 2）：operations.md 的"接通状态"必须是**三态**且计数自洽
// =============================================================================
//  判据原文："对每个键，operations.md 必须标注三态之一 —— `生效` /
//            `拒绝启动（列出触发条件）` / `已读但无效果（必须给出理由与下一步）`"。
//  这条测试把"计数"变成机械断言（否则文档里的数字只是人写的字，会漂移）：
//    ① 示例文件的每个叶子键都必须在 §1.2 的某一行里以状态标记开头；
//    ② 三态计数 = 生效 141 / 拒绝启动 14 / 已读但无效果 2，且相加 = 157；
//    ③ 文档正文里声明的数字也必须一致（防止只改表格不改正文）。
//  ★ 只认"最后一列以状态标记开头"的行：同一个键在别处（如 §5.1 的档位表）出现不算。
// =============================================================================
TEST_CASE("★ C10.11 operations.md 三态计数自洽（生效 141 / 拒绝启动 14 / 已读但无效果 2）",
          "[phase10][docs][c10.11]") {
  const std::string example_path = std::string(FSS_REPO_ROOT) + "/config/fss.example.json";
  const std::string doc_path = std::string(FSS_REPO_ROOT) + "/docs/operations.md";
  std::string example_text;
  std::string doc;
  REQUIRE(ReadFile(example_path, example_text));
  REQUIRE(ReadFile(doc_path, doc));
  const auto parsed = fss::json::ParseWithComments(example_text);
  REQUIRE(parsed.ok());
  const std::vector<std::string> leaves = LeafPaths(parsed.value());

  const std::string kEffective = "**生效**";
  const std::string kReject = "**拒绝启动（触发条件）**";
  const std::string kIneffective = "**已读但无效果**";

  std::size_t n_effective = 0;
  std::size_t n_reject = 0;
  std::size_t n_ineffective = 0;
  std::vector<std::string> unmarked;
  for (const auto& leaf : leaves) {
    const std::string row_prefix = "| `" + leaf + "` |";
    bool found = false;
    std::istringstream stream(doc);
    std::string line;
    while (std::getline(stream, line)) {
      if (line.rfind(row_prefix, 0) != 0) continue;
      const auto last_bar = line.rfind('|');
      if (last_bar == std::string::npos || last_bar == 0) continue;
      const auto prev_bar = line.rfind('|', last_bar - 1);
      if (prev_bar == std::string::npos) continue;
      std::string cell = line.substr(prev_bar + 1, last_bar - prev_bar - 1);
      const auto first = cell.find_first_not_of(" \t");
      cell = (first == std::string::npos) ? std::string() : cell.substr(first);
      if (cell.rfind(kEffective, 0) == 0) {
        ++n_effective;
        found = true;
        break;
      }
      if (cell.rfind(kReject, 0) == 0) {
        ++n_reject;
        found = true;
        break;
      }
      if (cell.rfind(kIneffective, 0) == 0) {
        ++n_ineffective;
        found = true;
        break;
      }
    }
    if (!found) unmarked.push_back(leaf);
  }

  INFO("未标注三态的键（共 " << unmarked.size() << "）:");
  for (const auto& key : unmarked) INFO("  未标注: " << key);
  REQUIRE(unmarked.empty());
  INFO("生效=" << n_effective << " 拒绝启动=" << n_reject << " 已读但无效果=" << n_ineffective
               << " 合计=" << leaves.size());
  REQUIRE(n_effective == 141);
  REQUIRE(n_reject == 14);
  REQUIRE(n_ineffective == 2);
  REQUIRE(n_effective + n_reject + n_ineffective == leaves.size());
  //  正文声明的数字也必须一致（防止"只改表格、不改正文"）
  REQUIRE(doc.find("生效 141 / 拒绝启动 14 / 已读但无效果 2") != std::string::npos);
  REQUIRE(doc.find("**141 + 14 + 2 = 157**") != std::string::npos);
}

// =============================================================================
//  E1b：三态计数的**跨文档一致性**护栏（本切片新增；"陈旧数字复活"防线）
// =============================================================================
//  为什么新增：`C10.11` 只钉住 `docs/operations.md` **自己**；而"生效 N / 拒绝启动 M /
//  已读但无效果 K"这组数字在 `04-implementation-plan.md` / `02-design.md` / `runbook.md`
//  / `AGENTS.md` 里也各写了一份。本轮把 `storage.posix.one_filesystem_per_partition`
//  从「已读但无效果」移入「生效」时，正好暴露了"只改 operations.md、别处留旧值"这个
//  漂移类（`docs/04` 的"陈旧数字复活"）。护栏做法：
//    ① 从 `docs/operations.md` 取**权威三元组**（`std::regex` 的第一次匹配，默认 flags
//       —— ECMAScript 的 `\s` 覆盖换行，因此 `runbook.md` 里跨行书写的三元组也能匹配）；
//       要求恰为 141/14/2、N+M+K == 157，并要求正文含字面 `**141 + 14 + 2 = 157**`；
//    ② 扫描**每一个**受管文件的**每一处**该正则，任何一处三元组 != 权威值 → 失败，
//       并报出 `文件:行`（含多处时的全部位置）。
//
//  ★ 为什么**刻意排除** `docs/phase-status.md` 与 `docs/test-evidence/*.md`：它们是
//    **只追加**的历史日志，按设计保留被取代的中间值（例如 B1 时的 `122/16/18`、
//    B2b 时的 `129/15/13`）。把它们纳入扫描会让"如实保留历史登记"变成永远无法通过的
//    检查 —— 这不是漏检，而是刻意的口径（排除清单在本用例里是显式的，不会随目录枚举漂移）。
//
//  ★ **已知局限（如实登记）**：本护栏只覆盖"**写出来的**三元组是否一致"，**不**覆盖
//    "某个受管文件不再提这些数字"。若某个文件把整句删掉，本用例**不会**失败
//    （`C10.11` 也只检查 operations.md 自己）。要覆盖"缺失"必须维护一份"必须出现该
//    三元组的文件清单"，那会把历史文档也钉死，本切片不做。
//
//  ★ R1 自证（见 `docs/test-evidence/phase10.md` §24 的注入表"护栏注入"）：把
//    `docs/02-design.md` 的 `141` 改成 `140` → 本用例失败并指出 `docs/02-design.md:<行>`；
//    改回后恢复绿色。本用例开头还有对正则抽取器本身的**小自证**（能匹配、能跨行匹配、
//    且对不含三态词的旧式 `129/15/13` 不匹配）。
// =============================================================================
TEST_CASE("★ E1b 三态计数跨文档一致（权威值取自 operations.md；受管文件不得留旧值）",
          "[e1b][docs]") {
  const std::string root = std::string(FSS_REPO_ROOT);
  const std::regex pattern(
      "生效\\s+(\\d+)\\s*/\\s*拒绝启动\\s+(\\d+)\\s*/\\s*已读但无效果\\s+(\\d+)");

  const auto extract = [&](const std::string& text, std::size_t offset,
                           int* n, int* m, int* k) -> bool {
    std::smatch match;
    const std::string tail = text.substr(offset);
    if (!std::regex_search(tail, match, pattern)) return false;
    *n = std::stoi(match[1].str());
    *m = std::stoi(match[2].str());
    *k = std::stoi(match[3].str());
    return true;
  };

  //  ---- 抽取器小自证（R1：判据本身要能失败）----
  {
    int n = 0, m = 0, k = 0;
    REQUIRE(extract("……（生效 141 / 拒绝启动 14 / 已读但无效果 2）……", 0, &n, &m, &k));
    REQUIRE((n == 141 && m == 14 && k == 2));
    //  跨行（runbook.md 的真实形态）：`\s` 必须覆盖换行
    n = m = k = 0;
    REQUIRE(extract("清单（生效 141 / 拒绝启动 14 /\n已读但无效果 2；metadata.remote 后）", 0, &n, &m, &k));
    REQUIRE((n == 141 && m == 14 && k == 2));
    //  反面对照：历史净变化条目里的 `130/15/12` **不带三态词**，不得被当成一处声明
    //  （否则 operations.md 的历史日志会自己把自己判失败）。
    n = m = k = 0;
    REQUIRE_FALSE(extract("三态从 **137/14/6** 变为 **141/14/2**", 0, &n, &m, &k));
  }

  //  ---- ① 权威值：docs/operations.md 的第一次匹配 ----
  const std::string operations_path = root + "/docs/operations.md";
  std::string operations_text;
  REQUIRE(ReadFile(operations_path, operations_text));
  int auth_n = 0, auth_m = 0, auth_k = 0;
  INFO("权威文件: " << operations_path);
  REQUIRE(extract(operations_text, 0, &auth_n, &auth_m, &auth_k));
  INFO("权威三元组 = " << auth_n << "/" << auth_m << "/" << auth_k);
  REQUIRE(auth_n == 141);
  REQUIRE(auth_m == 14);
  REQUIRE(auth_k == 2);
  REQUIRE(auth_n + auth_m + auth_k == 157);
  REQUIRE(operations_text.find("**141 + 14 + 2 = 157**") != std::string::npos);

  //  ---- ② 受管文件逐处比对 ----
  //  ★ `AGENTS.md` 由父代理维护（本切片改它之外的文件）；这里只**读它**做一致性比对。
  const std::vector<std::string> managed = {
      operations_path,
      root + "/docs/04-implementation-plan.md",
      root + "/docs/02-design.md",
      root + "/docs/runbook.md",
      root + "/AGENTS.md",
  };
  const auto line_of = [](const std::string& text, std::size_t position) -> std::size_t {
    return 1 + static_cast<std::size_t>(
                   std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(position),
                              '\n'));
  };

  std::vector<std::string> mismatches;
  std::size_t seen = 0;
  for (const auto& path : managed) {
    std::string text;
    INFO("受管文件: " << path);
    REQUIRE(ReadFile(path, text));
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
      const auto& match = *it;
      const int n = std::stoi(match[1].str());
      const int m = std::stoi(match[2].str());
      const int k = std::stoi(match[3].str());
      ++seen;
      if (n == auth_n && m == auth_m && k == auth_k) continue;
      const std::string rel = path.substr(root.size() + 1);
      mismatches.push_back(rel + ":" + std::to_string(line_of(text, match.position())) + " → " +
                           std::to_string(n) + "/" + std::to_string(m) + "/" +
                           std::to_string(k) + "（权威 " + std::to_string(auth_n) + "/" +
                           std::to_string(auth_m) + "/" + std::to_string(auth_k) + "）");
    }
  }
  INFO("受管文件里共发现 " << seen << " 处三态三元组；不一致 " << mismatches.size() << " 处");
  {
    //  ★ 把不一致处拼成**一个** INFO（循环内的 INFO 到 REQUIRE 处已出作用域，打印不出来）。
    std::string report;
    for (const auto& mismatch : mismatches) report += "  旧值/错值: " + mismatch + "\n";
    INFO("不一致明细（文件:行 → 实际值）：\n" << report);
    //  ★ 非空洞（R16）：受管文件至少要有几处声明，否则"全部一致"可能在空集合上成立。
    REQUIRE(seen >= 5);
    REQUIRE(mismatches.empty());
  }
}
