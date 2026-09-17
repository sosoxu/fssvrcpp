// grpc_dto 实现。字段映射与 proto 的 `json_name` 一一对应（契约 §4.2）。
#include "adapters/grpc/dto/grpc_dto.h"

#include "app/usecases/wire_shapes.h"
#include "common/json/json.h"
#include "common/time/time_format.h"

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

// =============================================================================
//  位置 / 列表 / DMS / 交付 的转换（与 REST DTO 的输出逐字段对齐）
// =============================================================================
namespace fss::adapters::grpc {

namespace {

osdu::file::v1::StorageDriver DriverEnumFromName(std::string_view name) {
  //  领域里 driver 是**小写字符串**（真实驱动名）；proto 是枚举（扩展字段）
  if (name == "posix") return osdu::file::v1::STORAGE_DRIVER_POSIX;
  if (name == "s3") return osdu::file::v1::STORAGE_DRIVER_S3;
  if (name == "gcs") return osdu::file::v1::STORAGE_DRIVER_GCS_COMPAT;
  return osdu::file::v1::STORAGE_DRIVER_UNSPECIFIED;
}
osdu::file::v1::StorageZone ZoneEnumFromDomain(fss::domain::StorageZone zone) {
  return zone == fss::domain::StorageZone::kPersistent
             ? osdu::file::v1::STORAGE_ZONE_PERSISTENT
             : osdu::file::v1::STORAGE_ZONE_STAGING;
}
void SetExpiryTimestamp(std::int64_t epoch_seconds, google::protobuf::Timestamp* out) {
  out->set_seconds(epoch_seconds);
  out->set_nanos(0);
}

}  // namespace

void FillLocationProto(const fss::app::LocationResult& result,
                       osdu::file::v1::LocationResponse* out) {
  out->set_file_id(result.file_id);
  out->mutable_location()->set_signed_url(result.signed_url);
  out->mutable_location()->set_file_source(result.file_source);
  out->set_driver(DriverEnumFromName(result.driver));
  out->set_zone(ZoneEnumFromDomain(result.zone));
  SetExpiryTimestamp(result.expires_at_epoch_seconds, out->mutable_expires_at());
}

void FillFileLocationProto(const fss::app::FileLocationView& view,
                           osdu::file::v1::GetFileLocationResponse* out) {
  out->set_driver(view.driver);
  out->set_location(view.location);
  out->set_file_source(view.file_source);
}

void FillDownloadUrlProto(const fss::app::DownloadLocationResult& result,
                          osdu::file::v1::DownloadUrlResponse* out) {
  out->set_signed_url(result.signed_url);
}

void FillFileListProto(const fss::app::FileListResult& result,
                       osdu::file::v1::FileListResponse* out) {
  out->set_number(result.number);
  out->set_size(result.size);
  out->set_number_of_elements(result.number_of_elements);
  for (const auto& entry : result.content) {
    auto* item = out->add_content();
    item->set_file_id(entry.file_id);
    item->set_driver(entry.driver);
    item->set_location(entry.location);
    item->set_created_at(fss::time::ToOsduTimestamp(entry.created_at_epoch_seconds, 0));
    item->set_created_by(entry.created_by);
  }
}

void FillStorageInstructionsProto(const fss::app::StorageInstructions& instructions,
                                  bool collection,
                                  osdu::file::v1::StorageInstructionsResponse* out) {
  out->set_provider_key(instructions.provider_key);
  //  ★ 位置对象是两条协议**共用**的 JSON 形状（`app::DmsLocationJson`）：
  //    REST 直接写进响应体，gRPC 装进 `Struct` —— 键集合不可能漂移。
  JsonToStruct(fss::app::DmsLocationJson(instructions.signed_url, instructions.file_source,
                                         instructions.created_by,
                                         instructions.expires_at_epoch_seconds, collection, 0, {}),
               out->mutable_storage_location());
}

void FillRetrievalInstructionsProto(
    const std::vector<fss::app::RetrievalInstruction>& instructions, bool collection,
    osdu::file::v1::RetrievalInstructionsResponse* out) {
  for (const auto& instruction : instructions) {
    auto* item = out->add_datasets();
    item->set_dataset_registry_id(instruction.dataset_registry_id);
    item->set_provider_key(instruction.provider_key);
    JsonToStruct(fss::app::DmsLocationJson(instruction.signed_url, instruction.file_source,
                                           instruction.created_by,
                                           instruction.expires_at_epoch_seconds, collection, 0, {}),
                 item->mutable_retrieval_properties());
  }
}

void FillCopyDmsProto(const std::vector<fss::app::CopyFileOutcome>& outcomes,
                      osdu::file::v1::CopyDmsResponseList* out) {
  for (const auto& outcome : outcomes) {
    auto* item = out->add_results();
    item->set_success(outcome.success);
    item->set_dataset_blob_storage_path(outcome.dataset_blob_storage_path);
  }
}

void FillUrlSigningProto(const fss::app::SignedUrlResult& result,
                         osdu::file::v1::UrlSigningResponse* out) {
  for (const auto& srn : result.unprocessed) out->add_unprocessed(srn);
  for (const auto& [srn, entry] : result.processed) {
    auto& slot = (*out->mutable_processed())[srn];
    slot.set_signed_url(entry.signed_url);
    slot.set_unsigned_url(entry.unsigned_url);
    slot.set_kind(entry.kind);
    //  ★ `connectionString` 必须**存在**（proto3 里 string 字段的空值不会出现在 JSON 里，
    //    因此这里留空；REST 侧显式输出 `null`。两边的"字段存在性"语义见契约 §2.10）
    slot.set_connection_string("");
  }
}

std::optional<std::string> ExpiryFromProto(const osdu::file::v1::ExpirySpec& spec) {
  if (spec.raw().empty()) return std::nullopt;
  return spec.raw();
}

fss::Result<fss::app::FileListRequest> FileListRequestFromProto(
    const osdu::file::v1::FileListRequest& proto) {
  fss::app::FileListRequest request;
  request.page_num = proto.page_num();
  request.items = proto.items();
  request.user_id = proto.user_id();
  //  ★ 时间解析与 REST 用**同一个** `ParseIso8601`（错误分类因此天然一致）
  if (!proto.time_from().empty()) {
    FSS_TRY(from, fss::time::ParseIso8601(proto.time_from()));
    request.time_from_epoch_seconds = from;
  }
  if (!proto.time_to().empty()) {
    FSS_TRY(to, fss::time::ParseIso8601(proto.time_to()));
    request.time_to_epoch_seconds = to;
  }
  return request;
}

std::vector<fss::app::CopyFileSource> CopySourcesFromProto(
    const osdu::file::v1::CopyDmsRequest& proto) {
  std::vector<fss::app::CopyFileSource> sources;
  for (const auto& node : proto.dataset_sources()) {
    //  ★ 与 REST 共用同一套"取值规则"（`app::FileSourceFromRecordNode`）
    sources.push_back(fss::app::CopyFileSource{
        fss::app::FileSourceFromRecordNode(StructToJson(node))});
  }
  return sources;
}

// -----------------------------------------------------------------------------
//  扩展 RPC（P7 切片 3）
// -----------------------------------------------------------------------------
fss::Result<fss::app::UploadStreamRequest> UploadStreamRequestFromProto(
    const osdu::file::v1::UploadFileInfo& proto) {
  fss::app::UploadStreamRequest request;
  request.file_source = proto.file_source();
  if (!proto.container().empty()) request.container = proto.container();
  if (!proto.key().empty()) request.key = proto.key();
  request.content_type = proto.content_type();
  request.expected_checksum = proto.expected_checksum();
  request.checksum_algorithm = proto.checksum_algorithm();
  request.register_metadata = proto.register_metadata();
  if (proto.has_metadata()) {
    //  ★ 与 `CreateFileMetadata` 共用同一个 proto → 领域转换（同一套结构校验）；
    //    错误**原样返回**（不静默丢弃，否则 registerMetadata=true 会退化成"没登记"）
    FSS_TRY(record, MetadataFromProto(proto.metadata()));
    request.metadata = std::move(record);
  }
  return request;
}

void FillUploadFileResponse(const fss::app::UploadStreamResult& result,
                            osdu::file::v1::UploadFileResponse* out) {
  out->set_file_id(result.file_id);
  out->set_file_source(result.file_source);
  out->set_checksum(result.checksum);
  out->set_bytes_written(result.bytes_written);
  out->set_metadata_record_id(result.metadata_record_id);
}

void FillDownloadTrailer(const fss::app::DownloadStreamResult& result,
                         osdu::file::v1::DownloadFileResponse* out) {
  out->clear_chunk();
  out->set_total_size(result.total_size > 0 ? static_cast<std::uint64_t>(result.total_size) : 0);
  out->set_checksum(result.checksum);
}

void FillServerSideCopyProto(const fss::app::ServerSideCopyResult& result,
                             osdu::file::v1::ServerSideCopyResponse* out) {
  out->set_file_source(result.file_source);
  out->set_bytes_copied(result.bytes_copied);
}

fss::domain::StorageZone StorageZoneFromProto(osdu::file::v1::StorageZone zone) {
  return zone == osdu::file::v1::STORAGE_ZONE_STAGING ? fss::domain::StorageZone::kStaging
                                                     : fss::domain::StorageZone::kPersistent;
}

}  // namespace fss::adapters::grpc
