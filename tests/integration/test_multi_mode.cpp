// =============================================================================
//  B1（ADR-009 §4.4/§4.5 + §10）：`deployment.mode=multi` **真的能跑**
// =============================================================================
//  判据（本切片）：
//    ① multi 真的启动，且**真的用 PG**：真实进程上传 → 记录可从 PG 直连读回
//       （正控），而该 data dir 下**没有** SQLite 文件（负断言 + 同路径正控）；
//    ② single + `*.repository=postgres` 也成立（证明接线不是 multi-only）；
//    ③ 进程**真的持有**配置的 advisory lock（`pg_locks` 正控 + 不同键负控），
//       SIGTERM 后**轮询**到释放（不用固定 sleep）；
//    ④ 两个共键进程**恰好一个** leader：非 leader 的 `fss_gc_runs_total` **不增加**，
//       leader 的增加 —— 这是"选举真的门控了 GC"的判据；
//    ⑤ multi + 空 `deployment.instance_id` → 自动生成非空唯一 id，两个进程不同；
//       single 模式**不生成**（逐字不变）；
//    ⑥ fail-closed 负例：multi + 不可达 metadata DSN / single + postgres + 不可达 DSN /
//       `metadata.repository=mysql` → exit **78** + 可读原因 + **不绑定** HTTP 端口。
//
//  ★ 铁律（AGENTS R1 / §4.3）：
//    · **连不上 PG 就失败**，绝不静默跳过（跳过的门槛是空证据）；
//    · 每个负断言都配**同路径正控**（否则 `REQUIRE_FALSE` 可能恒真）；
//    · 一切等待都**轮询实际条件** + 有界超时，不用固定 sleep。
//
//  ★ 关于 partition：组合根的租户注册表是**静态单租户** `opendes`（`StaticPartitionRegistry`），
//    因此"每轮唯一"落在 **`file_source`**（幂等键）上：每次运行生成唯一 file_source，
//    teardown 只删**该 file_source** 的行（绝不 TRUNCATE）。这与 spec 里
//    「per-run unique partition」的意图一致（唯一化 + 只清自己的行），但按真实实现收窄措辞。
// =============================================================================
#include <catch2/catch.hpp>

#include "app_fixture.h"    // AppFixture::MakeRecord
#include "http_fixture.h"   // Authed / HttpDo / TargetOf
#include "raw_http.h"
#include "server_process.h"
#include "temp_dir.h"

#include "common/json/json.h"
#include "domain/model/file_metadata.h"
#include "infra/postgres/pg_connection.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
using fss::test::Authed;
using fss::test::HttpDo;
using fss::test::ProcessOutcome;
using fss::test::RunServerForExit;
using fss::test::ServerProcess;
using fss::test::ServerProcessOptions;
using fss::test::TargetOf;
using fss::test::TempDir;

//  本机 dev PG（`scripts/dev_postgres.sh`）的默认 DSN；目标库 12.6 用 FSS_PG_DSN 覆盖。
constexpr const char* kDefaultDsn = "postgresql://fss@127.0.0.1:15432/fss";
constexpr const char* kPartition = "opendes";

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

bool PortConnectable(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool connected = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  ::close(fd);
  return connected;
}

std::string WriteFile(const TempDir& dir, const std::string& name, const std::string& content) {
  const std::string path = dir.child(name);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.good());
  out << content;
  out.close();
  return path;
}

struct HttpReply {
  bool transport_ok = false;
  int status = 0;
  std::string body;
};

HttpReply HttpGet(int port, const std::string& target) {
  HttpReply reply;
  fss::test::RawClient client(port, /*tcp_nodelay=*/true);
  if (!client.Connect()) return reply;
  if (!client.SendRequest("GET", target, {}, "")) return reply;
  const auto response = client.ReadResponse(10000);
  if (!response.has_value()) return reply;
  reply.transport_ok = true;
  reply.status = response->status;
  reply.body = response->body;
  return reply;
}

bool WaitReady(int port, int attempts = 200) {
  for (int i = 0; i < attempts; ++i) {
    const auto reply = HttpGet(port, "/api/file/v2/readiness_check");
    if (reply.transport_ok && reply.status == 200) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

//  轮询实际条件（有界），不用固定 sleep。
template <typename Predicate>
bool WaitFor(Predicate predicate, int attempts = 200, int millis = 50) {
  for (int i = 0; i < attempts; ++i) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
  }
  return predicate();
}

//  ---- 直连 PG（libpq）的判据 ----
std::unique_ptr<PgConnection> OpenProbe() {
  auto connection = PgConnection::Connect(TestPgOptions(1));
  REQUIRE(connection.ok());  // 连不上 = 失败（不是 skip）
  return std::move(connection).value();
}

std::int64_t CountMetadataRows(const std::string& file_source) {
  auto connection = OpenProbe();
  const auto result = connection->ExecParams(
      "SELECT COUNT(*) FROM file_metadata_records WHERE partition_id = $1 AND file_source = $2",
      {kPartition, file_source});
  REQUIRE(result.ok());
  REQUIRE(result.value().RowCount() == 1);
  return std::strtoll(result.value().Value(0, 0).c_str(), nullptr, 10);
}

std::int64_t CountLocationRows(const std::string& file_source, const std::string& zone) {
  auto connection = OpenProbe();
  const auto result = connection->ExecParams(
      "SELECT COUNT(*) FROM file_locations WHERE partition_id = $1 AND file_source = $2"
      " AND zone = $3",
      {kPartition, file_source, zone});
  REQUIRE(result.ok());
  REQUIRE(result.value().RowCount() == 1);
  return std::strtoll(result.value().Value(0, 0).c_str(), nullptr, 10);
}

//  `leader_election.lock_key` 的单参数 bigint 形态在 `pg_locks` 里是
//  classid = key >> 32、objid = key & 0xFFFFFFFF、objsubid = 1。
bool AdvisoryLockHeld(std::int64_t key) {
  auto connection = OpenProbe();
  const std::string classid = std::to_string((key >> 32) & 0xFFFFFFFFLL);
  const std::string objid = std::to_string(key & 0xFFFFFFFFLL);
  const auto result = connection->ExecParams(
      "SELECT COUNT(*) FROM pg_locks WHERE locktype = 'advisory' AND objsubid = 1"
      " AND classid::text = $1 AND objid::text = $2",
      {classid, objid});
  REQUIRE(result.ok());
  return std::strtoll(result.value().Value(0, 0).c_str(), nullptr, 10) > 0;
}

std::int64_t SumMetric(const std::string& metrics, const std::string& name) {
  std::int64_t total = 0;
  std::istringstream stream(metrics);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.compare(0, name.size(), name) != 0) continue;
    const char next = line.size() > name.size() ? line[name.size()] : '\0';
    if (next != '{' && next != ' ') continue;
    const auto space = line.rfind(' ');
    if (space == std::string::npos) continue;
    total += std::strtoll(line.c_str() + space + 1, nullptr, 10);
  }
  return total;
}

//  只删本次运行自己的行（按 file_source；绝不 TRUNCATE）。
void CleanupFileSource(const std::string& file_source) {
  if (file_source.empty()) return;
  auto connection = PgConnection::Connect(TestPgOptions(1));
  if (!connection.ok()) return;
  (void)connection.value()->ExecParams(
      "DELETE FROM staging_leases WHERE partition_id = $1 AND file_source = $2",
      {kPartition, file_source});
  (void)connection.value()->ExecParams(
      "DELETE FROM file_locations WHERE partition_id = $1 AND file_source = $2",
      {kPartition, file_source});
  (void)connection.value()->ExecParams(
      "DELETE FROM file_metadata_records WHERE partition_id = $1 AND file_source = $2",
      {kPartition, file_source});
}

class FileSourceGuard {
 public:
  explicit FileSourceGuard(std::string file_source) : file_source_(std::move(file_source)) {}
  ~FileSourceGuard() { CleanupFileSource(file_source_); }
  FileSourceGuard(const FileSourceGuard&) = delete;
  FileSourceGuard& operator=(const FileSourceGuard&) = delete;

 private:
  std::string file_source_;
};

//  ---- 真实进程上传（uploadURL → PUT → POST metadata），返回 file_source 与 record id ----
struct UploadedFile {
  std::string file_id;
  std::string file_source;
  std::string record_id;
};

UploadedFile UploadViaProcess(int port, const std::string& payload) {
  UploadedFile out;
  const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(upload.status == 200);
  const auto upload_json = fss::json::ParseObject(upload.body);
  REQUIRE(upload_json.ok());
  out.file_id = upload_json.value()["FileID"].get<std::string>();
  out.file_source = upload_json.value()["Location"]["FileSource"].get<std::string>();
  const std::string put_url = upload_json.value()["Location"]["SignedURL"].get<std::string>();
  REQUIRE(out.file_source.compare(0, std::string("/osdu-user/").size(), "/osdu-user/") == 0);

  const auto put = HttpDo(port, "PUT", TargetOf(put_url), Authed(), payload);
  REQUIRE(put.status == 200);

  auto record = fss::test::AppFixture::MakeRecord(out.file_source, "multi-mode.bin");
  const auto created = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(),
                              fss::json::Dump(fss::domain::ToJson(record)));
  INFO("POST metadata: " << created.body);
  REQUIRE(created.status == 201);
  const auto created_json = fss::json::ParseObject(created.body);
  REQUIRE(created_json.ok());
  out.record_id = created_json.value()["id"].get<std::string>();
  return out;
}

//  ---- 进程启动选项：存储/PG DSN 等由配置/`--set` 给出（关掉夹具的默认 env 覆盖）----
ServerProcessOptions ProcessOptions(const std::vector<std::string>& args,
                                    const std::vector<std::pair<std::string, std::string>>& env = {}) {
  ServerProcessOptions options;
  options.default_http_port = false;   // 端口必须来自配置（自签 URL 依赖它）
  options.default_grpc_port = false;
  options.expect_grpc = false;
  options.default_storage_env = false;
  options.args = args;
  options.env = env;
  return options;
}

//  `deployment.mode=multi`（或 single）的**完整**配置：schema 跨字段要求全部满足。
std::string ScenarioConfig(const std::string& mode, const std::string& root, int port,
                           std::string instance_id, bool leader_enabled, std::int64_t lock_key,
                           bool gc_enabled) {
  std::ostringstream json;
  json << "{\n"
       << "  \"deployment\": {\"mode\": \"" << mode << "\", \"instance_id\": \""
       << instance_id << "\"},\n"
       << "  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " << port
       << "}, \"grpc\": {\"enabled\": false}},\n"
       << "  \"storage\": {\"posix\": {\"root\": \"" << root
       << "\", \"shared_mount_required\": true}},\n"
       << "  \"metadata\": {\"repository\": \"postgres\", \"sqlite\": {\"path\": \"" << root
       << "/metadata.db\"}},\n"
       << "  \"location\": {\"repository\": \"postgres\", \"sqlite\": {\"path\": \"" << root
       << "/location.db\"}},\n"
       << "  \"leases\": {\"enabled\": true},\n"
       << "  \"leader_election\": {\"enabled\": " << (leader_enabled ? "true" : "false")
       << ", \"lock_key\": " << lock_key << "},\n"
       << "  \"gc\": {\"enabled\": " << (gc_enabled ? "true" : "false")
       << ", \"dry_run\": true, \"require_lease_expiry\": true, \"interval_seconds\": 1},\n"
       << "  \"self_signed\": {\"signing_key\": \"multi-mode-test\", \"public_base_url\": "
          "\"http://127.0.0.1:"
       << port << "/api/file\"},\n"
       << "  \"auth\": {\"mode\": \"disabled\"}\n"
       << "}\n";
  return json.str();
}

std::vector<std::string> PgArgs(const std::string& config, const std::string& metadata_dsn,
                                const std::string& location_dsn) {
  return {"--config", config,
          "--set", "metadata.postgres.dsn=" + metadata_dsn,
          "--set", "location.postgres.dsn=" + location_dsn};
}

//  从横幅里取 `  instance id    : <值>` 的 `<值>`（到全角括号或行尾为止）。
std::string InstanceIdOf(const std::string& banner) {
  const std::string prefix = "  instance id    : ";
  const auto pos = banner.find(prefix);
  if (pos == std::string::npos) return {};
  const auto start = pos + prefix.size();
  const auto end = banner.find_first_of("\n（", start);
  return banner.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

}  // namespace

// =============================================================================
//  ① multi 真的启动，且**真的用 PG**（记录可从 PG 直连读回；SQLite 文件不存在）
// =============================================================================
TEST_CASE("★ B1-1：deployment.mode=multi 真的启动并用 PG 落库（真实进程 + 直连 PG 正控）",
          "[pg][infra][b1]") {
  TempDir cfg_dir("b1_multi_cfg");
  TempDir data_dir("b1_multi_data");
  const std::string root = data_dir.child("store");
  const int port = FreePort();
  REQUIRE(port > 0);
  const std::string config = WriteFile(
      cfg_dir, "multi.json",
      ScenarioConfig("multi", root, port, "b1-multi-1", /*leader_enabled=*/true,
                     /*lock_key=*/1800000001, /*gc_enabled=*/true));

  ServerProcess server(ProcessOptions(PgArgs(config, TestDsn(), TestDsn())));
  REQUIRE(server.http_port() == port);
  CAPTURE(server.DumpLog());
  REQUIRE(WaitReady(port));

  const auto uploaded = UploadViaProcess(port, "b1-multi-payload");
  FileSourceGuard guard(uploaded.file_source);

  //  ★ 正控：记录真的在 PG 里（而不是"上传伪造成功"）
  REQUIRE(CountMetadataRows(uploaded.file_source) == 1);
  REQUIRE(CountLocationRows(uploaded.file_source, "PERSISTENT") == 1);

  //  ★ 负断言 + 同路径正控：SQLite 库文件**不存在**，但同一 data dir 真的被用了
  //    （`blobs` 目录存在 ⇒ 路径解析正确 ⇒ "不存在"不是路径写错导致的恒真）。
  REQUIRE(std::filesystem::exists(root + "/blobs"));
  REQUIRE_FALSE(std::filesystem::exists(root + "/location.db"));
  REQUIRE_FALSE(std::filesystem::exists(root + "/metadata.db"));

  //  横幅必须如实说明后端与 leader 状态（可运维）
  const std::string banner = server.DumpLog();
  REQUIRE(banner.find("repositories   : metadata=postgres") != std::string::npos);
  REQUIRE(banner.find("location=postgres") != std::string::npos);
  REQUIRE(banner.find("lease backend  : postgres") != std::string::npos);
  REQUIRE(banner.find("启动时本实例=leader") != std::string::npos);
}

// =============================================================================
//  ② single + postgres 仓储也成立（证明接线不是 multi-only）
// =============================================================================
TEST_CASE("★ B1-2：deployment.mode=single + *.repository=postgres 也落 PG（不是 multi-only）",
          "[pg][infra][b1]") {
  TempDir cfg_dir("b1_single_cfg");
  TempDir data_dir("b1_single_data");
  const std::string root = data_dir.child("store");
  const int port = FreePort();
  REQUIRE(port > 0);
  const std::string config = WriteFile(
      cfg_dir, "single.json",
      ScenarioConfig("single", root, port, "b1-single-1", /*leader_enabled=*/false,
                     /*lock_key=*/1800000002, /*gc_enabled=*/false));

  ServerProcess server(ProcessOptions(PgArgs(config, TestDsn(), TestDsn())));
  REQUIRE(server.http_port() == port);
  CAPTURE(server.DumpLog());
  REQUIRE(WaitReady(port));

  const auto uploaded = UploadViaProcess(port, "b1-single-payload");
  FileSourceGuard guard(uploaded.file_source);

  REQUIRE(CountMetadataRows(uploaded.file_source) == 1);
  REQUIRE(CountLocationRows(uploaded.file_source, "PERSISTENT") == 1);
  REQUIRE(std::filesystem::exists(root + "/blobs"));
  REQUIRE_FALSE(std::filesystem::exists(root + "/location.db"));
  REQUIRE_FALSE(std::filesystem::exists(root + "/metadata.db"));
  //  leader election 未启用 → 横幅如实说明（single 下不需要选举）
  REQUIRE(server.DumpLog().find("leader         : 未启用") != std::string::npos);
}

// =============================================================================
//  ③ 进程真的持有配置的 advisory lock；SIGTERM 后**轮询**到释放
// =============================================================================
TEST_CASE("★ B1-3：进程真的持有 leader_election.lock_key 的 advisory lock，退出后释放",
          "[pg][infra][b1]") {
  TempDir cfg_dir("b1_lock_cfg");
  TempDir data_dir("b1_lock_data");
  const std::string root = data_dir.child("store");
  const int port = FreePort();
  REQUIRE(port > 0);
  const std::int64_t key = 1800000003;
  const std::string config = WriteFile(
      cfg_dir, "lock.json",
      ScenarioConfig("multi", root, port, "b1-lock-1", /*leader_enabled=*/true, key,
                     /*gc_enabled=*/false));

  ServerProcess server(ProcessOptions(PgArgs(config, TestDsn(), TestDsn())));
  REQUIRE(WaitReady(port));
  REQUIRE(server.alive());

  //  ★ 负控：查询本身有区分力 —— 另一个（未使用的）键必须**不**被持有。
  REQUIRE_FALSE(AdvisoryLockHeld(key + 1));
  //  ★ 正控：轮询到配置的键真的出现在 pg_locks 里（而不是靠"进程说自己是 leader"）。
  REQUIRE(WaitFor([&] { return AdvisoryLockHeld(key); }, /*attempts=*/100, /*millis=*/50));

  //  SIGTERM → 退出路径释放连接 → PG 释放会话级锁（轮询，不用固定 sleep）。
  REQUIRE(std::system(("kill " + server.pid() + " 2>/dev/null").c_str()) == 0);
  REQUIRE(WaitFor([&] { return !AdvisoryLockHeld(key); }, /*attempts=*/200, /*millis=*/50));
}

// =============================================================================
//  ④ 两个共键进程恰好一个 leader：非 leader 的 fss_gc_runs_total 不增加
// =============================================================================
TEST_CASE("★ B1-4：两个进程共享 lock_key → 恰好一个 leader，非 leader 的 GC 计数不增加",
          "[pg][infra][b1]") {
  TempDir cfg_a_dir("b1_two_a_cfg");
  TempDir cfg_b_dir("b1_two_b_cfg");
  TempDir data_a_dir("b1_two_a_data");
  TempDir data_b_dir("b1_two_b_data");
  const int port_a = FreePort();
  const int port_b = FreePort();
  REQUIRE(port_a > 0);
  REQUIRE(port_b > 0);
  REQUIRE(port_a != port_b);
  const std::int64_t key = 1800000004;

  //  ★ A 先启动并拿到锁；B 后启动。判据不依赖"谁先跑第一轮 GC"：A 起锁在 B 启动之前，
  //    且窗口内**反复轮询** B 的计数恒为 0。
  const std::string config_a = WriteFile(
      cfg_a_dir, "a.json",
      ScenarioConfig("multi", data_a_dir.child("store"), port_a, "b1-two-a",
                     /*leader_enabled=*/true, key, /*gc_enabled=*/true));
  const std::string config_b = WriteFile(
      cfg_b_dir, "b.json",
      ScenarioConfig("multi", data_b_dir.child("store"), port_b, "b1-two-b",
                     /*leader_enabled=*/true, key, /*gc_enabled=*/true));

  ServerProcess server_a(ProcessOptions(PgArgs(config_a, TestDsn(), TestDsn())));
  REQUIRE(WaitReady(port_a));
  REQUIRE(WaitFor([&] { return AdvisoryLockHeld(key); }));
  REQUIRE(server_a.DumpLog().find("启动时本实例=leader") != std::string::npos);
  REQUIRE(server_a.DumpLog().find("启动时本实例=非 leader") == std::string::npos);

  ServerProcess server_b(ProcessOptions(PgArgs(config_b, TestDsn(), TestDsn())));
  REQUIRE(WaitReady(port_b));
  CAPTURE(server_a.DumpLog(), server_b.DumpLog());
  //  B 的横幅必须**说明它没成为 leader**（可读原因），且周期调度确实"已启动"
  //  —— 否则"计数为 0"可能只是 gc.enabled=false，判据就没有区分力。
  REQUIRE(server_b.DumpLog().find("启动时本实例=非 leader") != std::string::npos);
  REQUIRE(server_b.DumpLog().find("gc             : 已启动（间隔 1s") != std::string::npos);

  //  ★ 正控：leader A 的 fss_gc_runs_total 真的在增加（证明指标本身会出现）。
  bool a_increased = false;
  std::int64_t a_runs = 0;
  for (int attempt = 0; attempt < 300; ++attempt) {
    const auto metrics = HttpGet(port_a, "/metrics");
    if (metrics.status == 200) {
      a_runs = SumMetric(metrics.body, "fss_gc_runs_total");
      if (a_runs > 0) {
        a_increased = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CAPTURE(a_runs);
  REQUIRE(a_increased);

  //  ★ 窗口内反复轮询 B：必须**始终**没有 GC 运行（缺失 = 0 次）。
  std::int64_t b_max = 0;
  for (int attempt = 0; attempt < 30; ++attempt) {
    const auto metrics = HttpGet(port_b, "/metrics");
    if (metrics.status == 200) {
      b_max = std::max(b_max, SumMetric(metrics.body, "fss_gc_runs_total"));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CAPTURE(b_max, a_runs);
  REQUIRE(b_max == 0);

  //  A 的计数还在持续增加（不是只跑了一轮就停了）
  const auto a_before = SumMetric(HttpGet(port_a, "/metrics").body, "fss_gc_runs_total");
  REQUIRE(WaitFor(
      [&] {
        return SumMetric(HttpGet(port_a, "/metrics").body, "fss_gc_runs_total") > a_before;
      },
      /*attempts=*/100, /*millis=*/100));
}

// =============================================================================
//  ⑤ multi + 空/未配置 instance_id → 自动生成唯一 id；single 不生成
// =============================================================================
TEST_CASE("★ B1-5：multi 下空/未配置 deployment.instance_id 自动生成唯一 id（两个进程不同）",
          "[pg][infra][b1]") {
  TempDir cfg_a_dir("b1_id_a_cfg");
  TempDir data_a_dir("b1_id_a_data");
  TempDir cfg_b_dir("b1_id_b_cfg");
  TempDir data_b_dir("b1_id_b_data");
  const int port_a = FreePort();
  const int port_b = FreePort();
  REQUIRE(port_a > 0);
  REQUIRE(port_b > 0);

  //  A：显式置空（`--set deployment.instance_id=`）；B：**完全未配置**（有效默认 local）。
  const std::string config_a = WriteFile(
      cfg_a_dir, "a.json",
      ScenarioConfig("multi", data_a_dir.child("store"), port_a, "", /*leader_enabled=*/true,
                     /*lock_key=*/1800000005, /*gc_enabled=*/false));
  const std::string config_b = WriteFile(
      cfg_b_dir, "b.json",
      ScenarioConfig("multi", data_b_dir.child("store"), port_b, "local",
                     /*leader_enabled=*/true, /*lock_key=*/1800000006, /*gc_enabled=*/false));

  ServerProcess server_a(ProcessOptions(PgArgs(config_a, TestDsn(), TestDsn())));
  ServerProcess server_b(ProcessOptions(PgArgs(config_b, TestDsn(), TestDsn())));
  REQUIRE(WaitReady(port_a));
  REQUIRE(WaitReady(port_b));

  const std::string id_a = InstanceIdOf(server_a.DumpLog());
  const std::string id_b = InstanceIdOf(server_b.DumpLog());
  CAPTURE(id_a, id_b, server_a.DumpLog(), server_b.DumpLog());
  REQUIRE_FALSE(id_a.empty());
  REQUIRE_FALSE(id_b.empty());
  REQUIRE(id_a != "local");
  REQUIRE(id_b != "local");
  REQUIRE(id_a != id_b);  // ★ M1：两个实例绝不能共享同一标识
  REQUIRE(server_a.DumpLog().find("自动生成唯一 id") != std::string::npos);
  REQUIRE(server_b.DumpLog().find("自动生成唯一 id") != std::string::npos);

  //  ★ 正例对照（R16）：single 模式**不生成** id（逐字保持不变）。
  TempDir cfg_s_dir("b1_id_s_cfg");
  TempDir data_s_dir("b1_id_s_data");
  const int port_s = FreePort();
  const std::string config_s = WriteFile(
      cfg_s_dir, "s.json",
      ScenarioConfig("single", data_s_dir.child("store"), port_s, "local",
                     /*leader_enabled=*/false, /*lock_key=*/1800000007, /*gc_enabled=*/false));
  ServerProcess server_s(ProcessOptions(PgArgs(config_s, TestDsn(), TestDsn())));
  REQUIRE(WaitReady(port_s));
  const std::string id_s = InstanceIdOf(server_s.DumpLog());
  CAPTURE(id_s, server_s.DumpLog());
  REQUIRE(id_s == "local");
  REQUIRE(server_s.DumpLog().find("自动生成唯一 id") == std::string::npos);
}

// =============================================================================
//  ⑥ fail-closed 负例：exit 78 + 可读原因 + 不绑定 HTTP 端口
// =============================================================================
TEST_CASE("★ B1-6：fail-closed 负例（不可达 DSN / 非法 repository）→ exit 78 且不绑定端口",
          "[pg][infra][b1]") {
  const std::string unreachable = "postgresql://fss@127.0.0.1:1/fss?connect_timeout=2";

  SECTION("multi + 不可达 metadata.postgres.dsn → exit 78") {
    TempDir cfg_dir("b1_neg_meta_cfg");
    TempDir data_dir("b1_neg_meta_data");
    const int port = FreePort();
    REQUIRE(port > 0);
    const std::string config = WriteFile(
        cfg_dir, "meta_bad.json",
        ScenarioConfig("multi", data_dir.child("store"), port, "b1-neg-meta",
                       /*leader_enabled=*/true, /*lock_key=*/1800000008, /*gc_enabled=*/false));
    //  location DSN 有效（先建位置仓储成功），metadata DSN 不可达 → 必须在 metadata 处失败。
    const ProcessOutcome outcome =
        RunServerForExit(PgArgs(config, unreachable, TestDsn()),
                         {}, /*timeout_seconds=*/30);
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("拒绝启动") != std::string::npos);
    REQUIRE(outcome.output.find("metadata") != std::string::npos);
    REQUIRE(outcome.output.find("已启动") == std::string::npos);
    REQUIRE_FALSE(PortConnectable(port));
  }

  SECTION("single + metadata.repository=postgres + 不可达 DSN → exit 78") {
    TempDir cfg_dir("b1_neg_single_cfg");
    TempDir data_dir("b1_neg_single_data");
    const int port = FreePort();
    REQUIRE(port > 0);
    const std::string config = WriteFile(
        cfg_dir, "single_bad.json",
        ScenarioConfig("single", data_dir.child("store"), port, "b1-neg-single",
                       /*leader_enabled=*/false, /*lock_key=*/1800000009, /*gc_enabled=*/false));
    const ProcessOutcome outcome =
        RunServerForExit(PgArgs(config, unreachable, TestDsn()),
                         {}, /*timeout_seconds=*/30);
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("拒绝启动") != std::string::npos);
    REQUIRE(outcome.output.find("metadata.repository=postgres") != std::string::npos);
    REQUIRE(outcome.output.find("已启动") == std::string::npos);
    REQUIRE_FALSE(PortConnectable(port));
  }

  SECTION("metadata.repository=mysql（非法）→ exit 78（schema 枚举）") {
    TempDir data_dir("b1_neg_mysql_data");
    const int port = FreePort();
    REQUIRE(port > 0);
    const ProcessOutcome outcome = RunServerForExit(
        {"--set", "metadata.repository=mysql", "--set", "server.http.bind=127.0.0.1",
         "--set", "server.http.port=" + std::to_string(port), "--set",
         "storage.posix.root=" + data_dir.child("store")},
        {}, /*timeout_seconds=*/30);
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("metadata.repository") != std::string::npos);
    REQUIRE(outcome.output.find("已启动") == std::string::npos);
    REQUIRE_FALSE(PortConnectable(port));
  }

  //  ★ 正控（R1）：同一段端口检查对**能启动**的配置必须为真，否则上面的
  //    `REQUIRE_FALSE(PortConnectable(port))` 可能是"探测器恒 false"。
  SECTION("正控：合法 multi 配置 → 端口真的可连（证明上面的负断言有区分力）") {
    TempDir cfg_dir("b1_neg_pos_cfg");
    TempDir data_dir("b1_neg_pos_data");
    const int port = FreePort();
    REQUIRE(port > 0);
    const std::string config = WriteFile(
        cfg_dir, "pos.json",
        ScenarioConfig("multi", data_dir.child("store"), port, "b1-neg-pos",
                       /*leader_enabled=*/true, /*lock_key=*/1800000010, /*gc_enabled=*/false));
    ServerProcess server(ProcessOptions(PgArgs(config, TestDsn(), TestDsn())));
    REQUIRE(WaitReady(port));
    REQUIRE(PortConnectable(port));
  }
}
