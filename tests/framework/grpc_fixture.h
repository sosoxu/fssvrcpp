// =============================================================================
//  grpc_fixture.h —— 真实 gRPC 端口 + 真实 channel 的测试装配
// =============================================================================
//  为什么必须"真实端口 + 真实 channel"：
//    C7.1 要求"对真实 gRPC 端口"可调用 —— 进程内直调 service 方法会绕过
//    metadata 解析、状态码序列化、尾随元数据、流控这些**只有走线路才存在**的行为。
//    与 REST 侧 `HttpFixture` 的取舍完全一致（C4.1 也坚持真实回环端口）。
//
//  ★ 生命周期：`service` 必须比 `server` 活得久（与 Router/Server 的关系同理）；
//    这里按声明顺序持有，并在析构里显式 `Shutdown()`。
//  ★ 端口：`AddListeningPort("127.0.0.1:0", ..., &port)` —— 由系统分配，避免端口冲突。
// =============================================================================
#pragma once

#include "app_fixture.h"
#include "http_fixture.h"  // HttpFixture / HttpDo / Authed / TargetOf（DualProtocolFixture 复用 REST 栈）

#include "adapters/grpc/file_service_adapter.h"

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

namespace fss::test {

struct GrpcFixture {
  //  与 REST 用例**同一套端口替身**：两条链路共用同一批用例与同一份状态
  fss::test::AppFixture app;
  std::unique_ptr<fss::adapters::grpc::FileServiceAdapter> service;
  std::unique_ptr<::grpc::Server> server;
  std::unique_ptr<osdu::file::v1::FileService::Stub> stub;
  int port = 0;

  GrpcFixture() {
    service = std::make_unique<fss::adapters::grpc::FileServiceAdapter>(*app.ports, "osdu-user");
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service.get());
    server = builder.BuildAndStart();
    REQUIRE(server != nullptr);
    REQUIRE(port != 0);  // 前置条件显式断言（R9）
    stub = osdu::file::v1::FileService::NewStub(
        ::grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                              ::grpc::InsecureChannelCredentials()));
    REQUIRE(stub != nullptr);
  }
  ~GrpcFixture() {
    if (server) server->Shutdown();
  }
  GrpcFixture(const GrpcFixture&) = delete;
  GrpcFixture& operator=(const GrpcFixture&) = delete;

  //  带调用元数据的上下文（契约 §4.3：authorization / data-partition-id / correlation-id）
  std::unique_ptr<::grpc::ClientContext> Context(
      const std::string& token = "Bearer test-token", const std::string& partition = "opendes",
      const std::string& correlation_id = "corr-grpc-e2e") const {
    auto context = std::make_unique<::grpc::ClientContext>();
    context->AddMetadata("authorization", token);
    context->AddMetadata("data-partition-id", partition);
    context->AddMetadata("correlation-id", correlation_id);
    return context;
  }
};

// -----------------------------------------------------------------------------
//  DualProtocolFixture —— **同一份状态**同时挂在 REST 与 gRPC 两条协议上
// -----------------------------------------------------------------------------
//  C7.3 的等价性矩阵要求"同一份输入分别经 REST 与 gRPC 调用"后逐项相等；
//  C7.8 还要求两条链路**同时运行、互不干扰**。两者都要求两条协议指向**同一批端口**
//  （同一个 metadata/location/repository 实例），因此这里用组合而不是各起一套。
struct DualProtocolFixture {
  fss::test::HttpFixture http;  // REST 栈（内存适配器 + 自签 codec + 真实回环端口）
  std::unique_ptr<fss::adapters::grpc::FileServiceAdapter> service;
  std::unique_ptr<::grpc::Server> grpc_server;
  std::unique_ptr<osdu::file::v1::FileService::Stub> stub;
  int grpc_port = 0;

  DualProtocolFixture() {
    service = std::make_unique<fss::adapters::grpc::FileServiceAdapter>(*http.ports, "osdu-user");
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &grpc_port);
    builder.RegisterService(service.get());
    grpc_server = builder.BuildAndStart();
    REQUIRE(grpc_server != nullptr);
    REQUIRE(grpc_port != 0);
    stub = osdu::file::v1::FileService::NewStub(
        ::grpc::CreateChannel("127.0.0.1:" + std::to_string(grpc_port),
                              ::grpc::InsecureChannelCredentials()));
  }
  ~DualProtocolFixture() {
    if (grpc_server) grpc_server->Shutdown();
  }
  DualProtocolFixture(const DualProtocolFixture&) = delete;
  DualProtocolFixture& operator=(const DualProtocolFixture&) = delete;

  int http_port() const { return http.port(); }

  std::unique_ptr<::grpc::ClientContext> Context(
      const std::string& token = "Bearer test-token", const std::string& partition = "opendes",
      const std::string& correlation_id = "corr-equivalence") const {
    auto context = std::make_unique<::grpc::ClientContext>();
    context->AddMetadata("authorization", token);
    context->AddMetadata("data-partition-id", partition);
    context->AddMetadata("correlation-id", correlation_id);
    return context;
  }
};

// -----------------------------------------------------------------------------
//  PosixDualProtocolFixture —— **真实 POSIX + SQLite** 栈上同时跑 REST 与 gRPC
// -----------------------------------------------------------------------------
//  为什么 C7.6 / C7.10 不能用内存适配器：
//    · `InMemoryBlobStore` 把整个对象放进 RAM —— "1 GiB 上传 RSS < 64 MiB"在那里
//      必然失败（那是适配器的固有开销，不是流式与否的信号；P6-D13 同类教训）。
//    · C7.10 要求 HTTP `Range` 与 gRPC 区间读落在**同一条位置记录**上，因此两条
//      协议必须共用同一个 blob/location 实例 —— 这正是本 fixture 提供的。
//  `PosixStackFixture` 已经是一条真实的 REST 栈（真实文件系统 + 真实 SQLite +
//  真实回环端口），这里只在**同一批 `ports`** 上再挂一个 gRPC 服务。
struct PosixDualProtocolFixture {
  fss::test::PosixStackFixture stack;
  std::unique_ptr<fss::adapters::grpc::FileServiceAdapter> service;
  std::unique_ptr<::grpc::Server> grpc_server;
  std::unique_ptr<osdu::file::v1::FileService::Stub> stub;
  int grpc_port = 0;

  PosixDualProtocolFixture() {
    service = std::make_unique<fss::adapters::grpc::FileServiceAdapter>(*stack.ports, "osdu-user");
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &grpc_port);
    builder.RegisterService(service.get());
    grpc_server = builder.BuildAndStart();
    REQUIRE(grpc_server != nullptr);
    REQUIRE(grpc_port != 0);
    stub = osdu::file::v1::FileService::NewStub(
        ::grpc::CreateChannel("127.0.0.1:" + std::to_string(grpc_port),
                              ::grpc::InsecureChannelCredentials()));
    REQUIRE(stub != nullptr);
  }
  ~PosixDualProtocolFixture() {
    if (grpc_server) grpc_server->Shutdown();
  }
  PosixDualProtocolFixture(const PosixDualProtocolFixture&) = delete;
  PosixDualProtocolFixture& operator=(const PosixDualProtocolFixture&) = delete;

  int http_port() const { return stack.port(); }
  //  POSIX 存储根（`<tempdir>/blobs`）：取消/残留用例直接检查磁盘上的临时文件
  std::string blob_root() const { return stack.dir.child("blobs"); }

  std::unique_ptr<::grpc::ClientContext> Context(
      const std::string& token = "Bearer test-token", const std::string& partition = "opendes",
      const std::string& correlation_id = "corr-streaming") const {
    auto context = std::make_unique<::grpc::ClientContext>();
    context->AddMetadata("authorization", token);
    context->AddMetadata("data-partition-id", partition);
    context->AddMetadata("correlation-id", correlation_id);
    return context;
  }
};

}  // namespace fss::test
