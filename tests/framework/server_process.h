// =============================================================================
//  server_process.h —— 把**真实组合根**（`build/bin/fss_server`）拉起来做端到端
// =============================================================================
//  为什么需要它（C7.8 的判据是"gRPC 与 REST **同时**运行、互不干扰"）
//    `DualProtocolFixture` 证明了两个**适配器**可以共存，但它是测试自己装配的；
//    "同一个进程里两个端口都在服务"这句话，只有在**真实二进制**上才算数
//    （组合根 R12：具体实现在 `src/main/` 里被创建，测试装配替代不了这一步）。
//
//  ★ 端口：HTTP 用 `FSS_HTTP_PORT=0`、gRPC 用 `FSS_GRPC_PORT=-1`（= 让系统分配），
//    实际端口从启动横幅里解析 —— 硬编码端口会在并行 ctest 下互相踩。
//  ★ 生命周期：析构里 `kill`（与 `tests/framework/mock_s3.h` 同一套做法）。
//  ★ 自签 URL 的 `<base>` 里带的是"配置端口"（这里是 0），因此**不要**用本 fixture
//    去 PUT 自签地址；它用于"两条协议同时服务同一份状态"的读写一致性检查
//    （gRPC 上传 → REST 读元数据 / REST 探活 + gRPC 探活并发）。
//
//  ---- 阶段 10 切片 1 的扩展（C10.1~C10.8）----
//    · `ServerProcessOptions`：可以传 `--config` / `--set` / 追加环境变量，并且可以
//      关掉默认的 `FSS_HTTP_PORT=0`（否则"配置文件里的端口"会被 env 覆盖，测不到文件层）；
//    · `RunServerForExit()`：跑一次真实二进制并拿**退出码 + 输出**（反向用例：配置非法
//      必须 exit 78），带 `timeout` 兜底，绝不为坏配置挂死。
//  ★ 默认构造的行为与扩展前**逐字一致**（既有使用者不受影响）。
// =============================================================================
#pragma once

#include "temp_dir.h"

#include <catch2/catch.hpp>

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fss::test {

//  与测试二进制同目录（`build/bin/` 或 `build-asan/bin/`），因此 sanitizer 门槛
//  跑的就是同一套构建的二进制
inline std::filesystem::path ServerBinaryPath() {
  std::error_code error;
  const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
  if (error) return {};
  return self.parent_path() / "fss_server";
}

//  单引号包裹：让含空格/逗号的参数与环境变量值原样到达进程
inline std::string ShellQuote(const std::string& text) {
  std::string out = "'";
  for (const char c : text) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

inline std::string ReadWholeFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

struct ServerProcessOptions {
  //  是否注入默认的 `FSS_HTTP_PORT=0` / `FSS_GRPC_PORT=-1`。配置面测试要自己控制端口时
  //  必须关掉 HTTP 那个（env 的优先级高于配置文件，否则"文件里的端口"永远测不到）。
  bool default_http_port = true;
  bool default_grpc_port = true;
  //  是否要求从横幅解析出非 0 的 gRPC 端口（默认构造 = 要求，与扩展前一致）
  bool expect_grpc = true;
  //  是否注入默认的存储/密钥环境变量（STORAGE_DRIVER/ROOT/SQLITE_PATH/TRANSFER_SECRET）
  bool default_storage_env = true;
  //  追加的环境变量（**最后生效**，可覆盖默认值）
  std::vector<std::pair<std::string, std::string>> env;
  //  追加的命令行参数（`--config <path>` / `--set k=v` / `--print-config` …）
  std::vector<std::string> args;
};

class ServerProcess {
 public:
  ServerProcess() : ServerProcess(ServerProcessOptions{}) {}
  explicit ServerProcess(ServerProcessOptions options) { Start(std::move(options)); }

  ~ServerProcess() {
    if (!pid_.empty()) {
      (void)std::system(("kill " + pid_ + " 2>/dev/null || true").c_str());
    }
  }
  ServerProcess(const ServerProcess&) = delete;
  ServerProcess& operator=(const ServerProcess&) = delete;

  int http_port() const { return http_port_; }
  int grpc_port() const { return grpc_port_; }
  const std::string& pid() const { return pid_; }
  const std::string& log_path() const { return log_path_; }
  std::string DumpLog() const { return ReadWholeFile(log_path_); }
  //  进程还在跑吗（反向用例里"拒绝了但仍然活着"要能被发现）
  bool alive() const {
    if (pid_.empty()) return false;
    return std::system(("kill -0 " + pid_ + " 2>/dev/null").c_str()) == 0;
  }

 private:
  void Start(ServerProcessOptions options) {
    const auto binary = ServerBinaryPath();
    //  前置条件显式断言 + 可执行的修复指令（R9）。Catch2 v2 没有 REQUIRE_MESSAGE，
    //  用 `INFO` + `REQUIRE` 表达同样的信息。
    INFO("组合根二进制：" << binary.string()
                          << "（不存在时请先 cmake --build build --target fss_server）");
    REQUIRE(std::filesystem::exists(binary));
    log_path_ = dir_.child("server.log");

    std::vector<std::pair<std::string, std::string>> envs;
    envs.emplace_back("FSS_BIND_ADDRESS", "127.0.0.1");
    if (options.default_http_port) envs.emplace_back("FSS_HTTP_PORT", "0");
    if (options.default_grpc_port) envs.emplace_back("FSS_GRPC_PORT", "-1");
    if (options.default_storage_env) {
      envs.emplace_back("FSS_STORAGE_DRIVER", "posix");
      envs.emplace_back("FSS_STORAGE_ROOT", dir_.child("data"));
      envs.emplace_back("FSS_SQLITE_PATH", dir_.child("data/location.db"));
      envs.emplace_back("FSS_TRANSFER_SECRET", "test-secret");
    }
    for (auto& kv : options.env) envs.push_back(std::move(kv));

    std::string command = "env ";
    for (const auto& [key, value] : envs) {
      command += key;
      command += '=';
      command += ShellQuote(value);
      command += ' ';
    }
    command += ShellQuote(binary.string());
    for (const auto& arg : options.args) {
      command += ' ';
      command += ShellQuote(arg);
    }
    command += " > " + ShellQuote(log_path_) + " 2>&1 & echo $!";

    std::FILE* pipe = ::popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buffer[64] = {0};
    REQUIRE(std::fgets(buffer, sizeof(buffer), pipe) != nullptr);
    pid_ = buffer;
    while (!pid_.empty() && (pid_.back() == '\n' || pid_.back() == ' ')) pid_.pop_back();
    ::pclose(pipe);
    CAPTURE(pid_);
    REQUIRE_FALSE(pid_.empty());

    //  轮询启动横幅里的端口（不用固定 sleep：就绪时间随机器变化）
    const bool want_grpc = options.expect_grpc;
    for (int attempt = 0; attempt < 200; ++attempt) {
      const bool http_ready = http_port_ != 0;
      const bool grpc_ready = !want_grpc || grpc_port_ != 0;
      if (http_ready && grpc_ready) break;
      std::ifstream log(log_path_);
      std::string line;
      while (std::getline(log, line)) {
        if (http_port_ == 0 && line.find("  bind           : ") != std::string::npos) {
          http_port_ = PortOf(line);
        }
        if (grpc_port_ == 0 && line.find("  grpc bind      : ") != std::string::npos) {
          grpc_port_ = PortOf(line);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CAPTURE(http_port_, grpc_port_, DumpLog());
    REQUIRE(http_port_ != 0);  // 超时未报出 HTTP 端口
    if (want_grpc) REQUIRE(grpc_port_ != 0);  // 超时未报出 gRPC 端口
  }

  static int PortOf(const std::string& line) {
    const auto colon = line.rfind(':');
    if (colon == std::string::npos) return 0;
    try {
      return std::stoi(line.substr(colon + 1));
    } catch (...) {
      return 0;
    }
  }

  fss::test::TempDir dir_{"fss_server_proc"};
  std::string log_path_;
  std::string pid_;
  int http_port_ = 0;
  int grpc_port_ = 0;
};

//  ---- 反向用例：跑一次真实二进制，拿退出码与完整输出 ----
struct ProcessOutcome {
  int exit_code = -1;
  std::string output;  // stdout + stderr（合并）
};

//  `timeout` 是兜底：坏配置必须**立刻**退出，真挂死了这里会给出 124 而不是无限等待。
inline ProcessOutcome RunServerForExit(const std::vector<std::string>& args,
                                       const std::vector<std::pair<std::string, std::string>>& env = {},
                                       int timeout_seconds = 30) {
  ProcessOutcome outcome;
  fss::test::TempDir dir("fss_server_once");
  const std::string log_path = dir.child("out.log");
  std::string command = "timeout " + std::to_string(timeout_seconds) + " env ";
  for (const auto& [key, value] : env) {
    command += key;
    command += '=';
    command += ShellQuote(value);
    command += ' ';
  }
  command += ShellQuote(ServerBinaryPath().string());
  for (const auto& arg : args) {
    command += ' ';
    command += ShellQuote(arg);
  }
  command += " > " + ShellQuote(log_path) + " 2>&1; echo EXIT:$?";

  std::FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) return outcome;
  std::string marker;
  char buffer[128] = {0};
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) marker += buffer;
  const int status = ::pclose(pipe);
  (void)status;
  const auto pos = marker.rfind("EXIT:");
  if (pos != std::string::npos) {
    try {
      outcome.exit_code = std::stoi(marker.substr(pos + 5));
    } catch (...) {
      outcome.exit_code = -1;
    }
  }
  outcome.output = ReadWholeFile(log_path);
  return outcome;
}

}  // namespace fss::test
