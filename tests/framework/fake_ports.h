// =============================================================================
//  tests/framework/fake_ports.h —— 端口的测试替身（可记录、可注入故障）
// =============================================================================
//  用途：应用层用例（P2）只依赖端口，因此可以用这些替身在**没有真实存储/DB/网络**的
//  情况下驱动全部业务逻辑，并在需要时注入"依赖失败"来验证错误映射与补偿逻辑。
//
//  ★ 两条纪律
//    ① 替身只实现**端口**语义，不复制真实实现的业务规则（否则测的是替身而不是用例）。
//    ② 每个替身都要能"记录调用"或"注入失败"，否则无法断言"用例真的走了这条路"（R1）。
//
//  ⚠️ `CapabilityOverrideBlobStore` 是**装饰器**：它借用 `InMemoryBlobStore` 的数据面，
//     只覆盖 `capabilities()` 与 `presign_*`，用来构造"有原生预签名能力"的后端
//     （内存实现本身 `native_presign=false`，见 ADR-003 §1）。
//     这样 C2.6 的两个能力组合都能被真实驱动，而不是靠改内存实现的常量。
#pragma once

#include "common/result/result.h"
#include "domain/model/file_metadata.h"
#include "domain/ports/ports.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fss::test {

// =============================================================================
//  存储
// =============================================================================
class CapabilityOverrideBlobStore final : public domain::IBlobStore {
 public:
  CapabilityOverrideBlobStore(domain::IBlobStore& inner, domain::BlobCapabilities caps,
                              std::string presign_prefix)
      : inner_(inner), caps_(std::move(caps)), presign_prefix_(std::move(presign_prefix)) {}

  domain::BlobCapabilities capabilities() const override { return caps_; }

  fss::Result<void> ensure_container(const std::string& container) override {
    return inner_.ensure_container(container);
  }
  fss::Result<domain::SignedLocation> presign_put(
      const domain::ObjectRef& ref, const domain::PresignOptions& options) override {
    return Presign("PUT", ref, options);
  }
  fss::Result<domain::SignedLocation> presign_get(
      const domain::ObjectRef& ref, const domain::PresignOptions& options) override {
    return Presign("GET", ref, options);
  }
  fss::Result<void> put(const domain::ObjectRef& ref, bytes::ByteSource& source,
                        const domain::PutOptions& options) override {
    return inner_.put(ref, source, options);
  }
  fss::Result<void> get(const domain::ObjectRef& ref, bytes::ByteSink& sink,
                        const domain::ByteRange& range) override {
    return inner_.get(ref, sink, range);
  }
  fss::Result<domain::ObjectStat> stat(const domain::ObjectRef& ref) override {
    FSS_TRY(out, inner_.stat(ref));
    if (hide_checksum) {
      out.checksum.clear();
      out.checksum_algorithm.clear();
    }
    return out;
  }
  fss::Result<void> remove(const domain::ObjectRef& ref) override { return inner_.remove(ref); }
  fss::Result<domain::ObjectStat> copy(const domain::ObjectRef& from,
                                       const domain::ObjectRef& to) override {
    FSS_TRY(out, inner_.copy(from, to));
    if (hide_checksum) {
      out.checksum.clear();
      out.checksum_algorithm.clear();
    }
    if (copy_checksum_override.has_value()) {
      out.checksum = copy_checksum_override.value();
      out.checksum_algorithm = copy_checksum_algorithm.value_or("");
    }
    return out;
  }
  fss::Result<domain::ListPage> list(const std::string& container, const std::string& prefix,
                                     const std::string& continuation_token,
                                     int limit) override {
    return inner_.list(container, prefix, continuation_token, limit);
  }

  //  C9.25：转发临时文件清理（装饰器不改变语义）
  fss::Result<domain::TempSweepResult> remove_temp_files(const std::string& container,
                                                 std::int64_t older_than_epoch_seconds,
                                                 bool dry_run) override {
    return inner_.remove_temp_files(container, older_than_epoch_seconds, dry_run);
  }

  //  置位后 presign 直接返回该错误（默认不置位）
  std::optional<fss::Error> presign_error;
  //  置位后 `stat`/`copy` **不报**校验和 —— 模拟"驱动不提供校验和"（逼出流式回算路径，C6.4/C6.9）
  bool hide_checksum = false;
  //  非空时 `copy` 返回该**原生**校验和（配 `copy_checksum_algorithm`）——
  //  模拟 Azure 那样的驱动（`getChecksum` 给的是 MD5，调研 §2.1 第 7 条）
  std::optional<std::string> copy_checksum_override;
  std::optional<std::string> copy_checksum_algorithm;

  int presign_put_calls() const { return presign_put_calls_; }
  int presign_get_calls() const { return presign_get_calls_; }
  const domain::PresignOptions& last_presign_options() const { return last_options_; }

 private:
  fss::Result<domain::SignedLocation> Presign(const std::string& method,
                                              const domain::ObjectRef& ref,
                                              const domain::PresignOptions& options) {
    if (!caps_.native_presign) {
      return Err(fss::ErrorKind::kUnimplemented, "测试替身：native_presign=false");
    }
    if (presign_error.has_value()) {
      //  注入"存储侧拒绝"（如 S3 的 AccessDenied）：用于证明**应用层会把驱动错误原样
      //  透出**，而不是折叠成 kInternal（C2.2 的覆盖率矩阵需要它触发 kStorageAccessDenied）
      return presign_error.value();
    }
    if (method == "PUT") {
      ++presign_put_calls_;
    } else {
      ++presign_get_calls_;
    }
    last_options_ = options;
    domain::SignedLocation out;
    out.url = presign_prefix_ + "/" + ref.container + "/" + ref.key + "?method=" + method;
    out.method = method;
    out.native = true;
    // 故意留空 expires_at：LocationIssuer 必须自己补上（C2.6 断言有效期来自 ExpiryPolicy）
    return out;
  }

  domain::IBlobStore& inner_;
  domain::BlobCapabilities caps_;
  std::string presign_prefix_;
  int presign_put_calls_ = 0;
  int presign_get_calls_ = 0;
  domain::PresignOptions last_options_{};
};

class FakeBlobStoreFactory final : public domain::IBlobStoreFactory {
 public:
  explicit FakeBlobStoreFactory(domain::IBlobStore& default_store) : default_(&default_store) {}

  void SetZoneStore(domain::StorageZone zone, domain::IBlobStore& store) {
    zone_stores_[zone] = &store;
  }

  fss::Result<domain::IBlobStore*> ForPartition(std::string_view partition,
                                                domain::StorageZone zone) override {
    if (fail) return Err(fss::ErrorKind::kUnavailable, "测试替身：ForPartition 注入失败");
    const auto it = zone_stores_.find(zone);  // 装配后只读
    std::lock_guard<std::mutex> lock(mutex_);
    ++calls;
    last_partition = std::string(partition);
    return it == zone_stores_.end() ? default_ : it->second;
  }

  bool fail = false;
  int calls = 0;
  std::string last_partition;

 private:
  domain::IBlobStore* default_;
  std::map<domain::StorageZone, domain::IBlobStore*> zone_stores_;
  //  ★ 并发用例（P9）会在多个服务线程里同时进来：计数器与 `last_partition` 必须加锁，
  //    否则是数据竞争 + 堆损坏（P9-D01 实测 double free）
  mutable std::mutex mutex_;
};

class RecordingSelfSignedCodec final : public domain::ISelfSignedUrlCodec {
 public:
  bool fail = false;

  fss::Result<std::string> Encode(const domain::TransferToken& token,
                                  std::string_view base_url) override {
    if (fail) return Err(fss::ErrorKind::kInternal, "测试替身：Encode 注入失败");
    last_token = token;
    ++encode_calls;
    return std::string(base_url) + "/v1/transfer/tok" + std::to_string(encode_calls) + "?op=" +
           token.op;
  }

  fss::Result<domain::TransferToken> Decode(std::string_view token, std::string_view expires,
                                            std::string_view signature) override {
    (void)token;
    (void)expires;
    (void)signature;
    return Err(fss::ErrorKind::kUnimplemented, "测试替身不实现 Decode");
  }

  domain::TransferToken last_token{};
  int encode_calls = 0;
};

// =============================================================================
//  租户 / 授权 / 校验
// =============================================================================
class FakePartitionRegistry final : public domain::IPartitionRegistry {
 public:
  void Add(domain::PartitionConfig config) { configs_[config.partition] = std::move(config); }

  fss::Result<domain::PartitionConfig> Get(std::string_view partition) override {
    const auto it = configs_.find(std::string(partition));
    if (it == configs_.end()) {
      return Err(fss::ErrorKind::kNotFound, "partition 未注册：" + std::string(partition));
    }
    return it->second;
  }

  fss::Result<std::vector<domain::PartitionConfig>> List() override {
    std::vector<domain::PartitionConfig> out;
    for (const auto& [name, config] : configs_) {
      (void)name;
      out.push_back(config);
    }
    return out;
  }

 private:
  std::map<std::string, domain::PartitionConfig> configs_;
};

class AllowAllAuthorizer final : public domain::IAuthorizer {
 public:
  bool deny = false;
  int calls = 0;

  //  ★ "缺 partition" 由**用例入口**判定（kUnauthenticated "Missing partitionID"），
  //    不是作者器的职责 —— 契约 §1.2 的中间件与用例是同一约束的两道防线。
  fss::Result<void> Authorize(std::string_view required_role, std::string_view partition,
                              std::string_view bearer_token) override {
    (void)partition;
    {
      //  ★ 并发用例会在多个服务线程里同时调用（P9-D01）：`calls`/`last_role` 必须加锁
      std::lock_guard<std::mutex> lock(mutex_);
      ++calls;
      last_role = std::string(required_role);
      last_roles = {last_role};
    }
    if (bearer_token.empty()) {
      return Err(fss::ErrorKind::kUnauthenticated, "Missing authorization token");
    }
    if (deny) {
      return Err(fss::ErrorKind::kPermissionDenied, "角色不足：" + std::string(required_role));
    }
    return Ok();
  }

  //  "任一角色即通过"：替身仍然只表达"放行/拒绝"，但把角色集合记下来（C6.8 用它断言）
  fss::Result<void> AuthorizeAny(std::span<const std::string_view> required_roles,
                                 std::string_view partition,
                                 std::string_view bearer_token) override {
    (void)partition;
    std::string front;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++calls;
      last_roles.clear();
      for (const auto role : required_roles) last_roles.emplace_back(role);
      last_role = last_roles.empty() ? std::string() : last_roles.front();
      front = last_role;
    }
    if (bearer_token.empty()) {
      return Err(fss::ErrorKind::kUnauthenticated, "Missing authorization token");
    }
    if (deny) {
      return Err(fss::ErrorKind::kPermissionDenied, "角色不足：" + front);
    }
    return Ok();
  }

  mutable std::mutex mutex_;
  std::string last_role;
  std::vector<std::string> last_roles;
};

class NoopLegalValidator final : public domain::ILegalValidator {
 public:
  bool fail = false;

  fss::Result<void> Validate(std::string_view partition,
                             const std::vector<std::string>& legal_tags) override {
    (void)partition;
    if (fail) return Err(fss::ErrorKind::kInvalidArgument, "测试替身：legal tag 非法");
    if (legal_tags.empty()) {
      return Err(fss::ErrorKind::kInvalidArgument, "legal tags 不能为空");
    }
    return Ok();
  }
};

class NoopSchemaValidator final : public domain::ISchemaValidator {
 public:
  bool fail = false;

  fss::Result<void> Validate(std::string_view kind, const json::Value& record) override {
    (void)kind;
    (void)record;
    if (fail) return Err(fss::ErrorKind::kInvalidArgument, "测试替身：schema 校验失败");
    return Ok();
  }
};

// =============================================================================
//  可观测
// =============================================================================
class RecordingEventPublisher final : public domain::IEventPublisher {
 public:
  bool fail = false;
  //  ★ 只让**指定 status** 的发布失败（`"IN_PROGRESS"` / `"SUCCESS"`）——
  //    否则无法区分第 1 步与第 10 步的"非致命"（两者都必须不影响响应）。
  std::string fail_status;
  std::vector<std::string> topics;
  std::vector<domain::StatusChangedEvent> events;

  fss::Result<void> PublishStatusChanged(std::string_view topic,
                                         const domain::StatusChangedEvent& event) override {
    const bool failing = fail || (!fail_status.empty() && event.status == fail_status);
    std::lock_guard<std::mutex> lock(mutex_);  // ★ 并发用例（P9-D01）
    ++calls;
    if (failing) {
      return Err(fss::ErrorKind::kUnavailable, "测试替身：事件发布失败");
    }
    topics.emplace_back(topic);
    events.push_back(event);
    return Ok();
  }

  //  `datasetDetails`（契约 §2.6 第 10 步）
  bool fail_dataset_details = false;
  std::vector<domain::DatasetDetailsEvent> details;

  fss::Result<void> PublishDatasetDetails(std::string_view topic,
                                          const domain::DatasetDetailsEvent& event) override {
    std::lock_guard<std::mutex> lock(mutex_);  // ★ 并发用例（P9-D01）
    ++details_calls;
    last_details_topic = std::string(topic);
    if (fail_dataset_details) {
      return Err(fss::ErrorKind::kUnavailable, "测试替身：datasetDetails 发布失败");
    }
    details.push_back(event);
    return Ok();
  }

  mutable std::mutex mutex_;
  int calls = 0;
  int details_calls = 0;
  std::string last_details_topic;

  //  发布**成功**的 status 序列（排障与顺序断言用）
  std::vector<std::string> statuses() const {
    std::vector<std::string> out;
    out.reserve(events.size());
    for (const auto& event : events) out.push_back(event.status);
    return out;
  }

  int CountStatus(std::string_view status) const {
    int n = 0;
    for (const auto& event : events) {
      if (event.status == status) ++n;
    }
    return n;
  }
};

// =============================================================================
//  仓储替身：可注入失败的元数据仓储（C6.3 的第 9 步故障注入）
// =============================================================================
//  为什么用**装饰器**而不是给 `InMemoryMetadataRepository` 加开关：
//    内存仓储是要跟 SQLite / 远端仓储共用同一份契约测试的"准产品实现"，
//    故障注入属于测试替身的职责（与 `FakeBlobStoreFactory` 同类）。
class FaultyMetadataRepository final : public domain::IMetadataRepository {
 public:
  explicit FaultyMetadataRepository(domain::IMetadataRepository& inner) : inner_(inner) {}

  bool fail_create = false;
  fss::ErrorKind create_error = fss::ErrorKind::kInternal;
  int create_calls = 0;

  fss::Result<domain::FileMetadataRecord> Create(std::string_view partition,
                                                const domain::FileMetadataRecord& record) override {
    ++create_calls;
    if (fail_create) {
      return Err(create_error, "测试替身：写入元数据记录失败");
    }
    return inner_.Create(partition, record);
  }
  fss::Result<domain::FileMetadataRecord> GetById(std::string_view partition,
                                                 std::string_view record_id) override {
    return inner_.GetById(partition, record_id);
  }
  fss::Result<domain::FileMetadataRecord> GetLatestByFileSource(
      std::string_view partition, std::string_view file_source) override {
    return inner_.GetLatestByFileSource(partition, file_source);
  }
  fss::Result<domain::FileMetadataRecord> Update(std::string_view partition,
                                                const domain::FileMetadataRecord& record) override {
    return inner_.Update(partition, record);
  }
  fss::Result<void> Delete(std::string_view partition, std::string_view record_id) override {
    return inner_.Delete(partition, record_id);
  }
  fss::Result<domain::MetadataPage> List(std::string_view partition,
                                         const domain::MetadataQuery& query) override {
    return inner_.List(partition, query);
  }

 private:
  domain::IMetadataRepository& inner_;
};

class RecordingAuditLogger final : public domain::IAuditLogger {
 public:
  bool fail = false;
  std::vector<domain::AuditEvent> events;

  fss::Result<void> Record(const domain::AuditEvent& event) override {
    //  ★ 审计在每个受保护端点上都会被调用（P8 起是 RAII 守卫）→ 并发下必须加锁（P9-D01）
    std::lock_guard<std::mutex> lock(mutex_);
    ++calls;
    if (fail) return Err(fss::ErrorKind::kInternal, "测试替身：审计写入失败");
    events.push_back(event);
    return Ok();
  }

  mutable std::mutex mutex_;
  int calls = 0;
};

// =============================================================================
//  租约（ADR-009）——内存实现，供 GC/多实例用例的单元测试
//  ★ 线程安全（内部一把互斥量）：C6.12 的"两个 GC 并发"用例会在两个线程里同时
//    `ClaimExpired` —— 没有锁时那既是数据竞争，也会让"原子领取"失去意义。
// =============================================================================
class InMemoryLeaseRepository final : public domain::ILeaseRepository {
 public:
  fss::Result<Lease> Acquire(std::string_view partition, std::string_view file_id,
                             std::string_view owner_instance_id,
                             std::int64_t ttl_millis) override {
    std::lock_guard<std::mutex> guard(mutex_);
    const std::string key = std::string(partition) + "\x1f" + std::string(file_id);
    const auto it = leases_.find(key);
    if (it != leases_.end() && it->second.expires_at_epoch_millis > now_millis_) {
      return Err(fss::ErrorKind::kUnavailable, "租约已被占用");
    }
    Lease lease;
    lease.file_id = std::string(file_id);
    lease.owner_instance_id = std::string(owner_instance_id);
    lease.expires_at_epoch_millis = now_millis_ + ttl_millis;
    leases_[key] = lease;
    return lease;
  }

  fss::Result<void> Renew(std::string_view partition, std::string_view file_id,
                          std::string_view owner_instance_id, std::int64_t ttl_millis) override {
    std::lock_guard<std::mutex> guard(mutex_);
    const std::string key = std::string(partition) + "\x1f" + std::string(file_id);
    const auto it = leases_.find(key);
    if (it == leases_.end() || it->second.owner_instance_id != owner_instance_id) {
      return Err(fss::ErrorKind::kNotFound, "租约不存在或不属于该实例");
    }
    it->second.expires_at_epoch_millis = now_millis_ + ttl_millis;
    return Ok();
  }

  fss::Result<void> Release(std::string_view partition, std::string_view file_id,
                            std::string_view owner_instance_id) override {
    std::lock_guard<std::mutex> guard(mutex_);
    const std::string key = std::string(partition) + "\x1f" + std::string(file_id);
    const auto it = leases_.find(key);
    if (it == leases_.end()) return Err(fss::ErrorKind::kNotFound, "租约不存在");
    if (it->second.owner_instance_id != owner_instance_id) {
      return Err(fss::ErrorKind::kPermissionDenied, "租约属于别的实例");
    }
    leases_.erase(it);
    return Ok();
  }

  //  ★ 原子领取：一次调用内"挑选 + 迁移 owner"，并发下同一条只会被一个实例领走
  fss::Result<std::vector<Lease>> ClaimExpired(std::string_view partition, int limit,
                                               std::string_view claimant_instance_id) override {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Lease> claimed;
    const std::string prefix = std::string(partition) + "\x1f";
    for (auto& [key, lease] : leases_) {
      if (claimed.size() >= static_cast<std::size_t>(limit)) break;
      if (key.rfind(prefix, 0) != 0) continue;
      if (lease.expires_at_epoch_millis > now_millis_) continue;
      lease.owner_instance_id = std::string(claimant_instance_id);
      lease.expires_at_epoch_millis = now_millis_ + 60000;
      claimed.push_back(lease);
    }
    return claimed;
  }

  void SetNowMillis(std::int64_t now) {
    std::lock_guard<std::mutex> guard(mutex_);
    now_millis_ = now;
  }

 private:
  std::mutex mutex_;
  std::map<std::string, Lease> leases_;
  std::int64_t now_millis_ = 1700000000000;
};

}  // namespace fss::test
