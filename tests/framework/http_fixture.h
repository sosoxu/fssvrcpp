// =============================================================================
//  tests/framework/http_fixture.h —— "真实回环端口 + 真实自签 URL" 的完整装配
// =============================================================================
//  C4.1（19 个端点）、C4.2（黄金样例）、C4.3（负向矩阵）、C4.5（三种错误体）、
//  C4.7（expiryTime 数值）、C4.8（端到端字节）都要求**真实 HTTP 端口**，而且
//  §2.1/§2.4 返回的 `SignedURL`/`SignedUrl` 必须是**真能取到字节**的自签 URL。
//
//  ★ 为什么放进 framework 而不是各测试各写一份：
//    装配里每一处（base path、自签 codec、数据面回调、线程数/连接上限）都是判据的
//    前置条件。抄 N 份的结果是"各测试测的其实不是同一个服务"，而且装配一改就有
//    一份悄悄漂移（P4-D04/D05 的根因正是这类"两处约定不一致"）。
//
//  ⚠️ 生命周期（P4-D02）：`Router` 与任何被注册进 `Server` 的回调都必须比 `Server`
//     活得久 → 这里两者都是**成员**，且声明顺序为 router 在前、server 在后。
#pragma once

#include "app_fixture.h"
#include "mock_s3.h"
#include "raw_http.h"
#include "temp_dir.h"

#include "adapters/http/router.h"
#include "app/services/location_issuer.h"
#include "app/usecases/usecases.h"
#include "common/http/http.h"
#include "common/ids/id_generator.h"
#include "common/logging/logging.h"
#include "common/time/clock.h"
#include "infra/blob/memory/memory_blob_store.h"
#include "infra/location/memory/memory_location_repository.h"
#include "infra/metadata/memory/memory_metadata_repository.h"
#include "infra/blob/posix/posix_blob_store.h"
#include "infra/blob/s3/s3_blob_store.h"
#include "infra/location/sqlite/sqlite_location_repository.h"
#include "infra/transfer/blob_byte_source.h"
#include "infra/transfer/transfer_endpoint.h"
#include "infra/transfer/transfer_token.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fss::test {

//  单租户、单 store 的演示工厂（语义与组合根的 `SingleStoreFactory` 一致：
//  staging 与 persistent 落在同一个物理 store 上）
class SingleStoreFactory final : public fss::domain::IBlobStoreFactory {
 public:
  explicit SingleStoreFactory(fss::domain::IBlobStore& store) : store_(&store) {}
  fss::Result<fss::domain::IBlobStore*> ForPartition(std::string_view, fss::domain::StorageZone) override {
    return store_;
  }

 private:
  fss::domain::IBlobStore* store_;
};

// -----------------------------------------------------------------------------
//  共享装配：数据面回调 → Router → Server → 注册（两个 fixture 只差适配器）
// -----------------------------------------------------------------------------
//  ★ 抽成一个函数而不是抄两份：装配顺序（Router 必须先于 Server 构造，见 P4-D02）
//    和"连接上限 = 线程数"（C1.12）都是判据的前置条件，抄一份就多一处漂移点。
template <typename Fixture>
void WireRouterAndServer(Fixture& self, fss::adapters::http::RouterOptions options,
                         fss::http::ServerOptions server_options = {}) {
  fss::adapters::http::TransferCallbacks transfers;
  transfers.put = [&self](std::string_view token, std::string_view expires,
                          std::string_view signature, std::string_view partition,
                          fss::bytes::ByteSource& body) -> fss::Result<void> {
    fss::infra::TransferRequest request{std::string(token), std::string(expires),
                                        std::string(signature), "put", std::string(partition)};
    return self.endpoint.Put(request, body);
  };
  transfers.open_get = [&self](std::string_view token, std::string_view expires,
                               std::string_view signature, std::string_view partition)
      -> fss::Result<std::shared_ptr<fss::bytes::ByteSource>> {
    fss::infra::TransferRequest request{std::string(token), std::string(expires),
                                        std::string(signature), "get", std::string(partition)};
    FSS_TRY(resolved, self.endpoint.Resolve(request, "get"));
    FSS_TRY(stat, self.endpoint.Stat(request));
    FSS_TRY(store, self.factory.ForPartition(resolved.partition, resolved.zone));
    fss::domain::ObjectRef ref;
    ref.container = resolved.container;
    ref.key = resolved.object_key;
    return std::static_pointer_cast<fss::bytes::ByteSource>(
        std::make_shared<fss::infra::BlobByteSource>(*store, std::move(ref), stat.size));
  };

  self.router =
      std::make_unique<fss::adapters::http::Router>(*self.ports, transfers, std::move(options));

  server_options.bind_address = "127.0.0.1";
  server_options.port = 0;  // 系统分配
  if (server_options.worker_threads == 0) server_options.worker_threads = 4;
  //  C1.12：连接上限不得大于工作线程数，否则超限请求会先排队、背压失效
  if (server_options.max_connections == 0) server_options.max_connections = 4;
  self.server =
      std::make_unique<fss::http::Server>(server_options, self.logger, self.clock);
  self.router->Register(*self.server);
  REQUIRE(self.server->Bind());
  REQUIRE(self.server->Start());
}

// -----------------------------------------------------------------------------
//  HttpFixture —— 内存适配器（快、无 IO；绝大多数契约用例用它）
// -----------------------------------------------------------------------------
struct HttpFixture {
  fss::ManualClock clock{1700000000};
  fss::SequentialIdGenerator ids{1};
  fss::infra::InMemoryBlobStore blob{clock};
  SingleStoreFactory factory{blob};
  fss::infra::HmacTransferTokenCodec codec{"e2e-secret", clock};
  fss::infra::TransferEndpoint endpoint{codec, factory};
  FakePartitionRegistry partitions;
  AllowAllAuthorizer authorizer;
  RecordingEventPublisher events;
  RecordingAuditLogger audit;
  NoopLegalValidator legal;
  NoopSchemaValidator schema;
  fss::infra::InMemoryLocationRepository locations;
  fss::infra::InMemoryMetadataRepository metadata{clock};
  std::unique_ptr<fss::app::LocationIssuer> issuer;
  std::unique_ptr<fss::app::UseCasePorts> ports;
  fss::logging::MemoryLogger logger;
  fss::adapters::http::RouterOptions router_options;
  std::unique_ptr<fss::adapters::http::Router> router;
  std::unique_ptr<fss::http::Server> server;

  //  `server_options` 只给需要非默认超时/并发参数的用例（C4.11 用它把空闲超时压到秒级）
  explicit HttpFixture(fss::adapters::http::RouterOptions options = {},
                       fss::http::ServerOptions server_options = {}) {
    //  ★ 自签 URL 的 base 必须与路由 base path 一致，否则发出去的地址是 404（P4-D04）
    issuer = std::make_unique<fss::app::LocationIssuer>(
        factory, locations, codec, clock, ids,
        "http://127.0.0.1" + std::string(fss::adapters::http::kDefaultBasePath));
    ports = std::make_unique<fss::app::UseCasePorts>(fss::app::UseCasePorts{
        factory, locations, metadata, authorizer, events, audit, partitions, legal, schema,
        *issuer, clock, ids});
    router_options = options;
    WireRouterAndServer(*this, std::move(options), server_options);
  }
  ~HttpFixture() {
    if (server) server->Stop();
  }
  HttpFixture(const HttpFixture&) = delete;
  HttpFixture& operator=(const HttpFixture&) = delete;

  int port() const { return server->port(); }
  const fss::adapters::http::HttpMetrics& metrics() const { return router->metrics(); }
};

// -----------------------------------------------------------------------------
//  PosixStackFixture —— **真实栈**：POSIX 存储 + SQLite 位置仓储 + 真实端口
// -----------------------------------------------------------------------------
//  为什么必须在门槛里跑一套"真实栈"：
//    `HttpFixture` 用的是内存适配器，而内存实现"永远记得住 `extra`"。第 5 分钟的长跑
//    实测（scripts/verify_transfer_no_timeout.sh）暴露了 SQLite 仓储在 JSON 往返里
//    把 `extra.container`/`extra.object_key` 当"已知键"吞掉 → `CreateFileMetadata`
//    报 500（P4-D09）。这套 fixture 就是那条路径的**常驻回归**：
//    上传 → 元数据（staging→persistent 复制）→ 下载，全程真实文件系统 + 真实 SQLite。
struct PosixStackFixture {
  fss::test::TempDir dir{"posix_stack"};
  fss::ManualClock clock{1700000000};
  fss::SequentialIdGenerator ids{1};
  fss::infra::PosixBlobStore blob;
  SingleStoreFactory factory;
  fss::infra::HmacTransferTokenCodec codec{"real-stack-secret", clock};
  fss::infra::TransferEndpoint endpoint{codec, factory};
  std::unique_ptr<fss::infra::SqliteLocationRepository> locations;
  fss::infra::InMemoryMetadataRepository metadata{clock};
  FakePartitionRegistry partitions;
  AllowAllAuthorizer authorizer;
  RecordingEventPublisher events;
  RecordingAuditLogger audit;
  NoopLegalValidator legal;
  NoopSchemaValidator schema;
  std::unique_ptr<fss::app::LocationIssuer> issuer;
  std::unique_ptr<fss::app::UseCasePorts> ports;
  fss::logging::MemoryLogger logger;
  std::unique_ptr<fss::adapters::http::Router> router;
  std::unique_ptr<fss::http::Server> server;

  explicit PosixStackFixture(fss::adapters::http::RouterOptions options = {})
      : blob(dir.child("blobs"), clock), factory(blob) {
    auto opened = fss::infra::SqliteLocationRepository::Open(dir.child("location.db"));
    REQUIRE(opened.ok());
    locations = std::move(opened.value());
    issuer = std::make_unique<fss::app::LocationIssuer>(
        factory, *locations, codec, clock, ids,
        "http://127.0.0.1" + std::string(fss::adapters::http::kDefaultBasePath));
    ports = std::make_unique<fss::app::UseCasePorts>(fss::app::UseCasePorts{
        factory, *locations, metadata, authorizer, events, audit, partitions, legal, schema,
        *issuer, clock, ids});
    WireRouterAndServer(*this, std::move(options));
  }
  ~PosixStackFixture() {
    if (server) server->Stop();
  }
  PosixStackFixture(const PosixStackFixture&) = delete;
  PosixStackFixture& operator=(const PosixStackFixture&) = delete;

  int port() const { return server->port(); }
};

// -----------------------------------------------------------------------------
//  S3StackFixture —— **对象存储模式**：S3BlobStore（指向 mock-S3）+ 真实端口
// -----------------------------------------------------------------------------
//  与 `PosixStackFixture` 的差别只有"装哪个驱动"：这正是 C5.9 要证明的东西
//  （同一套上层代码、同一份测试，换驱动只改装配）。
//  ★ C5.8 的关键断言在这套 fixture 上做：`uploadURL` 返回的 `SignedURL` 必须指向
//    **存储端点**（mock-S3），而不是本服务的 `/v1/transfer` —— 服务不代理字节。
struct S3StackFixture {
  fss::test::MockS3 mock;
  //  ★ 必须是**系统时钟**：预签名 URL 带真实的 `X-Amz-Date`，mock 按真实时钟判过期。
  //    写死 2023 年的假时间在当前容器（宿主时钟 2026 年）会被 mock 正确地拒绝 ——
  //    那会把环境差异伪装成签名缺陷（P5-D03 的邻居）。
  fss::SystemClock clock;
  fss::SequentialIdGenerator ids{1};
  fss::infra::S3BlobStore blob;
  SingleStoreFactory factory;
  //  自签数据面在 S3 模式下**不会被用到**（原生预签名让客户端直连存储），但
  //  `WireRouterAndServer` 需要这两个成员才能装配；真实组合根在 S3 模式下不注册该路由。
  fss::infra::HmacTransferTokenCodec codec{"unused-in-s3-mode", clock};
  fss::infra::TransferEndpoint endpoint{codec, factory};
  fss::infra::InMemoryLocationRepository locations;
  fss::infra::InMemoryMetadataRepository metadata{clock};
  FakePartitionRegistry partitions;
  AllowAllAuthorizer authorizer;
  RecordingEventPublisher events;
  RecordingAuditLogger audit;
  NoopLegalValidator legal;
  NoopSchemaValidator schema;
  std::unique_ptr<fss::app::LocationIssuer> issuer;
  std::unique_ptr<fss::app::UseCasePorts> ports;
  fss::logging::MemoryLogger logger;
  std::unique_ptr<fss::adapters::http::Router> router;
  std::unique_ptr<fss::http::Server> server;

  explicit S3StackFixture(fss::adapters::http::RouterOptions options = {})
      : blob(MakeOptions(mock), clock), factory(blob) {
    issuer = std::make_unique<fss::app::LocationIssuer>(
        factory, locations, codec, clock, ids,
        "http://127.0.0.1" + std::string(fss::adapters::http::kDefaultBasePath));
    ports = std::make_unique<fss::app::UseCasePorts>(fss::app::UseCasePorts{
        factory, locations, metadata, authorizer, events, audit, partitions, legal, schema,
        *issuer, clock, ids});
    WireRouterAndServer(*this, std::move(options));
  }
  ~S3StackFixture() {
    if (server) server->Stop();
  }
  S3StackFixture(const S3StackFixture&) = delete;
  S3StackFixture& operator=(const S3StackFixture&) = delete;

  static fss::infra::S3Options MakeOptions(const fss::test::MockS3& mock) {
    fss::infra::S3Options s3;
    s3.endpoint = mock.endpoint();
    s3.region = mock.region();
    s3.force_path_style = true;
    s3.verify_tls = false;  // mock 是明文 HTTP
    s3.scheme = "http";
    s3.credentials = fss::infra::AwsCredentials{mock.access_key(), mock.secret_key(), ""};
    return s3;
  }

  int port() const { return server->port(); }
  fss::infra::S3BlobStore& store() { return blob; }
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

//  发一个请求。★ `RawClient` 故意不替调用方补 `Content-Length`（它的价值是能构造
//  畸形请求），因此**带体的方法一律补长度**：0 字节 PUT 若不补，服务端按流式等体、
//  客户端等响应 → 死锁到超时（0 字节端到端用例踩到过）。
inline Reply HttpDo(int port, const std::string& method, const std::string& target,
                    const std::vector<std::string>& headers = {}, const std::string& body = {}) {
  CAPTURE(method);
  CAPTURE(target);
  fss::test::RawClient client(port, /*tcp_nodelay=*/true);
  REQUIRE(client.Connect());
  std::vector<std::string> all = headers;
  const bool method_has_body = method == "PUT" || method == "POST" || method == "PATCH";
  if (!body.empty() || method_has_body) {
    all.push_back("Content-Length: " + std::to_string(body.size()));
  }
  REQUIRE(client.SendRequest(method, target, all, body));
  const auto response = client.ReadResponse(30000);
  REQUIRE(response.has_value());
  INFO("HTTP " << method << " " << target << " → " << response->status
               << " (body " << response->body.size() << " bytes)");
  return Reply{response->status, response->body, response->headers};
}

inline std::vector<std::string> Authed(const std::string& partition = "opendes") {
  return {"authorization: Bearer test-token", "data-partition-id: " + partition};
}

//  把自签 URL 拆成 path+query（RawClient 只接受 origin-form 的 target）
inline std::string TargetOf(const std::string& url) {
  const auto scheme = url.find("://");
  const auto path_start = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
  REQUIRE(path_start != std::string::npos);
  return url.substr(path_start);
}

}  // namespace fss::test
