// =============================================================================
//  C1.3 / C1.11 / C1.12：真实回环端口上的集成测试
// =============================================================================
//  C1.3  keep-alive 复用、并发 ≥ 50、chunked 请求体、Range 各形态、
//        **≥ 1 GiB 流式收发的 RSS 峰值增长 < 64 MiB**（证明不整块驻留）
//  C1.11 TCP_NODELAY 生效性自证：默认开启；并**关闭**它作为对照 ——
//        此时 keep-alive 小请求必须复现 ~40 ms 的 delayed-ACK 停顿（否则该测试无效）
//  C1.12 并发上限可配置且可观测：超限 → 503 + Retry-After（不排队到超时）
#include <catch2/catch.hpp>

#include "common/http/http.h"
#include "common/logging/logging.h"
#include "common/time/clock.h"
#include "framework/raw_http.h"

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using fss::http::Response;
using fss::http::RouteOptions;
using fss::http::Server;
using fss::http::ServerOptions;
namespace raw = fss::test;

namespace {

constexpr std::int64_t kGiB = 1024LL * 1024 * 1024;

// 大文件测试的规模可通过环境变量缩小：ASan/UBSan 构建下 1 GiB 的流式用例会非常慢，
// 且 sanitizer 自身的影子内存会让 RSS 断言失去意义。默认仍是 1 GiB（C1.3 的要求）。
std::int64_t BigFileBytes() {
  if (const char* env = std::getenv("FSS_TEST_BIG_BYTES")) {
    const auto v = std::strtoll(env, nullptr, 10);
    if (v > 0) return v;
  }
  return kGiB;
}

// RSS 上限：正常构建断言 < 64 MiB；sanitizer 构建下影子内存/redzone 会抬高基线，
// 因此允许用 FSS_TEST_RSS_LIMIT_KIB 覆盖（默认不变 = 64 MiB）。
std::uint64_t RssLimitKib() {
  if (const char* env = std::getenv("FSS_TEST_RSS_LIMIT_KIB")) {
    const auto v = std::strtoull(env, nullptr, 10);
    if (v > 0) return v;
  }
  return 64 * 1024;
}

struct Fixture {
  fss::logging::MemoryLogger logger{};
  fss::ManualClock clock{1700000000};
  ServerOptions options;
  std::unique_ptr<Server> server;
  std::atomic<int> slow_started{0};
  std::int64_t big_bytes = kGiB;

  void Start(int workers = 16, int max_conn = 16, bool tcp_nodelay = true) {
    options.bind_address = "127.0.0.1";
    options.port = 0;
    options.worker_threads = workers;
    options.max_connections = max_conn;
    options.tcp_nodelay = tcp_nodelay;
    options.keep_alive_max_count = 1000000;
    options.transfer_buffer_bytes = 256 * 1024;
    options.transfer_memory_budget_bytes = 1024LL * 1024 * 1024;
    server = std::make_unique<Server>(options, logger, clock);

    server->Get("/small", RouteOptions{.name = "small"},
                [](fss::http::Request&) { return Response::Text(200, "hello"); });

    server->Get("/echo", RouteOptions{.name = "echo"},
                [](fss::http::Request& req) { return Response::Text(200, req.body); });

    // 大文件流式下载（1 GiB 虚拟内容；RepeatingSource 不驻留内存）
    big_bytes = BigFileBytes();
    server->Get("/big", RouteOptions{.name = "big"}, [this](fss::http::Request&) {
      return Response::Stream(200, "application/octet-stream",
                              std::make_shared<fss::bytes::RepeatingSource>(big_bytes),
                              big_bytes);
    });

    // 流式上传：只计数，不缓冲
    RouteOptions up;
    up.name = "upload_stream";
    up.max_body_bytes = 0;  // 不限
    up.stream_body = true;
    server->Post("/upload", up, [this](fss::http::Request& req) {
      std::string buf(64 * 1024, '\0');
      std::uint64_t total = 0;
      while (true) {
        auto n = req.body_stream->Read(buf.data(), buf.size());
        if (!n.ok()) return Response::Text(400, "stream error");
        if (n.value() == 0) break;
        total += n.value();
      }
      upload_bytes = total;
      return Response::Text(201, std::to_string(total));
    });

    // 阻塞式 handler（用于并发上限测试）
    server->Get("/slow", RouteOptions{.name = "slow"}, [this](fss::http::Request&) {
      ++slow_started;
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
      return Response::Text(200, "slow");
    });

    REQUIRE(server->Start());
  }

  std::atomic<std::uint64_t> upload_bytes{0};
  int port() const { return server->port(); }
};

double MillisSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

TEST_CASE("★ C1.3：keep-alive 复用（同一连接连续多次请求）", "[phase1][http][c1.3]") {
  Fixture fx;
  fx.Start();

  raw::RawClient c(fx.port());
  REQUIRE(c.Connect());
  for (int i = 0; i < 20; ++i) {
    REQUIRE(c.SendRequest("GET", "/small"));
    auto res = c.ReadResponse();
    REQUIRE(res.has_value());
    INFO("第 " << i << " 次请求: " << res->raw_head);
    REQUIRE(res->status == 200);
    REQUIRE(res->body == "hello");
    // 服务端不得主动关闭连接
    const auto conn = res->Header("Connection");
    if (conn.has_value()) REQUIRE(*conn != "close");
  }
}

TEST_CASE("★ C1.3：并发 ≥ 50（用阻塞 handler 证明**真的在同时处理**）",
          "[phase1][http][c1.3]") {
  Fixture fx;
  fx.Start(/*workers=*/64, /*max_conn=*/64);

  constexpr int kClients = 50;
  std::atomic<int> ok_count{0};
  std::vector<std::thread> threads;
  threads.reserve(kClients);
  const auto t_start = std::chrono::steady_clock::now();
  for (int i = 0; i < kClients; ++i) {
    threads.emplace_back([&] {
      raw::RawClient c(fx.port());
      if (!c.Connect()) return;
      if (!c.SendRequest("GET", "/slow")) return;   // handler 睡 400 ms
      auto res = c.ReadResponse(10000);
      if (res.has_value() && res->status == 200) ++ok_count;
    });
  }
  for (auto& t : threads) t.join();
  const auto elapsed_ms = MillisSince(t_start);
  INFO("50 个 400ms 请求总耗时 " << elapsed_ms << " ms（≈400ms 说明真的并发）");
  // ★ 回归：库默认 listen backlog=5，50 个并发连接会丢 SYN → 客户端 1s 后重传，
  //   总耗时实测 1436 ms（而非 ~400 ms）。包装层把 CPPHTTPLIB_LISTEN_BACKLOG 提到 512
  //   之后必须回到 ~400 ms 量级；这条断言就是那个设置的守卫。
  REQUIRE(elapsed_ms < 3 * 400.0);
  REQUIRE(fx.slow_started.load() == kClients);

  REQUIRE(ok_count.load() == kClients);
  // ★ 关键断言：50 个请求必须**同时在处理**（handler 阻塞 400 ms，因此必然重叠）
  const auto stats = fx.server->stats();
  INFO("peak_in_flight=" << stats.peak_in_flight << " requests_total=" << stats.requests_total
                          << " slow_started=" << fx.slow_started.load());
  REQUIRE(stats.peak_in_flight >= kClients);
  REQUIRE(stats.in_flight == 0);
}

TEST_CASE("★ C1.3：50 连接 × 4 次 keep-alive 请求全部成功", "[phase1][http][c1.3]") {
  Fixture fx;
  fx.Start(/*workers=*/64, /*max_conn=*/64);

  constexpr int kClients = 50;
  constexpr int kPerClient = 4;
  std::atomic<int> ok_count{0};
  std::atomic<int> bad_status{0};
  std::vector<std::thread> threads;
  threads.reserve(kClients);
  for (int i = 0; i < kClients; ++i) {
    threads.emplace_back([&] {
      raw::RawClient c(fx.port());
      if (!c.Connect()) return;
      for (int k = 0; k < kPerClient; ++k) {
        if (!c.SendRequest("GET", "/small")) return;
        auto res = c.ReadResponse(5000);
        if (!res.has_value()) return;
        if (res->status == 200 && res->body == "hello") ++ok_count;
        else ++bad_status;
      }
    });
  }
  for (auto& t : threads) t.join();

  INFO("成功 " << ok_count << " / " << kClients * kPerClient << "，异常状态 " << bad_status);
  REQUIRE(ok_count.load() == kClients * kPerClient);
  REQUIRE(bad_status.load() == 0);
  REQUIRE(fx.server->stats().requests_total == static_cast<std::uint64_t>(kClients * kPerClient));
}

TEST_CASE("★ C1.3：chunked 请求体被正确接收（内容与长度都对）", "[phase1][http][c1.3]") {
  Fixture fx;
  fx.Start();

  std::vector<std::string> chunks = {"hello ", "chunked ", "world"};
  raw::RawClient c(fx.port());
  REQUIRE(c.Connect());
  REQUIRE(c.SendChunkedRequest("POST", "/upload", chunks));
  auto res = c.ReadResponse();
  REQUIRE(res.has_value());
  INFO(res->raw_head);
  REQUIRE(res->status == 201);
  REQUIRE(fx.upload_bytes.load() == 19);
}

TEST_CASE("★ C1.3：1 GiB 虚拟文件流式下载期间 RSS 峰值增长 < 64 MiB",
          "[phase1][http][c1.3]") {
  Fixture fx;
  fx.Start(/*workers=*/8, /*max_conn=*/8);

  // 基线 RSS（同一进程内既有服务端也有本客户端 → 这条约束比"只看服务端"更严）
  const auto rss_before = raw::PeakRssKib();

  raw::RawClient c(fx.port());
  REQUIRE(c.Connect());
  REQUIRE(c.SendRequest("GET", "/big"));

  auto head = c.ReadHead(5000);
  REQUIRE(head.has_value());
  INFO(head->raw_head);
  REQUIRE(head->status == 200);
  REQUIRE(head->Header("Content-Length").value() == std::to_string(fx.big_bytes));
  REQUIRE(head->Header("Content-Range").has_value() == false);

  const auto received = c.DiscardBody(static_cast<std::uint64_t>(fx.big_bytes), 60000);
  REQUIRE(received == static_cast<std::uint64_t>(fx.big_bytes));

  const auto rss_after = raw::PeakRssKib();
  const auto growth_kib = rss_after > rss_before ? rss_after - rss_before : 0;
  INFO("RSS 峰值增长 " << growth_kib << " KiB（上限 " << RssLimitKib() << " KiB）");
  REQUIRE(growth_kib < RssLimitKib());
  c.Close();
}

TEST_CASE("★ C1.3：1 GiB 流式上传（chunked）期间 RSS 峰值增长 < 64 MiB",
          "[phase1][http][c1.3]") {
  Fixture fx;
  fx.Start(/*workers=*/8, /*max_conn=*/8);

  const auto rss_before = raw::PeakRssKib();

  raw::RawClient c(fx.port());
  REQUIRE(c.Connect());
  REQUIRE(c.Send("POST /upload HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"));

  // 1 GiB 分块发送（每次 256 KiB），中间不做任何缓冲
  const std::string chunk(256 * 1024, 'u');
  std::string framed;
  {
    char size_line[32];
    std::snprintf(size_line, sizeof size_line, "%zx\r\n", chunk.size());
    framed += size_line;
    framed += chunk;
    framed += "\r\n";
  }
  const auto big = static_cast<std::uint64_t>(fx.big_bytes);
  std::uint64_t sent = 0;
  while (sent < big) {
    // 每次发一整块（含分块头与尾），保证分块结构不被切断
    if (!c.Send(framed)) break;
    sent += chunk.size();
  }
  REQUIRE(sent == big);
  REQUIRE(c.Send("0\r\n\r\n"));
  auto res = c.ReadResponse(60000);
  REQUIRE(res.has_value());
  INFO(res->raw_head);
  REQUIRE(res->status == 201);
  REQUIRE(fx.upload_bytes.load() == big);

  const auto rss_after = raw::PeakRssKib();
  const auto growth_kib = rss_after > rss_before ? rss_after - rss_before : 0;
  INFO("RSS 峰值增长 " << growth_kib << " KiB（上限 " << RssLimitKib() << " KiB）");
  REQUIRE(growth_kib < RssLimitKib());
  c.Close();
}

TEST_CASE("★ C1.11：TCP_NODELAY 生效性自证（开启快 / 关闭必须复现 ~40 ms 停顿）",
          "[phase1][http][c1.11]") {
  // 为什么必须成对测：只断言"开启时很快"无法排除"这台机器上根本不会出现 40ms 停顿"。
  // 关闭对照复现出停顿，才说明这条测试真的在测 TCP_NODELAY（R1）。
  constexpr int kRequests = 25;

  const auto measure = [](bool tcp_nodelay) {
    Fixture fx;
    fx.Start(/*workers=*/8, /*max_conn=*/8, tcp_nodelay);
    raw::RawClient c(fx.port(), /*tcp_nodelay=*/false);  // 客户端也不设，保持客户端侧行为一致
    REQUIRE(c.Connect());
    double total_ms = 0;
    for (int i = 0; i < kRequests; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      c.SendRequest("GET", "/small");
      auto res = c.ReadResponse();
      REQUIRE(res.has_value());
      REQUIRE(res->status == 200);
      total_ms += MillisSince(t0);
    }
    c.Close();
    return total_ms / kRequests;
  };

  const double with_nodelay = measure(true);
  const double without_nodelay = measure(false);
  INFO("TCP_NODELAY=on  平均延迟 " << with_nodelay << " ms");
  INFO("TCP_NODELAY=off 平均延迟 " << without_nodelay << " ms");

  REQUIRE(with_nodelay < 5.0);          // 开启：亚毫秒级（本机实测 ~0.05 ms）
  REQUIRE(without_nodelay > 20.0);      // 关闭：必须复现 delayed-ACK 的 ~40 ms 特征
  REQUIRE(without_nodelay > with_nodelay * 10);  // 量级差异必须显著
}

TEST_CASE("★ C1.12：并发上限 → 超限立刻 503 + Retry-After（不排队到超时）",
          "[phase1][http][c1.12]") {
  Fixture fx;
  fx.Start(/*workers=*/8, /*max_conn=*/2);  // 上限 2，线程 8（保证第 3 个请求有线程可用）

  const auto stats0 = fx.server->stats();
  REQUIRE(stats0.worker_threads == 8);
  REQUIRE(stats0.max_connections == 2);

  // 先占满 2 个在途槽位
  std::vector<std::thread> holders;
  for (int i = 0; i < 2; ++i) {
    holders.emplace_back([&] {
      raw::RawClient c(fx.port());
      if (c.Connect()) {
        c.SendRequest("GET", "/slow");
        (void)c.ReadResponse(5000);
      }
    });
  }
  // 轮询等到 2 个请求真的在途（不用固定 sleep：AGENTS §4.3）
  for (int i = 0; i < 500 && fx.server->stats().in_flight < 2; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  REQUIRE(fx.server->stats().in_flight == 2);

  // 第 3 个请求：必须**立刻**收到 503，而不是等前两个结束
  raw::RawClient c3(fx.port());
  REQUIRE(c3.Connect());
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(c3.SendRequest("GET", "/slow"));
  auto res = c3.ReadResponse(2000);
  const auto latency = MillisSince(t0);
  REQUIRE(res.has_value());
  INFO(res->raw_head);
  INFO("503 延迟 " << latency << " ms");
  REQUIRE(res->status == 503);
  REQUIRE(res->Header("Retry-After").value() == "1");
  REQUIRE(res->body.find("\"code\":503") != std::string::npos);
  // ★ "不排队到超时"：必须显著快于 handler 的 400 ms
  REQUIRE(latency < 300.0);

  for (auto& t : holders) t.join();

  // ★ 在途计数必须**归还**（否则几次 503 之后服务器会永久 503）
  for (int i = 0; i < 500 && fx.server->stats().in_flight != 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  REQUIRE(fx.server->stats().in_flight == 0);
  REQUIRE(fx.server->stats().rejected_busy >= 1);
  REQUIRE(fx.server->stats().peak_in_flight >= 2);

  SECTION("恢复后必须能正常服务（证明 503 不会把服务器'卡死'）") {
    raw::RawClient c(fx.port());
    REQUIRE(c.Connect());
    REQUIRE(c.SendRequest("GET", "/small", {}, {}, true));
    auto ok = c.ReadResponse();
    REQUIRE(ok.has_value());
    REQUIRE(ok->status == 200);
  }
}

TEST_CASE("C1.12：并发上限内的请求全部成功（对照）", "[phase1][http][c1.12]") {
  Fixture fx;
  fx.Start(/*workers=*/8, /*max_conn=*/4);
  // 4 个并发（正好等于上限）都应成功
  std::atomic<int> ok{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&] {
      raw::RawClient c(fx.port());
      if (!c.Connect()) return;
      if (!c.SendRequest("GET", "/slow")) return;
      auto res = c.ReadResponse(5000);
      if (res.has_value() && res->status == 200) ++ok;
    });
  }
  for (auto& t : threads) t.join();
  REQUIRE(ok.load() == 4);
  REQUIRE(fx.server->stats().rejected_busy == 0);
}
