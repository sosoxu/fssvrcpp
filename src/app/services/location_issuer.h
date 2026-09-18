// =============================================================================
//  LocationIssuer（L4）—— 全项目**唯一**的存储能力分支点（ADR-003 §6.3）
// =============================================================================
//  它回答一个问题：给定租户/用户/文件，上传（staging）或下载（persistent）的
//  地址是什么？两种后端的能力差异只在这里被查询一次：
//
//      caps = store->capabilities()
//        ├─ native_presign == true  → store->presign_put/get(...)   （客户端直连存储）
//        └─ native_presign == false → ISelfSignedUrlCodec::Encode   （本服务数据面代理）
//
//  ★ 为什么必须"只有这一处"
//    ADR-003 的核心结论是"按能力编程，不按类型分支编程"。若 `if (driver == kPosix)`
//    扩散到用例/适配层，新增第三种后端就要改业务逻辑。因此：
//      · 本文件是 `capabilities()` 的**白名单调用点**之一（另一个是
//        `storage_instruction_service.cpp`），由 `tests/unit/test_capability_guard.cpp`
//        机械化执行（C2.7）；
//      · 本文件**不得出现** `StorageDriver::kPosix/kS3` 之类的驱动类型枚举分支
//        （同一护栏检查），"分支痕迹"不许进入领域结果 —— 结果里的 `driver` 直接取
//        `capabilities().driver_name`（字符串），而不是由枚举推导。
//
//  ★ 位置记录如何携带物理引用
//    契约要求 `FileLocation` 是"FileSource ↔ 物理位置"的权威映射，但 P2 的领域模型
//    （`domain/model/types.h`）没有 container/object_key 字段。这里把物理引用写进
//    `FileLocation.extra`（开放字段，仓储契约要求原样保留），避免为了取 key 而引入
//    驱动类型分支：
//        extra["container"]  = 桶/目录
//        extra["object_key"] = 相对 key
//        extra["driver_name"] = capabilities().driver_name（"memory" 等无法映射到枚举时也不丢信息）
//    P3 引入 POSIX/S3 驱动时若把这两列提升为正式字段，只需改这里的两处辅助函数。
#pragma once

#include "app/services/expiry_policy.h"
#include "common/ids/id_generator.h"
#include "common/result/result.h"
#include "common/time/clock.h"
#include "domain/model/types.h"
#include "domain/ports/ports.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fss::app {

//  ★ 阶段 10 切片 5：`self_signed.{default_ttl_seconds,max_ttl_seconds}` 的**上界**语义。
//  为什么是"上界合成"而不是"让这两个键覆盖 `expiry.default`"：
//    `expiry.*` 的语义（= `expiryTime` 参数的解析规则与缺省）已由 C10.12 定稿并被
//    `tests/integration/test_config_wiring.cpp` 的 C10.12 用例锁死。若让 `self_signed.*`
//    去改写 `expiry.default`，就必须先推翻那条既定语义并同步契约与测试 —— 那不是本切片
//    的范围。因此这里只做**上界夹紧**（与 `storage.s3.presign_*` 的形态一致）：
//      · `expiry.*`       → `expiryTime` 参数的**解析规则与缺省**（两条分支共用）；
//      · `self_signed.*`  → **仅自签分支**的缺省上界 / 绝对上界。
//  默认值（3600 / 604800）与 `expiry.*` 的 schema 默认相同 ⇒ 默认配置下结果与接线前**逐字一致**。
struct SelfSignedTtlOptions {
  std::int64_t default_seconds = 3600;    // 请求**未提供** `expiryTime` 时的上界
  std::int64_t max_seconds = 604800;      // 自签分支的绝对上界（任何请求都夹紧）
};

//  颁发结果（领域层表达，**不含** HTTP 细节；DTO 在 P4 的适配层组装）
struct LocationResult {
  std::string file_id;
  std::string file_source;   // 带前导 '/'，对客户端可见的权威定位符
  std::string signed_url;
  std::string driver;        // ★ 来自 capabilities().driver_name（不是 StorageDriver 枚举）
  domain::StorageZone zone = domain::StorageZone::kStaging;
  std::int64_t expires_at_epoch_seconds = 0;
  bool native_presign = false;   // true = 存储原生预签名；false = 本服务自签
};

class LocationIssuer {
 public:
  //  `FileLocation.extra` 里的物理引用键（见文件头说明）
  static constexpr std::string_view kExtraContainer = "container";
  static constexpr std::string_view kExtraObjectKey = "object_key";
  static constexpr std::string_view kExtraDriverName = "driver_name";

  LocationIssuer(domain::IBlobStoreFactory& blobs, domain::IFileLocationRepository& locations,
                 domain::ISelfSignedUrlCodec& self_signed, const fss::IClock& clock,
                 const fss::IIdGenerator& ids, std::string self_base_url,
                 ExpiryOptions expiry = {},
                 //  ★ 阶段 10（C10.16 续）：租户注册表（可选）。非空且注册表里登记了该
                 //    partition 时，容器名按 `PartitionConfig` 的
                 //    `staging_container`/`persistent_container` 覆盖解析；否则退回
                 //    `ObjectKeyPolicy` 的默认命名（接线前逐字一致）。
                 domain::IPartitionRegistry* partitions = nullptr,
                 //  ★ 阶段 10 切片 5：自签分支的 TTL 上界（见 `SelfSignedTtlOptions`）。
                 //    默认值 = 接线前行为；**native_presign 分支完全不受影响**。
                 SelfSignedTtlOptions self_signed_ttl = {})
      : blobs_(blobs),
        locations_(locations),
        self_signed_(self_signed),
        clock_(clock),
        ids_(ids),
        self_base_url_(std::move(self_base_url)),
        expiry_(expiry),
        partitions_(partitions),
        self_signed_ttl_(self_signed_ttl) {}

  //  上传地址（staging 区）：
  //    · `requested_file_id` 为空 → 服务端生成（`NewUuidNoDash()`，与契约 §2.1 样例一致）
  //    · 提供了则必须通过 `^[\w,\s-]+(\.\w+)?$`，且**不得已存在**（契约 §2.2 的 400）
  Result<LocationResult> IssueUploadLocation(std::string_view partition, std::string_view user_id,
                                             const std::optional<std::string>& requested_file_id,
                                             const std::optional<std::string>& expiry_time);

  //  下载地址：位置记录必须已存在（否则 `Not found location for fileID : <id>` → 404）
  Result<LocationResult> IssueDownloadLocation(std::string_view partition, std::string_view file_id,
                                               const std::optional<std::string>& expiry_time);

 private:
  Result<LocationResult> SignAndShape(domain::IBlobStore& store, const domain::ObjectRef& ref,
                                      const domain::FileLocation& location,
                                      const domain::BlobCapabilities& caps,
                                      std::string_view partition, std::int64_t ttl_seconds,
                                      bool upload);

  //  ★ 阶段 10 切片 5：把 `expiry.*` 解析出的 TTL 按自签上界夹紧。
  //    `caps.native_presign == true` → **原样返回**（原生预签名分支完全不受这两个键影响）；
  //    `caps.native_presign == false` → `min(ttl, max_seconds)`，且请求**未提供**
  //    `expiryTime` 时再 `min(ttl, default_seconds)`。
  std::int64_t ApplySelfSignedTtl(std::int64_t ttl_seconds, const domain::BlobCapabilities& caps,
                                  bool expiry_time_provided) const;

  domain::IBlobStoreFactory& blobs_;
  domain::IFileLocationRepository& locations_;
  domain::ISelfSignedUrlCodec& self_signed_;
  const fss::IClock& clock_;
  const fss::IIdGenerator& ids_;
  std::string self_base_url_;
  //  ★ C10.12：`expiry.default` / `expiry.max` 解析后的基数（默认 = 契约值）
  ExpiryOptions expiry_;
  //  ★ 阶段 10：可选的租户注册表（见构造函数注释）
  domain::IPartitionRegistry* partitions_ = nullptr;
  //  ★ 阶段 10 切片 5：自签分支的 TTL 上界（见 `SelfSignedTtlOptions`）
  SelfSignedTtlOptions self_signed_ttl_;
};

}  // namespace fss::app
