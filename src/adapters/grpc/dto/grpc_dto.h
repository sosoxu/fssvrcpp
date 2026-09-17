// =============================================================================
//  grpc_dto（L5）—— proto ↔ 领域模型的双向转换
// =============================================================================
//  约束（docs/02-design.md §3.2、契约 §4.2）
//    · proto 类型**只允许**出现在 `src/adapters/grpc/` 内（护栏 C7.7 强制执行）；
//      领域层不知道 protobuf 的存在。
//    · proto 的 `json_name` 已对齐 OSDU JSON（`data` 内 PascalCase、信封 camelCase），
//      因此"启用 proto3-JSON 的客户端"能直接生产/消费规范 JSON（C7.5）。
//
//  ⚠️ 一处**已知的表达力差异**（如实登记，不静默丢失）
//    proto3 没有"未知字段保留"：领域模型用 `extra`（`json::Value`）承载的**未识别字段**
//    无法经 proto 往返。规则：
//      · 领域 → proto：`data.extra["ExtensionProperties"]` → `data.extension_properties`，
//        `data.extra` 里的其它键**不会**出现在 proto 里；
//      · proto → 领域：`data.extension_properties` → `data.extra["ExtensionProperties"]`。
//    因此"全字段无损往返"只在 REST 链路上成立（C6.1 的语境）；RPC 链路按**已建模字段**
//    比对（C7.3 的等价性矩阵比较的是**领域结果**，不是 JSON 字符串）。
// =============================================================================
#pragma once

#include "app/usecases/usecases.h"
#include "common/result/result.h"
#include "domain/model/file_metadata.h"

#include <osdu/file/v1/file_service.pb.h>

#include <optional>
#include <vector>

namespace fss::adapters::grpc {

//  `InfoResponse`（运维端点，无错误路径）
void FillInfoProto(const fss::app::VersionInfo& info, osdu::file::v1::InfoResponse* out);

//  元数据记录：领域 → proto（`version` 只在响应里有值）
void FillMetadataProto(const fss::domain::FileMetadataRecord& record,
                       osdu::file::v1::FileMetadataRecord* out);
//  proto → 领域。结构性错误（缺必需段等）交给领域校验（`ValidateMetadataRecord`）报 400。
fss::Result<fss::domain::FileMetadataRecord> MetadataFromProto(
    const osdu::file::v1::FileMetadataRecord& proto);

//  ---- 位置 / 列表 / DMS / 交付 的响应填充（与 REST 的 DTO 一一对应）----
void FillLocationProto(const fss::app::LocationResult& result,
                       osdu::file::v1::LocationResponse* out);
void FillFileLocationProto(const fss::app::FileLocationView& view,
                           osdu::file::v1::GetFileLocationResponse* out);
void FillDownloadUrlProto(const fss::app::DownloadLocationResult& result,
                          osdu::file::v1::DownloadUrlResponse* out);
void FillFileListProto(const fss::app::FileListResult& result,
                       osdu::file::v1::FileListResponse* out);
//  `collection = true` 时用集合版的键集合（`fileCollectionSource` + `fileCount`/`fileNames`）
void FillStorageInstructionsProto(const fss::app::StorageInstructions& instructions,
                                  bool collection,
                                  osdu::file::v1::StorageInstructionsResponse* out);
void FillRetrievalInstructionsProto(
    const std::vector<fss::app::RetrievalInstruction>& instructions, bool collection,
    osdu::file::v1::RetrievalInstructionsResponse* out);
void FillCopyDmsProto(const std::vector<fss::app::CopyFileOutcome>& outcomes,
                      osdu::file::v1::CopyDmsResponseList* out);
void FillUrlSigningProto(const fss::app::SignedUrlResult& result,
                         osdu::file::v1::UrlSigningResponse* out);

//  ---- 请求侧（proto → 领域输入）----
//  `ExpirySpec.raw` → 用例的 `expiryTime`（空串 = 未提供，走缺省 1H / 上限 7D）
std::optional<std::string> ExpiryFromProto(const osdu::file::v1::ExpirySpec& spec);
//  `FileListRequest` → 领域请求（时间用与 REST 相同的 `ParseIso8601`）
fss::Result<fss::app::FileListRequest> FileListRequestFromProto(
    const osdu::file::v1::FileListRequest& proto);
//  `CopyDmsRequest.datasetSources`（Struct 列表，元素是记录或路径字符串）
std::vector<fss::app::CopyFileSource> CopySourcesFromProto(
    const osdu::file::v1::CopyDmsRequest& proto);

//  ---- 扩展 RPC（P7 切片 3：流式上传/下载 + 服务端复制）----
//  `UploadFileInfo` → 上传用例输入（`registerMetadata=true` 时带上 metadata 记录）。
//  ★ 返回 `Result` 而不是"尽力而为"的结构体：`metadata` 段的结构错误必须**显式报错**，
//    静默丢掉它会让 `registerMetadata=true` 变成"上传成功但没登记记录"。
fss::Result<fss::app::UploadStreamRequest> UploadStreamRequestFromProto(
    const osdu::file::v1::UploadFileInfo& proto);
void FillUploadFileResponse(const fss::app::UploadStreamResult& result,
                            osdu::file::v1::UploadFileResponse* out);
//  下载流的**尾块**（`chunk` 为空，只携带 totalSize/checksum）
void FillDownloadTrailer(const fss::app::DownloadStreamResult& result,
                         osdu::file::v1::DownloadFileResponse* out);
void FillServerSideCopyProto(const fss::app::ServerSideCopyResult& result,
                             osdu::file::v1::ServerSideCopyResponse* out);
//  proto 的 `targetZone` → 领域 zone。★ `STORAGE_ZONE_UNSPECIFIED` 取 **persistent**
//  （该 RPC 的用途是"搬到持久区"；未指定时按这个语义处理，已在契约 §4 登记）。
fss::domain::StorageZone StorageZoneFromProto(osdu::file::v1::StorageZone zone);

}  // namespace fss::adapters::grpc
