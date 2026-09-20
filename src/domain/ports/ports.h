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
#include <memory>
#include <optional>
#include <string>
#include <span>
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

//  ★ C9.25：`remove_temp_files` 的结果。
//  为什么不是"只返回删了几个"：临时文件对 `list()` 是**不可见**的（它们不是对象），
//  所以"看到但太新所以保护"这件事只有驱动自己知道。若只回删除数，POSIX 路径上
//  "在途上传被正确保护"就**不可观测**（`GcTask` 里等价的 list 分支在这条路径上永不执行），
//  运维与测试都只能确认"没删"而无法确认"看到了却没删"。
struct TempSweepResult {
  std::int64_t removed = 0;             // 已删（dry_run 下 = 将被删）
  std::int64_t skipped_too_young = 0;   // 看到但太新 → 保护（在途上传）
  std::int64_t skipped_unknown_mtime = 0;  // 看到但取不到 mtime → 保护（保守方向）
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

  //  ★ ADR-006：**可选**的零拷贝能力 —— 打开一个"持有原生 fd 的字节源"。
  //    语义与默认值：
  //      · 默认实现返回 `kUnimplemented`，表示"该驱动没有原生 fd"（内存/S3 都是如此）
  //        → 调用方**必须**回退到用户态拷贝（`get()` 或普通 `Read`）。这是**唯一**
  //        允许的探测方式：能力以"调用结果"表达，而不是让上层去问驱动类型。
  //      · POSIX 驱动覆盖它，返回一个 `NativeFd() >= 0` 的来源（`sendfile` 可直读）。
  //      · 返回的源与 `get()` 读到的是**同一个对象**，`Size`/`Seek` 语义一致。
  //    为什么放在端口上（而不是让组合根 `dynamic_cast` 到具体驱动）：R12 要求上层的
  //    能力分支只认端口；加一个 defaulted 虚函数让"没有该能力的驱动"自动得到一个
  //    明确的 `kUnimplemented`，而不是编译期分支。
  virtual Result<std::shared_ptr<bytes::ByteSource>> OpenNativeRead(const ObjectRef& ref) {
    (void)ref;
    return Err(ErrorKind::kUnimplemented,
               "该存储不支持原生 fd 读取（调用方需回退到用户态拷贝）");
  }

  virtual Result<ObjectStat> stat(const ObjectRef&) = 0;
  virtual Result<void> remove(const ObjectRef&) = 0;
  virtual Result<ObjectStat> copy(const ObjectRef& from, const ObjectRef& to) = 0;
  virtual Result<ListPage> list(const std::string& container, const std::string& prefix,
                                const std::string& continuation_token, int limit) = 0;

  //  ★ C9.25：清理**内部临时文件**（键里含 `.tmp.` 的文件）。
  //    为什么单独一个方法而不是让 `list()` 把它们列出来：
  //      `list()` 的语义是"列出**对象**"，而临时文件**永远不是**对象（不能下载、不能被
  //      位置记录引用）。把它们混进 list 会让"对象列举"的判据（getFileList、孤儿扫描）
  //      都需要额外过滤 —— 那正是"临时文件被当成有效对象"的入口。
  //    但 GC 又必须能把**残留**的临时文件清掉，所以这里给一个专用入口。
  //    参数：`older_than_epoch_seconds` 之前的临时文件才处理（保护在途上传）；
  //          `dry_run=true` 只统计不删除。
  virtual Result<TempSweepResult> remove_temp_files(const std::string& container,
                                                    std::int64_t older_than_epoch_seconds,
                                                    bool dry_run) = 0;
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

//  ★ C1（ADR-009 §4.2）：元数据记录的**写入状态机** `claiming → ready`。
//    它是**仓储列**，**不是** OSDU 记录的一部分 —— 绝不进 `ToJson` / `ParseFileMetadataRecord`，
//    否则 REST/gRPC 的线上契约就会多出一个字段（本切片的硬要求）。
//    `kDeleted` 对应 PG schema 里由外部写入的墓碑行（本仓储的 `Delete` 仍是硬删除）。
enum class MetadataState { kClaiming, kReady, kDeleted };

//  `ClaimForWrite` 的结果（ADR-009 §4.2 的"原子领取"）：
//    · `claimed == true`  → 本调用赢得领取权，刚插入/占用了 (partition, file_source) 上的
//      一条 `claiming` 行；调用方**必须**负责把它推到 `kReady`（成功后 `MarkReady`，
//      失败后 `ReleaseClaim` 释放，否则该 file_source 永远无法被重试）。
//    · `claimed == false` → 已存在活动记录：`record` 是既有那条，`state` 是它的状态。
//      调用方**不得复制**（这正是 ADR-009 M2 要消除的"两个实例各复制一遍"）。
struct MetadataClaim {
  bool claimed = false;
  FileMetadataRecord record;  // claimed=true 时是刚插入的 claiming 行；false 时是既有的活动记录
  MetadataState state = MetadataState::kReady;  // claimed=false 时既有记录的状态
};

class IMetadataRepository {
 public:
  virtual ~IMetadataRepository() = default;
  //  创建：**幂等**（同 partition + 同 FileSource 重复创建必须返回同一条记录，R5）。
  //  ★ C1：本方法按新的原语实现（claim → mark ready），可观测语义与接线前逐字一致。
  virtual Result<FileMetadataRecord> Create(std::string_view partition,
                                            const FileMetadataRecord& record) = 0;

  //  ★ C1：ADR-009 §4.2 的**原子领取**（单条 INSERT … ON CONFLICT DO NOTHING）：
  //    不存在活动记录 → 插入 `state='claiming'` 的 v1 并返回 {claimed=true}；
  //    已存在活动记录 → 返回 {claimed=false, record=既有, state=它的状态}，**不复制、不报错**。
  //    ★ 领取发生在 staging→persistent **复制之前**：并发实例里只有一个能领到，
  //      因此只有一个会去复制（这正是本切片要交付的）。
  virtual Result<MetadataClaim> ClaimForWrite(std::string_view partition,
                                             const FileMetadataRecord& record) = 0;

  //  ★ C1：`claiming → ready`，并把**最终**记录（含复制后才算出的 checksum）落库。
  //    只对 (partition, id, version) 且 `state='claiming'` 的行生效；0 行 → kNotFound。
  //    ⚠️ 记录里的 `file_source`（幂等键）必须与领取时的行**完全一致**：不一致 → kInvalidArgument
  //       且**保持 claiming**（调用方仍可 `ReleaseClaim`）—— 允许在 mark-ready 时改写幂等键
  //       会让 `ux_mr_source` 与所有按 file_source 的查找静默失配（见测试证据 §18 的判断记录）。
  //    ★ 4 参形态是父代理批准的偏离（原定 3 参）：checksum 只能在复制/回算之后才知道，
  //       必须随本调用一次性落库（单条 UPDATE），否则 claiming 行里的 data 是过期快照。
  virtual Result<FileMetadataRecord> MarkReady(std::string_view partition,
                                              std::string_view record_id,
                                              std::int64_t version,
                                              const FileMetadataRecord& record) = 0;

  //  ★ C1：放弃领取（复制/校验和/mark-ready 失败时调用）：只删**本版本**且
  //    `state='claiming'` 的行；0 行 → kNotFound。删掉后同一 file_source 可被重新领取。
  virtual Result<void> ReleaseClaim(std::string_view partition, std::string_view record_id,
                                    std::int64_t version) = 0;

  //  ★ C2（ADR-009 §4.2/§4.3）：回收"崩溃的领取者"留下的 claiming 行：只回收
  //    `file_source` 落在 `live_expired_sources` 里、且 `created_at <= older_than_epoch_seconds`
  //    的 claiming 行（最多 limit 条），返回实际回收数。
  //  ★ 为什么必须由调用方给出 file_source 集合：领取是否"已死"只有**租约**知道，
  //    而"租约已到期并被本 GC 原子领取"这个事实由 `ILeaseRepository::ClaimExpired` 提供 ——
  //    模块化地把它传进来，三个实现（内存/SQLite/PG）才能有完全一致的语义，且不需要跨表 SQL。
  //    空集合 ⇒ 一条都不回收（调用方"没有领到任何过期租约"时不得凭年龄误删活 claim）。
  virtual Result<std::int64_t> ReclaimStaleClaiming(
      std::string_view partition, std::int64_t older_than_epoch_seconds, int limit,
      const std::vector<std::string>& live_expired_sources) = 0;

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
  //  ★ 阶段 10（C10.16 续）：`partition.file.<partition>.{staging_container,
  //    persistent_container,storage_driver}`。空串 = **与接线前逐字一致**的默认命名
  //    （`<partition>-staging` / `<partition>-persistent`）与"驱动跟随顶层
  //    `storage.driver`"。非空值由组合根从配置读入；`storage_driver` 与顶层冲突时
  //    组合根**拒绝启动**（分区级驱动覆盖未交付 —— 组合根只装配一个 BlobStore）。
  std::string staging_container;
  std::string persistent_container;
  std::string storage_driver;
  json::Value extra = json::Value::object();
};

class IPartitionRegistry {
 public:
  virtual ~IPartitionRegistry() = default;
  virtual Result<PartitionConfig> Get(std::string_view partition) = 0;
  virtual Result<std::vector<PartitionConfig>> List() = 0;
};

// =============================================================================
//  OSDU 的 9 个角色名（契约 §1.3；**唯一真相**，其它地方只能引用，不得再写字面量）
// =============================================================================
//  为什么集中在这里
//    角色是字符串字面量：写错一个字母（`viewers` vs `viewer`）不会编译失败，
//    只会让线上 403。把 9 个值放在一处 + `tests/unit/test_roles.cpp` 的逐字节断言，
//    才让"拼写"变成可机械检查的东西（C6.8）。
//
//  上游一手依据（vendored 到 `/home/ll/osdu-file-upstream`，commit d7c25c2d）：
//    · 值：`FileServiceRole` / `DatasetConstants` / `StorageRole` / `DeliveryRole`
//      （`docs/01-osdu-research.md` §1.3 有实测表）
//    · 用途：各 `api/*.java` 的 `@PreAuthorize("@authorizationFilter.hasPermission(...)")`
inline constexpr std::string_view kRoleFileViewers = "service.file.viewers";
inline constexpr std::string_view kRoleFileEditors = "service.file.editors";
inline constexpr std::string_view kRoleFileAdmin = "service.file.admin";
inline constexpr std::string_view kRoleDeliveryViewer = "service.delivery.viewer";
inline constexpr std::string_view kRoleDatasetViewers = "service.dataset.viewers";
inline constexpr std::string_view kRoleDatasetEditors = "service.dataset.editors";
inline constexpr std::string_view kRoleStorageViewer = "service.storage.viewer";
inline constexpr std::string_view kRoleStorageCreator = "service.storage.creator";
inline constexpr std::string_view kRoleStorageAdmin = "service.storage.admin";

//  历史短名（保留以免改动大量调用点；**值**仍然只有上面那一份）
inline constexpr std::string_view kRoleViewers = kRoleFileViewers;
inline constexpr std::string_view kRoleEditors = kRoleFileEditors;

class IAuthorizer {
 public:
  virtual ~IAuthorizer() = default;
  //  失败语义：缺 token → kUnauthenticated（"Missing authorization token"）；
  //            缺 partition → kUnauthenticated（"Missing partitionID"）；角色不足 → kPermissionDenied
  virtual Result<void> Authorize(std::string_view required_role, std::string_view partition,
                                 std::string_view bearer_token) = 0;
  //  **任一角色即通过**（上游 `hasPermission('a','b')`，见 `FileMetadataApi` 的 DELETE、
  //  `FileDmsApi`/`FileCollectionDmsApi` 的 copy）。空集合必须报错（不能"空 = 放行"）。
  virtual Result<void> AuthorizeAny(std::span<const std::string_view> required_roles,
                                    std::string_view partition,
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
  std::string user;        // actor（来自 `x-user-id` 或 JWT 的 email/sub）
  std::string partition;
  std::string object_id;   // 受影响的对象（记录 id / file id；未知时为空）
  std::string result;      // success / failure
  std::int64_t epoch_millis = 0;
  //  ★ C8.7 要求审计含 correlation-id：跨服务追踪的关联键（缺失时为空串）
  std::string correlation_id;
  json::Value extra = json::Value::object();
};

class IAuditLogger {
 public:
  virtual ~IAuditLogger() = default;
  //  端口只表达"要记"，并如实返回写入结果（失败 → `Err`）。
  //  ★ C10.13：**是否让请求失败**是调用方（用例）的策略：`observability.audit_fail_closed=true`
  //    时用例把这个 `Err` 传播成 500（`UseCasePorts::audit_fail_closed`），
  //    `false`（默认）时审计失败非致命 —— 端口本身不做这个判定。
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
  //    这也正是计划里 payload = `{op, container, key, partition, exp, key_id, nonce}` 的原因。
  std::string container;
  std::string object_key;
  StorageZone zone = StorageZone::kStaging;
  std::string op;  // "put" / "get"
  std::int64_t expires_at_epoch_seconds = 0;
  //  ★ 阶段 10 切片 5：`self_signed.key_id` 绑定。codec 在**配置了非空 key_id** 时把它写进
  //    被签名的载荷；解码侧要求载荷里的 `key_id` 与当前配置**完全相等**（缺失也拒绝，
  //    fail-closed）。字段放在**末尾**以免破坏既有聚合初始化。
  //    ⚠️ **多密钥轮换未交付**（ADR-009:227 的"多 key 并存"仍未做）：这里只是"把 id 绑进
  //    签名载荷并在解码侧拒绝不匹配"，不存在"按 id 选密钥"的能力。
  std::string key_id;
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
