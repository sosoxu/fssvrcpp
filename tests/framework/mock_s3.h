// =============================================================================
//  tests/framework/mock_s3.h —— 起一个 mock-S3 进程（Python 独立验签）的公共脚手架
// =============================================================================
//  为什么放进 framework：S3 相关的用例（预签名验签、数据面契约、S3 模式端到端）都要
//  起同一个假 S3。各写一份的话，任何一处改动（端口获取方式、清理、seed）都会漂移
//  —— 与 P4 把 HTTP fixture 收进 framework 是同一个理由。
//
//  取端口的方式：`--port 0` + 读 stdout 的 `LISTENING <port>` 行（**轮询**，不用固定 sleep；
//  R9 的教训：固定 sleep 会在慢机器上偶发失败，且失败信息毫无指向）。
#pragma once

#include "repo_path.h"

#include <catch2/catch.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fss::test {

class MockS3 {
 public:
  struct Options {
    int seed_count = 0;
    std::string seed_prefix = "page/";
    std::string seed_bucket = "testbucket";
    std::string access_key = "AKIDEXAMPLE";
    std::string secret_key = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    std::string region = "us-east-1";
  };

  MockS3() : MockS3(Options{}) {}
  explicit MockS3(Options options) : options_(std::move(options)) {
    const std::string script = RepoRelative("tests/tools/mock_s3.py");
    REQUIRE(std::filesystem::exists(script));
    std::string command = "python3 " + script + " --port 0 --access-key " + options_.access_key +
                          " --secret-key '" + options_.secret_key + "' --region " +
                          options_.region;
    if (options_.seed_count > 0) {
      command += " --seed-count " + std::to_string(options_.seed_count) + " --seed-prefix " +
                 options_.seed_prefix + " --seed-bucket " + options_.seed_bucket;
    }
    port_file_ = "/tmp/fss_mock_s3_" + std::to_string(::getpid()) + "_" +
                 std::to_string(instance_counter()++);
    command += " > " + port_file_ + " 2>/dev/null & echo $!";

    std::FILE* pipe = ::popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buffer[64] = {0};
    REQUIRE(std::fgets(buffer, sizeof(buffer), pipe) != nullptr);
    pid_ = buffer;
    while (!pid_.empty() && (pid_.back() == '\n' || pid_.back() == ' ')) pid_.pop_back();
    ::pclose(pipe);

    for (int attempt = 0; attempt < 150 && port_ == 0; ++attempt) {
      std::ifstream in(port_file_);
      std::string line;
      if (in.good() && std::getline(in, line)) {
        const auto space = line.find(' ');
        if (space != std::string::npos) port_ = std::stoi(line.substr(space + 1));
      }
      if (port_ == 0) ::usleep(100 * 1000);
    }
    REQUIRE(port_ > 0);
  }

  ~MockS3() {
    if (!pid_.empty()) {
      (void)std::system(("kill " + pid_ + " 2>/dev/null || true").c_str());
    }
    std::error_code error;
    std::filesystem::remove(port_file_, error);
  }
  MockS3(const MockS3&) = delete;
  MockS3& operator=(const MockS3&) = delete;

  int port() const { return port_; }
  std::string endpoint() const { return "127.0.0.1:" + std::to_string(port_); }
  std::string base_url() const { return "http://" + endpoint(); }
  const std::string& access_key() const { return options_.access_key; }
  const std::string& secret_key() const { return options_.secret_key; }
  const std::string& region() const { return options_.region; }

 private:
  static int& instance_counter() {
    static int counter = 0;
    return counter;
  }

  Options options_;
  std::string pid_;
  std::string port_file_;
  int port_ = 0;
};

}  // namespace fss::test
