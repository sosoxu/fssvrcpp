// =============================================================================
//  FileServiceAdapter（L5）—— `osdu.file.v1.FileService` 的服务端实现
// =============================================================================
//  设计约束（ADR-001、docs/02-design.md §3.2）
//    · **只做翻译**：解析 metadata/请求 → `app::CallerContext` → 调 L4 用例 →
//      领域结果 → proto。业务逻辑一行都不在这里（两条协议共用同一批用例）。
//    · 服务端**不代理字节**的原则与 REST 一致：`UploadFile`/`DownloadFile` 是扩展
//      （集中存储没有原生签名机制时的字节通道），见 proto 的说明。
//    · 未实现的 RPC 明确回 `UNIMPLEMENTED`（不是静默成功、也不是 INTERNAL）——
//      C7.1 逐条要求 17 个可调用，本文件按切片推进并如实标注。
//
//  实现进度：**17 个 RPC 全部实现**（切片 1 = 运维 2 个；切片 2 = 其余 12 个一元 RPC；
//  切片 3 = 3 个流式/代理 RPC）。流式字节的翻译在 `grpc_streaming_io.{h,cpp}`。
// =============================================================================
#pragma once

#include "app/usecases/usecases.h"

#include <osdu/file/v1/file_service.grpc.pb.h>

namespace fss::adapters::grpc {

class FileServiceAdapter final : public osdu::file::v1::FileService::Service {
 public:
  explicit FileServiceAdapter(fss::app::UseCasePorts& ports, std::string default_user_id)
      : ports_(ports), default_user_id_(std::move(default_user_id)) {}

  //  ---- 运维（REST：/v2/info、/v2/{liveness,readiness}_check；都免鉴权）----
  ::grpc::Status GetInfo(::grpc::ServerContext* context, const google::protobuf::Empty* request,
                       osdu::file::v1::InfoResponse* response) override;
  ::grpc::Status Check(::grpc::ServerContext* context, const osdu::file::v1::CheckRequest* request,
                     osdu::file::v1::CheckResponse* response) override;

  //  ---- 一元 RPC（切片 1/2）----
  ::grpc::Status GetUploadLocation(::grpc::ServerContext*, const osdu::file::v1::GetUploadLocationRequest*,
                                 osdu::file::v1::LocationResponse*) override;
  ::grpc::Status GetFileLocation(::grpc::ServerContext*, const osdu::file::v1::GetFileLocationRequest*,
                               osdu::file::v1::GetFileLocationResponse*) override;
  ::grpc::Status GetDownloadLocation(::grpc::ServerContext*,
                                   const osdu::file::v1::GetDownloadLocationRequest*,
                                   osdu::file::v1::DownloadUrlResponse*) override;
  ::grpc::Status GetFileList(::grpc::ServerContext*, const osdu::file::v1::GetFileListRequest*,
                           osdu::file::v1::FileListResponse*) override;
  ::grpc::Status CreateFileMetadata(::grpc::ServerContext*,
                                  const osdu::file::v1::FileMetadataRecord*,
                                  osdu::file::v1::CreateFileMetadataResponse*) override;
  ::grpc::Status GetFileMetadata(::grpc::ServerContext*,
                               const osdu::file::v1::GetFileMetadataRequest*,
                               osdu::file::v1::FileMetadataRecord*) override;
  ::grpc::Status DeleteFileMetadata(::grpc::ServerContext*,
                                  const osdu::file::v1::DeleteFileMetadataRequest*,
                                  google::protobuf::Empty*) override;
  ::grpc::Status GetStorageInstructions(::grpc::ServerContext*,
                                      const osdu::file::v1::GetStorageInstructionsRequest*,
                                      osdu::file::v1::StorageInstructionsResponse*) override;
  ::grpc::Status GetRetrievalInstructions(::grpc::ServerContext*,
                                        const osdu::file::v1::GetRetrievalInstructionsRequest*,
                                        osdu::file::v1::RetrievalInstructionsResponse*) override;
  ::grpc::Status CopyFilesToPersistent(::grpc::ServerContext*,
                                     const osdu::file::v1::CopyDmsRequest*,
                                     osdu::file::v1::CopyDmsResponseList*) override;
  ::grpc::Status GetFileSignedUrl(::grpc::ServerContext*,
                                const osdu::file::v1::UrlSigningRequest*,
                                osdu::file::v1::UrlSigningResponse*) override;
  ::grpc::Status RevokeUrl(::grpc::ServerContext*, const osdu::file::v1::RevokeUrlRequest*,
                         google::protobuf::Empty*) override;
  ::grpc::Status ServerSideCopy(::grpc::ServerContext*,
                              const osdu::file::v1::ServerSideCopyRequest*,
                              osdu::file::v1::ServerSideCopyResponse*) override;

  //  ---- 流式 / 代理（切片 3）----
  ::grpc::Status UploadFile(::grpc::ServerContext*,
                          ::grpc::ServerReader<osdu::file::v1::UploadFileRequest>*,
                          osdu::file::v1::UploadFileResponse*) override;
  ::grpc::Status DownloadFile(::grpc::ServerContext*, const osdu::file::v1::DownloadFileRequest*,
                            ::grpc::ServerWriter<osdu::file::v1::DownloadFileResponse>*) override;

 private:
  //  调用元数据（契约 §4.3：`authorization` / `data-partition-id` / `correlation-id`）→
  //  与 REST **共用**同一套解析规则（`app::CallerFromHeaders`）
  fss::app::CallerContext CallerFrom(::grpc::ServerContext* context) const;

  fss::app::UseCasePorts& ports_;
  std::string default_user_id_;
};

}  // namespace fss::adapters::grpc
