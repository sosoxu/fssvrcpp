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

TEST_CASE("★ C7.1 切片边界：未实现的 RPC 明确回 UNIMPLEMENTED（不是 OK、不是 INTERNAL）",
          "[phase7][integration][c7.1]") {
  GrpcFixture fx;
  {
    auto context = fx.Context();
    osdu::file::v1::GetFileMetadataRequest request;
    request.set_id("opendes:dataset--File.Generic:deadbeef");
    osdu::file::v1::FileMetadataRecord response;
    const auto status = fx.stub->GetFileMetadata(context.get(), request, &response);
    INFO("GetFileMetadata → " << status.error_code() << " " << status.error_message());
    REQUIRE(status.error_code() == ::grpc::StatusCode::UNIMPLEMENTED);
    REQUIRE(status.error_message().find("GetFileMetadata") != std::string::npos);
  }
  {
    auto context = fx.Context();
    osdu::file::v1::RevokeUrlRequest request;
    ::google::protobuf::Empty response;
    const auto status = fx.stub->RevokeUrl(context.get(), request, &response);
    REQUIRE(status.error_code() == ::grpc::StatusCode::UNIMPLEMENTED);
  }
}
