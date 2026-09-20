// =============================================================================
//  E1b：`storage.posix.one_filesystem_per_partition` 从「已读但无效果」
//        变成**启动期强制校验**（L2 规则 + 组合根接线 + 真实进程）
// =============================================================================
//  配置键的语义（本切片钉住，见 `src/infra/blob/posix/partition_filesystem_check.h`）：
//    · `true` = 部署方**断言**"每个 partition 独占文件系统"。组合根启动期用 `::stat(2)`
//      （跟随符号链接）验证，不成立 → **exit 78**（fail-closed），绝不静默忽略。
//    · 规则 A：partition 的目录（staging/persistent）必须与 `storage.posix.root` **分盘**。
//    · 规则 B：**两个不同 partition 不得解析到同一个文件系统**（今天组合根只构造
//      **一个** partition ⇒ 规则 B 在生产形态下是空集，必须用**合成多 partition 输入**
//      的 L2 用例 U3 钉住）。
//    · **刻意不检查**：同一 partition 的 staging 与 persistent 是否同盘（§10.3 允许两卷）；
//      U5 专门钉住"没有加这条规则"。
//    · **刻意不声称**：该 fs 上有没有非本服务的负载（进程内不可观测）。
//
//  为什么用 /dev/shm 造"另一个文件系统"：本机不能 `mount`（无 root）。`/dev/shm` 是
//  tmpfs，`st_dev` 与仓库/`/tmp` 不同（实测 65 vs 2096）。把一个目录做成指向 /dev/shm
//  的**符号链接**，`::stat` 就会返回不同的 `st_dev` —— 这正是"目录是挂载点/软链"的
//  分辨方式，且 `fs::SafeJoin` 的 canonical 包含性检查仍然成立（W2 同时钉住这一点）。
//  U5 还用到 `/run/lock`（另一个可写的 tmpfs，`st_dev` 又与 /dev/shm 不同）。
//
//  判据纪律：
//    · 每个"设备不同"的前置条件都显式断言 + `CAPTURE`：若环境哪天不再提供两个不同
//      文件系统，用例**明确失败**并告诉读者"本环境无法表达该正例"，绝不静默跳过（R4/R9）。
//    · U4 的负断言（"没有副作用"）配正控（同一路径解析上先证明它确实不存在）。
//
//  ★ R1 自证注入对应关系见 `docs/test-evidence/phase10.md` §24 的注入表（I1~I5 + 护栏注入）。
// =============================================================================
#include <catch2/catch.hpp>

#include "raw_http.h"
#include "server_process.h"
#include "temp_dir.h"

#include "common/json/json.h"
#include "domain/model/file_metadata.h"
#include "infra/blob/posix/partition_filesystem_check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using fss::infra::CheckOneFilesystemPerPartition;
using fss::infra::PartitionDirSpec;
using fss::infra::PartitionFilesystemReport;
using fss::test::RawClient;
using fss::test::ServerProcess;
using fss::test::ServerProcessOptions;
using fss::test::TempDir;

constexpr const char* kKey = "storage.posix.one_filesystem_per_partition";
//  本机实测：仓库/`/tmp` = 2096，`/dev/shm` = 65，`/run/lock` = 64（E1b 实测值）。
//  **不把数字写进判据**（可能随机器变化），只断言"互不相同"，并在不满足时明确失败。
constexpr const char* kShmBase = "/dev/shm";
constexpr const char* kLockBase = "/run/lock";

unsigned long long DeviceOf(const std::string& path) {
  struct ::stat info {};
  INFO("stat(" << path << ")");
  REQUIRE(::stat(path.c_str(), &info) == 0);
  return static_cast<unsigned long long>(info.st_dev);
}

std::string Join(const std::string& a, const std::string& b) { return a + "/" + b; }

//  /dev/shm、/run/lock 上的 scratch 目录（RAII：析构时删掉自己）。
class ScratchDir {
 public:
  explicit ScratchDir(const std::string& base, const std::string& tag) {
    static std::atomic<int> counter{0};
    path_ = base + "/fss_e1b_" + tag + "_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter.fetch_add(1));
    std::error_code ec;
    fs::create_directories(path_, ec);
    INFO("在 " << base << " 上创建 scratch 目录失败（需可写 tmpfs）：" << ec.message());
    REQUIRE_FALSE(ec);
  }
  ~ScratchDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);  // 清理失败不影响判据
  }
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;
  ScratchDir(ScratchDir&&) = delete;
  ScratchDir& operator=(ScratchDir&&) = delete;
  const std::string& str() const { return path_; }

 private:
  std::string path_;
};

//  目录做成指向 `target` 的符号链接（`::stat` 会跟随到 target 的设备号）。
void SymlinkDir(const std::string& target, const std::string& link) {
  std::error_code ec;
  fs::create_directory_symlink(target, link, ec);
  INFO("创建目录符号链接失败：" << link << " -> " << target << "：" << ec.message());
  REQUIRE_FALSE(ec);
}

std::string WriteFile(const TempDir& dir, const std::string& name, const std::string& content) {
  const std::string path = dir.child(name);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.good());
  out << content;
  out.close();
  return path;
}

//  ★ W1/W4 用 `RunServerForExit`（不注入 `FSS_HTTP_PORT=0`），因此配置里必须给一个
//  **空闲端口**：否则"检查通过后继续启动"的注入场景会撞上被占用的默认 8080，
//  退回一个**与检查无关的 exit 78**，让判据失去区分力（本机 8080 实测被占用）。
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

std::string ConfigJson(const std::string& root, bool one_filesystem, int port,
                       const std::string& driver = "posix") {
  std::ostringstream json;
  json << "{\n"
       << "  \"deployment\": {\"mode\": \"single\"},\n"
       << "  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " << port
       << "}, \"grpc\": {\"enabled\": false}},\n"
       << "  \"storage\": {\"driver\": \"" << driver << "\", \"posix\": {\"root\": \"" << root
       << "\", \"one_filesystem_per_partition\": " << (one_filesystem ? "true" : "false")
       << "}},\n"
       << "  \"metadata\": {\"sqlite\": {\"path\": \"" << root << "/metadata.db\"}},\n"
       << "  \"location\": {\"sqlite\": {\"path\": \"" << root << "/location.db\"}},\n"
       << "  \"self_signed\": {\"signing_key\": \"e1b-test\"},\n"
       << "  \"auth\": {\"mode\": \"disabled\"},\n"
       << "  \"gc\": {\"enabled\": false}\n"
       << "}\n";
  return json.str();
}

//  ---- 最小 HTTP helper（raw_http.h 的 RawClient；不引入适配层/夹具）----
struct HttpReply {
  bool transport_ok = false;
  int status = 0;
  std::string body;
};

HttpReply Request(int port, const std::string& method, const std::string& target,
                  const std::vector<std::string>& headers = {}, const std::string& body = {}) {
  HttpReply reply;
  RawClient client(port, /*tcp_nodelay=*/true);
  if (!client.Connect()) return reply;
  std::vector<std::string> all = headers;
  const bool method_has_body = method == "PUT" || method == "POST" || method == "PATCH";
  if (!body.empty() || method_has_body) {
    all.push_back("Content-Length: " + std::to_string(body.size()));
  }
  if (!client.SendRequest(method, target, all, body)) return reply;
  const auto response = client.ReadResponse(30000);
  if (!response.has_value()) return reply;
  reply.transport_ok = true;
  reply.status = response->status;
  reply.body = response->body;
  return reply;
}

std::vector<std::string> Authed() {
  return {"authorization: Bearer test-token", "data-partition-id: opendes"};
}

std::string TargetOf(const std::string& url) {
  const auto scheme = url.find("://");
  const auto path_start = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
  REQUIRE(path_start != std::string::npos);
  return url.substr(path_start);
}

bool WaitReady(int port, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto reply = Request(port, "GET", "/api/file/v2/readiness_check", Authed());
    if (reply.transport_ok && reply.status == 200) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

//  与 `AppFixture::MakeRecord` 同一份记录（只搬需要的几行，避免为一个 helper 多引一层）；
//  `present` 必须置位（proto3 的段存在性）。
fss::domain::FileMetadataRecord MakeRecord(const std::string& file_source) {
  fss::domain::FileMetadataRecord record;
  record.kind = "opendes:wks:dataset--File.Generic:1.0.0";
  record.acl.viewers = {"data.default.viewers@opendes.example.com"};
  record.acl.owners = {"data.default.owners@opendes.example.com"};
  record.legal.legaltags = {"opendes-public-1"};
  record.legal.other_relevant_data_countries = {"US"};
  record.legal.status = fss::domain::LegalStatus::kCompliant;
  record.data.name = "e1b.bin";
  record.data.endian = "LITTLE";
  record.data.dataset_properties.file_source_info.file_source = file_source;
  record.data.dataset_properties.present = true;
  return record;
}

}  // namespace

// =============================================================================
//  ---- L2：直接调用新模块（无需 PostgreSQL；scratch 目录建在 build/ 的 TempDir 下）----
// =============================================================================

//  U1 正控（通过）：root 在仓库文件系统；两个 partition 目录是指向 /dev/shm 的符号链接。
TEST_CASE("★ E1b-U1 正控：partition 目录与 root 分盘 ⇒ 通过，且报告的设备号真的不同",
          "[e1b][integration]") {
  TempDir dir("e1b_u1");
  const std::string root = dir.child("root");
  REQUIRE(fs::create_directories(root + "/blobs"));
  ScratchDir staging(kShmBase, "u1s");
  ScratchDir persistent(kShmBase, "u1p");
  SymlinkDir(staging.str(), root + "/blobs/opendes-staging");
  SymlinkDir(persistent.str(), root + "/blobs/opendes-persistent");

  const PartitionDirSpec spec{"opendes", root + "/blobs/opendes-staging",
                              root + "/blobs/opendes-persistent"};
  const auto report = CheckOneFilesystemPerPartition(root, {spec});
  INFO("错误（若有）：" << (report.ok() ? std::string() : report.error().ToString()));
  REQUIRE(report.ok());
  REQUIRE(report.value().entries.size() == 1);

  const auto root_device = report.value().root_device;
  const auto partition_device = report.value().entries[0].device;
  CAPTURE(root_device, partition_device);
  //  ★ 前置条件显式断言：本环境必须真的能表达"两个不同文件系统"。若两者相等，
  //    用例**明确失败**并告诉读者这是环境能力问题 —— 绝不静默跳过（R4/R9）。
  REQUIRE(root_device != partition_device);
  REQUIRE(report.value().entries[0].persistent_device == partition_device);
  REQUIRE(report.value().entries[0].sample_path == root + "/blobs/opendes-staging");
  //  正控：报告里的设备号 == 直接 stat 出来的（不是常量/占位符）。
  REQUIRE(root_device == DeviceOf(root));
  REQUIRE(partition_device == DeviceOf(staging.str()));
}

//  U2 反例（规则 A）：目录与 root 同盘 ⇒ 报错，消息含键名 / partition 名 / 两条路径。
TEST_CASE("★ E1b-U2 反例（规则 A）：partition 目录与 root 同盘 ⇒ 拒绝并给出可读原因",
          "[e1b][integration]") {
  TempDir dir("e1b_u2");
  const std::string root = dir.child("root");
  const std::string staging = root + "/blobs/opendes-staging";
  const std::string persistent = root + "/blobs/opendes-persistent";
  REQUIRE(fs::create_directories(staging));
  REQUIRE(fs::create_directories(persistent));

  const PartitionDirSpec spec{"opendes", staging, persistent};
  const auto report = CheckOneFilesystemPerPartition(root, {spec});
  REQUIRE_FALSE(report.ok());
  const std::string message = report.error().ToString();
  CAPTURE(message);
  REQUIRE(report.error().kind() == fss::ErrorKind::kInvalidArgument);
  REQUIRE(message.find(kKey) != std::string::npos);
  REQUIRE(message.find("opendes") != std::string::npos);
  REQUIRE(message.find(staging) != std::string::npos);
  REQUIRE(message.find(persistent) != std::string::npos);
  //  为什么/修法都要在消息里（运维可执行）。
  REQUIRE(message.find("syncfs") != std::string::npos);
  REQUIRE(message.find("false") != std::string::npos);
}

//  U3 反例（规则 B）：两个合成 partition 共用一个文件系统（都在 /dev/shm）⇒ 报错并点名两者。
//  ★ 这就是"今天生产形态下空集"的那条规则：组合根只构造一个 partition，所以它只能由
//    这个**合成输入**用例钉住。
TEST_CASE("★ E1b-U3 反例（规则 B）：两个 partition 同盘 ⇒ 拒绝并点名两个 partition（今天生产空集）",
          "[e1b][integration]") {
  TempDir dir("e1b_u3");
  const std::string root = dir.child("root");
  REQUIRE(fs::create_directories(root + "/blobs"));
  ScratchDir a_staging(kShmBase, "u3as");
  ScratchDir a_persistent(kShmBase, "u3ap");
  ScratchDir b_staging(kShmBase, "u3bs");
  ScratchDir b_persistent(kShmBase, "u3bp");
  const std::string alpha_staging = root + "/blobs/alpha-staging";
  const std::string alpha_persistent = root + "/blobs/alpha-persistent";
  const std::string beta_staging = root + "/blobs/beta-staging";
  const std::string beta_persistent = root + "/blobs/beta-persistent";
  SymlinkDir(a_staging.str(), alpha_staging);
  SymlinkDir(a_persistent.str(), alpha_persistent);
  SymlinkDir(b_staging.str(), beta_staging);
  SymlinkDir(b_persistent.str(), beta_persistent);

  const std::vector<PartitionDirSpec> specs = {
      {"alpha", alpha_staging, alpha_persistent},
      {"beta", beta_staging, beta_persistent},
  };
  //  前置条件：两个 partition 的目录确实在同一个设备上，且该设备 != root（否则规则 A
  //  先触发，用例测的就不是规则 B 了）。
  const auto alpha_device = DeviceOf(alpha_staging);
  const auto beta_device = DeviceOf(beta_staging);
  const auto root_device = DeviceOf(root);
  CAPTURE(alpha_device, beta_device, root_device);
  REQUIRE(alpha_device == beta_device);
  REQUIRE(alpha_device != root_device);

  const auto report = CheckOneFilesystemPerPartition(root, specs);
  REQUIRE_FALSE(report.ok());
  const std::string message = report.error().ToString();
  CAPTURE(message);
  REQUIRE(message.find(kKey) != std::string::npos);
  REQUIRE(message.find("规则 B") != std::string::npos);
  REQUIRE(message.find("\"alpha\"") != std::string::npos);
  REQUIRE(message.find("\"beta\"") != std::string::npos);
  REQUIRE(message.find(std::to_string(alpha_device)) != std::string::npos);
  //  两条具体路径也要在（运维能直接 `ls`）。
  REQUIRE(message.find(alpha_staging) != std::string::npos);
  REQUIRE(message.find(beta_persistent) != std::string::npos);
}

//  U4 反例（目录不存在）+ 只读自证：报错，且事后该路径**仍然不存在**。
TEST_CASE("★ E1b-U4 反例（目录不存在）：报错要求先创建/挂载，且不产生任何副作用",
          "[e1b][integration]") {
  TempDir dir("e1b_u4");
  const std::string root = dir.child("root");
  REQUIRE(fs::create_directories(root + "/blobs"));
  const std::string missing_staging = root + "/blobs/opendes-staging";
  ScratchDir persistent(kShmBase, "u4p");
  const std::string persistent_dir = root + "/blobs/opendes-persistent";
  SymlinkDir(persistent.str(), persistent_dir);

  //  正控（R16 / 负断言陷阱）：先证明"该存在的存在、该不存在的不存在"，
  //  否则 REQUIRE_FALSE(exists) 可能因为路径写错而恒真。
  REQUIRE(fs::exists(persistent_dir));
  REQUIRE_FALSE(fs::exists(missing_staging));

  const PartitionDirSpec spec{"opendes", missing_staging, persistent_dir};
  const auto report = CheckOneFilesystemPerPartition(root, {spec});
  REQUIRE_FALSE(report.ok());
  const std::string message = report.error().ToString();
  CAPTURE(message);
  REQUIRE(message.find(kKey) != std::string::npos);
  REQUIRE(message.find("opendes") != std::string::npos);
  REQUIRE(message.find(missing_staging) != std::string::npos);
  REQUIRE(message.find("创建") != std::string::npos);

  //  ★ 只读保证：检查**没有**替运维把目录建出来（否则挂载点会被空目录顶掉）。
  REQUIRE_FALSE(fs::exists(missing_staging));
}

//  U5 边界：**刻意不检查**"staging == persistent"。
//  root 在 /dev/shm（tmpfs），一个 partition 的 staging 在仓库文件系统、persistent 在
//  /run/lock（第三个设备）—— 三者互不相同 ⇒ 必须**通过**。
//  若实现（错误地）加上"staging 设备必须等于 persistent 设备"，本用例会失败（R1 的 I4）。
TEST_CASE("★ E1b-U5 边界：staging 与 persistent 分属不同文件系统时**必须通过**"
          "（证明没有加「staging==persistent」规则）",
          "[e1b][integration]") {
  ScratchDir shm_root(kShmBase, "u5root");
  const std::string root = Join(shm_root.str(), "root");
  REQUIRE(fs::create_directories(Join(root, "blobs")));
  TempDir repo_dir("e1b_u5_repo");              // 仓库/`/tmp` 文件系统
  ScratchDir lock_target(kLockBase, "u5persist");  // /run/lock（第三个设备）

  const std::string staging_dir = root + "/blobs/opendes-staging";
  const std::string persistent_dir = root + "/blobs/opendes-persistent";
  SymlinkDir(repo_dir.str(), staging_dir);
  SymlinkDir(lock_target.str(), persistent_dir);

  const auto root_device = DeviceOf(root);
  const auto staging_device = DeviceOf(staging_dir);
  const auto persistent_device = DeviceOf(persistent_dir);
  CAPTURE(root_device, staging_device, persistent_device);
  //  前置条件：三个设备互不相同 —— 否则这个正例在本环境里无法表达（明确失败，不跳过）。
  REQUIRE(root_device != staging_device);
  REQUIRE(root_device != persistent_device);
  REQUIRE(staging_device != persistent_device);

  const PartitionDirSpec spec{"opendes", staging_dir, persistent_dir};
  const auto report = CheckOneFilesystemPerPartition(root, {spec});
  INFO("错误（若有）：" << (report.ok() ? std::string() : report.error().ToString()));
  REQUIRE(report.ok());
  REQUIRE(report.value().entries.size() == 1);
  REQUIRE(report.value().entries[0].device == staging_device);
  REQUIRE(report.value().entries[0].persistent_device == persistent_device);
}

//  U6 空输入：必须是错误（"检查恒真"保护），不能静默通过。
TEST_CASE("★ E1b-U6 边界：空 partition 列表 ⇒ 报错（拒绝「检查恒真」）", "[e1b][integration]") {
  TempDir dir("e1b_u6");
  const std::string root = dir.child("root");
  REQUIRE(fs::create_directories(root));
  const auto report = CheckOneFilesystemPerPartition(root, {});
  REQUIRE_FALSE(report.ok());
  const std::string message = report.error().ToString();
  CAPTURE(message);
  REQUIRE(report.error().kind() == fss::ErrorKind::kInvalidArgument);
  REQUIRE(message.find(kKey) != std::string::npos);
  REQUIRE(message.find("没有 partition") != std::string::npos);
}

//  U7 边界：相对路径必须是错误 —— 否则 `::stat` 相对**当前工作目录**解析，
//  检查结果会随启动目录漂移（"必须确定性"这条约定的可执行判据；R15）。
TEST_CASE("★ E1b-U7 边界：相对路径 ⇒ 报错（检查不得依赖当前工作目录）", "[e1b][integration]") {
  TempDir dir("e1b_u7");
  const std::string root = dir.child("root");
  REQUIRE(fs::create_directories(root + "/blobs/opendes-staging"));
  REQUIRE(fs::create_directories(root + "/blobs/opendes-persistent"));

  //  root 用**绝对**路径，partition 目录用相对路径 ⇒ 必须被拒（而不是相对 CWD 解析）。
  const PartitionDirSpec spec{"opendes", "blobs/opendes-staging", "blobs/opendes-persistent"};
  const auto report = CheckOneFilesystemPerPartition(root, {spec});
  INFO("错误（若有）：" << (report.ok() ? std::string() : report.error().ToString()));
  REQUIRE_FALSE(report.ok());
  REQUIRE(report.error().ToString().find("绝对路径") != std::string::npos);

  //  反面对照：同一组目录写成绝对路径时，本用例关心的分支（相对路径校验）不再触发 ——
  //  这里只断言"能 stat 到、进入规则判定"（它会因与 root 同盘而失败，那是规则 A 的正确行为）。
  const PartitionDirSpec absolute_spec{"opendes", root + "/blobs/opendes-staging",
                                       root + "/blobs/opendes-persistent"};
  const auto absolute_report = CheckOneFilesystemPerPartition(root, {absolute_spec});
  REQUIRE_FALSE(absolute_report.ok());
  REQUIRE(absolute_report.error().ToString().find("规则 A") != std::string::npos);
}

// =============================================================================
//  ---- 接线：真实 `fss_server` 进程（组合根读键 + 拒绝/放行 + 横幅可见）----
// =============================================================================

//  W1 反例：posix + true + 普通布局（容器目录是 root/blobs 下的真实目录）⇒ exit 78。
TEST_CASE("★ E1b-W1 反例：posix + one_filesystem_per_partition=true + 未分盘 ⇒ exit 78",
          "[e1b][integration]") {
  TempDir dir("e1b_w1");
  const std::string root = dir.child("data");
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, /*one=*/true, FreePort()));

  const auto outcome = fss::test::RunServerForExit({"--config", config});
  CAPTURE(outcome.exit_code, outcome.output);
  REQUIRE(outcome.exit_code == 78);
  REQUIRE(outcome.output.find(kKey) != std::string::npos);
  REQUIRE(outcome.output.find("opendes") != std::string::npos);
  REQUIRE(outcome.output.find("下一步") != std::string::npos);
}

//  W2 正例：同配置，但 `<root>/blobs` 是指向 /dev/shm 的符号链接 ⇒ 能启动、ready，
//  且一次真实上传的字节**真的落在 /dev/shm 的目标目录**（正控，不是"看起来写了"）。
TEST_CASE("★ E1b-W2 正例：blobs 软链到 /dev/shm ⇒ 启动成功 + 真实上传落在该文件系统",
          "[e1b][integration]") {
  TempDir dir("e1b_w2");
  const std::string root = dir.child("data");
  REQUIRE(fs::create_directories(root));
  ScratchDir shm_target(kShmBase, "w2blobs");
  SymlinkDir(shm_target.str(), root + "/blobs");
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, /*one=*/true, FreePort()));

  ServerProcessOptions options;
  options.default_http_port = true;   // FSS_HTTP_PORT=0 → 系统分配，横幅里解析实际端口
  options.default_grpc_port = false;
  options.expect_grpc = false;
  options.default_storage_env = false;  // 存储根/驱动全部来自 --config，避免 env 覆盖
  options.args = {"--config", config};
  ServerProcess server(options);
  INFO("启动横幅：\n" << server.DumpLog());
  REQUIRE(WaitReady(server.http_port(), 30000));

  const std::string payload = "E1B-W2-PAYLOAD-0123456789";
  const auto upload = Request(server.http_port(), "GET", "/api/file/v2/files/uploadURL", Authed());
  CAPTURE(upload.status, upload.body);
  REQUIRE(upload.status == 200);
  const auto upload_json = fss::json::ParseObject(upload.body);
  REQUIRE(upload_json.ok());
  const std::string file_source = upload_json.value()["Location"]["FileSource"].get<std::string>();
  const std::string put_url = upload_json.value()["Location"]["SignedURL"].get<std::string>();
  REQUIRE(put_url.find("/v1/transfer/") != std::string::npos);

  const auto put = Request(server.http_port(), "PUT", TargetOf(put_url), Authed(), payload);
  CAPTURE(put.status, put.body);
  REQUIRE(put.status == 200);

  //  ★ 正控：PUT 之后（**metadata 的 staging→persistent 复制发生之前**）对象文件
  //    **真的**出现在 /dev/shm 的目标目录里（读取内容逐字节比对）。
  const std::string object_path = shm_target.str() + "/opendes-staging" + file_source;
  INFO("期望的物理对象路径：" << object_path);
  REQUIRE(fs::exists(object_path));
  REQUIRE(DeviceOf(object_path) == DeviceOf(shm_target.str()));
  REQUIRE(DeviceOf(object_path) != DeviceOf(root));
  {
    std::ifstream in(object_path, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream buffer;
    buffer << in.rdbuf();
    REQUIRE(buffer.str() == payload);
  }

  auto record = MakeRecord(file_source);
  const auto created =
      Request(server.http_port(), "POST", "/api/file/v2/files/metadata", Authed(),
              fss::json::Dump(fss::domain::ToJson(record)));
  CAPTURE(created.status, created.body);
  REQUIRE(created.status == 201);
}

//  W3 正控（默认不变）：key=false（普通布局）⇒ 照常启动并 ready。
TEST_CASE("★ E1b-W3 正控：one_filesystem_per_partition=false ⇒ 普通布局照常启动（行为不变）",
          "[e1b][integration]") {
  TempDir dir("e1b_w3");
  const std::string root = dir.child("data");
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, /*one=*/false, FreePort()));

  ServerProcessOptions options;
  options.default_http_port = true;
  options.default_grpc_port = false;
  options.expect_grpc = false;
  options.default_storage_env = false;
  options.args = {"--config", config};
  ServerProcess server(options);
  INFO("启动横幅：\n" << server.DumpLog());
  REQUIRE(WaitReady(server.http_port(), 30000));
  //  横幅如实说"未启用"（R11：不能靠猜）。
  REQUIRE(server.DumpLog().find("part fs check : 未启用") != std::string::npos);
}

//  W4 反例（s3 矛盾）：storage.driver=s3 + key true ⇒ exit 78，且消息是 s3 专属的。
//  不需要 S3 服务：组合根在**任何 S3 I/O 之前**就拒绝。
TEST_CASE("★ E1b-W4 反例：storage.driver=s3 + key true ⇒ exit 78（矛盾配置，s3 无 partition 文件系统）",
          "[e1b][integration]") {
  TempDir dir("e1b_w4");
  const std::string root = dir.child("data");
  const std::string config =
      WriteFile(dir, "fss.json", ConfigJson(root, /*one=*/true, FreePort(), /*driver=*/"s3"));

  const auto outcome = fss::test::RunServerForExit({"--config", config});
  CAPTURE(outcome.exit_code, outcome.output);
  REQUIRE(outcome.exit_code == 78);
  REQUIRE(outcome.output.find(kKey) != std::string::npos);
  REQUIRE(outcome.output.find("storage.driver=s3") != std::string::npos);
  REQUIRE(outcome.output.find("posix") != std::string::npos);
  REQUIRE(outcome.output.find("下一步") != std::string::npos);
}

//  W5 正控（R11「探测结果必须可见」）：key=true + 软链布局 ⇒ 横幅真的打印已校验的设备号。
//  ★ 与 W2 是**两个进程**（W2 已在同一配置上证明"能启动 + 能写字节"）；这里只看横幅。
TEST_CASE("★ E1b-W5 正控：key=true 时横幅打印 root 与 partition 的实际 st_dev（探测结果可见）",
          "[e1b][integration]") {
  TempDir dir("e1b_w5");
  const std::string root = dir.child("data");
  REQUIRE(fs::create_directories(root));
  ScratchDir shm_target(kShmBase, "w5blobs");
  SymlinkDir(shm_target.str(), root + "/blobs");
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, /*one=*/true, FreePort()));

  ServerProcessOptions options;
  options.default_http_port = true;
  options.default_grpc_port = false;
  options.expect_grpc = false;
  options.default_storage_env = false;
  options.args = {"--config", config};
  ServerProcess server(options);
  REQUIRE(WaitReady(server.http_port(), 30000));
  const std::string banner = server.DumpLog();
  const std::string root_device = std::to_string(DeviceOf(root));
  const std::string shm_device = std::to_string(DeviceOf(shm_target.str()));
  CAPTURE(root_device, shm_device);
  REQUIRE(root_device != shm_device);
  REQUIRE(banner.find("part fs check : 已启用") != std::string::npos);
  REQUIRE(banner.find("root st_dev=" + root_device) != std::string::npos);
  REQUIRE(banner.find("staging st_dev=" + shm_device) != std::string::npos);
  REQUIRE(banner.find("persistent st_dev=" + shm_device) != std::string::npos);
}
