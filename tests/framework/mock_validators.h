// =============================================================================
//  mock_validators.h —— 把 `tests/tools/mock_validators.py` 拉起来
// =============================================================================
//  与 `mock_entitlements.h` / `mock_s3.h` 同一套做法（`popen` 取 pid、轮询 stdout 里的
//  `LISTENING <port>`、析构时 kill）。用独立进程而不是"进程内假服务"，是因为
//  C10.18 要求的故障形态（超时 / 连不上 / 5xx）只有真实进程才能确定性注入。
//
//  额外能力：`--observe-file`（mock 每次请求后把请求计数与请求体原子写进该文件）。
//  测试用它断言三件事：
//    ① **请求体形状**（legal 有 partition/legaltags；schema 有 kind/record；
//       webhook 有 topic/kind/body）—— 否则"客户端确实按约定发了"只是文档里的一句话（R15）；
//    ② **"不发请求"**（`requests == 0`）—— noop / none 模式下"不发起任何请求"必须可证；
//    ③ **"两个事件都发了"**（`bodies`，切片 6b 新增）—— 一次 createMetadata 会产生
//       statusChanged 与 datasetDetails 两个事件，只看 `last_body` 无法断言这件事。
//
//  ⚠️ 本文件只在**真实进程**用例里用（`build/bin/fss_server` + 本 mock），
//     不测进程内假对象：判据是"真实二进制按配置选择了远端校验器 / webhook 发布器"。
// =============================================================================
#pragma once

#include "repo_path.h"
#include "temp_dir.h"

#include "common/json/json.h"

#include <unistd.h>

#include <catch2/catch.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fss::test {

class MockValidators {
 public:
  struct Options {
    std::string mode = "legal";  // legal | schema | webhook（同时决定默认响应语义）
    bool valid = false;          // 返回 {"valid":true}
    std::string invalid_message; // 返回 {"valid":false,"message":M}
    int delay_ms = 0;            // 响应前睡多久（用于触发客户端超时）
    int force_status = 0;        // 非 0 → 一律返回该状态码
    bool malformed = false;      // 返回非 JSON
    bool no_valid_field = false; // 200 + {}（缺 valid）
    std::string fail_file;       // 该文件存在时一律 500（删除即恢复）
  };

  MockValidators() : MockValidators(Options{}) {}
  explicit MockValidators(Options options) : options_(std::move(options)) {
    const std::string script = RepoRelative("tests/tools/mock_validators.py");
    INFO("mock 校验器脚本：" << script);
    REQUIRE(std::filesystem::exists(script));
    const std::string unique = std::to_string(::getpid()) + "_" +
                               std::to_string(instance_counter()++);
    port_file_ = "/tmp/fss_mock_validators_" + unique + ".out";
    observe_file_ = "/tmp/fss_mock_validators_" + unique + ".json";
    std::error_code error;
    std::filesystem::remove(observe_file_, error);

    std::string command = "python3 " + script + " --port 0 --mode " + options_.mode;
    if (options_.valid) command += " --valid";
    if (!options_.invalid_message.empty()) {
      command += " --invalid-message " + ShellQuoteForMock(options_.invalid_message);
    }
    if (options_.delay_ms > 0) command += " --delay-ms " + std::to_string(options_.delay_ms);
    if (options_.force_status != 0) command += " --status " + std::to_string(options_.force_status);
    if (options_.malformed) command += " --malformed";
    if (options_.no_valid_field) command += " --no-valid-field";
    if (!options_.fail_file.empty()) {
      command += " --fail-file " + ShellQuoteForMock(options_.fail_file);
    }
    command += " --observe-file " + ShellQuoteForMock(observe_file_);
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

  ~MockValidators() {
    if (!pid_.empty()) {
      (void)std::system(("kill " + pid_ + " 2>/dev/null || true").c_str());
    }
    std::error_code error;
    std::filesystem::remove(port_file_, error);
    std::filesystem::remove(observe_file_, error);
  }
  MockValidators(const MockValidators&) = delete;
  MockValidators& operator=(const MockValidators&) = delete;

  int port() const { return port_; }
  std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_); }
  const std::string& observe_file() const { return observe_file_; }

  //  观测快照（mock 还没收到过请求时 → requests=0 / body 为空对象）。
  //  ★ 判据用**轮询**而非固定 sleep（AGENTS §4.3）：`WaitRequests(n)` 等"计数到 n"，
  //    `RequestsStayZero()` 在等"必须保持 0"时用（给 mock 一点时间也收不到请求）。
  struct Observation {
    int requests = 0;
    json::Value body = json::Value::object();  // last_body（解析后的 JSON）
    //  ★ 切片 6b：**全部**请求体（按到达顺序）。一次 createMetadata 产生的
    //    statusChanged / datasetDetails 都在这里；`last_body` 只保留兼容。
    std::vector<json::Value> bodies;
    //  最后一次请求的表头（切片 6b 用它断言 `Content-Type: application/json`）
    std::map<std::string, std::string> last_headers;
    std::string raw;                           // 整个观测文件的文本（诊断用）

    //  ★ 切片 6b：按 `kind` 过滤 webhook 载荷（`{"topic","kind","body"}`）。
    //    没有它就只能在测试里手写循环；把它放这里让"两个 kind 都发了"的断言更直白。
    std::vector<json::Value> BodiesOfKind(const std::string& kind) const {
      std::vector<json::Value> out;
      for (const auto& entry : bodies) {
        if (entry.is_object() && entry.contains("kind") && entry["kind"].is_string() &&
            entry["kind"].get<std::string>() == kind) {
          out.push_back(entry);
        }
      }
      return out;
    }
  };

  Observation ReadObservation() const {
    Observation out;
    std::ifstream in(observe_file_);
    if (!in.good()) return out;
    std::stringstream buffer;
    buffer << in.rdbuf();
    out.raw = buffer.str();
    //  ★ 用项目自己的 JSON 解析器（而不是手写 substring）：观测文件由 mock 原子写入，
    //    但解析失败仍要能被发现 —— 解析失败时保持 requests=0，调用方的断言会失败。
    const auto parsed = json::Parse(out.raw);
    if (!parsed.ok() || !parsed.value().is_object()) return out;
    const auto& root = parsed.value();
    if (root.contains("requests") && root["requests"].is_number_integer()) {
      out.requests = root["requests"].get<int>();
    }
    if (root.contains("last_body")) out.body = root["last_body"];
    if (root.contains("last_headers") && root["last_headers"].is_object()) {
      for (auto it = root["last_headers"].begin(); it != root["last_headers"].end(); ++it) {
        if (it.value().is_string()) out.last_headers[it.key()] = it.value().get<std::string>();
      }
    }
    if (root.contains("bodies") && root["bodies"].is_array()) {
      for (const auto& entry : root["bodies"]) out.bodies.push_back(entry);
    }
    return out;
  }

  //  等到 requests >= count（返回最后一次观测）。超时返回当前观测（调用方自行断言）。
  Observation WaitRequests(int count, int timeout_ms = 5000) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    Observation last = ReadObservation();
    while (last.requests < count && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      last = ReadObservation();
    }
    return last;
  }

  //  断言"在给的窗口内一个请求都没收到"（noop 模式的反向证据）。
  bool RequestsStayZero(int window_ms = 300) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(window_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (ReadObservation().requests != 0) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return ReadObservation().requests == 0;
  }

 private:
  static int& instance_counter() {
    static int counter = 0;
    return counter;
  }

  //  单引号包裹：让含空格/逗号/中文的参数原样到达 python
  static std::string ShellQuoteForMock(const std::string& text) {
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

  Options options_;
  std::string port_file_;
  std::string observe_file_;
  std::string pid_;
  int port_ = 0;
};

}  // namespace fss::test
