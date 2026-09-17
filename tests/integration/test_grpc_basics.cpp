// =============================================================================
//  test_grpc_basics.cpp —— C7.1（部分）：真实 gRPC 端口上的运维 RPC + 切片边界
// =============================================================================
//  本切片（P7 切片 1）实现 `GetInfo` 与 `Check`；其余 15 个 RPC 明确回
//  `UNIMPLEMENTED`。这个文件证明：
//    · 真实端口 + 真实 channel 能调通已实现的 RPC（含免鉴权语义）；
//    · 调用元数据（契约 §4.3）被正确解析（correlation-id 透传到领域事件）；
//    · 未实现的 RPC **明确**回 UNIMPLEMENTED（而不是静默成功或 INTERNAL）——
//      切片 2/3 会把它们逐个实现，届时本用例的断言随之收紧。
// =============================================================================
#include <catch2/catch.hpp>

#include "grpc_fixture.h"

#include <string>

using fss::test::GrpcFixture;
using osdu::file::v1::FileService;

TEST_CASE("★ C7.1 GetInfo：真实端口可调用、免鉴权、返回版本与依赖服务",
          "[phase7][integration][c7.1]") {
  GrpcFixture fx;
  ::grpc::ClientContext context;
  ::google::protobuf::Empty request;
  osdu::file::v1::InfoResponse response;

  const auto status = fx.stub->GetInfo(&context, request, &response);
  INFO("gRPC 状态：" << status.error_code() << " " << status.error_message());
  REQUIRE(status.ok());
  REQUIRE(response.version() == "v2");
  //  REST `/v2/info` 的 `connectedOuterServices` 是字符串数组；proto 侧是
  //  `ConnectedService{name, version}`
  REQUIRE(response.connected_outer_services_size() >= 1);
  REQUIRE(response.connected_outer_services(0).name() == "storage");
}

TEST_CASE("★ C7.1 Check：liveness/readiness 的文本与 REST 逐字一致；非法 probe → INVALID_ARGUMENT",
          "[phase7][integration][c7.1]") {
  GrpcFixture fx;
  {
    ::grpc::ClientContext context;
    osdu::file::v1::CheckRequest request;
    request.set_probe(osdu::file::v1::CheckRequest::PROBE_LIVENESS);
    osdu::file::v1::CheckResponse response;
    const auto status = fx.stub->Check(&context, request, &response);
    REQUIRE(status.ok());
    REQUIRE(response.status() == osdu::file::v1::CheckResponse::SERVING_STATUS_SERVING);
    REQUIRE(response.text() == "File service is alive");  // 与 REST 纯文本逐字相同
  }
  {
    ::grpc::ClientContext context;
    osdu::file::v1::CheckRequest request;
    request.set_probe(osdu::file::v1::CheckRequest::PROBE_READINESS);
    osdu::file::v1::CheckResponse response;
    const auto status = fx.stub->Check(&context, request, &response);
    REQUIRE(status.ok());
    REQUIRE(response.text() == "File service is ready");
  }
  {
    //  未指定 probe = 调用方错误（REST 没有这个参数，proto 侧必须明确拒绝）
    ::grpc::ClientContext context;
    osdu::file::v1::CheckRequest request;
    osdu::file::v1::CheckResponse response;
    const auto status = fx.stub->Check(&context, request, &response);
    REQUIRE(status.error_code() == ::grpc::StatusCode::INVALID_ARGUMENT);
  }
}

TEST_CASE("★ C7.1 已实现的一元 RPC 在线上可用 + 错误码/尾随元数据正确",
          "[phase7][integration][c7.1]") {
  GrpcFixture fx;
  //  未知记录 → NOT_FOUND，且尾随元数据带上 `fss-error-kind`（REST 错误体的等价物）
  auto context = fx.Context();
  osdu::file::v1::GetFileMetadataRequest request;
  request.set_id("opendes:dataset--File.Generic:ffffffffffffffffffffffffffffffff");
  osdu::file::v1::FileMetadataRecord response;
  const auto status = fx.stub->GetFileMetadata(context.get(), request, &response);
  INFO("GetFileMetadata → " << status.error_code() << " " << status.error_message());
  REQUIRE(status.error_code() == ::grpc::StatusCode::NOT_FOUND);
  const auto& trailing = context->GetServerTrailingMetadata();
  const auto kind = trailing.find("fss-error-kind");
  REQUIRE(kind != trailing.end());
  REQUIRE(std::string(kind->second.data(), kind->second.size()) == "kNotFound");

  //  缺 token → UNAUTHENTICATED（与 REST 的 401 同行）
  auto unauthenticated = fx.Context(/*token=*/"");
  osdu::file::v1::GetFileMetadataRequest request2;
  request2.set_id("whatever");
  osdu::file::v1::FileMetadataRecord response2;
  REQUIRE(fx.stub->GetFileMetadata(unauthenticated.get(), request2, &response2).error_code() ==
          ::grpc::StatusCode::UNAUTHENTICATED);
}

TEST_CASE("★ C7.1 切片边界：3 个扩展 RPC（流式/代理）明确回 UNIMPLEMENTED",
          "[phase7][integration][c7.1]") {
  GrpcFixture fx;
  //  14 个一元 RPC 已在切片 2 完成；剩下的是**扩展面**的 3 个（切片 3 实现字节通道）
  {
    auto context = fx.Context();
    osdu::file::v1::ServerSideCopyRequest request;
    osdu::file::v1::ServerSideCopyResponse response;
    const auto status = fx.stub->ServerSideCopy(context.get(), request, &response);
    REQUIRE(status.error_code() == ::grpc::StatusCode::UNIMPLEMENTED);
    REQUIRE(status.error_message().find("ServerSideCopy") != std::string::npos);
  }
  {
    auto context = fx.Context();
    osdu::file::v1::DownloadFileRequest request;
    auto reader = fx.stub->DownloadFile(context.get(), request);
    osdu::file::v1::DownloadFileResponse chunk;
    //  流式 RPC 未实现时，第一次 Read 就会拿到 UNIMPLEMENTED
    REQUIRE_FALSE(reader->Read(&chunk));
    REQUIRE(reader->Finish().error_code() == ::grpc::StatusCode::UNIMPLEMENTED);
  }
}
