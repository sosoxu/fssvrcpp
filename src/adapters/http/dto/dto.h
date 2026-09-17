// =============================================================================
//  REST DTO（L5 适配层）—— 把领域结果翻译成 OSDU 的**线上 JSON**
// =============================================================================
//  契约依据：`docs/03-api-contract.md` §2（逐端点字段）与 §3.2（大小写规则）。
//
//  ★ 三条纪律（`docs/02-design.md` §7.3）
//    ① 适配层**不含业务判断**（无有效期计算、无路径构造）；
//    ② 适配层**不含直接 IO**；
//    ③ DTO ↔ 领域模型的转换**只在本层**发生，且是纯函数（可单测、无 IO）。
//
//  ★ 大小写是**线上契约**（C4.4）
//    `FileID` / `Location` / `SignedURL` / `FileSource` / `SignedUrl` / `Content` /
//    `NumberOfElements` / `Size` / `CreatedAt` 的每个字母都必须逐字符一致；
//    `fileId` / `fileSource` / `results` 这类"看起来更自然"的名字**不得出现**。
#pragma once

#include "common/json/json.h"
#include "domain/model/file_metadata.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fss::adapters::http {

// -----------------------------------------------------------------------------
//  §2.1 / §2.2 `LocationResponse`
// -----------------------------------------------------------------------------
//  `FileSource` 在 `Location` **内部**（不是顶层）；该响应**不返回 `Driver`**。
struct LocationBody {
  std::string signed_url;   // → `SignedURL`
  std::string file_source;  // → `FileSource`
};

struct LocationResponse {
  std::string file_id;  // → `FileID`
  LocationBody location;
};

json::Value ToJson(const LocationResponse& response);

// -----------------------------------------------------------------------------
//  §2.4 `GET files/{id}/downloadURL` → `{"SignedUrl": "..."}`
// -----------------------------------------------------------------------------
//  ⚠️ 注意这里是 `SignedUrl`（小写 url），与 §2.1 的 `SignedURL` **不同**。
struct DownloadUrlResponse {
  std::string signed_url;
};

json::Value ToJson(const DownloadUrlResponse& response);

// -----------------------------------------------------------------------------
//  §2.3 `FileLocationResponse` → `{"Driver": "posix", "Location": "..."}`
// -----------------------------------------------------------------------------
struct FileLocationResponse {
  std::string driver;
  std::string location;
};

json::Value ToJson(const FileLocationResponse& response);

// -----------------------------------------------------------------------------
//  §2.5 `getFileList` —— Spring Page 结构
// -----------------------------------------------------------------------------
//  字段是 `Content` / `Number` / `NumberOfElements` / `Size`，**不是**
//  `results` / `totalCount`。`CreatedAt` 用 `yyyy-MM-dd'T'HH:mm:ss.SSS+0000`。
struct FileListEntryDto {
  std::string file_id;
  std::string driver;
  std::string location;
  std::int64_t created_at_epoch_seconds = 0;
  std::string created_by;
};

struct FileListResponse {
  std::vector<FileListEntryDto> content;
  int number = 0;  // PageNum（从 0 开始）
  int number_of_elements = 0;
  int size = 0;  // 请求的 Items
};

json::Value ToJson(const FileListResponse& response);

// -----------------------------------------------------------------------------
//  §2.12 `/v2/info` —— `VersionInfo`
// -----------------------------------------------------------------------------
struct VersionInfoResponse {
  std::string version;
  std::string build_version;
  std::vector<std::string> connected_outer_services;
};

json::Value ToJson(const VersionInfoResponse& response);

// -----------------------------------------------------------------------------
//  §2.9 DMS —— `StorageInstructionsResponse`
// -----------------------------------------------------------------------------
//  ⚠️ 这里是 **camelCase**（`providerKey`/`storageLocation`），与 §2.1 的
//     PascalCase（`FileID`/`Location`）**不同**：两套端点来自上游不同的 Java 类，
//     不能"统一风格"。`expiryTime` 用与 `CreatedAt` 相同的 OSDU 时间戳格式。
struct StorageLocationDto {
  std::string signed_url;                     // → `signedUrl`
  std::string file_source;                    // → `fileSource`
  std::string created_by;                     // → `createdBy`
  std::int64_t expires_at_epoch_seconds = 0;  // → `expiryTime`（ISO-8601）
};

struct StorageInstructionsResponse {
  std::string provider_key;  // `POSIX` / `S3`（或 `storage.provider_key_override`）
  StorageLocationDto storage_location;
};

json::Value ToJson(const StorageInstructionsResponse& response);

// -----------------------------------------------------------------------------
//  §2.9 DMS —— `RetrievalInstructionsResponse`
// -----------------------------------------------------------------------------
struct RetrievalPropertiesDto {
  std::string signed_url;  // → `signedUrl`
};

struct RetrievalInstructionDto {
  std::string dataset_registry_id;  // → `datasetRegistryId`
  RetrievalPropertiesDto retrieval_properties;
  std::string provider_key;
};

struct RetrievalInstructionsResponse {
  std::vector<RetrievalInstructionDto> datasets;
};

json::Value ToJson(const RetrievalInstructionsResponse& response);

// -----------------------------------------------------------------------------
//  §2.9 DMS —— `POST files/copy` 的响应是**数组**（每个元素一个结果）
// -----------------------------------------------------------------------------
struct CopyDmsResponse {
  bool success = false;
  std::string dataset_blob_storage_path;  // → `datasetBlobStoragePath`
};

json::Value ToJson(const std::vector<CopyDmsResponse>& results);

// -----------------------------------------------------------------------------
//  §2.10 `POST delivery/GetFileSignedUrl` → `UrlSigningResponse`
// -----------------------------------------------------------------------------
//  `processed` 是**以 SRN 为键的对象**（不是数组），`unprocessed` 是数组。
//  `connectionString` 必须显式输出 `null`（上游客户端按字段存在性判断）。
struct SignedUrlDto {
  std::string signed_url;
  std::string unsigned_url;
  std::string kind;
};

struct UrlSigningResponse {
  std::map<std::string, SignedUrlDto> processed;  // 键 = SRN（有序，便于比对）
  std::vector<std::string> unprocessed;
};

json::Value ToJson(const UrlSigningResponse& response);

// -----------------------------------------------------------------------------
//  §2.6/§2.7 元数据记录 + `version`
// -----------------------------------------------------------------------------
//  PascalCase 映射集中在领域层（`domain/model/file_metadata.cpp`），适配层只加信封
//  里本来就有、但只在响应里才出现的 `version` —— 领域 `ToJson` 已包含它，这里保持透传。
json::Value RecordToJson(const domain::FileMetadataRecord& record);

}  // namespace fss::adapters::http
