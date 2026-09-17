// =============================================================================
//  领域基础类型（L3）
// =============================================================================
//  规则：本层**只允许**依赖 `fss_result` / 通用 L1 技术抽象（json、time 等）。
//        不得出现 grpc / sqlite3 / curl / openssl / httplib（编译期 + 源码护栏双重强制）。
//
//  命名规则来自契约 §3.2：**JSON 字段名不等于 C++ 成员名**。
//    · 信封层（id/kind/acl/legal/data…）是 lowerCamel
//    · `data` 及其子层是 **PascalCase**（`Name`/`TotalSize`/`FileSource`…）
//    · REST 响应是 PascalCase（`FileID`/`Location`/`SignedURL`/`FileSource`）
//  为避免"在 C++ 里也照抄 PascalCase 成员名"（那会让代码风格与契约纠缠），
//  本层成员统一 snake_case，**映射集中在 ToJson/FromJson**（映射表在契约 §4）。
#pragma once

#include "common/json/json.h"
#include "common/result/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fss::domain {

// -----------------------------------------------------------------------------
//  存储区 / 驱动
// -----------------------------------------------------------------------------
//  `StorageZone` / `StorageDriver` 的取值必须与 proto 枚举和 OSDU 响应一致（契约 §4.2）。
enum class StorageZone {
  kStaging,  // 上传落地区
  kPersistent,
};

std::string_view StorageZoneName(StorageZone zone);  // "STAGING" / "PERSISTENT"
std::optional<StorageZone> ParseStorageZone(std::string_view text);

enum class StorageDriver {
  kPosix,  // 集中存储（含 NFS）
  kS3,     // S3 兼容对象存储
};

std::string_view StorageDriverName(StorageDriver driver);  // "POSIX" / "S3"
std::optional<StorageDriver> ParseStorageDriver(std::string_view text);

// -----------------------------------------------------------------------------
//  对象引用与状态（存储抽象的最小语言，见 ADR-003 §6.1）
// -----------------------------------------------------------------------------
struct ObjectRef {
  std::string container;  // POSIX: 挂载下的"桶目录"；S3: bucket
  std::string key;        // 对象键（**相对** container，不含前导 '/'）
  std::optional<std::string> version_id;

  bool operator==(const ObjectRef& o) const {
    return container == o.container && key == o.key && version_id == o.version_id;
  }
  std::string ToString() const;  // `container/key[@version]`，仅用于日志与排障
};

struct ObjectStat {
  std::int64_t size = 0;
  std::string content_type;
  std::string checksum;            // 存储侧给出的校验和（如 S3 ETag）
  std::string checksum_algorithm;  // 与 checksum 配套的算法名
  std::int64_t last_modified_epoch_seconds = 0;
  bool exists = false;
};

struct ListEntry {
  std::string key;
  std::int64_t size = 0;
  std::int64_t last_modified_epoch_seconds = 0;
};

struct ListPage {
  std::vector<ListEntry> entries;
  std::string continuation_token;  // 空 = 没有更多
  bool truncated = false;
};

// 字节区间（`length == 0` 表示"到末尾"）。64 位偏移是硬要求：大文件 > 2 GiB。
struct ByteRange {
  std::uint64_t offset = 0;
  std::uint64_t length = 0;

  bool IsWholeObject() const { return offset == 0 && length == 0; }
};

// -----------------------------------------------------------------------------
//  预签名结果
// -----------------------------------------------------------------------------
struct SignedLocation {
  std::string url;                 // 客户端直接使用的 URL
  std::string method;              // "PUT" / "GET"
  std::string file_source;         // 集中存储的 FileSource（对象存储可空）
  std::int64_t expires_at_epoch_seconds = 0;
  // 需要客户端额外携带的头（如 S3 的 Content-Type、自签 URL 的校验头）
  std::vector<std::pair<std::string, std::string>> required_headers;
  bool native = false;             // true = 存储原生预签名；false = 本服务自签

  bool IsExpired(std::int64_t now_epoch_seconds) const {
    return expires_at_epoch_seconds > 0 && now_epoch_seconds >= expires_at_epoch_seconds;
  }
};

// -----------------------------------------------------------------------------
//  位置记录（集中存储的"文件在哪"）
// -----------------------------------------------------------------------------
struct FileLocation {
  std::string file_id;
  StorageDriver driver = StorageDriver::kPosix;
  StorageZone zone = StorageZone::kStaging;
  std::string file_source;  // 带前导 '/'；POSIX 的权威定位符
  //  上传者（OSDU 的 `UserID` 过滤条件 / 响应里的 `CreatedBy`）。
  //  它同时也是 FileSource 的第 1 段，但**显式成列**才能支持"按用户过滤 + 正确分页"
  //  （若靠 FileSource 现解析，仓储就得依赖 L4 的 ObjectKeyPolicy —— 方向违规）。
  std::string user_id;
  std::string signed_url;   // 最近一次签发的 URL（可空；不作为权威）
  std::int64_t created_at_epoch_seconds = 0;
  std::int64_t updated_at_epoch_seconds = 0;
  // 便于扩展而不破坏仓储契约（未知字段原样保留）
  json::Value extra = json::Value::object();
};

}  // namespace fss::domain
