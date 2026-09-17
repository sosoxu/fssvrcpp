// =============================================================================
//  InMemoryLocationRepository（L2，测试用）—— IFileLocationRepository 的内存实现
// =============================================================================
//  ★ 语义（与 tests/framework/port_contract.h 的登记表逐条对应）
//    · `Save` 按 `file_id` **upsert**：同 file_id 再次 Save 覆盖（zone 可变）——
//      `CreateFileMetadata` 的"staging → persistent"更新依赖这一点；端口里没有
//      `UpdateZone`，若 Save 是纯插入则该流程无法表达（见证据文件的取舍记录）。
//    · `(partition, file_source)` 是**幂等键**，唯一（R5）：已被别的 file_id 占用时
//      返回 `kLocationAlreadyExists`（上游映射 400 ALREADY_EXISTS）。
//    · **partition_id 严格隔离**：一切查询都先按 partition 分桶，跨租户一律 miss。
//      这是相对上游 `file_locations_osm`（只按 id 全局唯一）的安全加固
//      （docs/02-design.md §9.1）。
//    · 删除时必须**同时**清理 id 桶与 file_source 索引（R10：先删数据再重建约束），
//      否则 file_source 将被永久占用。
#pragma once

#include "common/result/result.h"
#include "domain/ports/ports.h"

#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace fss::infra {

class InMemoryLocationRepository final : public domain::IFileLocationRepository {
 public:
  fss::Result<void> Save(std::string_view partition,
                         const domain::FileLocation& location) override;
  fss::Result<domain::FileLocation> Find(std::string_view partition,
                                         std::string_view file_id) override;
  fss::Result<void> UpdateSignedUrl(std::string_view partition, std::string_view file_id,
                                    std::string_view signed_url,
                                    std::int64_t updated_at_epoch_seconds) override;
  fss::Result<void> Delete(std::string_view partition, std::string_view file_id) override;
  fss::Result<domain::FileLocation> FindByFileSource(std::string_view partition,
                                                     std::string_view file_source) override;
  fss::Result<domain::LocationPage> List(std::string_view partition,
                                         const domain::LocationQuery& query) override;

  // 测试可观测性
  std::size_t size(std::string_view partition) const;

 private:
  using FileId = std::string;
  using Partition = std::string;
  using SourceKey = std::pair<Partition, std::string>;

  using ById = std::map<FileId, domain::FileLocation>;
  std::map<Partition, ById> by_id_;                 // partition → file_id → 记录
  std::map<SourceKey, FileId> source_index_;        // (partition, file_source) → file_id（唯一）

  const ById* FindPartition(std::string_view partition) const;
  ById* FindPartition(std::string_view partition);
};

}  // namespace fss::infra
