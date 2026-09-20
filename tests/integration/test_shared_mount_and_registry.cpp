// =============================================================================
//  B2b（ADR-009 §5.3 / §8.1 item 4）：`instance_registry` 心跳 + 配置/版本一致性
//  + `storage.posix.shared_mount_required` 的**真实探针**
// =============================================================================
//  判据（本切片；逐条对应下面的用例）：
//    ① 两个进程、**同一存储根**、同一 PG：都 ready；各自能在共享目录里看到对方的
//       `<root>/.fss_probe.<instance_id>`（轮询真实文件）；无一致性抱怨；
//       两个注册表行的 `config_hash` 相等（证明"每实例不同的键已被排除"）。
//    ② ★ 判别性用例：两个进程、同一 PG、**不同 storage root** → 后启动者
//       **exit 78**，原因里同时出现"live peer 的 instance_id"与探针文件路径；
//       而且它在退出前已注销自己的注册表行（不留半截状态）。
//    ③ 配置不一致（`config_hash` 不同）→ 后启动者 **not ready**，原因可读并指出对端；
//       正控：配置逐字一致 → ready。版本不兼容（`FSS_SERVICE_VERSION_OVERRIDE`）→ not ready。
//    ④ stale 行被忽略：`kill -9` 对端并删掉它的探针 → 幸存者先 **not ready**
//       （证明 live peer 真的被检查，负断言不是恒真），过 live 窗口后回到 **ready**
//       （证明 stale 行被忽略，不会永久阻塞）。
//
//  ★ 隔离：每个用例一个**独立 scratch schema**（`pgtest_b2b_*`，应用同一份
//    `db/migrations/001_init.sql`），结束即 `DROP SCHEMA ... CASCADE` ——
//    `instance_registry` 的行随之清掉，**共享库零残留**，也不会看到别的测试的行。
//  ★ 铁律（AGENTS R1/R9/§4.3）：连不上 PG 就失败（绝不 skip）；每个负断言配正控；
//    一切等待都**轮询真实条件** + 有界超时，不用固定 sleep 赌时序。
//  ★ 参数（钉住的设计）：心跳 10s、live 窗口 30s、stale 清理阈值 300s。
// =============================================================================
#include <catch2/catch.hpp>

#include "raw_http.h"
#include "server_process.h"
#include "temp_dir.h"

#include "infra/postgres/pg_connection.h"
#include "infra/postgres/pg_instance_registry.h"
#include "infra/postgres/pg_schema.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
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
bool WaitFor(Predicate predicate, int attempts = 200, int millis = 50) {
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
  const auto response = client.ReadResponse(10000);
  if (!response.has_value()) return reply;
  reply.transport_ok = true;
  reply.status = response->status;
  reply.body = response->body;
  return reply;
}

bool PollReadiness(int port, int wanted_status, std::string* last_body, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto reply = HttpGet(port, kReadinessPath);
    if (last_body != nullptr) *last_body = reply.body;
    if (reply.transport_ok && reply.status == wanted_status) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

std::string ProbeFileName(const std::string& instance_id) {
  return ".fss_probe." + instance_id;
}
std::string ProbePath(const std::string& root, const std::string& instance_id) {
  return root + "/" + ProbeFileName(instance_id);
}

//  探针文件存在、且内容里的 `instance_id=` **整行**等于该实例（防止半截/陈旧文件）。
bool ProbeFileVisible(const std::string& root, const std::string& instance_id) {
  std::ifstream in(ProbePath(root, instance_id), std::ios::binary);
  if (!in) return false;
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string text = buffer.str();
  const std::string line = "instance_id=" + instance_id;
  return text == line || text.find(line + "\n") != std::string::npos;
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

 private:
  std::string name_;
  std::unique_ptr<PgConnection> connection_;
};

std::string ScratchName(const std::string& tag) {
  static int counter = 0;
  return "pgtest_b2b_" + tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(++counter);
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
//  `expected_instances=2`：本文件最多同时跑 2 个实例；每实例最坏 4 连接 ⇒ 需要 8。
std::string MultiConfig(const std::string& root, int port, const std::string& instance_id,
                        std::int64_t lock_key, const std::string& log_level = "info",
                        bool schema_check = true) {
  std::ostringstream json;
  json << "{\n"
       << "  \"deployment\": {\"mode\": \"multi\", \"instance_id\": \"" << instance_id
       << "\", \"expected_instances\": 2, \"max_clock_skew_seconds\": 5},\n"
       << "  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " << port
       << "}, \"grpc\": {\"enabled\": false}},\n"
       << "  \"storage\": {\"posix\": {\"root\": \"" << root
       << "\", \"shared_mount_required\": true}},\n"
       << "  \"metadata\": {\"repository\": \"postgres\", \"postgres\": {\"max_connections\": 1,"
       << " \"schema_version_check\": " << (schema_check ? "true" : "false") << "}},\n"
       << "  \"location\": {\"repository\": \"postgres\", \"postgres\": {\"max_connections\": 1}},\n"
       << "  \"leases\": {\"enabled\": true},\n"
       << "  \"leader_election\": {\"enabled\": true, \"lock_key\": " << lock_key << "},\n"
       << "  \"gc\": {\"enabled\": false, \"require_lease_expiry\": true},\n"
       << "  \"self_signed\": {\"signing_key\": \"b2b-test\", \"public_base_url\": "
          "\"http://127.0.0.1:"
       << port << "/api/file\"},\n"
       << "  \"auth\": {\"mode\": \"disabled\"},\n"
       << "  \"observability\": {\"log_level\": \"" << log_level << "\"}\n"
       << "}\n";
  return json.str();
}

std::vector<std::string> PgArgs(const std::string& config, const std::string& dsn) {
  return {"--config", config, "--set", "metadata.postgres.dsn=" + dsn, "--set",
          "location.postgres.dsn=" + dsn};
}

//  注册表行（从 scratch schema 直连读回）。
struct RegistryRow {
  std::string instance_id;
  std::string service_version;
  std::string config_hash;
  std::string heartbeat_age_seconds;  // `extract(epoch from now()-heartbeat_at)::int`
};

std::vector<RegistryRow> RegistryRows(const std::string& scratch_dsn) {
  PgOptions options;
  options.dsn = scratch_dsn;
  options.max_connections = 1;
  options.statement_timeout_millis = 5000;
  auto connection = PgConnection::Connect(options);
  REQUIRE(connection.ok());
  const auto result = connection.value()->ExecParams(
      "SELECT instance_id, service_version, config_hash,"
      " extract(epoch from now() - heartbeat_at)::int::text"
      " FROM instance_registry ORDER BY instance_id",
      {});
  REQUIRE(result.ok());
  std::vector<RegistryRow> rows;
  for (int i = 0; i < result.value().RowCount(); ++i) {
    rows.push_back(RegistryRow{result.value().Value(i, 0), result.value().Value(i, 1),
                               result.value().Value(i, 2), result.value().Value(i, 3)});
  }
  return rows;
}

bool AnyRowFor(const std::vector<RegistryRow>& rows, const std::string& id) {
  for (const auto& row : rows) {
    if (row.instance_id == id) return true;
  }
  return false;
}

std::string RowConfigHash(const std::vector<RegistryRow>& rows, const std::string& id) {
  for (const auto& row : rows) {
    if (row.instance_id == id) return row.config_hash;
  }
  return {};
}

bool ProcessAlive(const std::string& pid) {
  return std::system(("kill -0 " + pid + " 2>/dev/null").c_str()) == 0;
}

}  // namespace

// =============================================================================
//  ① 同一存储根的两个实例：都 ready + 互相看得见探针 + config_hash 相等（正控）
// =============================================================================
TEST_CASE("★ B2b-1：同一 PG + 同一 storage root 的两个 multi 实例都 ready，且互相看得见探针",
          "[pg][infra][b2b]") {
  ScratchSchema scratch(ScratchName("shared"));
  scratch.ApplyMigrations();
  scratch.RecordVersion(fss::infra::kExpectedSchemaVersion, "001_init.sql");
  TempDir cfg_dir("b2b_shared_cfg");
  TempDir data_dir("b2b_shared_data");
  const std::string root = data_dir.child("store");
  const int port_a = FreePort();
  const int port_b = FreePort();
  REQUIRE(port_a > 0);
  REQUIRE(port_b > 0);
  const std::int64_t lock_key = 1820000000LL + static_cast<std::int64_t>(::getpid() % 100000);
  const std::string id_a = "b2b-a1-" + std::to_string(::getpid());
  const std::string id_b = "b2b-b1-" + std::to_string(::getpid());

  const std::string config_a = WriteFile(
      cfg_dir, "a.json", MultiConfig(root, port_a, id_a, lock_key));
  const std::string config_b = WriteFile(
      cfg_dir, "b.json", MultiConfig(root, port_b, id_b, lock_key));

  ServerProcess server_a(ProcessOptions(PgArgs(config_a, scratch.SearchPathDsn())));
  REQUIRE(server_a.http_port() == port_a);
  std::string body_a;
  {
    const bool ready = PollReadiness(port_a, 200, &body_a, 30000);
    CAPTURE(server_a.DumpLog(), body_a);
    REQUIRE(ready);
    REQUIRE(body_a == "File service is ready");
  }
  //  ★ 启动时没有 live peer ⇒ **必须如实登记"尚未验证"**（不能声称已验证）。
  REQUIRE(server_a.DumpLog().find("shared mount   : 已写入") != std::string::npos);
  REQUIRE(server_a.DumpLog().find("跨实例可见性尚未验证") != std::string::npos);

  ServerProcess server_b(ProcessOptions(PgArgs(config_b, scratch.SearchPathDsn())));
  REQUIRE(server_b.http_port() == port_b);
  std::string body_b;
  {
    const bool ready = PollReadiness(port_b, 200, &body_b, 30000);
    CAPTURE(server_a.DumpLog(), server_b.DumpLog(), body_b);
    REQUIRE(ready);
    REQUIRE(body_b == "File service is ready");
  }
  //  ★ B 启动时 A 是 live peer，且 A 的探针在共享根下可见 ⇒ 横幅必须说"已验证"。
  REQUIRE(server_b.DumpLog().find("shared mount   : 已验证（1 个 live peer") !=
          std::string::npos);

  //  ★ 真实条件（轮询真实文件，不是只信横幅）：两个探针文件都出现且内容正确。
  const bool both_visible = WaitFor(
      [&] { return ProbeFileVisible(root, id_a) && ProbeFileVisible(root, id_b); },
      /*attempts=*/100, /*millis=*/50);
  CAPTURE(server_a.DumpLog(), server_b.DumpLog());
  REQUIRE(both_visible);
  //  正控 + 负控（R1）：探针路径写错会让上面的断言恒真 —— 用一个**不存在的实例 id**
  //  确认 `ProbeFileVisible` 会失败。
  REQUIRE_FALSE(ProbeFileVisible(root, id_a + "-does-not-exist"));

  //  ★ 注册表直连读回：两行都在，`config_hash` 相等（证明端口/实例 id 已被排除），
  //    且 `service_version` 非空。
  const auto rows = RegistryRows(scratch.SearchPathDsn());
  CAPTURE(rows.size());
  REQUIRE(AnyRowFor(rows, id_a));
  REQUIRE(AnyRowFor(rows, id_b));
  const std::string hash_a = RowConfigHash(rows, id_a);
  const std::string hash_b = RowConfigHash(rows, id_b);
  CAPTURE(hash_a, hash_b);
  REQUIRE_FALSE(hash_a.empty());
  REQUIRE(hash_a == hash_b);
  bool saw_version = false;
  for (const auto& row : rows) {
    if (!row.service_version.empty()) saw_version = true;
  }
  REQUIRE(saw_version);
}

// =============================================================================
//  ② ★ 判别性用例：同一 PG、**不同 storage root** → 第二个实例 exit 78
// =============================================================================
TEST_CASE("★ B2b-2：同一 PG + **不同** storage root → 第二个实例 exit 78（不是共享挂载）",
          "[pg][infra][b2b]") {
  ScratchSchema scratch(ScratchName("split"));
  scratch.ApplyMigrations();
  scratch.RecordVersion(fss::infra::kExpectedSchemaVersion, "001_init.sql");
  TempDir cfg_dir("b2b_split_cfg");
  TempDir data_a_dir("b2b_split_data_a");
  TempDir data_b_dir("b2b_split_data_b");
  const std::string root_a = data_a_dir.child("store");
  const std::string root_b = data_b_dir.child("store");
  REQUIRE(root_a != root_b);
  const int port_a = FreePort();
  const int port_b = FreePort();
  REQUIRE(port_a > 0);
  REQUIRE(port_b > 0);
  const std::int64_t lock_key = 1821000000LL + static_cast<std::int64_t>(::getpid() % 100000);
  const std::string id_a = "b2b-a2-" + std::to_string(::getpid());
  const std::string id_b = "b2b-b2-" + std::to_string(::getpid());

  const std::string config_a = WriteFile(
      cfg_dir, "a.json", MultiConfig(root_a, port_a, id_a, lock_key));
  const std::string config_b = WriteFile(
      cfg_dir, "b.json", MultiConfig(root_b, port_b, id_b, lock_key));

  ServerProcess server_a(ProcessOptions(PgArgs(config_a, scratch.SearchPathDsn())));
  REQUIRE(server_a.http_port() == port_a);
  std::string body_a;
  {
    const bool ready = PollReadiness(port_a, 200, &body_a, 30000);
    CAPTURE(server_a.DumpLog(), body_a);
    REQUIRE(ready);
  }

  //  ★ 第二实例：同一 PG、自己的（非共享）root ⇒ 启动期必须 fail-closed。
  const ProcessOutcome outcome =
      RunServerForExit(PgArgs(config_b, scratch.SearchPathDsn()), {}, /*timeout_seconds=*/20);
  CAPTURE(outcome.exit_code, outcome.output);
  REQUIRE(outcome.exit_code == 78);
  REQUIRE(outcome.output.find("拒绝启动") != std::string::npos);
  //  原因必须**同时**给出：live peer 的 instance_id、不可见的探针路径、两个 root。
  REQUIRE(outcome.output.find(id_a) != std::string::npos);
  REQUIRE(outcome.output.find(ProbeFileName(id_a)) != std::string::npos);
  REQUIRE(outcome.output.find(root_b) != std::string::npos);
  REQUIRE(outcome.output.find("不是所有实例共享的挂载") != std::string::npos);
  REQUIRE(outcome.output.find("已启动") == std::string::npos);

  //  ★ 半截状态不留：B 在 exit 78 前已 upsert，退出路径必须注销自己的行。
  const auto rows_after = RegistryRows(scratch.SearchPathDsn());
  CAPTURE(rows_after.size());
  REQUIRE_FALSE(AnyRowFor(rows_after, id_b));
  REQUIRE(AnyRowFor(rows_after, id_a));

  //  ★ A 不受影响：B 注销后 A 保持 ready（轮询真实条件）。
  std::string body_a_after;
  const bool still_ready = PollReadiness(port_a, 200, &body_a_after, 20000);
  CAPTURE(server_a.DumpLog(), body_a_after);
  REQUIRE(still_ready);
}

// =============================================================================
//  ③ config_hash 不一致 → not ready；版本不兼容 → not ready；相同配置 → ready（正控）
// =============================================================================
TEST_CASE("★ B2b-3：config_hash / 服务版本不一致 → 后启动者 not ready（含正控）",
          "[pg][infra][b2b]") {
  TempDir cfg_dir("b2b_mismatch_cfg");
  TempDir data_dir("b2b_mismatch_data");
  const std::string root = data_dir.child("store");

  SECTION("config_hash 不一致（log_level 不同）→ 503，原因指出对端") {
    ScratchSchema scratch(ScratchName("cfg"));
    scratch.ApplyMigrations();
    scratch.RecordVersion(fss::infra::kExpectedSchemaVersion, "001_init.sql");
    const int port_a = FreePort();
    const int port_b = FreePort();
    REQUIRE(port_a > 0);
    REQUIRE(port_b > 0);
    const std::int64_t lock_key = 1822000000LL + static_cast<std::int64_t>(::getpid() % 100000);
    const std::string id_a = "b2b-a3-" + std::to_string(::getpid());
    const std::string id_b = "b2b-b3-" + std::to_string(::getpid());
    const std::string config_a = WriteFile(
        cfg_dir, "cfg_a.json", MultiConfig(root, port_a, id_a, lock_key, "info"));
    const std::string config_b = WriteFile(
        cfg_dir, "cfg_b.json", MultiConfig(root, port_b, id_b, lock_key, "debug"));

    ServerProcess server_a(ProcessOptions(PgArgs(config_a, scratch.SearchPathDsn())));
    REQUIRE(PollReadiness(port_a, 200, nullptr, 30000));

    //  ★ 直连正控：两行的 config_hash **确实不同**（否则下面的 503 可能是别的原因）。
    const auto rows = RegistryRows(scratch.SearchPathDsn());
    const std::string hash_a = RowConfigHash(rows, id_a);
    ServerProcess server_b(ProcessOptions(PgArgs(config_b, scratch.SearchPathDsn())));
    REQUIRE(server_b.http_port() == port_b);
    REQUIRE(WaitFor([&] { return !RowConfigHash(RegistryRows(scratch.SearchPathDsn()), id_b).empty(); }));
    const auto rows2 = RegistryRows(scratch.SearchPathDsn());
    const std::string hash_b = RowConfigHash(rows2, id_b);
    CAPTURE(hash_a, hash_b);
    REQUIRE_FALSE(hash_a.empty());
    REQUIRE_FALSE(hash_b.empty());
    REQUIRE(hash_a != hash_b);

    std::string body;
    const bool not_ready = PollReadiness(port_b, 503, &body, 30000);
    CAPTURE(server_a.DumpLog(), server_b.DumpLog(), body);
    REQUIRE(not_ready);
    REQUIRE(body.find(id_a) != std::string::npos);
    REQUIRE(body.find("config_hash") != std::string::npos);
  }

  SECTION("正控（R16）：配置逐字一致（只有端口/实例 id 不同）→ 两个都 ready") {
    ScratchSchema scratch(ScratchName("cfg_ok"));
    scratch.ApplyMigrations();
    scratch.RecordVersion(fss::infra::kExpectedSchemaVersion, "001_init.sql");
    const int port_a = FreePort();
    const int port_b = FreePort();
    REQUIRE(port_a > 0);
    REQUIRE(port_b > 0);
    const std::int64_t lock_key = 1823000000LL + static_cast<std::int64_t>(::getpid() % 100000);
    const std::string id_a = "b2b-a3ok-" + std::to_string(::getpid());
    const std::string id_b = "b2b-b3ok-" + std::to_string(::getpid());
    const std::string config_a = WriteFile(
        cfg_dir, "ok_a.json", MultiConfig(root, port_a, id_a, lock_key, "info"));
    const std::string config_b = WriteFile(
        cfg_dir, "ok_b.json", MultiConfig(root, port_b, id_b, lock_key, "info"));

    ServerProcess server_a(ProcessOptions(PgArgs(config_a, scratch.SearchPathDsn())));
    REQUIRE(PollReadiness(port_a, 200, nullptr, 30000));
    ServerProcess server_b(ProcessOptions(PgArgs(config_b, scratch.SearchPathDsn())));
    std::string body_b;
    const bool ready = PollReadiness(port_b, 200, &body_b, 30000);
    CAPTURE(server_a.DumpLog(), server_b.DumpLog(), body_b);
    REQUIRE(ready);
    REQUIRE(body_b == "File service is ready");
  }

  SECTION("服务版本不兼容（FSS_SERVICE_VERSION_OVERRIDE）→ 503，原因指出版本") {
    ScratchSchema scratch(ScratchName("ver"));
    scratch.ApplyMigrations();
    scratch.RecordVersion(fss::infra::kExpectedSchemaVersion, "001_init.sql");
    const int port_a = FreePort();
    const int port_b = FreePort();
    REQUIRE(port_a > 0);
    REQUIRE(port_b > 0);
    const std::int64_t lock_key = 1824000000LL + static_cast<std::int64_t>(::getpid() % 100000);
    const std::string id_a = "b2b-a3v-" + std::to_string(::getpid());
    const std::string id_b = "b2b-b3v-" + std::to_string(::getpid());
    const std::string config_a = WriteFile(
        cfg_dir, "ver_a.json", MultiConfig(root, port_a, id_a, lock_key, "info"));
    const std::string config_b = WriteFile(
        cfg_dir, "ver_b.json", MultiConfig(root, port_b, id_b, lock_key, "info"));

    ServerProcess server_a(ProcessOptions(PgArgs(config_a, scratch.SearchPathDsn())));
    REQUIRE(PollReadiness(port_a, 200, nullptr, 30000));

    //  B：配置逐字相同，只有服务版本不同（测试接缝）⇒ 只能由版本判据把住。
    ServerProcess server_b(ProcessOptions(PgArgs(config_b, scratch.SearchPathDsn()),
                                          {{"FSS_SERVICE_VERSION_OVERRIDE", "9.9.9"}}));
    REQUIRE(server_b.http_port() == port_b);
    std::string body;
    const bool not_ready = PollReadiness(port_b, 503, &body, 30000);
    CAPTURE(server_a.DumpLog(), server_b.DumpLog(), body);
    REQUIRE(not_ready);
    REQUIRE(body.find(id_a) != std::string::npos);
    REQUIRE(body.find("版本") != std::string::npos);
    REQUIRE(body.find("9.9.9") != std::string::npos);
  }
}

// =============================================================================
//  ④ stale peer：live 窗口内仍被检查；过窗后 stale 行被忽略 → ready（正控）
// =============================================================================
TEST_CASE("★ B2b-4：kill -9 的对端在 live 窗口内仍被检查；过窗后 stale 行被忽略 → ready",
          "[pg][infra][b2b]") {
  ScratchSchema scratch(ScratchName("stale"));
  scratch.ApplyMigrations();
  scratch.RecordVersion(fss::infra::kExpectedSchemaVersion, "001_init.sql");
  TempDir cfg_dir("b2b_stale_cfg");
  TempDir data_dir("b2b_stale_data");
  const std::string root = data_dir.child("store");
  const int port_a = FreePort();
  const int port_b = FreePort();
  REQUIRE(port_a > 0);
  REQUIRE(port_b > 0);
  const std::int64_t lock_key = 1825000000LL + static_cast<std::int64_t>(::getpid() % 100000);
  const std::string id_a = "b2b-a4-" + std::to_string(::getpid());
  const std::string id_b = "b2b-b4-" + std::to_string(::getpid());
  const std::string config_a = WriteFile(
      cfg_dir, "stale_a.json", MultiConfig(root, port_a, id_a, lock_key));
  const std::string config_b = WriteFile(
      cfg_dir, "stale_b.json", MultiConfig(root, port_b, id_b, lock_key));

  ServerProcess server_a(ProcessOptions(PgArgs(config_a, scratch.SearchPathDsn())));
  ServerProcess server_b(ProcessOptions(PgArgs(config_b, scratch.SearchPathDsn())));
  REQUIRE(PollReadiness(port_a, 200, nullptr, 30000));
  REQUIRE(PollReadiness(port_b, 200, nullptr, 30000));
  REQUIRE(WaitFor([&] { return ProbeFileVisible(root, id_b); }, 100, 50));

  //  ---- 真实 `kill -9`：B 的心跳行立刻变"不再更新"，但仍在 live 窗口内 ----
  const int kill_rc = std::system(("kill -9 " + server_b.pid()).c_str());
  REQUIRE(kill_rc == 0);
  REQUIRE(WaitFor([&] { return !ProcessAlive(server_b.pid()); }, 200, 25));

  //  ---- 让 B 的探针**不可见**（模拟"它看到的挂载消失了"）：此时 B 的行还在窗口内，
  //       幸存者必须**先**判 not ready —— 这是"live peer 真的被检查"的判别性证据。
  std::error_code remove_error;
  REQUIRE(std::filesystem::remove(ProbePath(root, id_b), remove_error));
  REQUIRE_FALSE(ProbeFileVisible(root, id_b));

  std::string not_ready_body;
  const bool saw_not_ready = PollReadiness(port_a, 503, &not_ready_body, /*timeout_ms=*/20000);
  CAPTURE(server_a.DumpLog(), not_ready_body);
  REQUIRE(saw_not_ready);
  REQUIRE(not_ready_body.find(id_b) != std::string::npos);
  REQUIRE(not_ready_body.find("共享挂载") != std::string::npos);

  //  ---- 过 live 窗口（30s）+ 一个心跳周期：stale 行被忽略 → A 回到 ready ----
  std::string ready_body;
  const bool recovered = PollReadiness(port_a, 200, &ready_body, /*timeout_ms=*/90000);
  CAPTURE(server_a.DumpLog(), ready_body);
  REQUIRE(recovered);
  REQUIRE(ready_body == "File service is ready");

  //  ★ 正控：B 的行**仍在表里**且心跳已超过 live 窗口 —— 证明"ready"来自**忽略 stale 行**，
  //    不是"行被删掉了"（运行期不做清理，只有启动期清理）。
  const auto rows = RegistryRows(scratch.SearchPathDsn());
  bool saw_stale_b = false;
  for (const auto& row : rows) {
    if (row.instance_id != id_b) continue;
    saw_stale_b = true;
    CAPTURE(row.heartbeat_age_seconds);
    REQUIRE(std::strtol(row.heartbeat_age_seconds.c_str(), nullptr, 10) >= 30);
  }
  REQUIRE(saw_stale_b);
}
