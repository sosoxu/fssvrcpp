// FileServiceAdapter 实现（P7 切片 3：17 个 RPC 全部实现）。
#include "adapters/grpc/file_service_adapter.h"

#include "adapters/grpc/dto/grpc_dto.h"
#include "adapters/grpc/grpc_error_mapper.h"
#include "adapters/grpc/grpc_streaming_io.h"
#include "app/usecases/caller_context.h"
#include "app/usecases/usecases.h"

#include <string>

namespace fss::adapters::grpc {

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
      //  ★ B2a：与 REST `/v2/readiness_check` 读**同一份**判据（`ports_.shared_state_probe`，
      //    由组合根注入 = PG 存活 + 迁移版本；空则只有"仓储可达"最小探针）。
      //    失败原因带在文本里，与 REST 的 503 体同源。
      if (ports_.shared_state_probe) {
        const auto shared = ports_.shared_state_probe();
        if (!shared.ok()) {
          const std::string text = "File service is not ready: " + shared.error().message();
          response->set_status(osdu::file::v1::CheckResponse::SERVING_STATUS_NOT_SERVING);
          response->set_text(text);
          return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, text);
        }
        //  ★ 本切片：共享状态探针存在时它是**唯一**判据（与 REST 逐字同源）。
        //    remote 仓储的 `List` 恒 `kUnimplemented`，继续走下面的最小探针会让 readiness
        //    永远 not ready；PG 形态下最小探针本来就恒成立，跳过无行为影响。
        response->set_status(osdu::file::v1::CheckResponse::SERVING_STATUS_SERVING);
        response->set_text("File service is ready");
        return ::grpc::Status::OK;
      }
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
//  ⑮ ServerSideCopy（扩展：服务端复制字节，不写任何记录）
// ---------------------------------------------------------------------------
::grpc::Status FileServiceAdapter::ServerSideCopy(
    ::grpc::ServerContext* context, const osdu::file::v1::ServerSideCopyRequest* request,
    osdu::file::v1::ServerSideCopyResponse* response) {
  const auto caller = CallerFrom(context);
  fss::app::ServerSideCopy usecase(ports_);
  const auto result = usecase.Execute(caller, request->source_file_source(),
                                      request->target_file_source(),
                                      StorageZoneFromProto(request->target_zone()));
  if (!result.ok()) {
    AttachErrorMetadata(*context, result.error());
    return ToGrpcStatus(result.error());
  }
  FillServerSideCopyProto(result.value(), response);
  return ::grpc::Status::OK;
}

// ---------------------------------------------------------------------------
//  ⑯ UploadFile（客户端流）：首片必须是 `info`，其后全是 `chunk`
// ---------------------------------------------------------------------------
::grpc::Status FileServiceAdapter::UploadFile(
    ::grpc::ServerContext* context,
    ::grpc::ServerReader<osdu::file::v1::UploadFileRequest>* reader,
    osdu::file::v1::UploadFileResponse* response) {
  const auto caller = CallerFrom(context);
  osdu::file::v1::UploadFileRequest first;
  if (!reader->Read(&first)) {
    return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                          "UploadFile 的第一个分片必须携带 info");
  }
  if (!first.has_info()) {
    //  ★ 明确的协议错误，不是"当作空文件上传成功"
    return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                          "UploadFile 的第一个分片必须携带 info（chunk 不能作为首片）");
  }
  const auto request = UploadStreamRequestFromProto(first.info());
  if (!request.ok()) {
    AttachErrorMetadata(*context, request.error());
    return ToGrpcStatus(request.error());
  }
  //  首片已消费 → 只把**后续**分片暴露成字节源
  GrpcUploadSource source(*reader, *context);
  fss::app::UploadFile usecase(ports_);
  const auto result = usecase.Execute(caller, request.value(), source);
  if (!result.ok()) {
    AttachErrorMetadata(*context, result.error());
    return ToGrpcStatus(result.error());
  }
  FillUploadFileResponse(result.value(), response);
  return ::grpc::Status::OK;
}

// ---------------------------------------------------------------------------
//  ⑰ DownloadFile（服务端流）：数据分片 + 一个尾块（totalSize/checksum）
// ---------------------------------------------------------------------------
::grpc::Status FileServiceAdapter::DownloadFile(
    ::grpc::ServerContext* context, const osdu::file::v1::DownloadFileRequest* request,
    ::grpc::ServerWriter<osdu::file::v1::DownloadFileResponse>* writer) {
  const auto caller = CallerFrom(context);
  GrpcDownloadSink sink(*writer);
  fss::app::DownloadFile usecase(ports_);
  const auto result = usecase.Execute(caller, request->file_id(), request->file_source(),
                                      request->offset(), request->length(), sink);
  if (!result.ok()) {
    AttachErrorMetadata(*context, result.error());
    return ToGrpcStatus(result.error());
  }
  //  ★ 尾块：`chunk` 为空，携带完整大小与校验和（0 字节对象也会走到这里）
  osdu::file::v1::DownloadFileResponse trailer;
  FillDownloadTrailer(result.value(), &trailer);
  if (!writer->Write(trailer)) {
    return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "下载流已断开（尾块写失败）");
  }
  return ::grpc::Status::OK;
}

}  // namespace fss::adapters::grpc
