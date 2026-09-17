// InMemoryLocationRepository 实现。语义登记表见头文件与 tests/framework/port_contract.h。
#include "infra/location/memory/memory_location_repository.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace fss::infra {

namespace {
fss::Error Invalid(const std::string& message) {
  return Err(fss::ErrorKind::kInvalidArgument, message);
}
fss::Error NotFound(const std::string& message) {
  return Err(fss::ErrorKind::kNotFound, message);
}
}  // namespace

const InMemoryLocationRepository::ById* InMemoryLocationRepository::FindPartition(
    std::string_view partition) const {
  const auto it = by_id_.find(std::string(partition));
  return it == by_id_.end() ? nullptr : &it->second;
}

InMemoryLocationRepository::ById* InMemoryLocationRepository::FindPartition(
    std::string_view partition) {
  const auto it = by_id_.find(std::string(partition));
  return it == by_id_.end() ? nullptr : &it->second;
}

fss::Result<void> InMemoryLocationRepository::Save(std::string_view partition,
                                                   const domain::FileLocation& location) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (partition.empty()) return Invalid("partition 不能为空");
  if (location.file_id.empty()) return Invalid("file_id 不能为空");
  if (location.file_source.empty()) return Invalid("file_source 不能为空");

  const std::string part(partition);
  const std::string file_source(location.file_source);

  auto& bucket = by_id_[part];
  const auto existing = bucket.find(location.file_id);

  // 幂等键校验：该 file_source 是否已被**别的** file_id 占用
  const SourceKey key{part, file_source};
  const auto owner = source_index_.find(key);
  if (owner != source_index_.end() && owner->second != location.file_id) {
    return Err(fss::ErrorKind::kLocationAlreadyExists,
               "file_source 已被另一个 file_id 占用：" + file_source);
  }

  // upsert：同 file_id 改 file_source 时，先释放旧键（R10：先删数据再重建约束）
  if (existing != bucket.end() && existing->second.file_source != file_source) {
    source_index_.erase(SourceKey{part, existing->second.file_source});
  }

  bucket[location.file_id] = location;
  source_index_[key] = location.file_id;
  return Ok();
}

fss::Result<domain::FileLocation> InMemoryLocationRepository::Find(
    std::string_view partition, std::string_view file_id) {
  std::lock_guard<std::mutex> guard(mutex_);
  const ById* bucket = FindPartition(partition);
  if (bucket == nullptr) return NotFound("位置记录不存在：partition=" + std::string(partition));
  const auto it = bucket->find(std::string(file_id));
  if (it == bucket->end()) {
    return NotFound("Not found location for fileID : " + std::string(file_id));
  }
  return it->second;
}

fss::Result<void> InMemoryLocationRepository::UpdateSignedUrl(
    std::string_view partition, std::string_view file_id, std::string_view signed_url,
    std::int64_t updated_at_epoch_seconds) {
  std::lock_guard<std::mutex> guard(mutex_);
  ById* bucket = FindPartition(partition);
  if (bucket == nullptr) return NotFound("位置记录不存在：" + std::string(file_id));
  const auto it = bucket->find(std::string(file_id));
  if (it == bucket->end()) return NotFound("位置记录不存在：" + std::string(file_id));
  it->second.signed_url = std::string(signed_url);
  it->second.updated_at_epoch_seconds = updated_at_epoch_seconds;
  return Ok();
}

fss::Result<void> InMemoryLocationRepository::Delete(std::string_view partition,
                                                     std::string_view file_id) {
  std::lock_guard<std::mutex> guard(mutex_);
  ById* bucket = FindPartition(partition);
  if (bucket == nullptr) return NotFound("位置记录不存在：" + std::string(file_id));
  const auto it = bucket->find(std::string(file_id));
  if (it == bucket->end()) return NotFound("位置记录不存在：" + std::string(file_id));

  // 先删数据（源索引），再删记录本身 —— 两个索引必须同时清理
  source_index_.erase(SourceKey{std::string(partition), it->second.file_source});
  bucket->erase(it);
  return Ok();
}

fss::Result<domain::FileLocation> InMemoryLocationRepository::FindByFileSource(
    std::string_view partition, std::string_view file_source) {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto owner = source_index_.find(SourceKey{std::string(partition), std::string(file_source)});
  if (owner == source_index_.end()) {
    return NotFound("位置记录不存在（按 file_source）：" + std::string(file_source));
  }
  //  ★ 这里**不能**调用 `Find(partition, owner->second)`：它也会去拿同一把**非递归**
  //    互斥量 → 自死锁（实测：两个实例并发时整个进程挂死）。查询逻辑内联一份。
  const ById* bucket = FindPartition(partition);
  if (bucket == nullptr) {
    return NotFound("位置记录不存在：partition=" + std::string(partition));
  }
  const auto it = bucket->find(owner->second);
  if (it == bucket->end()) {
    return NotFound("Not found location for fileID : " + owner->second);
  }
  return it->second;
}

std::size_t InMemoryLocationRepository::size(std::string_view partition) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const ById* bucket = FindPartition(partition);
  return bucket == nullptr ? 0 : bucket->size();
}

fss::Result<domain::LocationPage> InMemoryLocationRepository::List(
    std::string_view partition, const domain::LocationQuery& query) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (query.limit <= 0) return Invalid("limit 必须 > 0");
  if (query.offset < 0) return Invalid("offset 必须 >= 0");

  domain::LocationPage page;
  const ById* bucket = FindPartition(partition);
  if (bucket == nullptr) {
    page.total = 0;
    return page;
  }

  //  过滤（时间边界含端点；-1 = 无界；user_id 空 = 不过滤）
  std::vector<const domain::FileLocation*> matched;
  for (const auto& [file_id, location] : *bucket) {
    (void)file_id;
    if (!query.user_id.empty() && location.user_id != query.user_id) continue;
    if (query.created_after_epoch_seconds >= 0 &&
        location.created_at_epoch_seconds < query.created_after_epoch_seconds) {
      continue;
    }
    if (query.created_before_epoch_seconds >= 0 &&
        location.created_at_epoch_seconds > query.created_before_epoch_seconds) {
      continue;
    }
    matched.push_back(&location);
  }

  //  稳定全序：created_at 升序，同秒按 file_id 升序（否则 offset 分页会漏/重）
  std::sort(matched.begin(), matched.end(),
            [](const domain::FileLocation* a, const domain::FileLocation* b) {
              if (a->created_at_epoch_seconds != b->created_at_epoch_seconds) {
                return a->created_at_epoch_seconds < b->created_at_epoch_seconds;
              }
              return a->file_id < b->file_id;
            });

  page.total = static_cast<std::int64_t>(matched.size());
  const std::size_t begin = static_cast<std::size_t>(query.offset);
  for (std::size_t i = begin; i < matched.size(); ++i) {
    if (static_cast<int>(page.records.size()) >= query.limit) break;
    page.records.push_back(*matched[i]);
  }
  return page;
}

}  // namespace fss::infra
