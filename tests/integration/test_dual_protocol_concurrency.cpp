// =============================================================================
//  阶段 7 切片 3：C7.8 —— gRPC 与 REST **同时**运行、互不干扰
// =============================================================================
//  判据 C7.8 的"同时运行"有两种读法，这里都覆盖：
//    ① 适配器级：`DualProtocolFixture`（同一份端口替身挂两条协议）——
//       由 `test_protocol_equivalence` 的 12 行矩阵覆盖（顺序调用）。
//    ② **真实二进制级**：把 `build/bin/fss_server` 拉起来（`FSS_GRPC_PORT=-1`），
//       两个端口都在服务，然后**并发**用两条协议读写**同一份状态**。
//
//  本文件做 ②：
//    · 启动真实组合根（R12：具体实现只在那里创建），解析横幅里的两个实际端口
//    · 用 gRPC 并发写入 K 条记录（上传 + 登记元数据）
//    · 在**同一次运行**里，用 REST 与 gRPC 线程组**并发**读取这些记录
//    · 断言：一条都不失败（没有 5xx / INTERNAL），且两条协议读到**同一份内容**
//
//  ★ 工作线程里**不能**用 Catch2 的 REQUIRE（宏不是线程安全的）：线程只收集结果，
//    断言全部回到主线程做。
// =============================================================================
#include <catch2/catch.hpp>

#include "grpc_fixture.h"       // DualProtocolFixture / AppFixture::MakeRecord
#include "http_fixture.h"       // Authed
#include "raw_http.h"
#include "server_process.h"

#include "adapters/grpc/dto/grpc_dto.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using fss::test::Authed;
using fss::test::RawClient;
using fss::test::ServerProcess;
using osdu::file::v1::FileService;

std::unique_ptr<::grpc::ClientContext> ContextFor(const std::string& correlation_id) {
  auto context = std::make_unique<::grpc::ClientContext>();
  context->AddMetadata("authorization", "Bearer test-token");
  context->AddMetadata("data-partition-id", "opendes");
  context->AddMetadata("correlation-id", correlation_id);
  return context;
}

//  ---- 工作线程用的 REST GET（不用 Catch2 宏）----
struct RestResult {
  bool transport_ok = false;
  int status = 0;
  std::string body;
};

RestResult RestGet(int port, const std::string& target) {
  RestResult out;
  RawClient client(port, /*tcp_nodelay=*/true);
  if (!client.Connect()) return out;
  std::vector<std::string> headers = Authed();
  if (!client.SendRequest("GET", target, headers, "")) return out;
  const auto response = client.ReadResponse(15000);
  if (!response.has_value()) return out;
  out.transport_ok = true;
  out.status = response->status;
  out.body = response->body;
  return out;
}

struct SeededRecord {
  std::string metadata_id;
  std::string file_source;
  std::string name;
};

}  // namespace

TEST_CASE("★ C7.8 真实二进制：REST 与 gRPC 两个端口同时服务、并发互不干扰",
          "[phase7][integration][c7.8]") {
  ServerProcess server;
  INFO("HTTP 端口 " << server.http_port() << "，gRPC 端口 " << server.grpc_port());
  REQUIRE(server.http_port() != server.grpc_port());

  auto channel = ::grpc::CreateChannel("127.0.0.1:" + std::to_string(server.grpc_port()),
                                       ::grpc::InsecureChannelCredentials());
  auto stub = FileService::NewStub(channel);
  REQUIRE(stub != nullptr);

  //  ---- 前置：两条协议都活着（R9：显式前置条件）----
  {
    const auto rest_info = RestGet(server.http_port(), "/api/file/v2/info");
    INFO("REST /v2/info → " << rest_info.status << " " << rest_info.body);
    REQUIRE(rest_info.transport_ok);
    REQUIRE(rest_info.status == 200);

    auto context = ContextFor("c7.8-precheck");
    google::protobuf::Empty request;
    osdu::file::v1::InfoResponse response;
    const auto status = stub->GetInfo(context.get(), request, &response);
    INFO("gRPC GetInfo → " << status.error_code());
    REQUIRE(status.ok());
    REQUIRE(response.version() == "v2");
  }

  //  ---- 用 gRPC 写入 6 条记录（上传字节 + 登记元数据）----
  constexpr int kRecords = 6;
  std::vector<SeededRecord> seeded;
  for (int i = 0; i < kRecords; ++i) {
    const std::string payload = "c7.8-payload-" + std::to_string(i);
    auto upload_context = ContextFor("c7.8-issue-" + std::to_string(i));
    osdu::file::v1::GetUploadLocationRequest issue;
    osdu::file::v1::LocationResponse location;
    REQUIRE(stub->GetUploadLocation(upload_context.get(), issue, &location).ok());
    const std::string file_source = location.location().file_source();

    {
      auto context = ContextFor("c7.8-upload-" + std::to_string(i));
      osdu::file::v1::UploadFileResponse response;
      auto writer = stub->UploadFile(context.get(), &response);
      osdu::file::v1::UploadFileRequest info;
      info.mutable_info()->set_file_source(file_source);
      REQUIRE(writer->Write(info));
      osdu::file::v1::UploadFileRequest chunk;
      chunk.set_chunk(payload);
      REQUIRE(writer->Write(chunk));
      writer->WritesDone();
      const auto status = writer->Finish();
      INFO("UploadFile #" << i << " → " << status.error_code() << " " << status.error_message());
      REQUIRE(status.ok());
    }

    auto record = fss::test::AppFixture::MakeRecord(file_source, "c7.8-" + std::to_string(i) + ".bin");
    osdu::file::v1::FileMetadataRecord metadata;
    fss::adapters::grpc::FillMetadataProto(record, &metadata);
    auto context = ContextFor("c7.8-register-" + std::to_string(i));
    osdu::file::v1::CreateFileMetadataResponse response;
    const auto status = stub->CreateFileMetadata(context.get(), metadata, &response);
    INFO("CreateFileMetadata #" << i << " → " << status.error_code() << " "
                                << status.error_message());
    REQUIRE(status.ok());
    seeded.push_back(SeededRecord{response.id(), file_source, metadata.data().name()});
  }

  //  ---- 并发：REST 线程组 + gRPC 线程组同时读同一批记录 ----
  std::atomic<int> rest_calls{0};
  std::atomic<int> grpc_calls{0};
  std::atomic<int> rest_failures{0};
  std::atomic<int> grpc_failures{0};
  std::mutex failure_mutex;
  std::vector<std::string> failure_messages;
  const auto note_failure = [&](const std::string& message) {
    std::lock_guard<std::mutex> guard(failure_mutex);
    failure_messages.push_back(message);
  };

  constexpr int kRounds = 5;
  std::vector<std::thread> threads;

  //  REST 线程：探活 + 逐条读元数据（跨协议读 gRPC 写入的记录）
  for (int worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&, worker]() {
      for (int round = 0; round < kRounds; ++round) {
        ++rest_calls;
        const auto info = RestGet(server.http_port(), "/api/file/v2/info");
        if (!info.transport_ok || info.status != 200) {
          ++rest_failures;
          note_failure("REST /v2/info worker=" + std::to_string(worker) +
                       " status=" + std::to_string(info.status));
        }
        const auto& record = seeded[(worker + round) % seeded.size()];
        ++rest_calls;
        const auto metadata =
            RestGet(server.http_port(), "/api/file/v2/files/" + record.metadata_id + "/metadata");
        if (!metadata.transport_ok || metadata.status != 200) {
          ++rest_failures;
          note_failure("REST metadata worker=" + std::to_string(worker) + " status=" +
                       std::to_string(metadata.status));
          continue;
        }
        //  跨协议一致性：REST 读到的 Name 必须与 gRPC 写入的一致
        const auto json = fss::json::ParseObject(metadata.body);
        if (!json.ok() || json.value()["data"]["Name"].get<std::string>() != record.name) {
          ++rest_failures;
          note_failure("REST metadata 内容与 gRPC 写入不一致 worker=" + std::to_string(worker));
        }
      }
    });
  }

  //  gRPC 线程：探活 + GetFileMetadata
  for (int worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&, worker]() {
      for (int round = 0; round < kRounds; ++round) {
        ++grpc_calls;
        {
          auto context = ContextFor("c7.8-concurrent-info");
          google::protobuf::Empty request;
          osdu::file::v1::InfoResponse response;
          const auto status = stub->GetInfo(context.get(), request, &response);
          if (!status.ok()) {
            ++grpc_failures;
            note_failure("gRPC GetInfo worker=" + std::to_string(worker) + " code=" +
                         std::to_string(status.error_code()));
          }
        }
        const auto& record = seeded[(worker * 2 + round) % seeded.size()];
        ++grpc_calls;
        auto context = ContextFor("c7.8-concurrent-metadata");
        osdu::file::v1::GetFileMetadataRequest request;
        request.set_id(record.metadata_id);
        osdu::file::v1::FileMetadataRecord response;
        const auto status = stub->GetFileMetadata(context.get(), request, &response);
        if (!status.ok() || response.data().name() != record.name) {
          ++grpc_failures;
          note_failure("gRPC GetFileMetadata worker=" + std::to_string(worker) + " code=" +
                       std::to_string(status.error_code()));
        }
      }
    });
  }

  for (auto& thread : threads) thread.join();

  {
    std::lock_guard<std::mutex> guard(failure_mutex);
    for (const auto& message : failure_messages) INFO("失败：" << message);
    CAPTURE(rest_calls.load(), grpc_calls.load(), rest_failures.load(), grpc_failures.load());
    REQUIRE(failure_messages.empty());
  }
  REQUIRE(rest_calls.load() >= 40);
  REQUIRE(grpc_calls.load() >= 40);
  REQUIRE(rest_failures.load() == 0);
  REQUIRE(grpc_failures.load() == 0);
  //  ★ 两条协议在**同一次运行**里都服务过（不是"先跑完 REST 再跑 gRPC"）
  CHECK(rest_calls.load() > 0);
  CHECK(grpc_calls.load() > 0);

  //  ---- 收尾：两条协议读到的记录集合一致 ----
  for (const auto& record : seeded) {
    const auto rest =
        RestGet(server.http_port(), "/api/file/v2/files/" + record.metadata_id + "/metadata");
    REQUIRE(rest.status == 200);
    auto context = ContextFor("c7.8-final");
    osdu::file::v1::GetFileMetadataRequest request;
    request.set_id(record.metadata_id);
    osdu::file::v1::FileMetadataRecord response;
    REQUIRE(stub->GetFileMetadata(context.get(), request, &response).ok());
    REQUIRE(response.data().name() == record.name);
    REQUIRE(response.data().name() ==
            fss::json::ParseObject(rest.body).value()["data"]["Name"].get<std::string>());
  }
}
