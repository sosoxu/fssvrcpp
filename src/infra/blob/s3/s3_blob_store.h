// =============================================================================
//  S3BlobStore（L2）—— S3 兼容对象存储驱动
// =============================================================================
//  设计要点（ADR-005，P5 定稿）：
//    · **不引入 AWS SDK**：签名用 `SigV4Signer`（OpenSSL），数据面用 libcurl；
//    · **原生预签名**（`capabilities().native_presign = true`）→ 客户端直连存储端点，
//      服务**不代理字节**（C5.8 的判据就是"`SignedURL` 指向存储端点"）；
//    · path-style / virtual-host 两种寻址由 `SigV4Options::force_path_style` 决定（C5.4）；
//    · 所有具体实现只在组合根创建（R12），本文件只提供"被装配"的类。
//
//  ★ 切片状态（P5 分 5 个切片）：
//    切片 2：`presign_put` / `presign_get` / `capabilities` / `BuildUrl`（原生预签名）。
//    切片 3（当前）：数据面 `ensure_container`/`put`/`get`/`stat`/`remove`/`copy`/`list`
//            （libcurl 流式 + S3 错误码映射 + ListObjectsV2 分页）。
//    切片 4：S3 模式端到端（C5.8）与按配置切驱动（C5.9）；切片 5：ADR-005 与证据收口。
#pragma once

#include "common/result/result.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/blob/s3/sigv4_signer.h"

#include <cstdint>
#include <string>

namespace fss::infra {

struct S3Options {
  //  端点：`127.0.0.1:9000` / `s3.amazonaws.com`（**不含** scheme）
  std::string endpoint;
  std::string region = "us-east-1";
  //  MinIO / Ceph RGW / SeaweedFS 需要 true；AWS 两种都支持
  bool force_path_style = true;
  bool verify_tls = true;
  std::string scheme;  // 空 = 由 verify_tls 推导（https / http）
  AwsCredentials credentials;

  std::int64_t connect_timeout_ms = 3000;
  std::int64_t total_timeout_ms = 30000;
  std::int64_t presign_default_seconds = 3600;
  std::int64_t presign_max_seconds = 604800;  // 7 天（S3 硬上限）
  //  数据面的"无进展"空闲超时（**没有整体超时**；与 C4.11 的同一原则）
  std::int64_t transfer_idle_timeout_seconds = 120;
  //  上游把各家云的 `Driver` 硬编码成 "GCS"；留空则上报真实驱动
  std::string driver_report_override;
  std::string provider_key_override;

  //  数据面（切片 3 用）
  std::string ca_bundle_path;
  bool use_path_style_addressing() const { return force_path_style; }
  std::string effective_scheme() const {
    if (!scheme.empty()) return scheme;
    return verify_tls ? "https" : "http";
  }
};

class S3BlobStore final : public domain::IBlobStore {
 public:
  S3BlobStore(S3Options options, const fss::IClock& clock);

  domain::BlobCapabilities capabilities() const override;
  fss::Result<void> ensure_container(const std::string& container) override;
  fss::Result<domain::SignedLocation> presign_put(const domain::ObjectRef& ref,
                                                 const domain::PresignOptions& options) override;
  fss::Result<domain::SignedLocation> presign_get(const domain::ObjectRef& ref,
                                                 const domain::PresignOptions& options) override;
  fss::Result<void> put(const domain::ObjectRef& ref, bytes::ByteSource& source,
                        const domain::PutOptions& options) override;
  fss::Result<void> get(const domain::ObjectRef& ref, bytes::ByteSink& sink,
                        const domain::ByteRange& range) override;
  fss::Result<domain::ObjectStat> stat(const domain::ObjectRef& ref) override;
  fss::Result<void> remove(const domain::ObjectRef& ref) override;
  fss::Result<domain::ObjectStat> copy(const domain::ObjectRef& from,
                                      const domain::ObjectRef& to) override;
  fss::Result<domain::ListPage> list(const std::string& container, const std::string& prefix,
                                     const std::string& continuation_token,
                                     int limit) override;

  //  供测试与排障：把 (bucket,key) 拼成 URL（path-style / virtual-host）
  fss::Result<std::string> BuildUrl(const domain::ObjectRef& ref) const;
  const S3Options& options() const { return options_; }
  S3Options EffectiveOptions() const;
  const SigV4Signer& signer() const { return signer_; }

 private:
  //  ★ 寻址形态决定路径里是否含桶名（C5.4）：
  //    path-style   → `/bucket/key`（桶在路径里）
  //    virtual-host → `/key`（桶在 Host 的子域里）
  //  漏掉这一步的后果：virtual-host 模式下会去请求 `/bucket/bucket/key` 这类不存在的对象。
  std::string ObjectPath(const domain::ObjectRef& ref) const;
  std::string BucketPath(const std::string& container) const;

  fss::Result<domain::SignedLocation> Presign(const domain::ObjectRef& ref,
                                              const domain::PresignOptions& options,
                                              std::string_view method);

  //  ⚠️ 声明顺序 = 初始化顺序：`signer_` 依赖 `sign_options_`，因此后者必须在前
  S3Options options_;
  SigV4Options sign_options_;
  SigV4Signer signer_;
  const fss::IClock& clock_;
};

}  // namespace fss::infra
