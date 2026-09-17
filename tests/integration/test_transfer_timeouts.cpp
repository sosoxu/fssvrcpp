// =============================================================================
//  C4.11：数据面**没有整体超时**，但"空闲无进展"必须被主动断开
// =============================================================================
//  为什么这两条要一起测：
//    · 只测"慢传输不被中断" → 一个把所有超时都关掉的实现也能通过，代价是
//      "客户端断了但连接不释放"会耗尽工作线程（C1.12 的背压随之失效）；
//    · 只测"空闲被断开" → 一个把整体超时设成 30s 的实现也能通过，代价是
//      TB 级传输永远传不完（契约 §1.7 明确不许对数据面做整体超时）。
//
//  ★ 规模：把 `transfer_idle_timeout_sec` / `read_timeout_sec` 压到 **2 秒**，
//    再用"每 300ms 送一块、持续 6 秒（= 3× 空闲超时）"的慢客户端证明**没有整体超时**。
//    ≥5 分钟的长时间版本见 `scripts/verify_transfer_no_timeout.sh`（不放进日常门槛，
//    因为 5 分钟会让门槛不可用；那条命令的输出记在阶段证据里）。
// =============================================================================
#include <catch2/catch.hpp>

#include "http_fixture.h"
#include "raw_http.h"

#include "common/json/json.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using fss::test::Authed;
using fss::test::HttpDo;
using fss::test::HttpFixture;
using fss::test::RawClient;
using fss::test::TargetOf;

constexpr int kIdleTimeoutSec = 2;

fss::http::ServerOptions FastTimeoutServerOptions() {
  fss::http::ServerOptions options;
  options.read_timeout_sec = kIdleTimeoutSec;
  options.transfer_idle_timeout_sec = kIdleTimeoutSec;
  options.json_request_timeout_sec = kIdleTimeoutSec;
  return options;
}

//  仅握手 + 建空对象，拿到上传地址（不发送请求体）
std::string UploadTarget(HttpFixture& fixture) {
  const auto upload = HttpDo(fixture.port(), "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(upload.status == 200);
  const auto value = fss::json::ParseObject(upload.body);
  REQUIRE(value.ok());
  return TargetOf(value.value()["Location"]["SignedURL"].get<std::string>());
}

}  // namespace

TEST_CASE("★ C4.11 数据面：慢而有进展的传输**不受整体超时**限制",
          "[phase4][integration][c4.11]") {
  HttpFixture fixture({}, FastTimeoutServerOptions());
  const std::string target = UploadTarget(fixture);

  //  32 KiB，分 20 块、每块间隔 300ms → 总时长约 6s = 3× 空闲超时
  const std::size_t chunk_size = 1638;
  const int chunks = 20;
  const std::size_t total = chunk_size * static_cast<std::size_t>(chunks);
  const std::string chunk(chunk_size, 'x');

  RawClient client(fixture.port(), /*tcp_nodelay=*/true);
  REQUIRE(client.Connect());
  REQUIRE(client.Send("PUT " + target + " HTTP/1.1\r\nHost: h\r\n" +
                      "authorization: Bearer test-token\r\ndata-partition-id: opendes\r\n" +
                      "Content-Length: " + std::to_string(total) + "\r\n\r\n"));

  const auto started = std::chrono::steady_clock::now();
  for (int i = 0; i < chunks; ++i) {
    REQUIRE(client.Send(chunk));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  INFO("总时长 " << elapsed << " ms（空闲超时 " << kIdleTimeoutSec << " s）");
  //  ★ 前提断言：这次传输**确实**超过了空闲超时；否则这条用例什么也没证明（R9）
  REQUIRE(elapsed > kIdleTimeoutSec * 1000);

  const auto response = client.ReadResponse(5000);
  REQUIRE(response.has_value());
  REQUIRE(response->status == 200);  // ★ 没有被整体超时打断
}

TEST_CASE("★ C4.11 数据面：空闲无进展超过空闲超时 → 被主动断开（不是 200）",
          "[phase4][integration][c4.11]") {
  HttpFixture fixture({}, FastTimeoutServerOptions());
  const std::string target = UploadTarget(fixture);

  RawClient client(fixture.port(), /*tcp_nodelay=*/true);
  REQUIRE(client.Connect());
  const std::string declared = std::to_string(64 * 1024);
  REQUIRE(client.Send("PUT " + target + " HTTP/1.1\r\nHost: h\r\n" +
                      "authorization: Bearer test-token\r\ndata-partition-id: opendes\r\n" +
                      "Content-Length: " + declared + "\r\n\r\n"));
  REQUIRE(client.Send(std::string(1024, 'y')));  // 只发 1 KiB，然后**停住**

  std::this_thread::sleep_for(std::chrono::seconds(kIdleTimeoutSec * 2 + 1));

  //  ★ 必须是 **408**（而不是笼统的 400）：如果空闲超时被报成
  //    `content-length mismatch` / `failed to read request body`，客户端与运维会去查
  //    客户端/网络，而真正的原因是超时配置。报文里要能看出是"无进展超时"。
  const auto response = client.ReadResponse(2000);
  REQUIRE(response.has_value());
  INFO("服务端响应 " << response->status << " " << response->body);
  REQUIRE(response->status == 408);
  REQUIRE(response->body.find("timed out") != std::string::npos);
}

TEST_CASE("★ C4.11 普通 JSON 路由仍有**整体**超时（数据面的豁免不外溢）",
          "[phase4][integration][c4.11]") {
  HttpFixture fixture({}, FastTimeoutServerOptions());

  RawClient client(fixture.port(), /*tcp_nodelay=*/true);
  REQUIRE(client.Connect());
  //  声明 1 KiB 的 JSON 体，但每 300ms 只送 64 字节：
  //  · 空闲检查（每块间隔 300ms < 2s）不会触发；
  //  · **整体**超时（2s）必须在总时长超过 2s 时触发 —— 这是数据面与 JSON 路由的差别
  REQUIRE(client.Send("POST /api/file/v2/files/metadata HTTP/1.1\r\nHost: h\r\n"
                      "authorization: Bearer test-token\r\ndata-partition-id: opendes\r\n"
                      "Content-Length: 1024\r\n\r\n"));
  const std::string piece(64, 'z');
  for (int i = 0; i < 12; ++i) {  // 12 × 300ms ≈ 3.6s > 2s
    if (!client.Send(piece)) break;  // 服务端提前断开也算"被超时打断"
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

 //  普通路由必须命中**整体**超时（数据面的豁免不得外溢）
  const auto response = client.ReadResponse(2000);
  REQUIRE(response.has_value());
  INFO("服务端响应 " << response->status << " " << response->body);
  REQUIRE(response->status == 408);
  REQUIRE(response->body.find("timed out") != std::string::npos);
}
