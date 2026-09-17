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

::fss::app::CallerContext FileServiceAdapter::CallerFrom(::grpc::ServerContext* context) const {
  const auto& metadata = context->client_metadata();
  const auto lookup = [&metadata](std::string_view name) -> std::optional<std::string> {
    const auto it = metadata.find(::grpc::string_ref(name.data(), name.size()));
    if (it == metadata.end()) return std::nullopt;
    return std::string(it->second.data(), it->second.size());
  };
  std::string correlation_id;
  if (const auto value = lookup("correlation-id"); value.has_value()) correlation_id = *value;
  return fss::app::CallerFromHeaders(lookup, correlation_id, default_user_id_);
}

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

::grpc::Status FileServiceAdapter::GetUploadLocation(
    ::grpc::ServerContext* context, const osdu::file::v1::GetUploadLocationRequest* request,
    osdu::file::v1::LocationResponse* response) {
  const auto caller = CallerFrom(context);
  fss::app::GetUploadLocation usecase(ports_);
  const std::optional<std::string> file_id =
      request->file_id().empty() ? std::nullopt
                                 : std::optional<std::string>(request->file_id());
  const auto result = usecase.Execute(caller, file_id, ExpiryFromProto(request->expiry()));
  if (!result.ok()) {
    AttachErrorMetadata(*context, result.error());
    return ToGrpcStatus(result.error());
  }
  FillLocationProto(result.value(), response);
  return ::grpc::Status::OK;
}

::grpc::Status FileServiceAdapter::GetFileLocation(
    ::grpc::ServerContext* context, const osdu::file::v1::GetFileLocationRequest* request,
    osdu::file::v1::GetFileLocationResponse* response) {
  const auto caller = CallerFrom(context);
  fss::app::GetFileLocation usecase(ports_);
  const auto view = usecase.Execute(caller, request->file_id());
  if (!view.ok()) {
    AttachErrorMetadata(*context, view.error());
    return ToGrpcStatus(view.error());
  }
  FillFileLocationProto(view.value(), response);
  return ::grpc::Status::OK;
}

::grpc::Status FileServiceAdapter::GetDownloadLocation(
    ::grpc::ServerContext* context, const osdu::file::v1::GetDownloadLocationRequest* request,
    osdu::file::v1::DownloadUrlResponse* response) {
  const auto caller = CallerFrom(context);
  fss::app::GetDownloadLocation usecase(ports_);
  const auto result = usecase.Execute(caller, request->id(), ExpiryFromProto(request->expiry()));
  if (!result.ok()) {
    AttachErrorMetadata(*context, result.error());
    return ToGrpcStatus(result.error());
  }
  FillDownloadUrlProto(result.value(), response);
  return ::grpc::Status::OK;
}

::grpc::Status FileServiceAdapter::GetFileList(::grpc::ServerContext* context,
                                                 const osdu::file::v1::GetFileListRequest* request,
                                                 osdu::file::v1::FileListResponse* response) {
  const auto caller = CallerFrom(context);
  const auto list_request = FileListRequestFromProto(request->request());
  if (!list_request.ok()) {
    AttachErrorMetadata(*context, list_request.error());
    return ToGrpcStatus(list_request.error());
  }
  fss::app::GetFileList usecase(ports_);
  const auto page = usecase.Execute(caller, list_request.value());
  if (!page.ok()) {
    AttachErrorMetadata(*context, page.error());
    return ToGrpcStatus(page.error());
  }
  FillFileListProto(page.value(), response);
  return ::grpc::Status::OK;
}

::grpc::Status FileServiceAdapter::CreateFileMetadata(
    ::grpc::ServerContext* context, const osdu::file::v1::FileMetadataRecord* request,
    osdu::file::v1::CreateFileMetadataResponse* response) {
  const auto caller = CallerFrom(context);
  const auto record = MetadataFromProto(*request);
  if (!record.ok()) {
    AttachErrorMetadata(*context, record.error());
    return ToGrpcStatus(record.error());
  }
  fss::app::CreateFileMetadata usecase(ports_);
  const auto id = usecase.Execute(caller, record.value());
  if (!id.ok()) {
    AttachErrorMetadata(*context, id.error());
    return ToGrpcStatus(id.error());
  }
  response->set_id(id.value());
  return ::grpc::Status::OK;
}

::grpc::Status FileServiceAdapter::GetFileMetadata(
    ::grpc::ServerContext* context, const osdu::file::v1::GetFileMetadataRequest* request,
    osdu::file::v1::FileMetadataRecord* response) {
  const auto caller = CallerFrom(context);
  fss::app::GetFileMetadata usecase(ports_);
  const auto record = usecase.Execute(caller, request->id());
  if (!record.ok()) {
    AttachErrorMetadata(*context, record.error());
    return ToGrpcStatus(record.error());
  }
  FillMetadataProto(record.value(), response);
  return ::grpc::Status::OK;
}

::grpc::Status FileServiceAdapter::DeleteFileMetadata(
    ::grpc::ServerContext* context, const osdu::file::v1::DeleteFileMetadataRequest* request,
    google::protobuf::Empty* /*response*/) {
  const auto caller = CallerFrom(context);
  fss::app::DeleteFileMetadata usecase(ports_);
  const auto result = usecase.Execute(caller, request->id());
  if (!result.ok()) {
    AttachErrorMetadata(*context, result.error());
    return ToGrpcStatus(result.error());
  }
  return ::grpc::Status::OK;
}

::grpc::Status FileServiceAdapter::GetStorageInstructions(
    ::grpc::ServerContext* context, const osdu::file::v1::GetStorageInstructionsRequest* request,
    osdu::file::v1::StorageInstructionsResponse* response) {
  const auto caller = CallerFrom(context);
  fss::app::GetStorageInstructions usecase(ports_);
  const auto result = usecase.Execute(caller, ExpiryFromProto(request->expiry()));
  if (!result.ok()) {
    AttachErrorMetadata(*context, result.error());
    return ToGrpcStatus(result.error());
  }
  //  ★ 集合版的键集合由调用方的 RPC 决定（proto 没有这个区分，因为它们是两条 REST 路径）。
  //    gRPC 侧用**单文件版**键集合（与 `/v2/files/storageInstructions` 等价）。
  FillStorageInstructionsProto(result.value(), /*collection=*/false, response);
  return ::grpc::Status::OK;
}

::grpc::Status FileServiceAdapter::GetRetrievalInstructions(
    ::grpc::ServerContext* context, const osdu::file::v1::GetRetrievalInstructionsRequest* request,
    osdu::file::v1::RetrievalInstructionsResponse* response) {
  const auto caller = CallerFrom(context);
  std::vector<std::string> registry_ids(request->dataset_registry_ids().begin(),
                                        request->dataset_registry_ids().end());
  fss::app::GetRetrievalInstructions usecase(ports_);
  const auto result =
      usecase.Execute(caller, registry_ids, ExpiryFromProto(request->expiry()));
  if (!result.ok()) {
    AttachErrorMetadata(*context, result.error());
    return ToGrpcStatus(result.error());
  }
  FillRetrievalInstructionsProto(result.value(), /*collection=*/false, response);
  return ::grpc::Status::OK;
}

::grpc::Status FileServiceAdapter::CopyFilesToPersistent(
    ::grpc::ServerContext* context, const osdu::file::v1::CopyDmsRequest* request,
    osdu::file::v1::CopyDmsResponseList* response) {
  const auto caller = CallerFrom(context);
  fss::app::CopyFiles usecase(ports_);
  const auto outcomes = usecase.Execute(caller, CopySourcesFromProto(*request));
  if (!outcomes.ok()) {
    AttachErrorMetadata(*context, outcomes.error());
    return ToGrpcStatus(outcomes.error());
  }
  FillCopyDmsProto(outcomes.value(), response);
  return ::grpc::Status::OK;
}

::grpc::Status FileServiceAdapter::GetFileSignedUrl(
    ::grpc::ServerContext* context, const osdu::file::v1::UrlSigningRequest* request,
    osdu::file::v1::UrlSigningResponse* response) {
  const auto caller = CallerFrom(context);
  std::vector<std::string> srns(request->srns().begin(), request->srns().end());
  fss::app::GetFileSignedUrl usecase(ports_);
  const auto result = usecase.Execute(caller, srns, std::nullopt);
  if (!result.ok()) {
    AttachErrorMetadata(*context, result.error());
    return ToGrpcStatus(result.error());
  }
  FillUrlSigningProto(result.value(), response);
  return ::grpc::Status::OK;
}

::grpc::Status FileServiceAdapter::RevokeUrl(::grpc::ServerContext* context,
                                           const osdu::file::v1::RevokeUrlRequest* /*request*/,
                                           google::protobuf::Empty* /*response*/) {
  const auto caller = CallerFrom(context);
  fss::app::RevokeUrl usecase(ports_);
  const auto result = usecase.Execute(caller);
  if (!result.ok()) {
    AttachErrorMetadata(*context, result.error());
    return ToGrpcStatus(result.error());
  }
  return ::grpc::Status::OK;
}

// ---------------------------------------------------------------------------
//  切片 2 / 3 的实现位置（保留签名以免"忘了实现"变成编译错误而不是运行错误）
// ---------------------------------------------------------------------------
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
