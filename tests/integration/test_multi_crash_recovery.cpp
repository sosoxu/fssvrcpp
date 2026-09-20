// =============================================================================
//  C9.26：**两个真实 `fss_server` 进程** + **真实 `kill -9`** 的多实例崩溃回收 E2E
// =============================================================================
//  这是 ADR-009 §4.2/§4.3（原子领取 + 在途租约 + 崩溃回收）在**真实进程**上的判据：
//  C1/C2 的进程内集成测试（`test_postgres_lease_lifecycle.cpp`）用"直接放弃"模拟崩溃，
//  而"另一个 OS 进程被 SIGKILL 之后，幸存实例真的会接管领导者并回收它留下的 claiming 行"
//  只有在**两个真实二进制**上才能证明（组合根 R12：具体实现只在 `src/main/` 里创建）。
//
//  ---- 场景（每一步都是**轮询到的真实条件**，没有一处用固定 sleep 赌时序）----
//    ① 两个进程就绪；`pg_locks` 里该 `leader_election.lock_key` **恰好一行**（恰好一个
//       leader），且 A（先启动者）的横幅自认 leader、B 自认非 leader；
//    ② 经 A 的 HTTP：`uploadURL` → `PUT` 字节 → 后台线程发起 `createMetadata`
//       （A 上设置了 `FSS_CLAIM_HOLD_MS`，于是它在"原子领取成功 + 在途租约就绪"之后、
//        复制之前**停住**）；
//    ③ 轮询 PG 直到 `state='claiming'` 的行存在（**正控**：我们确实在窗口里），
//       并断言语义对象真的落在磁盘上（不是"上传伪造成功"）；
//    ④ `kill -9` A；立刻断言 claiming 行**还在**（崩溃没有替我们"清理"）；
//    ⑤ B 接管（轮询 `pg_locks` 换了个 backend pid）后，**在租约到期之前**在 B 上强制
//       一轮按需 GC：`fss_gc_reclaimed_claiming_total` 不变、行仍在（**守卫的正控**）；
//    ⑥ 等租约到期 → 轮询 B 的指标 ≥1 → claiming 行消失、staging 孤儿对象被收、位置记录被删；
//    ⑦ 同一个 `(partition, file_source)` 重新走一遍（`uploadURL`(+恢复位置记录) → PUT →
//       `createMetadata`）→ **201**、真的发生了一次新的复制、记录可读、SHA-256 与重传字节一致。
//
//  ---- 共享存储的**诚实标注**（不要外推）----
//    本用例的"共享存储"是**同一台机器上的一个本地目录被两个进程共同使用**，
//    **不是 NFS**（也没有任何网络文件系统语义参与）。因此本用例**不能**替代 C9.27：
//    NFS 的锁/缓存/rename/`syncfs` 语义仍**未验证**（见 docs/phase-status.md 的未验证清单）。
//
//  ---- 配置上的一个刻意选择 ----
//    `leases.time_source=local`（而不是默认的 `database`）：本用例的两条时间线都必须来自
//    **同一台机器的本地钟** —— 租约到期（`ClaimExpired`）与 claiming 行的年龄护栏
//    （`ReclaimStaleClaiming` 的 `created_at <= now - ttl`）。目标 PG 12.6 跑在**另一台
//    主机**上，用 `database` 会让"租约到期"与"年龄够老"分别落在两个钟上，从而把判据
//    变成时钟偏移的函数。`local` 让两处都用组合根自己的 `IClock`（组合根已支持，C2 交付）。
//
//  ---- R1（自证对照）----
//    本用例的三条"否决式/守卫式"判据都做了**注入自证**（见 docs/test-evidence/phase10.md §20）：
//      ① 关掉 `FSS_CLAIM_HOLD_MS` 的阻塞 → 步骤③"claiming 行存在"必须失败；
//      ② GC 不调用 `ReclaimStaleClaiming` → 步骤⑥"到期后被回收"必须失败；
//      ③ GC 无视租约/年龄门（立刻回收）→ 步骤⑤"到期前不回收"必须失败；
//      ④ 领导者永不接管 → 步骤⑥的接管/回收必须失败。
//    注入残留以源码/测试树里不再出现注入标记串为空为准（见证据 §20 的 grep 输出）。
//
//  ★ 连不上 PG 就失败（REQUIRE），绝不静默跳过；teardown 删本用例自己的行 + 删临时目录
//    （残留必须为 0）。
// =============================================================================
#include <catch2/catch.hpp>

#include "app_fixture.h"    // AppFixture::MakeRecord
#include "http_fixture.h"   // Authed / HttpDo / TargetOf
#include "raw_http.h"
#include "server_process.h"
#include "temp_dir.h"

#include "common/crypto/crypto.h"
#include "common/json/json.h"
#include "domain/model/file_metadata.h"
#include "infra/location/postgres/postgres_location_repository.h"
#include "infra/postgres/pg_connection.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
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
using fss::test::ServerProcess;
using fss::test::ServerProcessOptions;
using fss::test::TargetOf;

//  本机 dev PG（`scripts/dev_postgres.sh`）的默认 DSN；目标库 12.6 用 FSS_PG_DSN 覆盖。
constexpr const char* kDefaultDsn = "postgresql://fss@127.0.0.1:15432/fss";
constexpr const char* kPartition = "opendes";
//  `FSS_CLAIM_HOLD_MS`：足够长，保证"轮询到 claiming 行 → kill -9"有充足余量；
//  同时又短于测试超时（真正的窗口由接缝创建，而不是靠运气）。
constexpr std::int64_t kClaimHoldMillis = 15000;
//  租约 TTL：崩溃后 survivor 必须在**未到期前**观察到一轮 GC（步骤⑤），
//  到期后回收（步骤⑥）。8s 给"接管 + 一轮 GC"留出数秒余量。
constexpr std::int64_t kLeaseTtlSeconds = 8;
constexpr std::int64_t kLeaseRenewSeconds = 1;

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

std::unique_ptr<PgConnection> OpenProbe() {
  auto connection = PgConnection::Connect(TestPgOptions(1));
  REQUIRE(connection.ok());  // 连不上 = 失败（不是 skip）
  return std::move(connection).value();
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

std::string WriteFile(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.good());
  out << content;
  out.close();
  return path;
}

//  轮询实际条件（有界），不用固定 sleep（AGENTS §4.3）。
template <typename Predicate>
bool WaitFor(Predicate predicate, int attempts = 200, int millis = 50) {
  for (int i = 0; i < attempts; ++i) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
  }
  return predicate();
}

//  ---- HTTP（主线程用；探测/就绪检查不得因连接失败而中止测试的去向判断）----
struct HttpReply {
  bool transport_ok = false;
  int status = 0;
  std::string body;
};

HttpReply HttpGet(int port, const std::string& target, int timeout_ms = 10000) {
  HttpReply reply;
  fss::test::RawClient client(port, /*tcp_nodelay=*/true);
  if (!client.Connect()) return reply;
  if (!client.SendRequest("GET", target, {}, "")) return reply;
  const auto response = client.ReadResponse(timeout_ms);
  if (!response.has_value()) return reply;
  reply.transport_ok = true;
  reply.status = response->status;
  reply.body = response->body;
  return reply;
}

bool WaitReady(int port, int attempts = 300) {
  for (int i = 0; i < attempts; ++i) {
    const auto reply = HttpGet(port, "/api/file/v2/readiness_check");
    if (reply.transport_ok && reply.status == 200) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
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

std::int64_t MetricOf(int port, const std::string& name) {
  const auto reply = HttpGet(port, "/metrics");
  if (!reply.transport_ok || reply.status != 200) return 0;
  return SumMetric(reply.body, name);
}

//  ---- 直连 PG 的判据（每条都显式断言"查询本身成功"）----
std::int64_t ScalarInt(const std::string& sql, const std::vector<PgConnection::Param>& params) {
  auto connection = OpenProbe();
  const auto result = connection->ExecParams(sql, params);
  REQUIRE(result.ok());
  REQUIRE(result.value().RowCount() == 1);
  return std::strtoll(result.value().Value(0, 0).c_str(), nullptr, 10);
}

std::int64_t CountMetadataRows(const std::string& file_source, const std::string& state) {
  return ScalarInt(
      "SELECT COUNT(*) FROM file_metadata_records"
      " WHERE partition_id = $1 AND file_source = $2 AND state = $3",
      {kPartition, file_source, state});
}

std::int64_t CountLocationRows(const std::string& file_source) {
  return ScalarInt(
      "SELECT COUNT(*) FROM file_locations WHERE partition_id = $1 AND file_source = $2",
      {kPartition, file_source});
}

struct LocationRow {
  int rows = 0;
  std::string file_id;
  std::string container;
  std::string object_key;
  std::string zone;
};

LocationRow QueryLocation(const std::string& file_source) {
  LocationRow row;
  auto connection = OpenProbe();
  const auto result = connection->ExecParams(
      "SELECT file_id, container, object_key, zone FROM file_locations"
      " WHERE partition_id = $1 AND file_source = $2",
      {kPartition, file_source});
  REQUIRE(result.ok());
  row.rows = result.value().RowCount();
  if (row.rows > 0) {
    row.file_id = result.value().Value(0, 0);
    row.container = result.value().Value(0, 1);
    row.object_key = result.value().Value(0, 2);
    row.zone = result.value().Value(0, 3);
  }
  return row;
}

struct LeaseRow {
  int rows = 0;
  std::string owner;
  std::int64_t expires_epoch_seconds = 0;
};

LeaseRow QueryLease(const std::string& file_source) {
  LeaseRow row;
  auto connection = OpenProbe();
  const auto result = connection->ExecParams(
      "SELECT owner, extract(epoch from expires_at)::bigint FROM staging_leases"
      " WHERE partition_id = $1 AND file_source = $2",
      {kPartition, file_source});
  REQUIRE(result.ok());
  row.rows = result.value().RowCount();
  if (row.rows > 0) {
    row.owner = result.value().Value(0, 0);
    row.expires_epoch_seconds = std::strtoll(result.value().Value(0, 1).c_str(), nullptr, 10);
  }
  return row;
}

//  `leader_election.lock_key` 的单参数 bigint 形态在 `pg_locks` 里是
//  classid = key >> 32、objid = key & 0xFFFFFFFF、objsubid = 1（与 test_multi_mode 同源）。
std::int64_t AdvisoryLockRows(std::int64_t key) {
  const std::string classid = std::to_string((key >> 32) & 0xFFFFFFFFLL);
  const std::string objid = std::to_string(key & 0xFFFFFFFFLL);
  return ScalarInt(
      "SELECT COUNT(*) FROM pg_locks WHERE locktype = 'advisory' AND objsubid = 1"
      " AND classid::text = $1 AND objid::text = $2",
      {classid, objid});
}

//  失败诊断用：把该 file_source 的**所有**元数据行拼成 `state/created_at;…`（空 = `<none>`）。
std::string MetadataDigest(const std::string& file_source) {
  auto connection = OpenProbe();
  const auto result = connection->ExecParams(
      "SELECT state || '/' || extract(epoch from created_at)::bigint"
      " FROM file_metadata_records WHERE partition_id = $1 AND file_source = $2"
      " ORDER BY version",
      {kPartition, file_source});
  REQUIRE(result.ok());
  std::string out;
  for (int i = 0; i < result.value().RowCount(); ++i) {
    out += result.value().Value(i, 0);
    out += ';';
  }
  return out.empty() ? std::string("<none>") : out;
}

//  持有该 advisory lock 的 backend pid（0 = 没人持有）。
std::int64_t AdvisoryLockPid(std::int64_t key) {  const std::string classid = std::to_string((key >> 32) & 0xFFFFFFFFLL);
  const std::string objid = std::to_string(key & 0xFFFFFFFFLL);
  auto connection = OpenProbe();
  const auto result = connection->ExecParams(
      "SELECT pid FROM pg_locks WHERE locktype = 'advisory' AND objsubid = 1"
      " AND classid::text = $1 AND objid::text = $2 ORDER BY pid LIMIT 1",
      {classid, objid});
  REQUIRE(result.ok());
  if (result.value().RowCount() == 0) return 0;
  return std::strtoll(result.value().Value(0, 0).c_str(), nullptr, 10);
}

//  只删本用例自己的行（按 file_source；绝不 TRUNCATE）。
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
  std::vector<std::string>* sources() { return &sources_; }
  ~FileSourceGuard() {
    for (const auto& source : sources_) CleanupFileSource(source);
  }
  FileSourceGuard(const FileSourceGuard&) = delete;
  FileSourceGuard& operator=(const FileSourceGuard&) = delete;
  FileSourceGuard() = default;

 private:
  std::vector<std::string> sources_;
};

//  build/ 下的临时目录（RAII）：共享存储根刻意放在 build 目录里（本用例的"共享"
//  是**本地目录被两个进程共用**，不是 NFS —— 见文件头）。
class BuildDirGuard {
 public:
  explicit BuildDirGuard(std::string path) : path_(std::move(path)) {
    std::filesystem::create_directories(path_);
  }
  ~BuildDirGuard() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  BuildDirGuard(const BuildDirGuard&) = delete;
  BuildDirGuard& operator=(const BuildDirGuard&) = delete;
  const std::string& str() const { return path_; }

 private:
  std::string path_;
};

//  早退时也一定 `join`（AGENTS §4.3：提前 return 留下 joinable `std::thread` → `terminate`，
//  表现为"测试崩了"而不是"判据失败"，会掩盖真正的根因）。
class ThreadJoiner {
 public:
  explicit ThreadJoiner(std::thread thread) : thread_(std::move(thread)) {}
  ~ThreadJoiner() { Join(); }
  //  幂等：显式 Join 之后再析构不会再 join（也避免二次析构的 UB）。
  void Join() {
    if (thread_.joinable()) thread_.join();
  }
  ThreadJoiner(const ThreadJoiner&) = delete;
  ThreadJoiner& operator=(const ThreadJoiner&) = delete;

 private:
  std::thread thread_;
};

//  ---- 真实进程启动选项：存储/PG DSN 等由配置/`--set` 给出（关掉夹具的默认 env 覆盖）----
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

//  `deployment.mode=multi` 的**完整**配置：schema 跨字段要求全部满足
//  （postgres 仓储 + 租约 + 选举 + shared_mount_required + require_lease_expiry）。
//  两个进程只差 `instance_id`、HTTP 端口与 `gc.enabled`（见下面 `gc_enabled` 的说明）：
//  **同一个** storage root、**同一个** lock_key。
//
//  ★ `gc_enabled`：A（将被 kill 的 leader）上**关闭周期 GC**。理由：C9.26 的判据是
//    "**幸存者**的 GC 接管后回收崩溃者的 claiming 行"。若 A 上也跑周期 GC，判据会被
//    A 自己的 GC 穿插（正常情况下它无害 —— 租约活着；但在 R1 注入"无视租约门"时，
//    A 会在 kill **之前**就回收掉自己的 claiming 行，让失败点落在步骤③而不是步骤⑤）。
//    关掉 A 的周期 GC 只去掉这个混淆项，不改变"谁最终回收"这件事（B 接管后才跑）。
std::string ScenarioConfig(const std::string& root, int port, const std::string& instance_id,
                           std::int64_t lock_key, bool gc_enabled) {
  std::ostringstream json;
  json << "{\n"
       << "  \"deployment\": {\"mode\": \"multi\", \"instance_id\": \"" << instance_id
       << "\"},\n"
       << "  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " << port
       << "}, \"grpc\": {\"enabled\": false}},\n"
       << "  \"storage\": {\"posix\": {\"root\": \"" << root
       << "\", \"shared_mount_required\": true}},\n"
       << "  \"metadata\": {\"repository\": \"postgres\", \"sqlite\": {\"path\": \"" << root
       << "/metadata.db\"}},\n"
       << "  \"location\": {\"repository\": \"postgres\", \"sqlite\": {\"path\": \"" << root
       << "/location.db\"}},\n"
       //  ★ time_source=local：租约到期与 claiming 行的年龄护栏都用组合根本地钟，
       //    这样远端 PG 12.6 的钟偏移不会把判据变成时钟的函数（见文件头）。
       << "  \"leases\": {\"enabled\": true, \"ttl_seconds\": " << kLeaseTtlSeconds
       << ", \"renew_interval_seconds\": " << kLeaseRenewSeconds
       << ", \"time_source\": \"local\"},\n"
       << "  \"leader_election\": {\"enabled\": true, \"lock_key\": " << lock_key << "},\n"
       //  dry_run=false：本用例要判"孤儿对象真的被收"，预览不算数。
       << "  \"gc\": {\"enabled\": " << (gc_enabled ? "true" : "false")
       << ", \"dry_run\": false, \"require_lease_expiry\": true,"
          " \"interval_seconds\": 1},\n"
       << "  \"self_signed\": {\"signing_key\": \"c926-test\", \"public_base_url\": "
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

//  一次上传的结果（真实 HTTP：uploadURL → PUT → POST metadata）。
struct UploadedFile {
  std::string file_id;
  std::string file_source;
  std::string record_id;
  std::string put_url;
};

//  uploadURL + PUT。`post_metadata=true` 时同步 POST metadata（201）。
UploadedFile UploadViaProcess(int port, const std::string& payload, bool post_metadata) {
  UploadedFile out;
  const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(upload.status == 200);
  const auto upload_json = fss::json::ParseObject(upload.body);
  REQUIRE(upload_json.ok());
  out.file_id = upload_json.value()["FileID"].get<std::string>();
  out.file_source = upload_json.value()["Location"]["FileSource"].get<std::string>();
  out.put_url = upload_json.value()["Location"]["SignedURL"].get<std::string>();
  REQUIRE(out.file_source.compare(0, std::string("/osdu-user/").size(), "/osdu-user/") == 0);

  const auto put = HttpDo(port, "PUT", TargetOf(out.put_url), Authed(), payload);
  REQUIRE(put.status == 200);

  if (post_metadata) {
    auto record = fss::test::AppFixture::MakeRecord(out.file_source, "c926.bin");
    const auto created = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(),
                                fss::json::Dump(fss::domain::ToJson(record)));
    INFO("POST metadata: " << created.body);
    REQUIRE(created.status == 201);
    const auto created_json = fss::json::ParseObject(created.body);
    REQUIRE(created_json.ok());
    out.record_id = created_json.value()["id"].get<std::string>();
  }
  return out;
}

//  从 `GET /v2/files/{id}/metadata` 的完整信封里取 `data.Checksum` / `ChecksumAlgorithm`。
struct ReadRecord {
  int status = 0;
  std::string file_source;
  std::string checksum;
  std::string algorithm;
};

ReadRecord ReadMetadata(int port, const std::string& record_id) {
  ReadRecord out;
  const auto reply = HttpDo(port, "GET", "/api/file/v2/files/" + record_id + "/metadata", Authed());
  out.status = reply.status;
  if (reply.status != 200) return out;
  const auto value = fss::json::ParseObject(reply.body);
  REQUIRE(value.ok());
  const auto& data = value.value()["data"];
  out.file_source =
      data["DatasetProperties"]["FileSourceInfo"]["FileSource"].get<std::string>();
  if (data.contains("Checksum")) out.checksum = data["Checksum"].get<std::string>();
  if (data.contains("ChecksumAlgorithm")) {
    out.algorithm = data["ChecksumAlgorithm"].get<std::string>();
  }
  return out;
}

std::string LowerHex(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

constexpr const char* kStagingContainer = "opendes-staging";
constexpr const char* kPersistentContainer = "opendes-persistent";

std::string ObjectPath(const std::string& root, const std::string& container,
                       const std::string& key) {
  return root + "/blobs/" + container + "/" + key;
}

//  当前正在运行的 `fss_server` 进程（按 `/proc/<pid>/exe` 的 basename 判定）。
//  用于用例开头的前置条件：**残留实例会抢走本用例的过期租约**（见 TEST_CASE 里的说明）。
std::vector<long> RunningServerPids() {
  std::vector<long> pids;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
    const std::string name = entry.path().filename().string();
    if (name.empty()) continue;
    bool digits = true;
    for (const char c : name) {
      if (c < '0' || c > '9') {
        digits = false;
        break;
      }
    }
    if (!digits) continue;
    std::error_code link_error;
    const auto exe = std::filesystem::read_symlink(entry.path() / "exe", link_error);
    if (link_error) continue;
    if (exe.filename() == "fss_server") pids.push_back(std::strtol(name.c_str(), nullptr, 10));
  }
  return pids;
}

}  // namespace

// =============================================================================
//  C9.26：唯一用例（两个真实进程 + 真实 kill -9）
// =============================================================================
TEST_CASE("★ C9.26：两个真实进程共享 PG+本地存储；kill -9 持 claiming 的实例 → survivor 接管并回收（非 NFS）",
          "[pg][infra][c926]") {
  //  ★ 前置条件（R9）：**不能有别的 `fss_server` 实例**。GC 是按 `partition` 扫描的
  //    （组合根的 `gc_partition` 固定为 `opendes`），任何一个**残留实例**的 GC 都可能
  //    先一步领取本用例的过期租约、回收本用例的 claiming 行 —— 指标落在**别人**身上，
  //    本用例会以"survivor 没回收"的形式莫名失败（实测：一个因 `terminate` 未清理的
  //    实例让本用例 5 次里失败 3 次）。把这条前置条件显式化，失败信息才可读、可修。
  {
    const auto strays = RunningServerPids();
    INFO("残留的 fss_server 进程数：" << strays.size()
                                      << "（处置：pkill -9 -x fss_server）");
    REQUIRE(strays.empty());
  }

  //  共享存储根刻意放在 build 目录（本用例的"共享"= 本地目录被两个进程共用）。
  const std::filesystem::path build_dir =
      fss::test::ServerBinaryPath().parent_path().parent_path();
  BuildDirGuard work_dir(
      (build_dir / ("fss_c926_" + std::to_string(static_cast<long>(::getpid())))).string());
  const std::string root = work_dir.str() + "/store";
  std::filesystem::create_directories(root);

  const int port_a = FreePort();
  REQUIRE(port_a > 0);
  //  每次运行用唯一 lock_key：上一轮若被强杀也不影响本轮（残留锁不阻塞）。
  const std::int64_t lock_key =
      1810000000LL + static_cast<std::int64_t>(::getpid() % 100000) +
      static_cast<std::int64_t>(std::time(nullptr) % 1000000);

  const std::string config_a = WriteFile(work_dir.str() + "/a.json",
                                         ScenarioConfig(root, port_a, "c926-a", lock_key,
                                                        /*gc_enabled=*/false));

  const std::string crashed_payload =
      std::string("C9.26-crashed-upload-payload:") + std::string(512, 'A');
  const std::string fresh_payload =
      std::string("C9.26-survivor-fresh-payload:") + std::string(333, 'B');
  const std::string retry_payload =
      std::string("C9.26-same-source-retry-payload:") + std::string(777, 'C');

  FileSourceGuard guard;
  auto* const sources = guard.sources();

  // ---------------------------------------------------------------------------
  //  ① 两个进程就绪；恰好一个 leader（pg_locks 恰好一行 + 横幅自认）
  // ---------------------------------------------------------------------------
  //  A 先启动并拿到锁 ⇒ 它**就是** leader；只有 A 带 `FSS_CLAIM_HOLD_MS`
  //  （接缝只给将被 kill 的那个进程，这样 survivor 的重试路径零延迟）。
  ServerProcess server_a(ProcessOptions(
      PgArgs(config_a, TestDsn()),
      {{"FSS_CLAIM_HOLD_MS", std::to_string(kClaimHoldMillis)}}));
  REQUIRE(server_a.http_port() == port_a);
  CAPTURE(server_a.DumpLog());
  REQUIRE(WaitReady(port_a));
  REQUIRE(WaitFor([&] { return AdvisoryLockRows(lock_key) == 1; }, 200, 50));
  REQUIRE(server_a.DumpLog().find("启动时本实例=leader") != std::string::npos);
  REQUIRE(server_a.DumpLog().find("claim hold ms  : " + std::to_string(kClaimHoldMillis)) !=
          std::string::npos);
  //  ★ 正控：接缝的取值真的来自环境变量（而不是硬编码的 0/别的数）。
  const std::int64_t leader_backend_pid = AdvisoryLockPid(lock_key);
  REQUIRE(leader_backend_pid > 0);
  CAPTURE(leader_backend_pid);

  const int port_b = FreePort();
  REQUIRE(port_b > 0);
  REQUIRE(port_b != port_a);
  const std::string config_b = WriteFile(work_dir.str() + "/b.json",
                                         ScenarioConfig(root, port_b, "c926-b", lock_key,
                                                        /*gc_enabled=*/true));
  //  B 不带 `FSS_CLAIM_HOLD_MS`（负控：接缝只在 A 上）。
  ServerProcess server_b(ProcessOptions(PgArgs(config_b, TestDsn())));
  REQUIRE(server_b.http_port() == port_b);
  CAPTURE(server_b.DumpLog());
  REQUIRE(WaitReady(port_b));
  //  ★ 恰好一个 leader：锁行数恒为 1（B 没有拿到锁），B 的横幅自认非 leader，
  //    且 B 上**没有**接缝横幅（负控）。
  REQUIRE(AdvisoryLockRows(lock_key) == 1);
  REQUIRE(server_b.DumpLog().find("启动时本实例=非 leader") != std::string::npos);
  REQUIRE(server_b.DumpLog().find("claim hold ms") == std::string::npos);
  REQUIRE_FALSE(AdvisoryLockPid(lock_key) == 0);
  //  ★ 场景的正控（见 `ScenarioConfig` 的 `gc_enabled` 说明）：A 不跑周期 GC、B 跑。
  REQUIRE(server_a.DumpLog().find("gc             : 未启动（gc.enabled=false") !=
          std::string::npos);
  REQUIRE(server_b.DumpLog().find("gc             : 已启动（间隔 1s") != std::string::npos);
  //  实测值（成功路径也打印：判据的"观测值"必须可归档，而不是只在失败时可见）。
  std::cout << "[C9.26] ① leader = A pid(process)=" << server_a.pid()
            << " lock_key=" << lock_key
            << " leader_backend_pid=" << leader_backend_pid
            << " advisory_lock_rows=" << AdvisoryLockRows(lock_key) << std::endl;

  // ---------------------------------------------------------------------------
  //  ② 经 leader（A）的 HTTP：uploadURL → PUT → 后台 createMetadata（A 上被接缝停住）
  // ---------------------------------------------------------------------------
  const auto uploaded = UploadViaProcess(port_a, crashed_payload, /*post_metadata=*/false);
  sources->push_back(uploaded.file_source);
  const auto crashed_location = QueryLocation(uploaded.file_source);
  REQUIRE(crashed_location.rows == 1);
  REQUIRE(crashed_location.zone == "STAGING");
  const std::string staging_path =
      ObjectPath(root, crashed_location.container, crashed_location.object_key);
  //  ★ PUT 的正控：字节真的落盘了（大小与 payload 一致）。
  REQUIRE(std::filesystem::exists(staging_path));
  REQUIRE(fss::test::ReadWholeFile(staging_path).size() == crashed_payload.size());

  auto record = fss::test::AppFixture::MakeRecord(uploaded.file_source, "c926-crashed.bin");
  const std::string post_body = fss::json::Dump(fss::domain::ToJson(record));

  //  后台线程发起 POST：它是**阻塞的**（A 要在领取后停住 kClaimHoldMillis），
  //  因此不能占住测试线程。★ 本助手**不用 Catch2 宏**：断言宏抛出的异常逃出
  //  这个线程会直接 `terminate` 整个测试进程。
  struct AsyncReply {
    bool transport_ok = false;
    int status = 0;
    std::string body;
  } post_reply;
  //  "请求是否已返回"的原子标志：主线程用它把"窗口稳定存在"变成**可轮询的实际条件**
  //  （而不是靠"恰好轮询到一闪而过的 claiming 行"）。所有返回路径都置位。
  std::atomic<bool> post_done{false};
  std::thread requester([&] {
    struct DoneFlag {
      std::atomic<bool>* flag;
      ~DoneFlag() { flag->store(true); }
    } done_flag{&post_done};
    fss::test::RawClient client(port_a, /*tcp_nodelay=*/true);
    if (!client.Connect(/*timeout_ms=*/5000)) return;
    std::vector<std::string> headers = Authed();
    headers.push_back("Content-Length: " + std::to_string(post_body.size()));
    if (!client.SendRequest("POST", "/api/file/v2/files/metadata", headers, post_body)) return;
    const auto response = client.ReadResponse(/*timeout_ms=*/30000);
    if (!response.has_value()) return;
    post_reply.transport_ok = true;
    post_reply.status = response->status;
    post_reply.body = response->body;
  });
  ThreadJoiner requester_joiner(std::move(requester));

  // ---------------------------------------------------------------------------
  //  ③ 轮询 PG 直到 claiming 行存在，并证明这个窗口**稳定持续**（**正控**）
  // ---------------------------------------------------------------------------
  const bool claim_visible = WaitFor(
      [&] { return CountMetadataRows(uploaded.file_source, "claiming") == 1; },
      /*attempts=*/200, /*millis=*/50);
  CAPTURE(claim_visible, CountMetadataRows(uploaded.file_source, "claiming"));
  REQUIRE(claim_visible);
  //  ★ 窗口的**稳定性**判据：后台 POST 必须在随后的 5 秒里**始终没有返回**。
  //    没有 `FSS_CLAIM_HOLD_MS` 时，复制+`MarkReady`+释放租约只需几十到几百毫秒，
  //    请求早就返回了 ⇒ 这里的正控会失败（这正是 R1 注入①要证明的：窗口是**接缝**造的，
  //    不是"恰好轮询到一个瞬时状态"）。用轮询 `post_done` 表达，而不是"睡 5 秒再说"。
  bool stayed_in_flight = true;
  for (int i = 0; i < 50 && stayed_in_flight; ++i) {
    if (post_done.load()) stayed_in_flight = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CAPTURE(stayed_in_flight, post_done.load());
  REQUIRE(stayed_in_flight);
  //  正控：此刻**仍是** claiming（复制尚未发生）、还没有 ready、在途租约存在且活着。
  REQUIRE(CountMetadataRows(uploaded.file_source, "claiming") == 1);
  REQUIRE(CountMetadataRows(uploaded.file_source, "ready") == 0);
  const auto pre_kill_lease = QueryLease(uploaded.file_source);
  REQUIRE(pre_kill_lease.rows == 1);
  REQUIRE(pre_kill_lease.expires_epoch_seconds > std::time(nullptr));
  std::cout << "[C9.26] ③ file_source=" << uploaded.file_source << " file_id="
            << uploaded.file_id << " claiming_rows=1 ready_rows=0 窗口稳定≥5s"
            << " lease_owner=" << pre_kill_lease.owner
            << " lease_ttl_left_s=" << (pre_kill_lease.expires_epoch_seconds -
                                        static_cast<std::int64_t>(std::time(nullptr)))
            << std::endl;

  // ---------------------------------------------------------------------------
  //  ④ 真实 `kill -9`：claiming 行必须**还在**（崩溃不会替我们清理）
  // ---------------------------------------------------------------------------
  REQUIRE(std::system(("kill -9 " + server_a.pid() + " 2>/dev/null").c_str()) == 0);
  const auto kill_steady = std::chrono::steady_clock::now();  // 回收耗时（观测值）
  REQUIRE(WaitFor([&] { return !server_a.alive(); }, /*attempts=*/200, /*millis=*/50));
  CAPTURE(server_a.DumpLog());
  REQUIRE(CountMetadataRows(uploaded.file_source, "claiming") == 1);
  REQUIRE(CountLocationRows(uploaded.file_source) == 1);
  REQUIRE(std::filesystem::exists(staging_path));

  //  后台 POST 的结局：连接被 SIGKILL 切断 → 不可能拿到 201（它根本没走到复制）。
  //  （`requester_joiner` 保证任何早退路径也会 join，不会 terminate。）
  requester_joiner.Join();
  CAPTURE(post_reply.transport_ok, post_reply.status, post_reply.body);
  REQUIRE_FALSE(post_reply.status == 201);
  std::cout << "[C9.26] ④ kill -9 pid=" << server_a.pid()
            << " alive_after=" << (server_a.alive() ? "true" : "false")
            << " claiming_rows_after=" << CountMetadataRows(uploaded.file_source, "claiming")
            << " location_rows_after=" << CountLocationRows(uploaded.file_source)
            << " staging_exists_after=" << (std::filesystem::exists(staging_path) ? 1 : 0)
            << " async_post_status=" << post_reply.status << std::endl;

  // ---------------------------------------------------------------------------
  //  ⑤ B 接管（锁换了 backend pid）后，**租约到期之前**强制一轮 GC：不得回收
  // ---------------------------------------------------------------------------
  const bool takeover = WaitFor(
      [&] {
        const auto pid = AdvisoryLockPid(lock_key);
        return pid > 0 && pid != leader_backend_pid;
      },
      /*attempts=*/300, /*millis=*/100);
  CAPTURE(takeover, leader_backend_pid, AdvisoryLockPid(lock_key));
  REQUIRE(takeover);
  REQUIRE(AdvisoryLockRows(lock_key) == 1);
  //  ★ 正控：接管的进程真的是幸存者 B（它还在服务；A 已经死了）。
  REQUIRE(server_b.alive());
  REQUIRE(HttpGet(port_b, "/api/file/v2/readiness_check").status == 200);

  //  ★ 守卫的正控：租约**仍未到期**（本地钟），此时 GC 绝不能回收 claiming 行。
  const auto live_lease = QueryLease(uploaded.file_source);
  REQUIRE(live_lease.rows == 1);
  REQUIRE(live_lease.expires_epoch_seconds > std::time(nullptr));
  const std::int64_t reclaimed_before = MetricOf(port_b, "fss_gc_reclaimed_claiming_total");

  //  在 B 上强制一轮按需 GC：leader 门控可能在"刚接管"的瞬间返回 503，因此有界重试。
  bool gc_ran = false;
  int gc_status = 0;
  for (int i = 0; i < 100 && !gc_ran; ++i) {
    const auto gc = HttpDo(port_b, "POST", "/api/file/v2/gc:run", Authed());
    gc_status = gc.status;
    if (gc.status == 200) {
      gc_ran = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CAPTURE(gc_ran, gc_status);
  REQUIRE(gc_ran);  // 200 = `GcTask::Run` 真的跑了一轮
  //  守卫判据：行还在、对象还在、计数没涨。
  REQUIRE(CountMetadataRows(uploaded.file_source, "claiming") == 1);
  REQUIRE(std::filesystem::exists(staging_path));
  REQUIRE(MetricOf(port_b, "fss_gc_reclaimed_claiming_total") == reclaimed_before);
  REQUIRE(reclaimed_before == 0);
  //  ★ 对照：同一时刻的 `fss_gc_runs_total` 必须 > 0 —— 证明"没回收"不是因为 GC 没跑。
  REQUIRE(MetricOf(port_b, "fss_gc_runs_total") > 0);
  std::cout << "[C9.26] ⑤ survivor_backend_pid=" << AdvisoryLockPid(lock_key)
            << " lease_left_s="
            << (live_lease.expires_epoch_seconds - static_cast<std::int64_t>(std::time(nullptr)))
            << " gc_round_http=" << gc_status
            << " reclaimed_before=" << reclaimed_before
            << " claiming_rows=1 staging_exists=1"
            << " fss_gc_runs_total=" << MetricOf(port_b, "fss_gc_runs_total") << std::endl;

  // ---------------------------------------------------------------------------
  //  ⑥ 租约到期 → survivor 的 GC 回收 claiming 行 + 收走孤儿对象 + 删位置记录
  // ---------------------------------------------------------------------------
  const bool reclaimed = WaitFor(
      [&] { return MetricOf(port_b, "fss_gc_reclaimed_claiming_total") >= 1; },
      /*attempts=*/400, /*millis=*/100);
  CAPTURE(reclaimed, MetricOf(port_b, "fss_gc_reclaimed_claiming_total"));
  //  失败时的可诊断快照（租约还剩多久 / 行还在不在 / GC 到底跑了几轮）。
  const auto dbg_lease = QueryLease(uploaded.file_source);
  CAPTURE(dbg_lease.rows, dbg_lease.owner, dbg_lease.expires_epoch_seconds, std::time(nullptr),
          MetadataDigest(uploaded.file_source),
          CountMetadataRows(uploaded.file_source, "claiming"),
          CountLocationRows(uploaded.file_source), MetricOf(port_b, "fss_gc_runs_total"),
          MetricOf(port_b, "fss_gc_reclaimed_claiming_total"));
  REQUIRE(reclaimed);
  //  行消失（轮询：回收与对象清理在同一轮 GC 内，但指标先可见）。
  REQUIRE(WaitFor([&] { return CountMetadataRows(uploaded.file_source, "claiming") == 0; },
                  /*attempts=*/100, /*millis=*/50));
  REQUIRE(WaitFor([&] { return CountLocationRows(uploaded.file_source) == 0; },
                  /*attempts=*/100, /*millis=*/50));
  //  孤儿 staging 对象被收走（正控在步骤③：它确实存在过）。
  REQUIRE(WaitFor([&] { return !std::filesystem::exists(staging_path); },
                  /*attempts=*/100, /*millis=*/50));
  CAPTURE(server_b.DumpLog());
  REQUIRE(server_b.alive());
  std::cout << "[C9.26] ⑥ reclaimed_total="
            << MetricOf(port_b, "fss_gc_reclaimed_claiming_total") << " claiming_rows="
            << CountMetadataRows(uploaded.file_source, "claiming") << " location_rows="
            << CountLocationRows(uploaded.file_source) << " staging_exists="
            << (std::filesystem::exists(staging_path) ? 1 : 0)
            << " kill_to_reclaim_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - kill_steady)
                   .count()
            << std::endl;

  // ---------------------------------------------------------------------------
  //  ⑦a survivor 是一条**完整可用**的实例：全新的 uploadURL → PUT → createMetadata → 201
  // ---------------------------------------------------------------------------
  const auto fresh = UploadViaProcess(port_b, fresh_payload, /*post_metadata=*/true);
  sources->push_back(fresh.file_source);
  REQUIRE(fresh.file_source != uploaded.file_source);
  const auto fresh_read = ReadMetadata(port_b, fresh.record_id);
  REQUIRE(fresh_read.status == 200);
  REQUIRE(fresh_read.file_source == fresh.file_source);
  REQUIRE(LowerHex(fresh_read.checksum) == fss::crypto::Sha256Hex(fresh_payload));
  REQUIRE(fresh_read.algorithm == "SHA256");
  const auto fresh_location = QueryLocation(fresh.file_source);
  REQUIRE(fresh_location.rows == 1);
  REQUIRE(fresh_location.zone == "PERSISTENT");
  const std::string fresh_persistent =
      ObjectPath(root, kPersistentContainer, fresh_location.object_key);
  REQUIRE(std::filesystem::exists(fresh_persistent));
  REQUIRE(fss::test::ReadWholeFile(fresh_persistent) == fresh_payload);

  // ---------------------------------------------------------------------------
  //  ⑦b **同一个 file_source** 的重试：crash 的 claiming 行已被回收 ⇒ 该幂等键重新可用
  // ---------------------------------------------------------------------------
  //  GC（正确地）把位置记录连同孤儿对象一起删了（步骤⑥已断言），而 `uploadURL` 无法
  //  重现同一个 file_source（它内嵌服务端生成的 epoch+file_id）。因此"重试同一个
  //  file_source"的**前置条件**由测试显式恢复，并在此处**显式断言**（R9：前置条件要断言）：
  //  重新写回 staging 对象 + 恢复位置记录；PUT 走**步骤②那张仍然有效的自签 URL**
  //  （token 的 `single_use_nonce` 默认 false，见 transfer_token.h）——数据面通道不变。
  const auto put_retry = HttpDo(port_b, "PUT", TargetOf(uploaded.put_url), Authed(), retry_payload);
  REQUIRE(put_retry.status == 200);
  REQUIRE(std::filesystem::exists(staging_path));
  REQUIRE(fss::test::ReadWholeFile(staging_path) == retry_payload);
  {
    //  ★ 用**真实**的 PG 位置仓储恢复（而不是手写 INSERT）：容器/对象键在领域模型里
    //    属于 `extra`，读回时由 `ParseLocation` 从 `data` 的摊平键还原 —— 手写
    //    `data='{}'` 会得到"位置记录缺少物理引用"的 500（本用例第一版正是如此，
    //    这条注释就是那次失败的结论）。
    fss::infra::PostgresLocationRepositoryOptions opts;
    opts.pg = TestPgOptions(2);
    auto locations = fss::infra::PostgresLocationRepository::Open(opts);
    CAPTURE(locations.ok() ? std::string("ok") : locations.error().message());
    REQUIRE(locations.ok());
    fss::domain::FileLocation restored;
    restored.file_id = crashed_location.file_id;
    restored.file_source = uploaded.file_source;
    restored.driver = fss::domain::StorageDriver::kPosix;
    restored.zone = fss::domain::StorageZone::kStaging;
    restored.user_id = "osdu-user";
    restored.created_at_epoch_seconds = static_cast<std::int64_t>(std::time(nullptr));
    restored.updated_at_epoch_seconds = restored.created_at_epoch_seconds;
    restored.extra["container"] = crashed_location.container;
    restored.extra["object_key"] = crashed_location.object_key;
    const auto saved = locations.value()->Save(kPartition, restored);
    CAPTURE(saved.ok() ? std::string("ok") : saved.error().message());
    REQUIRE(saved.ok());
  }
  REQUIRE(CountLocationRows(uploaded.file_source) == 1);

  const auto retried = HttpDo(port_b, "POST", "/api/file/v2/files/metadata", Authed(), post_body);
  INFO("retry POST metadata: " << retried.body);
  REQUIRE(retried.status == 201);
  const auto retried_json = fss::json::ParseObject(retried.body);
  REQUIRE(retried_json.ok());
  const std::string retried_id = retried_json.value()["id"].get<std::string>();

  const auto retried_read = ReadMetadata(port_b, retried_id);
  REQUIRE(retried_read.status == 200);
  REQUIRE(retried_read.file_source == uploaded.file_source);
  //  ★ SHA-256 必须与**重传的**字节一致（不是崩溃前那一份）。
  REQUIRE(LowerHex(retried_read.checksum) == fss::crypto::Sha256Hex(retry_payload));
  REQUIRE(retried_read.algorithm == "SHA256");
  REQUIRE(LowerHex(retried_read.checksum) != fss::crypto::Sha256Hex(crashed_payload));

  //  真的发生了一次**新的复制**：persistent 对象的内容 = 重传字节，staging 被清理。
  REQUIRE(CountLocationRows(uploaded.file_source) == 1);
  const auto retried_location = QueryLocation(uploaded.file_source);
  REQUIRE(retried_location.zone == "PERSISTENT");
  const std::string retried_persistent =
      ObjectPath(root, kPersistentContainer, retried_location.object_key);
  REQUIRE(std::filesystem::exists(retried_persistent));
  REQUIRE(fss::test::ReadWholeFile(retried_persistent) == retry_payload);
  REQUIRE(WaitFor([&] { return !std::filesystem::exists(staging_path); },
                  /*attempts=*/100, /*millis=*/50));
  std::cout << "[C9.26] ⑦a fresh_status=201 fresh_sha256="
            << LowerHex(fresh_read.checksum).substr(0, 16) << "… persistent_bytes="
            << fss::test::ReadWholeFile(fresh_persistent).size() << std::endl;
  std::cout << "[C9.26] ⑦b retry_same_file_source=" << uploaded.file_source
            << " status=201 retry_sha256=" << LowerHex(retried_read.checksum).substr(0, 16)
            << "… persistent_bytes=" << fss::test::ReadWholeFile(retried_persistent).size()
            << " staging_removed=1" << std::endl;

  // ---------------------------------------------------------------------------
  //  数据卫生：本用例自己的行全部清掉（残留 0）；advisory lock 随着 A 的死亡释放
  // ---------------------------------------------------------------------------
  CleanupFileSource(uploaded.file_source);
  CleanupFileSource(fresh.file_source);
  REQUIRE(CountMetadataRows(uploaded.file_source, "claiming") == 0);
  REQUIRE(CountMetadataRows(uploaded.file_source, "ready") == 0);
  REQUIRE(CountLocationRows(uploaded.file_source) == 0);
  REQUIRE(CountMetadataRows(fresh.file_source, "ready") == 0);
  REQUIRE(QueryLease(uploaded.file_source).rows == 0);
  REQUIRE(QueryLease(fresh.file_source).rows == 0);

  //  两个进程都活着/已被杀；B 优雅退出（SIGTERM）→ 会话级锁释放（轮询，不用固定 sleep）。
  REQUIRE(std::system(("kill " + server_b.pid() + " 2>/dev/null").c_str()) == 0);
  REQUIRE(WaitFor([&] { return AdvisoryLockRows(lock_key) == 0; }, /*attempts=*/200,
                  /*millis=*/50));
}
