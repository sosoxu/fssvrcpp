// C1.5：common/config —— 分层优先级、${ENV:VAR}、**一次性全量校验**、密钥打码
#include <catch2/catch.hpp>

#include "common/config/config.h"
#include "common/json/json.h"
#include "framework/temp_dir.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <string>

using fss::config::FieldSpec;
using fss::config::LoadRequest;
using fss::config::Problem;
using fss::config::ValueType;

namespace {

// 测试用小型 schema（不依赖 CoreSchema，便于把断言集中在机制上）
fss::config::Schema TestSchema() {
  fss::config::Schema s;
  s.Add(FieldSpec{"a.port"}.Int(1, 65535, "监听端口").Default("8080"));
  s.Add(FieldSpec{"a.tcp_nodelay"}.Bool().Default("true"));
  s.Add(FieldSpec{"a.mode"}.Str().Enum({"x", "y"}).Default("x"));
  s.Add(FieldSpec{"a.secret"}.Str().Secret().Default(""));
  s.Add(FieldSpec{"a.required_thing"}.Str().Required());
  s.Add(FieldSpec{"a.tags"}.List().Default("p,q"));
  return s;
}

std::string WriteFile(const std::string& dir, const std::string& name, const std::string& content) {
  const std::string p = dir + "/" + name;
  std::ofstream(p) << content;
  return p;
}

LoadRequest BaseReq(const std::string& path) {
  LoadRequest r;
  r.schema = TestSchema();
  r.file_path = path;
  r.env_lookup = [](std::string_view) { return std::optional<std::string>{}; };
  return r;
}

bool HasProblem(const std::vector<Problem>& ps, const std::string& path, const std::string& substr) {
  for (const auto& p : ps) {
    if (p.path == path && p.message.find(substr) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("★ 一次性列出**全部**问题（不是只报第一个）", "[phase1][config][c1.5]") {
  fss::test::TempDir tmp("cfg_all");
  // 故意同时犯 5 个错：缺必填、类型错、超范围、枚举非法、未知键
  const std::string file = WriteFile(tmp.str(), "bad.json", R"JSON({
    "a": {
      "port": "not-a-number",
      "tcp_nodelay": true,
      "mode": "z",
      "typo_key": 1
    }
  })JSON");

  auto r = fss::config::Load(BaseReq(file));
  REQUIRE_FALSE(r.ok);

  const auto& ps = r.config.problems();
  // ★ 核心断言：5 类问题必须**同时**出现
  REQUIRE(HasProblem(ps, "a.port", "期望整数"));
  REQUIRE(HasProblem(ps, "a.mode", "取值非法"));
  REQUIRE(HasProblem(ps, "a.typo_key", "未知配置项"));
  REQUIRE(HasProblem(ps, "a.required_thing", "必填项缺失"));
  REQUIRE(ps.size() >= 4);
  INFO(r.config.ProblemsToString());
  // 汇总信息要能一次看全
  REQUIRE(r.config.ProblemsToString().find("共 4 个问题") != std::string::npos);
}

TEST_CASE("未知配置项被拒（拼写错误必须暴露，而不是静默忽略）", "[phase1][config]") {
  fss::test::TempDir tmp("cfg_unknown");
  const std::string file = WriteFile(tmp.str(), "u.json",
      R"({"a":{"port":8080,"required_thing":"v","prot":9}})");  // port 拼错成 prot
  auto r = fss::config::Load(BaseReq(file));
  REQUIRE_FALSE(r.ok);
  REQUIRE(HasProblem(r.config.problems(), "a.prot", "未知配置项"));

  // 动态子树内的键是允许的
  fss::config::Schema s = TestSchema();
  s.AllowDynamicPrefix("a.dynamic");
  LoadRequest req = BaseReq(WriteFile(tmp.str(), "d.json",
      R"({"a":{"required_thing":"v","dynamic":{"anything":{"deep":1}}}})"));
  req.schema = s;
  auto r2 = fss::config::Load(req);
  REQUIRE(r2.ok);
}

TEST_CASE("优先级：命令行 > 环境变量 > 文件 > 默认值", "[phase1][config]") {
  fss::test::TempDir tmp("cfg_layers");
  const std::string file = WriteFile(tmp.str(), "l.json",
      R"({"a":{"port":1234,"required_thing":"v"}})");

  // ① 文件覆盖默认值
  auto r1 = fss::config::Load(BaseReq(file));
  REQUIRE(r1.ok);
  REQUIRE(r1.config.GetInt("a.port").value() == 1234);
  REQUIRE(r1.config.SourceOf("a.port") == "file");

  // ② 环境变量覆盖文件
  LoadRequest req2 = BaseReq(file);
  req2.env_lookup = [](std::string_view k) -> std::optional<std::string> {
    if (k == "FSS_A_PORT") return std::string("2222");
    return std::nullopt;
  };
  auto r2 = fss::config::Load(req2);
  REQUIRE(r2.ok);
  REQUIRE(r2.config.GetInt("a.port").value() == 2222);
  REQUIRE(r2.config.SourceOf("a.port") == "env");

  // ③ 命令行覆盖环境变量
  LoadRequest req3 = req2;
  req3.cli_overrides = {{"a.port", "3333"}};
  auto r3 = fss::config::Load(req3);
  REQUIRE(r3.ok);
  REQUIRE(r3.config.GetInt("a.port").value() == 3333);
  REQUIRE(r3.config.SourceOf("a.port") == "cli");

  // ④ 都没给 → 默认值
  auto r4 = fss::config::Load(BaseReq(WriteFile(tmp.str(), "e.json",
      R"({"a":{"required_thing":"v"}})")));
  REQUIRE(r4.ok);
  REQUIRE(r4.config.GetInt("a.port").value() == 8080);
  REQUIRE(r4.config.SourceOf("a.port") == "default");
}

TEST_CASE("命令行覆盖未知项也要报错", "[phase1][config]") {
  fss::test::TempDir tmp("cfg_cli");
  LoadRequest req = BaseReq(WriteFile(tmp.str(), "c.json", R"({"a":{"required_thing":"v"}})"));
  req.cli_overrides = {{"a.nonexistent", "1"}};
  auto r = fss::config::Load(req);
  REQUIRE_FALSE(r.ok);
  REQUIRE(HasProblem(r.config.problems(), "a.nonexistent", "未知配置项"));
}

TEST_CASE("类型校验：布尔接受常见写法，但拒绝任意字符串", "[phase1][config]") {
  fss::test::TempDir tmp("cfg_bool");
  for (const char* ok : {"true", "false", "1", "0", "yes", "no", "on", "off"}) {
    auto r = fss::config::Load(BaseReq(WriteFile(tmp.str(), "b.json",
        std::string(R"({"a":{"required_thing":"v","tcp_nodelay":")") + ok + R"("}})")));
    INFO("应接受: " << ok);
    REQUIRE(r.ok);
  }
  auto bad = fss::config::Load(BaseReq(WriteFile(tmp.str(), "b2.json",
      R"({"a":{"required_thing":"v","tcp_nodelay":"maybe"}})")));
  REQUIRE_FALSE(bad.ok);
  REQUIRE(HasProblem(bad.config.problems(), "a.tcp_nodelay", "期望布尔值"));
}

TEST_CASE("整数范围校验", "[phase1][config]") {
  fss::test::TempDir tmp("cfg_range");
  auto lo = fss::config::Load(BaseReq(WriteFile(tmp.str(), "lo.json",
      R"({"a":{"required_thing":"v","port":0}})")));
  REQUIRE_FALSE(lo.ok);
  REQUIRE(HasProblem(lo.config.problems(), "a.port", "超出范围"));

  auto hi = fss::config::Load(BaseReq(WriteFile(tmp.str(), "hi.json",
      R"({"a":{"required_thing":"v","port":70000}})")));
  REQUIRE_FALSE(hi.ok);
  REQUIRE(HasProblem(hi.config.problems(), "a.port", "超出范围"));
}

TEST_CASE("${ENV:VAR} 展开：能解析则替换，不能解析则报错（有默认值则回退）", "[phase1][config]") {
  fss::test::TempDir tmp("cfg_env");

  // 能解析
  LoadRequest req = BaseReq(WriteFile(tmp.str(), "s.json",
      R"({"a":{"required_thing":"$ENV{}","secret":"${ENV:MY_SECRET}"}})"));  // 占位，下面重写
  req.file_path = WriteFile(tmp.str(), "s2.json",
      R"({"a":{"required_thing":"v","secret":"${ENV:MY_SECRET}"}})");
  req.env_lookup = [](std::string_view k) -> std::optional<std::string> {
    if (k == "MY_SECRET") return std::string("s3cr3t");
    return std::nullopt;
  };
  auto r = fss::config::Load(req);
  REQUIRE(r.ok);
  REQUIRE(r.config.GetString("a.secret").value() == "s3cr3t");

  // 不能解析 → 报错
  LoadRequest req2 = BaseReq(WriteFile(tmp.str(), "s3.json",
      R"({"a":{"required_thing":"v","secret":"${ENV:ABSENT_VAR}"}})"));
  auto r2 = fss::config::Load(req2);
  REQUIRE_FALSE(r2.ok);
  REQUIRE(HasProblem(r2.config.problems(), "a.secret", "ABSENT_VAR 未设置"));
}

TEST_CASE("配置解析错误要带位置（复用 json 的定位能力）", "[phase1][config]") {
  fss::test::TempDir tmp("cfg_parse");
  auto r = fss::config::Load(BaseReq(WriteFile(tmp.str(), "bad.json", "{\n  \"a\": 1\n  \"b\": 2\n}")));
  REQUIRE_FALSE(r.ok);
  REQUIRE(HasProblem(r.config.problems(), "<file>", "配置解析失败"));
  // 位置信息必须传上来
  INFO(r.config.ProblemsToString());
  REQUIRE(r.config.ProblemsToString().find("line") != std::string::npos);
}

TEST_CASE("★ secret 字段在诊断输出中必须打码", "[phase1][config][c1.6]") {
  fss::test::TempDir tmp("cfg_secret");
  LoadRequest req = BaseReq(WriteFile(tmp.str(), "sec.json",
      R"({"a":{"required_thing":"v","secret":"TOPSECRET-VALUE"}})"));
  auto r = fss::config::Load(req);
  REQUIRE(r.ok);

  const std::string dump = r.config.RedactedDump();
  REQUIRE(dump.find("TOPSECRET-VALUE") == std::string::npos);  // ★ 明文绝不能出现
  REQUIRE(dump.find("***") != std::string::npos);
  // 非 secret 字段仍可见（否则排障没法用）
  REQUIRE(dump.find("\"required_thing\"") != std::string::npos);
  const bool has_port = dump.find("a.port") != std::string::npos || dump.find("\"port\"") != std::string::npos;
  REQUIRE(has_port);
}

TEST_CASE("数组型字段：JSON 数组与逗号分隔的覆盖都能用", "[phase1][config]") {
  fss::test::TempDir tmp("cfg_list");
  // 默认值（逗号分隔的 default）
  auto r0 = fss::config::Load(BaseReq(WriteFile(tmp.str(), "d.json", R"({"a":{"required_thing":"v"}})")));
  REQUIRE(r0.ok);
  REQUIRE(r0.config.GetStringList("a.tags").value() == std::vector<std::string>{"p", "q"});

  // JSON 数组
  auto r1 = fss::config::Load(BaseReq(WriteFile(tmp.str(), "l.json",
      R"({"a":{"required_thing":"v","tags":["x","y","z"]}})")));
  REQUIRE(r1.ok);
  REQUIRE(r1.config.GetStringList("a.tags").value() == std::vector<std::string>{"x", "y", "z"});

  // 环境变量覆盖（逗号分隔）
  LoadRequest req = BaseReq(WriteFile(tmp.str(), "l2.json", R"({"a":{"required_thing":"v"}})"));
  req.env_lookup = [](std::string_view k) -> std::optional<std::string> {
    if (k == "FSS_A_TAGS") return std::string("m,n");
    return std::nullopt;
  };
  auto r2 = fss::config::Load(req);
  REQUIRE(r2.ok);
  REQUIRE(r2.config.GetStringList("a.tags").value() == std::vector<std::string>{"m", "n"});
}

TEST_CASE("跨字段规则：multi 模式的强制校验（ADR-009 §8.1）", "[phase1][config][contract]") {
  fss::test::TempDir tmp("cfg_cross");
  fss::config::Schema s = fss::config::CoreSchema();

  // single（默认）→ 不触发
  LoadRequest ok_req;
  ok_req.schema = s;
  ok_req.file_path = WriteFile(tmp.str(), "single.json", R"({})");
  auto r1 = fss::config::Load(ok_req);
  REQUIRE(r1.ok);

  // multi 但仓储仍是 sqlite、租约与选举未开 → 一次列出多条
  LoadRequest bad_req;
  bad_req.schema = s;
  bad_req.file_path = WriteFile(tmp.str(), "multi.json", R"({"deployment":{"mode":"multi"}})");
  auto r2 = fss::config::Load(bad_req);
  REQUIRE_FALSE(r2.ok);
  const auto& ps = r2.config.problems();
  REQUIRE(HasProblem(ps, "metadata.repository", "必须是 postgres"));
  REQUIRE(HasProblem(ps, "location.repository", "必须是 postgres"));
  REQUIRE(HasProblem(ps, "leases.enabled", "必须开启"));
  REQUIRE(HasProblem(ps, "leader_election.enabled", "必须开启"));

  // 补齐后通过
  LoadRequest good_req;
  good_req.schema = s;
  good_req.file_path = WriteFile(tmp.str(), "multi_ok.json", R"({
    "deployment": {"mode": "multi"},
    "metadata": {"repository": "postgres"},
    "location": {"repository": "postgres"},
    "leases": {"enabled": true},
    "leader_election": {"enabled": true},
    "storage": {"posix": {"shared_mount_required": true}}
  })");
  auto r3 = fss::config::Load(good_req);
  INFO(r3.config.ProblemsToString());
  REQUIRE(r3.ok);
}

TEST_CASE("★ C8.5 认证配置的强制校验：production 不得 disabled / 不得关验签 / 必须有密钥",
          "[phase8][config][c8.5]") {
  fss::test::TempDir tmp("cfg_auth");
  LoadRequest req;
  req.schema = fss::config::CoreSchema();

  //  ① production + disabled → 拒绝（这是"忘了开鉴权"的典型形态）
  req.file_path = WriteFile(tmp.str(), "prod_disabled.json", R"({
    "deployment": {"environment": "production"},
    "auth": {"mode": "disabled", "jwt": {"hmac_secret": "s"}}
  })");
  auto r1 = fss::config::Load(req);
  REQUIRE_FALSE(r1.ok);
  REQUIRE(HasProblem(r1.config.problems(), "auth.mode", "不允许 disabled"));

  //  ② production + 关闭验签 → 拒绝
  req.file_path = WriteFile(tmp.str(), "prod_noverify.json", R"({
    "deployment": {"environment": "production"},
    "auth": {"mode": "jwt", "jwt": {"verify_signature": false, "hmac_secret": "s"}}
  })");
  auto r2 = fss::config::Load(req);
  REQUIRE_FALSE(r2.ok);
  REQUIRE(HasProblem(r2.config.problems(), "auth.jwt.verify_signature", "必须为 true"));

  //  ③ **production** + jwt 模式 + 开启验签 + 没有密钥 → 拒绝
  //     （开发环境允许：语义是"本实例拒绝所有 token"+ 启动告警，见 ADR-012 §5.2）
  req.file_path = WriteFile(tmp.str(), "no_secret.json", R"({
    "deployment": {"environment": "production"},
    "auth": {"mode": "jwt", "jwt": {"verify_signature": true}}
  })");
  auto r3 = fss::config::Load(req);
  REQUIRE_FALSE(r3.ok);
  REQUIRE(HasProblem(r3.config.problems(), "auth.jwt.hmac_secret", "必须配置共享密钥"));

  //  ④ jwks_url 非空 → 拒绝（RS256/JWKS 未实现，ADR-012 §5.3；不能让配置"看起来有鉴权"）
  req.file_path = WriteFile(tmp.str(), "jwks.json", R"({
    "auth": {"mode": "jwt", "jwt": {"hmac_secret": "s", "jwks_url": "https://idp/jwks"}}
  })");
  auto r4 = fss::config::Load(req);
  REQUIRE_FALSE(r4.ok);
  REQUIRE(HasProblem(r4.config.problems(), "auth.jwt.jwks_url", "尚未实现"));

  //  ⑤ remote-entitlements：缺地址 / fail_closed=false → 拒绝；
  //     补齐后必须通过（★ 正例：区分"校验正确"与"校验恒真"）
  req.file_path = WriteFile(tmp.str(), "remote_no_url.json", R"({
    "auth": {"mode": "remote-entitlements",
             "remote_entitlements": {"base_url": "", "fail_closed": true}}
  })");
  auto r6 = fss::config::Load(req);
  REQUIRE_FALSE(r6.ok);
  REQUIRE(HasProblem(r6.config.problems(), "auth.remote_entitlements.base_url",
                     "必须配置 Entitlements 地址"));

  req.file_path = WriteFile(tmp.str(), "remote_open.json", R"({
    "auth": {"mode": "remote-entitlements",
             "remote_entitlements": {"base_url": "http://entitlements:8080",
                                     "fail_closed": false}}
  })");
  auto r7 = fss::config::Load(req);
  REQUIRE_FALSE(r7.ok);
  REQUIRE(HasProblem(r7.config.problems(), "auth.remote_entitlements.fail_closed",
                     "必须为 true"));

  req.file_path = WriteFile(tmp.str(), "remote_ok.json", R"({
    "auth": {"mode": "remote-entitlements",
             "remote_entitlements": {"base_url": "http://entitlements:8080",
                                     "authorize_path": "/api/entitlements/v2/authorizeAny",
                                     "fail_closed": true, "timeout_ms": 3000,
                                     "connect_timeout_ms": 1000}}
  })");
  auto r8 = fss::config::Load(req);
  INFO(r8.config.ProblemsToString());
  REQUIRE(r8.ok);

  //  ★ 开发环境 + 空密钥：**允许加载**（运行时拒绝所有 token + 启动告警）
  req.file_path = WriteFile(tmp.str(), "dev_no_secret.json", R"({
    "deployment": {"environment": "development"},
    "auth": {"mode": "jwt", "jwt": {"verify_signature": true}}
  })");
  auto dev = fss::config::Load(req);
  INFO(dev.config.ProblemsToString());
  REQUIRE(dev.ok);

  //  ★ 正例（R16）：补齐后的生产配置**必须通过** —— 否则无法区分"校验正确"与"校验恒真"
  req.file_path = WriteFile(tmp.str(), "prod_ok.json", R"({
    "deployment": {"environment": "production", "mode": "single"},
    "auth": {"mode": "jwt",
             "jwt": {"verify_signature": true, "hmac_secret": "a-long-shared-secret",
                     "partition_claim": "data-partition-id", "require_partition_claim": true}}
  })");
  auto r5 = fss::config::Load(req);
  INFO(r5.config.ProblemsToString());
  REQUIRE(r5.ok);
}

TEST_CASE("★ C8.9 multi 模式的 5 条强制启动校验：逐条拒绝 + 补齐后必须通过",
          "[phase8][config][c8.9]") {
  fss::test::TempDir tmp("cfg_multi_c89");
  LoadRequest req;
  req.schema = fss::config::CoreSchema();

  //  ① 仓储必须为 PG（缺一条 → 拒绝，并给出该字段）
  req.file_path = WriteFile(tmp.str(), "m1.json", R"({"deployment": {"mode": "multi"}})");
  auto r1 = fss::config::Load(req);
  REQUIRE_FALSE(r1.ok);
  REQUIRE(HasProblem(r1.config.problems(), "metadata.repository", "必须是 postgres"));
  REQUIRE(HasProblem(r1.config.problems(), "location.repository", "必须是 postgres"));
  REQUIRE(HasProblem(r1.config.problems(), "leases.enabled", "必须开启"));
  REQUIRE(HasProblem(r1.config.problems(), "leader_election.enabled", "必须开启"));
  REQUIRE(HasProblem(r1.config.problems(), "storage.posix.shared_mount_required", "必须为 true"));

  //  ② GC 必须要求租约到期
  req.file_path = WriteFile(tmp.str(), "m2.json", R"({
    "deployment": {"mode": "multi", "max_clock_skew_seconds": 5},
    "metadata": {"repository": "postgres"}, "location": {"repository": "postgres"},
    "leases": {"enabled": true}, "leader_election": {"enabled": true},
    "storage": {"posix": {"shared_mount_required": true}},
    "gc": {"require_lease_expiry": false}
  })");
  auto r2 = fss::config::Load(req);
  REQUIRE_FALSE(r2.ok);
  REQUIRE(HasProblem(r2.config.problems(), "gc.require_lease_expiry", "multi 时必须为 true"));

  //  ③ 时钟偏差容忍范围必须 0 < 值 <= 60
  //     （字段级范围是 0..300；>60 由跨字段规则拒绝，>300 连字段级都过不了）
  req.file_path = WriteFile(tmp.str(), "m3.json", R"({
    "deployment": {"mode": "multi", "max_clock_skew_seconds": 120},
    "metadata": {"repository": "postgres"}, "location": {"repository": "postgres"},
    "leases": {"enabled": true}, "leader_election": {"enabled": true},
    "storage": {"posix": {"shared_mount_required": true}},
    "gc": {"require_lease_expiry": true}
  })");
  auto r3 = fss::config::Load(req);
  REQUIRE_FALSE(r3.ok);
  REQUIRE(HasProblem(r3.config.problems(), "deployment.max_clock_skew_seconds", "<= 60"));

  //  ③b 超出字段范围（0..300）的值：字段级校验就拒绝（两道防线都要有）
  req.file_path = WriteFile(tmp.str(), "m3b.json", R"({
    "deployment": {"mode": "multi", "max_clock_skew_seconds": 3600},
    "metadata": {"repository": "postgres"}, "location": {"repository": "postgres"},
    "leases": {"enabled": true}, "leader_election": {"enabled": true},
    "storage": {"posix": {"shared_mount_required": true}},
    "gc": {"require_lease_expiry": true}
  })");
  auto r3b = fss::config::Load(req);
  REQUIRE_FALSE(r3b.ok);
  REQUIRE(HasProblem(r3b.config.problems(), "deployment.max_clock_skew_seconds", ""));

  //  ★ 正例（R16）：五条全部满足 → **必须通过**（否则无法区分"校验正确"与"校验恒真"）
  req.file_path = WriteFile(tmp.str(), "m_ok.json", R"({
    "deployment": {"mode": "multi", "max_clock_skew_seconds": 5},
    "metadata": {"repository": "postgres"}, "location": {"repository": "postgres"},
    "leases": {"enabled": true}, "leader_election": {"enabled": true},
    "storage": {"posix": {"shared_mount_required": true}},
    "gc": {"require_lease_expiry": true}
  })");
  auto ok = fss::config::Load(req);
  INFO(ok.config.ProblemsToString());
  REQUIRE(ok.ok);

  //  单实例：同样的 GC/时钟设置**不受** multi 约束（不误伤）
  req.file_path = WriteFile(tmp.str(), "single.json", R"({
    "deployment": {"mode": "single", "max_clock_skew_seconds": 5},
    "gc": {"require_lease_expiry": false}
  })");
  auto single = fss::config::Load(req);
  INFO(single.config.ProblemsToString());
  REQUIRE(single.ok);
}

TEST_CASE("跨字段规则：租约续租间隔必须小于 TTL、传输内存预算", "[phase1][config]") {
  fss::test::TempDir tmp("cfg_cross2");
  LoadRequest req;
  req.schema = fss::config::CoreSchema();
  req.file_path = WriteFile(tmp.str(), "lease.json", R"({
    "leases": {"ttl_seconds": 30, "renew_interval_seconds": 30},
    "server": {"http": {"worker_threads": 1024, "transfer_buffer_bytes": 1048576,
                        "transfer_memory_budget_bytes": 268435456}}
  })");
  auto r = fss::config::Load(req);
  REQUIRE_FALSE(r.ok);
  REQUIRE(HasProblem(r.config.problems(), "leases.renew_interval_seconds", "必须小于"));
  REQUIRE(HasProblem(r.config.problems(), "server.http.transfer_memory_budget_bytes", "超过预算"));
}

TEST_CASE("★ 示例配置文件的键集必须与 CoreSchema 一致（防止示例漂移）",
          "[phase1][config][contract]") {
  // 这条断言的作用：以后给 schema 加字段却忘了同步 config/fss.example.json（或反之）
  // 时，这里会失败。示例文件是运维真正会抄的东西，漂移的代价很高。
  const std::string example = std::string(FSS_REPO_ROOT) + "/config/fss.example.json";
  std::ifstream in(example);
  REQUIRE(in.good());
  std::stringstream ss; ss << in.rdbuf();
  auto parsed = fss::json::ParseWithComments(ss.str());
  INFO("示例文件: " << example);
  REQUIRE(parsed.ok());

  const auto schema = fss::config::CoreSchema();

  // 收集示例中的所有叶子路径
  std::vector<std::string> leaves;
  std::function<void(const fss::json::Value&, const std::string&)> walk =
      [&](const fss::json::Value& v, const std::string& prefix) {
        if (!v.is_object()) { if (!prefix.empty()) leaves.push_back(prefix); return; }
        if (v.empty() && !prefix.empty()) { leaves.push_back(prefix); return; }
        for (auto it = v.begin(); it != v.end(); ++it) {
          walk(it.value(), prefix.empty() ? it.key() : prefix + "." + it.key());
        }
      };
  walk(parsed.value(), "");

  std::vector<std::string> unknown_in_example;
  for (const auto& l : leaves) {
    if (!schema.IsAllowedPath(l)) unknown_in_example.push_back(l);
  }
  INFO("示例中有但 schema 未声明的键:");
  for (const auto& u : unknown_in_example) INFO("  " << u);
  REQUIRE(unknown_in_example.empty());

  // 反方向：schema 里声明了、示例里却没有的字段。
  // 这条同样重要：新增字段却不更新示例，运维就永远不知道该配什么，
  // 而"示例没写"通常被理解成"不用配"。
  const auto& fields = schema.fields();
  std::vector<std::string> missing_in_example;
  for (const auto& f : fields) {
    if (std::find(leaves.begin(), leaves.end(), f.path) == leaves.end())
      missing_in_example.push_back(f.path);
  }
  INFO("schema 已声明但示例中缺失的键:");
  for (const auto& m : missing_in_example) INFO("  " << m);
  REQUIRE(missing_in_example.empty());
}
