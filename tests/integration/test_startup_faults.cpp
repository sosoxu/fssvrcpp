// =============================================================================
//  C9.32（P9 补交，P10 期间完成）：**未预期异常必须以干净退出码结束**
// =============================================================================
//  背景（真实缺陷，实测）：`docker run --memory=128m --pids-limit=64` → 启动期
//  `pthread_create` 返回 `EAGAIN` → `std::thread` 抛 `std::system_error`。当时的
//  `main()` **没有任何 catch** → `terminate called after throwing an instance of
//  'std::system_error'` / `what(): Resource temporarily unavailable` / 容器
//  `ExitCode=139`（**不是 OOM**）。139 对运维不可读（看起来像段错误），把排障引向
//  错误方向；本切片把这条路径变成"可读原因 + 确定退出码 70（EX_SOFTWARE）"。
//
//  判据（本文件是它的**可回归形式**，每条都能因故障注入而失败）：
//    ① `throw_system_error` → exit_code == 70、输出含「未预期异常」与
//       「Resource temporarily unavailable」、**不含** `terminate` / `what():`、
//       且**不进入服务状态**（输出里没有「已启动」）；
//    ② `throw_bad_alloc` → exit_code == 70 + 可读原因（内存耗尽）；
//    ③ `throw_unknown`（**非 std** 类型）→ exit_code == 70 + 「未知类型」
//       —— 证明顶层 `catch (...)` 真的存在且生效；
//    ④ **不注入** → 正常启动（readiness 200）且**不出现**「未预期异常」
//       （R16 正例：证明①里那行输出**不是恒有**）；
//    ⑤ `throw_after_start`（HTTP 服务器已 Start()、GC 调度线程已在跑**之后**抛）
//       → exit_code == 70、**进程真的退出**（不是挂死）、不含 `terminate`、
//       且**墙钟退出耗时 < 5 s**（证明 RAII 清理没有卡在 join/死锁）。
//
//  ★ 故障注入接缝 = 环境变量 `FSS_STARTUP_FAULT_INJECT`（**不是配置键**；
//    为什么不做成配置键见 `src/main/server_main.cpp` 的说明，运维侧登记在
//    `docs/runbook.md` 的测试/演练小节，**明确写"不要在生产设置"**）。
//  ★ 全部断言都在**真实 `build/bin/fss_server`** 上做（组合根 R12：测试自己装配的
//    对象替代不了"进程真的走了顶层 catch"）。
// =============================================================================
#include <catch2/catch.hpp>

#include "raw_http.h"
#include "server_process.h"
#include "temp_dir.h"

#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using fss::test::ProcessOutcome;
using fss::test::RawClient;
using fss::test::RunServerForExit;
using fss::test::ServerProcess;
using fss::test::TempDir;

//  与组合根 `kExitInternalError` 同源（EX_SOFTWARE）。这里**重新声明**而不是 include
//  组合根：断言必须独立于实现，否则把常量改成 0 时测试会跟着改（R1 的注入 ② 正是
//  要抓这一点）。
constexpr int kExitInternalError = 70;

struct HttpReply {
  bool transport_ok = false;
  int status = 0;
  std::string body;
};

HttpReply HttpGet(int port, const std::string& target) {
  HttpReply reply;
  RawClient client(port, /*tcp_nodelay=*/true);
  if (!client.Connect()) return reply;
  if (!client.SendRequest("GET", target, {}, "")) return reply;
  const auto response = client.ReadResponse(10000);
  if (!response.has_value()) return reply;
  reply.transport_ok = true;
  reply.status = response->status;
  reply.body = response->body;
  return reply;
}

//  ★ 轮询实际条件（AGENTS §4.3：不用固定 sleep 等状态）。
bool WaitReady(int port, int attempts = 100) {
  for (int i = 0; i < attempts; ++i) {
    const auto reply = HttpGet(port, "/api/file/v2/readiness_check");
    if (reply.transport_ok && reply.status == 200) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

using EnvList = std::vector<std::pair<std::string, std::string>>;

//  跑一次真实二进制，但先在子 shell 里把该用户的进程/线程上限压到"**当前数 + headroom**"
//  —— 用于制造**真实的** `pthread_create` EAGAIN（而不是注入合成的异常）。
//  ★ 为什么必须留 headroom：只有"**先成功建出若干线程、再失败**"才会触发 httplib
//    `ThreadPool` 的致命形态（已建 joinable 线程在栈展开中被析构 → `terminate`）。
//    若第 1 个线程就失败，连旧实现也只是干净地抛异常 —— 那样判据**没有区分力**。
//  ★ 这条路径是本切片真正的根因所在：容器 `--pids-limit=64` 下 httplib 的线程池在
//    **runner 线程**里创建失败，旧代码会 terminate（139）。`FSS_STARTUP_FAULT_INJECT`
//    注入的是**主线程**上的合成异常，覆盖不到它。
fss::test::ProcessOutcome RunServerWithNprocHeadroom(const std::vector<std::string>& args,
                                                     const EnvList& env, int headroom,
                                                     int timeout_seconds = 30) {
  fss::test::ProcessOutcome outcome;
  TempDir dir("fss_server_nproc");
  const std::string log_path = dir.child("out.log");
  std::string command = "timeout --kill-after=5 " + std::to_string(timeout_seconds) + " env ";
  for (const auto& [key, value] : env) {
    command += key + "=" + fss::test::ShellQuote(value) + " ";
  }
  const std::string script =
      "cur=$(ps -u \"$(id -u)\" -L --no-headers 2>/dev/null | wc -l); cur=${cur:-0}; "
      "ulimit -u $((cur + " +
      std::to_string(headroom) + ")); " + "exec \"$0\" \"$@\"";
  command += "bash -c " + fss::test::ShellQuote(script) + " ";
  command += fss::test::ShellQuote(fss::test::ServerBinaryPath().string());
  for (const auto& arg : args) {
    command += ' ';
    command += fss::test::ShellQuote(arg);
  }
  command += " > " + fss::test::ShellQuote(log_path) + " 2>&1; echo EXIT:$?";

  std::FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) return outcome;
  std::string marker;
  char buffer[128] = {0};
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) marker += buffer;
  ::pclose(pipe);
  const auto pos = marker.rfind("EXIT:");
  if (pos != std::string::npos) {
    try {
      outcome.exit_code = std::stoi(marker.substr(pos + 5));
    } catch (...) {
      outcome.exit_code = -1;
    }
  }
  outcome.output = fss::test::ReadWholeFile(log_path);
  return outcome;
}

}  // namespace

// =============================================================================
//  ① 注入 std::system_error：精确复刻 pids 那次失败形态 → 干净退出码 70
// =============================================================================
TEST_CASE("★ C9.32：注入 std::system_error（pids EAGAIN 形态）→ exit 70 + 可读原因、无 terminate",
          "[phase9][startup-faults][c9.32]") {
  const ProcessOutcome outcome =
      RunServerForExit({}, EnvList{{"FSS_STARTUP_FAULT_INJECT", "throw_system_error"}});
  CAPTURE(outcome.exit_code, outcome.output);
  REQUIRE(outcome.exit_code == kExitInternalError);
  //  可读原因：静态提示 + `what()`（异常里唯一允许携带的内容）
  REQUIRE(outcome.output.find("未预期异常") != std::string::npos);
  REQUIRE(outcome.output.find("Resource temporarily unavailable") != std::string::npos);
  REQUIRE(outcome.output.find("pids-limit") != std::string::npos);
  //  ★ 反向：**不能**再是 terminate 那副面孔（139/134）
  REQUIRE(outcome.output.find("terminate") == std::string::npos);
  REQUIRE(outcome.output.find("what():") == std::string::npos);
  //  注入点在装配之前 ⇒ 从未进入服务状态
  REQUIRE(outcome.output.find("已启动") == std::string::npos);
  //  消息里不得出现配置面内容（更不得出现密钥）
  REQUIRE(outcome.output.find("config sources") == std::string::npos);
  REQUIRE(outcome.output.find("secret") == std::string::npos);
}

// =============================================================================
//  ② 注入 std::bad_alloc：内存耗尽 → exit 70 + 可读原因
// =============================================================================
TEST_CASE("★ C9.32：注入 std::bad_alloc → exit 70 + 可读原因",
          "[phase9][startup-faults][c9.32]") {
  const ProcessOutcome outcome =
      RunServerForExit({}, EnvList{{"FSS_STARTUP_FAULT_INJECT", "throw_bad_alloc"}});
  CAPTURE(outcome.exit_code, outcome.output);
  REQUIRE(outcome.exit_code == kExitInternalError);
  REQUIRE(outcome.output.find("未预期异常") != std::string::npos);
  REQUIRE(outcome.output.find("bad_alloc") != std::string::npos);
  REQUIRE(outcome.output.find("terminate") == std::string::npos);
  REQUIRE(outcome.output.find("已启动") == std::string::npos);
}

// =============================================================================
//  ③ 注入**非 std** 类型 → 顶层 `catch (...)` 必须接住（exit 70 + 「未知类型」）
// =============================================================================
TEST_CASE("★ C9.32：注入非 std 类型 → exit 70 + 「未知类型」（证明 catch (...) 生效）",
          "[phase9][startup-faults][c9.32]") {
  const ProcessOutcome outcome =
      RunServerForExit({}, EnvList{{"FSS_STARTUP_FAULT_INJECT", "throw_unknown"}});
  CAPTURE(outcome.exit_code, outcome.output);
  REQUIRE(outcome.exit_code == kExitInternalError);
  REQUIRE(outcome.output.find("未知类型") != std::string::npos);
  REQUIRE(outcome.output.find("未预期异常") != std::string::npos);
  //  非 std 类型若逃出 `main` → terminate（139/134）；这里必须没有
  REQUIRE(outcome.output.find("terminate") == std::string::npos);
}

// =============================================================================
//  ④ R16 正例：**不注入** → 正常启动（readiness 200）且不出现「未预期异常」
// =============================================================================
TEST_CASE("★ C9.32 R16 正例：不注入 → 正常启动（readiness 200）且不出现「未预期异常」",
          "[phase9][startup-faults][c9.32]") {
  ServerProcess server;  // 默认注入 FSS_HTTP_PORT=0 / storage env / secret
  REQUIRE(WaitReady(server.http_port()));

  const auto readiness = HttpGet(server.http_port(), "/api/file/v2/readiness_check");
  CAPTURE(readiness.status, readiness.body);
  REQUIRE(readiness.status == 200);
  REQUIRE(readiness.body == "File service is ready");

  const std::string log = server.DumpLog();
  CAPTURE(log);
  //  正控：服务确实起来了（否则下面那条否定判据可能只是"日志为空"而恒真）
  REQUIRE(log.find("fss_server 已启动") != std::string::npos);
  REQUIRE(log.find("未预期异常") == std::string::npos);
}

// =============================================================================
//  ⑤ RAII：服务器已启动 + GC 调度已跑之后再抛 → exit 70、< 5 s 退出、无 join 死锁
// =============================================================================
TEST_CASE("★ C9.32 RAII：服务器已启动 + GC 调度已起后抛 → exit 70、<5s 退出（无 join 死锁）",
          "[phase9][startup-faults][c9.32]") {
  TempDir dir("fss_fault_after_start");
  //  ★ 晚注入点要落在"服务器已 Start()、GC 调度线程已在跑"之后 ⇒ 显式打开 GC 周期
  //    调度（`gc.enabled` 默认 false，不开就没有调度线程可 join）。
  const EnvList env{
      {"FSS_STARTUP_FAULT_INJECT", "throw_after_start"},
      {"FSS_BIND_ADDRESS", "127.0.0.1"},
      {"FSS_HTTP_PORT", "0"},
      {"FSS_STORAGE_ROOT", dir.child("data")},
      {"FSS_TRANSFER_SECRET", "test-secret"},
      {"FSS_GC_ENABLED", "true"},
      {"FSS_GC_INTERVAL_SECONDS", "1"},
  };
  //  `timeout` 兜底 8 s：实现若卡在 join/死锁，`RunServerForExit` 会拿到 124/137
  //  而不是无限等待（helper 用 `--kill-after` 强制收尸）。
  const auto started = std::chrono::steady_clock::now();
  const ProcessOutcome outcome = RunServerForExit({}, env, /*timeout_seconds=*/8);
  const double elapsed_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  CAPTURE(outcome.exit_code, elapsed_seconds, outcome.output);

  REQUIRE(outcome.exit_code == kExitInternalError);
  //  ★ 墙钟判据：卡在 join/死锁时不可能 < 5 s
  REQUIRE(elapsed_seconds < 5.0);
  REQUIRE(outcome.output.find("terminate") == std::string::npos);
  //  ★ 证明"晚注入点确实在服务器起来之后"（而不是退化成了早注入点）
  REQUIRE(outcome.output.find("fss_server 已启动") != std::string::npos);
  REQUIRE(outcome.output.find("gc             : 已启动") != std::string::npos);
}

// =============================================================================
//  ⑥ **真实** `pthread_create` EAGAIN（`ulimit -u` 限制）→ 仍须 exit 70 + 可读原因
//     ★ 这条覆盖合成注入覆盖不到的根因：线程池是在 **runner 线程**里创建的
//       （httplib 的 `listen_internal()`），旧实现会因析构 joinable 线程而 `terminate`；
//       只加"顶层 catch"**修不了**它（实测：加了顶层 catch 仍是 139 / 偶发挂死）。
// =============================================================================
TEST_CASE("★ C9.32：真实线程创建 EAGAIN（**部分创建**）→ exit 70 + 可读原因、无 terminate",
          "[phase9][startup-faults][c9.32]") {
  TempDir dir("fss_fault_nproc_data");
  const EnvList env{
      {"FSS_BIND_ADDRESS", "127.0.0.1"},
      {"FSS_HTTP_PORT", "0"},
      {"FSS_STORAGE_ROOT", dir.child("data")},
      {"FSS_TRANSFER_SECRET", "test-secret"},
  };
  //  worker_threads=20000 ≫ headroom ⇒ 线程池**创建到一半**才 EAGAIN（这正是 httplib
  //  `ThreadPool` 会 terminate 的形态）；headroom=500 < 20000 且 > 进程自身占用，
  //  保证"先成功建出若干线程、再失败"。
  //  ★ 为什么是 500 而不是 40：部分创建 + 清理要**跨越主线程的 2ms 轮询**，才能把
  //    "只等 is_running()"的竞态（自证注入 ⑤）暴露成挂死；40 太快会漏掉它。
  const ProcessOutcome outcome = RunServerWithNprocHeadroom(
      {"--set", "server.http.worker_threads=20000",
       "--set", "server.http.transfer_buffer_bytes=4096",
       "--set", "server.http.transfer_memory_budget_bytes=100000000"},
      env, /*headroom=*/500);
  CAPTURE(outcome.exit_code, outcome.output);
  REQUIRE(outcome.exit_code == kExitInternalError);
  REQUIRE(outcome.output.find("未预期异常") != std::string::npos);
  REQUIRE(outcome.output.find("Resource temporarily unavailable") != std::string::npos);
  REQUIRE(outcome.output.find("terminate") == std::string::npos);
  REQUIRE(outcome.output.find("已启动") == std::string::npos);
}

// =============================================================================
//  ★ 位置自证：注入点在 **CLI 解析之后** —— `--help` 不被注入打断（仍 exit 0）
// =============================================================================
TEST_CASE("★ C9.32：注入点在 CLI 解析之后 —— --help 仍 exit 0（不被注入打断）",
          "[phase9][startup-faults][c9.32]") {
  const ProcessOutcome outcome =
      RunServerForExit({"--help"}, EnvList{{"FSS_STARTUP_FAULT_INJECT", "throw_system_error"}});
  CAPTURE(outcome.exit_code, outcome.output);
  REQUIRE(outcome.exit_code == 0);
  REQUIRE(outcome.output.find("用法: fss_server") != std::string::npos);
  REQUIRE(outcome.output.find("未预期异常") == std::string::npos);
}
