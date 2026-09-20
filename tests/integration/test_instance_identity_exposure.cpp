// =============================================================================
//  E1a：`/v2/info` 的 `instanceId`（REST + gRPC 同源）
//       + `instance_registry` 陈旧行的**运行期**清理（非仅启动期）
// =============================================================================
//  交付 1（`instanceId`，唯一来源 `app::GetInfo`）：
//    ① DTO/JSON 渲染（进程内）：`instanceId` **恒渲染**（含空值），且键名是 camelCase；
//    ② 用例来源（进程内）：`app::GetInfo` 输出 == `UseCasePorts::instance_id`，改端口值即变；
//    ③ 真实进程 + 双协议同源：single 模式 `deployment.instance_id=ws-77` → REST 与 gRPC 都是
//       `ws-77`；换成 `ws-88` 的第二个进程 → 两者都 `ws-88`（负控：不是硬编码、不读共享文件）；
//       positive control：single + **未配置** `deployment.instance_id` → `instanceId == "local"`。
//    ⑥ 单实例（无 PG）**不暴露** `fss_instance_registry_*` 指标族（反面对照）。
//
//  交付 2（运行期清理，E1a）：
//    ④ multi + PG + 未配置 `deployment.instance_id` → REST `instanceId` 非空且 != "local"，
//       且等于 `instance_registry` 里 `heartbeat_at` 最新的那一行的 `instance_id`；
//    ⑤ 在同一存活进程里插入 synthetic 陈旧行 `ghost-stale`（400s 前）与新鲜行 `ghost-live`
//       （now）：**不重启进程**，轮询直到 `ghost-stale` 消失（应发生在下一个 10s tick）；
//       `ghost-live` 必须仍在（阈值被尊重，不是无差别 DELETE）；`/metrics` 的
//       `fss_instance_registry_stale_rows_removed_total >= 1`；
//    ⑦ 回归：`ghost-boot`（400s 前）在**启动前**插入 → 启动完成后必须已被启动期清理删掉。
//
//  ★ R1（自证对照）：本文件的 5 个注入点见 `docs/test-evidence/phase10.md` §23 的注入表。
//  ★ 铁律：连不上 PG 就失败（绝不 skip）；负断言配正控；一切等待轮询真实条件 + 有界超时。
//  ★ 隔离：每个用例一个独立 scratch schema（`pgtest_e1a_*`），结束即 DROP。
//  ★ 参数（钉住的设计，非配置键）：心跳 10s、live 窗口 30s、陈旧清理阈值 300s。
// =============================================================================
#include <catch2/catch.hpp>

#include "app_fixture.h"
#include "raw_http.h"
#include "server_process.h"
#include "temp_dir.h"

#include "adapters/http/dto/dto.h"
#include "app/usecases/usecases.h"
#include "common/json/json.h"
#include "infra/postgres/pg_connection.h"
#include "infra/postgres/pg_instance_registry.h"
#include "infra/postgres/pg_schema.h"

#include <grpcpp/grpcpp.h>
#include <google/protobuf/util/json_util.h>
#include <osdu/file/v1/file_service.grpc.pb.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using fss::infra::PgConnection;
using fss::infra::PgOptions;
using fss::test::RawClient;
using fss::test::ServerProcess;
using fss::test::ServerProcessOptions;
using fss::test::TempDir;

constexpr const char* kDefaultDsn = "postgresql://fss@127.0.0.1:15432/fss";
constexpr const char* kInfoPath = "/api/file/v2/info";
constexpr const char* kReadinessPath = "/api/file/v2/readiness_check";

std::string TestDsn() {
  const char* env = std::getenv("FSS_PG_DSN");
  if (env != nullptr && *env != '\0') return std::string(env);
  return kDefaultDsn;
}

PgOptions TestPgOptions() {
  PgOptions options;
  options.dsn = TestDsn();
  options.max_connections = 1;
  options.statement_timeout_millis = 5000;
  return options;
}

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

//  有界轮询一个**真实条件**（不用固定 sleep 赌时序）。
template <typename Predicate>
bool WaitFor(Predicate predicate, int attempts, int millis) {
  for (int i = 0; i < attempts; ++i) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
  }
  return predicate();
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
  const auto response = client.ReadResponse(15000);
  if (!response.has_value()) return reply;
  reply.transport_ok = true;
  reply.status = response->status;
  reply.body = response->body;
  return reply;
}

bool WaitReady(int port, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto reply = HttpGet(port, kReadinessPath);
    if (reply.transport_ok && reply.status == 200) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

//  `/v2/info` 的 `instanceId`：用 **camelCase 真解析**（不是子串），缺字段即失败。
std::string RestInstanceId(int port) {
  const auto reply = HttpGet(port, kInfoPath);
  INFO("REST /v2/info → " << reply.status << " " << reply.body);
  REQUIRE(reply.transport_ok);
  REQUIRE(reply.status == 200);
  const auto parsed = fss::json::Parse(reply.body);
  REQUIRE(parsed.ok());
  const auto& value = parsed.value();
  REQUIRE(value.contains("instanceId"));
  REQUIRE(value["instanceId"].is_string());
  return value["instanceId"].get<std::string>();
}

std::string GrpcInstanceId(int grpc_port) {
  auto channel = ::grpc::CreateChannel("127.0.0.1:" + std::to_string(grpc_port),
                                       ::grpc::InsecureChannelCredentials());
  auto stub = osdu::file::v1::FileService::NewStub(channel);
  REQUIRE(stub != nullptr);
  ::grpc::ClientContext context;
  ::google::protobuf::Empty request;
  osdu::file::v1::InfoResponse response;
  const auto status = stub->GetInfo(&context, request, &response);
  INFO("gRPC GetInfo → " << status.error_code() << " " << status.error_message());
  REQUIRE(status.ok());
  return response.instance_id();
}

//  Prometheus 文本里取一个**无标签**计数器的值；找不到返回 -1（判据用 `>= 0` 前置断言）。
long MetricValue(const std::string& body, const std::string& name) {
  std::size_t start = 0;
  while (start <= body.size()) {
    const auto end = body.find('\n', start);
    const std::string line =
        body.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (line.rfind(name + " ", 0) == 0) {
      return std::strtol(line.c_str() + name.size() + 1, nullptr, 10);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return -1;
}

//  ---- scratch schema（每个用例一个，结束即 DROP；注册表行随之清掉）----
class ScratchSchema {
 public:
  explicit ScratchSchema(std::string name) : name_(std::move(name)) {
    auto connection = PgConnection::Connect(TestPgOptions());
    REQUIRE(connection.ok());
    connection_ = std::move(connection).value();
    (void)connection_->ExecSimple("SET client_min_messages TO warning");
    REQUIRE(connection_->ExecSimple("CREATE SCHEMA " + name_).ok());
    REQUIRE(connection_->ExecSimple("SET search_path TO " + name_).ok());
  }
  ~ScratchSchema() {
    connection_.reset();
    auto connection = PgConnection::Connect(TestPgOptions());
    if (connection.ok()) {
      (void)connection.value()->ExecSimple("SET client_min_messages TO warning");
      (void)connection.value()->ExecSimple("DROP SCHEMA IF EXISTS " + name_ + " CASCADE");
    }
  }
  ScratchSchema(const ScratchSchema&) = delete;
  ScratchSchema& operator=(const ScratchSchema&) = delete;

  const std::string& name() const { return name_; }

  void ApplyMigrations() {
    std::ifstream in(std::string(FSS_REPO_ROOT) + "/db/migrations/001_init.sql");
    REQUIRE(in.good());
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string sql = buffer.str();
    REQUIRE_FALSE(sql.empty());
    REQUIRE(connection_->ExecSimple(sql).ok());
  }
  void RecordVersion(int version, const std::string& name) {
    const auto result = connection_->ExecParams(
        "INSERT INTO schema_migrations(version,name) VALUES($1,$2) ON CONFLICT DO NOTHING",
        {std::to_string(version), name});
    REQUIRE(result.ok());
  }
  std::string SearchPathDsn() const {
    return DsnWithQuery(TestDsn(), "options=-c%20search_path%3D" + name_);
  }

  //  ---- 注册表直连读写（测试用的**独立观测手段**，与服务器不是同一条连接）----
  std::vector<std::string> InstanceIds() {
    const auto result = connection_->ExecParams(
        "SELECT instance_id FROM instance_registry ORDER BY instance_id", {});
    REQUIRE(result.ok());
    std::vector<std::string> ids;
    for (int i = 0; i < result.value().RowCount(); ++i) {
      ids.push_back(result.value().Value(i, 0));
    }
    return ids;
  }
  bool HasRow(const std::string& instance_id) {
    const auto result = connection_->ExecParams(
        "SELECT count(*)::text FROM instance_registry WHERE instance_id = $1", {instance_id});
    REQUIRE(result.ok());
    return result.value().Value(0, 0) != "0";
  }
  //  心跳最新的那一行的 instance_id（④的判据：REST 值必须与注册表行一致）。
  std::string NewestHeartbeatRow() {
    const auto result = connection_->ExecParams(
        "SELECT instance_id FROM instance_registry ORDER BY heartbeat_at DESC LIMIT 1", {});
    REQUIRE(result.ok());
    REQUIRE(result.value().RowCount() == 1);
    return result.value().Value(0, 0);
  }
  //  合成陈旧/新鲜行（⑤）；`age_seconds` 为 0 时用 now()。
  void InsertGhost(const std::string& id, int age_seconds) {
    const auto result = connection_->ExecParams(
        "INSERT INTO instance_registry(instance_id, service_version, config_hash, started_at,"
        " heartbeat_at) VALUES($1,'ghost-synthetic','ghost-synthetic',"
        " now() - ($2::int * INTERVAL '1 second'), now() - ($2::int * INTERVAL '1 second'))"
        " ON CONFLICT (instance_id) DO UPDATE SET heartbeat_at = EXCLUDED.heartbeat_at",
        {id, std::to_string(age_seconds)});
    REQUIRE(result.ok());
  }
  void DeleteGhost(const std::string& id) {
    (void)connection_->ExecParams("DELETE FROM instance_registry WHERE instance_id = $1", {id});
  }

 private:
  std::string name_;
  std::unique_ptr<PgConnection> connection_;
};

std::string ScratchName(const std::string& tag) {
  static int counter = 0;
  return "pgtest_e1a_" + tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(++counter);
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

//  `deployment.mode=multi` 的完整配置（schema 的跨字段要求全部满足）。
//  `instance_id` 为空 ⇒ **不写该键**（有效默认 `local` ⇒ 组合根自动生成唯一 id）。
std::string MultiConfig(const std::string& root, int port, std::int64_t lock_key,
                        const std::string& instance_id = "") {
  std::ostringstream json;
  json << "{\n"
       << "  \"deployment\": {\"mode\": \"multi\",";
  if (!instance_id.empty()) json << " \"instance_id\": \"" << instance_id << "\",";
  json << " \"expected_instances\": 2, \"max_clock_skew_seconds\": 5},\n"
       << "  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " << port
       << "}, \"grpc\": {\"enabled\": false}},\n"
       << "  \"storage\": {\"posix\": {\"root\": \"" << root
       << "\", \"shared_mount_required\": true}},\n"
       << "  \"metadata\": {\"repository\": \"postgres\", \"postgres\": {\"max_connections\": 1,"
       << " \"schema_version_check\": true}},\n"
       << "  \"location\": {\"repository\": \"postgres\", \"postgres\": {\"max_connections\": 1}},\n"
       << "  \"leases\": {\"enabled\": true},\n"
       << "  \"leader_election\": {\"enabled\": true, \"lock_key\": " << lock_key << "},\n"
       << "  \"gc\": {\"enabled\": false, \"require_lease_expiry\": true},\n"
       << "  \"self_signed\": {\"signing_key\": \"e1a-test\", \"public_base_url\": "
          "\"http://127.0.0.1:"
       << port << "/api/file\"},\n"
       << "  \"auth\": {\"mode\": \"disabled\"},\n"
       << "  \"observability\": {\"log_level\": \"info\"}\n"
       << "}\n";
  return json.str();
}

std::vector<std::string> PgArgs(const std::string& config, const std::string& dsn) {
  return {"--config", config, "--set", "metadata.postgres.dsn=" + dsn, "--set",
          "location.postgres.dsn=" + dsn};
}

}  // namespace

// =============================================================================
//  ① DTO/JSON 渲染（进程内）：恒渲染 + camelCase（R1 的 I1/I5 注入点）
// =============================================================================
TEST_CASE("★ E1a-1：VersionInfoResponse 的 instanceId 恒渲染（含空值）且是 camelCase",
          "[e1a][unit]") {
  using fss::adapters::http::ToJson;
  using fss::adapters::http::VersionInfoResponse;

  SECTION("非空值：JSON 里 instanceId == 原值，且没有 snake_case / PascalCase 变体") {
    VersionInfoResponse response;
    response.version = "v2";
    response.instance_id = "ws-77";
    const std::string text = fss::json::Dump(ToJson(response));
    INFO("渲染出的 JSON：" << text);
    const auto parsed = fss::json::Parse(text);
    REQUIRE(parsed.ok());
    const auto& value = parsed.value();
    REQUIRE(value.contains("instanceId"));
    REQUIRE(value["instanceId"].is_string());
    REQUIRE(value["instanceId"].get<std::string>() == "ws-77");
    //  ①的负控：错误的键名不得出现（I5 注入会让这两条失败）
    REQUIRE_FALSE(value.contains("instance_id"));
    REQUIRE_FALSE(value.contains("InstanceId"));
  }

  SECTION("空值：键**仍然存在**（恒渲染决策被机械钉住；I1 注入会让这条失败）") {
    VersionInfoResponse response;
    response.version = "v2";
    response.instance_id = "";
    const std::string text = fss::json::Dump(ToJson(response));
    INFO("渲染出的 JSON（空 instance_id）：" << text);
    const auto parsed = fss::json::Parse(text);
    REQUIRE(parsed.ok());
    const auto& value = parsed.value();
    //  ★ 这条就是"字段是否存在"只由**版本**决定、与取值无关的判据。
    REQUIRE(value.contains("instanceId"));
    REQUIRE(value["instanceId"].is_string());
    REQUIRE(value["instanceId"].get<std::string>().empty());
    //  正控：同一份 JSON 里 `version` 一定是非空的 —— 证明解析没有整体退化成空对象
    REQUIRE(value["version"].get<std::string>() == "v2");
  }
}

// =============================================================================
//  §4 契约：proto3-JSON 的键名必须是 `instanceId`（`json_name`），与 REST 对齐
// =============================================================================
//  为什么单独钉：gRPC 客户端启用 proto3-JSON 时序列化出的键名就是 `json_name`；
//  写错/漏写会让"gRPC 那条协议的 JSON 形状"与 REST（唯一合规面，ADR-001）不一致。
TEST_CASE("★ E1a-proto：InfoResponse.instance_id 的 proto3-JSON 键名是 instanceId",
          "[e1a][unit]") {
  osdu::file::v1::InfoResponse response;
  response.set_instance_id("ws-77");
  std::string json;
  REQUIRE(google::protobuf::util::MessageToJsonString(response, &json).ok());
  INFO("proto3-JSON：" << json);
  REQUIRE(json.find("\"instanceId\":\"ws-77\"") != std::string::npos);
  //  反面对照：snake_case 键名不得出现
  REQUIRE(json.find("\"instance_id\"") == std::string::npos);
}

// =============================================================================
//  ② 用例来源（进程内）：`app::GetInfo` 原样转发 `UseCasePorts::instance_id`
// =============================================================================
TEST_CASE("★ E1a-2：app::GetInfo 的 instance_id 来自 UseCasePorts（改端口值即变）",
          "[e1a][unit]") {
  //  `UseCasePorts` 全是引用成员 ⇒ 没有默认构造；用仓库既有的内存装配（app_fixture）。
  fss::test::AppFixture fx;
  fss::app::GetInfo usecase(*fx.ports);

  fx.ports->instance_id = "peer-alpha";
  const auto first = usecase.Execute();
  REQUIRE(first.ok());
  REQUIRE(first.value().instance_id == "peer-alpha");

  //  ★ 负控（I2 注入点）：换端口值 ⇒ 输出必须跟着换（证明不是常量、不是硬编码）。
  fx.ports->instance_id = "peer-beta-42";
  const auto second = usecase.Execute();
  REQUIRE(second.ok());
  REQUIRE(second.value().instance_id == "peer-beta-42");
  REQUIRE(second.value().instance_id != first.value().instance_id);

  //  正控：默认端口值是 "local"（组合根在 single 下原样透传）。
  fss::app::UseCasePorts defaults = *fx.ports;
  defaults.instance_id = "local";
  fss::app::GetInfo default_usecase(defaults);
  const auto plain = default_usecase.Execute();
  REQUIRE(plain.ok());
  REQUIRE(plain.value().instance_id == "local");
}

// =============================================================================
//  ③ 真实进程 + 双协议同源：ws-77 / ws-88（负控）+ 未配置 → local（正控）
// =============================================================================
TEST_CASE("★ E1a-3：真实 single 进程 REST 与 gRPC 同源（ws-77 / ws-88 / 未配置→local）",
          "[e1a][pg][integration]") {
  const auto start = [](const std::string& instance_id) {
    ServerProcessOptions options;  // 默认：HTTP=0、gRPC=-1、默认存储 env
    if (!instance_id.empty()) options.args = {"--set", "deployment.instance_id=" + instance_id};
    auto server = std::make_unique<ServerProcess>(options);
    REQUIRE(WaitReady(server->http_port(), 30000));
    REQUIRE(server->grpc_port() != 0);
    return server;
  };

  {
    auto server = start("ws-77");
    CAPTURE(server->DumpLog());
    REQUIRE(RestInstanceId(server->http_port()) == "ws-77");
    REQUIRE(GrpcInstanceId(server->grpc_port()) == "ws-77");
  }
  {
    //  ★ 负控：换一个进程、换一个 id ⇒ 两条协议都必须跟着变
    //    （证明字段既不是硬编码常量，也不是读某个共享文件/全局）。
    auto server = start("ws-88");
    CAPTURE(server->DumpLog());
    REQUIRE(RestInstanceId(server->http_port()) == "ws-88");
    REQUIRE(GrpcInstanceId(server->grpc_port()) == "ws-88");
  }
  {
    //  ★ R16 正控：single + **未配置** `deployment.instance_id` ⇒ 恰好 `local`
    //    （证明 ④ 的 "!= local" 断言不是恒真）。single 不自动生成 id（逐字不变）。
    auto server = start("");
    CAPTURE(server->DumpLog());
    REQUIRE(RestInstanceId(server->http_port()) == "local");
    REQUIRE(GrpcInstanceId(server->grpc_port()) == "local");
  }
}

// =============================================================================
//  ⑥ 单实例（无 PG）不暴露 `fss_instance_registry_*` 指标族（反面对照）
// =============================================================================
TEST_CASE("★ E1a-6：single 模式的 /metrics **没有** fss_instance_registry_ 族",
          "[e1a][integration]") {
  ServerProcessOptions options;
  options.expect_grpc = false;
  options.default_grpc_port = false;
  ServerProcess server(options);
  REQUIRE(WaitReady(server.http_port(), 30000));

  const auto metrics = HttpGet(server.http_port(), "/metrics");
  CAPTURE(server.DumpLog(), metrics.body);
  REQUIRE(metrics.transport_ok);
  REQUIRE(metrics.status == 200);
  //  ★ 反面对照：该指标族只在 PG 注册表被装配时存在。若它出现在 single 进程上，
  //    说明指标被无条件注册（= 与 PG 装配脱钩）。
  REQUIRE(metrics.body.find("fss_instance_registry_") == std::string::npos);
  //  正控：同一份 /metrics 里其它族一定在 —— 证明上面的"找不到"不是因为抓取到了空文本。
  REQUIRE(metrics.body.find("fss_io_engine") != std::string::npos);
}

// =============================================================================
//  ④ + ⑤ multi + PG：自动生成 id 与注册表一致；运行期清理陈旧行
// =============================================================================
//  ★ ④ 与 ⑤ 共用**同一个存活进程**：⑤ 的判据必须在进程存活期间成立
//    （如果靠"重启时清理"，I3 注入就不会让它失败 ⇒ 判据失去区分力）。
TEST_CASE("★ E1a-4/5：multi 自动生成 instanceId 与注册表一致；运行期删除陈旧行",
          "[e1a][pg][integration]") {
  ScratchSchema scratch(ScratchName("runtime"));
  scratch.ApplyMigrations();
  scratch.RecordVersion(fss::infra::kExpectedSchemaVersion, "001_init.sql");
  TempDir cfg_dir("e1a_runtime_cfg");
  TempDir data_dir("e1a_runtime_data");
  const std::string root = data_dir.child("store");
  const int port = FreePort();
  REQUIRE(port > 0);
  const std::int64_t lock_key = 1830000000LL + static_cast<std::int64_t>(::getpid() % 100000);

  const std::string config = WriteFile(
      cfg_dir, "multi.json", MultiConfig(root, port, lock_key));
  ServerProcess server(ProcessOptions(PgArgs(config, scratch.SearchPathDsn())));
  REQUIRE(server.http_port() == port);
  REQUIRE(WaitReady(port, 30000));

  //  ---- ④ 自动生成 + 与注册表最新心跳行一致 ----
  const std::string rest_id = RestInstanceId(port);
  CAPTURE(server.DumpLog(), rest_id);
  REQUIRE_FALSE(rest_id.empty());
  REQUIRE(rest_id != "local");  // ★ 负控（正控在 E1a-3：未配置的 single == "local"）
  REQUIRE(scratch.HasRow(rest_id));
  REQUIRE(scratch.NewestHeartbeatRow() == rest_id);

  //  ---- ⑤ 运行期清理（判据必须在进程存活期间成立）----
  const std::string stale_id = "ghost-stale-" + std::to_string(::getpid());
  const std::string live_id = "ghost-live-" + std::to_string(::getpid());
  scratch.InsertGhost(stale_id, /*age_seconds=*/400);
  scratch.InsertGhost(live_id, /*age_seconds=*/0);
  //  正控：插入后 `ghost-stale` **确实存在** —— 否则下面的"它消失了"可能只是因为
  //  插入根本没成功（恒真的负断言，见 AGENTS §4.3 的"否定式判据必须配正控"）。
  REQUIRE(scratch.HasRow(stale_id));
  REQUIRE(scratch.HasRow(live_id));

  const auto inserted_at = std::chrono::steady_clock::now();
  const bool removed = WaitFor([&] { return !scratch.HasRow(stale_id); },
                               /*attempts=*/80, /*millis=*/500);  // 最多 40s（≥3 个 tick）
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - inserted_at)
                           .count();
  INFO("ghost-stale 从插入到消失耗时（ms）：" << elapsed);
  CAPTURE(server.DumpLog(), elapsed);
  REQUIRE(removed);

  //  ★ 阈值被尊重：新鲜行**必须仍在**（I4 注入会让这条失败：无差别 DELETE）。
  REQUIRE(scratch.HasRow(live_id));

  //  ★ 指标：删除计数 >= 1（与"行真的消失了"互为印证，R1）。
  const auto metrics = HttpGet(port, "/metrics");
  REQUIRE(metrics.transport_ok);
  REQUIRE(metrics.status == 200);
  const long removed_metric = MetricValue(metrics.body, "fss_instance_registry_stale_rows_removed_total");
  const long runs_metric = MetricValue(metrics.body, "fss_instance_registry_cleanup_runs_total");
  INFO("fss_instance_registry_stale_rows_removed_total = " << removed_metric
       << "；cleanup_runs_total = " << runs_metric);
  REQUIRE(removed_metric >= 1);
  REQUIRE(runs_metric >= 1);

  //  ---- 清理 synthetic 行（teardown 的一部分；scratch schema 也会整体 DROP）----
  scratch.DeleteGhost(stale_id);
  scratch.DeleteGhost(live_id);
  REQUIRE_FALSE(scratch.HasRow(stale_id));
  REQUIRE_FALSE(scratch.HasRow(live_id));
}

// =============================================================================
//  ⑦ 回归：启动期清理仍然工作（不能被"运行期清理"悄悄替换掉）
// =============================================================================
TEST_CASE("★ E1a-7：启动期清理回归 —— 启动前插入的 400s 陈旧行在 ready 时已消失",
          "[e1a][pg][integration]") {
  ScratchSchema scratch(ScratchName("boot"));
  scratch.ApplyMigrations();
  scratch.RecordVersion(fss::infra::kExpectedSchemaVersion, "001_init.sql");
  TempDir cfg_dir("e1a_boot_cfg");
  TempDir data_dir("e1a_boot_data");
  const std::string root = data_dir.child("store");
  const int port = FreePort();
  REQUIRE(port > 0);
  const std::int64_t lock_key = 1831000000LL + static_cast<std::int64_t>(::getpid() % 100000);

  const std::string boot_id = "ghost-boot-" + std::to_string(::getpid());
  scratch.InsertGhost(boot_id, /*age_seconds=*/400);
  REQUIRE(scratch.HasRow(boot_id));  // 前置条件显式断言（R9）

  const std::string config = WriteFile(cfg_dir, "boot.json", MultiConfig(root, port, lock_key));
  ServerProcess server(ProcessOptions(PgArgs(config, scratch.SearchPathDsn())));
  REQUIRE(WaitReady(port, 30000));

  CAPTURE(server.DumpLog());
  REQUIRE_FALSE(scratch.HasRow(boot_id));  // 启动期清理已删掉它
  REQUIRE(server.DumpLog().find("启动清理") != std::string::npos);
  scratch.DeleteGhost(boot_id);
}
