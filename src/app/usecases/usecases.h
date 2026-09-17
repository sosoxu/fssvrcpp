// =============================================================================
//  应用层用例（L4）—— 13 个（docs/04-implementation-plan.md 阶段 2 任务 8）
// =============================================================================
//  ★ 纪律
//    · 用例**只依赖端口**（+ 领域类型 + L4 的纯策略）；不 include grpc/sqlite/curl/
//      openssl/httplib/common/http（由 test_layering_guard 与 CMake 目标图强制）。
//    · 用例里**不得查询存储能力**：需要上传/下载地址一律走 `LocationIssuer`
//      （`capabilities()` 的调用点白名单只有它，见 C2.7）。
//    · 授权在用例入口做一次：缺 token → `kUnauthenticated`("Missing authorization token")、
//      缺 partition → `kUnauthenticated`("Missing partitionID")、角色不足 → `kPermissionDenied`。
//      这与契约 §1.2/§1.3/§5 一致；P4 的中间件是同一约束的第二道（纵深防御）。
//    · 有副作用的用例记录审计事件；审计失败**不影响**主流程（由实现决定 fail-open/closed）。
//
//  13 个用例与契约端点的对应
//    GetUploadLocation        §2.1/§2.2   GetFileLocation        §2.3
//    GetDownloadLocation      §2.4        GetFileList            §2.5
//    CreateFileMetadata       §2.6        GetFileMetadata        §2.7
//    DeleteFileMetadata       §2.8        GetStorageInstructions §2.9
//    GetRetrievalInstructions §2.9        CopyFiles              §2.9
//    GetFileSignedUrl         §2.10       RevokeUrl              §2.11
//    GetInfo                  §2.12
#pragma once

#include "app/services/location_issuer.h"
#include "common/ids/id_generator.h"
#include "common/result/result.h"
#include "common/time/clock.h"
#include "domain/model/file_metadata.h"
#include "domain/model/types.h"
#include "domain/ports/ports.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fss::app {

// =============================================================================
//  调用上下文与端口集合
// =============================================================================
//  `CallerContext` 是"从请求头解析出来"的最小集合（P4 组装；P2 由测试直接构造）。
struct CallerContext {
  std::string partition;      // data-partition-id（revokeURL 不要求）
  std::string user_id;        // 用于 FileSource 与 getFileList 的 UserID 过滤
  std::string bearer_token;   // "Bearer xxx" 或裸 token（由适配层归一化）
  //  请求的 `correlation-id`（适配层填充）—— 上游把它放进 `datasetDetails` 事件的
  //  `properties.correlationId`，用于跨服务追踪。非 HTTP 入口（gRPC/测试）可以留空。
  std::string correlation_id;
};

//  角色名的**短别名**：值只定义在 `domain::ports`（单一真相，C6.8），这里只做转发。
inline constexpr std::string_view kRoleAdmin = domain::kRoleFileAdmin;
inline constexpr std::string_view kRoleDatasetEditors = domain::kRoleDatasetEditors;
inline constexpr std::string_view kRoleDatasetViewers = domain::kRoleDatasetViewers;
inline constexpr std::string_view kRoleStorageCreator = domain::kRoleStorageCreator;
inline constexpr std::string_view kRoleDeliveryViewer = domain::kRoleDeliveryViewer;

//  组合根在 `src/main/` 组装后交给用例（R12：具体实现只在组合根创建）。
struct UseCasePorts {
  domain::IBlobStoreFactory& blobs;
  domain::IFileLocationRepository& locations;
  domain::IMetadataRepository& metadata;
  domain::IAuthorizer& authorizer;
  domain::IEventPublisher& events;
  domain::IAuditLogger& audit;
  domain::IPartitionRegistry& partitions;
  domain::ILegalValidator& legal;
  domain::ISchemaValidator& schema;
  LocationIssuer& issuer;
  const fss::IClock& clock;
  const fss::IIdGenerator& ids;
};

// =============================================================================
//  结果类型（领域表达；P4 负责转成 DTO）
// =============================================================================
struct FileLocationView {
  std::string driver;
  std::string location;     // 物理位置（POSIX 路径 / 对象键）
  std::string file_source;  // 对客户端可见的相对路径
};

struct DownloadLocationResult {
  std::string file_id;
  std::string signed_url;
  std::string driver;
  std::int64_t expires_at_epoch_seconds = 0;
  bool native_presign = false;
};

struct FileListEntry {
  std::string file_id;
  std::string driver;
  std::string location;
  std::int64_t created_at_epoch_seconds = 0;
  std::string created_by;
};

struct FileListResult {
  std::vector<FileListEntry> content;
  int number = 0;              // PageNum（从 0 开始）
  int number_of_elements = 0;
  int size = 0;                // 请求的 Items
  std::int64_t total = 0;
};

struct StorageInstructions {
  std::string provider_key;    // "POSIX" / "S3" / ...
  std::string signed_url;
  std::string file_source;
  std::string created_by;
  std::int64_t expires_at_epoch_seconds = 0;
};

struct RetrievalInstruction {
  std::string dataset_registry_id;
  std::string signed_url;
  std::string provider_key;
};

struct CopyFileOutcome {
  bool success = false;
  std::string dataset_blob_storage_path;
};

struct SignedUrlEntry {
  std::string signed_url;
  std::string unsigned_url;
  std::string kind;
};

struct SignedUrlResult {
  std::map<std::string, SignedUrlEntry> processed;
  std::vector<std::string> unprocessed;
};

struct VersionInfo {
  std::string version;
  std::string build_version;
  std::vector<std::string> connected_outer_services;
};

// =============================================================================
//  ① GetUploadLocation（POST/GET uploadURL / getLocation）
// =============================================================================
class GetUploadLocation {
 public:
  explicit GetUploadLocation(UseCasePorts& ports) : ports_(ports) {}
  Result<LocationResult> Execute(const CallerContext& caller,
                                 const std::optional<std::string>& requested_file_id,
                                 const std::optional<std::string>& expiry_time);

 private:
  UseCasePorts& ports_;
};

// =============================================================================
//  ② GetFileLocation（POST getFileLocation，deprecated）
// =============================================================================
class GetFileLocation {
 public:
  explicit GetFileLocation(UseCasePorts& ports) : ports_(ports) {}
  Result<FileLocationView> Execute(const CallerContext& caller, std::string_view file_id);

 private:
  UseCasePorts& ports_;
};

// =============================================================================
//  ③ GetDownloadLocation（GET files/{id}/downloadURL）
// =============================================================================
class GetDownloadLocation {
 public:
  explicit GetDownloadLocation(UseCasePorts& ports) : ports_(ports) {}
  Result<DownloadLocationResult> Execute(const CallerContext& caller, std::string_view file_id,
                                         const std::optional<std::string>& expiry_time);

 private:
  UseCasePorts& ports_;
};

// =============================================================================
//  ④ GetFileList（POST getFileList）
// =============================================================================
struct FileListRequest {
  std::string user_id;                                // 空 = 不过滤
  std::int64_t time_from_epoch_seconds = -1;          // -1 = 无界（含端点）
  std::int64_t time_to_epoch_seconds = -1;
  int page_num = 0;                                   // 从 0 开始
  //  ★ 缺省 0（**不是** 10）：上游 `FileListRequest.items` 是基本类型，缺省 0 会被 `@Positive`
  //    拒掉 —— 验收样例 `File_GetList_EmptyPayload.json`（`{}`）与
  //    `File_GetList_InvalidPayload.json`（缺 `Items`）都期望 **400**。
  //    给个"友好的默认 10"会让这两条上游样例变成 200（P6-D09）。
  int items = 0;
};

class GetFileList {
 public:
  explicit GetFileList(UseCasePorts& ports) : ports_(ports) {}
  Result<FileListResult> Execute(const CallerContext& caller, const FileListRequest& request);

 private:
  UseCasePorts& ports_;
};

// =============================================================================
//  ⑤ CreateFileMetadata（POST files/metadata）—— 契约 §2.6 的 12 步序列
// =============================================================================
class CreateFileMetadata {
 public:
  explicit CreateFileMetadata(UseCasePorts& ports) : ports_(ports) {}
  Result<std::string> Execute(const CallerContext& caller, const domain::FileMetadataRecord& record);

 private:
  UseCasePorts& ports_;
};

// =============================================================================
//  ⑥ GetFileMetadata / ⑦ DeleteFileMetadata
// =============================================================================
class GetFileMetadata {
 public:
  explicit GetFileMetadata(UseCasePorts& ports) : ports_(ports) {}
  Result<domain::FileMetadataRecord> Execute(const CallerContext& caller, std::string_view record_id);

 private:
  UseCasePorts& ports_;
};

class DeleteFileMetadata {
 public:
  explicit DeleteFileMetadata(UseCasePorts& ports) : ports_(ports) {}
  Result<void> Execute(const CallerContext& caller, std::string_view record_id);

 private:
  UseCasePorts& ports_;
};

// =============================================================================
//  ⑧ GetStorageInstructions（DMS）
// =============================================================================
class GetStorageInstructions {
 public:
  explicit GetStorageInstructions(UseCasePorts& ports) : ports_(ports) {}
  Result<StorageInstructions> Execute(const CallerContext& caller,
                                      const std::optional<std::string>& expiry_time);

 private:
  UseCasePorts& ports_;
};

// =============================================================================
//  ⑨ GetRetrievalInstructions（DMS）
// =============================================================================
class GetRetrievalInstructions {
 public:
  explicit GetRetrievalInstructions(UseCasePorts& ports) : ports_(ports) {}
  Result<std::vector<RetrievalInstruction>> Execute(
      const CallerContext& caller, const std::vector<std::string>& dataset_registry_ids,
      const std::optional<std::string>& expiry_time);

 private:
  UseCasePorts& ports_;
};

// =============================================================================
//  ⑩ CopyFiles（DMS copy）
// =============================================================================
struct CopyFileSource {
  std::string file_source;   // 客户端给的源路径
};

class CopyFiles {
 public:
  explicit CopyFiles(UseCasePorts& ports) : ports_(ports) {}
  Result<std::vector<CopyFileOutcome>> Execute(const CallerContext& caller,
                                               const std::vector<CopyFileSource>& sources);

 private:
  UseCasePorts& ports_;
};

// =============================================================================
//  ⑪ GetFileSignedUrl（delivery）
// =============================================================================
class GetFileSignedUrl {
 public:
  explicit GetFileSignedUrl(UseCasePorts& ports) : ports_(ports) {}
  Result<SignedUrlResult> Execute(const CallerContext& caller,
                                  const std::vector<std::string>& srns,
                                  const std::optional<std::string>& expiry_time);

 private:
  UseCasePorts& ports_;
};

// =============================================================================
//  ⑫ RevokeUrl（admin）—— 契约 §2.11：**不要求 partition**，恒定 204 语义
// =============================================================================
class RevokeUrl {
 public:
  explicit RevokeUrl(UseCasePorts& ports) : ports_(ports) {}
  Result<void> Execute(const CallerContext& caller);

 private:
  UseCasePorts& ports_;
};

// =============================================================================
//  ⑬ GetInfo（运维；无鉴权）
// =============================================================================
class GetInfo {
 public:
  explicit GetInfo(UseCasePorts& ports) : ports_(ports) {}
  Result<VersionInfo> Execute();

 private:
  UseCasePorts& ports_;
};

// =============================================================================
//  共享辅助（实现内部使用，也便于单测）
// =============================================================================
//  FileLocation.extra 里的物理引用（由 LocationIssuer 写入）
Result<domain::ObjectRef> ObjectRefFromLocation(const domain::FileLocation& location);
//  契约 §2.3 的 `Location`：物理位置字符串
std::string PhysicalLocationOf(const domain::FileLocation& location);
//  provider key（"POSIX"/"S3"/…）：来自记录里的真实驱动名，转大写
std::string ProviderKeyOf(const domain::FileLocation& location);

}  // namespace fss::app
