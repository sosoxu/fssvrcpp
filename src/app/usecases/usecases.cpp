// 13 个应用层用例的实现。设计约束与端点对应见 usecases.h。
#include "app/usecases/usecases.h"

#include "app/services/expiry_policy.h"
#include "app/services/kind_validator.h"
#include "app/services/object_key_policy.h"
#include "common/bytes/bytes.h"
#include "common/crypto/crypto.h"
#include "domain/model/file_metadata.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

namespace fss::app {

namespace {

constexpr std::string_view kExtraContainer = LocationIssuer::kExtraContainer;
constexpr std::string_view kExtraObjectKey = LocationIssuer::kExtraObjectKey;
constexpr std::string_view kExtraDriverName = LocationIssuer::kExtraDriverName;

std::string ToUpper(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string ToLower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

fss::Error Invalid(const std::string& message) {
  return Err(fss::ErrorKind::kInvalidArgument, message);
}

//  用例入口的统一鉴权：缺 token / 缺 partition / 角色不足（契约 §1.2/§1.3/§5）
fss::Result<void> AuthorizeCaller(UseCasePorts& ports, const CallerContext& caller,
                                  std::string_view role, bool require_partition) {
  if (require_partition && caller.partition.empty()) {
    return Err(fss::ErrorKind::kUnauthenticated, "Missing partitionID");
  }
  return ports.authorizer.Authorize(role, caller.partition, caller.bearer_token);
}

//  "任一角色即通过"（上游 `hasPermission('a','b')`）。★ 空集合必须报错：
//  "没有要求任何角色"如果被当成"放行"，就是一个静默的鉴权后门。
fss::Result<void> AuthorizeCallerAny(UseCasePorts& ports, const CallerContext& caller,
                                     std::span<const std::string_view> roles,
                                     bool require_partition) {
  if (require_partition && caller.partition.empty()) {
    return Err(fss::ErrorKind::kUnauthenticated, "Missing partitionID");
  }
  if (roles.empty()) {
    return Err(fss::ErrorKind::kInternal, "AuthorizeCallerAny 要求至少一个角色");
  }
  return ports.authorizer.AuthorizeAny(roles, caller.partition, caller.bearer_token);
}

//  审计失败**不影响**主流程（是否 fail-closed 由实现决定，端口只表达"要记"）
void RecordAudit(UseCasePorts& ports, std::string_view operation, const CallerContext& caller,
                 std::string_view object_id, bool success) {
  domain::AuditEvent event;
  event.operation = std::string(operation);
  event.user = caller.user_id;
  event.partition = caller.partition;
  event.object_id = std::string(object_id);
  event.result = success ? "success" : "failure";
  event.epoch_millis = ports.clock.NowEpochMillis();
  event.correlation_id = caller.correlation_id;
  (void)ports.audit.Record(event);
}

//  审计守卫（C8.7）：**每个受保护端点在成功与失败两侧都要有审计记录**。
//  为什么用 RAII 而不是在每个 return 前手写一次：一次 Execute 里有 4~8 个失败出口
//  （授权、解析、仓储、存储、事件…），手写必然漏掉某一条 —— 而"漏审计"是**静默**的。
//  守卫在析构时按 `Success()` 是否被调用决定 success/failure，任何 `FSS_TRY` 提前返回
//  都会走到它。`SetObjectId` 用于"对象 id 在过程中才知道"的用例（如创建类端点）。
class AuditGuard {
 public:
  AuditGuard(UseCasePorts& ports, const CallerContext& caller, std::string_view operation,
             std::string object_id = {})
      : ports_(ports),
        caller_(caller),
        operation_(operation),
        object_id_(std::move(object_id)) {}
  //  ★ 操作名按上游 `AuditOperation` 的约定带结果后缀（createLocationSuccess / Failure）：
  //    调用点只写"业务动作"，避免每个 endpoint 各写两遍名字（写错一个就少一条审计）
  ~AuditGuard() {
    RecordAudit(ports_, operation_ + (success_ ? "Success" : "Failure"), caller_, object_id_,
                success_);
  }

  void SetObjectId(std::string object_id) { object_id_ = std::move(object_id); }
  void Success() { success_ = true; }

  AuditGuard(const AuditGuard&) = delete;
  AuditGuard& operator=(const AuditGuard&) = delete;

 private:
  UseCasePorts& ports_;
  const CallerContext& caller_;
  std::string operation_;
  std::string object_id_;
  bool success_ = false;
};

//  状态变更事件（契约 §2.6 的第 1/10/12 步）：**非致命**
void PublishStatus(UseCasePorts& ports, const CallerContext& caller, std::string_view status,
                   std::int64_t version) {
  domain::StatusChangedEvent event;
  event.partition = caller.partition;
  event.status = std::string(status);
  event.dataset_sync = "DATASET_SYNC";
  event.version = version;
  (void)ports.events.PublishStatusChanged("status-changed", event);
}

//  `datasetDetails` 事件（契约 §2.6 第 10 步）—— **非致命**，与 `status` 事件同策略。
//  上游依据：`FileDatasetDetailsPublisher.publishDatasetDetails(datasetId, datasetVersionId)`
//  （`FileMetadataService` 在 `publishSuccessStatus` 之后立刻调用它）。
void PublishDatasetDetails(UseCasePorts& ports, const CallerContext& caller,
                           std::string_view record_id, std::int64_t version) {
  domain::DatasetDetailsEvent event;
  event.partition = caller.partition;
  event.correlation_id = caller.correlation_id;
  event.dataset_id = std::string(record_id);
  event.dataset_version_id = std::to_string(version);
  event.timestamp_millis = ports.clock.NowEpochMillis();
  (void)ports.events.PublishDatasetDetails("datasetDetails", event);
}

//  跨 zone 复制：同一 store 用服务端 copy；不同 store（由不同后端承载 zone）走 get→put 兜底。
//  ⚠️ 兜底会把对象整体放进内存（P2 够用）；P3 用真正的流式管道替换（大文件门槛 C1.3）。
//  把 `IBlobStore` 的对象包成**可定位读**的 `ByteSource`：一次只读一块，
//  因此"跨 store 复制"与"算校验和"都不会把整个对象读进内存（C6.9）。
//  ★ 这里刻意不依赖 L2 的 `BlobByteSource`：那是给数据面用的实现，L4 只依赖端口。
class StoreByteSource final : public bytes::ByteSource {
 public:
  StoreByteSource(domain::IBlobStore& store, domain::ObjectRef ref, std::int64_t size)
      : store_(store), ref_(std::move(ref)), size_(size) {}

  fss::Result<std::size_t> Read(char* out, std::size_t capacity) override {
    if (out == nullptr || capacity == 0 || offset_ >= static_cast<std::uint64_t>(size_)) {
      return std::size_t{0};
    }
    //  ★ 每次只请求一段：固定缓冲，RSS 与对象大小无关
    const std::uint64_t want = std::min<std::uint64_t>(capacity,
                                                      static_cast<std::uint64_t>(size_) - offset_);
    bytes::BufferSink sink(out, static_cast<std::size_t>(want));
    FSS_TRY(store_.get(ref_, sink, domain::ByteRange{offset_, want}));
    offset_ += sink.written();
    return sink.written();
  }

  std::optional<std::int64_t> Size() const override { return size_; }

 private:
  domain::IBlobStore& store_;
  domain::ObjectRef ref_;
  std::int64_t size_ = 0;
  std::uint64_t offset_ = 0;
};

//  边收字节边算校验和的 sink（复制/回读两条路径共用）
class HashingSink final : public bytes::ByteSink {
 public:
  explicit HashingSink(crypto::Hasher& hasher) : hasher_(hasher) {}
  fss::Result<void> Write(std::string_view data) override {
    hasher_.Update(data);
    ++chunks_;
    return Ok();
  }

 private:
  crypto::Hasher& hasher_;
  std::size_t chunks_ = 0;
};

//  计数装饰器：把写出的字节数透传给内层 sink（`CountingSink` 是独立实现，不能包别人）
class CountingProxySink final : public bytes::ByteSink {
 public:
  explicit CountingProxySink(bytes::ByteSink& inner) : inner_(inner) {}
  fss::Result<void> Write(std::string_view data) override {
    FSS_TRY(inner_.Write(data));
    written_ += data.size();
    return Ok();
  }
  fss::Result<void> Close() override { return inner_.Close(); }
  std::uint64_t bytes_written() const { return written_; }

 private:
  bytes::ByteSink& inner_;
  std::uint64_t written_ = 0;
};

//  流式计算某个对象的校验和（**不把对象读进内存**；C6.9 的 RSS 要求）
fss::Result<std::string> ComputeChecksumStreaming(UseCasePorts& ports, std::string_view partition,
                                                 const domain::ObjectRef& ref,
                                                 domain::StorageZone zone,
                                                 crypto::ChecksumAlgorithm algorithm) {
  FSS_TRY(store, ports.blobs.ForPartition(partition, zone));
  crypto::Hasher hasher(algorithm);
  HashingSink sink(hasher);
  FSS_TRY(store->get(ref, sink, domain::ByteRange{}));
  return hasher.HexDigest();
}

fss::Result<domain::ObjectStat> CopyBetweenZones(UseCasePorts& ports, std::string_view partition,
                                                 const domain::ObjectRef& from,
                                                 const domain::ObjectRef& to,
                                                 domain::StorageZone from_zone,
                                                 domain::StorageZone to_zone) {
  FSS_TRY(from_store, ports.blobs.ForPartition(partition, from_zone));
  FSS_TRY(to_store, ports.blobs.ForPartition(partition, to_zone));
  FSS_TRY(to_store->ensure_container(to.container));

  if (from_store == to_store) {
    return to_store->copy(from, to);
  }
  //  ★ 跨 store 复制必须**流式**：曾经把整个对象读进 `StringSink` 再交给 `put`，
  //    1 GiB 对象的 RSS 会直接抬高 1 GiB（C6.9）。现在用可定位读的 ByteSource 边读边写。
  FSS_TRY(stat, from_store->stat(from));
  StoreByteSource source(*from_store, from, stat.size);
  FSS_TRY(to_store->put(to, source, domain::PutOptions{}));
  return to_store->stat(to);
}

//  第 12 步：任一步 6/7/9 失败 → **回滚删除**已搬迁的 persistent 对象 + publish FAILED + 审计。
//  ★ 三处必须**完全一致**：只报错不删会留下"元数据没有、对象却在"的孤儿（会被 GC 当成在途对象）；
//    不回滚就会在客户端重试时留下一堆无主副本。
void RollbackCreatedObject(UseCasePorts& ports, const CallerContext& caller,
                           const domain::ObjectRef& to_ref, std::string_view file_source,
                           std::string_view record_id) {
  //  ★★ 只有确认"没有任何记录指向这个对象"时才删。并发实例可能已经把记录写成功 ——
  //     此时 `to_ref` 是**对方的活对象**，删掉它就是数据丢失（P6-D13 的第二种形态：
  //     第一种是"把 persistent 对象当 staging 清理"）。
  bool owned_by_record = false;
  if (auto existing = ports.metadata.GetLatestByFileSource(caller.partition, file_source);
      existing.ok()) {
    owned_by_record = true;
  }
  if (!owned_by_record) {
    if (auto to_store = ports.blobs.ForPartition(caller.partition, domain::StorageZone::kPersistent);
        to_store.ok()) {
      (void)to_store.value()->remove(to_ref);
    }
  }
  PublishStatus(ports, caller, "FAILED", 0);
  RecordAudit(ports, "createMetadataFailure", caller, record_id, false);
}

//  幂等命中：同 (partition, FileSource) 已有记录 → 发布成功事件并返回**既有记录**的 id。
//  ★ 这是"客户端重试/并发实例"的正确语义：既不重复复制，也不会因为 staging 已被
//    上一个成功请求清理掉而报错（ADR-009 M2）。
fss::Result<std::string> ReturnExistingRecord(UseCasePorts& ports, const CallerContext& caller,
                                              const domain::FileMetadataRecord& existing) {
  PublishStatus(ports, caller, "SUCCESS", existing.version);
  PublishDatasetDetails(ports, caller, existing.id, existing.version);
  RecordAudit(ports, "createMetadataSuccess", caller, existing.id, true);
  return existing.id;
}

//  读取对象内容（用于计算校验和；仅在存储没有给出校验和时调用）

bool LooksLikeFileSource(std::string_view value) {
  return !value.empty() && value.front() == '/' && value.find("..") == std::string_view::npos;
}

}  // namespace

// =============================================================================
//  共享辅助
// =============================================================================
fss::Result<domain::ObjectRef> ObjectRefFromLocation(const domain::FileLocation& location) {
  if (!location.extra.is_object() || !location.extra.contains(std::string(kExtraContainer)) ||
      !location.extra.contains(std::string(kExtraObjectKey))) {
    return Err(fss::ErrorKind::kInternal,
               "位置记录缺少物理引用（extra.container / extra.object_key）");
  }
  domain::ObjectRef ref;
  ref.container = location.extra[std::string(kExtraContainer)].get<std::string>();
  ref.key = location.extra[std::string(kExtraObjectKey)].get<std::string>();
  return ref;
}

std::string PhysicalLocationOf(const domain::FileLocation& location) {
  const auto ref = ObjectRefFromLocation(location);
  if (!ref.ok()) return location.file_source;
  return ref.value().container + "/" + ref.value().key;
}

std::string ProviderKeyOf(const domain::FileLocation& location) {
  if (location.extra.is_object() && location.extra.contains(std::string(kExtraDriverName))) {
    return ToUpper(location.extra[std::string(kExtraDriverName)].get<std::string>());
  }
  return ToUpper(domain::StorageDriverName(location.driver));
}

// =============================================================================
//  ① GetUploadLocation
// =============================================================================
fss::Result<LocationResult> GetUploadLocation::Execute(
    const CallerContext& caller, const std::optional<std::string>& requested_file_id,
    const std::optional<std::string>& expiry_time) {
  AuditGuard audit(ports_, caller, "createLocation", requested_file_id.value_or(""));
  FSS_TRY(AuthorizeCaller(ports_, caller, domain::kRoleEditors, /*require_partition=*/true));

  auto result = ports_.issuer.IssueUploadLocation(caller.partition, caller.user_id,
                                                  requested_file_id, expiry_time);
  if (!result.ok()) {
    return result.error();  // 失败路径由守卫记 `createLocationFailure`
  }
  audit.SetObjectId(result.value().file_id);
  audit.Success();
  return result;
}

// =============================================================================
//  ② GetFileLocation
// =============================================================================
fss::Result<FileLocationView> GetFileLocation::Execute(const CallerContext& caller,
                                                       std::string_view file_id) {
  AuditGuard audit(ports_, caller, "readFileLocation", std::string(file_id));
  FSS_TRY(AuthorizeCaller(ports_, caller, domain::kRoleEditors, true));
  FSS_TRY(location, ports_.locations.Find(caller.partition, file_id));

  FileLocationView view;
  view.driver = ProviderKeyOf(location);
  //  契约 §2.3 的 `Driver` 是小写（"posix"），providerKey 是大写；这里返回小写形态
  std::transform(view.driver.begin(), view.driver.end(), view.driver.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  view.location = PhysicalLocationOf(location);
  view.file_source = location.file_source;
  audit.Success();
  return view;
}

// =============================================================================
//  ③ GetDownloadLocation
// =============================================================================
fss::Result<DownloadLocationResult> GetDownloadLocation::Execute(
    const CallerContext& caller, std::string_view file_id,
    const std::optional<std::string>& expiry_time) {
  AuditGuard audit(ports_, caller, "createDownloadLocation", std::string(file_id));
  FSS_TRY(AuthorizeCaller(ports_, caller, domain::kRoleViewers, true));
  FSS_TRY(result, ports_.issuer.IssueDownloadLocation(caller.partition, file_id, expiry_time));

  DownloadLocationResult out;
  out.file_id = result.file_id;
  out.signed_url = result.signed_url;
  out.driver = result.driver;
  out.expires_at_epoch_seconds = result.expires_at_epoch_seconds;
  out.native_presign = result.native_presign;
  audit.Success();
  return out;
}

// =============================================================================
//  ④ GetFileList
// =============================================================================
fss::Result<FileListResult> GetFileList::Execute(const CallerContext& caller,
                                                 const FileListRequest& request) {
  AuditGuard audit(ports_, caller, "getFileList");
  FSS_TRY(AuthorizeCaller(ports_, caller, domain::kRoleEditors, true));
  if (request.items <= 0) return Invalid("Items 必须 > 0");
  if (request.page_num < 0) return Invalid("PageNum 必须 >= 0");
  //  上游 `CommonFileListRequestValidator`：`TimeFrom` 必须早于 `TimeTo`
  //  （`ValidationServiceTest` 期望消息 "should be before TimeTo"）。★ 这条必须在**查询之前**
  //  报错：否则"区间反了"会被"区间内没有记录"掩盖，客户端拿到的是误导性的 No record found。
  if (request.time_from_epoch_seconds >= 0 && request.time_to_epoch_seconds >= 0 &&
      request.time_from_epoch_seconds > request.time_to_epoch_seconds) {
    return Invalid("TimeFrom should be before TimeTo");
  }

  domain::LocationQuery query;
  query.user_id = request.user_id;
  query.created_after_epoch_seconds = request.time_from_epoch_seconds;
  query.created_before_epoch_seconds = request.time_to_epoch_seconds;
  query.limit = request.items;
  query.offset = request.page_num * request.items;

  FSS_TRY(page, ports_.locations.List(caller.partition, query));
  //  契约 §2.5：无匹配记录时上游返回 **400**（不是 200 + 空数组）。
  //  消息逐字对齐上游 provider：`FileLocationRepository.findAll` 在 `page.isEmpty()` 时抛
  //  `FileLocationNotFoundException("Nothing found for such filter and page(num: %s, size: %s).")`
  //  （vendored：`provider/file-azure/.../repository/FileLocationRepository.java`）。
  if (page.total == 0 || page.records.empty()) {
    return Invalid("Nothing found for such filter and page(num: " +
                   std::to_string(request.page_num) + ", size: " + std::to_string(request.items) +
                   ").");
  }

  FileListResult out;
  out.number = request.page_num;
  out.size = request.items;
  out.total = page.total;
  for (const auto& location : page.records) {
    FileListEntry entry;
    entry.file_id = location.file_id;
    //  ★ `Driver` 与 §2.2/§2.3 一致用小写驱动名（契约 §2.5 的样例是 `"posix"`）。
    //    此前这里直接用 `ProviderKeyOf`（大写 provider key）→ 与 getFileLocation 不一致（P6-D09）。
    entry.driver = ToLower(ProviderKeyOf(location));
    entry.location = PhysicalLocationOf(location);
    entry.created_at_epoch_seconds = location.created_at_epoch_seconds;
    entry.created_by = location.user_id;
    out.content.push_back(std::move(entry));
  }
  out.number_of_elements = static_cast<int>(out.content.size());
  audit.Success();
  return out;
}

// =============================================================================
//  ⑤ CreateFileMetadata（契约 §2.6 的 12 步序列）
// =============================================================================
fss::Result<std::string> CreateFileMetadata::Execute(
    const CallerContext& caller, const domain::FileMetadataRecord& record) {
  //  对象 id 在序列进行到第 9 步才知道 → 用 `SetObjectId` 补上（失败时至少带上 FileSource）
  AuditGuard audit(ports_, caller, "createMetadata",
                   record.data.dataset_properties.file_source_info.file_source);
  FSS_TRY(AuthorizeCaller(ports_, caller, domain::kRoleEditors, true));

  // 1. 状态事件 IN_PROGRESS（非致命）
  PublishStatus(ports_, caller, "IN_PROGRESS", 0);

  // 2. kind：4 段 + wks + dataset--File.Generic
  FSS_TRY(KindValidator::Validate(record.kind));

  // 3. 领域校验（契约 §3.1 必需字段 + §3.4 负向矩阵）
  //  ⚠️ 必须在这里显式调用：`ParseFileMetadataRecord` 只保证"结构/类型"，不做
  //     `Endian` 枚举、ACL 主体格式、legal tag、FileSource 路径形状这些**语义**校验。
  //     漏掉它的后果是"非法 Endian 也能建记录"（P4-D07，HTTP 侧负向矩阵抓到）。
  if (const auto validation = domain::ValidateMetadataRecord(record); !validation.ok()) {
    const auto& first = validation.issues.front();
    //  两条固定消息（契约 §5）要映射到各自的 ErrorKind，其余归 kInvalidArgument
    fss::ErrorKind kind = fss::ErrorKind::kInvalidArgument;
    if (first.message == "FileSource can not be empty") {
      kind = fss::ErrorKind::kFileSourceEmpty;
    } else if (first.message.rfind("Invalid source file path to copy from ", 0) == 0) {
      kind = fss::ErrorKind::kInvalidSourcePath;
    }
    return Err(kind, first.message);
  }

  // 3b. FileSource 必需（校验器已覆盖，这里保留一条"防御性"断言，避免校验器被改坏时静默通过）
  const std::string file_source =
      record.data.dataset_properties.file_source_info.file_source;
  if (file_source.empty()) {
    return Err(fss::ErrorKind::kFileSourceEmpty, "FileSource can not be empty");
  }
  // 3c. 可选校验器（legal / schema）；失败即 400
  FSS_TRY(ports_.legal.Validate(caller.partition, record.legal.legaltags));
  FSS_TRY(ports_.schema.Validate(record.kind, ToJson(record)));

  // 4. 服务端生成 id
  domain::FileMetadataRecord out = record;
  out.id = caller.partition + ":dataset--File.Generic:" + ports_.ids.NewUuidNoDash();
  out.version = 1;

  // 4b. **幂等快路径**：同 (partition, FileSource) 已有记录 → 直接返回它。
  //     ★ 顺序很重要：必须在"复制 + 清理 staging"**之前**判断。否则重试（或并发实例）
  //       会去复制一个已经被上一个成功请求清理掉的 staging 对象 → 502（P6-D13）；
  //       更糟的是它的回滚会把对方刚写好的 persistent 对象删掉。
  if (auto existing = ports_.metadata.GetLatestByFileSource(caller.partition, file_source);
      existing.ok()) {
    return ReturnExistingRecord(ports_, caller, existing.value());
  }

  // 5. 位置记录（FileSource → 物理位置）
  const auto location_result = ports_.locations.FindByFileSource(caller.partition, file_source);
  if (!location_result.ok()) {
    RecordAudit(ports_, "createMetadataFailure", caller, out.id, false);
    //  ★ 消息逐字对齐上游：源路径在存储侧不存在时，上游（Azure/GCP provider 的 copyFile
    //    失败分支）抛 `INVALID_SOURCE_EXCEPTION + "/" + <path>`，期望报文见
    //    `output_payloads/File_invalid_fileSource_msg.json`。此前这里给的是中文消息，
    //    虽然同为 400，但客户端按 message 匹配时会不兼容（P4-D10）。
    return Err(fss::ErrorKind::kInvalidSourcePath,
               "Invalid source file path to copy from " + file_source);
  }
  const domain::FileLocation location = location_result.value();
  FSS_TRY(from_ref, ObjectRefFromLocation(location));
  FSS_TRY(persistent_container,
          ObjectKeyPolicy::ContainerFor(caller.partition, domain::StorageZone::kPersistent));
  domain::ObjectRef to_ref;
  to_ref.container = persistent_container;
  to_ref.key = from_ref.key;

  // 6. staging → persistent 复制
  //
  //  ★★ 只有"源确实在 staging"时才搬迁。位置记录里的 `zone` 会被**上一次成功**的
  //     CreateFileMetadata 改成 persistent；并发实例 / 客户端重试如果再按它去
  //     "复制 + 清理源对象"，就会把 **persistent 对象本身**当成 staging 清理掉
  //     （记录还在、对象没了 —— P6-D13，本切片的并发用例抓到）。
  const bool source_is_staging = location.zone == domain::StorageZone::kStaging;
  domain::ObjectStat copied_stat;
  if (source_is_staging) {
    const auto copied = CopyBetweenZones(ports_, caller.partition, from_ref, to_ref, location.zone,
                                         domain::StorageZone::kPersistent);
    if (!copied.ok()) {
      //  ★ 并发实例可能在我们复制期间已经登记成功并清理了 staging（源"消失"正是这么来的）：
      //    此时幂等地返回既有记录，**不要**报错，更不要回滚（见 RollbackCreatedObject 的守卫）。
      if (auto existing = ports_.metadata.GetLatestByFileSource(caller.partition, file_source);
          existing.ok()) {
        return ReturnExistingRecord(ports_, caller, existing.value());
      }
      RollbackCreatedObject(ports_, caller, to_ref, file_source, out.id);
      //  依赖服务（存储）异常 → 502（契约 §2.6：失败 → 502/500）
      return Err(fss::ErrorKind::kBadGateway,
                 "复制到 persistent 失败：" + copied.error().message());
    }
    copied_stat = copied.value();
  } else {
    //  已经搬迁过（并发/重试）：直接复用 persistent 对象，**不再复制、也不会清理它**
    const auto persistent_store = ports_.blobs.ForPartition(caller.partition,
                                                            domain::StorageZone::kPersistent);
    if (!persistent_store.ok()) {
      RollbackCreatedObject(ports_, caller, to_ref, file_source, out.id);
      return Err(fss::ErrorKind::kBadGateway,
                 "无法解析 persistent 存储：" + persistent_store.error().message());
    }
    const auto existing = persistent_store.value()->stat(to_ref);
    if (!existing.ok() || !existing.value().exists) {
      //  位置记录说"已经迁过"，但对象不在 → 依赖故障（或被人删过）。
      //  绝不能静默写一条指向空对象的记录。
      RollbackCreatedObject(ports_, caller, to_ref, file_source, out.id);
      return Err(fss::ErrorKind::kBadGateway,
                 "persistent 对象缺失（位置记录已迁移但对象不存在）");
    }
    copied_stat = existing.value();
  }

  // 7. 校验和：`checksum = storageUtil.getChecksum(persistentLocation)`，非空则**回写覆盖**
  //    `FileSourceInfo.Checksum` + `ChecksumAlgorithm`（调研 §2.1 第 7 条 / §2.3）。
  //
  //  ★★ 客户端传入的 `Checksum`/`ChecksumAlgorithm` 是**被覆写的输入**，不是"待校验的断言"：
  //     上游证据 ①：`docs/01-osdu-research.md` §2.3「校验和：**服务端覆写**客户端传入的
  //     `Checksum`/`ChecksumAlgorithm`（至少 Azure 实现如此）」；
  //     上游证据 ②：vendored 样例 `File_CorrectPayload.json` 里客户端给的是
  //     `MD5("") = d41d8cd9…` 却声明 `ChecksumAlgorithm = "SHA-256"`，而它的期望响应是 **201** ——
  //     任何"比对不符就拒绝"的实现在这条权威样例上都会 **400**。
  //     （本项目计划里曾写过"提供但不符 → 400 + 删除对象"，那是**没有上游依据的臆断**，
  //       已在 `docs/00-final-design.md` §5 记录为被推翻的结论。）
  auto& source_info = out.data.dataset_properties.file_source_info;

  //  ★ 优先用复制返回的**原生**校验和（零额外读盘）；只有在"原生缺失/算法不认识/不是合法 hex"
  //    时才**流式回算**。`ETAG` 这类非 hex 摘要绝不能当校验和写进记录（那是"看起来有值"的假象）。
  std::string checksum = copied_stat.checksum;
  std::string algorithm = copied_stat.checksum_algorithm;
  const auto native_algorithm = crypto::ParseChecksumAlgorithm(algorithm);
  const bool native_usable =
      native_algorithm.has_value() && crypto::IsHexDigestOf(checksum, *native_algorithm);
  if (native_usable) {
    //  规范名（`SHA256`/`SHA1`/`MD5`）：驱动回什么写法都不影响记录里的一致性
    algorithm = std::string(crypto::CanonicalChecksumName(*native_algorithm));
  } else {
    //  默认算法是 SHA-256（上游 Azure 驱动给的是 MD5，故算法必须跟着驱动走 —— C6.4 的"算法覆盖"）
    const auto computed = ComputeChecksumStreaming(ports_, caller.partition, to_ref,
                                                   domain::StorageZone::kPersistent,
                                                   crypto::ChecksumAlgorithm::kSha256);
    if (!computed.ok()) {
      //  ★ 第 7 步失败也属于第 12 步的"任一步 6/7/9 失败"：必须**回滚删除**已搬迁的对象。
      //    此前这里是 `FSS_TRY`，直接 return 就漏掉了回滚（C6.3 的故障注入点③抓到）。
      RollbackCreatedObject(ports_, caller, to_ref, file_source, out.id);
      return Err(fss::ErrorKind::kBadGateway,
                 "计算校验和失败：" + computed.error().message());
    }
    checksum = computed.value();
    algorithm = std::string(crypto::CanonicalChecksumName(crypto::ChecksumAlgorithm::kSha256));
  }

  if (!checksum.empty()) {
    source_info.checksum = checksum;
    source_info.checksum_algorithm = algorithm.empty() ? "SHA256" : algorithm;
    out.data.checksum = checksum;
    out.data.checksum_algorithm = source_info.checksum_algorithm;
  }

  // 8/9. 写元数据记录（幂等键 = partition + FileSource）
  const auto created = ports_.metadata.Create(caller.partition, out);
  if (!created.ok()) {
    RollbackCreatedObject(ports_, caller, to_ref, file_source, out.id);
    return Err(fss::ErrorKind::kInternal, "写入元数据记录失败：" + created.error().message());
  }

  // 10. 成功事件（非致命）：**两个**事件，顺序与上游一致（先 status，再 datasetDetails）
  PublishStatus(ports_, caller, "SUCCESS", created.value().version);
  PublishDatasetDetails(ports_, caller, created.value().id, created.value().version);

  // 位置记录迁到 persistent（zone 更新）并记上传者（getFileList 的 UserID 过滤）
  domain::FileLocation persistent_location = location;
  persistent_location.zone = domain::StorageZone::kPersistent;
  //  ★ 物理引用必须**一起**改：只改 zone 会让自签下载 URL 去 staging 容器取对象，
  //    取不到时数据面返回 200 + `Content-Length: 0`（静默空文件）。端到端用例（P4-D05）抓到。
  persistent_location.extra[std::string(kExtraContainer)] = to_ref.container;
  persistent_location.extra[std::string(kExtraObjectKey)] = to_ref.key;
  persistent_location.user_id = caller.user_id;
  persistent_location.updated_at_epoch_seconds = ports_.clock.NowEpochSeconds();
  (void)ports_.locations.Save(caller.partition, persistent_location);

  // 11. 删除 staging 对象：**失败被忽略**（上游 issue #76：清理失败不得让已成功的登记变失败），
  //     但必须留下审计告警（契约 §2.6 第 11 步），否则"staging 里堆着孤儿"会无人察觉。
  bool staging_removed = true;
  //  ★ 只有"源在 staging"时才清理：`from_ref` 在 source_is_staging=false 时指向的是
  //    **persistent** 对象，绝不能当 staging 清理（P6-D13）。
  if (source_is_staging) {
    if (auto staging_store =
            ports_.blobs.ForPartition(caller.partition, domain::StorageZone::kStaging);
        staging_store.ok()) {
      const auto removal = staging_store.value()->remove(from_ref);
      staging_removed = removal.ok();
      if (!staging_removed) {
        RecordAudit(ports_, "createMetadataStagingCleanupFailure", caller, created.value().id, false);
      }
    }
  }
  (void)staging_removed;
  audit.SetObjectId(created.value().id);
  audit.Success();
  return created.value().id;
}

// =============================================================================
//  ⑥ GetFileMetadata
// =============================================================================
fss::Result<domain::FileMetadataRecord> GetFileMetadata::Execute(
    const CallerContext& caller, std::string_view record_id) {
  AuditGuard audit(ports_, caller, "readMetadata", std::string(record_id));
  FSS_TRY(AuthorizeCaller(ports_, caller, domain::kRoleViewers, true));
  FSS_TRY(record, ports_.metadata.GetById(caller.partition, record_id));
  audit.Success();
  return record;
}

// =============================================================================
//  ⑦ DeleteFileMetadata
// =============================================================================
fss::Result<void> DeleteFileMetadata::Execute(const CallerContext& caller,
                                              std::string_view record_id) {
  AuditGuard audit(ports_, caller, "deleteMetadata", std::string(record_id));
  //  上游 `FileMetadataApi`：`hasPermission(FILE_EDITORS, FILE_ADMIN)` —— 任一即可
  static constexpr std::string_view kRoles[] = {domain::kRoleFileEditors,
                                                domain::kRoleFileAdmin};
  FSS_TRY(AuthorizeCallerAny(ports_, caller, kRoles, true));
  FSS_TRY(record, ports_.metadata.GetById(caller.partition, record_id));

  FSS_TRY(ports_.metadata.Delete(caller.partition, record_id));

  //  尽力删除 persistent 对象与位置记录（失败不影响 204 语义，但要记账）
  const std::string file_source =
      record.data.dataset_properties.file_source_info.file_source;
  if (!file_source.empty()) {
    const auto location = ports_.locations.FindByFileSource(caller.partition, file_source);
    if (location.ok()) {
      const auto ref = ObjectRefFromLocation(location.value());
      if (ref.ok()) {
        if (auto store = ports_.blobs.ForPartition(caller.partition, location.value().zone);
            store.ok()) {
          (void)store.value()->remove(ref.value());
        }
      }
      (void)ports_.locations.Delete(caller.partition, location.value().file_id);
    }
  }
  audit.Success();
  return Ok();
}

// =============================================================================
//  ⑧ GetStorageInstructions
// =============================================================================
fss::Result<StorageInstructions> GetStorageInstructions::Execute(
    const CallerContext& caller, const std::optional<std::string>& expiry_time) {
  AuditGuard audit(ports_, caller, "getStorageInstructions");
  FSS_TRY(AuthorizeCaller(ports_, caller, kRoleDatasetEditors, true));
  FSS_TRY(location, ports_.issuer.IssueUploadLocation(caller.partition, caller.user_id,
                                                      std::nullopt, expiry_time));

  StorageInstructions out;
  out.provider_key = ToUpper(location.driver);
  out.signed_url = location.signed_url;
  out.file_source = location.file_source;
  out.created_by = caller.user_id;
  out.expires_at_epoch_seconds = location.expires_at_epoch_seconds;
  audit.SetObjectId(location.file_id);
  audit.Success();
  return out;
}

// =============================================================================
//  ⑨ GetRetrievalInstructions
// =============================================================================
fss::Result<std::vector<RetrievalInstruction>> GetRetrievalInstructions::Execute(
    const CallerContext& caller, const std::vector<std::string>& dataset_registry_ids,
    const std::optional<std::string>& expiry_time) {
  AuditGuard audit(ports_, caller, "getRetrievalInstructions");
  FSS_TRY(AuthorizeCaller(ports_, caller, kRoleDatasetViewers, true));

  std::vector<RetrievalInstruction> out;
  for (const auto& registry_id : dataset_registry_ids) {
    //  registry id → 元数据记录 → FileSource → 位置记录 → 下载地址
    const auto record = ports_.metadata.GetById(caller.partition, registry_id);
    if (!record.ok()) continue;
    const std::string file_source =
        record.value().data.dataset_properties.file_source_info.file_source;
    if (file_source.empty()) continue;
    const auto location = ports_.locations.FindByFileSource(caller.partition, file_source);
    if (!location.ok()) continue;
    const auto signed_url =
        ports_.issuer.IssueDownloadLocation(caller.partition, location.value().file_id, expiry_time);
    if (!signed_url.ok()) continue;

    RetrievalInstruction instruction;
    instruction.dataset_registry_id = registry_id;
    instruction.signed_url = signed_url.value().signed_url;
    instruction.provider_key = ProviderKeyOf(location.value());
    instruction.file_source = location.value().file_source;
    instruction.created_by = caller.user_id;
    instruction.expires_at_epoch_seconds = signed_url.value().expires_at_epoch_seconds;
    out.push_back(std::move(instruction));
  }
  audit.Success();
  return out;
}

// =============================================================================
//  ⑩ CopyFiles
// =============================================================================
fss::Result<std::vector<CopyFileOutcome>> CopyFiles::Execute(
    const CallerContext& caller, const std::vector<CopyFileSource>& sources) {
  AuditGuard audit(ports_, caller, "copyFiles");
  //  上游 `FileDmsApi`/`FileCollectionDmsApi`：`hasPermission(STORAGE_CREATOR, STORAGE_ADMIN)`
  static constexpr std::string_view kRoles[] = {domain::kRoleStorageCreator,
                                                domain::kRoleStorageAdmin};
  FSS_TRY(AuthorizeCallerAny(ports_, caller, kRoles, true));

  std::vector<CopyFileOutcome> out;
  for (const auto& source : sources) {
    if (!LooksLikeFileSource(source.file_source)) {
      //  契约 §5 的固定消息：`Invalid source file path to copy from <path>`
      return Err(fss::ErrorKind::kInvalidSourcePath,
                 "Invalid source file path to copy from " + source.file_source);
    }
    CopyFileOutcome outcome;
    const auto location = ports_.locations.FindByFileSource(caller.partition, source.file_source);
    if (!location.ok()) {
      outcome.success = false;
      out.push_back(std::move(outcome));
      continue;
    }
    const auto from_ref = ObjectRefFromLocation(location.value());
    const auto persistent_container =
        ObjectKeyPolicy::ContainerFor(caller.partition, domain::StorageZone::kPersistent);
    if (!from_ref.ok() || !persistent_container.ok()) {
      outcome.success = false;
      out.push_back(std::move(outcome));
      continue;
    }
    domain::ObjectRef to_ref;
    to_ref.container = persistent_container.value();
    to_ref.key = from_ref.value().key;
    const auto copied = CopyBetweenZones(ports_, caller.partition, from_ref.value(), to_ref,
                                         location.value().zone, domain::StorageZone::kPersistent);
    outcome.success = copied.ok();
    outcome.dataset_blob_storage_path = to_ref.container + "/" + to_ref.key;
    out.push_back(std::move(outcome));
  }
  audit.Success();
  return out;
}

// =============================================================================
//  ⑪ GetFileSignedUrl
// =============================================================================
fss::Result<SignedUrlResult> GetFileSignedUrl::Execute(
    const CallerContext& caller, const std::vector<std::string>& srns,
    const std::optional<std::string>& expiry_time) {
  AuditGuard audit(ports_, caller, "getFileSignedUrl");
  FSS_TRY(AuthorizeCaller(ports_, caller, kRoleDeliveryViewer, true));

  SignedUrlResult out;
  for (const auto& srn : srns) {
    static constexpr std::string_view kSrnPrefix = "srn:file/";
    if (srn.rfind(kSrnPrefix, 0) != 0 || srn.size() == kSrnPrefix.size()) {
      out.unprocessed.push_back(srn);
      continue;
    }
    const std::string file_id = srn.substr(kSrnPrefix.size());
    const auto location = ports_.locations.Find(caller.partition, file_id);
    if (!location.ok()) {
      out.unprocessed.push_back(srn);
      continue;
    }
    const auto signed_url = ports_.issuer.IssueDownloadLocation(caller.partition, file_id, expiry_time);
    if (!signed_url.ok()) {
      out.unprocessed.push_back(srn);
      continue;
    }
    SignedUrlEntry entry;
    entry.signed_url = signed_url.value().signed_url;
    entry.unsigned_url = signed_url.value().file_source;
    //  kind 从元数据取（若有）
    const auto record =
        ports_.metadata.GetLatestByFileSource(caller.partition, location.value().file_source);
    if (record.ok()) entry.kind = record.value().kind;
    out.processed.emplace(srn, std::move(entry));
  }
  audit.Success();
  return out;
}

// =============================================================================
//  ⑫ RevokeUrl（不要求 partition；恒定成功语义）
// =============================================================================
fss::Result<void> RevokeUrl::Execute(const CallerContext& caller) {
  AuditGuard audit(ports_, caller, "revokeUrl");
  FSS_TRY(AuthorizeCaller(ports_, caller, kRoleAdmin, /*require_partition=*/false));
  audit.Success();
  return Ok();
}

// =============================================================================
//  ⑬ GetInfo（无鉴权）
// =============================================================================
fss::Result<VersionInfo> GetInfo::Execute() {
  VersionInfo info;
  info.version = "v2";
#ifdef FSS_BUILD_VERSION
  info.build_version = FSS_BUILD_VERSION;
#else
  info.build_version = "unknown";
#endif
  info.connected_outer_services = {"storage"};
  info.auth_mode = ports_.auth_mode;  // C8.5：鉴权模式必须在 `/v2/info` 可见
  return info;
}

// =============================================================================
//  ⑭ UploadFile（gRPC 扩展：客户端流 → 对象字节）
// =============================================================================
//  授权与 `GetUploadLocation` **同一个角色**（editors）：能拿到上传地址的人才能写字节。
//  ★ 位置记录是授权的锚点：只允许写到"已经签发过位置记录"的 FileSource 上。
//    显式 `container`/`key` 不接受覆盖，只做一致性校验（与 `/v1/transfer` 内核
//    "对象键只来自 token 载荷"同一条防线）。
fss::Result<UploadStreamResult> UploadFile::Execute(const CallerContext& caller,
                                                    const UploadStreamRequest& request,
                                                    bytes::ByteSource& body) {
  AuditGuard audit(ports_, caller, "uploadFile", request.file_source);
  FSS_TRY(AuthorizeCaller(ports_, caller, domain::kRoleEditors, /*require_partition=*/true));
  if (request.file_source.empty()) {
    return Invalid("UploadFile 要求 file_source（先调用 GetUploadLocation 取得）");
  }
  const auto location = ports_.locations.FindByFileSource(caller.partition, request.file_source);
  if (!location.ok()) {
    return Err(fss::ErrorKind::kNotFound,
               "没有 FileSource = " + request.file_source + " 的位置记录（请先调用 GetUploadLocation）");
  }
  FSS_TRY(ref, ObjectRefFromLocation(location.value()));
  if (request.container.has_value() && *request.container != ref.container) {
    return Err(fss::ErrorKind::kPermissionDenied,
               "container 与已签发的位置记录不一致（不接受坐标覆盖）");
  }
  if (request.key.has_value() && *request.key != ref.key) {
    return Err(fss::ErrorKind::kPermissionDenied,
               "key 与已签发的位置记录不一致（不接受坐标覆盖）");
  }

  FSS_TRY(store, ports_.blobs.ForPartition(caller.partition, location.value().zone));
  //  纵深防御：容器由 LocationIssuer 在签发时保证存在（幂等）
  FSS_TRY(store->ensure_container(ref.container));

  domain::PutOptions options;
  options.content_type = request.content_type;
  options.expected_checksum = request.expected_checksum;
  options.checksum_algorithm = request.checksum_algorithm;
  const auto put = store->put(ref, body, options);
  if (!put.ok()) {
    return put.error();
  }

  FSS_TRY(stat, store->stat(ref));
  UploadStreamResult out;
  out.file_id = location.value().file_id;
  out.file_source = location.value().file_source;
  out.bytes_written = stat.size > 0 ? static_cast<std::uint64_t>(stat.size) : 0;
  out.checksum = stat.checksum;
  out.checksum_algorithm = stat.checksum_algorithm;
  if (out.checksum.empty()) {
    //  驱动不提供校验和时**流式回算**（与第 7 步同一条路径），而不是留空或编一个值
    FSS_TRY(computed, ComputeChecksumStreaming(ports_, caller.partition, ref,
                                               location.value().zone,
                                               crypto::ChecksumAlgorithm::kSha256));
    out.checksum = computed;
    out.checksum_algorithm = "SHA256";
  }

  //  `register_metadata=true` 时复用**同一个** 12 步用例（不另写一条注册路径）
  if (request.register_metadata) {
    if (!request.metadata.has_value()) {
      return Invalid("register_metadata=true 时必须提供 metadata");
    }
    const auto& declared =
        request.metadata->data.dataset_properties.file_source_info.file_source;
    if (declared != request.file_source) {
      return Invalid("metadata 的 data.FileSource 与上传目标不一致");
    }
    CreateFileMetadata create(ports_);
    const auto created = create.Execute(caller, *request.metadata);
    if (!created.ok()) {
      return created.error();
    }
    out.metadata_record_id = created.value();
  }
  audit.SetObjectId(location.value().file_id);
  audit.Success();
  return out;
}

// =============================================================================
//  ⑮ DownloadFile（gRPC 扩展：对象字节 → 服务端流）
// =============================================================================
//  授权与 `GetDownloadLocation` 同一个角色（viewers）。定位方式二选一：
//  `file_id` 优先（位置记录主键），否则用 `file_source`（幂等键）。
fss::Result<DownloadStreamResult> DownloadFile::Execute(const CallerContext& caller,
                                                        std::string_view file_id,
                                                        std::string_view file_source,
                                                        std::uint64_t offset,
                                                        std::uint64_t length,
                                                        bytes::ByteSink& sink) {
  AuditGuard audit(ports_, caller, "downloadFile",
                   file_id.empty() ? std::string(file_source) : std::string(file_id));
  FSS_TRY(AuthorizeCaller(ports_, caller, domain::kRoleViewers, /*require_partition=*/true));
  if (file_id.empty() && file_source.empty()) {
    return Invalid("DownloadFile 要求 file_id 或 file_source");
  }
  fss::Result<domain::FileLocation> location =
      file_id.empty() ? ports_.locations.FindByFileSource(caller.partition, file_source)
                      : ports_.locations.Find(caller.partition, file_id);
  FSS_TRY(resolved, std::move(location));
  FSS_TRY(ref, ObjectRefFromLocation(resolved));
  FSS_TRY(store, ports_.blobs.ForPartition(caller.partition, resolved.zone));
  FSS_TRY(stat, store->stat(ref));
  if (!stat.exists) {
    return Err(fss::ErrorKind::kNotFound, "对象不存在：" + ref.key);
  }

  //  ★ 与 HTTP `Range` 的语义对齐：`length == 0` = 从 `offset` 到末尾；
  //    `offset` 越界由驱动报 `kInvalidArgument`（与 `/v1/transfer` 的 416 同类）。
  const domain::ByteRange range{offset, length};
  CountingProxySink counter(sink);
  FSS_TRY(store->get(ref, counter, range));

  DownloadStreamResult out;
  out.total_size = stat.size;
  out.checksum = stat.checksum;
  out.checksum_algorithm = stat.checksum_algorithm;
  out.bytes_written = static_cast<std::uint64_t>(counter.bytes_written());
  audit.Success();
  return out;
}

// =============================================================================
//  ⑯ ServerSideCopy（gRPC 扩展：服务端复制字节，供无存储凭证的调用方）
// =============================================================================
//  角色与 `/v2/files/copy` 相同（storage creator / storage admin）。
//  ★ 只搬字节，不动任何记录：目标坐标由 `target_file_source` + `target_zone` 推导。
//    记录迁移（staging→persistent 的 zone 更新）属于 `CreateFileMetadata`。
fss::Result<ServerSideCopyResult> ServerSideCopy::Execute(
    const CallerContext& caller, std::string_view source_file_source,
    std::string_view target_file_source, domain::StorageZone target_zone) {
  AuditGuard audit(ports_, caller, "serverSideCopy", std::string(source_file_source));
  static constexpr std::string_view kRoles[] = {domain::kRoleStorageCreator,
                                                domain::kRoleStorageAdmin};
  FSS_TRY(AuthorizeCallerAny(ports_, caller, kRoles, /*require_partition=*/true));
  if (!LooksLikeFileSource(source_file_source)) {
    return Err(fss::ErrorKind::kInvalidSourcePath, "Invalid source file path to copy from " +
                                                      std::string(source_file_source));
  }
  if (!LooksLikeFileSource(target_file_source)) {
    return Invalid("target_file_source 非法：" + std::string(target_file_source));
  }
  FSS_TRY(source_location,
          ports_.locations.FindByFileSource(caller.partition, source_file_source));
  FSS_TRY(from_ref, ObjectRefFromLocation(source_location));
  //  目标键由目标的 FileSource 按同一套安全策略推导（逐段白名单，避免任意键写入）
  FSS_TRY(parts, ObjectKeyPolicy::ParseFileSource(target_file_source));
  FSS_TRY(container, ObjectKeyPolicy::ContainerFor(caller.partition, target_zone));
  domain::ObjectRef to_ref;
  to_ref.container = container;
  to_ref.key = ObjectKeyPolicy::MakePosixKey(parts);

  FSS_TRY(stat, CopyBetweenZones(ports_, caller.partition, from_ref, to_ref,
                                 source_location.zone, target_zone));

  ServerSideCopyResult out;
  out.file_source = std::string(target_file_source);
  out.bytes_copied = stat.size > 0 ? static_cast<std::uint64_t>(stat.size) : 0;
  audit.SetObjectId(source_location.file_id);
  audit.Success();
  return out;
}

}  // namespace fss::app
