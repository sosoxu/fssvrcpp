// grpc_dto 实现。字段映射与 proto 的 `json_name` 一一对应（契约 §4.2）。
#include "adapters/grpc/dto/grpc_dto.h"

#include "common/json/json.h"

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include <optional>
#include <string>
#include <utility>

namespace fss::adapters::grpc {

namespace {

//  "没设置" 与 "设置为空串" 在 proto3 里不可区分 → 空串一律视为未设置
//  （OSDU 的这些可选字段没有"空串有语义"的情形；注册在 dto 头部的差异说明里）。
void SetIfNotEmpty(const std::string& value, std::string* out) {
  if (!value.empty()) *out = value;
}
std::optional<std::string> OptFromProto(const std::string& value) {
  if (value.empty()) return std::nullopt;
  return value;
}

//  `json::Value` ⇄ `google::protobuf::Struct`
void JsonToStruct(const fss::json::Value& value, google::protobuf::Struct* out) {
  if (out == nullptr) return;
  const auto parsed = google::protobuf::util::JsonStringToMessage(fss::json::Dump(value), out);
  (void)parsed;  // 非对象/非法 JSON 时保持空 Struct（调用方保证输入是对象）
}
fss::json::Value StructToJson(const google::protobuf::Struct& value) {
  std::string text;
  const auto ok = google::protobuf::util::MessageToJsonString(value, &text);
  if (!ok.ok()) return fss::json::Value::object();
  const auto parsed = fss::json::Parse(text);
  return parsed.ok() ? parsed.value() : fss::json::Value::object();
}

void FillAcl(const fss::domain::Acl& acl, osdu::file::v1::Acl* out) {
  for (const auto& viewer : acl.viewers) out->add_viewers(viewer);
  for (const auto& owner : acl.owners) out->add_owners(owner);
}
fss::domain::Acl AclFromProto(const osdu::file::v1::Acl& proto) {
  fss::domain::Acl acl;
  for (const auto& viewer : proto.viewers()) acl.viewers.push_back(viewer);
  for (const auto& owner : proto.owners()) acl.owners.push_back(owner);
  return acl;
}

void FillLegal(const fss::domain::Legal& legal, osdu::file::v1::Legal* out) {
  for (const auto& tag : legal.legaltags) out->add_legaltags(tag);
  for (const auto& country : legal.other_relevant_data_countries) {
    out->add_other_relevant_data_countries(country);
  }
  out->set_status(std::string(fss::domain::LegalStatusName(legal.status)));
}
fss::domain::Legal LegalFromProto(const osdu::file::v1::Legal& proto) {
  fss::domain::Legal legal;
  for (const auto& tag : proto.legaltags()) legal.legaltags.push_back(tag);
  for (const auto& country : proto.other_relevant_data_countries()) {
    legal.other_relevant_data_countries.push_back(country);
  }
  legal.status = fss::domain::ParseLegalStatus(proto.status()).value_or(
      fss::domain::LegalStatus::kCompliant);
  return legal;
}

void FillFileSourceInfo(const fss::domain::FileSourceInfo& info,
                        osdu::file::v1::FileSourceInfo* out) {
  out->set_file_source(info.file_source);
  SetIfNotEmpty(info.preload_file_path.value_or(""), out->mutable_preload_file_path());
  SetIfNotEmpty(info.preload_file_create_user.value_or(""),
                out->mutable_preload_file_create_user());
  SetIfNotEmpty(info.preload_file_create_date.value_or(""),
                out->mutable_preload_file_create_date());
  SetIfNotEmpty(info.preload_file_modify_user.value_or(""),
                out->mutable_preload_file_modify_user());
  SetIfNotEmpty(info.preload_file_modify_date.value_or(""),
                out->mutable_preload_file_modify_date());
  SetIfNotEmpty(info.name.value_or(""), out->mutable_name());
  SetIfNotEmpty(info.file_size.value_or(""), out->mutable_file_size());
  SetIfNotEmpty(info.encoding_format_type_id.value_or(""),
                out->mutable_encoding_format_type_id());
  SetIfNotEmpty(info.checksum.value_or(""), out->mutable_checksum());
  SetIfNotEmpty(info.checksum_algorithm.value_or(""), out->mutable_checksum_algorithm());
}
fss::domain::FileSourceInfo FileSourceInfoFromProto(const osdu::file::v1::FileSourceInfo& proto) {
  fss::domain::FileSourceInfo info;
  info.present = true;  // 见下：整个 `data.DatasetProperties` 段的"出现"由调用方判断
  info.file_source = proto.file_source();
  info.preload_file_path = OptFromProto(proto.preload_file_path());
  info.preload_file_create_user = OptFromProto(proto.preload_file_create_user());
  info.preload_file_create_date = OptFromProto(proto.preload_file_create_date());
  info.preload_file_modify_user = OptFromProto(proto.preload_file_modify_user());
  info.preload_file_modify_date = OptFromProto(proto.preload_file_modify_date());
  info.name = OptFromProto(proto.name());
  info.file_size = OptFromProto(proto.file_size());
  info.encoding_format_type_id = OptFromProto(proto.encoding_format_type_id());
  info.checksum = OptFromProto(proto.checksum());
  info.checksum_algorithm = OptFromProto(proto.checksum_algorithm());
  return info;
}

void FillData(const fss::domain::FileData& data, osdu::file::v1::FileData* out) {
  SetIfNotEmpty(data.name.value_or(""), out->mutable_name());
  SetIfNotEmpty(data.description.value_or(""), out->mutable_description());
  SetIfNotEmpty(data.total_size.value_or(""), out->mutable_total_size());
  SetIfNotEmpty(data.encoding_format_type_id.value_or(""),
                out->mutable_encoding_format_type_id());
  SetIfNotEmpty(data.schema_format_type_id.value_or(""), out->mutable_schema_format_type_id());
  SetIfNotEmpty(data.resource_home_region_id.value_or(""),
                out->mutable_resource_home_region_id());
  for (const auto& region : data.resource_host_region_ids) {
    out->add_resource_host_region_ids(region);
  }
  SetIfNotEmpty(data.resource_curation_status.value_or(""),
                out->mutable_resource_curation_status());
  SetIfNotEmpty(data.resource_lifecycle_status.value_or(""),
                out->mutable_resource_lifecycle_status());
  SetIfNotEmpty(data.resource_security_classification.value_or(""),
                out->mutable_resource_security_classification());
  SetIfNotEmpty(data.source.value_or(""), out->mutable_source());
  SetIfNotEmpty(data.existence_kind.value_or(""), out->mutable_existence_kind());
  SetIfNotEmpty(data.endian.value_or(""), out->mutable_endian());
  SetIfNotEmpty(data.checksum.value_or(""), out->mutable_checksum());
  //  ★ 已建模的下载标志：`DatasetProperties` 出现与否由 `present` 决定
  if (data.dataset_properties.present) {
    FillFileSourceInfo(data.dataset_properties.file_source_info,
                       out->mutable_dataset_properties()->mutable_file_source_info());
  }
  //  开放字段：只映射 `ExtensionProperties`（其余未建模键在 proto3 里无处安放）
  if (data.extra.is_object()) {
    const auto extension = data.extra.find("ExtensionProperties");
    if (extension != data.extra.end()) {
      JsonToStruct(*extension, out->mutable_extension_properties());
    }
  }
}
fss::domain::FileData DataFromProto(const osdu::file::v1::FileData& proto) {
  fss::domain::FileData data;
  data.name = OptFromProto(proto.name());
  data.description = OptFromProto(proto.description());
  data.total_size = OptFromProto(proto.total_size());
  data.encoding_format_type_id = OptFromProto(proto.encoding_format_type_id());
  data.schema_format_type_id = OptFromProto(proto.schema_format_type_id());
  data.resource_home_region_id = OptFromProto(proto.resource_home_region_id());
  for (const auto& region : proto.resource_host_region_ids()) {
    data.resource_host_region_ids.push_back(region);
  }
  data.resource_curation_status = OptFromProto(proto.resource_curation_status());
  data.resource_lifecycle_status = OptFromProto(proto.resource_lifecycle_status());
  data.resource_security_classification =
      OptFromProto(proto.resource_security_classification());
  data.source = OptFromProto(proto.source());
  data.existence_kind = OptFromProto(proto.existence_kind());
  data.endian = OptFromProto(proto.endian());
  data.checksum = OptFromProto(proto.checksum());
  //  ★ `present` 的语义：proto3 无法区分"字段缺席"与"默认值"，因此用
  //    "`DatasetProperties` 段是否被填过"来判断 —— `has_dataset_properties()` 对
  //    message 字段是可靠的（proto3 的 message 有 presence）。
  if (proto.has_dataset_properties()) {
    data.dataset_properties.present = true;
    data.dataset_properties.file_source_info =
        FileSourceInfoFromProto(proto.dataset_properties().file_source_info());
  }
  if (proto.has_extension_properties()) {
    data.extra["ExtensionProperties"] = StructToJson(proto.extension_properties());
  }
  return data;
}

}  // namespace

void FillInfoProto(const fss::app::VersionInfo& info, osdu::file::v1::InfoResponse* out) {
  out->set_version(info.version);
  //  ★ REST 的 `/v2/info` 用 `buildVersion` + `connectedOuterServices`（字符串数组）；
  //    proto 的 `InfoResponse` 是上游 Java 类的完整形状（groupId/artifactId/...）。
  //    这里只填两边都有的：`version` 与依赖服务列表（`ConnectedService.name`）。
  for (const auto& service : info.connected_outer_services) {
    out->add_connected_outer_services()->set_name(service);
  }
}

void FillMetadataProto(const fss::domain::FileMetadataRecord& record,
                       osdu::file::v1::FileMetadataRecord* out) {
  out->set_id(record.id);
  out->set_kind(record.kind);
  out->set_version(record.version);
  FillAcl(record.acl, out->mutable_acl());
  FillLegal(record.legal, out->mutable_legal());
  FillData(record.data, out->mutable_data());
  if (record.ancestry.has_value()) {
    for (const auto& parent : record.ancestry->parents) {
      out->mutable_ancestry()->add_parents(parent);
    }
  }
  if (record.meta.is_array()) {
    for (const auto& item : record.meta) {
      JsonToStruct(item, out->add_meta());
    }
  }
  if (record.tags.is_object()) {
    for (auto it = record.tags.begin(); it != record.tags.end(); ++it) {
      if (it.value().is_string()) {
        (*out->mutable_tags())[it.key()] = it.value().get<std::string>();
      }
    }
  }
}

fss::Result<fss::domain::FileMetadataRecord> MetadataFromProto(
    const osdu::file::v1::FileMetadataRecord& proto) {
  fss::domain::FileMetadataRecord record;
  record.id = proto.id();
  record.kind = proto.kind();
  record.version = proto.version();
  record.acl = AclFromProto(proto.acl());
  record.legal = LegalFromProto(proto.legal());
  record.data = DataFromProto(proto.data());
  if (proto.has_ancestry()) {
    fss::domain::Ancestry ancestry;
    for (const auto& parent : proto.ancestry().parents()) {
      ancestry.parents.push_back(parent);
      if (!ancestry.parent.has_value()) ancestry.parent = parent;
    }
    record.ancestry = ancestry;
  }
  for (const auto& item : proto.meta()) {
    record.meta.push_back(StructToJson(item));
  }
  for (const auto& [key, value] : proto.tags()) {
    record.tags[key] = value;
  }
  return record;
}

}  // namespace fss::adapters::grpc
