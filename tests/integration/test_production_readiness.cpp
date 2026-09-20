// =============================================================================
//  B2a（ADR-009 §5.3/§6.4/§8.1 + C9.28）：生产就绪检查 —— **真实 fss_server 进程**
// =============================================================================
//  判据（docs/04-implementation-plan.md 之外的切片目标；逐条对应本文件的用例）：
//    ① `metadata.postgres.schema_version_check`：
//       · `true`（默认）+ 已应用版本 ≠ 期望 → readiness **not ready**，原因可读；
//       · `true` + 正确迁移的库 → readiness **ready**（正控）；
//       · `false` + 版本不符 → readiness **ready**（证明"跳过版本比对"真的生效，且
//         存活探针仍在跑 —— 这里用"可达 PG"覆盖）。
//    ② C9.28 PG 连接预算：`deployment.expected_instances` 过大 → **exit 78**，
//       消息里同时出现 PG max_connections、每实例最坏连接数、乘积与修复方向；
//       正控：拟合法的配置真的启动且 readiness 200。
//    ③ 时钟偏移（ADR-009 §6.4/§8.1）：`FSS_CLOCK_SKEW_INJECT_MS` 超出
//       `deployment.clock_skew_tolerance_seconds` → **exit 78**（消息含本地值、
//       数据库值、偏差与容忍值）；注入在容忍内 → 启动；**不注入 → 启动**（正控：
//       证明结论确实由接缝翻转）。
//    ④ readiness 探针失败 + 恢复：`pg_terminate_backend` 杀掉**本实例**的后端
//       （按 `application_name` 精确定位，不误伤别的测试）→ readiness **not ready**；
//       随后轮询到 **ready**（池丢弃坏连接后重建）。
//
//  ★ 铁律（AGENTS R1 / §4.3）：
//    · 连不上 PG 就失败，绝不静默跳过（跳过的门槛是空证据）；
//    · 每个负断言都配**同路径正控**；
//    · 一切等待都**轮询实际条件** + 有界超时，不用固定 sleep 等状态；
//    · 版本不符的用例指向**独立的 scratch schema**（`pgtest_b2a_*`），结束即 DROP，
//      绝不改动共享库的 `schema_migrations`（残留 = 0）。
// =============================================================================
#include <catch2/catch.hpp>

#include "raw_http.h"
#include "server_process.h"
#include "temp_dir.h"

#include "infra/postgres/pg_connection.h"
#include "infra/postgres/pg_schema.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using fss::infra::PgConnection;
using fss::infra::PgOptions;
using fss::infra::PgResult;
using fss::test::ProcessOutcome;
using fss::test::RawClient;
using fss::test::RunServerForExit;
using fss::test::ServerProcess;
using fss::test::ServerProcessOptions;
using fss::test::TempDir;

//  本机 dev PG（`scripts/dev_postgres.sh`）的默认 DSN；目标库 12.6 用 FSS_PG_DSN 覆盖。
constexpr const char* kDefaultDsn = "postgresql://fss@127.0.0.1:15432/fss";
const std::string kReadinessPath = "/api/file/v2/readiness_check";

std::string TestDsn() {
  const char* env = std::getenv("FSS_PG_DSN");
  if (env != nullptr && *env != '\0') return std::string(env);
  return kDefaultDsn;
}

PgOptions TestPgOptions(int max_connections = 2) {
  PgOptions options;
  options.dsn = TestDsn();
  options.max_connections = max_connections;
  options.statement_timeout_millis = 5000;
  return options;
}

//  DSN 追加一个 query 参数（URI 形态：`?k=v` 或 `&k=v`）。**URL 编码**由调用方保证。
std::string DsnWithQuery(const std::string& dsn, const std::string& query) {
  return dsn + (dsn.find('?') == std::string::npos ? "?" : "&") + query;
}

int FreePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  int port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
      port = ntohs(addr.sin_port);
    }
  }
  ::close(fd);
  return port;
}

std::string WriteFile(const TempDir& dir, const std::string& name, const std::string& content) {
  const std::string path = dir.child(name);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.good());
  out << content;
  out.close();
  return path;
}

//  取 `marker` **紧邻前方**的十进制整数（找不到 → -1）。仅供测试断言使用。
//  这里用 "ms（正" 作为 marker 精确定位"偏差 = N ms（正 = 本地快）"里的 N。
long ParseIntBefore(const std::string& text, const std::string& marker) {
  const auto pos = text.find(marker);
  if (pos == std::string::npos) return -1;
  std::size_t i = pos;
  //  marker 自身可能以字母开头（如 "ms（正"）→ 先向前跳过非数字，再收集数字。
  while (i > 0 && !std::isdigit(static_cast<unsigned char>(text[i - 1]))) --i;
  const std::size_t digit_end = i;
  while (i > 0 && std::isdigit(static_cast<unsigned char>(text[i - 1]))) --i;
  if (i == digit_end) return -1;
  long value = 0;
  for (std::size_t k = i; k < digit_end; ++k) value = value * 10 + (text[k] - '0');
  return value;
}

struct HttpReply {
  bool transport_ok = false;
  int status = 0;
  std::string body;
};

HttpReply HttpGet(int port, const std::string& target) {
  HttpReply reply;
  RawClient client(port, /*tcp_nodelay=*/true);
  if (!client.Connect()) return reply;
  if (!client.SendRequest("GET", target, {}, "")) return reply;
  const auto response = client.ReadResponse(10000);
  if (!response.has_value()) return reply;
  reply.transport_ok = true;
  reply.status = response->status;
  reply.body = response->body;
  return reply;
}

//  轮询**真实条件**：readiness 返回期望的状态码。
bool PollReadiness(int port, int wanted_status, std::string* last_body, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto reply = HttpGet(port, kReadinessPath);
    if (last_body != nullptr) *last_body = reply.body;
    if (reply.transport_ok && reply.status == wanted_status) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

//  读取服务端 `max_connections`（测试自己直连；组合根读的是同一 GUC）。
int ServerMaxConnections() {
  auto connection = PgConnection::Connect(TestPgOptions(1));
  REQUIRE(connection.ok());
  const auto result = connection.value()->ExecParams(
      R"pgsql(SELECT setting::int FROM pg_settings WHERE name = 'max_connections')pgsql", {});
  REQUIRE(result.ok());
  return std::stoi(result.value().Value(0, 0));
}

//  ---- scratch schema（版本不符用例的隔离环境；结束必 DROP，残留 0）----
class ScratchSchema {
 public:
  explicit ScratchSchema(std::string name) : name_(std::move(name)) {
    auto connection = PgConnection::Connect(TestPgOptions(1));
    REQUIRE(connection.ok());
    connection_ = std::move(connection).value();
    //  迁移文件含 IF EXISTS/IF NOT EXISTS，会产生 NOTICE；只保留 warning 及以上。
    (void)connection_->ExecSimple("SET client_min_messages TO warning");
    REQUIRE(connection_->ExecSimple("CREATE SCHEMA " + name_).ok());
    //  同一连接上设置 search_path，后续所有 DDL/DML 都落在 scratch schema 内。
    REQUIRE(connection_->ExecSimple("SET search_path TO " + name_).ok());
  }

  ~ScratchSchema() {
    connection_.reset();
    //  用**不带 search_path** 的连接删除（DROP 的目标是限定名，不受 search_path 影响，
    //  但换连接可以确保即使本对象构建到一半也能清干净）。
    auto connection = PgConnection::Connect(TestPgOptions(1));
    if (connection.ok()) {
      (void)connection.value()->ExecSimple("SET client_min_messages TO warning");
      (void)connection.value()->ExecSimple("DROP SCHEMA IF EXISTS " + name_ + " CASCADE");
    }
  }

  ScratchSchema(const ScratchSchema&) = delete;
  ScratchSchema& operator=(const ScratchSchema&) = delete;

  const std::string& name() const { return name_; }

  //  在 scratch schema 里应用 `db/migrations/001_init.sql`（内容只读，不改）。
  void ApplyMigrations() {
    std::ifstream in(std::string(FSS_REPO_ROOT) + "/db/migrations/001_init.sql");
    REQUIRE(in.good());
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string sql = buffer.str();
    REQUIRE_FALSE(sql.empty());
    REQUIRE(connection_->ExecSimple(sql).ok());
  }

  //  写入一条已应用版本记录（覆盖迁移表）。
  void RecordVersion(int version, const std::string& name) {
    const auto result = connection_->ExecParams(
        "INSERT INTO schema_migrations(version,name) VALUES($1,$2) ON CONFLICT DO NOTHING",
        {std::to_string(version), name});
    REQUIRE(result.ok());
  }

  std::string SearchPathDsn() const {
    //  libpq URI：`options=-c search_path=<schema>`（空格 %20、`=` %3D）
    return DsnWithQuery(TestDsn(), "options=-c%20search_path%3D" + name_);
  }

 private:
  std::string name_;
  std::unique_ptr<PgConnection> connection_;
};

std::string ScratchName(const std::string& tag) {
  static int counter = 0;
  return "pgtest_b2a_" + tag + "_" + std::to_string(::getpid()) + "_" +
         std::to_string(++counter);
}

//  ---- 真实进程选项：端口/DSN 全部由配置/`--set` 给出（关掉夹具默认 env 覆盖）----
ServerProcessOptions ProcessOptions(const std::vector<std::string>& args,
                                    const std::vector<std::pair<std::string, std::string>>& env = {}) {
  ServerProcessOptions options;
  options.default_http_port = false;
  options.default_grpc_port = false;
  options.expect_grpc = false;
  options.default_storage_env = false;
  options.args = args;
  options.env = env;
  return options;
}

//  single + `*.repository=postgres` 的完整配置（schema 跨字段无 multi 要求）。
std::string SinglePgConfig(const std::string& root, int port, int metadata_pool,
                           int location_pool, long expected_instances, bool schema_check,
                           long clock_skew_tolerance_seconds = 60) {
  std::ostringstream json;
  json << "{\n"
       << "  \"deployment\": {\"mode\": \"single\", \"expected_instances\": "
       << expected_instances << ", \"clock_skew_tolerance_seconds\": "
       << clock_skew_tolerance_seconds << "},\n"
       << "  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " << port
       << "}, \"grpc\": {\"enabled\": false}},\n"
       << "  \"storage\": {\"posix\": {\"root\": \"" << root << "\"}},\n"
       << "  \"metadata\": {\"repository\": \"postgres\", \"postgres\": {\"max_connections\": "
       << metadata_pool << ", \"schema_version_check\": " << (schema_check ? "true" : "false")
       << "}},\n"
       << "  \"location\": {\"repository\": \"postgres\", \"postgres\": {\"max_connections\": "
       << location_pool << "}},\n"
       << "  \"self_signed\": {\"signing_key\": \"b2a-test\", \"public_base_url\": "
          "\"http://127.0.0.1:"
       << port << "/api/file\"},\n"
       << "  \"auth\": {\"mode\": \"disabled\"}\n"
       << "}\n";
  return json.str();
}

std::vector<std::string> PgArgs(const std::string& config, const std::string& dsn) {
  return {"--config", config, "--set", "metadata.postgres.dsn=" + dsn, "--set",
          "location.postgres.dsn=" + dsn};
}

}  // namespace

// =============================================================================
//  ① C9.28：连接预算（`deployment.expected_instances` 真的被用于判定）
// =============================================================================
TEST_CASE("★ B2a-1：PG 连接预算太小 → exit 78（数字可读）；拟合法的配置真的就绪",
          "[pg][infra][b2a]") {
  TempDir cfg_dir("b2a_budget_cfg");
  TempDir data_dir("b2a_budget_data");
  const std::string root = data_dir.child("store");
  const int max_connections = ServerMaxConnections();
  REQUIRE(max_connections >= 4);

  //  每实例最坏连接数 = metadata(1) + location(1) + lease(0，未启用) + leader_lock(1) = 3。
  const long cap = 3;
  const long fits = std::max<long>(1, max_connections / cap);
  const long too_many = fits + 1;  // 一定超过 max_connections
  REQUIRE(fits * cap <= max_connections);
  REQUIRE(too_many * cap > max_connections);

  SECTION("反例：expected_instances 过大 → exit 78 + 两组数字 + 三种修复方向") {
    const int port = FreePort();
    REQUIRE(port > 0);
    const std::string config = WriteFile(
        cfg_dir, "budget_bad.json",
        SinglePgConfig(root, port, /*metadata_pool=*/1, /*location_pool=*/1, too_many,
                       /*schema_check=*/true));
    const ProcessOutcome outcome = RunServerForExit(
        PgArgs(config, TestDsn()), {}, /*timeout_seconds=*/30);
    INFO("退出码=" << outcome.exit_code << "\n输出：\n" << outcome.output);
    REQUIRE(outcome.exit_code == 78);
    //  必须同时出现：PG 的 max_connections、每实例上限、乘积、以及修复方向。
    REQUIRE(outcome.output.find(std::to_string(max_connections)) != std::string::npos);
    REQUIRE(outcome.output.find(std::to_string(cap)) != std::string::npos);
    REQUIRE(outcome.output.find(std::to_string(too_many * cap)) != std::string::npos);
    REQUIRE(outcome.output.find("deployment.expected_instances") != std::string::npos);
    REQUIRE(outcome.output.find("max_connections") != std::string::npos);
  }

  SECTION("正控（R16）：拟合法的 expected_instances → 真的启动且 readiness 200") {
    const int port = FreePort();
    REQUIRE(port > 0);
    const std::string config = WriteFile(
        cfg_dir, "budget_ok.json",
        SinglePgConfig(root, port, /*metadata_pool=*/1, /*location_pool=*/1, fits,
                       /*schema_check=*/true));
    ServerProcess server(ProcessOptions(PgArgs(config, TestDsn())));
    REQUIRE(server.http_port() == port);
    std::string body;
    const bool ready = PollReadiness(port, 200, &body, 30000);
    CAPTURE(server.DumpLog(), body);
    REQUIRE(ready);
    REQUIRE(body == "File service is ready");
    //  横幅必须打印预算的实际结论（可运维）。
    REQUIRE(server.DumpLog().find("pg budget      : OK") != std::string::npos);
  }
}

// =============================================================================
//  ② 迁移版本校验（`metadata.postgres.schema_version_check`）
// =============================================================================
TEST_CASE("★ B2a-2：schema_version_check=true 且已应用版本不符 → not ready（原因可读）；"
          "正确迁移的库 → ready；false → 跳过版本比对",
          "[pg][infra][b2a]") {
  TempDir cfg_dir("b2a_schema_cfg");
  TempDir data_dir("b2a_schema_data");
  const std::string root = data_dir.child("store");

  SECTION("反例 + 正控：同一个 scratch schema 先写错版本（503）再改成期望版本（200）") {
    ScratchSchema scratch(ScratchName("schema"));
    scratch.ApplyMigrations();
    const int port = FreePort();
    REQUIRE(port > 0);
    const std::string config = WriteFile(
        cfg_dir, "schema_bad.json",
        SinglePgConfig(root, port, /*metadata_pool=*/2, /*location_pool=*/2,
                       /*expected_instances=*/1, /*schema_check=*/true));

    //  ---- 反例：写入一个"超前"的版本 → not ready ----
    scratch.RecordVersion(999, "injected_mismatch.sql");
    {
      ServerProcess server(ProcessOptions(PgArgs(config, scratch.SearchPathDsn())));
      REQUIRE(server.http_port() == port);
      std::string body;
      const bool not_ready = PollReadiness(port, 503, &body, 30000);
      CAPTURE(server.DumpLog(), body);
      REQUIRE(not_ready);
      //  原因必须可读：包含已应用版本、期望版本与"schema 版本不符"。
      REQUIRE(body.find("File service is not ready") != std::string::npos);
      REQUIRE(body.find("999") != std::string::npos);
      REQUIRE(body.find(std::to_string(fss::infra::kExpectedSchemaVersion)) !=
              std::string::npos);
      REQUIRE(body.find("schema") != std::string::npos);
    }

    //  ---- 正控：正确迁移的库（把版本改成期望值）→ ready ----
    {
      auto connection = PgConnection::Connect(TestPgOptions(1));
      REQUIRE(connection.ok());
      //  限定名删除错误行（不改共享库状态）。
      REQUIRE(connection.value()
                  ->ExecSimple("DELETE FROM " + scratch.name() +
                               ".schema_migrations WHERE version = 999")
                  .ok());
    }
    scratch.RecordVersion(fss::infra::kExpectedSchemaVersion, "001_init.sql");
    {
      ServerProcess server(ProcessOptions(PgArgs(config, scratch.SearchPathDsn())));
      std::string body;
      const bool ready = PollReadiness(port, 200, &body, 30000);
      CAPTURE(server.DumpLog(), body);
      REQUIRE(ready);
      REQUIRE(body == "File service is ready");
      REQUIRE(server.DumpLog().find("pg schema check: true") != std::string::npos);
    }
  }

  SECTION("控制键：schema_version_check=false + 版本不符 → 仍 ready（跳过版本比对真的生效）") {
    ScratchSchema scratch(ScratchName("skip"));
    scratch.ApplyMigrations();
    scratch.RecordVersion(999, "injected_mismatch.sql");
    const int port = FreePort();
    REQUIRE(port > 0);
    const std::string config = WriteFile(
        cfg_dir, "schema_off.json",
        SinglePgConfig(root, port, /*metadata_pool=*/2, /*location_pool=*/2,
                       /*expected_instances=*/1, /*schema_check=*/false));
    ServerProcess server(ProcessOptions(PgArgs(config, scratch.SearchPathDsn())));
    std::string body;
    const bool ready = PollReadiness(port, 200, &body, 30000);
    CAPTURE(server.DumpLog(), body);
    REQUIRE(ready);
    REQUIRE(body == "File service is ready");
    REQUIRE(server.DumpLog().find("pg schema check: false") != std::string::npos);
  }
}

// =============================================================================
//  ③ 时钟偏移（ADR-009 §6.4/§8.1）+ 测试接缝 `FSS_CLOCK_SKEW_INJECT_MS`
// =============================================================================
TEST_CASE("★ B2a-3：本地钟 vs PG now() 超容忍 → exit 78（两值可读）；接缝是唯一变量",
          "[pg][infra][b2a]") {
  TempDir cfg_dir("b2a_skew_cfg");
  TempDir data_dir("b2a_skew_data");
  const std::string root = data_dir.child("store");
  const int port = FreePort();
  REQUIRE(port > 0);
  //  容忍值 60s 是 schema/组合根默认；注入 120s 一定超限。
  const std::string config = WriteFile(
      cfg_dir, "skew.json",
      SinglePgConfig(root, port, /*metadata_pool=*/2, /*location_pool=*/2,
                     /*expected_instances=*/1, /*schema_check=*/true));

  SECTION("反例：注入 +120000 ms > 60000 ms 容忍 → exit 78 + 两值与容忍值") {
    const ProcessOutcome outcome =
        RunServerForExit(PgArgs(config, TestDsn()), {{"FSS_CLOCK_SKEW_INJECT_MS", "120000"}},
                         /*timeout_seconds=*/30);
    INFO("退出码=" << outcome.exit_code << "\n输出：\n" << outcome.output);
    REQUIRE(outcome.exit_code == 78);
    //  消息必须**同时**给出本地值、数据库值与偏差，且偏差 ≈ 注入值（+真实偏差）。
    REQUIRE(outcome.output.find("本地钟") != std::string::npos);
    REQUIRE(outcome.output.find("数据库 now()") != std::string::npos);
    const long skew_ms = ParseIntBefore(outcome.output, "ms（正");
    INFO("从消息解析出的偏差 = " << skew_ms << " ms");
    REQUIRE(skew_ms > 60000);    // 明确超过 60s 容忍
    REQUIRE(skew_ms > 100000);   // 确实由 120s 注入驱动（不是机器真实的大偏差）
    REQUIRE(skew_ms < 200000);
    //  容忍值以毫秒出现在消息里（60s → 60000ms），键名同样可见。
    REQUIRE(outcome.output.find("60000") != std::string::npos);
    REQUIRE(outcome.output.find("clock_skew_tolerance_seconds") != std::string::npos);
  }

  SECTION("正控 A：注入 +1000 ms（容忍内）→ 启动且 readiness 200，横幅打印实际偏差") {
    ServerProcess server(
        ProcessOptions(PgArgs(config, TestDsn()), {{"FSS_CLOCK_SKEW_INJECT_MS", "1000"}}));
    std::string body;
    const bool ready = PollReadiness(server.http_port(), 200, &body, 30000);
    CAPTURE(server.DumpLog(), body);
    REQUIRE(ready);
    REQUIRE(server.DumpLog().find("clock skew     : OK") != std::string::npos);
    //  接缝自身必须可见（避免演练结论被误读）。
    REQUIRE(server.DumpLog().find("clock inject   : 1000 ms") != std::string::npos);
  }

  SECTION("正控 B（R1）：**不注入** → 启动且 readiness 200（证明结论由接缝翻转）") {
    ServerProcess server(ProcessOptions(PgArgs(config, TestDsn())));
    std::string body;
    const bool ready = PollReadiness(server.http_port(), 200, &body, 30000);
    CAPTURE(server.DumpLog(), body);
    REQUIRE(ready);
    REQUIRE(server.DumpLog().find("clock skew     : OK") != std::string::npos);
    REQUIRE(server.DumpLog().find("clock inject") == std::string::npos);
  }

  SECTION("正控 C（R16）：同一个 120s 注入，把容忍改成 300s → 启动（键真的参与判定）") {
    const int tolerant_port = FreePort();
    REQUIRE(tolerant_port > 0);
    const std::string tolerant_config = WriteFile(
        cfg_dir, "skew_tolerant.json",
        SinglePgConfig(root, tolerant_port, /*metadata_pool=*/2, /*location_pool=*/2,
                       /*expected_instances=*/1, /*schema_check=*/true,
                       /*clock_skew_tolerance_seconds=*/300));
    ServerProcess server(ProcessOptions(PgArgs(tolerant_config, TestDsn()),
                                        {{"FSS_CLOCK_SKEW_INJECT_MS", "120000"}}));
    std::string body;
    const bool ready = PollReadiness(server.http_port(), 200, &body, 30000);
    CAPTURE(server.DumpLog(), body);
    REQUIRE(ready);
    //  横幅里的容忍值必须是配置的 300000 ms（不是写死的 60000）。
    REQUIRE(server.DumpLog().find("300000") != std::string::npos);
  }
}

// =============================================================================
//  ④ readiness 探针失败 + 恢复（PG 对本实例不可用 → not ready；恢复后 ready）
// =============================================================================
TEST_CASE("★ B2a-4：PG 后端被终止 → readiness not ready；后端重建后自动恢复 ready",
          "[pg][infra][b2a]") {
  TempDir cfg_dir("b2a_probe_cfg");
  TempDir data_dir("b2a_probe_data");
  const std::string root = data_dir.child("store");
  const int port = FreePort();
  REQUIRE(port > 0);

  //  ★ 用 `application_name` **精确**定位本实例的连接：只杀它自己的后端，
  //    不误伤同一 PG 上其它测试的连接（这是"不扰乱其它测试"的关键）。
  const std::string app_name = "pgtest_b2a_probe_" + std::to_string(::getpid());
  const std::string dsn = DsnWithQuery(TestDsn(), "application_name=" + app_name);
  const std::string config = WriteFile(
      cfg_dir, "probe.json",
      SinglePgConfig(root, port, /*metadata_pool=*/2, /*location_pool=*/2,
                     /*expected_instances=*/1, /*schema_check=*/true));
  ServerProcess server(ProcessOptions(PgArgs(config, dsn)));
  REQUIRE(server.http_port() == port);
  {
    std::string body;
    REQUIRE(PollReadiness(port, 200, &body, 30000));
    REQUIRE(body == "File service is ready");
  }

  //  ---- 让 PG 对本实例不可用：终止它的后端（只按 application_name 定位）----
  bool saw_not_ready = false;
  std::string last_body;
  for (int attempt = 0; attempt < 40 && !saw_not_ready; ++attempt) {
    {
      auto connection = PgConnection::Connect(TestPgOptions(1));
      REQUIRE(connection.ok());
      const auto killed = connection.value()->ExecParams(
          "SELECT count(pg_terminate_backend(pid)) FROM pg_stat_activity "
          "WHERE application_name = $1 AND pid <> pg_backend_pid()",
          {app_name});
      REQUIRE(killed.ok());
    }
    const auto reply = HttpGet(port, kReadinessPath);
    last_body = reply.body;
    if (reply.transport_ok && reply.status == 503 && reply.body.find("not ready") != std::string::npos) {
      saw_not_ready = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CAPTURE(server.DumpLog(), last_body);
  REQUIRE(saw_not_ready);

  //  ---- 恢复：池会丢弃坏连接并重建 → 轮询到 ready（PG 一直可达，恢复 = 连接重建）----
  std::string recovered_body;
  const bool recovered = PollReadiness(port, 200, &recovered_body, 30000);
  CAPTURE(server.DumpLog(), recovered_body);
  REQUIRE(recovered);
  REQUIRE(recovered_body == "File service is ready");
}
