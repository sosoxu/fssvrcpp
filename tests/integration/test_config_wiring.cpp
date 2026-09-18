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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <fstream>
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
