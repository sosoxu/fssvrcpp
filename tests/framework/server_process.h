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
// =============================================================================
#pragma once

#include "temp_dir.h"

#include <catch2/catch.hpp>

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fss::test {

class ServerProcess {
 public:
  ServerProcess() {
    const auto binary = BinaryPath();
    //  前置条件显式断言 + 可执行的修复指令（R9）。Catch2 v2 没有 REQUIRE_MESSAGE，
    //  用 `INFO` + `REQUIRE` 表达同样的信息。
    INFO("组合根二进制：" << binary.string()
                          << "（不存在时请先 cmake --build build --target fss_server）");
    REQUIRE(std::filesystem::exists(binary));
    log_path_ = dir_.child("server.log");
    const std::string command =
        "FSS_BIND_ADDRESS=127.0.0.1 FSS_HTTP_PORT=0 FSS_GRPC_PORT=-1 FSS_STORAGE_DRIVER=posix" +
        std::string(" FSS_STORAGE_ROOT=") + dir_.child("data") + " FSS_SQLITE_PATH=" +
        dir_.child("data/location.db") + " FSS_TRANSFER_SECRET=test-secret " +
        binary.string() + " > " + log_path_ + " 2>&1 & echo $!";
    std::FILE* pipe = ::popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buffer[64] = {0};
    REQUIRE(std::fgets(buffer, sizeof(buffer), pipe) != nullptr);
    pid_ = buffer;
    while (!pid_.empty() && (pid_.back() == '\n' || pid_.back() == ' ')) pid_.pop_back();
    ::pclose(pipe);
    CAPTURE(pid_);
    REQUIRE_FALSE(pid_.empty());

    //  轮询启动横幅里的两个端口（不用固定 sleep：就绪时间随机器变化）
    for (int attempt = 0; attempt < 200 && (http_port_ == 0 || grpc_port_ == 0); ++attempt) {
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
      if (http_port_ == 0 || grpc_port_ == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
    }
    CAPTURE(http_port_, grpc_port_, DumpLog());
    REQUIRE(http_port_ != 0);   // 超时未报出 HTTP 端口
    REQUIRE(grpc_port_ != 0);   // 超时未报出 gRPC 端口
  }

  ~ServerProcess() {
    if (!pid_.empty()) {
      (void)std::system(("kill " + pid_ + " 2>/dev/null || true").c_str());
    }
  }
  ServerProcess(const ServerProcess&) = delete;
  ServerProcess& operator=(const ServerProcess&) = delete;

  int http_port() const { return http_port_; }
  int grpc_port() const { return grpc_port_; }
  std::string DumpLog() const {
    std::ifstream log(log_path_);
    std::string all((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    return all;
  }

 private:
  //  与测试二进制同目录（`build/bin/` 或 `build-asan/bin/`），因此 sanitizer 门槛
  //  跑的就是同一套构建的二进制
  static std::filesystem::path BinaryPath() {
    std::error_code error;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) return {};
    return self.parent_path() / "fss_server";
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

}  // namespace fss::test
