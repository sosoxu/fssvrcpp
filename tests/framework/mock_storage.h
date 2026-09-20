// =============================================================================
//  mock_storage.h —— 把 `tests/tools/mock_validators.py --mode storage` 拉起来
// =============================================================================
//  与 `mock_validators.h` / `mock_entitlements.h` / `mock_s3.h` 同一套做法
//  （`popen` 取 pid、轮询 stdout 里的 `LISTENING <port>`、析构时 kill）。用独立进程
//  而不是"进程内假服务"，是因为本切片要求的故障形态（超时 / 5xx / 401 / 404 / 非 JSON）
//  只有真实进程才能确定性注入；并且 M9/M10/M11 要的是**真实 `fss_server` 二进制**
//  按配置选择远端仓储 —— 那不是进程内替身能证明的（AGENTS §4.3 的"夹具接上了、
//  组合根没接 = 产品里不存在"）。
//
//  覆盖的线协议（`--mode storage`；ADR-004 / `docs/01-osdu-research.md`）：
//    PUT  {base}/records             → 200 记录 JSON（version 按 id 递增）
//    GET  {base}/records/{id}        → 200 记录 JSON / 404
//    POST {base}/records/{id}:delete → 204（`delete_status` 可改成 200 等）
//  故障开关：`fail_put` / `fail_get` / `timeout_ms`（响应前睡）/ `require_token`（401）。
//  ★ 故障响应体故意是"看起来成功的记录 JSON（version=999）"，这样"非 2xx 必须
//    fail-closed"只能靠**真的检查状态码**满足（R1：判据要有区分力）。
//
//  ★ `port`：默认 0 = 系统分配。M10（就绪探针"杀掉→重启"）必须用**固定端口**，
//    否则重启后端口变了、组合根配置里的 base_url 就失效了。
// =============================================================================
#pragma once

#include "repo_path.h"
#include "temp_dir.h"

#include "common/json/json.h"

#include <signal.h>
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

class MockStorage {
 public:
  struct Options {
    int port = 0;  // 0 = 系统分配；固定端口用于"杀掉后同端口重启"（M10）
    int fail_put = 0;
    int fail_get = 0;
    int timeout_ms = 0;  // 响应前睡 N 毫秒（> 客户端 timeout_ms → 触发超时）
    std::string require_token;  // 非空 → 要求 `Authorization: Bearer <t>`
    bool put_envelope = false;  // PUT 返回 OSDU `{recordCount,recordIds,versions}` 信封
    int delete_status = 204;    // `:delete` 的状态码（200 = ADR-004 要求的失败形态）
    bool malformed = false;     // GET/PUT 一律回非 JSON（映射到 kUnavailable）
  };

  MockStorage() : MockStorage(Options{}) {}
  explicit MockStorage(Options options) : options_(std::move(options)) {
    const std::string script = RepoRelative("tests/tools/mock_validators.py");
    INFO("mock storage 脚本：" << script);
    REQUIRE(std::filesystem::exists(script));
    const std::string unique = std::to_string(::getpid()) + "_" +
                               std::to_string(instance_counter()++);
    port_file_ = "/tmp/fss_mock_storage_" + unique + ".out";
    observe_file_ = "/tmp/fss_mock_storage_" + unique + ".json";
    std::error_code error;
    std::filesystem::remove(observe_file_, error);
    Start(script);
  }

  ~MockStorage() { Stop(); }

  MockStorage(const MockStorage&) = delete;
  MockStorage& operator=(const MockStorage&) = delete;

  //  ★ 显式停止（幂等）：析构也会调。M10 用它把"依赖挂掉"变成**确定的**状态，
  //    而不用把 mock 对象拆成嵌套作用域。
  void Stop() {
    if (pid_.empty()) return;
    //  先 SIGTERM，并**轮询**确认进程真的消失（`kill(pid,0)` 失败）——固定 sleep 会让
    //  "在同端口重启"变成时序赌博（AGENTS §4.3）。
    (void)::kill(static_cast<pid_t>(std::stoi(pid_)), SIGTERM);
    for (int i = 0; i < 200; ++i) {
      if (::kill(static_cast<pid_t>(std::stoi(pid_)), 0) != 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    (void)::kill(static_cast<pid_t>(std::stoi(pid_)), SIGKILL);
    pid_.clear();
    std::error_code error;
    std::filesystem::remove(port_file_, error);
  }

  int port() const { return port_; }
  //  组合根要填的是**基址**（适配器会追加 /records）
  std::string base_url() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/api/storage/v2";
  }
  const std::string& observe_file() const { return observe_file_; }

  struct Observation {
    int requests = 0;
    int puts = 0;
    int gets = 0;
    int deletes = 0;
    json::Value body = json::Value::object();  // last_body
    std::map<std::string, std::string> last_headers;
    std::vector<json::Value> bodies;  // 全部请求体（按到达顺序）
    std::vector<json::Value> calls;   // {method,path,headers,body}
    std::string raw;

    //  按方法+路径过滤（例如 CountCalls("PUT", "/api/storage/v2/records")）。
    int CountCalls(const std::string& method, const std::string& path) const {
      int n = 0;
      for (const auto& call : calls) {
        if (!call.is_object()) continue;
        if (call.value("method", std::string()) == method &&
            call.value("path", std::string()) == path) {
          ++n;
        }
      }
      return n;
    }
    const json::Value* LastCall(const std::string& method, const std::string& path) const {
      for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
        if (!it->is_object()) continue;
        if (it->value("method", std::string()) == method &&
            it->value("path", std::string()) == path) {
          return &(*it);
        }
      }
      return nullptr;
    }
  };

  Observation ReadObservation() const {
    Observation out;
    std::ifstream in(observe_file_);
    if (!in.good()) return out;
    std::stringstream buffer;
    buffer << in.rdbuf();
    out.raw = buffer.str();
    const auto parsed = json::Parse(out.raw);
    if (!parsed.ok() || !parsed.value().is_object()) return out;
    const auto& root = parsed.value();
    const auto read_int = [&](const char* key) -> int {
      const auto it = root.find(key);
      if (it != root.end() && it->is_number_integer()) return it->get<int>();
      return 0;
    };
    out.requests = read_int("requests");
    out.puts = read_int("puts");
    out.gets = read_int("gets");
    out.deletes = read_int("deletes");
    if (root.contains("last_body")) out.body = root["last_body"];
    if (root.contains("last_headers") && root["last_headers"].is_object()) {
      for (auto it = root["last_headers"].begin(); it != root["last_headers"].end(); ++it) {
        if (it.value().is_string()) out.last_headers[it.key()] = it.value().get<std::string>();
      }
    }
    if (root.contains("bodies") && root["bodies"].is_array()) {
      for (const auto& entry : root["bodies"]) out.bodies.push_back(entry);
    }
    if (root.contains("calls") && root["calls"].is_array()) {
      for (const auto& entry : root["calls"]) out.calls.push_back(entry);
    }
    return out;
  }

  Observation WaitRequests(int count, int timeout_ms = 5000) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    Observation last = ReadObservation();
    while (last.requests < count && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      last = ReadObservation();
    }
    return last;
  }

  //  等 PUT 计数达到 count（M1/M2/M9 用；比"等总请求数"更精确）。
  Observation WaitPuts(int count, int timeout_ms = 5000) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    Observation last = ReadObservation();
    while (last.puts < count && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      last = ReadObservation();
    }
    return last;
  }

  //  断言"在给的窗口内没有新请求"（例如 M2 的"第二次不发 PUT"）。
  bool PutsStayAt(int expected, int window_ms = 500) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(window_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (ReadObservation().puts != expected) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return ReadObservation().puts == expected;
  }

  int pid_for_test() const { return pid_.empty() ? 0 : std::stoi(pid_); }

 private:
  static int& instance_counter() {
    static int counter = 0;
    return counter;
  }

  static std::string ShellQuote(const std::string& text) {
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

  void Start(const std::string& script) {
    std::string command = "python3 " + script + " --port " + std::to_string(options_.port) +
                          " --mode storage";
    if (options_.fail_put != 0) command += " --fail-put " + std::to_string(options_.fail_put);
    if (options_.fail_get != 0) command += " --fail-get " + std::to_string(options_.fail_get);
    if (options_.timeout_ms > 0) command += " --timeout-ms " + std::to_string(options_.timeout_ms);
    if (!options_.require_token.empty()) {
      command += " --require-token " + ShellQuote(options_.require_token);
    }
    if (options_.put_envelope) command += " --put-envelope";
    if (options_.malformed) command += " --malformed";
    if (options_.delete_status != 204) {
      command += " --delete-status " + std::to_string(options_.delete_status);
    }
    command += " --observe-file " + ShellQuote(observe_file_);
    command += " > " + port_file_ + " 2>/dev/null & echo $!";

    std::FILE* pipe = ::popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buffer[64] = {0};
    REQUIRE(std::fgets(buffer, sizeof(buffer), pipe) != nullptr);
    pid_ = buffer;
    while (!pid_.empty() && (pid_.back() == '\n' || pid_.back() == ' ')) pid_.pop_back();
    ::pclose(pipe);

    port_ = 0;
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

  Options options_;
  std::string port_file_;
  std::string observe_file_;
  std::string pid_;
  int port_ = 0;
};

}  // namespace fss::test
