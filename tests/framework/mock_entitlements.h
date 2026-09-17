// =============================================================================
//  mock_entitlements.h —— 把 `tests/tools/mock_entitlements.py` 拉起来
// =============================================================================
//  与 `mock_s3.h` 同一套做法（`popen` 取 pid、轮询 stdout 里的 `LISTENING <port>`、
//  析构时 kill）。用独立进程而不是"进程内假服务"，是为了让**超时/连接失败**这类
//  故障形态可注入（C8.4 明确要求"有测试注入超时"）。
// =============================================================================
#pragma once

#include "temp_dir.h"

#include <unistd.h>

#include <catch2/catch.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace fss::test {

//  ★ 与 `mock_s3.h` 共用同一个 `RepoRelative`（同一命名空间、inline 定义）
inline std::string RepoRelative(const std::string& relative) {
  return std::string(FSS_REPO_ROOT) + "/" + relative;
}

class MockEntitlements {
 public:
  struct Options {
    //  逗号分隔的被授予角色（空 = 一律拒绝）
    std::string grant;
    int delay_ms = 0;         // 响应前睡多久（用于触发客户端超时）
    int force_status = 0;     // 非 0 → 一律返回该状态码
    bool malformed = false;   // 返回非 JSON
    std::string require_partition;  // 收到的 data-partition-id 不符 → 400
    std::string require_role;       // 请求里没有该角色 → 400
  };

  MockEntitlements() : MockEntitlements(Options{}) {}
  explicit MockEntitlements(Options options) : options_(std::move(options)) {
    const std::string script = RepoRelative("tests/tools/mock_entitlements.py");
    INFO("mock entitlements 脚本：" << script);
    REQUIRE(std::filesystem::exists(script));
    port_file_ = "/tmp/fss_mock_entitlements_" + std::to_string(::getpid()) + "_" +
                 std::to_string(instance_counter()++);
    std::string command = "python3 " + script + " --port 0";
    if (!options_.grant.empty()) command += " --grant " + options_.grant;
    if (options_.delay_ms > 0) command += " --delay-ms " + std::to_string(options_.delay_ms);
    if (options_.force_status != 0) command += " --status " + std::to_string(options_.force_status);
    if (options_.malformed) command += " --malformed";
    if (!options_.require_partition.empty()) {
      command += " --require-partition " + options_.require_partition;
    }
    if (!options_.require_role.empty()) command += " --require-role " + options_.require_role;
    command += " > " + port_file_ + " 2>/dev/null & echo $!";

    std::FILE* pipe = ::popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buffer[64] = {0};
    REQUIRE(std::fgets(buffer, sizeof(buffer), pipe) != nullptr);
    pid_ = buffer;
    while (!pid_.empty() && (pid_.back() == '\n' || pid_.back() == ' ')) pid_.pop_back();
    ::pclose(pipe);

    for (int attempt = 0; attempt < 200 && port_ == 0; ++attempt) {
      std::ifstream in(port_file_);
      std::string line;
      if (in.good() && std::getline(in, line)) {
        const auto space = line.find(' ');
        if (space != std::string::npos) {
          try {
            port_ = std::stoi(line.substr(space + 1));
          } catch (...) {
            port_ = 0;
          }
        }
      }
      if (port_ == 0) ::usleep(25 * 1000);
    }
    REQUIRE(port_ > 0);  // 前置条件显式断言（R9）
  }
  ~MockEntitlements() {
    if (!pid_.empty()) {
      (void)std::system(("kill " + pid_ + " 2>/dev/null || true").c_str());
    }
    std::error_code error;
    std::filesystem::remove(port_file_, error);
  }
  MockEntitlements(const MockEntitlements&) = delete;
  MockEntitlements& operator=(const MockEntitlements&) = delete;

  int port() const { return port_; }
  std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_); }

 private:
  static int& instance_counter() {
    static int counter = 0;
    return counter;
  }

  Options options_;
  std::string port_file_;
  std::string pid_;
  int port_ = 0;
};

}  // namespace fss::test
