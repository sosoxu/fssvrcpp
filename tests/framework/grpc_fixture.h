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

}  // namespace fss::test
