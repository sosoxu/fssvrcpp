// =============================================================================
//  P9 硬化：数据面（流式路由）的**错误响应语义** —— P9-D05 / P9-D07 的回归
// =============================================================================
//  背景（真实的缺陷，不是假想）
//  ---------------------------------------------------------------------------
//  流式路由（`stream_body`：数据面 PUT/GET）的包装层在 handler 返回之后还要收尾读体：
//      pump->Stop();  req.body_stream.reset();
//      HandleBodyError(..., reader_ok->load(), ...)
//  `pump->Stop()` 之后 httplib 的 reader 会返回 false（"消费者不再取了"），于是
//  `reader_ok == false`。第一版实现**无条件**用
//      `400 failed to read request body`
//  覆盖 handler 的响应 —— 只要 handler 在读干请求体**之前**返回，它真正的状态码就消失了。
//
//  实测现象：向 `/api/file/v1/transfer/<token>` 发一个带 body 的 PUT，token 签名不合法 →
//  handler 立刻返回 401，客户端收到的却是
//      {"code":400,"detail":"failed to read request body"}
//  排障方向被从"鉴权/存储"引到"客户端网络"，而真实原因（签名、存储故障、配额…）
//  在响应里**完全不可见**（P9-D05）。
//
//  修法（`src/common/http/server.cpp` Dispatch 的收尾块）：只有两种情况允许覆盖
//    ① handler 报了 **2xx**：此时"体没读完却回成功"是**静默截断**，必须纠正；
//    ② 读体错误是**确定性**的（空闲/整体超时、超限、声明长度不符）：那是精确诊断，
//       比 handler 的次生错误更接近根因（例如客户端声明 100 只发 50 → handler 因为
//       读到半个对象而报 500，我们回 400 + mismatch 更准确）。
//  其余情况保留 handler 的响应（note 记为 `handler_error_kept_over_body_read_failure`）。
//
//  第三副面孔（P9-D07）：`BodyPump::Push` **只要队列没满就返回 true**，所以
//  "handler 一个字节都没读、字节进了队列随后随 `Stop()` 被丢弃"时 httplib 的
//  `reader_ok` 仍是 true —— 上面那条分支根本不触发，而 handler 报了 2xx。
//  这就是"必须读完才算数"类缺陷（同 P7-D07 / P8-D05），必须以**服务端错误**收场。
//  修法：`BodyPump` 记 `consumed()`（真正交给 handler 的字节），2xx + `consumed != read` → 500。
//
//  ★ 为什么这里用**裸 Server + 自定路由**而不是真实数据面
//  ---------------------------------------------------------------------------
//  这两个判据都要求"handler 故意不读/提前返回"，真实 handler（`TransferEndpoint::Put`
//  → 驱动 `put`）永远会把流读干。用一个`echo` 式的最小路由可以把语义钉死，
//  不依赖某个驱动的实现细节；真实数据面的对应回归在
//  `test_transfer_error_semantics.cpp` 的另一条用例（篡改签名 → 401）。
//
//  ★ 为什么"声明长度 vs 分块"两种请求体都要有
//    `body_error_is_conclusive` 里的"声明长度不符"分支只在有 `Content-Length` 时才可能成立，
//    所以用**分块**体才能**确定性地**只考验"保留 handler 错误"这一条（P9-D05 的核心），
//    而带 `Content-Length` 的用例检验的是"静默截断必须被改成读体错误"。
// =============================================================================
#include <catch2/catch.hpp>

#include "common/http/http.h"
#include "common/json/json.h"
#include "common/logging/logging.h"
#include "common/time/clock.h"
#include "http_fixture.h"
#include "raw_http.h"

#include <atomic>
#include <cstdint>
#include <sys/socket.h>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace {

using fss::test::RawClient;

//  一个只开"流式上传"路由的裸服务器。handler 的行为由回调给定。
struct StreamServer {
  fss::SystemClock clock;
  fss::logging::MemoryLogger logger;
  std::unique_ptr<fss::http::Server> server;
  std::atomic<int> handler_calls{0};

  explicit StreamServer(std::function<fss::http::Response(fss::http::Request&)> handler) {
    fss::http::ServerOptions options;
    options.bind_address = "127.0.0.1";
    options.port = 0;
    options.worker_threads = 4;
    options.max_connections = 4;
    options.tcp_nodelay = true;
    //  ★ 队列必须**大于**用例的请求体：这样泵线程能把整段体一次收进队列
    //    （`Push` 在队列满时会阻塞），"已从客户端读入多少"因此不受 handler 读取速度影响。
    options.transfer_buffer_bytes = 64 * 1024;
    server = std::make_unique<fss::http::Server>(options, logger, clock);
    fss::http::RouteOptions route;
    route.name = "stream_upload";
    route.max_body_bytes = 0;  // 不限：本用例测的不是上限
    route.stream_body = true;
    server->Post("/stream", route, [this, handler = std::move(handler)](fss::http::Request& req) {
      ++handler_calls;
      return handler(req);
    });
    REQUIRE(server->Bind());
    REQUIRE(server->Start());
  }
  ~StreamServer() {
    if (server) server->Stop();
  }
  StreamServer(const StreamServer&) = delete;
  StreamServer& operator=(const StreamServer&) = delete;

  int port() const { return server->port(); }
};

//  把整个请求体读干的 handler（正例对照用）
fss::http::Response DrainBody(fss::http::Request& req) {
  std::string buffer(4096, '\0');
  std::uint64_t total = 0;
  for (;;) {
    auto n = req.body_stream->Read(buffer.data(), buffer.size());
    if (!n.ok()) return fss::http::Response::Text(400, "stream error");
    if (n.value() == 0) break;
    total += n.value();
  }
  return fss::http::Response::Text(201, std::to_string(total));
}

}  // namespace

TEST_CASE("★ P9-D05 数据面：handler 提前报的错不得被“读体失败 400”覆盖（分块体）",
          "[phase9][hardening][p9-d05]") {
  //  handler 立刻报 409，**一个字节都不读**。分块体 → 不存在"声明长度不符"这条
  //  确定性分支 → 唯一正确的行为就是保留 409。
  StreamServer fx([](fss::http::Request&) { return fss::http::Response::Text(409, "conflict"); });

  RawClient client(fx.port(), /*tcp_nodelay=*/true);
  REQUIRE(client.Connect());
  REQUIRE(client.SendChunkedRequest("POST", "/stream", {"0123456789", "abcdefghij"}));

  const auto response = client.ReadResponse(10000);
  REQUIRE(response.has_value());
  INFO("响应 " << response->status << " " << response->body);
  REQUIRE(response->body.find("failed to read request body") == std::string::npos);
  REQUIRE(response->status == 409);
  REQUIRE(response->body.find("conflict") != std::string::npos);
}

TEST_CASE("★ P9-D07 数据面：handler 回 2xx 却没读干请求体 → 必须报服务端错误（不得静默截断）",
          "[phase9][hardening][p9-d07]") {
  //  ★ 这个 handler 只读**第一块**（16 字节），然后睡一会儿再回 201。
  //    睡眠是**判据的一部分**，不是随意的等待：
  //      · 泵线程是独立线程，会继续把请求体读进队列 —— 睡够之后
  //        "已从客户端读入的字节数"必然等于整段体（`read == total`）；
  //      · 而 handler 只消费了 16 字节 → `consumed != read` 是**确定**的，
  //        不依赖调度时序（这正是这条护栏存在的原因）。
  //  ★ 为什么用**分块**体：带 `Content-Length` 时，`consumed != read` 往往先被
  //    "声明长度不符"这条**确定性**分支命中（回 400 mismatch），用例就会"因为别的原因
  //    通过" —— 断言恒真的陷阱（R16）。分块体没有长度先验，只考验本护栏。
  StreamServer fx([](fss::http::Request& req) {
    char first[16] = {};
    REQUIRE(req.body_stream->Read(first, sizeof first).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return fss::http::Response::Text(201, "created");  // ★ 谎报成功：体没读完
  });

  const std::string first_chunk = "0123456789abcdef";   // 16 字节
  const std::string rest(4080, 'r');                    // 余下 4080 字节
  RawClient client(fx.port(), /*tcp_nodelay=*/true);
  REQUIRE(client.Connect());
  REQUIRE(client.SendChunkedRequest("POST", "/stream", {first_chunk, rest}));

  const auto response = client.ReadResponse(10000);
  REQUIRE(response.has_value());
  INFO("响应 " << response->status << " " << response->body);
  REQUIRE_FALSE((response->status >= 200 && response->status < 300));
  //  必须**正是**这条护栏（而不是"长度不符"那一条）：
  REQUIRE(response->body.find("without consuming") != std::string::npos);
}

TEST_CASE("★ P9-D07 正例对照：把请求体读干的 handler 仍然正常 201（护栏不得恒真）",
          "[phase9][hardening][p9-d07]") {
  StreamServer fx(&DrainBody);

  const std::string payload(4096, 'q');
  RawClient client(fx.port(), /*tcp_nodelay=*/true);
  REQUIRE(client.Connect());
  REQUIRE(client.SendRequest("POST", "/stream",
                             {"Content-Length: " + std::to_string(payload.size())}, payload));

  const auto response = client.ReadResponse(10000);
  REQUIRE(response.has_value());
  INFO("响应 " << response->status << " " << response->body);
  REQUIRE(response->status == 201);
  REQUIRE(response->body == std::to_string(payload.size()));  // 一个字节都没丢
}

TEST_CASE("★ P9-D05 真实数据面：篡改签名的 PUT 必须回 401（而不是“读体失败 400”）",
          "[phase9][hardening][p9-d05]") {
  //  为什么用**真实**装配（`PosixStackFixture`：真编解码器 + 真 POSIX 驱动）：
  //    内存替身 `RecordingSelfSignedCodec::Decode` **恒**返回 `kUnimplemented`，
  //    那样"handler 提前失败"是必然的，用例就无法区分"保留了真错误"与"放过所有响应"（R1/R16）。
  fss::test::PosixStackFixture fx;

  //  为什么用**分块**体：带 `Content-Length` 时，"handler 先返回、字节还没到"会让
  //    "声明长度不符"这条确定性分支命中（回 400），于是用例变成**看时序**的。
  //    分块体的长度先验未知 → 只考验"保留 handler 错误"这一条，判定是确定的。
  RawClient client(fx.port(), /*tcp_nodelay=*/true);
  REQUIRE(client.Connect());
  REQUIRE(client.SendChunkedRequest(
      "PUT", "/api/file/v1/transfer/tampered?op=put&exp=9999999999&sig=deadbeefdeadbeef",
      {"some-bytes-", "the-handler-never-reads"}, {"data-partition-id: opendes"}));

  const auto response = client.ReadResponse(10000);
  REQUIRE(response.has_value());
  INFO("响应 " << response->status << " " << response->body);
  REQUIRE(response->body.find("failed to read request body") == std::string::npos);
  REQUIRE(response->status == 401);  // 真正的根因：签名不合法
}

TEST_CASE("★ P9-D05 另一半：handler 报 2xx 但请求体读失败 → 状态码必须被纠正（不得静默成功）",
          "[phase9][hardening][p9-d05]") {
  //  这是"保留 handler 错误"这条放开的**反向约束**：放开的只有"handler 已报错"的情况，
  //  handler 报成功时读体失败**必须**纠正 —— 否则就是"客户端以为存好了、字节却少了"。
  //  构造：分块体**不发**结束块就半关连接（`SHUT_WR`）→ httplib 的 content reader 返回 false，
  //  而 `content_length < 0` 使"声明长度不符"那条确定性分支不可能命中 ——
  //  唯一能纠正它的就是"2xx + 读体失败"这条规则。
  //  handler 故意**忽略**读错误并回 201（模拟"没检查返回值"的实现缺陷）。
  //
  //  ★ 判据为什么落在**访问日志**而不是 HTTP 响应上（实测结论，不是偷懒）：
  //    httplib 在 content reader 返回 false 之后会**中止连接**，所以这个 400 根本发不出去；
  //    客户端看到的是"连接被关掉"（它因此也不会把这次上传当成成功）。
  //    但服务端**必须**在访问日志里记 400 + `body_read_failed` —— 那才是运维读到的真相；
  //    若这条规则失效，日志里会留下一个骗人的 `status: 201`。
  std::atomic<int> read_errors{0};
  StreamServer fx([&read_errors](fss::http::Request& req) {
    char buffer[1024];
    for (;;) {
      auto n = req.body_stream->Read(buffer, sizeof buffer);
      if (!n.ok()) {
        ++read_errors;
        break;
      }
      if (n.value() == 0) break;
    }
    return fss::http::Response::Text(201, "created");  // ★ 谎报成功
  });

  RawClient client(fx.port(), /*tcp_nodelay=*/true);
  REQUIRE(client.Connect());
  //  一个**不完整**的分块体：有块、没有结束块
  REQUIRE(client.Send("POST /stream HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
                      "10\r\n0123456789abcdef\r\n"));
  REQUIRE(::shutdown(client.fd(), SHUT_WR) == 0);  // 半关：服务端读到 EOF
  (void)client.ReadResponse(5000);                 // 响应可能因为连接被中止而读不到（见上）

  //  前提断言（R9）：handler 真的被调用了、真的读到了错误，否则这条用例什么也没证明
  REQUIRE(fx.handler_calls.load() > 0);
  REQUIRE(read_errors.load() > 0);

  bool found = false;
  for (const auto& rec : fx.logger.records()) {
    if (rec.value("path", std::string()) != "/stream") continue;
    found = true;
    INFO("访问日志 " << fss::json::Dump(rec));
    REQUIRE(rec.value("status", 0) == 400);                     // ★ 不是 201
    REQUIRE(rec.value("note", std::string()) == "body_read_failed");
  }
  REQUIRE(found);
}
