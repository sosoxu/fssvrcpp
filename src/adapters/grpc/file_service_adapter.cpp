// FileServiceAdapter 实现（P7 切片 1：运维两个 RPC + 其余明确 UNIMPLEMENTED）。
#include "adapters/grpc/file_service_adapter.h"

#include "adapters/grpc/dto/grpc_dto.h"
#include "adapters/grpc/grpc_error_mapper.h"
#include "app/usecases/caller_context.h"
#include "app/usecases/usecases.h"

#include <string>

namespace fss::adapters::grpc {

namespace {

//  未实现的 RPC 统一回 UNIMPLEMENTED（契约 §5：`kUnimplemented` ↔ `UNIMPLEMENTED`）
::grpc::Status NotYet(const char* rpc) {
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED,
                      std::string(rpc) + " 尚未实现（P7 切片 2/3）");
}

}  // namespace

::grpc::Status FileServiceAdapter::GetInfo(::grpc::ServerContext* /*context*/,
                                         const google::protobuf::Empty* /*request*/,
                                         osdu::file::v1::InfoResponse* response) {
  //  REST 的 `/v2/info` 同样免鉴权；两条链路都返回 `VersionInfo`
  fss::app::GetInfo usecase(ports_);
  const auto info = usecase.Execute();
  if (!info.ok()) {
    return ToGrpcStatus(info.error());
  }
  FillInfoProto(info.value(), response);
  return ::grpc::Status::OK;
}

::grpc::Status FileServiceAdapter::Check(::grpc::ServerContext* /*context*/,
                                       const osdu::file::v1::CheckRequest* request,
                                       osdu::file::v1::CheckResponse* response) {
  //  与 REST 探针**逐字相同**的语义（契约 §2.12）：
  //    liveness  → 200 "File service is alive"
  //    readiness → 仓储可达 → 200 "File service is ready"；不可达 → 503
  switch (request->probe()) {
    case osdu::file::v1::CheckRequest::PROBE_LIVENESS: {
      response->set_status(osdu::file::v1::CheckResponse::SERVING_STATUS_SERVING);
      response->set_text("File service is alive");
      return ::grpc::Status::OK;
    }
    case osdu::file::v1::CheckRequest::PROBE_READINESS: {
      const auto probe = ports_.metadata.List("__readiness__", fss::domain::MetadataQuery{});
      if (!probe.ok()) {
        response->set_status(osdu::file::v1::CheckResponse::SERVING_STATUS_NOT_SERVING);
        response->set_text("File service is not ready");
        //  与 REST 的 503 对齐：gRPC 侧用 UNAVAILABLE（契约 §5 的同义词）
        return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "File service is not ready");
      }
      response->set_status(osdu::file::v1::CheckResponse::SERVING_STATUS_SERVING);
      response->set_text("File service is ready");
      return ::grpc::Status::OK;
    }
    case osdu::file::v1::CheckRequest::PROBE_UNSPECIFIED:
    default:
      //  REST 的这两个探针没有参数；未指定探测类型属于**调用方错误**
      return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                          "probe 必须是 PROBE_LIVENESS 或 PROBE_READINESS");
  }
}

// ---------------------------------------------------------------------------
//  切片 2 / 3 的实现位置（保留签名以免"忘了实现"变成编译错误而不是运行错误）
// ---------------------------------------------------------------------------
::grpc::Status FileServiceAdapter::GetUploadLocation(::grpc::ServerContext*,
                                                   const osdu::file::v1::GetUploadLocationRequest*,
                                                   osdu::file::v1::LocationResponse*) {
  return NotYet("GetUploadLocation");
}
::grpc::Status FileServiceAdapter::GetFileLocation(::grpc::ServerContext*,
                                                 const osdu::file::v1::GetFileLocationRequest*,
                                                 osdu::file::v1::GetFileLocationResponse*) {
  return NotYet("GetFileLocation");
}
::grpc::Status FileServiceAdapter::GetDownloadLocation(
    ::grpc::ServerContext*, const osdu::file::v1::GetDownloadLocationRequest*,
    osdu::file::v1::DownloadUrlResponse*) {
  return NotYet("GetDownloadLocation");
}
::grpc::Status FileServiceAdapter::GetFileList(::grpc::ServerContext*,
                                             const osdu::file::v1::GetFileListRequest*,
                                             osdu::file::v1::FileListResponse*) {
  return NotYet("GetFileList");
}
::grpc::Status FileServiceAdapter::CreateFileMetadata(
    ::grpc::ServerContext*, const osdu::file::v1::FileMetadataRecord*,
    osdu::file::v1::CreateFileMetadataResponse*) {
  return NotYet("CreateFileMetadata");
}
::grpc::Status FileServiceAdapter::GetFileMetadata(
    ::grpc::ServerContext*, const osdu::file::v1::GetFileMetadataRequest*,
    osdu::file::v1::FileMetadataRecord*) {
  return NotYet("GetFileMetadata");
}
::grpc::Status FileServiceAdapter::DeleteFileMetadata(
    ::grpc::ServerContext*, const osdu::file::v1::DeleteFileMetadataRequest*,
    google::protobuf::Empty*) {
  return NotYet("DeleteFileMetadata");
}
::grpc::Status FileServiceAdapter::GetStorageInstructions(
    ::grpc::ServerContext*, const osdu::file::v1::GetStorageInstructionsRequest*,
    osdu::file::v1::StorageInstructionsResponse*) {
  return NotYet("GetStorageInstructions");
}
::grpc::Status FileServiceAdapter::GetRetrievalInstructions(
    ::grpc::ServerContext*, const osdu::file::v1::GetRetrievalInstructionsRequest*,
    osdu::file::v1::RetrievalInstructionsResponse*) {
  return NotYet("GetRetrievalInstructions");
}
::grpc::Status FileServiceAdapter::CopyFilesToPersistent(::grpc::ServerContext*,
                                                       const osdu::file::v1::CopyDmsRequest*,
                                                       osdu::file::v1::CopyDmsResponseList*) {
  return NotYet("CopyFilesToPersistent");
}
::grpc::Status FileServiceAdapter::GetFileSignedUrl(::grpc::ServerContext*,
                                                  const osdu::file::v1::UrlSigningRequest*,
                                                  osdu::file::v1::UrlSigningResponse*) {
  return NotYet("GetFileSignedUrl");
}
::grpc::Status FileServiceAdapter::RevokeUrl(::grpc::ServerContext*,
                                           const osdu::file::v1::RevokeUrlRequest*,
                                           google::protobuf::Empty*) {
  return NotYet("RevokeUrl");
}
::grpc::Status FileServiceAdapter::ServerSideCopy(::grpc::ServerContext*,
                                                const osdu::file::v1::ServerSideCopyRequest*,
                                                osdu::file::v1::ServerSideCopyResponse*) {
  return NotYet("ServerSideCopy");
}
::grpc::Status FileServiceAdapter::UploadFile(::grpc::ServerContext*,
                                            ::grpc::ServerReader<osdu::file::v1::UploadFileRequest>*,
                                            osdu::file::v1::UploadFileResponse*) {
  return NotYet("UploadFile");
}
::grpc::Status FileServiceAdapter::DownloadFile(
    ::grpc::ServerContext*, const osdu::file::v1::DownloadFileRequest*,
    ::grpc::ServerWriter<osdu::file::v1::DownloadFileResponse>*) {
  return NotYet("DownloadFile");
}

}  // namespace fss::adapters::grpc
