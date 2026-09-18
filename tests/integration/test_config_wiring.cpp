// =============================================================================
//  阶段 10 切片 1：C10.1~C10.8 —— 配置面接线（真实二进制）
// =============================================================================
//  判据（docs/04-implementation-plan.md「阶段 10」）：
//    C10.1 `--config` / `--print-config`（脱敏 + 来源）；非法配置拒绝启动
//    C10.2 优先级 cli > env > file > default，且来源可见
//    C10.3 `server.http.*` 生效（含"非法值拒绝启动"的反向用例）
//    C10.4 `storage.*`（io_engine 的拒绝/回退可见）
//    C10.5 `auth.*` + production 强校验（production + disabled → 拒绝启动）
//    C10.6 `observability.*`（metrics 开关/路径）
//    C10.7 未接通清单与实现一致（本文件只断言"声明接通的键确实生效"）
//    C10.8 自证（R1）：把组合根的配置加载去掉（退回只读环境变量）后，本文件的
//          **哪条断言会先失败**，以及注入验证的实际结果，见下面 §自证。
//
//  ---- §自证（R1，实际做过一次）----
//  注入方式：把 `src/main/server_main.cpp` 里的 `load_request.file_path = config_path;`
//  临时改成 `load_request.file_path.clear();`（等价"退回只读环境变量 + 只有 --set"）。
//  会失败的断言（按执行顺序，第一条即失败点）：
//    · TEST_CASE「配置文件改端口 → 该端口就绪」的
//      `REQUIRE(server.http_port() == from_file)` —— 因为端口退回默认 8080，
//      横幅解析出的端口不是配置文件里的值；
//    · 紧随其后的「worker_threads」`REQUIRE(metrics 值 == 7)`（退回默认 64）；
//    · 「auth.mode=jwt」的 `REQUIRE(info 含 authMode=jwt)`（退回默认 disabled）；
//    · 「优先级」里 `--set` 仍会赢（cli 层不走 file_path），但"文件层生效"的那条会失败。
//  即：**去掉 Load 的 file 层 → 至少 4 条断言失败**（不只是"可能失败"）。
//  恢复方式：把该行改回 `load_request.file_path = config_path;` 并重新构建。
//
//  ★ 全部断言都在**真实 `build/bin/fss_server`** 上做（组合根 R12：测试自己装配的
//    对象替代不了"进程真的读了这份配置"）。端口一律用 `--config` 显式给定或由
//    系统分配，避免并行 ctest 互踩。
// =============================================================================
#include <catch2/catch.hpp>

#include "http_fixture.h"  // Authed / TargetOf
#include "raw_http.h"
#include "server_process.h"
#include "temp_dir.h"

#include "common/crypto/crypto.h"
#include "common/json/json.h"
#include "infra/blob/posix/posix_blob_store.h"

#include <grpcpp/grpcpp.h>
#include <osdu/file/v1/file_service.grpc.pb.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using fss::test::Authed;
using fss::test::ProcessOutcome;
using fss::test::RawClient;
using fss::test::RunServerForExit;
using fss::test::ServerProcess;
using fss::test::ServerProcessOptions;
using fss::test::TempDir;

//  让内核分配一个空闲端口（配置文件里要写一个**具体**端口，不能用 0，
//  否则"文件里的端口生效"就无从断言）
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

struct HttpReply {
  bool transport_ok = false;
  int status = 0;
  std::string body;
};

HttpReply HttpGet(int port, const std::string& target,
                  const std::vector<std::string>& headers = {}) {
  HttpReply reply;
  RawClient client(port, /*tcp_nodelay=*/true);
  if (!client.Connect()) return reply;
  if (!client.SendRequest("GET", target, headers, "")) return reply;
  const auto response = client.ReadResponse(10000);
  if (!response.has_value()) return reply;
  reply.transport_ok = true;
  reply.status = response->status;
  reply.body = response->body;
  return reply;
}

bool WaitReady(int port, int attempts = 100) {
  for (int i = 0; i < attempts; ++i) {
    const auto reply = HttpGet(port, "/api/file/v2/readiness_check");
    if (reply.transport_ok && reply.status == 200) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

//  只用于"配置文件驱动的端口"：默认的 `FSS_HTTP_PORT=0` 会被 env 层覆盖文件层，
//  所以这类用例必须关掉它；gRPC 面也一并关掉（与配置面无关，少一个变量）。
ServerProcessOptions FileDrivenOptions(const std::vector<std::string>& extra_args) {
  ServerProcessOptions options;
  options.default_http_port = false;
  options.default_grpc_port = false;
  options.expect_grpc = false;
  options.args = extra_args;
  return options;
}

//  阶段 10 切片 2（C10.9~C10.12）：存储根/库路径全部由**配置文件**给出，
//  因此必须关掉夹具默认注入的 `FSS_STORAGE_ROOT` / `FSS_SQLITE_PATH`（env 优先级更高，
//  否则"配置里的 root"永远测不到）。
ServerProcessOptions SelfContainedOptions(
    const std::vector<std::string>& args,
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

//  造一个"够旧的残留 .tmp.*"：POSIX 驱动的 staging 容器目录是 `<root>/blobs/opendes-staging`。
//  ★ 用 `utime` 直接改 mtime（与 tests/hardening/test_metrics_and_gc.cpp 同源）：
//    `std::filesystem::last_write_time` 与 `system_clock` 的转换在 gcc 11 上不隐式。
std::string MakeOldTempFile(const std::string& storage_root, const std::string& key,
                            std::int64_t age_seconds) {
  const std::string dir = storage_root + "/blobs/opendes-staging";
  std::filesystem::create_directories(dir);
  const std::string path = dir + "/" + key;
  {
    std::ofstream out(path);
    out << "partial-upload-bytes";
  }
  struct utimbuf times {};
  times.actime = ::time(nullptr) - static_cast<std::time_t>(age_seconds);
  times.modtime = times.actime;
  REQUIRE(::utime(path.c_str(), &times) == 0);
  return path;
}

//  从 `uploadURL` 响应里取自签 URL 的 `exp=`（epoch 秒）。找不到 → -1。
std::int64_t ExpiresOf(const std::string& body) {
  const auto pos = body.find("exp=");
  if (pos == std::string::npos) return -1;
  return std::strtoll(body.c_str() + pos + 4, nullptr, 10);
}

std::string ReadWholeFileOrEmpty(const std::string& path) {
  std::ifstream in(path);
  if (!in.good()) return {};
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

}  // namespace

// =============================================================================
//  C10.1 / C10.3：配置文件改端口 → 服务在该端口就绪；横幅首行打印配置来源
// =============================================================================
TEST_CASE("★ C10.1/C10.3：配置文件里的 server.http.port 真的生效（真实进程就绪）",
          "[phase10][config][c10.1][c10.3]") {
  TempDir cfg_dir("fss_cfg_port");
  const int from_file = FreePort();
  REQUIRE(from_file > 0);
  const std::string config = WriteFile(cfg_dir, "port.json",
                                       "{\n  // 带注释的 JSON：端口来自配置文件\n"
                                       "  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", "
                                       "\"port\": " + std::to_string(from_file) + "}}\n}\n");

  ServerProcess server(FileDrivenOptions({"--config", config}));
  CAPTURE(from_file, server.http_port(), server.DumpLog());
  REQUIRE(server.http_port() == from_file);
  REQUIRE(WaitReady(server.http_port()));

  const auto readiness = HttpGet(server.http_port(), "/api/file/v2/readiness_check");
  REQUIRE(readiness.status == 200);
  //  横幅的**第一行**必须是配置来源（C10.1）—— 运维第一眼要知道读了哪份配置。
  //  注：stderr 的告警（如 auth.mode=disabled）与 stdout 合并进同一个日志，因此这里
  //  断言的是"第一条以 fss_server 开头的行"就是配置来源行。
  const std::string banner = server.DumpLog();
  const auto source_pos = banner.find("fss_server 配置来源");
  REQUIRE(source_pos != std::string::npos);
  REQUIRE(banner.find("fss_server") == source_pos);
  REQUIRE(banner.find(config) != std::string::npos);
}

// =============================================================================
//  C10.3：worker_threads 从配置文件生效 → /metrics 的值等于它
// =============================================================================
TEST_CASE("★ C10.3：配置文件里的 worker_threads 反映到 /metrics（fss_http_worker_threads）",
          "[phase10][config][c10.3]") {
  TempDir cfg_dir("fss_cfg_workers");
  const int port = FreePort();
  const std::string config =
      WriteFile(cfg_dir, "workers.json",
                "{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
                    std::to_string(port) +
                    ", \"worker_threads\": 7}},\n"
                    "  \"observability\": {\"metrics_enabled\": true, \"metrics_path\": "
                    "\"/metrics\"}\n}\n");
  ServerProcess server(FileDrivenOptions({"--config", config}));
  REQUIRE(server.http_port() == port);
  REQUIRE(WaitReady(port));

  const auto metrics = HttpGet(port, "/metrics");
  CAPTURE(metrics.body);
  REQUIRE(metrics.status == 200);
  REQUIRE(metrics.body.find("fss_http_worker_threads 7") != std::string::npos);
}

// =============================================================================
//  C10.5：auth.mode=jwt（来自配置文件）→ 无 token 401 + /v2/info authMode=jwt
//  正例对照（R16）：auth.mode=disabled 时同一个端点带 token 是 200 —— 证明 401
//  是鉴权造成的，而不是"路由根本不存在"。
// =============================================================================
TEST_CASE("★ C10.5：auth.mode=jwt 来自配置文件 → 401 且 /v2/info 显示 jwt",
          "[phase10][config][c10.5]") {
  TempDir cfg_dir("fss_cfg_auth");
  const int jwt_port = FreePort();
  const int open_port = FreePort();
  const std::string jwt_config =
      WriteFile(cfg_dir, "jwt.json",
                "{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
                    std::to_string(jwt_port) +
                    "}},\n  \"auth\": {\"mode\": \"jwt\", \"jwt\": {\"hmac_secret\": "
                    "\"test-secret\", \"verify_signature\": true}}\n}\n");
  const std::string open_config =
      WriteFile(cfg_dir, "open.json",
                "{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
                    std::to_string(open_port) +
                    "}},\n  \"auth\": {\"mode\": \"disabled\"}\n}\n");

  ServerProcess jwt_server(FileDrivenOptions({"--config", jwt_config}));
  REQUIRE(WaitReady(jwt_server.http_port()));
  const auto info = HttpGet(jwt_server.http_port(), "/api/file/v2/info");
  CAPTURE(info.body);
  REQUIRE(info.status == 200);
  REQUIRE(info.body.find("\"authMode\":\"jwt\"") != std::string::npos);
  //  无 token → 401（契约 §2.1）
  REQUIRE(HttpGet(jwt_server.http_port(), "/api/file/v2/files/uploadURL").status == 401);
  //  伪造 token → 401（jwt 模式真的在校验签名，而不是只看 token 是否为空）
  REQUIRE(HttpGet(jwt_server.http_port(), "/api/file/v2/files/uploadURL",
                  {"authorization: Bearer not-a-jwt", "data-partition-id: opendes"})
              .status == 401);

  //  正例对照：disabled 模式下带 token 的同一请求必须成功（否则上面的 401 可能只是路由缺失）
  ServerProcess open_server(FileDrivenOptions({"--config", open_config}));
  REQUIRE(WaitReady(open_server.http_port()));
  const auto open_upload = HttpGet(open_server.http_port(), "/api/file/v2/files/uploadURL",
                                   Authed());
  CAPTURE(open_upload.body);
  REQUIRE(open_upload.status == 200);
}

// =============================================================================
//  C10.2：优先级 —— cli(--set) > 通用 env > 旧别名 env > file
// =============================================================================
TEST_CASE("★ C10.2：同一端口同时由 file/--set/环境变量给出 → 高优先级生效",
          "[phase10][config][c10.2]") {
  TempDir cfg_dir("fss_cfg_priority");
  const int from_file = FreePort();
  const int from_cli = FreePort();
  const int from_alias = FreePort();
  const int from_generic_env = FreePort();
  const std::string config =
      WriteFile(cfg_dir, "port.json",
                "{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
                    std::to_string(from_file) + "}}\n}\n");

  SECTION("--set(cli) 覆盖文件") {
    auto options = FileDrivenOptions({"--config", config, "--set",
                                      "server.http.port=" + std::to_string(from_cli)});
    ServerProcess server(options);
    CAPTURE(from_file, from_cli, server.http_port(), server.DumpLog());
    REQUIRE(server.http_port() == from_cli);
    REQUIRE(WaitReady(from_cli));
  }

  SECTION("环境变量（旧别名 FSS_HTTP_PORT）覆盖文件") {
    auto options = FileDrivenOptions({"--config", config});
    options.env.emplace_back("FSS_HTTP_PORT", std::to_string(from_alias));
    ServerProcess server(options);
    CAPTURE(from_file, from_alias, server.http_port(), server.DumpLog());
    REQUIRE(server.http_port() == from_alias);
    REQUIRE(WaitReady(from_alias));
  }

  SECTION("通用名 FSS_SERVER_HTTP_PORT 优先于旧别名（歧义只有一个赢家）") {
    auto options = FileDrivenOptions({"--config", config});
    options.env.emplace_back("FSS_HTTP_PORT", std::to_string(from_alias));
    options.env.emplace_back("FSS_SERVER_HTTP_PORT", std::to_string(from_generic_env));
    ServerProcess server(options);
    CAPTURE(from_file, from_alias, from_generic_env, server.http_port());
    REQUIRE(server.http_port() == from_generic_env);
    REQUIRE(WaitReady(from_generic_env));
    //  歧义被显式告知（而不是静默地"看谁先到"）
    REQUIRE(server.DumpLog().find("FSS_HTTP_PORT") != std::string::npos);
  }
}

// =============================================================================
//  C10.3 反向 + C10.1 反向：非法/未知配置必须拒绝启动（exit 78）
// =============================================================================
TEST_CASE("★ C10.1/C10.5 反向：production+disabled / 未知键 / max_connections>worker_threads → exit 78",
          "[phase10][config][c10.1][c10.5]") {
  TempDir cfg_dir("fss_cfg_reject");

  SECTION("production + auth.mode=disabled → exit 78，并逐条说明 production") {
    const std::string config =
        WriteFile(cfg_dir, "prod_disabled.json",
                  "{\n  \"deployment\": {\"environment\": \"production\"},\n"
                  "  \"auth\": {\"mode\": \"disabled\", \"jwt\": {\"hmac_secret\": \"x\"}}\n}\n");
    const ProcessOutcome outcome = RunServerForExit({"--config", config});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("production") != std::string::npos);
    REQUIRE(outcome.output.find("auth.mode") != std::string::npos);
    REQUIRE(outcome.output.find("已启动") == std::string::npos);
  }

  SECTION("production + auth.mode=remote-entitlements → exit 78（生产要求 jwt）") {
    const std::string config = WriteFile(
        cfg_dir, "prod_remote.json",
        "{\n  \"deployment\": {\"environment\": \"production\"},\n"
        "  \"auth\": {\"mode\": \"remote-entitlements\", \"remote_entitlements\": "
        "{\"base_url\": \"http://entitlements:8080\", \"fail_closed\": true}}\n}\n");
    const ProcessOutcome outcome = RunServerForExit({"--config", config});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("production") != std::string::npos);
  }

  SECTION("配置文件里的未知键 → exit 78（逐条给出路径）") {
    const std::string config = WriteFile(cfg_dir, "unknown.json",
                                         "{\n  \"server\": {\"http\": {\"prot\": 8080}}\n}\n");
    const ProcessOutcome outcome = RunServerForExit({"--config", config});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("server.http.prot") != std::string::npos);
    REQUIRE(outcome.output.find("未知配置项") != std::string::npos);
  }

  SECTION("--set 未知键 → exit 78") {
    const ProcessOutcome outcome = RunServerForExit({"--set", "nope.key=1"});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("nope.key") != std::string::npos);
  }

  SECTION("配置文件不存在 → exit 78") {
    const ProcessOutcome outcome = RunServerForExit({"--config", cfg_dir.child("missing.json")});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("无法打开配置文件") != std::string::npos);
  }

  SECTION("未知命令行参数 → 拒绝启动（非 0，并打印用法）") {
    const ProcessOutcome outcome = RunServerForExit({"--bogus-flag"});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code != 0);
    REQUIRE(outcome.output.find("用法: fss_server") != std::string::npos);
  }

  SECTION("max_connections > worker_threads → exit 78（否则背压静默失效）") {
    const std::string config =
        WriteFile(cfg_dir, "backpressure.json",
                  "{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
                      std::to_string(FreePort()) +
                      ", \"worker_threads\": 4, \"max_connections\": 8}}\n}\n");
    const ProcessOutcome outcome = RunServerForExit({"--config", config});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("max_connections") != std::string::npos);
  }

  SECTION("非法取值（--set 给了 schema 不认的 durability）→ exit 78") {
    const ProcessOutcome outcome = RunServerForExit({"--set", "storage.posix.durability=never"});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("storage.posix.durability") != std::string::npos);
  }
}

// =============================================================================
//  C10.1：--print-config —— 脱敏 + 每项来源 + 成功退出
// =============================================================================
TEST_CASE("★ C10.1：--print-config 打印脱敏后的有效配置与来源，且不含明文密钥",
          "[phase10][config][c10.1]") {
  TempDir cfg_dir("fss_cfg_print");
  const std::string config =
      WriteFile(cfg_dir, "print.json",
                "{\n  \"server\": {\"http\": {\"port\": 9099}},\n"
                "  \"auth\": {\"mode\": \"jwt\", \"jwt\": {\"hmac_secret\": "
                "\"SUPERSECRET-VALUE\"}},\n"
                "  \"storage\": {\"posix\": {\"durability\": \"batch\"}}\n}\n");
  const ProcessOutcome outcome = RunServerForExit({"--config", config, "--print-config"});
  CAPTURE(outcome.exit_code, outcome.output);
  REQUIRE(outcome.exit_code == 0);
  //  ① 密钥必须打码（★ 绝不能出现明文）
  REQUIRE(outcome.output.find("SUPERSECRET-VALUE") == std::string::npos);
  REQUIRE(outcome.output.find("auth.jwt.hmac_secret = ***") != std::string::npos);
  //  ② 每项来源可见
  REQUIRE(outcome.output.find("server.http.port = 9099 [file]") != std::string::npos);
  REQUIRE(outcome.output.find("storage.posix.durability = batch [file]") != std::string::npos);
  //  ③ 未被配置覆盖的键标为 default
  REQUIRE(outcome.output.find("observability.metrics_path = /metrics [default]") !=
          std::string::npos);
}

// =============================================================================
//  C10.4：storage.io_engine —— uring 不可用时**拒绝启动**；auto 回退 blocking 且可见
// =============================================================================
TEST_CASE("★ C10.4：storage.io_engine=uring 不可用 → 拒绝启动并提示 check_io_uring.sh",
          "[phase10][config][c10.4]") {
  const ProcessOutcome outcome = RunServerForExit({"--set", "storage.io_engine=uring"});
  CAPTURE(outcome.exit_code, outcome.output);
  REQUIRE(outcome.exit_code == 78);
  REQUIRE(outcome.output.find("uring") != std::string::npos);
  REQUIRE(outcome.output.find("check_io_uring.sh") != std::string::npos);
}

TEST_CASE("★ C10.4：storage.io_engine=auto 回退 blocking（横幅 + /metrics 可见）",
          "[phase10][config][c10.4]") {
  TempDir cfg_dir("fss_cfg_io_auto");
  const int port = FreePort();
  const std::string config =
      WriteFile(cfg_dir, "io_auto.json",
                "{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
                    std::to_string(port) +
                    "}},\n  \"storage\": {\"io_engine\": \"auto\"}\n}\n");
  ServerProcess server(FileDrivenOptions({"--config", config}));
  REQUIRE(server.http_port() == port);
  REQUIRE(WaitReady(port));

  const std::string banner = server.DumpLog();
  CAPTURE(banner);
  REQUIRE(banner.find("io engine") != std::string::npos);
  REQUIRE(banner.find("blocking") != std::string::npos);
  //  ★ R11：探测/回退结果必须可见 —— /metrics 的 fss_io_engine 明确给出 engine 与 requested
  const auto metrics = HttpGet(port, "/metrics");
  CAPTURE(metrics.body);
  REQUIRE(metrics.status == 200);
  REQUIRE(metrics.body.find("fss_io_engine{engine=\"blocking\",requested=\"auto\"} 1") !=
          std::string::npos);
}

// =============================================================================
//  C10.6：observability.metrics_enabled=false 不注册 /metrics；metrics_path 可改
// =============================================================================
TEST_CASE("★ C10.6：metrics_enabled=false 不注册 /metrics；metrics_path 生效",
          "[phase10][config][c10.6]") {
  TempDir cfg_dir("fss_cfg_metrics");
  const int off_port = FreePort();
  const int moved_port = FreePort();
  const std::string off_config =
      WriteFile(cfg_dir, "metrics_off.json",
                "{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
                    std::to_string(off_port) +
                    "}},\n  \"observability\": {\"metrics_enabled\": false}\n}\n");
  const std::string moved_config =
      WriteFile(cfg_dir, "metrics_moved.json",
                "{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
                    std::to_string(moved_port) +
                    "}},\n  \"observability\": {\"metrics_enabled\": true, \"metrics_path\": "
                    "\"/internal/metrics\"}\n}\n");

  ServerProcess off_server(FileDrivenOptions({"--config", off_config}));
  REQUIRE(WaitReady(off_server.http_port()));
  REQUIRE(HttpGet(off_server.http_port(), "/metrics").status == 404);

  ServerProcess moved_server(FileDrivenOptions({"--config", moved_config}));
  REQUIRE(WaitReady(moved_server.http_port()));
  const auto moved = HttpGet(moved_server.http_port(), "/internal/metrics");
  CAPTURE(moved.status, moved.body);
  REQUIRE(moved.status == 200);
  REQUIRE(moved.body.find("fss_http_requests_total") != std::string::npos);
  //  老路径不再存在（不是"两边都注册"）
  REQUIRE(HttpGet(moved_server.http_port(), "/metrics").status == 404);
}

// =============================================================================
//  C10.4：storage.posix.root / durability 从配置文件生效（print-config 的 file 来源）
// =============================================================================
TEST_CASE("★ C10.4：storage.* 从配置文件生效（root / durability / instance_id 进 schema 校验）",
          "[phase10][config][c10.4]") {
  TempDir cfg_dir("fss_cfg_storage");
  TempDir data_dir("fss_cfg_storage_data");
  const std::string config =
      WriteFile(cfg_dir, "storage.json",
                "{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
                    std::to_string(FreePort()) +
                    "}},\n  \"storage\": {\"driver\": \"posix\", \"posix\": {\"root\": \"" +
                    data_dir.child("blobs-root") +
                    "\", \"durability\": \"batch\", \"fsync_threshold_bytes\": 0}},\n"
                    "  \"deployment\": {\"instance_id\": \"node-a\"}\n}\n");
  const ProcessOutcome outcome = RunServerForExit({"--config", config, "--print-config"});
  CAPTURE(outcome.exit_code, outcome.output);
  REQUIRE(outcome.exit_code == 0);
  REQUIRE(outcome.output.find("storage.posix.root = " + data_dir.child("blobs-root") +
                              " [file]") != std::string::npos);
  REQUIRE(outcome.output.find("storage.posix.durability = batch [file]") != std::string::npos);
  REQUIRE(outcome.output.find("deployment.instance_id = node-a [file]") != std::string::npos);

  //  `fsync_threshold_bytes > 0` 也必须能从**配置面**（--set）给出（否则 batch 只能经旧别名
  //  表达"按大小摊销"，阈值 0 = 所有对象都 fsync —— 见 docs/operations.md §5.1）。
  const ProcessOutcome via_cli =
      RunServerForExit({"--config", config, "--set", "storage.posix.fsync_threshold_bytes=12345",
                        "--print-config"});
  CAPTURE(via_cli.exit_code, via_cli.output);
  REQUIRE(via_cli.exit_code == 0);
  REQUIRE(via_cli.output.find("storage.posix.fsync_threshold_bytes = 12345 [cli]") !=
          std::string::npos);
}

// =============================================================================
//  ---- 阶段 10 切片 2（C10.9~C10.12）----
// =============================================================================
//  C10.9：组合根真的装配 `GcTask` 并按 `gc.interval_seconds` 周期运行；`gc.*` 的
//         非默认值生效；`fss_gc_*` 在**真实进程**的 `/metrics` 可见；`--once` 跑一轮即退出。
//  C10.10：`config/fss.example.json` 作为 `--config`（只覆盖环境相关项）真的能起来；
//          改坏一个键 → 拒绝启动（exit 78）。
//  C10.11：未实现能力的非默认值 → 拒绝启动（exit 78 + "未实现 + 下一步"）；
//          默认/合法值 → 能启动（R16 正例对照）。
//  C10.12：`expiry.default` / `expiry.max` 作用于签发 URL 的 TTL（夹紧语义 + 边界）。
//
//  ★ 等待一律**轮询实际条件**（`/metrics` 是否出现该计数、文件是否消失），
//    不用固定 sleep（AGENTS §4.3）。
// =============================================================================

TEST_CASE("★ C10.9：GC 调度真的跑起来（gc.enabled=true + interval=1s；旧 .tmp.* 被清）",
          "[phase10][config][c10.9]") {
  TempDir cfg_dir("fss_cfg_gc");
  TempDir data_dir("fss_cfg_gc_data");
  const std::string root = data_dir.child("store");
  const int port = FreePort();
  REQUIRE(port > 0);

  const std::string residue = MakeOldTempFile(root, "residue.bin.tmp.local.7.1", 3 * 24 * 3600);
  const std::string fresh = MakeOldTempFile(root, "inflight.bin.tmp.local.7.2", 5);

  const std::string config =
      WriteFile(cfg_dir, "gc_on.json",
                "{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
                    std::to_string(port) +
                    "}},\n"
                    "  \"storage\": {\"posix\": {\"root\": \"" + root + "\"}},\n"
                    "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child("loc.db") +
                    "\"}},\n"
                    "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child("meta.db") +
                    "\"}},\n"
                    "  \"self_signed\": {\"signing_key\": \"gc-test\"},\n"
                    "  \"auth\": {\"mode\": \"disabled\"},\n"
                    "  \"gc\": {\"enabled\": true, \"dry_run\": false, \"require_lease_expiry\": "
                    "true, \"staging_ttl_hours\": 24, \"orphan_grace_hours\": 72, "
                    "\"interval_seconds\": 1}\n}\n");

  ServerProcess server(SelfContainedOptions({"--config", config}));
  REQUIRE(server.http_port() == port);
  REQUIRE(WaitReady(port));

  //  横幅必须如实说明"GC 在跑 + 参数"（C10.9：不启动也要说明原因）
  const std::string banner = server.DumpLog();
  CAPTURE(banner);
  REQUIRE(banner.find("gc             : 已启动（间隔 1s") != std::string::npos);
  REQUIRE(banner.find("dry_run=false") != std::string::npos);

  //  ★ 轮询实际条件：① `.tmp.*` 真的被清掉；② `/metrics` 上 `fss_gc_*` 真的动过。
  bool residue_gone = false;
  bool metric_visible = false;
  std::string last_metrics;
  for (int attempt = 0; attempt < 200; ++attempt) {
    const bool gone = !std::filesystem::exists(residue);
    const auto metrics = HttpGet(port, "/metrics");
    if (metrics.status == 200) last_metrics = metrics.body;
    const bool counted = last_metrics.find("fss_gc_runs_total{") != std::string::npos &&
                         last_metrics.find("fss_gc_tmp_removed_total 1") != std::string::npos;
    if (gone && counted) {
      residue_gone = true;
      metric_visible = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CAPTURE(last_metrics, residue_gone, metric_visible);
  REQUIRE(residue_gone);     // 够旧的残留 tmp 被清理
  REQUIRE(metric_visible);   // 真实进程 /metrics 上的 fss_gc_*（C9.6/C10.9）
  //  ★ 在途保护：mtime 太新的 `.tmp.*` 必须**还在**（TTL 判据，绝不是"见到 tmp 就删"）
  REQUIRE(std::filesystem::exists(fresh));
}

TEST_CASE("★ C10.9 正例对照（R16）：gc.enabled=false → 不启动调度，/metrics 无 fss_gc_*",
          "[phase10][config][c10.9]") {
  TempDir cfg_dir("fss_cfg_gc_off");
  TempDir data_dir("fss_cfg_gc_off_data");
  const std::string root = data_dir.child("store");
  const int port = FreePort();
  const std::string config =
      WriteFile(cfg_dir, "gc_off.json",
                "{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
                    std::to_string(port) +
                    "}},\n"
                    "  \"storage\": {\"posix\": {\"root\": \"" + root + "\"}},\n"
                    "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child("loc.db") +
                    "\"}},\n"
                    "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child("meta.db") +
                    "\"}},\n"
                    "  \"self_signed\": {\"signing_key\": \"gc-test\"},\n"
                    "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
  //  默认（无 gc.* 配置）也必须与"接线前"一致：不跑 GC。
  ServerProcess server(SelfContainedOptions({"--config", config}));
  REQUIRE(WaitReady(port));
  const std::string banner = server.DumpLog();
  CAPTURE(banner);
  REQUIRE(banner.find("gc             : 未启动（gc.enabled=false") != std::string::npos);
  const auto metrics = HttpGet(port, "/metrics");
  CAPTURE(metrics.body);
  REQUIRE(metrics.status == 200);
  REQUIRE(metrics.body.find("fss_gc_runs_total") == std::string::npos);
}

TEST_CASE("★ C10.9：--once 跑一轮 GC 后退出 0，并打印 GcReport 摘要",
          "[phase10][config][c10.9]") {
  TempDir data_dir("fss_once_data");
  const std::string root = data_dir.child("store");
  const std::string residue = MakeOldTempFile(root, "once.bin.tmp.local.8.1", 3 * 24 * 3600);

  const ProcessOutcome outcome =
      RunServerForExit({"--once", "--set", "storage.posix.root=" + root, "--set",
                        "location.sqlite.path=" + data_dir.child("loc.db"), "--set",
                        "metadata.sqlite.path=" + data_dir.child("meta.db"), "--set",
                        "self_signed.signing_key=x", "--set", "gc.dry_run=false", "--set",
                        "gc.staging_ttl_hours=24"});
  CAPTURE(outcome.exit_code, outcome.output);
  REQUIRE(outcome.exit_code == 0);
  REQUIRE(outcome.output.find("gc once : partition=opendes dry_run=false") != std::string::npos);
  REQUIRE(outcome.output.find("tmp_removed=1") != std::string::npos);
  REQUIRE(outcome.output.find("errors=0") != std::string::npos);
  REQUIRE_FALSE(std::filesystem::exists(residue));
}

TEST_CASE("★ C10.10：config/fss.example.json 作为 --config 真的能启动（readiness 200）",
          "[phase10][config][c10.10]") {
  TempDir data_dir("fss_example_data");
  const std::string example = std::string(FSS_REPO_ROOT) + "/config/fss.example.json";
  REQUIRE(std::filesystem::exists(example));
  const int port = FreePort();

  //  只覆盖**环境相关项**（路径/端口/密钥）：样例文件里的 7 个 `${ENV:...}` 必须先注入，
  //  否则加载器把"引用未解析"计为问题 → exit 78（fail-closed，见 operations.md §0）。
  std::vector<std::pair<std::string, std::string>> env = {
      {"FSS_S3_ACCESS_KEY", "example-access"},
      {"FSS_S3_SECRET_KEY", "example-secret"},
      {"FSS_TRANSFER_SIGNING_KEY", "example-transfer"},
      {"FSS_PG_DSN", "postgres://example"},
      {"FSS_JWT_HMAC_SECRET", "example-jwt"},
      {"FSS_STORAGE_TOKEN", "example-storage-token"},
      {"FSS_GRPC_PORT", "0"},  // 样例的 50051 会在并行 ctest 下互踩；gRPC 面与 C10.10 无关
  };
  const std::vector<std::string> args = {
      "--config", example,
      "--set", "server.http.bind=127.0.0.1",
      "--set", "server.http.port=" + std::to_string(port),
      "--set", "storage.posix.root=" + data_dir.child("store"),
      "--set", "location.sqlite.path=" + data_dir.child("location.db"),
      "--set", "metadata.sqlite.path=" + data_dir.child("metadata.db"),
  };

  ServerProcess server(SelfContainedOptions(args, env));
  REQUIRE(server.http_port() == port);
  REQUIRE(WaitReady(port));
  const auto readiness = HttpGet(port, "/api/file/v2/readiness_check");
  CAPTURE(server.DumpLog());
  REQUIRE(readiness.status == 200);
  REQUIRE(readiness.body == "File service is ready");

  SECTION("反面：故意把样例里的 server.http.port 改成 -1 → 拒绝启动（exit 78）") {
    std::string broken = ReadWholeFileOrEmpty(example);
    REQUIRE_FALSE(broken.empty());
    const std::string needle = "\"port\": 8080";
    REQUIRE(broken.find(needle) != std::string::npos);
    broken.replace(broken.find(needle), needle.size(), "\"port\": -1");
    const std::string broken_path = data_dir.child("broken.json");
    {
      std::ofstream out(broken_path, std::ios::binary | std::ios::trunc);
      out << broken;
    }
    std::vector<std::pair<std::string, std::string>> broken_env = env;
    const ProcessOutcome outcome =
        RunServerForExit({"--config", broken_path, "--set", "storage.posix.root=" +
                                                             data_dir.child("store2"),
                          "--set", "location.sqlite.path=" + data_dir.child("loc2.db"),
                          "--set", "metadata.sqlite.path=" + data_dir.child("meta2.db")},
                         broken_env);
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("server.http.port") != std::string::npos);
    REQUIRE(outcome.output.find("已启动") == std::string::npos);
  }
}

TEST_CASE("★ C10.11：未实现能力的非默认值必须拒绝启动（exit 78）；合法值必须能启动",
          "[phase10][config][c10.11]") {
  SECTION("正例对照（R16）：所有守卫键都取默认/合法值 → 真的能启动") {
    TempDir cfg_dir("fss_guard_ok");
    TempDir data_dir("fss_guard_ok_data");
    const int port = FreePort();
    const std::string config =
        WriteFile(cfg_dir, "legal.json",
                  "{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
                      std::to_string(port) +
                      ", \"max_connections_per_partition\": 0, \"large_file_plane\": "
                      "{\"enabled\": false}},\n"
                      "   \"grpc\": {\"max_message_bytes\": 4194304, \"streaming_chunk_bytes\": "
                      "262144}},\n"
                      "  \"storage\": {\"proxy_mode\": \"auto\", \"io_engine\": \"blocking\", "
                      "\"io_uring\": {\"register_files\": false}, \"posix\": {\"root\": \"" +
                      data_dir.child("store") + "\"}},\n"
                      "  \"location\": {\"repository\": \"sqlite\", \"sqlite\": {\"path\": \"" +
                      data_dir.child("loc.db") + "\"}},\n"
                      "  \"metadata\": {\"repository\": \"sqlite\", \"sqlite\": {\"path\": \"" +
                      data_dir.child("meta.db") + "\"}},\n"
                      "  \"self_signed\": {\"signing_key\": \"g\", \"single_use_nonce\": false, "
                      "\"nonce_store\": \"memory\"},\n"
                      "  \"leases\": {\"enabled\": false, \"ttl_seconds\": 60, "
                      "\"renew_interval_seconds\": 20, \"time_source\": \"database\"},\n"
                      "  \"leader_election\": {\"enabled\": false},\n"
                      "  \"legal\": {\"validator\": \"noop\"}, \"schema\": {\"validator\": "
                      "\"noop\"},\n"
                      "  \"events\": {\"publisher\": \"log\"},\n"
                      "  \"partition\": {\"registry\": \"file\"},\n"
                      "  \"deployment\": {\"mode\": \"single\", \"clock_skew_tolerance_seconds\": "
                      "60},\n"
                      "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
    ServerProcess server(SelfContainedOptions({"--config", config}));
    REQUIRE(server.http_port() == port);
    CAPTURE(server.DumpLog());
    REQUIRE(WaitReady(port));
    REQUIRE(HttpGet(port, "/api/file/v2/readiness_check").status == 200);
  }

  SECTION("反向：每个未实现能力的非默认值 → exit 78 + 可读原因 + 没有进入服务状态") {
    struct RejectCase {
      const char* key;
      const char* value;
      const char* needle;  // 稳定出现在拒绝原因里的片段
    };
    const std::vector<RejectCase> cases = {
        {"server.http.large_file_plane.enabled", "true", "large_file_plane"},
        {"server.http.max_connections_per_partition", "3", "max_connections_per_partition"},
        {"server.grpc.max_message_bytes", "1024", "max_message_bytes"},
        {"server.grpc.streaming_chunk_bytes", "4096", "streaming_chunk_bytes"},
        {"storage.proxy_mode", "always", "proxy_mode"},
        {"storage.io_uring.register_files", "true", "register_files"},
        {"storage.io_engine", "uring", "io_engine=uring"},
        {"metadata.repository", "postgres", "metadata.repository=postgres"},
        {"location.repository", "postgres", "location.repository=postgres"},
        {"deployment.mode", "multi", "multi"},
        {"leases.enabled", "true", "leases.enabled=true"},
        {"leader_election.enabled", "true", "leader_election.enabled=true"},
        {"events.publisher", "webhook", "events.publisher=webhook"},
        {"legal.validator", "remote", "legal.validator=remote"},
        {"schema.validator", "remote", "schema.validator=remote"},
        {"self_signed.single_use_nonce", "true", "single_use_nonce=true"},
        {"self_signed.nonce_store", "postgres", "nonce_store=postgres"},
        {"partition.registry", "remote", "partition.registry=remote"},
        {"deployment.clock_skew_tolerance_seconds", "30", "clock_skew_tolerance_seconds"},
    };
    for (const auto& test_case : cases) {
      const std::string assignment = std::string(test_case.key) + "=" + test_case.value;
      const ProcessOutcome outcome = RunServerForExit({"--set", assignment});
      INFO("注入: " << assignment << "\n输出:\n" << outcome.output);
      REQUIRE(outcome.exit_code == 78);
      REQUIRE(outcome.output.find("拒绝启动") != std::string::npos);
      REQUIRE(outcome.output.find(test_case.needle) != std::string::npos);
      REQUIRE(outcome.output.find("已启动") == std::string::npos);
    }
  }
}

TEST_CASE("★ C10.12：expiry.default / expiry.max 作用于签发 URL 的 TTL（夹紧 + 边界）",
          "[phase10][config][c10.12]") {
  TempDir cfg_dir("fss_cfg_expiry");
  TempDir data_dir("fss_cfg_expiry_data");
  const int port = FreePort();
  const std::string config = WriteFile(
      cfg_dir, "expiry.json",
      "{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
          std::to_string(port) +
          "}},\n"
          "  \"storage\": {\"posix\": {\"root\": \"" + data_dir.child("store") + "\"}},\n"
          "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child("loc.db") + "\"}},\n"
          "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child("meta.db") + "\"}},\n"
          "  \"self_signed\": {\"signing_key\": \"expiry-test\"},\n"
          "  \"auth\": {\"mode\": \"disabled\"},\n"
          "  \"expiry\": {\"default\": \"5M\", \"max\": \"10M\"}\n}\n");

  ServerProcess server(SelfContainedOptions({"--config", config}));
  REQUIRE(WaitReady(port));
  const std::string banner = server.DumpLog();
  CAPTURE(banner);
  REQUIRE(banner.find("expiry         : default=5M（300s）max=10M（600s）") != std::string::npos);

  const auto now = static_cast<std::int64_t>(::time(nullptr));
  const std::string endpoint = "/api/file/v2/files/uploadURL";

  //  ① 未提供 expiryTime → 配置里的 default（300s），而**不是**契约默认的 3600s
  {
    const auto reply = HttpGet(port, endpoint, Authed());
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 200);
    const std::int64_t exp = ExpiresOf(reply.body);
    REQUIRE(exp >= now + 295);
    REQUIRE(exp <= now + 305);
  }
  //  ② 边界：请求值正好等于 expiry.max（10M）→ 必须通过，且 TTL = max
  {
    const auto reply = HttpGet(port, endpoint + "?expiryTime=10M", Authed());
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 200);
    const std::int64_t exp = ExpiresOf(reply.body);
    REQUIRE(exp >= now + 595);
    REQUIRE(exp <= now + 605);
  }
  //  ③ 超上限（60M）→ **静默夹紧**到 max（真实语义，不是拒绝）：200 且 TTL = max
  {
    const auto reply = HttpGet(port, endpoint + "?expiryTime=60M", Authed());
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 200);
    const std::int64_t exp = ExpiresOf(reply.body);
    REQUIRE(exp >= now + 595);
    REQUIRE(exp <= now + 605);
  }
  //  ④ 语法非法（5X）→ 400 + 固定消息（与"超限夹紧"严格区分）
  {
    const auto reply = HttpGet(port, endpoint + "?expiryTime=5X", Authed());
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 400);
    REQUIRE(reply.body.find("expiryTime pattern isn't supported") != std::string::npos);
  }
}

// =============================================================================
//  ---- 阶段 10 切片 3（C10.13~C10.15）----
// =============================================================================
//  C10.13：`observability.audit_fail_closed` 真的决定"审计写入失败是否让请求失败"。
//          可驱动接缝：`FSS_AUDIT_FAULT_INJECT=1`（测试/演练用的故障注入，**不是**配置键，
//          见 server_main.cpp 的 FailingAuditLogger）。正例对照：同一个坏后端下
//          fail_closed=false 必须照常 200（R16）。
//  C10.14：SQLite 调优键作用到两个仓储 —— `journal_mode` 用 `python3 sqlite3` 读回
//          数据库文件（WAL 持久在文件头），`busy_timeout` 由启动横幅确认（它只作用于
//          连接，不落盘，无法从文件读回 —— 如实标注）。
//  C10.15：`auth.jwt.roles_claim` / `auth.local_roles.*` / `server.grpc.enabled`。
// =============================================================================
namespace {

//  ---- C10.13：审计注入故障下的"必然失败"后端 ----
struct AuditConfig {
  std::string path;
  int port = 0;
};

AuditConfig WriteAuditConfig(const TempDir& dir, const std::string& name, int port,
                             const std::string& root, bool fail_closed) {
  AuditConfig out;
  out.port = port;
  out.path = WriteFile(
      dir, name,
      std::string("{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": ") +
          std::to_string(port) + "}},\n  \"storage\": {\"posix\": {\"root\": \"" + root +
          "\"}},\n  \"location\": {\"sqlite\": {\"path\": \"" + dir.child(name + ".loc.db") +
          "\"}},\n  \"metadata\": {\"sqlite\": {\"path\": \"" + dir.child(name + ".meta.db") +
          "\"}},\n  \"self_signed\": {\"signing_key\": \"audit-test\"},\n  \"auth\": {\"mode\": "
          "\"disabled\"},\n  \"observability\": {\"audit_enabled\": true, \"audit_fail_closed\": " +
          (fail_closed ? "true" : "false") + "}\n}\n");
  return out;
}

//  ---- C10.14：用 python3 读回数据库文件的 journal_mode（环境没有 sqlite3 CLI）----
std::string RunCapture(const std::string& command) {
  std::string out;
  std::FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) return out;
  char buffer[256] = {0};
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) out += buffer;
  ::pclose(pipe);
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

std::string PythonJournalMode(const std::string& db_path) {
  const std::string script =
      "import sqlite3,sys;c=sqlite3.connect(sys.argv[1]);"
      "print(c.execute('PRAGMA journal_mode').fetchone()[0])";
  return RunCapture("python3 -c \"" + script + "\" \"" + db_path + "\"");
}

//  ---- C10.15：手工签一个 HS256 JWT（claim 名与是否带 roles 都可控）----
std::string MintToken(const std::string& secret, const std::string& email,
                      const std::string& claim_name, const std::vector<std::string>& roles,
                      bool include_claim) {
  const auto now = static_cast<std::int64_t>(::time(nullptr));
  fss::json::Value header;
  header["alg"] = "HS256";
  fss::json::Value payload;
  payload["sub"] = "user-1";
  payload["email"] = email;
  payload["data-partition-id"] = "opendes";
  payload["nbf"] = now - 10;
  payload["exp"] = now + 3600;
  if (include_claim) payload[claim_name] = roles;
  const std::string header_b64 = fss::crypto::Base64UrlEncode(fss::json::Dump(header));
  const std::string payload_b64 = fss::crypto::Base64UrlEncode(fss::json::Dump(payload));
  const std::string signing_input = header_b64 + "." + payload_b64;
  const auto digest = fss::crypto::HmacSha256(secret, signing_input);
  return signing_input + "." + fss::crypto::Base64UrlEncode(digest);
}

bool TcpConnects(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  ::close(fd);
  return ok;
}

constexpr char kAuditJwtSecret[] = "c10.15-hs256-secret";
//  `GET .../files/uploadURL` 要求 `service.file.editors`（契约 §1.3），且无需请求体 ——
//  比 `getFileList` 更适合只验"角色判定"（后者的 Items 形状另有 400 规则）。
constexpr char kEditorsEndpoint[] = "/api/file/v2/files/uploadURL";

}  // namespace

TEST_CASE("★ C10.13：audit_fail_closed=true → 审计写入失败让请求 500；false → 仍 200（正例对照）",
          "[phase10][config][c10.13]") {
  TempDir cfg_dir("fss_cfg_audit");
  TempDir data_dir("fss_cfg_audit_data");
  const int closed_port = FreePort();
  const int open_port = FreePort();
  const auto closed_cfg =
      WriteAuditConfig(cfg_dir, "closed.json", closed_port, data_dir.child("store_a"), true);
  const auto open_cfg =
      WriteAuditConfig(cfg_dir, "open.json", open_port, data_dir.child("store_b"), false);
  const std::vector<std::pair<std::string, std::string>> inject = {
      {"FSS_AUDIT_FAULT_INJECT", "1"}};

  SECTION("fail_closed=true：审计后端必然失败 → 请求 500，且响应里没有成功载荷") {
    ServerProcess server(SelfContainedOptions({"--config", closed_cfg.path}, inject));
    REQUIRE(server.http_port() == closed_port);
    REQUIRE(WaitReady(closed_port));
    CAPTURE(server.DumpLog());
    REQUIRE(server.DumpLog().find("fail_closed=true") != std::string::npos);
    REQUIRE(server.DumpLog().find("故障注入") != std::string::npos);

    const auto reply = HttpGet(closed_port, "/api/file/v2/files/uploadURL", Authed());
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 500);
    //  ★ 绝不谎报成功：不能出现成功载荷（uploadURL / signedUrl）
    REQUIRE(reply.body.find("SignedURL") == std::string::npos);
  }

  SECTION("正例对照（R16）：fail_closed=false + 同一个坏后端 → 照常 200") {
    ServerProcess server(SelfContainedOptions({"--config", open_cfg.path}, inject));
    REQUIRE(server.http_port() == open_port);
    REQUIRE(WaitReady(open_port));
    CAPTURE(server.DumpLog());
    REQUIRE(server.DumpLog().find("fail_closed=false") != std::string::npos);
    const auto reply = HttpGet(open_port, "/api/file/v2/files/uploadURL", Authed());
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 200);
    REQUIRE(reply.body.find("SignedURL") != std::string::npos);
  }

  SECTION("无注入（默认）时 audit_fail_closed 不影响正常请求") {
    ServerProcess server(SelfContainedOptions({"--config", closed_cfg.path}));
    REQUIRE(WaitReady(closed_port));
    const auto reply = HttpGet(closed_port, "/api/file/v2/files/uploadURL", Authed());
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 200);
  }
}

TEST_CASE("★ C10.14：location.sqlite.journal_mode 真的作用到位置仓储（python3 读回）",
          "[phase10][config][c10.14]") {
  TempDir cfg_dir("fss_cfg_sqlite");
  TempDir data_dir("fss_cfg_sqlite_data");

  const auto run_with = [&](const std::string& name, const std::string& journal_mode) {
    const int port = FreePort();
    const std::string db = data_dir.child(name + ".db");
    const std::string config = WriteFile(
        cfg_dir, name + ".json",
        std::string("{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": ") +
            std::to_string(port) + "}},\n  \"storage\": {\"posix\": {\"root\": \"" +
            data_dir.child(name + "-store") + "\"}},\n  \"location\": {\"sqlite\": {\"path\": \"" +
            db + "\", \"journal_mode\": \"" + journal_mode +
            "\", \"busy_timeout_ms\": 7000}},\n  \"metadata\": {\"sqlite\": {\"path\": \"" +
            data_dir.child(name + "-meta.db") +
            "\", \"busy_timeout_ms\": 8000}},\n  \"self_signed\": {\"signing_key\": \"slite\"},\n"
            "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
    return std::make_tuple(db, config, port);
  };

  SECTION("WAL（默认）→ python3 读回 'wal'；busy_timeout 由横幅确认") {
    const auto [db, config, port] = run_with("wal", "WAL");
    ServerProcess server(SelfContainedOptions({"--config", config}));
    REQUIRE(WaitReady(port));
    CAPTURE(server.DumpLog());
    REQUIRE(server.DumpLog().find("journal_mode=WAL") != std::string::npos);
    REQUIRE(server.DumpLog().find("busy_timeout=7000ms") != std::string::npos);
    REQUIRE(server.DumpLog().find("metadata busy_timeout=8000ms") != std::string::npos);
    const std::string mode = PythonJournalMode(db);
    CAPTURE(db, mode);
    REQUIRE(mode == "wal");
  }

  SECTION("DELETE → python3 读回 'delete'（证明配置真的改了行为，而不是恒为 WAL）") {
    const auto [db, config, port] = run_with("del", "DELETE");
    ServerProcess server(SelfContainedOptions({"--config", config}));
    REQUIRE(WaitReady(port));
    CAPTURE(server.DumpLog());
    REQUIRE(server.DumpLog().find("journal_mode=DELETE") != std::string::npos);
    const std::string mode = PythonJournalMode(db);
    CAPTURE(db, mode);
    REQUIRE(mode == "delete");
  }

  SECTION("TRUNCATE → 拒绝启动（只接通 WAL|DELETE 两档，不静默当成 DELETE）") {
    const auto [db, config, port] = run_with("trunc", "TRUNCATE");
    (void)db;
    (void)port;
    const ProcessOutcome outcome = RunServerForExit({"--config", config});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("location.sqlite.journal_mode=TRUNCATE") != std::string::npos);
    REQUIRE(outcome.output.find("已启动") == std::string::npos);
  }
}

TEST_CASE("★ C10.15：auth.jwt.roles_claim 与 auth.local_roles.* 决定 200/403（真实 JWT）",
          "[phase10][config][c10.15]") {
  TempDir cfg_dir("fss_cfg_roles");
  TempDir data_dir("fss_cfg_roles_data");
  const int with_roles_port = FreePort();
  const int without_roles_port = FreePort();

  const auto jwt_config = [&](const std::string& name, int port, bool with_local_roles) {
    std::string body =
        std::string("{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": ") +
        std::to_string(port) + "}},\n  \"storage\": {\"posix\": {\"root\": \"" +
        data_dir.child(name + "-store") +
        "\"}},\n  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child(name + "-loc.db") +
        "\"}},\n  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child(name + "-meta.db") +
        "\"}},\n  \"self_signed\": {\"signing_key\": \"roles\"},\n  \"auth\": {\"mode\": \"jwt\", "
        "\"jwt\": {\"hmac_secret\": \"" +
        std::string(kAuditJwtSecret) + "\", \"verify_signature\": true, \"roles_claim\": \"myroles\"}";
    if (with_local_roles) {
      body += ", \"local_roles\": {\"viewer@example.com\": [\"service.file.editors\"]}";
    }
    body += "}\n}\n";
    return WriteFile(cfg_dir, name + ".json", body);
  };

  const std::string with_roles = jwt_config("roles_on", with_roles_port, true);
  const std::string without_roles = jwt_config("roles_off", without_roles_port, false);

  ServerProcess server(SelfContainedOptions({"--config", with_roles}));
  REQUIRE(WaitReady(with_roles_port));

  //  ① roles_claim=myroles：token 用 `myroles` 携带角色 → 200
  {
    const std::string token = MintToken(kAuditJwtSecret, "someone@example.com", "myroles",
                                        {"service.file.editors"}, true);
    const auto reply = HttpGet(with_roles_port, kEditorsEndpoint,
                               {"authorization: Bearer " + token, "data-partition-id: opendes"});
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 200);
  }
  //  ② 同一个 token 内容放在**默认 claim 名** `roles` 里 → 403（roles_claim 真的在生效）
  {
    const std::string token = MintToken(kAuditJwtSecret, "someone@example.com", "roles",
                                        {"service.file.editors"}, true);
    const auto reply = HttpGet(with_roles_port, kEditorsEndpoint,
                               {"authorization: Bearer " + token, "data-partition-id: opendes"});
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 403);
  }
  //  ③ 无 roles claim，但 email 命中 `auth.local_roles` → 200（静态角色表真的被装配）
  {
    const std::string token =
        MintToken(kAuditJwtSecret, "viewer@example.com", "myroles", {}, false);
    const auto reply = HttpGet(with_roles_port, kEditorsEndpoint,
                               {"authorization: Bearer " + token, "data-partition-id: opendes"});
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 200);
  }

  //  ④ 正例对照（R16）：**没有** local_roles 的实例上，同一个 token → 403
  {
    ServerProcess control(SelfContainedOptions({"--config", without_roles}));
    REQUIRE(WaitReady(without_roles_port));
    const std::string token =
        MintToken(kAuditJwtSecret, "viewer@example.com", "myroles", {}, false);
    const auto reply = HttpGet(without_roles_port, kEditorsEndpoint,
                               {"authorization: Bearer " + token, "data-partition-id: opendes"});
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 403);
  }
}

TEST_CASE("★ C10.15：server.grpc.enabled=false 不开端口；true 时 GetInfo 可用",
          "[phase10][config][c10.15]") {
  TempDir cfg_dir("fss_cfg_grpc");
  TempDir data_dir("fss_cfg_grpc_data");

  const auto grpc_config = [&](const std::string& name, int http_port, int grpc_port,
                               bool enabled) {
    return WriteFile(
        cfg_dir, name + ".json",
        std::string("{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": ") +
            std::to_string(http_port) + "}, \"grpc\": {\"enabled\": " +
            (enabled ? "true" : "false") + ", \"bind\": \"127.0.0.1\", \"port\": " +
            std::to_string(grpc_port) +
            "}},\n  \"storage\": {\"posix\": {\"root\": \"" + data_dir.child(name + "-store") +
            "\"}},\n  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child(name + "-loc.db") +
            "\"}},\n  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child(name + "-meta.db") +
            "\"}},\n  \"self_signed\": {\"signing_key\": \"grpc\"},\n  \"auth\": {\"mode\": "
            "\"disabled\"}\n}\n");
  };

  SECTION("enabled=false + port 非 0 → gRPC 端口**不监听**（横幅给出原因）") {
    const int http_port = FreePort();
    const int grpc_port = FreePort();
    const std::string config = grpc_config("grpc_off", http_port, grpc_port, false);
    ServerProcess server(SelfContainedOptions({"--config", config}));
    REQUIRE(server.http_port() == http_port);
    REQUIRE(WaitReady(http_port));
    CAPTURE(server.DumpLog(), grpc_port);
    REQUIRE(server.DumpLog().find("server.grpc.enabled=false") != std::string::npos);
    REQUIRE_FALSE(TcpConnects(grpc_port));
  }

  SECTION("enabled=true → gRPC 端口在监听，且 GetInfo 可用（免鉴权）") {
    const int http_port = FreePort();
    const int grpc_port = FreePort();
    const std::string config = grpc_config("grpc_on", http_port, grpc_port, true);
    ServerProcess server(SelfContainedOptions({"--config", config}));
    REQUIRE(server.http_port() == http_port);
    REQUIRE(WaitReady(http_port));
    CAPTURE(server.DumpLog(), grpc_port);
    REQUIRE(TcpConnects(grpc_port));

    auto channel = ::grpc::CreateChannel("127.0.0.1:" + std::to_string(grpc_port),
                                         ::grpc::InsecureChannelCredentials());
    auto stub = osdu::file::v1::FileService::NewStub(channel);
    ::grpc::ClientContext context;
    ::google::protobuf::Empty request;
    osdu::file::v1::InfoResponse response;
    const auto status = stub->GetInfo(&context, request, &response);
    INFO("gRPC 状态：" << status.error_code() << " " << status.error_message());
    REQUIRE(status.ok());
    REQUIRE(response.version() == "v2");
  }
}

// =============================================================================
//  C10.16：`partition.file.<partition>.*`（max_file_bytes / 校验算法集合与默认算法）
// =============================================================================
//  ★ 真实语义（不发明）：
//    · `max_file_bytes` → `PartitionConfig::max_object_bytes`（-1 = 不限），
//      并落到数据面 PUT 的 `RouteOptions::max_body_bytes`：带 `Content-Length` 超限
//      → **413**（读体前前置拒绝，一个字节都不读）；chunked → 400。
//    · 校验算法集合 / 默认算法：`C6.4` 有上游一手证据"客户端声明的算法**被覆写**"，
//      所以请求期不做 400；真实语义是**启动期校验**：未知算法名 / 默认不在集合内
//      → **exit 78**。R16 正例：合法的集合 + 默认必须能启动。
TEST_CASE("★ C10.16：partition.file.<p>.max_file_bytes 超限上传被拒（413），等于上限通过",
          "[phase10][config][c10.16]") {
  TempDir data_dir("c1016_max");
  const int http_port = FreePort();
  const std::string config = WriteFile(
      data_dir, "c1016_max.json",
      "{\n  \"server\": {\"http\": {\"port\": " + std::to_string(http_port) +
          ", \"bind\": \"127.0.0.1\"}},\n"
          "  \"storage\": {\"posix\": {\"root\": \"" + data_dir.child("data") + "\"}},\n"
          "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child("loc.db") + "\"}},\n"
          "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child("meta.db") + "\"}},\n"
          "  \"self_signed\": {\"signing_key\": \"c1016\"},\n"
          "  \"partition\": {\"file\": {\"opendes\": {\"max_file_bytes\": 16}}},\n"
          "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
  ServerProcess server(SelfContainedOptions({"--config", config}));
  REQUIRE(server.http_port() == http_port);
  REQUIRE(WaitReady(http_port));

  auto upload_target = [&]() -> std::string {
    const auto reply = HttpGet(http_port, "/api/file/v2/files/uploadURL", Authed());
    REQUIRE(reply.status == 200);
    const auto parsed = fss::json::Parse(reply.body);
    REQUIRE(parsed.ok());
    const auto& url = parsed.value()["Location"]["SignedURL"];
    REQUIRE(url.is_string());
    return fss::test::TargetOf(url.get<std::string>());
  };

  // 反向：17 字节 > 16 → 413
  const auto over =
      fss::test::HttpDo(http_port, "PUT", upload_target(), {}, std::string(17, 'x'));
  CAPTURE(server.DumpLog());
  REQUIRE(over.status == 413);

  // 正例对照（R16）：恰好 16 字节必须通过 —— 否则"超限被拒"可能只是"恒拒"。
  const auto exact =
      fss::test::HttpDo(http_port, "PUT", upload_target(), {}, std::string(16, 'x'));
  REQUIRE(exact.status == 200);
}

TEST_CASE("★ C10.16：非法校验算法 → exit 78；默认算法不在集合内 → exit 78；合法 → 能启动",
          "[phase10][config][c10.16]") {
  TempDir data_dir("c1016_checksum");
  const int http_port = FreePort();
  auto config_with = [&](const std::string& tag, const std::string& algorithms,
                         const std::string& def) {
    return WriteFile(
        data_dir, tag + ".json",
        "{\n  \"server\": {\"http\": {\"port\": " + std::to_string(http_port) +
            ", \"bind\": \"127.0.0.1\"}},\n"
            "  \"storage\": {\"posix\": {\"root\": \"" + data_dir.child(tag + "-data") +
            "\"}},\n"
            "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child(tag + "-loc.db") +
            "\"}},\n"
            "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child(tag + "-meta.db") +
            "\"}},\n"
            "  \"self_signed\": {\"signing_key\": \"c1016\"},\n"
            "  \"partition\": {\"file\": {\"opendes\": {\"allowed_checksum_algorithms\": [" +
            algorithms + "], \"default_checksum_algorithm\": \"" + def + "\"}}},\n"
            "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
  };

  SECTION("集合含未知算法 → exit 78") {
    const auto outcome =
        RunServerForExit({"--config", config_with("bad_algo", "\"SHA-256\", \"CRC-32\"", "SHA-256")});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("CRC-32") != std::string::npos);
  }

  SECTION("默认算法不在集合内 → exit 78") {
    const auto outcome =
        RunServerForExit({"--config", config_with("bad_default", "\"MD5\"", "SHA-256")});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("default_checksum_algorithm") != std::string::npos);
  }

  SECTION("正例（R16）：合法集合 + 合法默认 → 真的启动（readiness 200）") {
    const std::string config = config_with("ok_checksum", "\"SHA-256\", \"SHA-1\", \"MD5\"", "MD5");
    ServerProcess server(SelfContainedOptions({"--config", config}));
    REQUIRE(server.http_port() == http_port);
    CAPTURE(server.DumpLog());
    REQUIRE(WaitReady(http_port));
    REQUIRE(HttpGet(http_port, "/api/file/v2/readiness_check").status == 200);
  }
}

// =============================================================================
//  阶段 10 切片 4：`server.http.transfer_max_body_bytes` 与 SQLite 的
//  `journal_mode` / `synchronous`
// =============================================================================
//  ★ 数据面 PUT 上限的**落点** = 全局键（>0 时）与
//    `partition.file.<p>.max_file_bytes`（0 = 不限）的**较小者**；两者都为 0/不限时结果
//    仍是 0（行为与接线前逐字一致）。超限由 HTTP 包装层在**读体前**拒绝（带
//    `Content-Length` → 413），HTTP 包装层本身不改。
//  ★ `journal_mode` 会写进库文件头 → 可用 `python3 sqlite3` 另开连接读回；
//    `synchronous` **不落盘** → 真实进程侧只断言横幅打印了实际取值（"另开连接读回"
//    的进程内 L2 断言见 `tests/integration/test_sqlite_{location,metadata}_repository.cpp`
//    的 `AppliedPragma` 用例——`synchronous` 必须用同连接访问器）。
// =============================================================================

TEST_CASE("★ 切片 4：server.http.transfer_max_body_bytes 收紧数据面 PUT（413 / 200 / 取较小者）",
          "[phase10][config][c10.16]") {
  TempDir data_dir("c10s4_transfer");

  //  `global_value` / `partition_value` 为空串 = **不写**该键（走默认 0 = 不限）。
  const auto write_config = [&](const std::string& tag, int port,
                                const std::string& global_value,
                                const std::string& partition_value) {
    std::string server_http = "{\"port\": " + std::to_string(port) + ", \"bind\": \"127.0.0.1\"";
    if (!global_value.empty()) {
      server_http += ", \"transfer_max_body_bytes\": " + global_value;
    }
    server_http += "}";
    std::string partition_json;
    if (!partition_value.empty()) {
      partition_json = "  \"partition\": {\"file\": {\"opendes\": {\"max_file_bytes\": " +
                       partition_value + "}}},\n";
    }
    return WriteFile(
        data_dir, tag + ".json",
        "{\n  \"server\": {\"http\": " + server_http + "},\n"
        "  \"storage\": {\"posix\": {\"root\": \"" + data_dir.child(tag + "-data") + "\"}},\n"
        "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child(tag + "-loc.db") + "\"}},\n"
        "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child(tag + "-meta.db") + "\"}},\n"
        "  \"self_signed\": {\"signing_key\": \"c10s4\"},\n" + partition_json +
        "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
  };

  //  每次 PUT 都用**新签发**的自签 URL（数据面从 token 载荷解析对象键）。
  const auto upload_target = [](int http_port) -> std::string {
    const auto reply = HttpGet(http_port, "/api/file/v2/files/uploadURL", Authed());
    REQUIRE(reply.status == 200);
    const auto parsed = fss::json::Parse(reply.body);
    REQUIRE(parsed.ok());
    const auto& url = parsed.value()["Location"]["SignedURL"];
    REQUIRE(url.is_string());
    return fss::test::TargetOf(url.get<std::string>());
  };

  //  ★ 为什么"超限"这一侧只发**请求头**（用 `Content-Length` 声明 2 MiB / 8192），
  //    而不真的把字节发完：`max_body_bytes` 的真实语义就是**按 Content-Length 前置拒绝**
  //    （`common/http` 的 H-2①：一个字节都不读）。真把 2 MiB 灌进去时，服务端在读体前
  //    就 413 并关闭连接，未读的残余请求体会让内核回 RST，把已经收到的 413 丢掉
  //    （客户端 send 报错 + 响应丢失，非确定性）。只发头 + 声明长度正好命中那条真实路径。
  const auto put_declared_length = [](int http_port, const std::string& target,
                                      std::size_t declared_len) -> int {
    RawClient client(http_port, /*tcp_nodelay=*/true);
    if (!client.Connect()) return 0;
    std::string head = "PUT " + target + " HTTP/1.1\r\nHost: h\r\n";
    head += "Content-Length: " + std::to_string(declared_len) + "\r\n";
    head += "authorization: Bearer test-token\r\ndata-partition-id: opendes\r\n\r\n";
    if (!client.Send(head)) return 0;
    const auto response = client.ReadResponse(10000);
    return response.has_value() ? response->status : 0;
  };

  SECTION("全局键 > 0 且未设 partition：2 MiB → 413；恰好 1 MiB → 200") {
    const int port = FreePort();
    const std::string config = write_config("global_only", port, "1048576", "");
    ServerProcess server(SelfContainedOptions({"--config", config}));
    REQUIRE(server.http_port() == port);
    REQUIRE(WaitReady(port));
    CAPTURE(server.DumpLog());
    //  横幅打印**实际生效值**（= 全局键，因为 partition 侧不限）
    REQUIRE(server.DumpLog().find("transfer limit : 数据面 PUT max_body_bytes=1048576") !=
            std::string::npos);

    const int over = put_declared_length(port, upload_target(port), std::size_t{2} * 1024 * 1024);
    CAPTURE(over);
    REQUIRE(over == 413);

    //  正例（R16）：恰好 1 MiB 必须通过 —— 否则"超限被拒"可能只是"恒拒"。
    const auto exact = fss::test::HttpDo(port, "PUT", upload_target(port), {},
                                         std::string(std::size_t{1024} * 1024, 'x'));
    CAPTURE(exact.status, exact.body);
    REQUIRE(exact.status == 200);
  }

  SECTION("R16 正例对照：**未设**该键（默认 0 = 不限）→ 同一个 2 MiB PUT 必须 200") {
    const int port = FreePort();
    const std::string config = write_config("unlimited", port, "", "");
    ServerProcess server(SelfContainedOptions({"--config", config}));
    REQUIRE(WaitReady(port));
    CAPTURE(server.DumpLog());
    REQUIRE(server.DumpLog().find("max_body_bytes=0（0=不限") != std::string::npos);
    const auto reply = fss::test::HttpDo(port, "PUT", upload_target(port), {},
                                         std::string(std::size_t{2} * 1024 * 1024, 'x'));
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 200);
  }

  SECTION("全局 > 0 且 partition 更小 → 取较小者（4096 生效，而非全局 1 MiB）") {
    const int port = FreePort();
    const std::string config = write_config("min_wins", port, "1048576", "4096");
    ServerProcess server(SelfContainedOptions({"--config", config}));
    REQUIRE(WaitReady(port));
    CAPTURE(server.DumpLog());
    REQUIRE(server.DumpLog().find("max_body_bytes=4096") != std::string::npos);

    //  8192 > partition 的 4096（但 < 全局 1 MiB）→ 必须 413：证明取的是**较小者**。
    const int over = put_declared_length(port, upload_target(port), 8192);
    CAPTURE(over);
    REQUIRE(over == 413);
    //  正例：恰好 4096 → 200
    const auto exact =
        fss::test::HttpDo(port, "PUT", upload_target(port), {}, std::string(4096, 'x'));
    REQUIRE(exact.status == 200);
  }
}

TEST_CASE("★ 切片 4：metadata.sqlite.journal_mode 生效（python3 读回）；TRUNCATE → exit 78",
          "[phase10][config][c10.14]") {
  TempDir cfg_dir("fss_cfg_meta_sqlite");
  TempDir data_dir("fss_cfg_meta_sqlite_data");

  const auto run_with = [&](const std::string& name, const std::string& journal_mode) {
    const int port = FreePort();
    const std::string db = data_dir.child(name + "-meta.db");
    const std::string config = WriteFile(
        cfg_dir, name + ".json",
        std::string("{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": ") +
            std::to_string(port) + "}},\n  \"storage\": {\"posix\": {\"root\": \"" +
            data_dir.child(name + "-store") + "\"}},\n  \"location\": {\"sqlite\": {\"path\": \"" +
            data_dir.child(name + "-loc.db") +
            "\"}},\n  \"metadata\": {\"sqlite\": {\"path\": \"" + db + "\", \"journal_mode\": \"" +
            journal_mode + "\"}},\n  \"self_signed\": {\"signing_key\": \"c10s4\"},\n"
            "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
    return std::make_tuple(db, config, port);
  };

  SECTION("WAL（默认）→ python3 读回 'wal'，横幅可见实际取值") {
    const auto [db, config, port] = run_with("meta_wal", "WAL");
    ServerProcess server(SelfContainedOptions({"--config", config}));
    REQUIRE(WaitReady(port));
    CAPTURE(server.DumpLog());
    REQUIRE(server.DumpLog().find("metadata busy_timeout=5000ms journal_mode=WAL") !=
            std::string::npos);
    const std::string mode = PythonJournalMode(db);
    CAPTURE(db, mode);
    REQUIRE(mode == "wal");
  }

  SECTION("DELETE → python3 读回 'delete'（证明配置真的改了行为，而不是恒为 WAL）") {
    const auto [db, config, port] = run_with("meta_del", "DELETE");
    ServerProcess server(SelfContainedOptions({"--config", config}));
    REQUIRE(WaitReady(port));
    CAPTURE(server.DumpLog());
    REQUIRE(server.DumpLog().find("metadata busy_timeout=5000ms journal_mode=DELETE") !=
            std::string::npos);
    const std::string mode = PythonJournalMode(db);
    CAPTURE(db, mode);
    REQUIRE(mode == "delete");
  }

  SECTION("TRUNCATE → 拒绝启动（只接通 WAL|DELETE，不静默当成 DELETE）") {
    const auto [db, config, port] = run_with("meta_trunc", "TRUNCATE");
    (void)db;
    (void)port;
    const ProcessOutcome outcome = RunServerForExit({"--config", config});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("metadata.sqlite.journal_mode=TRUNCATE") != std::string::npos);
    REQUIRE(outcome.output.find("已启动") == std::string::npos);
  }
}

TEST_CASE("★ 切片 4：*.sqlite.synchronous 映射到 PRAGMA 并在横幅可见；非法 → exit 78",
          "[phase10][config][c10.14]") {
  TempDir cfg_dir("fss_cfg_sync");
  TempDir data_dir("fss_cfg_sync_data");

  const auto write_config = [&](const std::string& name, int port,
                                const std::string& metadata_sync,
                                const std::string& location_sync) {
    return WriteFile(
        cfg_dir, name + ".json",
        "{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
            std::to_string(port) + "}},\n  \"storage\": {\"posix\": {\"root\": \"" +
            data_dir.child(name + "-store") +
            "\"}},\n  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child(name + "-loc.db") +
            "\", \"synchronous\": \"" + location_sync +
            "\"}},\n  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child(name + "-meta.db") +
            "\", \"synchronous\": \"" + metadata_sync +
            "\"}},\n  \"self_signed\": {\"signing_key\": \"c10s4\"},\n"
            "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
  };

  SECTION("FULL / OFF 落到横幅（实际生效取值可见），readiness 200") {
    const int port = FreePort();
    const std::string config = write_config("sync_full_off", port, "FULL", "OFF");
    ServerProcess server(SelfContainedOptions({"--config", config}));
    REQUIRE(server.http_port() == port);
    REQUIRE(WaitReady(port));
    CAPTURE(server.DumpLog());
    REQUIRE(server.DumpLog().find("synchronous=FULL") != std::string::npos);
    REQUIRE(server.DumpLog().find("synchronous=OFF") != std::string::npos);
  }

  SECTION("正例（R16）：默认 NORMAL 两个仓储都能启动且横幅可见") {
    const int port = FreePort();
    const std::string config = write_config("sync_default", port, "NORMAL", "NORMAL");
    ServerProcess server(SelfContainedOptions({"--config", config}));
    REQUIRE(WaitReady(port));
    CAPTURE(server.DumpLog());
    //  横幅同时打印 location 与 metadata 两处（同一行），出现两次
    REQUIRE(server.DumpLog().find("synchronous=NORMAL") != std::string::npos);
  }

  SECTION("非法 metadata.sqlite.synchronous=FAST → exit 78 + 可读原因") {
    const int port = FreePort();
    const std::string config = write_config("bad_meta_sync", port, "FAST", "NORMAL");
    const ProcessOutcome outcome = RunServerForExit({"--config", config});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("metadata.sqlite.synchronous") != std::string::npos);
    REQUIRE(outcome.output.find("FAST") != std::string::npos);
    REQUIRE(outcome.output.find("已启动") == std::string::npos);
  }

  SECTION("非法 location.sqlite.synchronous=FAST → exit 78 + 可读原因") {
    const int port = FreePort();
    const std::string config = write_config("bad_loc_sync", port, "NORMAL", "FAST");
    const ProcessOutcome outcome = RunServerForExit({"--config", config});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("location.sqlite.synchronous") != std::string::npos);
    REQUIRE(outcome.output.find("FAST") != std::string::npos);
  }
}

// =============================================================================
//  C10.16 续（阶段 10 收尾）：`storage.posix.*` 细节键 与 `partition.file.*` 容器名
// =============================================================================
//  ★ 为什么分两层验证：
//    · **驱动层**（真实 `PosixBlobStore` + 可注入 fadvise 接缝）—— 权限位、原子写分支、
//      fadvise 是否真的被下发，这些在进程级不可观察（`posix_fadvise` 没有可依赖的返回值）；
//    · **真实进程**（`build/bin/fss_server` + `--config`）—— 证明组合根确实把这些键
//      映射到了驱动与装配点（"测试夹具接上了、组合根没接"= 产品里不存在，P9-D10）。
//  ★ R1 对照：atomic_write 两种模式在同一个"失败注入"下都不得留下半成品；fadvise
//    关掉开关后同一段大读必须**不再**下发 DONTNEED（否则"计数 > 0"可能只是恒真）。
// =============================================================================
namespace {

//  故障注入字节源：给出 `good_bytes` 个字节后返回错误 → `put` 必须失败。
class FailingSource final : public fss::bytes::ByteSource {
 public:
  explicit FailingSource(std::size_t good_bytes) : good_bytes_(good_bytes) {}
  fss::Result<std::size_t> Read(char* out, std::size_t capacity) override {
    if (emitted_ >= good_bytes_) {
      return fss::Err(fss::ErrorKind::kInternal, "注入的读取失败（C10.16 续的故障注入）");
    }
    const std::size_t n = std::min(capacity, good_bytes_ - emitted_);
    for (std::size_t i = 0; i < n; ++i) out[i] = 'x';
    emitted_ += n;
    return n;
  }

 private:
  std::size_t good_bytes_;
  std::size_t emitted_ = 0;
};

//  计数型 fadvise 接缝：把"提示真的被下发"变成可断言的事实。
class CountingFadviseSink final : public fss::infra::IFadviseSink {
 public:
  void Random(int) override { ++random; }
  void DontNeed(int) override { ++dontneed; }
  int random = 0;
  int dontneed = 0;
};

std::size_t CountRegularFiles(const std::string& dir) {
  std::size_t count = 0;
  if (!std::filesystem::is_directory(dir)) return 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    if (entry.is_regular_file()) ++count;
  }
  return count;
}

unsigned ModeBits(const std::string& path) {
  struct ::stat info {};
  if (::stat(path.c_str(), &info) != 0) return 0xFFFFFFFFu;
  return static_cast<unsigned>(info.st_mode & 07777);
}

}  // namespace

TEST_CASE("★ C10.16 续：storage.posix.{dir_mode,file_mode,atomic_write,fadvise_*} 在驱动层生效",
          "[phase10][config][c10.16][posix]") {
  TempDir work("c1016_posix_opts");
  const std::string root = work.child("blobs");
  fss::SystemClock clock;
  fss::domain::ObjectRef ref;
  ref.container = "opendes-staging";
  ref.key = "osdu-user/seq-1/file";

  SECTION("dir_mode / file_mode → mkdir/open 用的就是配置的权限位") {
    fss::infra::PosixBlobStoreOptions options;
    options.fsync_policy = fss::infra::FsyncPolicy::kNever;
    options.dir_mode = 0700;
    options.file_mode = 0600;
    fss::infra::PosixBlobStore store(root, clock, options);
    REQUIRE(store.ensure_container(ref.container).ok());
    fss::bytes::StringSource source(std::string("mode-check"));
    REQUIRE(store.put(ref, source, fss::domain::PutOptions{}).ok());

    const std::string dir = root + "/" + ref.container;
    const std::string file = dir + "/" + ref.key;
    INFO("dir=" << dir << " mode=" << std::oct << ModeBits(dir));
    INFO("file=" << file << " mode=" << std::oct << ModeBits(file));
    REQUIRE(ModeBits(dir) == 0700u);
    REQUIRE(ModeBits(file) == 0600u);
  }

  SECTION("fadvise_random → 写/读路径各一次；DONTNEED 只对 > 1 MiB 的读（含关闭对照）") {
    CountingFadviseSink sink;
    fss::infra::PosixBlobStoreOptions options;
    options.fsync_policy = fss::infra::FsyncPolicy::kNever;
    options.fadvise_sink = &sink;
    options.fadvise_random = true;
    options.fadvise_dontneed_after_large_read = true;
    fss::infra::PosixBlobStore store(root, clock, options);
    REQUIRE(store.ensure_container(ref.container).ok());

    fss::bytes::StringSource small(std::string(1024, 'a'));
    REQUIRE(store.put(ref, small, fss::domain::PutOptions{}).ok());
    REQUIRE(sink.random == 1);  // 写路径
    REQUIRE(sink.dontneed == 0);

    fss::bytes::StringSink small_out;
    REQUIRE(store.get(ref, small_out, fss::domain::ByteRange{}).ok());
    REQUIRE(small_out.str().size() == 1024);
    REQUIRE(sink.random == 2);  // 读路径
    REQUIRE(sink.dontneed == 0);  // 1 KiB < 阈值 → 不触发

    fss::domain::ObjectRef big_ref;
    big_ref.container = ref.container;
    big_ref.key = "osdu-user/seq-1/big";
    fss::bytes::StringSource big(std::string(2 * 1024 * 1024, 'b'));
    REQUIRE(store.put(big_ref, big, fss::domain::PutOptions{}).ok());
    fss::bytes::StringSink big_out;
    REQUIRE(store.get(big_ref, big_out, fss::domain::ByteRange{}).ok());
    REQUIRE(big_out.str().size() == 2 * 1024 * 1024);
    REQUIRE(sink.dontneed == 1);  // 2 MiB > 1 MiB → 触发一次

    //  ★ R1 对照：把两个开关都关掉 → 同一段大读**不再**下发任何提示。
    //    （没有这条，"dontneed == 1" 无法区分"配置生效"与"实现恒发"。）
    CountingFadviseSink off_sink;
    fss::infra::PosixBlobStoreOptions off_options;
    off_options.fsync_policy = fss::infra::FsyncPolicy::kNever;
    off_options.fadvise_sink = &off_sink;
    off_options.fadvise_random = false;
    off_options.fadvise_dontneed_after_large_read = false;
    fss::infra::PosixBlobStore off_store(root, clock, off_options);
    fss::bytes::StringSink off_out;
    REQUIRE(off_store.get(big_ref, off_out, fss::domain::ByteRange{}).ok());
    REQUIRE(off_out.str().size() == 2 * 1024 * 1024);
    REQUIRE(off_sink.random == 0);
    REQUIRE(off_sink.dontneed == 0);
  }

  SECTION("atomic_write=false：失败注入后目标不残留（默认 true 同样不留半成品）") {
    for (const bool atomic : {false, true}) {
      const std::string sub_root = work.child(atomic ? "atomic_on" : "atomic_off");
      fss::infra::PosixBlobStoreOptions options;
      options.fsync_policy = fss::infra::FsyncPolicy::kNever;
      options.atomic_write = atomic;
      fss::infra::PosixBlobStore store(sub_root, clock, options);
      REQUIRE(store.ensure_container(ref.container).ok());

      FailingSource failing(1024);
      const auto result = store.put(ref, failing, fss::domain::PutOptions{});
      INFO("atomic_write=" << atomic << " 错误=" << result.error().ToString());
      REQUIRE_FALSE(result.ok());
      //  ① 目标文件绝不残留（"打开即截断"的非原子模式尤其关键）
      REQUIRE_FALSE(
          std::filesystem::exists(sub_root + "/" + ref.container + "/" + ref.key));
      //  ② 临时文件也不残留
      std::size_t leftovers = 0;
      for (const auto& entry : std::filesystem::recursive_directory_iterator(sub_root)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().find(".tmp.") != std::string::npos) {
          ++leftovers;
        }
      }
      REQUIRE(leftovers == 0);
    }

    //  ★ 正例对照（R16）：同一段代码在成功路径上**必须**留下对象 ——
    //    否则上面的"不存在"可能只是"这个 key 永远写不进去"。
    fss::infra::PosixBlobStoreOptions ok_options;
    ok_options.fsync_policy = fss::infra::FsyncPolicy::kNever;
    fss::infra::PosixBlobStore ok_store(work.child("atomic_ok"), clock, ok_options);
    REQUIRE(ok_store.ensure_container(ref.container).ok());
    fss::bytes::StringSource good(std::string("complete-object"));
    REQUIRE(ok_store.put(ref, good, fss::domain::PutOptions{}).ok());
    REQUIRE(std::filesystem::exists(work.child("atomic_ok") + "/" + ref.container + "/" +
                                    ref.key));
  }
}

TEST_CASE("★ C10.16 续：storage.posix.dir_mode / file_mode 从配置生效（真实进程 stat 权限位）",
          "[phase10][config][c10.16]") {
  TempDir data_dir("c1016_posix_mode");
  const int http_port = FreePort();
  const std::string root = data_dir.child("data");
  const std::string config = WriteFile(
      data_dir, "posix_mode.json",
      "{\n  \"server\": {\"http\": {\"port\": " + std::to_string(http_port) +
          ", \"bind\": \"127.0.0.1\"}},\n"
          "  \"storage\": {\"posix\": {\"root\": \"" + root +
          "\", \"dir_mode\": \"0700\", \"file_mode\": \"0600\"}},\n"
          "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child("loc.db") + "\"}},\n"
          "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child("meta.db") + "\"}},\n"
          "  \"self_signed\": {\"signing_key\": \"c1016-posix-mode\"},\n"
          "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
  ServerProcess server(SelfContainedOptions({"--config", config}));
  REQUIRE(server.http_port() == http_port);
  CAPTURE(server.DumpLog());
  REQUIRE(WaitReady(http_port));

  //  uploadURL 会在 staging 区创建空对象 → 触发目录创建（dir_mode）与文件写入（file_mode）
  const auto reply = HttpGet(http_port, "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(reply.status == 200);

  const std::string staging_dir = root + "/blobs/opendes-staging";
  const std::string persistent_dir = root + "/blobs/opendes-persistent";
  CAPTURE(staging_dir, persistent_dir);
  REQUIRE(ModeBits(staging_dir) == 0700u);
  REQUIRE(ModeBits(persistent_dir) == 0700u);

  bool saw_object = false;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(staging_dir)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() == ".fssmeta") continue;  // 侧车文件另算
    CAPTURE(entry.path().string());
    REQUIRE(ModeBits(entry.path().string()) == 0600u);
    saw_object = true;
  }
  REQUIRE(saw_object);
}

TEST_CASE("★ C10.16 续：partition.file.opendes.{staging,persistent}_container 自定义值真的生效",
          "[phase10][config][c10.16]") {
  TempDir data_dir("c1016_container");
  const int http_port = FreePort();
  const std::string root = data_dir.child("data");
  const std::string config = WriteFile(
      data_dir, "container.json",
      "{\n  \"server\": {\"http\": {\"port\": " + std::to_string(http_port) +
          ", \"bind\": \"127.0.0.1\"}},\n"
          "  \"storage\": {\"posix\": {\"root\": \"" + root + "\"}},\n"
          "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child("loc.db") + "\"}},\n"
          "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child("meta.db") + "\"}},\n"
          "  \"self_signed\": {\"signing_key\": \"c1016-container\"},\n"
          "  \"partition\": {\"file\": {\"opendes\": {\"staging_container\": \"custom-stage-1\","
          " \"persistent_container\": \"custom-persist-1\", \"storage_driver\": \"posix\"}}},\n"
          "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
  //  ★ R16 正例：自定义容器名 + 与顶层一致的 storage_driver → 必须能启动。
  ServerProcess server(SelfContainedOptions({"--config", config}));
  REQUIRE(server.http_port() == http_port);
  CAPTURE(server.DumpLog());
  REQUIRE(WaitReady(http_port));

  const std::string custom_staging = root + "/blobs/custom-stage-1";
  const std::string default_staging = root + "/blobs/opendes-staging";
  CAPTURE(custom_staging, default_staging);
  //  ① 启动时按**配置的**容器名创建目录；默认名目录不出现
  REQUIRE(std::filesystem::is_directory(custom_staging));
  REQUIRE(std::filesystem::is_directory(root + "/blobs/custom-persist-1"));
  REQUIRE_FALSE(std::filesystem::exists(default_staging));

  //  ② 上传地址签发（ObjectKeyPolicy::ContainerFor → 配置值）→ 对象落在自定义容器下
  const auto reply = HttpGet(http_port, "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(reply.status == 200);
  REQUIRE(CountRegularFiles(custom_staging) > 0);
  REQUIRE_FALSE(std::filesystem::exists(default_staging));
}

TEST_CASE("★ C10.16 续：非法容器名 / 分区级驱动冲突 / 非法权限位 → exit 78",
          "[phase10][config][c10.16]") {
  TempDir data_dir("c1016_container_bad");
  const int http_port = FreePort();
  auto config_with = [&](const std::string& tag, const std::string& partition_body) {
    return WriteFile(
        data_dir, tag + ".json",
        "{\n  \"server\": {\"http\": {\"port\": " + std::to_string(http_port) +
            ", \"bind\": \"127.0.0.1\"}},\n"
            "  \"storage\": {\"posix\": {\"root\": \"" + data_dir.child(tag + "-data") +
            "\"}},\n"
            "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child(tag + "-loc.db") +
            "\"}},\n"
            "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child(tag + "-meta.db") +
            "\"}},\n"
            "  \"self_signed\": {\"signing_key\": \"c1016-bad\"},\n"
            "  \"partition\": {\"file\": {\"opendes\": {" + partition_body + "}}},\n"
            "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
  };

  SECTION("容器名含 '/' → exit 78，且信息里点名 staging_container") {
    const auto outcome = RunServerForExit(
        {"--config", config_with("bad_container", "\"staging_container\": \"a/b\"")});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("staging_container") != std::string::npos);
  }

  SECTION("分区级 storage_driver 与顶层 storage.driver 冲突 → exit 78 并说明") {
    const auto outcome =
        RunServerForExit({"--config", config_with("bad_driver", "\"storage_driver\": \"s3\"")});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("storage_driver") != std::string::npos);
    REQUIRE(outcome.output.find("冲突") != std::string::npos);
  }

  SECTION("storage.posix.dir_mode 不是八进制权限位 → exit 78") {
    const std::string config = WriteFile(
        data_dir, "bad_mode.json",
        "{\n  \"server\": {\"http\": {\"port\": " + std::to_string(http_port) +
            ", \"bind\": \"127.0.0.1\"}},\n"
            "  \"storage\": {\"posix\": {\"root\": \"" + data_dir.child("bad_mode-data") +
            "\", \"dir_mode\": \"8x\"}},\n"
            "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child("bad_mode-loc.db") +
            "\"}},\n"
            "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child("bad_mode-meta.db") +
            "\"}},\n"
            "  \"self_signed\": {\"signing_key\": \"c1016-bad\"},\n"
            "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
    const auto outcome = RunServerForExit({"--config", config});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("dir_mode") != std::string::npos);
  }

  SECTION("storage.posix.group_commit_max_batch / sync_dir_after_batch 非默认 → exit 78（批提交未实现）") {
    auto cfg_with_posix = [&](const std::string& tag, const std::string& posix_extra) {
      return WriteFile(
          data_dir, tag + ".json",
          "{\n  \"server\": {\"http\": {\"port\": " + std::to_string(http_port) +
              ", \"bind\": \"127.0.0.1\"}},\n"
              "  \"storage\": {\"posix\": {\"root\": \"" + data_dir.child(tag + "-data") +
              "\", " + posix_extra + "}},\n"
              "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child(tag + "-loc.db") +
              "\"}},\n"
              "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child(tag + "-meta.db") +
              "\"}},\n"
              "  \"self_signed\": {\"signing_key\": \"c1016-bad\"},\n"
              "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
    };
    const auto outcome = RunServerForExit(
        {"--config", cfg_with_posix("bad_batch", "\"group_commit_max_batch\": 7")});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("批提交协议未实现") != std::string::npos);
    REQUIRE(outcome.output.find("ADR-008") != std::string::npos);

    const auto outcome2 = RunServerForExit(
        {"--config", cfg_with_posix("bad_syncdir", "\"sync_dir_after_batch\": false")});
    CAPTURE(outcome2.exit_code, outcome2.output);
    REQUIRE(outcome2.exit_code == 78);
    REQUIRE(outcome2.output.find("批提交协议未实现") != std::string::npos);
  }
}

// =============================================================================
//  阶段 10 切片 5（C10.17）：`self_signed` 的 3 个键
// =============================================================================
//  ★ `key_id`：进**被签名的载荷**，解码侧要求与当前配置一致（缺失也算不匹配）→ 换 id
//    重启后旧 URL 必须被拒。用**真实进程**做正反两侧：先 k1 签发并 PUT/GET 200，再以
//    k2 重启同一个存储根/同一签名密钥，重放同一个 URL → 必须 401（实测为准，不是猜的）。
//  ★ `{default,max}_ttl_seconds`：**自签分支**的 TTL 上界（`expiry.*` 仍是 `expiryTime`
//    参数的解析规则与缺省，见 `SelfSignedTtlOptions` 的理由）。"只设 `expiry.*` 时结论
//    不变"由**本文件未被修改的 C10.12 用例**继续锁定（`expiry.default=5M` + 不带
//    `expiryTime` → `exp=now+300`）。
// =============================================================================

TEST_CASE("★ C10.17：self_signed.key_id 进签名载荷；换 key_id 重启后旧 URL 必须被拒（真实进程）",
          "[phase10][config][c10.17]") {
  TempDir data_dir("c10s5_keyid");
  //  ★ 两次启动**共用**同一个存储根（对象真的还在），但**各自独立**的 SQLite 库：
  //    顺序重启时第一个进程的退出与第二个进程的打开会抢库锁（实测 `PRAGMA synchronous`
  //    报 `database is locked` → 拒绝启动，把用例变成 flaky）。负例的判据在 Decode 阶段，
  //    根本不需要读位置记录，因此独立库不影响结论。下面还会**轮询**第一个进程真正退出。
  const auto write_cfg = [&](const std::string& tag, int port, const std::string& key_id) {
    return WriteFile(
        data_dir, tag + ".json",
        "{\n  \"server\": {\"http\": {\"port\": " + std::to_string(port) +
            ", \"bind\": \"127.0.0.1\"}},\n"
            "  \"storage\": {\"posix\": {\"root\": \"" + data_dir.child("store") + "\"}},\n"
            "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child(tag + "-loc.db") +
            "\"}},\n"
            "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child(tag + "-meta.db") +
            "\"}},\n"
            "  \"self_signed\": {\"signing_key\": \"c10s5-secret\", \"key_id\": \"" + key_id +
            "\"},\n"
            "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
  };
  //  等一个后台进程真正退出（`kill -0` 失败），不要用固定 sleep（AGENTS §4.3）
  const auto wait_gone = [](const std::string& pid) {
    for (int i = 0; i < 400; ++i) {
      if (std::system(("kill -0 " + pid + " 2>/dev/null").c_str()) != 0) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
  };
  //  签发一对 PUT / GET URL，并返回 file_id
  const auto issue = [](int port, std::string& put_url, std::string& get_url) -> std::string {
    const auto upload = HttpGet(port, "/api/file/v2/files/uploadURL", Authed());
    REQUIRE(upload.status == 200);
    const auto upload_json = fss::json::Parse(upload.body);
    REQUIRE(upload_json.ok());
    const std::string file_id = upload_json.value()["FileID"].get<std::string>();
    put_url = fss::test::TargetOf(upload_json.value()["Location"]["SignedURL"].get<std::string>());
    const auto download =
        HttpGet(port, "/api/file/v2/files/" + file_id + "/downloadURL", Authed());
    REQUIRE(download.status == 200);
    const auto download_json = fss::json::Parse(download.body);
    REQUIRE(download_json.ok());
    get_url = fss::test::TargetOf(download_json.value()["SignedUrl"].get<std::string>());
    return file_id;
  };

  const int k1_port = FreePort();
  const int k2_port = FreePort();
  std::string put_url;
  std::string get_url;
  std::string k1_pid;

  //  ---- ① key_id=k1：签发的 URL 必须可用（R16 正例，证明后续的 401 不是"恒拒"）----
  {
    ServerProcess server(SelfContainedOptions({"--config", write_cfg("k1", k1_port, "k1")}));
    REQUIRE(server.http_port() == k1_port);
    REQUIRE(WaitReady(k1_port));
    CAPTURE(server.DumpLog());
    REQUIRE(server.DumpLog().find("self signed    : key_id=k1") != std::string::npos);

    issue(k1_port, put_url, get_url);
    const auto put = fss::test::HttpDo(k1_port, "PUT", put_url, {}, "k1-payload");
    CAPTURE(put.status, put.body);
    REQUIRE(put.status == 200);
    const auto got = fss::test::HttpDo(k1_port, "GET", get_url, {});
    CAPTURE(got.status, got.body);
    REQUIRE(got.status == 200);
    REQUIRE(got.body == "k1-payload");
    k1_pid = server.pid();
  }
  REQUIRE(wait_gone(k1_pid));  // R9：显式前置条件（旧进程退出）后才启动新进程

  //  ---- ② 以 key_id=k2 重启（同签名密钥 + 同存储根）→ 重放 k1 的 URL 必须被拒 ----
  {
    ServerProcess server(SelfContainedOptions({"--config", write_cfg("k2", k2_port, "k2")}));
    REQUIRE(server.http_port() == k2_port);
    REQUIRE(WaitReady(k2_port));
    CAPTURE(server.DumpLog());
    REQUIRE(server.DumpLog().find("self signed    : key_id=k2") != std::string::npos);

    //  目标对象仍在（同一存储根）：先证明"路径本身没问题"——k2 新签发的 PUT 能 200。
    std::string k2_put;
    std::string k2_get;
    issue(k2_port, k2_put, k2_get);
    const auto fresh = fss::test::HttpDo(k2_port, "PUT", k2_put, {}, "k2-payload");
    CAPTURE(fresh.status, fresh.body);
    REQUIRE(fresh.status == 200);

    //  重放 k1 的 URL：签名仍然有效（密钥/载荷都没变），被拒的**只能是 key_id 不匹配**。
    const auto replay_put = fss::test::HttpDo(k2_port, "PUT", put_url, {}, "replay");
    CAPTURE(replay_put.status, replay_put.body);
    REQUIRE(replay_put.status == 401);
    REQUIRE(replay_put.body.find("key_id") != std::string::npos);

    const auto replay_get = fss::test::HttpDo(k2_port, "GET", get_url, {});
    CAPTURE(replay_get.status, replay_get.body);
    REQUIRE(replay_get.status == 401);
    REQUIRE(replay_get.body.find("key_id") != std::string::npos);
  }
}

TEST_CASE("★ C10.17：self_signed.{default,max}_ttl_seconds 是自签分支的 TTL 上界（真实进程）",
          "[phase10][config][c10.17]") {
  TempDir data_dir("c10s5_ttl");
  const auto write_cfg = [&](const std::string& tag, int port, const std::string& self_extra) {
    return WriteFile(
        data_dir, tag + ".json",
        "{\n  \"server\": {\"http\": {\"port\": " + std::to_string(port) +
            ", \"bind\": \"127.0.0.1\"}},\n"
            "  \"storage\": {\"posix\": {\"root\": \"" + data_dir.child(tag + "-data") + "\"}},\n"
            "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child(tag + "-loc.db") +
            "\"}},\n"
            "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child(tag + "-meta.db") +
            "\"}},\n"
            "  \"self_signed\": {\"signing_key\": \"c10s5-ttl\"" + self_extra + "},\n"
            "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
  };
  const auto upload_exp = [](int port, const std::string& query) -> std::int64_t {
    const auto reply = HttpGet(port, "/api/file/v2/files/uploadURL" + query, Authed());
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 200);
    return ExpiresOf(reply.body);
  };

  SECTION("max_ttl_seconds=120 + expiryTime=9H（远小于 expiry.max=7D）→ exp=now+120") {
    const int port = FreePort();
    ServerProcess server(
        SelfContainedOptions({"--config", write_cfg("max120", port, ", \"max_ttl_seconds\": 120")}));
    REQUIRE(WaitReady(port));
    CAPTURE(server.DumpLog());
    REQUIRE(server.DumpLog().find("TTL 自签上界 default=3600s max=120s") != std::string::npos);

    const auto now = static_cast<std::int64_t>(::time(nullptr));
    const std::int64_t exp = upload_exp(port, "?expiryTime=9H");
    CAPTURE(now, exp);
    REQUIRE(exp >= now + 115);
    REQUIRE(exp <= now + 125);

    //  R16 正例：请求值 1M（60s）**小于**绝对上界 → 逐字保留（上界不是"常量改写"）。
    const std::int64_t small = upload_exp(port, "?expiryTime=1M");
    REQUIRE(small >= now + 55);
    REQUIRE(small <= now + 65);
  }

  SECTION("default_ttl_seconds=60 且请求不带 expiryTime → exp=now+60（expiry.default 仍是 1H）") {
    const int port = FreePort();
    ServerProcess server(SelfContainedOptions(
        {"--config", write_cfg("default60", port, ", \"default_ttl_seconds\": 60")}));
    REQUIRE(WaitReady(port));
    CAPTURE(server.DumpLog());
    REQUIRE(server.DumpLog().find("TTL 自签上界 default=60s max=604800s") != std::string::npos);

    const auto now = static_cast<std::int64_t>(::time(nullptr));
    const std::int64_t exp = upload_exp(port, "");
    CAPTURE(now, exp);
    REQUIRE(exp >= now + 55);
    REQUIRE(exp <= now + 65);

    //  给了 expiryTime 时**不叠加**缺省上界：只有绝对上界 604800s 会夹紧。
    //  ★ 用 2M（120s）而不是 1M（60s）：60s 与缺省上界**恰好同值**，那样这条断言
    //    "叠加与不叠加都能过"（恒真）。120s > 缺省上界 60s 且 < 绝对上界 → 必须原样保留。
    const std::int64_t with_query = upload_exp(port, "?expiryTime=2M");
    REQUIRE(with_query >= now + 115);
    REQUIRE(with_query <= now + 125);

    //  ★ 空串 `?expiryTime=` 必须按"未提供"处理（契约 §1.4 与 ExpiryPolicy 的既定语义），
    //    因此**仍受**缺省上界 60s 夹紧 —— 否则带一个空参数就能绕过该键。
    const std::int64_t empty_query = upload_exp(port, "?expiryTime=");
    REQUIRE(empty_query >= now + 55);
    REQUIRE(empty_query <= now + 65);
  }
}

