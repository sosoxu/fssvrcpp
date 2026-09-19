// InMemoryLeaseRepository 实现。语义与边界见头文件。
#include "infra/location/memory/memory_lease_repository.h"

#include <utility>

namespace fss::infra {

std::string InMemoryLeaseRepository::Key(std::string_view partition, std::string_view file_id) {
  return std::string(partition) + "\x1f" + std::string(file_id);
}

fss::Result<domain::ILeaseRepository::Lease> InMemoryLeaseRepository::Acquire(
    std::string_view partition, std::string_view file_id, std::string_view owner_instance_id,
    std::int64_t ttl_millis) {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::string key = Key(partition, file_id);
  const std::int64_t now = clock_.NowEpochMillis();
  const auto it = leases_.find(key);
  if (it != leases_.end() && it->second.expires_at_epoch_millis > now) {
    return Err(fss::ErrorKind::kUnavailable, "租约已被占用");
  }
  Lease lease;
  lease.file_id = std::string(file_id);
  lease.owner_instance_id = std::string(owner_instance_id);
  lease.expires_at_epoch_millis = now + ttl_millis;
  leases_[key] = lease;
  return lease;
}

fss::Result<void> InMemoryLeaseRepository::Renew(std::string_view partition,
                                                 std::string_view file_id,
                                                 std::string_view owner_instance_id,
                                                 std::int64_t ttl_millis) {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto it = leases_.find(Key(partition, file_id));
  if (it == leases_.end()) return Err(fss::ErrorKind::kNotFound, "租约不存在");
  //  ★ A1：把"不存在"（kNotFound）与"不是你的"（kPermissionDenied）**分开**。
  //    旧版把两者都折叠成 kNotFound —— 调用方无法区分"租约没了"与"抢错了"，
  //    而 Release 一直是后者用 kPermissionDenied，端口语义本来就要求可区分（R16）。
  //    端口契约测试 `CheckLeaseContract` 现在钉住这条（PG 实现同样如此）。
  if (it->second.owner_instance_id != owner_instance_id) {
    return Err(fss::ErrorKind::kPermissionDenied, "租约属于别的实例");
  }
  it->second.expires_at_epoch_millis = clock_.NowEpochMillis() + ttl_millis;
  return Ok();
}

fss::Result<void> InMemoryLeaseRepository::Release(std::string_view partition,
                                                   std::string_view file_id,
                                                   std::string_view owner_instance_id) {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto it = leases_.find(Key(partition, file_id));
  if (it == leases_.end()) return Err(fss::ErrorKind::kNotFound, "租约不存在");
  if (it->second.owner_instance_id != owner_instance_id) {
    return Err(fss::ErrorKind::kPermissionDenied, "租约属于别的实例");
  }
  leases_.erase(it);
  return Ok();
}

fss::Result<std::vector<domain::ILeaseRepository::Lease>> InMemoryLeaseRepository::ClaimExpired(
    std::string_view partition, int limit, std::string_view claimant_instance_id) {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<Lease> claimed;
  if (limit <= 0) return claimed;
  const std::string prefix = std::string(partition) + "\x1f";
  const std::int64_t now = clock_.NowEpochMillis();
  std::vector<std::string> expired_keys;
  for (auto& [key, lease] : leases_) {
    if (key.rfind(prefix, 0) != 0) continue;
    if (lease.expires_at_epoch_millis > now) continue;
    expired_keys.push_back(key);
  }
  for (const auto& key : expired_keys) {
    if (claimed.size() >= static_cast<std::size_t>(limit)) break;
    auto it = leases_.find(key);
    it->second.owner_instance_id = std::string(claimant_instance_id);
    //  领取后把过期时间往后推，避免下一轮同一实例把它当成"又过期了"重复领取
    //  （PG 版靠 `UPDATE ... SET owner, expires_at` 达成同样的语义）。
    it->second.expires_at_epoch_millis = now + 60000;
    claimed.push_back(it->second);
  }
  return claimed;
}

std::size_t InMemoryLeaseRepository::size(std::string_view partition) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::string prefix = std::string(partition) + "\x1f";
  std::size_t count = 0;
  for (const auto& [key, lease] : leases_) {
    (void)lease;
    if (key.rfind(prefix, 0) == 0) ++count;
  }
  return count;
}

}  // namespace fss::infra
