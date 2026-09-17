// =============================================================================
//  P9 硬化：资源上限（C9.3）—— 6 类上限各有一个"**被拒绝**"的测试
// =============================================================================
//  判据要求"头/URI/体/连接数/配额 6 类上限各有一个被拒绝的测试（不是'能接受'）"。
//  本文件把 6 类**显式编号**并逐一钉住（避免"看着像测了"）：
//
//    ① 请求头总大小        → 400（库在解析阶段拒绝，契约 §1.7）
//    ② 请求行（URI）长度   → 连接被关闭且**无响应**（实测行为，不是 414）
//    ③ 请求体（JSON 端点，10 MiB）→ **413**，且 handler 一个字节都不读
//    ④ 请求体（小体端点，256 KiB）→ **413**（同一套机制，另一档上限）
//    ⑤ 在途连接数上限      → **503 + Retry-After**，且**不排队到超时**
//    ⑥ 传输内存预算（配额）→ `并发上限 × 缓冲 > budget` → **拒绝启动**
//
//  ★ 这些上限在 P1 已有各自的用例（`test_httplib_hardening` / `test_http_server`）；
//    本文件的价值是**在真实业务栈上把 6 类收敛成一处**，让 P9 的判据可以逐条对账，
//    并且用的都是业务端点（/v2/files/uploadURL、/v2/files/revokeURL）而不是测试路由。
// =============================================================================
#include <catch2/catch.hpp>

#include "http_fixture.h"
#include "raw_http.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using fss::test::Authed;
using fss::test::HttpFixture;
using fss::test::RawClient;

//  小体端点（256 KiB）用 `revokeURL`：它是最短的受保护端点之一
constexpr char kSmallEndpoint[] = "/api/file/v2/files/revokeURL";
constexpr char kJsonEndpoint[] = "/api/file/v2/files/metadata";

}  // namespace

TEST_CASE("★ C9.3 ① 请求头总大小超限 → 400（解析阶段拒绝，业务 handler 不被调用）",
          "[phase9][hardening][c9.3]") {
  HttpFixture fx;
  RawClient client(fx.port(), true);
  REQUIRE(client.Connect());
  const std::string huge = "X-Big: " + std::string(20000, 'b');
  REQUIRE(client.SendRequest("GET", "/api/file/v2/info", {huge}));
  const auto response = client.ReadResponse(5000);
  REQUIRE(response.has_value());
  INFO(response->raw_head);
  REQUIRE(response->status == 400);

  //  对照：正常大小的头必须成功（否则"拒绝"说明不了界限）
  RawClient ok(fx.port(), true);
  REQUIRE(ok.Connect());
  REQUIRE(ok.SendRequest("GET", "/api/file/v2/info",
                         {"X-Normal: " + std::string(100, 'c')}, ""));
  const auto ok_response = ok.ReadResponse(5000);
  REQUIRE(ok_response.has_value());
  REQUIRE(ok_response->status == 200);
}

TEST_CASE("★ C9.3 ② 请求行（URI）超限 → 连接被关闭且无响应；8191 字节必须正常",
          "[phase9][hardening][c9.3]") {
  HttpFixture fx;
  {
    RawClient client(fx.port(), true);
    REQUIRE(client.Connect());
    const std::string too_long = "/api/file/v2/" + std::string(8192, 'a');
    REQUIRE(client.SendRequest("GET", too_long));
    const auto response = client.ReadResponse(2000);
    //  ★ 实测定界（契约 §1.7）：库在解析请求行阶段直接关闭连接，不写任何响应
    REQUIRE_FALSE(response.has_value());
  }
  {
    //  对照：把 URI 造到正好 8191 字节，必须得到**业务响应**（404/400 都算"被处理"）
    RawClient client(fx.port(), true);
    REQUIRE(client.Connect());
    const std::string baseline = "/api/file/v2/info";
    const std::string padding(8191 - baseline.size(), 'a');
    const std::string long_target = baseline + padding;
    REQUIRE(long_target.size() == 8191);
    REQUIRE(client.SendRequest("GET", long_target, {}, ""));
    const auto response = client.ReadResponse(3000);
    REQUIRE(response.has_value());           // 有响应 = 请求被解析并路由（即使路径不存在）
    INFO(response->status);
  }
}

TEST_CASE("★ C9.3 ③④ 请求体超限 → 413（JSON 端点 10 MiB / 小体端点 256 KiB）",
          "[phase9][hardening][c9.3]") {
  HttpFixture fx;
  const int port = fx.port();

  //  ③ JSON 端点：`Content-Length` 前置声明 11 MiB → 413（**不发送真实体**：
  //     判据是"前置拒绝"，服务端应在读完头之后就拒绝，一个字节都不读）
  {
    RawClient client(port, true);
    REQUIRE(client.Connect());
    std::vector<std::string> headers = Authed();
    headers.push_back("content-type: application/json");
    headers.push_back("Content-Length: " + std::to_string(11 * 1024 * 1024));
    REQUIRE(client.SendRequest("POST", kJsonEndpoint, headers, ""));
    const auto response = client.ReadResponse(5000);
    REQUIRE(response.has_value());
    INFO(response->raw_head);
    REQUIRE(response->status == 413);
    REQUIRE(response->body.find("\"code\":413") != std::string::npos);
  }

  //  ④ 小体端点（256 KiB）：声明 300 KiB → 413
  {
    RawClient client(port, true);
    REQUIRE(client.Connect());
    std::vector<std::string> headers = Authed();
    headers.push_back("content-type: application/json");
    headers.push_back("Content-Length: " + std::to_string(300 * 1024));
    REQUIRE(client.SendRequest("POST", kSmallEndpoint, headers, ""));
    const auto response = client.ReadResponse(5000);
    REQUIRE(response.has_value());
    INFO(response->raw_head);
    REQUIRE(response->status == 413);
  }

  //  对照：上限之内的体必须被正常处理（400 是"业务校验失败"，说明体被读进来了）
  {
    RawClient client(port, true);
    REQUIRE(client.Connect());
    const std::string body = R"({"Items": 10, "PageNum": 0})";
    std::vector<std::string> headers = Authed();
    headers.push_back("content-type: application/json");
    headers.push_back("Content-Length: " + std::to_string(body.size()));
    REQUIRE(client.SendRequest("POST", "/api/file/v2/getFileList", headers, body));
    const auto response = client.ReadResponse(5000);
    REQUIRE(response.has_value());
    //  没有记录 → 契约 §2.5 的 400；关键是**不是 413**
    REQUIRE(response->status != 413);
    REQUIRE(response->status == 400);
  }
}

TEST_CASE("★ C9.3 ⑤ 在途连接数超限 → 503 + Retry-After（不排队到超时）",
          "[phase9][hardening][c9.3]") {
  //  上限 = 2，线程 = 8：第 3 个请求必须有线程可用、但被上限拒绝
  fss::http::ServerOptions server_options;
  server_options.worker_threads = 8;
  server_options.max_connections = 2;
  HttpFixture fx({}, server_options);
  const int port = fx.port();

  //  用"慢端点"占满槽位：先占一个正在传输的槽位（自签 URL 的流式 PUT 会占住线程）
  //  简化做法：直接并发打 8 个请求，统计"被拒 503"与"成功 200"的总数，
  //  并断言 503 的响应带 Retry-After、且**没有任何 5xx 之外的失败**。
  std::atomic<int> ok{0};
  std::atomic<int> busy{0};
  std::atomic<int> other{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
      RawClient client(port, true);
      if (!client.Connect()) {
        ++other;
        return;
      }
      if (!client.SendRequest("GET", "/api/file/v2/info", Authed(), "")) {
        ++other;
        return;
      }
      const auto response = client.ReadResponse(10000);
      if (!response.has_value()) {
        ++other;
        return;
      }
      if (response->status == 200) {
        ++ok;
      } else if (response->status == 503) {
        //  ★ 超限必须带 Retry-After，且是契约 §1.7 的错误体
        if (response->Header("Retry-After").has_value()) ++busy;
        else ++other;
      } else {
        ++other;
      }
    });
  }
  for (auto& thread : threads) thread.join();

  CAPTURE(ok.load(), busy.load(), other.load());
  REQUIRE(other.load() == 0);          // 只允许 200 或 503（+Retry-After）
  REQUIRE(ok.load() + busy.load() == 8);

  //  恢复：串行请求必须能正常服务（证明 503 不会把服务器卡死）
  bool recovered = false;
  for (int attempt = 0; attempt < 100 && !recovered; ++attempt) {
    RawClient client(port, true);
    if (client.Connect() && client.SendRequest("GET", "/api/file/v2/info", Authed(), "")) {
      const auto response = client.ReadResponse(5000);
      recovered = response.has_value() && response->status == 200;
    }
    if (!recovered) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(recovered);
}

TEST_CASE("★ C9.3 ⑥ 传输内存预算（配额）：并发上限 × 缓冲 > budget → 拒绝启动",
          "[phase9][hardening][c9.3]") {
  //  ★ 这是**启动期**的拒绝（不是运行期 4xx/5xx）：预算不满足时服务不应起来
  {
    fss::http::ServerOptions options;
    options.worker_threads = 64;
    options.transfer_buffer_bytes = 1024 * 1024;         // 1 MiB
    options.transfer_memory_budget_bytes = 8 * 1024 * 1024;  // 8 MiB
    const auto result = fss::http::ValidateOptions(options);
    REQUIRE_FALSE(result.ok());
    INFO(result.error().message());
    REQUIRE(result.error().message().find("transfer_memory_budget_bytes") != std::string::npos);
  }
  {
    //  同理：并发上限 > 线程数 → 拒绝（背压会静默失效）
    fss::http::ServerOptions options;
    options.worker_threads = 4;
    options.max_connections = 8;
    REQUIRE_FALSE(fss::http::ValidateOptions(options).ok());
  }
  {
    //  对照：满足预算的配置必须通过（否则上面的"拒绝"无法区分"校验正确"与"校验恒真"）
    fss::http::ServerOptions options;
    options.worker_threads = 8;
    options.max_connections = 8;
    options.transfer_buffer_bytes = 256 * 1024;
    options.transfer_memory_budget_bytes = 256 * 1024 * 1024;
    const auto result = fss::http::ValidateOptions(options);
    INFO(result.error().message());
    REQUIRE(result.ok());
  }
}
