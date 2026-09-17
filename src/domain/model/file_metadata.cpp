#include "domain/model/file_metadata.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <regex>
#include <set>

namespace fss::domain {
namespace {

// -----------------------------------------------------------------------------
//  小工具：PascalCase 字段的读写（**唯一**允许出现契约字段名的地方）
// -----------------------------------------------------------------------------
void SetOpt(json::Value& obj, const char* field, const std::optional<std::string>& value) {
  if (value.has_value() && !value->empty()) obj[field] = *value;
}

std::optional<std::string> GetOpt(const json::Value& obj, const char* field) {
  const auto it = obj.find(field);
  if (it == obj.end() || !it->is_string()) return std::nullopt;
  const auto& s = it->get_ref<const std::string&>();
  if (s.empty()) return std::nullopt;
  return s;
}

void GetStrings(const json::Value& obj, const char* field, std::vector<std::string>* out) {
  out->clear();
  const auto it = obj.find(field);
  if (it == obj.end() || !it->is_array()) return;
  for (const auto& item : *it) {
    if (item.is_string()) out->push_back(item.get<std::string>());
  }
}

// 把"已知字段之外"的成员收集到 extra（保证未知字段不丢）
json::Value CollectExtra(const json::Value& obj, const std::vector<const char*>& known) {
  json::Value extra = json::Value::object();
  if (!obj.is_object()) return extra;
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    const bool is_known =
        std::any_of(known.begin(), known.end(), [&](const char* k) { return it.key() == k; });
    if (!is_known) extra[it.key()] = it.value();
  }
  return extra;
}

template <typename Fn>
void ForEachExtra(const json::Value& extra, Fn fn) {
  if (!extra.is_object()) return;
  for (auto it = extra.begin(); it != extra.end(); ++it) fn(it.key(), it.value());
}

// -----------------------------------------------------------------------------
//  校验辅助
// -----------------------------------------------------------------------------
bool HasControlChars(std::string_view s) {
  for (const char c : s) {
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7F) return true;
  }
  return false;
}

bool AllUnique(const std::vector<std::string>& items) {
  std::set<std::string> seen;
  for (const auto& s : items) {
    if (!seen.insert(s).second) return false;
  }
  return true;
}

constexpr std::size_t kMaxKindSegment = 64;

}  // namespace

// -----------------------------------------------------------------------------
//  枚举
// -----------------------------------------------------------------------------
std::string_view StorageZoneName(StorageZone zone) {
  return zone == StorageZone::kStaging ? "STAGING" : "PERSISTENT";
}

std::optional<StorageZone> ParseStorageZone(std::string_view text) {
  if (text == "STAGING") return StorageZone::kStaging;
  if (text == "PERSISTENT") return StorageZone::kPersistent;
  return std::nullopt;
}

std::string_view StorageDriverName(StorageDriver driver) {
  return driver == StorageDriver::kPosix ? "POSIX" : "S3";
}

std::optional<StorageDriver> ParseStorageDriver(std::string_view text) {
  if (text == "POSIX") return StorageDriver::kPosix;
  if (text == "S3") return StorageDriver::kS3;
  return std::nullopt;
}

std::string_view LegalStatusName(LegalStatus status) {
  return status == LegalStatus::kCompliant ? "compliant" : "incompliant";
}

std::optional<LegalStatus> ParseLegalStatus(std::string_view text) {
  if (text == "compliant") return LegalStatus::kCompliant;
  if (text == "incompliant") return LegalStatus::kIncompliant;
  return std::nullopt;
}

std::string ObjectRef::ToString() const {
  std::string out = container + "/" + key;
  if (version_id.has_value()) out += "@" + *version_id;
  return out;
}

std::string FileMetadataRecord::Partition() const {
  const auto colon = id.find(':');
  if (colon == std::string::npos || colon == 0) return {};
  return id.substr(0, colon);
}

// -----------------------------------------------------------------------------
//  序列化
// -----------------------------------------------------------------------------
namespace {
json::Value AclToJson(const Acl& acl) {
  json::Value out = acl.extra.is_object() ? acl.extra : json::Value::object();
  out["viewers"] = acl.viewers;
  out["owners"] = acl.owners;
  return out;
}

Acl AclFromJson(const json::Value& v) {
  Acl acl;
  GetStrings(v, "viewers", &acl.viewers);
  GetStrings(v, "owners", &acl.owners);
  acl.extra = CollectExtra(v, {"viewers", "owners"});
  return acl;
}

json::Value LegalToJson(const Legal& legal) {
  json::Value out = legal.extra.is_object() ? legal.extra : json::Value::object();
  out["legaltags"] = legal.legaltags;
  out["otherRelevantDataCountries"] = legal.other_relevant_data_countries;
  out["status"] = std::string(LegalStatusName(legal.status));
  return out;
}

Legal LegalFromJson(const json::Value& v) {
  Legal legal;
  GetStrings(v, "legaltags", &legal.legaltags);
  GetStrings(v, "otherRelevantDataCountries", &legal.other_relevant_data_countries);
  if (const auto s = GetOpt(v, "status"); s.has_value()) {
    legal.status = ParseLegalStatus(*s).value_or(LegalStatus::kCompliant);
  }
  legal.extra = CollectExtra(v, {"legaltags", "otherRelevantDataCountries", "status"});
  return legal;
}

json::Value AncestryToJson(const Ancestry& a) {
  json::Value out = a.extra.is_object() ? a.extra : json::Value::object();
  if (a.parent.has_value()) out["parent"] = *a.parent;
  if (!a.parents.empty()) out["parents"] = a.parents;
  return out;
}

Ancestry AncestryFromJson(const json::Value& v) {
  Ancestry a;
  a.parent = GetOpt(v, "parent");
  GetStrings(v, "parents", &a.parents);
  a.extra = CollectExtra(v, {"parent", "parents"});
  return a;
}

json::Value FileSourceInfoToJson(const FileSourceInfo& info) {
  json::Value out = info.extra.is_object() ? info.extra : json::Value::object();
  out["FileSource"] = info.file_source;
  SetOpt(out, "PreloadFilePath", info.preload_file_path);
  SetOpt(out, "PreloadFileCreateUser", info.preload_file_create_user);
  SetOpt(out, "PreloadFileCreateDate", info.preload_file_create_date);
  SetOpt(out, "PreloadFileModifyUser", info.preload_file_modify_user);
  SetOpt(out, "PreloadFileModifyDate", info.preload_file_modify_date);
  SetOpt(out, "Name", info.name);
  SetOpt(out, "FileSize", info.file_size);
  SetOpt(out, "EncodingFormatTypeID", info.encoding_format_type_id);
  SetOpt(out, "Checksum", info.checksum);
  SetOpt(out, "ChecksumAlgorithm", info.checksum_algorithm);
  return out;
}

FileSourceInfo FileSourceInfoFromJson(const json::Value& v) {
  FileSourceInfo info;
  if (const auto s = GetOpt(v, "FileSource"); s.has_value()) info.file_source = *s;
  info.preload_file_path = GetOpt(v, "PreloadFilePath");
  info.preload_file_create_user = GetOpt(v, "PreloadFileCreateUser");
  info.preload_file_create_date = GetOpt(v, "PreloadFileCreateDate");
  info.preload_file_modify_user = GetOpt(v, "PreloadFileModifyUser");
  info.preload_file_modify_date = GetOpt(v, "PreloadFileModifyDate");
  info.name = GetOpt(v, "Name");
  info.file_size = GetOpt(v, "FileSize");
  info.encoding_format_type_id = GetOpt(v, "EncodingFormatTypeID");
  info.checksum = GetOpt(v, "Checksum");
  info.checksum_algorithm = GetOpt(v, "ChecksumAlgorithm");
  info.extra = CollectExtra(v, {"FileSource", "PreloadFilePath", "PreloadFileCreateUser",
                                "PreloadFileCreateDate", "PreloadFileModifyUser",
                                "PreloadFileModifyDate", "Name", "FileSize",
                                "EncodingFormatTypeID", "Checksum", "ChecksumAlgorithm"});
  return info;
}

json::Value FileDataToJson(const FileData& d) {
  json::Value out = d.extra.is_object() ? d.extra : json::Value::object();
  SetOpt(out, "Name", d.name);
  SetOpt(out, "Description", d.description);
  SetOpt(out, "TotalSize", d.total_size);
  SetOpt(out, "EncodingFormatTypeID", d.encoding_format_type_id);
  SetOpt(out, "SchemaFormatTypeID", d.schema_format_type_id);
  SetOpt(out, "Endian", d.endian);
  SetOpt(out, "Checksum", d.checksum);
  SetOpt(out, "ChecksumAlgorithm", d.checksum_algorithm);
  SetOpt(out, "ResourceHomeRegionID", d.resource_home_region_id);
  if (!d.resource_host_region_ids.empty()) out["ResourceHostRegionIDs"] = d.resource_host_region_ids;
  SetOpt(out, "ResourceCurationStatus", d.resource_curation_status);
  SetOpt(out, "ResourceLifecycleStatus", d.resource_lifecycle_status);
  SetOpt(out, "ResourceSecurityClassification", d.resource_security_classification);
  SetOpt(out, "Source", d.source);
  SetOpt(out, "ExistenceKind", d.existence_kind);

  json::Value ds = d.dataset_properties.extra.is_object() ? d.dataset_properties.extra
                                                         : json::Value::object();
  ds["FileSourceInfo"] = FileSourceInfoToJson(d.dataset_properties.file_source_info);
  out["DatasetProperties"] = std::move(ds);
  out["ExtensionProperties"] =
      d.extension_properties.is_object() ? d.extension_properties : json::Value::object();
  return out;
}

FileData FileDataFromJson(const json::Value& v) {
  FileData d;
  d.name = GetOpt(v, "Name");
  d.description = GetOpt(v, "Description");
  d.total_size = GetOpt(v, "TotalSize");
  d.encoding_format_type_id = GetOpt(v, "EncodingFormatTypeID");
  d.schema_format_type_id = GetOpt(v, "SchemaFormatTypeID");
  d.endian = GetOpt(v, "Endian");
  d.checksum = GetOpt(v, "Checksum");
  d.checksum_algorithm = GetOpt(v, "ChecksumAlgorithm");
  d.resource_home_region_id = GetOpt(v, "ResourceHomeRegionID");
  GetStrings(v, "ResourceHostRegionIDs", &d.resource_host_region_ids);
  d.resource_curation_status = GetOpt(v, "ResourceCurationStatus");
  d.resource_lifecycle_status = GetOpt(v, "ResourceLifecycleStatus");
  d.resource_security_classification = GetOpt(v, "ResourceSecurityClassification");
  d.source = GetOpt(v, "Source");
  d.existence_kind = GetOpt(v, "ExistenceKind");

  if (const auto it = v.find("DatasetProperties"); it != v.end() && it->is_object()) {
    d.dataset_properties.present = true;
    d.dataset_properties.extra = CollectExtra(*it, {"FileSourceInfo"});
    if (const auto fs = it->find("FileSourceInfo"); fs != it->end() && fs->is_object()) {
      d.dataset_properties.file_source_info = FileSourceInfoFromJson(*fs);
      d.dataset_properties.file_source_info.present = true;
    }
  }
  if (const auto it = v.find("ExtensionProperties"); it != v.end()) {
    d.extension_properties = *it;
  }
  d.extra = CollectExtra(v, {"Name", "Description", "TotalSize", "EncodingFormatTypeID",
                             "SchemaFormatTypeID", "Endian", "Checksum", "ChecksumAlgorithm",
                             "ResourceHomeRegionID", "ResourceHostRegionIDs",
                             "ResourceCurationStatus", "ResourceLifecycleStatus",
                             "ResourceSecurityClassification", "Source", "ExistenceKind",
                             "DatasetProperties", "ExtensionProperties"});
  return d;
}
}  // namespace

json::Value ToJson(const FileMetadataRecord& r) {
  json::Value out = r.extra.is_object() ? r.extra : json::Value::object();
  if (!r.id.empty()) out["id"] = r.id;
  if (!r.kind.empty()) out["kind"] = r.kind;
  if (r.version > 0) out["version"] = r.version;
  out["acl"] = AclToJson(r.acl);
  out["legal"] = LegalToJson(r.legal);
  out["data"] = FileDataToJson(r.data);
  if (r.ancestry.has_value()) out["ancestry"] = AncestryToJson(*r.ancestry);
  if (!r.meta.is_null()) out["meta"] = r.meta;
  if (r.tags.is_object() && !r.tags.empty()) out["tags"] = r.tags;
  return out;
}

//  枚举校验（上游 `EnumValidationException` 的消息格式：`Invalid value of <值> for <枚举名>`）
//  ★ 时机：**解析阶段**。见 `ParseFileMetadataRecord` 里的说明。
Result<void> CheckEnums(const json::Value& data) {
  if (const auto it = data.find("Endian"); it != data.end() && it->is_string()) {
    const std::string value = it->get<std::string>();
    if (value != "BIG" && value != "LITTLE") {
      return Err(ErrorKind::kInvalidArgument, "Invalid value of " + value + " for Endian");
    }
  }
  //  `data.VectorHeaderMapping[].ScalarIndicator`：STANDARD / NOSCALE / OVERRIDE
  //  （上游 `filedetails/ScalarIndicator.java`）
  if (const auto it = data.find("VectorHeaderMapping");
      it != data.end() && it->is_array()) {
    for (const auto& item : *it) {
      if (!item.is_object()) continue;
      const auto scalar = item.find("ScalarIndicator");
      if (scalar == item.end()) continue;
      if (!scalar->is_string()) {
        return Err(ErrorKind::kInvalidArgument, "Invalid value of null for ScalarIndicator");
      }
      const std::string value = scalar->get<std::string>();
      if (value != "STANDARD" && value != "NOSCALE" && value != "OVERRIDE") {
        return Err(ErrorKind::kInvalidArgument,
                   "Invalid value of " + value + " for ScalarIndicator");
      }
    }
  }
  return Ok();
}

Result<FileMetadataRecord> ParseFileMetadataRecord(const json::Value& value) {
  if (!value.is_object()) {
    return Err(ErrorKind::kInvalidArgument, "元数据记录必须是 JSON 对象");
  }
  //  ★ 四个必需**段**缺失时在这里就报错：模型无法表达"段不存在"（缺失与为空在解析后
  //    长得一样），若不在这里拦，`File_missing_data.json` 会退化成"FileSource 为空"，
  //    客户端拿到的消息就不是契约 §3.4 要求的 `data cannot be empty`（P4-D07）。
  //
  //  ★ 消息**逐字**对齐上游（一手证据）：
  //    `file-core/.../model/filemetadata/FileMetadata.java` 第 36/40/46/50 行的
  //    `@NotNull(message = ...)`；期望报文见
  //    `testing/file-test-{baremetal,gc}/.../output_payloads/File_missing_*_msg.json`。
  //    `data` 为**空对象**（`{}`）与缺失同义 —— 上游 `File_missing_data.json` 里就是 `"data": {}`，
  //    期望消息同样是 `data cannot be empty`。
  struct RequiredSection {
    const char* key;
    const char* message;
    bool empty_object_is_empty;
  };
  for (const RequiredSection s : {RequiredSection{"kind", "kind must not be null", false},
                                  RequiredSection{"acl", "acl must not be null", false},
                                  RequiredSection{"legal", "legal tag cannot be empty", false},
                                  RequiredSection{"data", "data cannot be empty", true}}) {
    const auto it = value.find(s.key);
    if (it == value.end()) return Err(ErrorKind::kInvalidArgument, s.message);
    if (s.empty_object_is_empty && it->is_object() && it->empty()) {
      return Err(ErrorKind::kInvalidArgument, s.message);
    }
  }

  FileMetadataRecord r;
  if (const auto s = GetOpt(value, "id"); s.has_value()) r.id = *s;
  if (const auto s = GetOpt(value, "kind"); s.has_value()) r.kind = *s;
  if (const auto it = value.find("version"); it != value.end() && it->is_number_integer()) {
    r.version = it->get<std::int64_t>();
  }
  if (const auto it = value.find("acl"); it != value.end()) {
    if (!it->is_object()) return Err(ErrorKind::kInvalidArgument, "acl 必须是对象");
    r.acl = AclFromJson(*it);
  }
  if (const auto it = value.find("legal"); it != value.end()) {
    if (!it->is_object()) return Err(ErrorKind::kInvalidArgument, "legal 必须是对象");
    r.legal = LegalFromJson(*it);
  }
  if (const auto it = value.find("data"); it != value.end()) {
    if (!it->is_object()) return Err(ErrorKind::kInvalidArgument, "data 必须是对象");
    r.data = FileDataFromJson(*it);

    //  ★ 枚举校验放在**解析阶段**，与上游一致：上游把 `Endian` / `ScalarIndicator` 建模为
    //    带 `@JsonCreator` 的枚举，非法值在 **Jackson 反序列化**时就抛错，
    //    早于 Bean Validation（`@NotNull`/`@ValidAcl`）。顺序差异是**可观测**的：
    //    `File_invalid_Endian.json` 的 `FileSource` 同时也是非法形状（`"string"`），
    //    若枚举校验留到校验阶段（排在 FileSource 之后），我们就会报
    //    `Invalid source file path...` 而不是上游期望的 `Invalid value of Small for Endian`。
    FSS_TRY(CheckEnums(*it));
  }
  if (const auto it = value.find("ancestry"); it != value.end() && it->is_object()) {
    r.ancestry = AncestryFromJson(*it);
  }
  if (const auto it = value.find("meta"); it != value.end()) r.meta = *it;
  if (const auto it = value.find("tags"); it != value.end() && it->is_object()) r.tags = *it;
  r.extra = CollectExtra(value, {"id", "kind", "version", "acl", "legal", "data", "ancestry",
                                 "meta", "tags"});
  return r;
}

// -----------------------------------------------------------------------------
//  校验（契约 §3.1 / §3.4）
// -----------------------------------------------------------------------------
bool IsValidAclPrincipal(std::string_view principal) {
  // 契约 §1.5 的正则（组名/邮箱）。用静态 regex：编译一次。
  static const std::regex re(
      R"(^data\.[a-zA-Z0-9_+&*-]+(?:\.[a-zA-Z0-9_+&*-]+)*@(?:[a-zA-Z](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$)");
  return std::regex_match(std::string(principal), re);
}

bool IsValidLegalTag(std::string_view tag) {
  return !tag.empty() && !HasControlChars(tag) && tag.size() <= 256;
}

ValidationResult ValidateMetadataRecord(const FileMetadataRecord& r) {
  ValidationResult out;
  const auto add = [&](std::string path, std::string message) {
    out.issues.push_back({std::move(path), std::move(message)});
  };

  // kind：只做"非空 + 段数 + 段长上限"这类结构性检查；
  // 语义校验（wks / dataset--File.Generic）由 app 层的 KindValidator 负责（契约 §1.5）
  if (r.kind.empty()) {
    add("kind", "kind must not be null");  // 上游 FileMetadata.java:36
  } else {
    std::size_t segments = 1;
    for (const char c : r.kind) {
      if (c == ':') ++segments;
    }
    if (segments != 4) add("kind", "Invalid kind");
    if (r.kind.size() > 512) add("kind", "kind 过长");
  }

  // acl（契约 §3.1：viewers/owners 必需且非空）
  //  消息逐字对齐上游验收期望报文（`File_missing_viewers_msg.json` / `..._owners_msg.json`）
  if (r.acl.viewers.empty()) add("acl.viewers", "Record acl.viewers cannot be empty");
  if (r.acl.owners.empty()) add("acl.owners", "Record acl.owners cannot be empty");
  for (const auto& v : r.acl.viewers) {
    if (!IsValidAclPrincipal(v)) add("acl.viewers", "ACL 主体格式非法：" + v);
  }
  for (const auto& o : r.acl.owners) {
    if (!IsValidAclPrincipal(o)) add("acl.owners", "ACL 主体格式非法：" + o);
  }

  // legal（契约 §3.1：legaltags / otherRelevantDataCountries 各 ≥ 1 且元素唯一）
  if (r.legal.legaltags.empty()) add("legal.legaltags", "legal tag cannot be empty");
  if (r.legal.other_relevant_data_countries.empty()) {
    add("legal.otherRelevantDataCountries", "legal.otherRelevantDataCountries 不能为空");
  }
  if (!AllUnique(r.legal.legaltags)) add("legal.legaltags", "legal.legaltags 元素必须唯一");
  if (!AllUnique(r.legal.other_relevant_data_countries)) {
    add("legal.otherRelevantDataCountries", "legal.otherRelevantDataCountries 元素必须唯一");
  }
  for (const auto& t : r.legal.legaltags) {
    if (!IsValidLegalTag(t)) add("legal.legaltags", "legal tag 非法：" + t);
  }
  for (const auto& c : r.legal.other_relevant_data_countries) {
    // ISO 3166-1 alpha-2 的宽松校验：2 个字母
    if (c.size() != 2 || !std::isalpha(static_cast<unsigned char>(c[0])) ||
        !std::isalpha(static_cast<unsigned char>(c[1]))) {
      add("legal.otherRelevantDataCountries", "国家代码必须是 2 个字母：" + c);
    }
  }

  // data.DatasetProperties.FileSourceInfo.FileSource（★ 固定消息，契约 §3.4）
  const auto& info = r.data.dataset_properties.file_source_info;
  if (info.file_source.empty()) {
    add("data.DatasetProperties.FileSourceInfo.FileSource", "FileSource can not be empty");
  } else {
    if (info.file_source.front() != '/') {
      add("data.DatasetProperties.FileSourceInfo.FileSource",
          "Invalid source file path to copy from " + info.file_source);
    }
    if (info.file_source.find("..") != std::string::npos) {
      add("data.DatasetProperties.FileSourceInfo.FileSource",
          "Invalid source file path to copy from " + info.file_source);
    }
    if (HasControlChars(info.file_source)) {
      add("data.DatasetProperties.FileSourceInfo.FileSource",
          "Invalid source file path to copy from " + info.file_source);
    }
  }

  // Endian 只允许 BIG / LITTLE（契约 §3.1）
  //  Endian / ScalarIndicator 的消息格式 = `Invalid value of <值> for <枚举名>`
  //  （上游 `EnumValidationException`；期望报文 `File_invalid_Endian_msg.json` 等）。
  //  ⚠️ JSON 入口的枚举校验在**解析阶段**（`CheckEnums`）就返回了，这里的检查是给
  //     **非 JSON 入口**（P7 的 gRPC 由 protobuf 构造记录、测试手写记录）用的安全网。
  if (r.data.endian.has_value() && *r.data.endian != "BIG" && *r.data.endian != "LITTLE") {
    add("data.Endian", "Invalid value of " + *r.data.endian + " for Endian");
  }

  //  `data.DatasetProperties` / `FileSourceInfo` 必需（上游 `@NotNull(message = "DatasetProperties cannot be empty")`）
  //  ★ 存在性判定要**宽容**：`present` 标志只由解析器设置，而记录也可能由代码构造
  //    （P7 的 gRPC 入口、测试 fixture）。因此"段存在"= 标志为真 **或** 内容非空，
  //    否则手写的记录会被误判成"段缺失"。
  const bool dataset_properties_present =
      r.data.dataset_properties.present || !r.data.dataset_properties.extra.empty() ||
      !info.file_source.empty();
  if (!dataset_properties_present) {
    add("data.DatasetProperties", "DatasetProperties cannot be empty");
  } else if (!r.data.dataset_properties.file_source_info.present && info.file_source.empty()) {
    add("data.DatasetProperties.FileSourceInfo", "FileSourceInfo cannot be empty");
  }

  //  `data.VectorHeaderMapping[].ScalarIndicator`：枚举 STANDARD / NOSCALE / OVERRIDE
  //  （上游 `filedetails/ScalarIndicator.java`；期望报文 `File_invalid_ScalarIndicator_msg.json`）
  //  ⚠️ 该字段在领域模型里属于**开放内容**（`data.extra`），这里对它做定点校验：
  //     模型不展开 `VectorHeaderMapping` 的全部子字段（KeyName/WordFormat/... 与业务无关），
  //     展开反而会带来"往返丢字段"的风险。
  if (const auto it = r.data.extra.find("VectorHeaderMapping");
      it != r.data.extra.end() && it->is_array()) {
    for (const auto& item : *it) {
      const auto scalar = item.find("ScalarIndicator");
      if (scalar == item.end()) continue;
      if (!scalar->is_string()) {
        add("data.VectorHeaderMapping.ScalarIndicator",
            "Invalid value of null for ScalarIndicator");
        continue;
      }
      const std::string value = scalar->get<std::string>();
      if (value != "STANDARD" && value != "NOSCALE" && value != "OVERRIDE") {
        add("data.VectorHeaderMapping.ScalarIndicator",
            "Invalid value of " + value + " for ScalarIndicator");
      }
    }
  }
  return out;
}

}  // namespace fss::domain
