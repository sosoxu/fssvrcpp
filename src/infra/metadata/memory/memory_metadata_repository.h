// =============================================================================
//  InMemoryMetadataRepository（L2，测试用）—— IMetadataRepository 的内存实现
// =============================================================================
//  ★ 语义（与 tests/framework/port_contract.h 的登记表逐条对应，并对应 R5/R6）
//    · **幂等键 = `(partition, file_source)`**（R5）：`Create` 对同一幂等键重复调用
//      必须返回**第一次那条记录**，既不新建记录也不新增版本。这样"客户端重试"
//      与"多实例并发创建"都不会产生重复记录（ADR-009 的 M2 实测 20/20 复现）。
//    · **版本链**（R6）：`Update` 在**同一** record id 上 `version + 1`，并把
//      `is_latest` 从旧版本移到新版本；`List` 只返回 latest，避免"版本链看起来像
//      多条记录"。`Update` 不允许改写 `file_source`（幂等键必须稳定）。
//    · **partition_id 严格隔离**：所有索引都按 partition 分桶；`Create` 还校验
//      `record.id` 的租户前缀与 `partition` 参数一致，堵住"用 A 的记录写 B 的库"。
//    · `created_at` 由注入的 `IClock` 决定（记录模型里没有时间字段，时间属于
//      仓储的元数据）—— 因此 `List` 的时间区间过滤是可确定性测试的（C2.8）。
//    · 排序：`created_at` 升序、同秒按 `id` 升序 —— 分页必须有**稳定**全序，
//      否则 offset/limit 会漏项或重项。
//    · **线程安全**（内部一把互斥量）：组合根在 `single` 模式下就是用它，HTTP 的
//      并发请求会在多线程里同时打进来；没有锁时并发 `Create` 会破坏 `std::map` 的
//      内部结构（实测堆损坏），而不是给出错误（P6-D16）。
#pragma once

#include "common/result/result.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"

#include <cstdint>
#include <mutex>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fss::infra {

class InMemoryMetadataRepository final : public domain::IMetadataRepository {
 public:
  explicit InMemoryMetadataRepository(const fss::IClock& clock) : clock_(clock) {}

  fss::Result<domain::FileMetadataRecord> Create(std::string_view partition,
                                                 const domain::FileMetadataRecord& record) override;
  fss::Result<domain::MetadataClaim> ClaimForWrite(
      std::string_view partition, const domain::FileMetadataRecord& record) override;
  fss::Result<domain::FileMetadataRecord> MarkReady(
      std::string_view partition, std::string_view record_id, std::int64_t version,
      const domain::FileMetadataRecord& record) override;
  fss::Result<void> ReleaseClaim(std::string_view partition, std::string_view record_id,
                                 std::int64_t version) override;
  fss::Result<std::int64_t> ReclaimStaleClaiming(
      std::string_view partition, std::int64_t older_than_epoch_seconds, int limit,
      const std::vector<std::string>& live_expired_sources) override;
  fss::Result<domain::FileMetadataRecord> GetById(std::string_view partition,
                                                  std::string_view record_id) override;
  fss::Result<domain::FileMetadataRecord> GetLatestByFileSource(
      std::string_view partition, std::string_view file_source) override;
  fss::Result<domain::FileMetadataRecord> Update(std::string_view partition,
                                                 const domain::FileMetadataRecord& record) override;
  fss::Result<void> Delete(std::string_view partition, std::string_view record_id) override;
  fss::Result<domain::MetadataPage> List(std::string_view partition,
                                         const domain::MetadataQuery& query) override;

  // 测试可观测性：某个 record id 的版本数
  std::size_t version_count(std::string_view partition, std::string_view record_id) const;

 private:
  mutable std::mutex mutex_;

  struct Version {
    domain::FileMetadataRecord record;
    std::int64_t created_at_epoch_seconds = 0;
    bool is_latest = false;
    //  ★ C1（ADR-009 §4.2）：写入状态机。它**不是**记录模型的一部分（不进 JSON），
    //    只决定该版本对"读取路径"是否可见（只有 ready 可见）。
    domain::MetadataState state = domain::MetadataState::kReady;
  };
  using Chain = std::vector<Version>;
  using RecordId = std::string;
  using Partition = std::string;
  using SourceKey = std::pair<Partition, std::string>;

  std::map<Partition, std::map<RecordId, Chain>> by_id_;
  std::map<SourceKey, RecordId> source_index_;  // (partition, file_source) → record id

  const fss::IClock& clock_;

  const Chain* FindChain(std::string_view partition, std::string_view record_id) const;
};

}  // namespace fss::infra
