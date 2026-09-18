// =============================================================================
//  InMemoryLeaseRepository（L2）—— ILeaseRepository 的**单实例**内存实现
// =============================================================================
//  为什么在 src/infra/ 里有一份"生产用"的实现（而不是只放在 tests/framework）：
//    阶段 10 切片 2（C10.9）要求组合根**真的装配 GC 调度**。GC 的租约端口必须有
//    一个实现（端口不能为空），而 PG 版租约（ADR-009）尚未交付。单实例形态下
//    内存租约是**语义正确**的：进程内只有本实例在占用/回收。
//
//  ★ 边界（如实登记）：
//    · **仅单实例**：内存表不跨进程。`deployment.mode=multi` 时组合根**拒绝启动**
//      （ADR-009），因此不存在"以为共享、其实各存一份"的静默危险。
//    · 当前产品里没有任何用例会 `Acquire` 租约（上传路径尚未接租约；
//      `gc.require_lease_expiry=true` 时 GC 只走"领取过期租约"这条空路径）。
//      因此本实现的实际可观测效果是："GC 的租约路径是空的，不会误删"。
//      `.tmp.*` 残留的清理走 `IBlobStore::remove_temp_files`（与租约无关，C9.25）。
//    · 时间源是注入的 `IClock`（不是数据库 `now()`）：单实例下二者等价；
//      多实例必须换 PG 版（`leases.time_source=database`）。
//
//  ★ 线程安全（内部一把互斥量）：GC 在后台调度线程里 `ClaimExpired`，与可能的
//    其它线程并发；没有锁就会破坏 `std::map` 的内部结构（P9-D01 的同类教训）。
// =============================================================================
#pragma once

#include "common/result/result.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fss::infra {

class InMemoryLeaseRepository final : public domain::ILeaseRepository {
 public:
  explicit InMemoryLeaseRepository(const fss::IClock& clock) : clock_(clock) {}

  fss::Result<Lease> Acquire(std::string_view partition, std::string_view file_id,
                             std::string_view owner_instance_id,
                             std::int64_t ttl_millis) override;
  fss::Result<void> Renew(std::string_view partition, std::string_view file_id,
                          std::string_view owner_instance_id, std::int64_t ttl_millis) override;
  fss::Result<void> Release(std::string_view partition, std::string_view file_id,
                            std::string_view owner_instance_id) override;
  //  ★ 原子领取：一次调用内"挑选 + 迁移 owner"，并发 GC 下同一条只会被一个实例领走
  //    （PG 版对应 `FOR UPDATE SKIP LOCKED` / `ON CONFLICT`）。
  fss::Result<std::vector<Lease>> ClaimExpired(std::string_view partition, int limit,
                                               std::string_view claimant_instance_id) override;

  //  可观测性（测试/排障）：当前 partition 下的租约条数
  std::size_t size(std::string_view partition) const;

 private:
  static std::string Key(std::string_view partition, std::string_view file_id);

  const fss::IClock& clock_;
  mutable std::mutex mutex_;
  std::map<std::string, Lease> leases_;
};

}  // namespace fss::infra
