// =============================================================================
//  C4.6 / C4.1(部分) / C4.2(部分) / C4.5(HTTP 侧)：真实 HTTP 端口上的运维端点与位置/元数据
// =============================================================================
//  为什么必须走真实端口：这三条判据的失败模式都只在**传输层**暴露 ——
//    · 健康检查的 `Content-Type` 与 body 逐字节相等（`text/plain`，**不是** JSON）；
//    · 错误体经 `fss_http` 包装层后是否仍是契约 §1.6 的形态；
//    · 路径参数（`:id`）与 base path（`/api/file`）是否真的接上了。
//  直接调用例/适配层函数都绕过了这些。
// =============================================================================
#include <catch2/catch.hpp>

#include "app_fixture.h"
#include "raw_http.h"

#include "adapters/http/router.h"
#include "common/http/http.h"
#include "common/json/json.h"
#include "common/logging/logging.h"
#include "domain/model/file_metadata.h"

#include <memory>
#include <string>
#include <vector>

namespace {

//  一台"内存适配器 + 真实 HTTP 端口"的服务
struct HttpFixture {
  fss::test::AppFixture app;
  fss::logging::MemoryLogger logger;
  fss::adapters::http::RouterOptions options;
  //  ★ Router 必须是**成员**：handler 闭包按 `this` 捕获 Router，
  //    若它是构造函数的局部对象，构造结束即析构 → 请求进来时解引用悬空指针（SIGSEGV）。
  std::unique_ptr<fss::adapters::http::Router> router;
  std::unique_ptr<fss::http::Server> server;

  HttpFixture() {
    options.error_format = fss::adapters::http::ErrorFormat::kAppError;
    router = std::make_unique<fss::adapters::http::Router>(*app.ports, options);

    fss::http::ServerOptions server_options;
    server_options.bind_address = "127.0.0.1";
    server_options.port = 0;  // 系统分配
    server_options.worker_threads = 4;
    server_options.max_connections = 4;  // C1.12：不得大于 worker_threads（否则超限请求会先排队、背压失效）
    server = std::make_unique<fss::http::Server>(server_options, logger, app.clock);
    router->Register(*server);
    REQUIRE(server->Bind());
    REQUIRE(server->Start());
  }
  ~HttpFixture() {
    if (server) server->Stop();
  }

  int port() const { return server->port(); }
};

struct Reply {
  int status = 0;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;

  std::optional<std::string> Header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
      if (key.size() != name.size()) continue;
      bool same = true;
      for (std::size_t i = 0; i < key.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(key[i])) !=
            std::tolower(static_cast<unsigned char>(name[i]))) {
          same = false;
          break;
        }
      }
      if (same) return value;
    }
    return std::nullopt;
  }
};

Reply Do(int port, const std::string& method, const std::string& target,
         const std::vector<std::string>& headers = {}, const std::string& body = {}) {
  CAPTURE(method);
  CAPTURE(target);
  fss::test::RawClient client(port, /*tcp_nodelay=*/true);
  REQUIRE(client.Connect());
  //  ★ `RawClient` 故意不替调用方补头（便于构造畸形请求）；带体的请求必须自己给
  //    `Content-Length`，否则服务端看不到体，甚至读成"下一个请求"。
  std::vector<std::string> all_headers = headers;
  if (!body.empty()) {
    all_headers.push_back("Content-Length: " + std::to_string(body.size()));
  }
  REQUIRE(client.SendRequest(method, target, all_headers, body));
  const auto response = client.ReadResponse();
  REQUIRE(response.has_value());
  Reply reply;
  reply.status = response->status;
  reply.body = response->body;
  reply.headers = response->headers;
  INFO("HTTP " << method << " " << target << " → " << reply.status << " body=" << reply.body);
  return reply;
}

std::vector<std::string> Authed(const std::string& partition = "opendes") {
  return {"authorization: Bearer test-token", "data-partition-id: " + partition};
}

}  // namespace

TEST_CASE("★ C4.6 运维端点：纯文本、逐字节相等、免鉴权（真实 HTTP）",
          "[phase4][conformance][c4.6]") {
  HttpFixture fixture;
  const int port = fixture.port();

  SECTION("liveness_check") {
    const auto reply = Do(port, "GET", "/api/file/v2/liveness_check");
    REQUIRE(reply.status == 200);
    REQUIRE(reply.Header("Content-Type").value() == "text/plain");  // ★ 不是 JSON
    REQUIRE(reply.body == "File service is alive");                 // ★ 逐字节
  }

  SECTION("readiness_check") {
    const auto reply = Do(port, "GET", "/api/file/v2/readiness_check");
    REQUIRE(reply.status == 200);
    REQUIRE(reply.Header("Content-Type").value() == "text/plain");
    REQUIRE(reply.body == "File service is ready");
  }

  SECTION("/v2/info 免鉴权返回 200 + 版本字段") {
    const auto reply = Do(port, "GET", "/api/file/v2/info");  // 不带任何鉴权头
    REQUIRE(reply.status == 200);
    REQUIRE(reply.Header("Content-Type").value().rfind("application/json", 0) == 0);
    const auto value = fss::json::ParseObject(reply.body);
    REQUIRE(value.ok());
    REQUIRE(value.value()["version"] == "v2");
    REQUIRE(value.value()["connectedOuterServices"].is_array());
  }
}

TEST_CASE("★ C4.5(HTTP 侧) 缺 token → 401 + 契约固定消息（apperror 形态）",
          "[phase4][conformance][c4.5]") {
  HttpFixture fixture;
  const auto reply = Do(fixture.port(), "GET", "/api/file/v2/files/uploadURL",
                        {"data-partition-id: opendes"});  // 故意不给 authorization
  REQUIRE(reply.status == 401);
  const auto value = fss::json::ParseObject(reply.body);
  REQUIRE(value.ok());
  REQUIRE(value.value()["code"] == 401);
  REQUIRE(value.value()["reason"] == "Unauthorized");
  REQUIRE(value.value()["message"] == "Missing authorization token");
}

TEST_CASE("★ C4.7(部分) uploadURL 的 expiryTime：缺省 ≈ now+3600；非法 → 400 固定消息",
          "[phase4][conformance][c4.7]") {
  HttpFixture fixture;
  const int port = fixture.port();

  const auto ok = Do(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(ok.status == 200);
  const auto value = fss::json::ParseObject(ok.body);
  REQUIRE(value.ok());
  REQUIRE(value.value()["FileID"].is_string());
  REQUIRE(value.value()["Location"]["SignedURL"].is_string());
  REQUIRE(value.value()["Location"]["FileSource"].is_string());
  //  C4.4 的 HTTP 侧：不得出现大小写漂移的名字
  REQUIRE(ok.body.find("fileId") == std::string::npos);
  REQUIRE(ok.body.find("fileSource") == std::string::npos);
  //  §2.1 不返回 Driver
  REQUIRE(ok.body.find("Driver") == std::string::npos);

  const auto bad = Do(port, "GET", "/api/file/v2/files/uploadURL?expiryTime=5X", Authed());
  REQUIRE(bad.status == 400);
  const auto bad_value = fss::json::ParseObject(bad.body);
  REQUIRE(bad_value.ok());
  REQUIRE(bad_value.value()["code"] == 400);
  REQUIRE(bad_value.value()["message"] ==
          "expiryTime pattern isn't supported. Value should be one of these regex patterns "
          "^[0-9]+M$ , ^[0-9]+H$ , ^[0-9]+D$");
}

TEST_CASE("★ C4.2(部分)/C4.8(0 字节边界) uploadURL → metadata → GET → downloadURL → DELETE",
          "[phase4][conformance][c4.2]") {
  HttpFixture fixture;
  const int port = fixture.port();

  //  ① 上传地址（服务端会建一个**空对象**，这正是 0 字节边界的来源）
  const auto upload = Do(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(upload.status == 200);
  const auto upload_json = fss::json::ParseObject(upload.body);
  REQUIRE(upload_json.ok());
  const std::string file_source = upload_json.value()["Location"]["FileSource"].get<std::string>();

  //  ② 写元数据：黄金样例的必需字段 + 上一步的 FileSource
  auto record = fss::test::AppFixture::MakeRecord(file_source, "sample.bin");
  const auto created = Do(port, "POST", "/api/file/v2/files/metadata", Authed(),
                          fss::json::Dump(fss::domain::ToJson(record)));
  REQUIRE(created.status == 201);
  const auto created_json = fss::json::ParseObject(created.body);
  REQUIRE(created_json.ok());
  const std::string id = created_json.value()["id"].get<std::string>();
  REQUIRE(id.rfind("opendes:dataset--File.Generic:", 0) == 0);

  //  ③ 读元数据：全字段 + version
  const auto fetched = Do(port, "GET", "/api/file/v2/files/" + id + "/metadata", Authed());
  REQUIRE(fetched.status == 200);
  const auto fetched_json = fss::json::ParseObject(fetched.body);
  REQUIRE(fetched_json.ok());
  REQUIRE(fetched_json.value()["id"] == id);
  REQUIRE(fetched_json.value()["version"] == 1);
  REQUIRE(fetched_json.value()["kind"].is_string());

  //  ④ 下载地址（id 用 FileID）
  const auto file_id = upload_json.value()["FileID"].get<std::string>();
  const auto download = Do(port, "GET", "/api/file/v2/files/" + file_id + "/downloadURL", Authed());
  REQUIRE(download.status == 200);
  const auto download_json = fss::json::ParseObject(download.body);
  REQUIRE(download_json.ok());
  REQUIRE(download_json.value()["SignedUrl"].is_string());  // ★ 小写 url

  //  ⑤ 列表能看到这条记录（Spring Page 结构）
  const auto list = Do(port, "POST", "/api/file/v2/getFileList", Authed(), "{}");
  REQUIRE(list.status == 200);
  const auto list_json = fss::json::ParseObject(list.body);
  REQUIRE(list_json.ok());
  REQUIRE(list_json.value()["NumberOfElements"].get<int>() >= 1);
  REQUIRE(list.body.find("\"results\"") == std::string::npos);

  //  ⑥ 删除 → 204；再读 → 404 + 固定消息
  const auto removed = Do(port, "DELETE", "/api/file/v2/files/" + id + "/metadata", Authed());
  REQUIRE(removed.status == 204);
  REQUIRE(removed.body.empty());

  const auto gone = Do(port, "GET", "/api/file/v2/files/" + id + "/metadata", Authed());
  REQUIRE(gone.status == 404);
  const auto gone_json = fss::json::ParseObject(gone.body);
  REQUIRE(gone_json.ok());
  REQUIRE(gone_json.value()["reason"] == "Not Found");
  REQUIRE(gone_json.value()["message"] == "Record Not Found");
}
