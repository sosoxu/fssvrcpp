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
#include "common/bytes/bytes.h"
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
  //  运维响应需要的**只读**运行时配置（`/v2/info` 的 `authMode`，C8.5）。
  //  放在端口集合里而不是各适配层，是为了让 REST 与 gRPC 返回**同一个**值。
  std::string auth_mode = "disabled";
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
  //  ★ 上游的下载位置是 `AzureFileDmsDownloadLocation{signedUrl, fileSource, createdBy,
  //    expiryTime}`（`StorageServiceImpl.java:268`）—— 只回 `signedUrl` 会少三个键（C6.7）。
  std::string file_source;
  std::string created_by;
  std::int64_t expires_at_epoch_seconds = 0;
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
  //  ★ C8.5：`auth.mode` 必须**可见** —— "忘了开鉴权"不能是静默状态
  std::string auth_mode;  // "jwt" / "remote-entitlements" / "disabled"
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
//  ⑭⑮⑯ 扩展 RPC 的应用层用例（P7 切片 3）
// =============================================================================
//  这 3 个是 gRPC 的**平台外扩展**（ADR-001：REST 是唯一合规面）。它们没有 REST
//  端点，但**业务规则仍然住在 L4**（授权、位置记录解析、存储副作用、审计、以及
//  `register_metadata` 时复用同一个 `CreateFileMetadata` 12 步用例）。
//
//  与 REST 数据面的一致性（C7.3 的等价性依据）：
//    · `UploadFile`    == `PUT /v1/transfer/{自签 token}`（同一批字节、同一个对象键）
//    · `DownloadFile`  == `GET /v1/transfer/{自签 token}` + `Range`（同一区间语义）
//    · `ServerSideCopy`== `/v2/files/copy` 的**物理复制**部分（不写元数据记录）
//  适配层只做"proto 分片 ↔ ByteSource/ByteSink"的翻译，不做任何判定。
struct UploadStreamRequest {
  std::string file_source;  // 必填：由 GetUploadLocation 返回（位置记录是授权的锚点）
  std::optional<std::string> container;  // 可选：显式坐标，必须与位置记录**一致**（防参数覆盖）
  std::optional<std::string> key;
  std::string content_type;
  std::string expected_checksum;   // 非空时由驱动校验（不符 → kChecksumMismatch）
  std::string checksum_algorithm;
  bool register_metadata = false;  // true 时上传完成后调用 CreateFileMetadata
  std::optional<domain::FileMetadataRecord> metadata;
};

struct UploadStreamResult {
  std::string file_id;
  std::string file_source;
  std::string checksum;            // 存储侧算出的校验和（POSIX/memory 为 SHA-256）
  std::string checksum_algorithm;
  std::uint64_t bytes_written = 0;
  std::string metadata_record_id;  // register_metadata=false 时为空
};

class UploadFile {
 public:
  explicit UploadFile(UseCasePorts& ports) : ports_(ports) {}
  Result<UploadStreamResult> Execute(const CallerContext& caller,
                                     const UploadStreamRequest& request,
                                     bytes::ByteSource& body);

 private:
  UseCasePorts& ports_;
};

//  `length == 0` 表示"从 offset 到对象末尾"（proto3 没有 presence，无法区分 0 与缺省）
struct DownloadStreamResult {
  std::int64_t total_size = 0;   // 对象的完整大小（不是本次区间的大小）
  std::string checksum;
  std::string checksum_algorithm;
  std::uint64_t bytes_written = 0;  // 本次区间实际写出的字节数
};

class DownloadFile {
 public:
  explicit DownloadFile(UseCasePorts& ports) : ports_(ports) {}
  Result<DownloadStreamResult> Execute(const CallerContext& caller, std::string_view file_id,
                                       std::string_view file_source, std::uint64_t offset,
                                       std::uint64_t length, bytes::ByteSink& sink);

 private:
  UseCasePorts& ports_;
};

struct ServerSideCopyResult {
  std::string file_source;         // 回显目标逻辑路径
  std::uint64_t bytes_copied = 0;
};

//  纯**字节**原语：把 `source_file_source` 指向的对象复制到 `target_file_source`
//  在 `target_zone` 下的对象键。**不改动任何位置/元数据记录**（记录迁移属于
//  `CreateFileMetadata`）；目标已存在则覆盖（幂等）。
class ServerSideCopy {
 public:
  explicit ServerSideCopy(UseCasePorts& ports) : ports_(ports) {}
  Result<ServerSideCopyResult> Execute(const CallerContext& caller,
                                       std::string_view source_file_source,
                                       std::string_view target_file_source,
                                       domain::StorageZone target_zone);

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
