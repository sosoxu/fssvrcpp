// REST DTO 编解码实现。字段名与大小写的依据见头文件。
#include "adapters/http/dto/dto.h"

#include "app/usecases/wire_shapes.h"
#include "common/time/time_format.h"

namespace fss::adapters::http {

json::Value ToJson(const LocationResponse& response) {
  json::Value location = json::Value::object();
  location["SignedURL"] = response.location.signed_url;
  location["FileSource"] = response.location.file_source;

  json::Value body = json::Value::object();
  body["FileID"] = response.file_id;
  body["Location"] = std::move(location);
  return body;
}

json::Value ToJson(const DownloadUrlResponse& response) {
  json::Value body = json::Value::object();
  body["SignedUrl"] = response.signed_url;  // ★ 小写 url
  return body;
}

json::Value ToJson(const FileLocationResponse& response) {
  json::Value body = json::Value::object();
  body["Driver"] = response.driver;
  body["Location"] = response.location;
  return body;
}

json::Value ToJson(const FileListResponse& response) {
  json::Value content = json::Value::array();
  for (const auto& entry : response.content) {
    json::Value item = json::Value::object();
    item["FileID"] = entry.file_id;
    item["Driver"] = entry.driver;
    item["Location"] = entry.location;
    item["CreatedAt"] = time::ToOsduTimestamp(entry.created_at_epoch_seconds, 0);
    item["CreatedBy"] = entry.created_by;
    content.push_back(std::move(item));
  }

  json::Value body = json::Value::object();
  body["Content"] = std::move(content);
  body["Number"] = response.number;
  body["NumberOfElements"] = response.number_of_elements;
  body["Size"] = response.size;
  return body;
}

json::Value ToJson(const VersionInfoResponse& response) {
  json::Value body = json::Value::object();
  body["version"] = response.version;
  body["buildVersion"] = response.build_version;
  body["connectedOuterServices"] = response.connected_outer_services;
  if (!response.auth_mode.empty()) body["authMode"] = response.auth_mode;
  //  ★ C9.30（ADR-010 的 R11）：引擎与探测结果必须可见（camelCase，与 `authMode` 同风格）。
  //    `ioUringAvailable` **总是**渲染（含 `false`）—— 布尔字段"缺省即 false"会让
  //    运维分不清"探测说不可用"与"这个版本还没这个字段"。
  if (!response.io_engine.empty()) body["ioEngine"] = response.io_engine;
  body["ioUringAvailable"] = response.io_uring_available;
  return body;
}

json::Value ToJson(const StorageInstructionsResponse& response) {
  const auto& dto = response.storage_location;
  json::Value body = json::Value::object();
  body["providerKey"] = response.provider_key;
  body["storageLocation"] =
      app::DmsLocationJson(dto.signed_url, dto.file_source, dto.created_by,
                           dto.expires_at_epoch_seconds, dto.collection, dto.file_count,
                           dto.file_names);
  return body;
}

json::Value ToJson(const RetrievalInstructionsResponse& response) {
  json::Value datasets = json::Value::array();
  for (const auto& entry : response.datasets) {
    const auto& dto = entry.retrieval_properties;
    json::Value properties =
        app::DmsLocationJson(dto.signed_url, dto.file_source, dto.created_by,
                             dto.expires_at_epoch_seconds, dto.collection, dto.file_count,
                             dto.file_names);

    json::Value item = json::Value::object();
    item["datasetRegistryId"] = entry.dataset_registry_id;
    item["retrievalProperties"] = std::move(properties);
    item["providerKey"] = entry.provider_key;
    datasets.push_back(std::move(item));
  }

  json::Value body = json::Value::object();
  body["datasets"] = std::move(datasets);
  return body;
}

json::Value ToJson(const std::vector<CopyDmsResponse>& results) {
  json::Value body = json::Value::array();
  for (const auto& entry : results) {
    json::Value item = json::Value::object();
    item["success"] = entry.success;
    item["datasetBlobStoragePath"] = entry.dataset_blob_storage_path;
    body.push_back(std::move(item));
  }
  return body;
}

json::Value ToJson(const UrlSigningResponse& response) {
  json::Value processed = json::Value::object();
  for (const auto& [srn, entry] : response.processed) {
    json::Value item = json::Value::object();
    item["signedUrl"] = entry.signed_url;
    item["unsignedUrl"] = entry.unsigned_url;
    item["kind"] = entry.kind;
    //  ★ 显式 `null`：字段必须存在（上游客户端按字段存在性判断）
    item["connectionString"] = nullptr;
    processed[srn] = std::move(item);
  }

  json::Value unprocessed = json::Value::array();
  for (const auto& srn : response.unprocessed) unprocessed.push_back(srn);

  json::Value body = json::Value::object();
  body["processed"] = std::move(processed);
  body["unprocessed"] = std::move(unprocessed);
  return body;
}

json::Value RecordToJson(const domain::FileMetadataRecord& record) {
  //  领域编解码已经产出 PascalCase 的线上形态（含 `version`）
  return domain::ToJson(record);
}

json::Value ToJson(const GcRunResponse& response) {
  //  ★ 运维扩展：snake_case（与 `gc.*` 配置键一致），不套 OSDU 的大小写约定
  //    （上游没有 GC 端点，见契约 §7.3 与 ADR-013 §10）。
  json::Value body = json::Value::object();
  body["dry_run"] = response.dry_run;
  body["partition"] = response.partition;
  body["scheduled"] = response.scheduled;
  body["expired_leases_claimed"] = response.expired_leases_claimed;
  body["deleted_objects"] = response.deleted_objects;
  body["deleted_locations"] = response.deleted_locations;
  body["skipped_has_record"] = response.skipped_has_record;
  body["skipped_no_location"] = response.skipped_no_location;
  body["skipped_too_young"] = response.skipped_too_young;
  body["tmp_removed"] = response.tmp_removed;
  body["tmp_skipped_too_young"] = response.tmp_skipped_too_young;
  body["tmp_skipped_unknown_mtime"] = response.tmp_skipped_unknown_mtime;
  body["errors"] = response.errors;
  return body;
}

}  // namespace fss::adapters::http
