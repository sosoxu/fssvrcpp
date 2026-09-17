// =============================================================================
//  端口清单（L3，`src/domain/ports/`）—— 共 15 个
// =============================================================================
//  端口只在"存在多个实现"或"必须在测试中替换"时才引入（docs/02-design.md §5.2）。
//
//  ⓛ 其中两个**声明在 L1**（不在本文件）：`fss::IClock`（`common/time/clock.h`）与
//     `fss::IIdGenerator`（`common/ids/id_generator.h`）。理由：它们零依赖、无领域语义，
//     且 L1 自身测试也要用；放 L3 会让 L2 实现为一个无领域语义的接口反向依赖 L3。
//
//  因此"15 个端口" = 本文件的 13 个 + L1 的 2 个。
//  `tests/unit/test_ports.cpp` 会**机械地**核对这份清单（C2.11）。
//
//  规则
//    · 端口不得 include grpc/sqlite3/curl/openssl/httplib（编译期 + 源码护栏）
//    · 所有可能失败的操作用 `Result<T>`；**不抛异常**（异常只允许出现在适配层边界）
//    · 时间一律来自 `IClock`，ID 一律来自 `IIdGenerator`（可注入 → 可确定性测试）
#pragma once

#include "common/bytes/bytes.h"
#include "common/json/json.h"
#include "common/result/result.h"
#include "domain/model/file_metadata.h"
#include "domain/model/types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fss::domain {

// =============================================================================
//  一、存储（ADR-003）
// =============================================================================
struct BlobCapabilities {
  bool native_presign = false;   // 有原生预签名 URL（S3: true / POSIX: false）
  bool server_side_copy = false;  // 支持服务端复制
  bool range_read = false;        // 支持字节区间读取
  bool streaming_put = false;     // 支持流式写入
  std::size_t recommended_part_size = 0;
  std::string driver_name;  // 回填到响应的 Driver 字段（"posix" / "s3"）
};

struct PresignOptions {
  std::int64_t expires_in_seconds = 3600;
  std::string content_type;
  std::string file_name;  // 仅用于 Content-Disposition
  std::string method;     // "PUT" / "GET"
};

struct PutOptions {
  std::string content_type;
  std::string expected_checksum;
  std::string checksum_algorithm;
  std::int64_t expected_size = -1;  // -1 = 未知
};

class IBlobStore {
 public:
  virtual ~IBlobStore() = default;
  virtual BlobCapabilities capabilities() const = 0;
  virtual Result<void> ensure_container(const std::string& container) = 0;
  virtual Result<SignedLocation> presign_put(const ObjectRef&, const PresignOptions&) = 0;
  virtual Result<SignedLocation> presign_get(const ObjectRef&, const PresignOptions&) = 0;
  virtual Result<void> put(const ObjectRef&, bytes::ByteSource&, const PutOptions&) = 0;
  virtual Result<void> get(const ObjectRef&, bytes::ByteSink&, const ByteRange&) = 0;
  virtual Result<ObjectStat> stat(const ObjectRef&) = 0;
  virtual Result<void> remove(const ObjectRef&) = 0;
  virtual Result<ObjectStat> copy(const ObjectRef& from, const ObjectRef& to) = 0;
  virtual Result<ListPage> list(const std::string& container, const std::string& prefix,
                                const std::string& continuation_token, int limit) = 0;
};

// 按 partition 解析存储实例（多租户的关键接缝）
class IBlobStoreFactory {
 public:
  virtual ~IBlobStoreFactory() = default;
  virtual Result<IBlobStore*> ForPartition(std::string_view partition, StorageZone zone) = 0;
};

// =============================================================================
//  二、仓储（ADR-004 / ADR-009）
// =============================================================================
//  ★ 每个方法都带 `partition`：**分区隔离是契约级的**，不允许"按 id 全局查"。
//
//  `LocationQuery` 支撑 `getFileList`（契约 §2.5）：时间区间 + 用户 + 分页。
//  语义（由 tests/framework/port_contract.h 钉住）：
//    · 时间边界**含端点**；`-1` 表示无界
//    · `user_id` 为空串 = 不按用户过滤
//    · `limit <= 0` / `offset < 0` → `kInvalidArgument`
//    · 只返回本 partition 的记录；`total` 是**过滤后、分页前**的总数
//    · 排序必须是稳定全序：`created_at` 升序，同秒按 `file_id` 升序
//      （否则 offset 分页会漏项/重项）
struct LocationQuery {
  std::string user_id;
  std::int64_t created_after_epoch_seconds = -1;
  std::int64_t created_before_epoch_seconds = -1;
  int limit = 10;
  int offset = 0;
};

struct LocationPage {
  std::vector<FileLocation> records;
  std::int64_t total = 0;
};

class IFileLocationRepository {
 public:
  virtual ~IFileLocationRepository() = default;
  virtual Result<void> Save(std::string_view partition, const FileLocation& location) = 0;
  virtual Result<FileLocation> Find(std::string_view partition, std::string_view file_id) = 0;
  virtual Result<void> UpdateSignedUrl(std::string_view partition, std::string_view file_id,
                                       std::string_view signed_url,
                                       std::int64_t updated_at_epoch_seconds) = 0;
  virtual Result<void> Delete(std::string_view partition, std::string_view file_id) = 0;
  //  幂等键：`(partition, file_source)` 唯一（R5：约束必须建在幂等键上）
  virtual Result<FileLocation> FindByFileSource(std::string_view partition,
                                                std::string_view file_source) = 0;
  //  `getFileList` 的查询（阶段 3 计划的 `FindAll`，此处定名为 `List` 与其它仓储一致）
  virtual Result<LocationPage> List(std::string_view partition,
                                    const LocationQuery& query) = 0;
};

struct MetadataQuery {
  std::optional<std::string> kind;
  std::optional<std::string> name_prefix;
  std::int64_t created_after_epoch_seconds = -1;
  std::int64_t created_before_epoch_seconds = -1;
  int limit = 100;
  int offset = 0;
};

struct MetadataPage {
  std::vector<FileMetadataRecord> records;
  std::int64_t total = 0;
};

class IMetadataRepository {
 public:
  virtual ~IMetadataRepository() = default;
  //  创建：**幂等**（同 partition + 同 FileSource 重复创建必须返回同一条记录，R5）
  virtual Result<FileMetadataRecord> Create(std::string_view partition,
                                            const FileMetadataRecord& record) = 0;
  virtual Result<FileMetadataRecord> GetById(std::string_view partition,
                                             std::string_view record_id) = 0;
  //  拿最新版本（`is_latest` 语义，R6：部分唯一索引的谓词要含业务语义）
  virtual Result<FileMetadataRecord> GetLatestByFileSource(std::string_view partition,
                                                           std::string_view file_source) = 0;
  virtual Result<FileMetadataRecord> Update(std::string_view partition,
                                            const FileMetadataRecord& record) = 0;
  virtual Result<void> Delete(std::string_view partition, std::string_view record_id) = 0;
  virtual Result<MetadataPage> List(std::string_view partition, const MetadataQuery& query) = 0;
};

// 在途租约（ADR-009：GC 只能删除"租约到期且被原子领取"的对象）
class ILeaseRepository {
 public:
  struct Lease {
    std::string file_id;
    std::string owner_instance_id;
    std::string file_source;
    std::int64_t expires_at_epoch_millis = 0;
  };

  virtual ~ILeaseRepository() = default;
  virtual Result<Lease> Acquire(std::string_view partition, std::string_view file_id,
                                std::string_view owner_instance_id, std::int64_t ttl_millis) = 0;
  virtual Result<void> Renew(std::string_view partition, std::string_view file_id,
                             std::string_view owner_instance_id, std::int64_t ttl_millis) = 0;
  virtual Result<void> Release(std::string_view partition, std::string_view file_id,
                               std::string_view owner_instance_id) = 0;
  //  ★ 原子领取（`FOR UPDATE SKIP LOCKED` / `ON CONFLICT`）：并发实例下每一条只能被一个实例领走
  virtual Result<std::vector<Lease>> ClaimExpired(std::string_view partition, int limit,
                                                  std::string_view claimant_instance_id) = 0;
};

// =============================================================================
//  三、租户与授权
// =============================================================================
struct PartitionConfig {
  std::string partition;
  StorageDriver driver = StorageDriver::kPosix;
  std::string posix_root;         // POSIX 根（按 partition 分盘）
  std::string s3_bucket_prefix;   // S3 bucket 前缀
  std::string s3_endpoint;
  std::string s3_region;
  std::int64_t max_object_bytes = -1;    // -1 = 不限
  std::int64_t quota_bytes = -1;         // -1 = 不限
  bool legal_tag_validation = true;
  bool schema_validation = false;
  json::Value extra = json::Value::object();
};

class IPartitionRegistry {
 public:
  virtual ~IPartitionRegistry() = default;
  virtual Result<PartitionConfig> Get(std::string_view partition) = 0;
  virtual Result<std::vector<PartitionConfig>> List() = 0;
};

// OSDU 的角色名（契约 §1.3）
inline constexpr std::string_view kRoleViewers = "service.file.viewers";
inline constexpr std::string_view kRoleEditors = "service.file.editors";

class IAuthorizer {
 public:
  virtual ~IAuthorizer() = default;
  //  失败语义：缺 token → kUnauthenticated（"Missing authorization token"）；
  //            缺 partition → kUnauthenticated（"Missing partitionID"）；角色不足 → kPermissionDenied
  virtual Result<void> Authorize(std::string_view required_role, std::string_view partition,
                                 std::string_view bearer_token) = 0;
};

class ILegalValidator {
 public:
  virtual ~ILegalValidator() = default;
  virtual Result<void> Validate(std::string_view partition,
                                const std::vector<std::string>& legal_tags) = 0;
};

class ISchemaValidator {
 public:
  virtual ~ISchemaValidator() = default;
  virtual Result<void> Validate(std::string_view kind, const json::Value& record) = 0;
};

// =============================================================================
//  四、可观测：事件与审计
// =============================================================================
struct StatusChangedEvent {
  std::string record_id;
  std::string partition;
  std::string status;         // IN_PROGRESS / SUCCESS / FAILED
  std::string dataset_sync;   // DATASET_SYNC
  std::int64_t version = 0;
  json::Value extra = json::Value::object();
};

//  契约 §2.6 第 10 步的**第二个**事件。上游形状（一手依据：
//  `/home/ll/osdu-file-upstream/.../status/FileDatasetDetailsPublisher.java`）：
//    kind = "datasetDetails"，body 是**长度为 1 的数组**，元素 properties 含
//    `correlationId` / `datasetId`（= 记录 id）/ `datasetType` = `FILE` /
//    `datasetVersionId`（= 记录版本）/ `recordCount` = 1 / `timestamp`（毫秒）。
//  ★ 与 status 事件一样是**非致命**的：上游只 `log.warning("Failed to publish dataset details")`。
struct DatasetDetailsEvent {
  static constexpr std::string_view kKind = "datasetDetails";
  static constexpr std::string_view kDatasetTypeFile = "FILE";

  std::string partition;
  std::string correlation_id;    // 来自 `correlation-id` 头（缺失时为空）
  std::string dataset_id;        // 记录 id
  std::string dataset_version_id;  // 记录版本（字符串形态，与上游一致）
  std::string dataset_type = std::string(kDatasetTypeFile);
  int record_count = 1;
  std::int64_t timestamp_millis = 0;
};

class IEventPublisher {
 public:
  virtual ~IEventPublisher() = default;
  virtual Result<void> PublishStatusChanged(std::string_view topic,
                                            const StatusChangedEvent& event) = 0;
  //  `datasetDetails`（索引服务据此更新数据集清单）
  virtual Result<void> PublishDatasetDetails(std::string_view topic,
                                             const DatasetDetailsEvent& event) = 0;
};

struct AuditEvent {
  std::string operation;   // 对齐上游 `AuditOperation`（createLocationSuccess 等）
  std::string user;
  std::string partition;
  std::string object_id;
  std::string result;      // success / failure
  std::int64_t epoch_millis = 0;
  json::Value extra = json::Value::object();
};

class IAuditLogger {
 public:
  virtual ~IAuditLogger() = default;
  //  ★ 审计失败是否影响主流程由实现决定（`audit_fail_closed` 配置）；端口只表达"要记"
  virtual Result<void> Record(const AuditEvent& event) = 0;
};

// =============================================================================
//  五、自签 URL（集中存储的数据面入口）
// =============================================================================
struct TransferToken {
  std::string partition;
  std::string file_id;
  //  ★ 物理容器必须进 token：`/v1/transfer` 内核是 L2，**不能**依赖 L4 的
  //    `ObjectKeyPolicy::ContainerFor` 去反推容器名（否则 L2→L4 越层）。
  //    这也正是计划里 payload = `{op, container, key, partition, exp, nonce}` 的原因。
  std::string container;
  std::string object_key;
  StorageZone zone = StorageZone::kStaging;
  std::string op;  // "put" / "get"
  std::int64_t expires_at_epoch_seconds = 0;
};

class ISelfSignedUrlCodec {
 public:
  virtual ~ISelfSignedUrlCodec() = default;
  //  产出形如 `.../v1/transfer/{token}?exp=...&sig=...` 的 URL（HMAC 签名 + 过期）
  virtual Result<std::string> Encode(const TransferToken& token, std::string_view base_url) = 0;
  //  校验签名与过期：失败 → kUnauthenticated（签名错）/ kPermissionDenied（越权）/ kNotFound
  virtual Result<TransferToken> Decode(std::string_view token, std::string_view expires,
                                       std::string_view signature) = 0;
};

// =============================================================================
//  六、文件 I/O 引擎（ADR-010）
// =============================================================================
//  ★ 必须支持 **64 位偏移 + 区间读**（> 2 GiB 的文件；`ByteRange` 已是 uint64）。
struct IoEngineCapabilities {
  bool async = false;          // io_uring 等真异步
  bool direct_io = false;      // O_DIRECT
  std::size_t alignment = 0;   // O_DIRECT 所需对齐（0 = 无要求）
  std::string engine_name;     // "blocking" / "uring"
};

class IIoEngine {
 public:
  virtual ~IIoEngine() = default;
  virtual IoEngineCapabilities capabilities() const = 0;

  //  定位读：`offset` 为 **64 位**；`buffer`/`length` 由调用方提供（避免隐藏分配）
  virtual Result<std::size_t> ReadAt(int fd, std::uint64_t offset, char* buffer,
                                     std::size_t length) = 0;
  //  定位写：同样是 64 位偏移；支持"写满"（返回实际写入字节数）
  virtual Result<std::size_t> WriteAt(int fd, std::uint64_t offset, const char* buffer,
                                      std::size_t length) = 0;
  virtual Result<void> Sync(int fd) = 0;
  virtual Result<void> SyncFilesystem(int fd) = 0;  // syncfs（两阶段批提交的第二阶段，ADR-008）
  virtual Result<std::uint64_t> FileSize(int fd) = 0;
};

}  // namespace fss::domain
