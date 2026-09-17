// =============================================================================
//  `dataset--File.Generic` 元数据记录（L3）
// =============================================================================
//  契约依据：docs/03-api-contract.md §3（字段、大小写、黄金样例、负向矩阵）。
//
//  设计取舍
//    ① **开放字段用 `json::Value` 原样承载**（`ExtensionProperties`、`meta`、`tags`
//       以及各层的 `extra`）。理由：OSDU 的 schema 是可扩展的，服务端不应把不认识的
//       字段丢掉 —— 上游的验收样例里就有 `ResourceHomeRegionID` 这类不在我们模型里的字段。
//       测试断言"黄金样例解析 → 序列化 → 语义等价（含未知字段）"。
//    ② **C++ 成员用 snake_case**，PascalCase 只出现在 ToJson/FromJson 的映射里
//       （契约 §3.2 的大小写规则是**线上契约**，不该渗进代码风格）。
//    ③ 校验与序列化分开：`Validate()` 返回结构化问题列表，由应用层决定映射成哪个
//       `ErrorKind` 与固定消息（契约 §5 与 §3.4 的负向矩阵）。
#pragma once

#include "common/json/json.h"
#include "common/result/result.h"
#include "domain/model/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fss::domain {

// -----------------------------------------------------------------------------
//  信封内的复合结构（契约 §3.1）
// -----------------------------------------------------------------------------
struct Acl {
  std::vector<std::string> viewers;
  std::vector<std::string> owners;
  json::Value extra = json::Value::object();
};

enum class LegalStatus { kCompliant, kIncompliant };

std::string_view LegalStatusName(LegalStatus status);
std::optional<LegalStatus> ParseLegalStatus(std::string_view text);

struct Legal {
  std::vector<std::string> legaltags;
  std::vector<std::string> other_relevant_data_countries;
  LegalStatus status = LegalStatus::kCompliant;
  json::Value extra = json::Value::object();
};

struct Ancestry {
  std::optional<std::string> parent;
  std::vector<std::string> parents;
  json::Value extra = json::Value::object();
};

// `data.DatasetProperties.FileSourceInfo`（契约 §3.1）
struct FileSourceInfo {
  // ★ 必需（`@NotEmpty`）：空 → 400 `FileSource can not be empty`
  std::string file_source;
  //  ★ "段是否存在"必须单独记：模型无法表达"缺席"（缺席与空对象解析后长得一样），
  //    而上游对两者的消息不同（`DatasetProperties cannot be empty` vs 字段级消息）。
  bool present = false;
  // 以下都可选，且服务端会**覆写** Checksum / ChecksumAlgorithm（契约 §3.4）
  std::optional<std::string> preload_file_path;
  std::optional<std::string> preload_file_create_user;
  std::optional<std::string> preload_file_create_date;
  std::optional<std::string> preload_file_modify_user;
  std::optional<std::string> preload_file_modify_date;
  std::optional<std::string> name;
  std::optional<std::string> file_size;
  std::optional<std::string> encoding_format_type_id;
  std::optional<std::string> checksum;
  std::optional<std::string> checksum_algorithm;
  json::Value extra = json::Value::object();
};

struct DatasetProperties {
  FileSourceInfo file_source_info;
  bool present = false;  // `data.DatasetProperties` 是否出现
  json::Value extra = json::Value::object();
};

// `data`（契约 §3.2：本层字段名是 **PascalCase**）
struct FileData {
  // OSDU 通用字段（模型里显式支持，便于业务读取；未列出的进 `extra`）
  std::optional<std::string> name;
  std::optional<std::string> description;
  std::optional<std::string> total_size;
  std::optional<std::string> encoding_format_type_id;
  std::optional<std::string> schema_format_type_id;
  std::optional<std::string> endian;  // 只允许 BIG / LITTLE
  std::optional<std::string> checksum;
  std::optional<std::string> checksum_algorithm;
  std::optional<std::string> resource_home_region_id;
  std::vector<std::string> resource_host_region_ids;
  std::optional<std::string> resource_curation_status;
  std::optional<std::string> resource_lifecycle_status;
  std::optional<std::string> resource_security_classification;
  std::optional<std::string> source;
  std::optional<std::string> existence_kind;

  DatasetProperties dataset_properties;
  json::Value extension_properties = json::Value::object();
  json::Value extra = json::Value::object();
};

// -----------------------------------------------------------------------------
//  记录
// -----------------------------------------------------------------------------
struct FileMetadataRecord {
  // 服务端生成：`<partition>:dataset--File.Generic:<uuid 去横线>`
  std::string id;
  std::string kind;  // `<partition>:wks:dataset--File.Generic:<x.y.z>`
  std::int64_t version = 0;
  Acl acl;
  Legal legal;
  FileData data;
  std::optional<Ancestry> ancestry;
  json::Value meta = json::Value::array();
  json::Value tags = json::Value::object();
  json::Value extra = json::Value::object();

  // 从 `id` 解析出的 partition（空表示格式不合法）
  std::string Partition() const;
};

// -----------------------------------------------------------------------------
//  序列化 / 反序列化（PascalCase 映射集中在这里）
// -----------------------------------------------------------------------------
json::Value ToJson(const FileMetadataRecord& record);
// 宽容解析：未知字段进 `extra`（原样保留），不因未识别字段失败。
// 结构性错误（顶层不是对象、`data` 不是对象等）返回 kInvalidArgument。
Result<FileMetadataRecord> ParseFileMetadataRecord(const json::Value& value);

// -----------------------------------------------------------------------------
//  校验（契约 §3.1 + §3.4 的负向矩阵）
// -----------------------------------------------------------------------------
//  每条问题的 `path` 用 JSON 路径（如 `data.DatasetProperties.FileSourceInfo.FileSource`），
//  `message` 用**契约里的固定消息**（能对上上游验收测试的文案）。
struct ValidationIssue {
  std::string path;
  std::string message;
};

struct ValidationResult {
  std::vector<ValidationIssue> issues;
  bool ok() const { return issues.empty(); }
};

// 要求：kind 由调用方先经 KindValidator 校验（这里只查必需字段与枚举）
ValidationResult ValidateMetadataRecord(const FileMetadataRecord& record);

// ACL 组名/邮箱是否合法（契约 §1.5 的正则）
bool IsValidAclPrincipal(std::string_view principal);
// legal tag 元素是否合法（非空、无控制字符）
bool IsValidLegalTag(std::string_view tag);

}  // namespace fss::domain
