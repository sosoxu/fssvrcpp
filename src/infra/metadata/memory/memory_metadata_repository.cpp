// InMemoryMetadataRepository 实现。语义登记表见头文件与 tests/framework/port_contract.h。
#include "infra/metadata/memory/memory_metadata_repository.h"

#include <algorithm>
#include <cstddef>
#include <set>

namespace fss::infra {

namespace {

fss::Error Invalid(const std::string& message) {
  return Err(fss::ErrorKind::kInvalidArgument, message);
}
fss::Error NotFound(const std::string& message) {
  return Err(fss::ErrorKind::kNotFound, message);
}

const std::string& FileSourceOf(const domain::FileMetadataRecord& record) {
  return record.data.dataset_properties.file_source_info.file_source;
}

//  公共前置校验：partition 非空；record.id 非空且租户前缀与 partition 一致。
//  租户前缀必须校验 —— 否则可以用 A 租户的记录去写 B 租户的库（隔离被绕过）。
fss::Result<void> ValidatePartitionAndId(std::string_view partition,
                                         const domain::FileMetadataRecord& record) {
  if (partition.empty()) return Invalid("partition 不能为空");
  if (record.id.empty()) return Invalid("record.id 不能为空");
  if (record.Partition() != partition) {
    return Invalid("record.id 的租户前缀与 partition 参数不一致");
  }
  return Ok();
}

}  // namespace

const InMemoryMetadataRepository::Chain* InMemoryMetadataRepository::FindChain(
    std::string_view partition, std::string_view record_id) const {
  const auto bucket = by_id_.find(std::string(partition));
  if (bucket == by_id_.end()) return nullptr;
  const auto chain = bucket->second.find(std::string(record_id));
  if (chain == bucket->second.end()) return nullptr;
  return &chain->second;
}

fss::Result<domain::FileMetadataRecord> InMemoryMetadataRepository::Create(
    std::string_view partition, const domain::FileMetadataRecord& record) {
  FSS_TRY(ValidatePartitionAndId(partition, record));
  //  ★ C1：`Create` 用新原语实现（claim → mark ready）。可观测语义与接线前逐字一致：
  //    幂等键相同 → 返回**第一次那条**；否则写入 v1 ready 记录。
  FSS_TRY(claim, ClaimForWrite(partition, record));
  if (!claim.claimed) {
    //  claimed=false：既有记录（ready 或**另一个实例正在 claiming**）。两者都返回既有 id
    //  —— 旧 `Create` 的判据是"同幂等键返回同一条"，这一点没有改变。
    return claim.record;
  }
  //  ★ `MarkReady` 落库**最终** record（内容 = 调用方传入的 v1 记录），并翻到 ready。
  return MarkReady(partition, claim.record.id, claim.record.version, claim.record);
}

fss::Result<domain::MetadataClaim> InMemoryMetadataRepository::ClaimForWrite(
    std::string_view partition, const domain::FileMetadataRecord& record) {
  std::lock_guard<std::mutex> guard(mutex_);
  FSS_TRY(ValidatePartitionAndId(partition, record));
  const std::string part(partition);
  const std::string file_source = FileSourceOf(record);
  if (file_source.empty()) {
    return Invalid("file_source 不能为空（它是创建幂等键）");
  }

  // ★ R5/ADR-009 §4.2：已有**活动记录**（ready 或 claiming）→ 不插入、不复制、不报错。
  const SourceKey source_key{part, file_source};
  const auto existing = source_index_.find(source_key);
  if (existing != source_index_.end()) {
    const Chain* chain = FindChain(partition, existing->second);
    if (chain != nullptr && !chain->empty()) {
      domain::MetadataClaim out;
      out.claimed = false;
      out.record = chain->back().record;
      out.state = chain->back().state;
      return out;
    }
  }

  //  同 id 但 file_source 不同 → 冲突（一个 record id 只能属于一个幂等键）。
  //  ★ 这个守卫刻意**不过滤 state**：它是"一个 id 一条链"的**不变量检查**，不是客户端读取
  //    路径（不会把记录交给客户端）。若按 ready 过滤，内存实现会把第二条链追加到同一个 id
  //    下（内存没有主键约束兜底）。SQLite/PG 的等义效果由主键/唯一约束给出，报同一种
  //    kInvalidArgument（见各实现与契约用例）。
  auto& bucket = by_id_[part];
  if (bucket.find(record.id) != bucket.end()) {
    return Invalid("同 id 已存在且 file_source 不同：" + record.id);
  }

  Version version;
  version.record = record;
  version.record.version = 1;
  version.created_at_epoch_seconds = clock_.NowEpochSeconds();
  version.is_latest = true;
  version.state = domain::MetadataState::kClaiming;  // ★ 领取：对读取路径不可见
  bucket[record.id].push_back(std::move(version));
  source_index_[source_key] = record.id;

  domain::MetadataClaim out;
  out.claimed = true;
  out.record = by_id_[part][record.id].back().record;
  out.state = domain::MetadataState::kClaiming;
  return out;
}

fss::Result<domain::FileMetadataRecord> InMemoryMetadataRepository::MarkReady(
    std::string_view partition, std::string_view record_id, std::int64_t version,
    const domain::FileMetadataRecord& record) {
  std::lock_guard<std::mutex> guard(mutex_);
  FSS_TRY(ValidatePartitionAndId(partition, record));
  //  ★ `record_id` / `version`（列语义）是权威的：记录里的 id/version 只是同一份数据的拷贝，
  //    读路径也是用列覆盖 JSON。因此这里不做"record.version 必须等于 version"的拒绝
  //    （那会把"版本不存在 → kNotFound"的契约判据变成 kInvalidArgument）。
  const std::string part(partition);
  const auto bucket = by_id_.find(part);
  if (bucket == by_id_.end()) return NotFound("Record Not Found");
  const auto found = bucket->second.find(std::string(record_id));
  if (found == bucket->second.end()) return NotFound("Record Not Found");
  Chain& chain = found->second;
  for (auto it = chain.begin(); it != chain.end(); ++it) {
    if (it->record.version != version) continue;
    if (it->state != domain::MetadataState::kClaiming) {
      return NotFound("Record Not Found（该版本不是 claiming 状态）");
    }
    //  ★ 幂等键必须稳定：允许补 final 数据（checksum 等），不允许改写 file_source。
    if (FileSourceOf(it->record) != FileSourceOf(record)) {
      return Invalid("MarkReady 不得改写 (partition, file_source) 幂等键");
    }
    it->record = record;
    it->record.id = std::string(record_id);
    it->record.version = version;
    it->state = domain::MetadataState::kReady;  // ★ 对读取路径可见
    return it->record;
  }
  return NotFound("Record Not Found");
}

fss::Result<void> InMemoryMetadataRepository::ReleaseClaim(std::string_view partition,
                                                           std::string_view record_id,
                                                           std::int64_t version) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (partition.empty()) return Invalid("partition 不能为空");
  const std::string part(partition);
  const auto bucket = by_id_.find(part);
  if (bucket == by_id_.end()) return NotFound("Record Not Found");
  const auto found = bucket->second.find(std::string(record_id));
  if (found == bucket->second.end()) return NotFound("Record Not Found");
  Chain& chain = found->second;
  for (auto it = chain.begin(); it != chain.end(); ++it) {
    if (it->record.version != version) continue;
    if (it->state != domain::MetadataState::kClaiming) {
      //  只删 claiming 行：ready/deleted 的行绝不能被"放弃领取"顺手删掉。
      return NotFound("Record Not Found（该版本不是 claiming 状态）");
    }
    const std::string file_source = FileSourceOf(it->record);
    chain.erase(it);
    //  R10：清理顺序 —— 只剩空链时同步删掉幂等键索引，file_source 才能被重新领取。
    if (chain.empty()) {
      source_index_.erase(SourceKey{part, file_source});
      bucket->second.erase(found);
    }
    return Ok();
  }
  return NotFound("Record Not Found");
}

//  ★ C2（ADR-009 §4.2/§4.3）：回收"崩溃的领取者"留下的 claiming 行。
//  与 `ReleaseClaim` 同一套清理纪律（R10：记录与幂等键索引必须同时消失），
//  区别只在**判据**：这里按 `(file_source ∈ live_expired_sources) ∧ created_at <= 阈值`
//  批量回收，而不是按 (id, version)。`live_expired_sources` 由 GC 从 ClaimExpired 的结果
//  取得 —— 它是"租约已到期并被本 GC 原子领取"的**唯一**证据；空集合 ⇒ 一条都不回收。
fss::Result<std::int64_t> InMemoryMetadataRepository::ReclaimStaleClaiming(
    std::string_view partition, std::int64_t older_than_epoch_seconds, int limit,
    const std::vector<std::string>& live_expired_sources) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (partition.empty()) return Invalid("partition 不能为空");
  if (limit <= 0) return std::int64_t{0};
  if (live_expired_sources.empty()) return std::int64_t{0};  // ★ 没有"已领取的过期租约" → 不凭年龄误删
  const std::set<std::string> allowed(live_expired_sources.begin(), live_expired_sources.end());

  const std::string part(partition);
  const auto bucket = by_id_.find(part);
  if (bucket == by_id_.end()) return std::int64_t{0};

  std::int64_t reclaimed = 0;
  //  先收集再删除：链在被遍历时不能改（迭代器失效）。claiming 行至多一条（v1 + latest）。
  std::vector<std::pair<std::string, std::int64_t>> victims;
  for (const auto& [record_id, chain] : bucket->second) {
    if (static_cast<int>(victims.size()) >= limit) break;
    for (const auto& version : chain) {
      if (version.state != domain::MetadataState::kClaiming) continue;
      if (version.created_at_epoch_seconds > older_than_epoch_seconds) continue;
      if (allowed.count(FileSourceOf(version.record)) == 0) continue;
      victims.emplace_back(record_id, version.record.version);
      break;  // 一条 claiming 行
    }
  }
  for (const auto& [record_id, version] : victims) {
    if (reclaimed >= limit) break;
    const auto found = bucket->second.find(record_id);
    if (found == bucket->second.end()) continue;
    Chain& chain = found->second;
    for (auto it = chain.begin(); it != chain.end(); ++it) {
      if (it->record.version != version) continue;
      if (it->state != domain::MetadataState::kClaiming) break;  // 期间被 MarkReady → 放过
      const std::string file_source = FileSourceOf(it->record);
      chain.erase(it);
      if (chain.empty()) {
        source_index_.erase(SourceKey{part, file_source});
        bucket->second.erase(found);
      }
      ++reclaimed;
      break;
    }
  }
  return reclaimed;
}

fss::Result<domain::FileMetadataRecord> InMemoryMetadataRepository::GetById(
    std::string_view partition, std::string_view record_id) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Chain* chain = FindChain(partition, record_id);
  if (chain == nullptr || chain->empty()) {
    return NotFound("Record Not Found");
  }
  //  ★ C1：只把 `ready` 交给客户端 —— 另一个实例正在 claiming 的行**必须不可见**，
  //    否则客户端会把"还没登记完（对象可能还没复制）"的记录当成已完成。
  if (chain->back().state != domain::MetadataState::kReady) {
    return NotFound("Record Not Found");
  }
  return chain->back().record;  // back() 即 latest（按版本号追加）
}

fss::Result<domain::FileMetadataRecord> InMemoryMetadataRepository::GetLatestByFileSource(
    std::string_view partition, std::string_view file_source) {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto owner =
      source_index_.find(SourceKey{std::string(partition), std::string(file_source)});
  if (owner == source_index_.end()) {
    return NotFound("Record Not Found");
  }
  //  ★ 不能调用 `GetById(...)`：它也要拿同一把**非递归**互斥量 → 自死锁
  //    （实测：并发用例挂死；加锁的类里"公开方法互调"是第一个要审的地方）
  const Chain* chain = FindChain(partition, owner->second);
  if (chain == nullptr || chain->empty()) {
    return NotFound("Record Not Found");
  }
  //  ★ C1：claiming 行不可见（与 GetById 同一纪律）
  if (chain->back().state != domain::MetadataState::kReady) {
    return NotFound("Record Not Found");
  }
  return chain->back().record;
}

fss::Result<domain::FileMetadataRecord> InMemoryMetadataRepository::Update(
    std::string_view partition, const domain::FileMetadataRecord& record) {
  std::lock_guard<std::mutex> guard(mutex_);
  FSS_TRY(ValidatePartitionAndId(partition, record));

  const std::string part(partition);
  const Chain* existing_chain = FindChain(partition, record.id);
  if (existing_chain == nullptr || existing_chain->empty()) {
    return NotFound("Record Not Found");
  }
  //  ★ C1：`Update` 的"既有记录"查找同样只认 ready —— 否则 Update 会去改一条
  //    另一个实例正在 claiming（还没有最终数据）的行。
  if (existing_chain->back().state != domain::MetadataState::kReady) {
    return NotFound("Record Not Found");
  }
  const std::string file_source = FileSourceOf(record);
  if (file_source.empty()) {
    return Invalid("file_source 不能为空（它是创建幂等键）");
  }
  // 幂等键必须稳定：改 file_source 等于把记录搬到另一条键上，会破坏唯一性语义
  if (file_source != FileSourceOf(existing_chain->back().record)) {
    return Invalid("Update 不得改写 (partition, file_source) 幂等键");
  }

  auto& chain = by_id_[part][record.id];
  Version version;
  version.record = record;
  version.record.version = chain.back().record.version + 1;
  version.created_at_epoch_seconds = clock_.NowEpochSeconds();
  version.is_latest = true;
  for (auto& old : chain) old.is_latest = false;  // R6：latest 标记随新版本迁移
  chain.push_back(std::move(version));
  return chain.back().record;
}

fss::Result<void> InMemoryMetadataRepository::Delete(std::string_view partition,
                                                     std::string_view record_id) {
  std::lock_guard<std::mutex> guard(mutex_);
  Chain* chain = nullptr;
  {
    const auto bucket = by_id_.find(std::string(partition));
    if (bucket == by_id_.end()) return NotFound("Record Not Found");
    const auto it = bucket->second.find(std::string(record_id));
    if (it == bucket->second.end()) return NotFound("Record Not Found");
    chain = &it->second;
  }
  if (chain->empty()) return NotFound("Record Not Found");
  // 先删索引再删记录（R10：清理顺序 —— 两者必须同时消失）
  source_index_.erase(SourceKey{std::string(partition), FileSourceOf(chain->back().record)});
  by_id_[std::string(partition)].erase(std::string(record_id));
  return Ok();
}

fss::Result<domain::MetadataPage> InMemoryMetadataRepository::List(
    std::string_view partition, const domain::MetadataQuery& query) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (query.limit <= 0) return Invalid("limit 必须 > 0");
  if (query.offset < 0) return Invalid("offset 必须 >= 0");

  domain::MetadataPage page;
  const auto bucket = by_id_.find(std::string(partition));
  if (bucket == by_id_.end()) {
    page.total = 0;
    return page;
  }

  struct Row {
    const domain::FileMetadataRecord* record;
    std::int64_t created_at;
  };
  std::vector<Row> matched;
  for (const auto& [record_id, chain] : bucket->second) {
    (void)record_id;
    if (chain.empty()) continue;
    const Version& latest = chain.back();
    if (!latest.is_latest) continue;  // 只列 latest（版本链对外是一条记录）
    //  ★ C1：List 只列 ready —— claiming 行对客户端不可见
    if (latest.state != domain::MetadataState::kReady) continue;

    if (query.kind.has_value() && latest.record.kind != *query.kind) continue;
    if (query.name_prefix.has_value()) {
      const auto& name = latest.record.data.name;
      if (!name.has_value() || name->rfind(*query.name_prefix, 0) != 0) continue;
    }
    if (query.created_after_epoch_seconds >= 0 &&
        latest.created_at_epoch_seconds < query.created_after_epoch_seconds) {
      continue;  // 含下界
    }
    if (query.created_before_epoch_seconds >= 0 &&
        latest.created_at_epoch_seconds > query.created_before_epoch_seconds) {
      continue;  // 含上界
    }
    matched.push_back(Row{&latest.record, latest.created_at_epoch_seconds});
  }

  // 稳定全序：created_at 升序，同秒按 id 升序（分页不漏不重）
  std::sort(matched.begin(), matched.end(), [](const Row& a, const Row& b) {
    if (a.created_at != b.created_at) return a.created_at < b.created_at;
    return a.record->id < b.record->id;
  });

  page.total = static_cast<std::int64_t>(matched.size());
  const std::size_t begin = static_cast<std::size_t>(query.offset);
  for (std::size_t i = begin; i < matched.size(); ++i) {
    if (static_cast<int>(page.records.size()) >= query.limit) break;
    page.records.push_back(*matched[i].record);
  }
  return page;
}

std::size_t InMemoryMetadataRepository::version_count(std::string_view partition,
                                                      std::string_view record_id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const Chain* chain = FindChain(partition, record_id);
  return chain == nullptr ? 0 : chain->size();
}

}  // namespace fss::infra
