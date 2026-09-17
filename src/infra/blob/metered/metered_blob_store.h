// =============================================================================
//  MeteredBlobStore（L2）—— 给任意 `IBlobStore` 套上"操作数 + 字节数"计数
// =============================================================================
//  为什么要**装饰**而不是在每个驱动里各写一遍计数：
//    驱动有 4 个（POSIX / S3 / 内存 / 测试替身），"posix 记了、s3 忘了"这种漂移
//    在指标上表现为"某个环境的存储指标是空的"，而且不会有任何测试失败。
//    装饰器只有一份实现 → 所有驱动自动获得同样的指标。
//
//  ★ 字节数用**包装 ByteSource/ByteSink** 统计（流式路径不能被为了计数而整块读回）。
// =============================================================================
#pragma once

#include "common/metrics/metrics.h"
#include "domain/ports/ports.h"

#include <string>

namespace fss::infra {

class MeteredBlobStore final : public domain::IBlobStore {
 public:
  MeteredBlobStore(domain::IBlobStore& inner, fss::metrics::Registry& registry,
                   std::string driver_name);

  domain::BlobCapabilities capabilities() const override { return inner_.capabilities(); }
  fss::Result<void> ensure_container(const std::string& container) override;
  fss::Result<domain::SignedLocation> presign_put(const domain::ObjectRef& ref,
                                                 const domain::PresignOptions& options) override;
  fss::Result<domain::SignedLocation> presign_get(const domain::ObjectRef& ref,
                                                 const domain::PresignOptions& options) override;
  fss::Result<void> put(const domain::ObjectRef& ref, fss::bytes::ByteSource& source,
                        const domain::PutOptions& options) override;
  fss::Result<void> get(const domain::ObjectRef& ref, fss::bytes::ByteSink& sink,
                        const domain::ByteRange& range) override;
  fss::Result<domain::ObjectStat> stat(const domain::ObjectRef& ref) override;
  fss::Result<void> remove(const domain::ObjectRef& ref) override;
  fss::Result<domain::ObjectStat> copy(const domain::ObjectRef& from,
                                       const domain::ObjectRef& to) override;
  fss::Result<domain::ListPage> list(const std::string& container, const std::string& prefix,
                                     const std::string& continuation_token, int limit) override;

  //  C9.25：临时文件清理也要计数（`op=remove_temp_files`）
  fss::Result<domain::TempSweepResult> remove_temp_files(const std::string& container,
                                                 std::int64_t older_than_epoch_seconds,
                                                 bool dry_run) override;

 private:
  //  成功/失败各记一次：`fss_storage_operations_total{driver,op,outcome}`
  void Record(std::string_view op, bool ok) const;

  domain::IBlobStore& inner_;
  fss::metrics::Registry& registry_;
  std::string driver_;
};

}  // namespace fss::infra
