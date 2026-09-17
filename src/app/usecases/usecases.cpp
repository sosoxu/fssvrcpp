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
  (void)ports.audit.Record(event);
}

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
                           const domain::ObjectRef& to_ref, std::string_view record_id) {
  if (auto to_store = ports.blobs.ForPartition(caller.partition, domain::StorageZone::kPersistent);
      to_store.ok()) {
    (void)to_store.value()->remove(to_ref);
  }
  PublishStatus(ports, caller, "FAILED", 0);
  RecordAudit(ports, "createMetadataFailure", caller, record_id, false);
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
  FSS_TRY(AuthorizeCaller(ports_, caller, domain::kRoleEditors, /*require_partition=*/true));

  auto result = ports_.issuer.IssueUploadLocation(caller.partition, caller.user_id,
                                                  requested_file_id, expiry_time);
  if (!result.ok()) {
    RecordAudit(ports_, "createLocationFailure", caller, "", false);
    return result.error();
  }
  RecordAudit(ports_, "createLocationSuccess", caller, result.value().file_id, true);
  return result;
}

// =============================================================================
//  ② GetFileLocation
// =============================================================================
fss::Result<FileLocationView> GetFileLocation::Execute(const CallerContext& caller,
                                                       std::string_view file_id) {
  FSS_TRY(AuthorizeCaller(ports_, caller, domain::kRoleEditors, true));
  FSS_TRY(location, ports_.locations.Find(caller.partition, file_id));

  FileLocationView view;
  view.driver = ProviderKeyOf(location);
  //  契约 §2.3 的 `Driver` 是小写（"posix"），providerKey 是大写；这里返回小写形态
  std::transform(view.driver.begin(), view.driver.end(), view.driver.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  view.location = PhysicalLocationOf(location);
  view.file_source = location.file_source;
  return view;
}

// =============================================================================
//  ③ GetDownloadLocation
// =============================================================================
fss::Result<DownloadLocationResult> GetDownloadLocation::Execute(
    const CallerContext& caller, std::string_view file_id,
    const std::optional<std::string>& expiry_time) {
  FSS_TRY(AuthorizeCaller(ports_, caller, domain::kRoleViewers, true));
  FSS_TRY(result, ports_.issuer.IssueDownloadLocation(caller.partition, file_id, expiry_time));

  DownloadLocationResult out;
  out.file_id = result.file_id;
  out.signed_url = result.signed_url;
  out.driver = result.driver;
  out.expires_at_epoch_seconds = result.expires_at_epoch_seconds;
  out.native_presign = result.native_presign;
  RecordAudit(ports_, "getDownloadLocationSuccess", caller, file_id, true);
  return out;
}

// =============================================================================
//  ④ GetFileList
// =============================================================================
fss::Result<FileListResult> GetFileList::Execute(const CallerContext& caller,
                                                 const FileListRequest& request) {
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
  return out;
}

// =============================================================================
//  ⑤ CreateFileMetadata（契约 §2.6 的 12 步序列）
// =============================================================================
fss::Result<std::string> CreateFileMetadata::Execute(
    const CallerContext& caller, const domain::FileMetadataRecord& record) {
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
  const auto copied = CopyBetweenZones(ports_, caller.partition, from_ref, to_ref, location.zone,
                                       domain::StorageZone::kPersistent);
  if (!copied.ok()) {
    RollbackCreatedObject(ports_, caller, to_ref, out.id);
    //  依赖服务（存储）异常 → 502（契约 §2.6：失败 → 502/500）
    return Err(fss::ErrorKind::kBadGateway,
               "复制到 persistent 失败：" + copied.error().message());
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
  std::string checksum = copied.value().checksum;
  std::string algorithm = copied.value().checksum_algorithm;
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
      RollbackCreatedObject(ports_, caller, to_ref, out.id);
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
    RollbackCreatedObject(ports_, caller, to_ref, out.id);
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
  if (auto staging_store = ports_.blobs.ForPartition(caller.partition, domain::StorageZone::kStaging);
      staging_store.ok()) {
    const auto removal = staging_store.value()->remove(from_ref);
    staging_removed = removal.ok();
    if (!staging_removed) {
      RecordAudit(ports_, "createMetadataStagingCleanupFailure", caller, created.value().id, false);
    }
  }
  (void)staging_removed;
  RecordAudit(ports_, "createMetadataSuccess", caller, created.value().id, true);
  return created.value().id;
}

// =============================================================================
//  ⑥ GetFileMetadata
// =============================================================================
fss::Result<domain::FileMetadataRecord> GetFileMetadata::Execute(
    const CallerContext& caller, std::string_view record_id) {
  FSS_TRY(AuthorizeCaller(ports_, caller, domain::kRoleViewers, true));
  return ports_.metadata.GetById(caller.partition, record_id);
}

// =============================================================================
//  ⑦ DeleteFileMetadata
// =============================================================================
fss::Result<void> DeleteFileMetadata::Execute(const CallerContext& caller,
                                              std::string_view record_id) {
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
  RecordAudit(ports_, "deleteMetadataSuccess", caller, record_id, true);
  return Ok();
}

// =============================================================================
//  ⑧ GetStorageInstructions
// =============================================================================
fss::Result<StorageInstructions> GetStorageInstructions::Execute(
    const CallerContext& caller, const std::optional<std::string>& expiry_time) {
  FSS_TRY(AuthorizeCaller(ports_, caller, kRoleDatasetEditors, true));
  FSS_TRY(location, ports_.issuer.IssueUploadLocation(caller.partition, caller.user_id,
                                                      std::nullopt, expiry_time));

  StorageInstructions out;
  out.provider_key = ToUpper(location.driver);
  out.signed_url = location.signed_url;
  out.file_source = location.file_source;
  out.created_by = caller.user_id;
  out.expires_at_epoch_seconds = location.expires_at_epoch_seconds;
  return out;
}

// =============================================================================
//  ⑨ GetRetrievalInstructions
// =============================================================================
fss::Result<std::vector<RetrievalInstruction>> GetRetrievalInstructions::Execute(
    const CallerContext& caller, const std::vector<std::string>& dataset_registry_ids,
    const std::optional<std::string>& expiry_time) {
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
  return out;
}

// =============================================================================
//  ⑩ CopyFiles
// =============================================================================
fss::Result<std::vector<CopyFileOutcome>> CopyFiles::Execute(
    const CallerContext& caller, const std::vector<CopyFileSource>& sources) {
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
  RecordAudit(ports_, "copyFiles", caller, "", true);
  return out;
}

// =============================================================================
//  ⑪ GetFileSignedUrl
// =============================================================================
fss::Result<SignedUrlResult> GetFileSignedUrl::Execute(
    const CallerContext& caller, const std::vector<std::string>& srns,
    const std::optional<std::string>& expiry_time) {
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
  return out;
}

// =============================================================================
//  ⑫ RevokeUrl（不要求 partition；恒定成功语义）
// =============================================================================
fss::Result<void> RevokeUrl::Execute(const CallerContext& caller) {
  FSS_TRY(AuthorizeCaller(ports_, caller, kRoleAdmin, /*require_partition=*/false));
  RecordAudit(ports_, "revokeUrl", caller, "", true);
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
  return info;
}

}  // namespace fss::app
