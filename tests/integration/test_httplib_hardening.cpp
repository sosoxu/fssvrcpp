// =============================================================================
//  C1.2：H-1 / H-2 回归 + HTTP 层边界
// =============================================================================
//  这是本阶段最关键的一道防线（ADR-002 §4）：两个缺陷都是**已实测复现**的，
//  不是假设。所有断言都在**线缆层**观察（原始 socket），不经过任何客户端库。
//
//  H-1（0.10.3）：越界 Range → 下溢的 `Content-Length`（2^64 量级）
//  H-2（0.26.0）：流式请求体超限 → handler 被调用、交出 0 字节、最终 **201**
//                 → "上传成功但文件为空"的静默数据丢失
//
//  每个"必须被拒绝"的断言都配了"handler 无副作用"的检查：handler 会往一个
//  原子计数里记一笔，测试断言它在拒绝场景下**没有**被执行。
#include <catch2/catch.hpp>

#include "common/http/http.h"
#include "common/logging/logging.h"
#include "common/time/clock.h"
#include "framework/raw_http.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using fss::http::Method;
using fss::http::Response;
using fss::http::RouteOptions;
using fss::http::Server;
using fss::http::ServerOptions;
namespace raw = fss::test;

namespace {

// 一个可观测的测试服务端：handler 被调用会留下痕迹（副作用计数）
struct TestServer {
  fss::logging::LogOptions log_options;
  fss::logging::MemoryLogger logger{log_options};
  fss::ManualClock clock{1700000000};
  ServerOptions options;
  std::unique_ptr<Server> server;

  // 副作用痕迹
  std::atomic<int> handler_calls{0};
  std::atomic<std::uint64_t> body_bytes_seen{0};

  std::string payload = "0123456789abcdefghijklmnopqrstuvwxyz";  // 36 字节

  void Start(std::int64_t max_body = 1024, bool stream_body = false) {
    options.bind_address = "127.0.0.1";
    options.port = 0;
    options.worker_threads = 16;
    options.max_connections = 16;
    options.keep_alive_max_count = 1000000;
    options.tcp_nodelay = true;
    server = std::make_unique<Server>(options, logger, clock);

    server->Get("/small", RouteOptions{.name = "small"},
                [this](fss::http::Request&) {
                  ++handler_calls;
                  return Response::Text(200, "hello");
                });

    // 流式响应（H-1 的路径：Range 作用在 set_content_provider 上）
    server->Get("/blob",
                RouteOptions{.name = "blob"},
                [this](fss::http::Request&) {
                  ++handler_calls;
                  return Response::Stream(
                      200, "application/octet-stream",
                      std::make_shared<fss::bytes::StringSource>(payload),
                      static_cast<std::int64_t>(payload.size()));
                });

    server->Get("/norange",
                RouteOptions{.name = "norange"},
                [this](fss::http::Request&) {
                  ++handler_calls;
                  // 非寻址来源 → 多段 Range 应被降级为 200
                  return Response::Stream(200, "application/octet-stream",
                                          std::make_shared<OneShotSource>(payload),
                                          static_cast<std::int64_t>(payload.size()));
                });

    RouteOptions upload;
    upload.name = "upload";
    upload.max_body_bytes = max_body;
    upload.stream_body = stream_body;
    // 路由级 body 上限：超限必须 413（有 CL）/ 400（chunked），且 handler 不被调用
    server->Post("/upload", upload, [this](fss::http::Request& req) {
      ++handler_calls;
      if (req.body_stream) {
        std::string chunk(4096, '\0');
        while (true) {
          auto n = req.body_stream->Read(chunk.data(), chunk.size());
          if (!n.ok()) return Response::Text(400, "stream error");
          if (n.value() == 0) break;
          body_bytes_seen += n.value();
        }
      } else {
        body_bytes_seen += req.body.size();
      }
      return Response::Text(201, "created");
    });

    // 捕获"读体失败"的路由（用于对照：不吞错误时必须 400）
    server->Put("/upload_swallow", upload, [](fss::http::Request& req) {
      // ★ 故意吞掉读体错误并返回 201 —— 包装层必须把它纠正为 400
      if (req.body_stream) {
        std::string chunk(4096, '\0');
        while (true) {
          auto n = req.body_stream->Read(chunk.data(), chunk.size());
          if (!n.ok() || n.value() == 0) break;
        }
      }
      return Response::Text(201, "created");
    });

    REQUIRE(server->Start());
    REQUIRE(server->port() > 0);
  }

  // 一个只支持顺序读、不支持 seek 的来源（用于多段 Range 降级断言）
  class OneShotSource final : public fss::bytes::ByteSource {
   public:
    explicit OneShotSource(std::string data) : data_(std::move(data)) {}
    fss::Result<std::size_t> Read(char* out, std::size_t capacity) override {
      const auto n = std::min(capacity, data_.size() - pos_);
      std::memcpy(out, data_.data() + pos_, n);
      pos_ += n;
      return n;
    }
    std::optional<std::int64_t> Size() const override {
      return static_cast<std::int64_t>(data_.size());
    }
    // Seekable() 默认 false；Seek 默认返回 kUnimplemented

   private:
    std::string data_;
    std::size_t pos_ = 0;
  };

  int port() const { return server->port(); }
};

}  // namespace

TEST_CASE("★ H-1 回归：越界 Range 必须 416，且不得出现下溢的 Content-Length",
          "[phase1][http][c1.2]") {
  TestServer ts;
  ts.Start();

  SECTION("★ 原始 H-1 复现头（first > last）→ 按 RFC 忽略该头，但**绝不下溢**") {
    // H-1 的原始复现输入就是 `bytes=999999999-8388607`（探针 raw_range.cpp）。
    // 注意：`last < first` 在 RFC 7233 §2.1 里属于**语法非法** → 应当忽略该头返回 200 全量，
    // 而不是 416（416 只留给"语法合法但不可满足"）。0.26.0 在这条输入上给的是 416，
    // 那是它的解析器把非法语法当成了不可满足 —— 包装层按契约 §1.8 纠正为 200。
    // **本用例真正要守住的不变量是：Content-Length 不得下溢。**
    raw::RawClient c(ts.port());
    REQUIRE(c.Connect());
    REQUIRE(c.SendRequest("GET", "/blob", {"Range: bytes=999999999-8388607"}));
    auto res = c.ReadResponse();
    REQUIRE(res.has_value());
    INFO(res->raw_head);
    REQUIRE(res->status == 200);
    REQUIRE(res->Header("Content-Length").value() == std::to_string(ts.payload.size()));
    REQUIRE(res->body == ts.payload);
    // ★ 下溢特征（2^64 量级）绝不能出现
    REQUIRE(res->raw_head.find("1844674407") == std::string::npos);
  }

  SECTION("语法合法但越界（bytes=999999999-）→ 416 + bytes */total") {
    raw::RawClient c(ts.port());
    REQUIRE(c.Connect());
    REQUIRE(c.SendRequest("GET", "/blob", {"Range: bytes=999999999-"}));
    auto res = c.ReadResponse();
    REQUIRE(res.has_value());
    INFO(res->raw_head);
    REQUIRE(res->status == 416);
    const auto cl = res->Header("Content-Length");
    REQUIRE(cl.has_value());
    REQUIRE(std::strtoull(cl->c_str(), nullptr, 10) < 1000);  // 错误体很小，重点是**没有下溢**
    REQUIRE(res->raw_head.find("1844674407") == std::string::npos);
    REQUIRE(res->Header("Content-Range").value() == "bytes */36");
  }

  SECTION("bytes=36-（起点恰好等于总长）→ 416") {
    raw::RawClient c(ts.port());
    REQUIRE(c.Connect());
    REQUIRE(c.SendRequest("GET", "/blob", {"Range: bytes=36-"}));
    auto res = c.ReadResponse();
    REQUIRE(res.has_value());
    REQUIRE(res->status == 416);
  }

  SECTION("★ 对照：合法 Range 必须真的给出 206（否则上面的 416 证明不了什么）") {
    raw::RawClient c(ts.port());
    REQUIRE(c.Connect());
    REQUIRE(c.SendRequest("GET", "/blob", {"Range: bytes=0-4"}));
    auto res = c.ReadResponse();
    REQUIRE(res.has_value());
    INFO(res->raw_head);
    REQUIRE(res->status == 206);
    REQUIRE(res->body == "01234");
    REQUIRE(res->Header("Content-Range").value() == "bytes 0-4/36");
    REQUIRE(res->Header("Content-Length").value() == "5");
  }
}

TEST_CASE("★ Range 全边界：0-0 / -1 / len-1- / len- / 多段 / 语法非法", "[phase1][http][c1.2]") {
  TestServer ts;
  ts.Start();
  const std::string body = "0123456789abcdefghijklmnopqrstuvwxyz";  // 36 字节

  struct Case {
    const char* range;
    int status;
    std::string body;
    const char* content_range;
  };
  const std::vector<Case> cases = {
      {"bytes=0-0", 206, "0", "bytes 0-0/36"},
      {"bytes=-1", 206, "z", "bytes 35-35/36"},
      {"bytes=35-", 206, "z", "bytes 35-35/36"},
      {"bytes=30-35", 206, "uvwxyz", "bytes 30-35/36"},
      {"bytes=30-999", 206, "uvwxyz", "bytes 30-35/36"},   // 末尾截断
      {"bytes=-999", 206, body, "bytes 0-35/36"},          // 后缀 ≥ 总长
      {"bytes=36-", 416, "<json:416>", "bytes */36"},
      {"bytes=-0", 416, "<json:416>", "bytes */36"},
      // 语法非法 → 忽略该头，返回 200 全量（**不是** 416）
      {"bytes=abc-def", 200, body, nullptr},
      {"bytes=10-5", 200, body, nullptr},
      {"items=0-1", 200, body, nullptr},
  };

  for (const auto& c : cases) {
    raw::RawClient client(ts.port());
    REQUIRE(client.Connect());
    REQUIRE(client.SendRequest("GET", "/blob", {std::string("Range: ") + c.range}, {}, true));
    auto res = client.ReadResponse();
    INFO("Range: " << c.range << "  head: " << (res ? res->raw_head : std::string("<none>")));
    REQUIRE(res.has_value());
    REQUIRE(res->status == c.status);
    if (std::string(c.body) == "<json:416>") {
      // 416 也必须走契约 §1.6 的错误体（不是空体）
      REQUIRE(res->body.find("\"code\":416") != std::string::npos);
    } else {
      REQUIRE(res->body == c.body);
    }
    if (c.content_range != nullptr) {
      REQUIRE(res->Header("Content-Range").value() == c.content_range);
    }
  }
}

TEST_CASE("多段 Range：可寻址来源给 multipart，不可寻址来源降级为 200", "[phase1][http][c1.2]") {
  TestServer ts;
  ts.Start();
  const std::string body = "0123456789abcdefghijklmnopqrstuvwxyz";

  SECTION("可寻址（StringSource）→ 206 multipart/byteranges") {
    raw::RawClient c(ts.port());
    REQUIRE(c.Connect());
    REQUIRE(c.SendRequest("GET", "/blob", {"Range: bytes=0-1,34-35"}, {}, true));
    auto res = c.ReadResponse();
    REQUIRE(res.has_value());
    INFO(res->raw_head);
    REQUIRE(res->status == 206);
    const auto ct = res->Header("Content-Type");
    REQUIRE(ct.has_value());
    REQUIRE(ct->find("multipart/byteranges") != std::string::npos);
    REQUIRE(res->body.find("01") != std::string::npos);
    REQUIRE(res->body.find("yz") != std::string::npos);
  }

  SECTION("★ 不可寻址 → 忽略 Range 返回全量（宁可不满足可选特性，也不写坏 multipart）") {
    raw::RawClient c(ts.port());
    REQUIRE(c.Connect());
    REQUIRE(c.SendRequest("GET", "/norange", {"Range: bytes=0-1,34-35"}, {}, true));
    auto res = c.ReadResponse();
    REQUIRE(res.has_value());
    REQUIRE(res->status == 200);
    REQUIRE(res->body == body);
  }
}

TEST_CASE("★ H-2 回归：Content-Length 超限 → 413 且 handler 无副作用", "[phase1][http][c1.2]") {
  TestServer ts;
  ts.Start(/*max_body=*/100);

  const std::string big(1000, 'x');
  raw::RawClient c(ts.port());
  REQUIRE(c.Connect());
  REQUIRE(c.SendRequest("POST", "/upload",
                       {"Content-Length: " + std::to_string(big.size()),
                        "Content-Type: text/plain"},
                       big));
  auto res = c.ReadResponse();
  REQUIRE(res.has_value());
  INFO(res->raw_head);
  REQUIRE(res->status == 413);
  // ★ handler 必须**没有被调用**（没有任何字节落盘/落库）
  REQUIRE(ts.handler_calls.load() == 0);
  REQUIRE(ts.body_bytes_seen.load() == 0);
  REQUIRE(ts.server->stats().rejected_too_large == 1);

  SECTION("对照：刚好等于上限 → 正常处理（201）") {
    TestServer ok_ts;
    ok_ts.Start(/*max_body=*/100);
    raw::RawClient c2(ok_ts.port());
    REQUIRE(c2.Connect());
    const std::string exact(100, 'y');
    REQUIRE(c2.SendRequest("POST", "/upload",
                           {"Content-Length: " + std::to_string(exact.size())}, exact));
    auto ok = c2.ReadResponse();
    REQUIRE(ok.has_value());
    REQUIRE(ok->status == 201);
    REQUIRE(ok_ts.handler_calls.load() == 1);
    REQUIRE(ok_ts.body_bytes_seen.load() == 100);
  }
}

TEST_CASE("★ H-2 回归：chunked 超限 → 400 且 handler 无副作用", "[phase1][http][c1.2]") {
  TestServer ts;
  ts.Start(/*max_body=*/100, /*stream_body=*/true);

  std::vector<std::string> chunks;
  for (int i = 0; i < 10; ++i) chunks.push_back(std::string(50, 'c'));  // 共 500 字节

  raw::RawClient c(ts.port());
  REQUIRE(c.Connect());
  REQUIRE(c.SendChunkedRequest("POST", "/upload", chunks));
  auto res = c.ReadResponse();
  REQUIRE(res.has_value());
  INFO(res->raw_head);
  // ★ chunked 超限的契约行为是 400（库的读失败语义），不是 413
  REQUIRE(res->status == 400);
  REQUIRE(ts.body_bytes_seen.load() <= 100);  // 读到上限附近就中止
  REQUIRE(ts.server->stats().rejected_too_large >= 1);
}

TEST_CASE("★ H-2 回归：handler 吞掉读体错误返回 201 → 包装层强制改回 400",
          "[phase1][http][c1.2]") {
  // 这条是"静默数据丢失"的最后一道防线：即使业务代码写错了（把读失败当读完），
  // 也不能让客户端收到 201 —— 那意味着"上传成功但文件为空"。
  TestServer ts;
  ts.Start(/*max_body=*/100, /*stream_body=*/true);

  std::vector<std::string> chunks;
  for (int i = 0; i < 10; ++i) chunks.push_back(std::string(50, 'd'));

  raw::RawClient c(ts.port());
  REQUIRE(c.Connect());
  REQUIRE(c.SendChunkedRequest("PUT", "/upload_swallow", chunks));
  auto res = c.ReadResponse();
  REQUIRE(res.has_value());
  INFO(res->raw_head);
  REQUIRE(res->status == 400);  // 不是 201
}

TEST_CASE("★ H-2 防护③：Content-Length 与实际读取不一致 → 400（防静默截断）",
          "[phase1][http][c1.2]") {
  TestServer ts;
  ts.Start(/*max_body=*/10000);

  SECTION("声明 100 实际只发 50 后关闭连接") {
    raw::RawClient c(ts.port());
    REQUIRE(c.Connect());
    const std::string partial(50, 'p');
    // 故意声明 100 但只发 50，然后关闭写端（shutdown）
    REQUIRE(c.SendRequest("POST", "/upload", {"Content-Length: 100"}, partial));
    ::shutdown(c.fd(), SHUT_WR);
    auto res = c.ReadResponse(3000);
    // 服务器要么直接 400（读到提前结束），要么关连接；两者都可接受，但**不能是 201**
    if (res.has_value()) {
      INFO(res->raw_head);
      REQUIRE(res->status == 400);
    }
    REQUIRE(ts.server->stats().body_length_mismatch + ts.server->stats().body_limit_aborted >= 1);
  }

  SECTION("对照：声明 50 实发 50 → 201") {
    TestServer ok_ts;
    ok_ts.Start(/*max_body=*/10000);
    raw::RawClient c2(ok_ts.port());
    REQUIRE(c2.Connect());
    const std::string exact(50, 'q');
    REQUIRE(c2.SendRequest("POST", "/upload", {"Content-Length: 50"}, exact, true));
    auto res = c2.ReadResponse();
    REQUIRE(res.has_value());
    REQUIRE(res->status == 201);
  }
}

TEST_CASE("★ 协议级畸形请求：重复 CL / CL+chunked / 超长头 / 超长 URI", "[phase1][http][c1.2]") {
  TestServer ts;
  ts.Start();

  SECTION("重复 Content-Length → 400（请求走私防护）") {
    raw::RawClient c(ts.port());
    REQUIRE(c.Connect());
    const std::string body = "hello";
    REQUIRE(c.SendRequest("POST", "/upload", {"Content-Length: 5", "Content-Length: 5"}, body));
    auto res = c.ReadResponse(3000);
    REQUIRE(res.has_value());
    INFO(res->raw_head);
    REQUIRE(res->status == 400);
    REQUIRE(ts.handler_calls.load() == 0);
  }

  SECTION("Content-Length 与 chunked 并存 → 400") {
    raw::RawClient c(ts.port());
    REQUIRE(c.Connect());
    REQUIRE(c.SendChunkedRequest("POST", "/upload", {"data"},
                                 {"Content-Length: 4"}));
    auto res = c.ReadResponse(3000);
    REQUIRE(res.has_value());
    INFO(res->raw_head);
    REQUIRE(res->status == 400);
    REQUIRE(ts.handler_calls.load() == 0);
  }

  SECTION("单个请求头 > 8192 字节 → 400") {
    raw::RawClient c(ts.port());
    REQUIRE(c.Connect());
    const std::string huge = "X-Big: " + std::string(20000, 'b');
    REQUIRE(c.SendRequest("GET", "/small", {huge}));
    auto res = c.ReadResponse(3000);
    REQUIRE(res.has_value());
    INFO(res->raw_head);
    REQUIRE(res->status == 400);
  }

  SECTION("★ 请求行超限：实测是**连接被关闭且无响应**（不是 400/414）") {
    // 实测定界（见 docs/test-evidence/phase1.md）：target ≤ 8191 正常处理；
    // ≥ 8192 时库在解析请求行阶段直接关闭连接，**不写任何响应**。
    // ADR-002 §4 原先按编译期常量写成"400/414"，这里按实测更正并写进契约 §1.7。
    raw::RawClient c(ts.port());
    REQUIRE(c.Connect());
    const std::string too_long = "/" + std::string(8192, 'a');
    REQUIRE(c.SendRequest("GET", too_long));
    auto res = c.ReadResponse(2000);
    REQUIRE_FALSE(res.has_value());  // 无响应 = 连接被关闭
  }

  SECTION("★ 对照：8191 字节的 URI 必须被正常处理（否则上面的'无响应'说明不了界限）") {
    TestServer long_ts;
    long_ts.Start();
    long_ts.server->Get("/static/*", fss::http::RouteOptions{.name = "static"},
                        [](fss::http::Request& req) {
                          return Response::Text(200, std::to_string(req.Param("*")->size()));
                        });
    raw::RawClient c(long_ts.port());
    REQUIRE(c.Connect());
    const std::string long_target = "/static/" + std::string(8183, 'a');  // 总长 8191
    REQUIRE(long_target.size() == 8191);
    REQUIRE(c.SendRequest("GET", long_target, {}, {}, true));
    auto res = c.ReadResponse(3000);
    REQUIRE(res.has_value());
    INFO(res->raw_head);
    REQUIRE(res->status == 200);
  }

  SECTION("★ 对照：正常大小的头与 URI 必须成功（否则上面的拒绝没有意义）") {
    raw::RawClient c(ts.port());
    REQUIRE(c.Connect());
    REQUIRE(c.SendRequest("GET", "/small", {"X-Normal: " + std::string(100, 'c')}, {}, true));
    auto res = c.ReadResponse();
    REQUIRE(res.has_value());
    REQUIRE(res->status == 200);
    REQUIRE(res->body == "hello");
  }
}

TEST_CASE("方法不匹配 → 404（ADR-002 §4 的实测行为，契约 §1.7）", "[phase1][http][c1.2]") {
  TestServer ts;
  ts.Start();

  std::vector<std::pair<const char*, const char*>> cases = {
      {"PUT", "/small"},
      {"DELETE", "/small"},
      {"POST", "/blob"},
  };
  for (const auto& [method, path] : cases) {
    raw::RawClient c(ts.port());
    REQUIRE(c.Connect());
    REQUIRE(c.SendRequest(method, path, {"Content-Length: 0"}, {}, true));
    auto res = c.ReadResponse();
    INFO("method " << method << " " << path);
    REQUIRE(res.has_value());
    REQUIRE(res->status == 404);  // ★ 不是 405
  }
}

TEST_CASE("错误体形态：库自身的 400 也必须是我们生成的 JSON（契约 §1.6）",
          "[phase1][http][c1.2]") {
  TestServer ts;
  ts.Start();

  raw::RawClient c(ts.port());
  REQUIRE(c.Connect());
  REQUIRE(c.SendRequest("POST", "/upload", {"Content-Length: 4", "Content-Length: 4"}, "abcd"));
  auto res = c.ReadResponse(3000);
  REQUIRE(res.has_value());
  REQUIRE(res->status == 400);
  const auto ct = res->Header("Content-Type");
  REQUIRE(ct.has_value());
  REQUIRE(ct->find("application/json") != std::string::npos);
  // 必须是 JSON（能被解析），且带 code 字段
  REQUIRE(res->body.find("\"code\":400") != std::string::npos);
  REQUIRE(res->body.find("<html") == std::string::npos);
}

TEST_CASE("路由与中间件：命名段参数、HEAD、correlation-id 透传", "[phase1][http][c1.2]") {
  TestServer ts;
  ts.Start();

  // 追加一条带命名段的路由
  ts.server->Get("/files/:id/metadata", fss::http::RouteOptions{.name = "meta"},
                 [](fss::http::Request& req) {
                   const auto id = req.Param("id").value_or("<missing>");
                   return Response::Json(200, "{\"id\":\"" + id + "\"}");
                 });

  SECTION("命名段") {
    raw::RawClient c(ts.port());
    REQUIRE(c.Connect());
    REQUIRE(c.SendRequest("GET", "/files/abc-123/metadata", {}, {}, true));
    auto res = c.ReadResponse();
    REQUIRE(res.has_value());
    REQUIRE(res->status == 200);
    REQUIRE(res->body == "{\"id\":\"abc-123\"}");
  }

  SECTION("HEAD：无响应体，但有 Content-Length（可探测长度）") {
    raw::RawClient c(ts.port());
    REQUIRE(c.Connect());
    REQUIRE(c.SendRequest("HEAD", "/small", {}, {}, true));
    auto res = c.ReadResponse();
    REQUIRE(res.has_value());
    INFO(res->raw_head);
    REQUIRE(res->status == 200);
    REQUIRE(res->body.empty());
    REQUIRE(res->Header("Content-Length").value() == "5");
  }

  SECTION("correlation-id：入站提供了就透传，没提供就生成") {
    {
      raw::RawClient c(ts.port());
      REQUIRE(c.Connect());
      REQUIRE(c.SendRequest("GET", "/small", {"x-correlation-id: cid-from-client"}, {}, true));
      auto res = c.ReadResponse();
      REQUIRE(res.has_value());
      REQUIRE(res->Header("x-correlation-id").value() == "cid-from-client");
    }
    {
      raw::RawClient c(ts.port());
      REQUIRE(c.Connect());
      REQUIRE(c.SendRequest("GET", "/small", {}, {}, true));
      auto res = c.ReadResponse();
      REQUIRE(res.has_value());
      const auto cid = res->Header("x-correlation-id");
      REQUIRE(cid.has_value());
      REQUIRE(cid->size() == 36);  // UUID
    }
  }
}

TEST_CASE("访问日志：每条请求一条 JSON 记录，且 correlation-id 在其中", "[phase1][http][c1.2]") {
  TestServer ts;
  ts.Start();
  raw::RawClient c(ts.port());
  REQUIRE(c.Connect());
  REQUIRE(c.SendRequest("GET", "/small", {"x-correlation-id: cid-log"}, {}, true));
  REQUIRE(c.ReadResponse().has_value());

  const auto records = ts.logger.records();
  REQUIRE_FALSE(records.empty());
  bool found = false;
  for (const auto& rec : records) {
    if (rec.value("correlation_id", std::string()) == "cid-log") {
      found = true;
      REQUIRE(rec.value("method", std::string()) == "GET");
      REQUIRE(rec.value("path", std::string()) == "/small");
      REQUIRE(rec.value("status", 0) == 200);
      REQUIRE(rec.contains("duration_ms"));
    }
  }
  REQUIRE(found);
}

TEST_CASE("选项校验：并发上限不得大于线程数、传输内存预算", "[phase1][http][c1.13]") {
  fss::logging::MemoryLogger logger{};
  fss::ManualClock clock{1700000000};

  SECTION("max_connections > worker_threads → 拒绝（否则 503 会先排队，背压失效）") {
    ServerOptions o;
    o.worker_threads = 4;
    o.max_connections = 8;
    const auto r = fss::http::ValidateOptions(o);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().message().find("不得大于") != std::string::npos);
  }

  SECTION("线程数 × 缓冲 > 传输内存预算 → 拒绝") {
    ServerOptions o;
    o.worker_threads = 64;
    o.transfer_buffer_bytes = 1024 * 1024;
    o.transfer_memory_budget_bytes = 8 * 1024 * 1024;
    o.max_connections = 0;
    const auto r = fss::http::ValidateOptions(o);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().message().find("超过 transfer_memory_budget_bytes") != std::string::npos);
  }

  SECTION("默认线程数按公式推导，且受内存预算约束") {
    ServerOptions o;
    o.transfer_buffer_bytes = 1024 * 1024;
    o.transfer_memory_budget_bytes = 8 * 1024 * 1024;
    REQUIRE(fss::http::DefaultWorkerThreads(o) == 8);  // 64 被预算压到 8
    o.transfer_memory_budget_bytes = 1024LL * 1024 * 1024;
    REQUIRE(fss::http::DefaultWorkerThreads(o) >= 16);  // 无预算压力时按硬件并发放大
  }

  SECTION("错误格式取值非法 → 拒绝") {
    ServerOptions o;
    o.error_format = "nonsense";
    REQUIRE_FALSE(fss::http::ValidateOptions(o).ok());
  }

  SECTION("对照：合法选项必须通过") {
    ServerOptions o;
    o.worker_threads = 8;
    o.max_connections = 8;
    REQUIRE(fss::http::ValidateOptions(o).ok());
  }
}
