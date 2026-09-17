// InMemoryMetadataRepository 实现。语义登记表见头文件与 tests/framework/port_contract.h。
#include "infra/metadata/memory/memory_metadata_repository.h"

#include <algorithm>
#include <cstddef>

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
  std::lock_guard<std::mutex> guard(mutex_);
  FSS_TRY(ValidatePartitionAndId(partition, record));

  const std::string part(partition);
  const std::string file_source = FileSourceOf(record);
  if (file_source.empty()) {
    return Invalid("file_source 不能为空（它是创建幂等键）");
  }

  // ★ R5：先查幂等键 —— 已存在就直接返回**同一条记录**（第一次写入的内容获胜）
  const SourceKey source_key{part, file_source};
  const auto existing = source_index_.find(source_key);
  if (existing != source_index_.end()) {
    return by_id_[part][existing->second].back().record;
  }

  // 同 id 但 file_source 不同 → 冲突（一个 record id 只能属于一个幂等键）
  auto& bucket = by_id_[part];
  if (bucket.find(record.id) != bucket.end()) {
    return Invalid("record.id 已存在但 file_source 不同");
  }

  Version version;
  version.record = record;
  version.record.version = 1;
  version.created_at_epoch_seconds = clock_.NowEpochSeconds();
  version.is_latest = true;
  bucket[record.id].push_back(std::move(version));
  source_index_[source_key] = record.id;
  return by_id_[part][record.id].back().record;
}

fss::Result<domain::FileMetadataRecord> InMemoryMetadataRepository::GetById(
    std::string_view partition, std::string_view record_id) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Chain* chain = FindChain(partition, record_id);
  if (chain == nullptr || chain->empty()) {
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
