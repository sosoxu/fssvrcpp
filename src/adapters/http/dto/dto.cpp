// REST DTO 编解码实现。字段名与大小写的依据见头文件。
#include "adapters/http/dto/dto.h"

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
  return body;
}

namespace {

//  `fileNames` 里的"文件名"= 路径最后一段（上游从目录列表里取文件名，本项目一个指令一个对象）
std::string FileNameOf(const std::string& path) {
  const auto slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

//  上传/下载位置的两套键集合：files 版 `fileSource`；集合版 `fileCollectionSource`
//  + `fileCount` + `fileNames`（上游 `AzureFileDmsUploadLocation` /
//  `AzureFileCollectionDmsUploadLocation`）
json::Value LocationToJson(const std::string& signed_url, const std::string& source,
                           const std::string& created_by, std::int64_t expires_at_epoch_seconds,
                           bool collection, int file_count,
                           const std::vector<std::string>& file_names) {
  json::Value location = json::Value::object();
  location["signedUrl"] = signed_url;
  location[collection ? "fileCollectionSource" : "fileSource"] = source;
  if (collection) {
    //  本项目一个指令 = 一个对象：调用方没给名字时按 `fileCollectionSource` 的最后一段推导
    std::vector<std::string> names = file_names;
    if (names.empty() && !source.empty()) names.push_back(FileNameOf(source));
    json::Value array = json::Value::array();
    for (const auto& name : names) array.push_back(name);
    location["fileCount"] = names.empty() ? file_count : static_cast<int>(names.size());
    location["fileNames"] = std::move(array);
  }
  location["createdBy"] = created_by;
  location["expiryTime"] = time::ToOsduTimestamp(expires_at_epoch_seconds, 0);
  return location;
}

}  // namespace

json::Value ToJson(const StorageInstructionsResponse& response) {
  const auto& dto = response.storage_location;
  json::Value body = json::Value::object();
  body["providerKey"] = response.provider_key;
  body["storageLocation"] =
      LocationToJson(dto.signed_url, dto.file_source, dto.created_by,
                     dto.expires_at_epoch_seconds, dto.collection, dto.file_count, dto.file_names);
  return body;
}

json::Value ToJson(const RetrievalInstructionsResponse& response) {
  json::Value datasets = json::Value::array();
  for (const auto& entry : response.datasets) {
    const auto& dto = entry.retrieval_properties;
    json::Value properties =
        LocationToJson(dto.signed_url, dto.file_source, dto.created_by,
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

}  // namespace fss::adapters::http
