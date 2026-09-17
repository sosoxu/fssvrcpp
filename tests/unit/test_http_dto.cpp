// =============================================================================
//  C4.4：DTO 字段大小写逐字符（线上契约）
// =============================================================================
//  契约 §3.2 的大小写规则是**线上契约**：`FileID` / `Location` / `SignedURL` /
//  `FileSource` / `SignedUrl` / `Content` / `NumberOfElements` / `Size` 必须逐字符一致，
//  而 `fileId` / `fileSource` / `results` 这类"看起来更自然"的名字**不得出现**。
//
//  ⚠️ 为什么用"序列化后的字符串里查子串"而不是 `value["FileID"]`：
//    后者对大小写错误的**写法**也一样能编译通过（只要测试和实现犯同样的错就会一起绿）。
//    直接断言线上字节才能钉住大小写。
// =============================================================================
#include <catch2/catch.hpp>

#include "adapters/http/dto/dto.h"
#include "common/json/json.h"

#include <string>
#include <vector>

using fss::adapters::http::DownloadUrlResponse;
using fss::adapters::http::FileListEntryDto;
using fss::adapters::http::FileListResponse;
using fss::adapters::http::FileLocationResponse;
using fss::adapters::http::LocationBody;
using fss::adapters::http::LocationResponse;
using fss::adapters::http::VersionInfoResponse;

TEST_CASE("★ C4.4 §2.1 LocationResponse：FileID / Location{SignedURL, FileSource}",
          "[phase4][dto][c4.4]") {
  LocationResponse response;
  response.file_id = "da92f52401dc4d1cb93515f159c110d4";
  response.location.signed_url = "https://self.invalid/v1/transfer/tok?exp=1&sig=s";
  response.location.file_source = "/osdu-user/1-ts/da92f524";

  const std::string text = fss::json::Dump(ToJson(response));
  INFO(text);

  //  必须逐字符出现的键
  REQUIRE(text.find("\"FileID\":\"da92f52401dc4d1cb93515f159c110d4\"") != std::string::npos);
  REQUIRE(text.find("\"Location\":{") != std::string::npos);
  REQUIRE(text.find("\"SignedURL\":") != std::string::npos);
  REQUIRE(text.find("\"FileSource\":\"/osdu-user/1-ts/da92f524\"") != std::string::npos);

  //  禁止出现的写法（大小写漂移）
  REQUIRE(text.find("fileId") == std::string::npos);
  REQUIRE(text.find("fileID") == std::string::npos);
  REQUIRE(text.find("fileSource") == std::string::npos);
  REQUIRE(text.find("signedUrl") == std::string::npos);
  //  契约 §2.1 明确：该响应**不返回 `Driver`**
  REQUIRE(text.find("Driver") == std::string::npos);
}

TEST_CASE("★ C4.4 §2.4 与 §2.1 的大小写差异：SignedUrl vs SignedURL",
          "[phase4][dto][c4.4]") {
  DownloadUrlResponse response;
  response.signed_url = "https://self.invalid/x";
  const std::string text = fss::json::Dump(ToJson(response));

  REQUIRE(text == "{\"SignedUrl\":\"https://self.invalid/x\"}");
  REQUIRE(text.find("SignedURL") == std::string::npos);  // 这里必须是小写 url
}

TEST_CASE("C4.4 §2.3 FileLocationResponse：Driver + Location", "[phase4][dto][c4.4]") {
  FileLocationResponse response;
  response.driver = "posix";
  response.location = "/var/lib/fss/data/opendes/persistent/2025/09/16/da92";
  const auto value = ToJson(response);
  REQUIRE(value["Driver"] == "posix");
  REQUIRE(value["Location"] == response.location);
}

TEST_CASE("★ C4.4 §2.5 getFileList：Spring Page 结构 + CreatedAt 格式",
          "[phase4][dto][c4.4]") {
  FileListResponse response;
  FileListEntryDto entry;
  entry.file_id = "f-1";
  entry.driver = "posix";
  entry.location = "/root/c/k";
  entry.created_at_epoch_seconds = 1614784413;  // → 2021-03-03T15:13:33.000+0000
  entry.created_by = "osdu-user";
  response.content.push_back(entry);
  response.number = 0;
  response.number_of_elements = 1;
  response.size = 10;

  const auto value = ToJson(response);
  const std::string text = fss::json::Dump(value);
  INFO(text);

  REQUIRE(value["Number"] == 0);
  REQUIRE(value["NumberOfElements"] == 1);
  REQUIRE(value["Size"] == 10);
  REQUIRE(value["Content"].is_array());
  REQUIRE(value["Content"][0]["FileID"] == "f-1");
  REQUIRE(value["Content"][0]["Driver"] == "posix");
  REQUIRE(value["Content"][0]["CreatedBy"] == "osdu-user");
  //  ★ 契约 §2.5：`yyyy-MM-dd'T'HH:mm:ss.SSS+0000`（末尾是 +0000，不是 Z）
  REQUIRE(value["Content"][0]["CreatedAt"] == "2021-03-03T15:13:33.000+0000");

  //  禁止出现的"看起来更自然"的名字
  REQUIRE(text.find("\"results\"") == std::string::npos);
  REQUIRE(text.find("\"totalCount\"") == std::string::npos);
  REQUIRE(text.find("\"fileId\"") == std::string::npos);
}

TEST_CASE("C4.4 §2.12 VersionInfo：版本字段 + connectedOuterServices", "[phase4][dto][c4.4]") {
  VersionInfoResponse response;
  response.version = "v2";
  response.build_version = "0.1.0";
  response.connected_outer_services = {"storage"};
  const auto value = ToJson(response);
  REQUIRE(value["version"] == "v2");
  REQUIRE(value["buildVersion"] == "0.1.0");
  REQUIRE(value["connectedOuterServices"][0] == "storage");
}

TEST_CASE("★ C4.4 §2.9 DMS：camelCase（providerKey/signedUrl/datasetRegistryId）",
          "[phase4][dto][c4.4]") {
  fss::adapters::http::StorageInstructionsResponse storage;
  storage.provider_key = "POSIX";
  storage.storage_location.signed_url = "https://self.invalid/v1/transfer/t?exp=1&sig=s";
  storage.storage_location.file_source = "/osdu-user/1-ts/abc";
  storage.storage_location.created_by = "osdu-user";
  storage.storage_location.expires_at_epoch_seconds = 1700003600;

  const std::string storage_text = fss::json::Dump(ToJson(storage));
  INFO(storage_text);
  REQUIRE(storage_text.find("\"providerKey\":\"POSIX\"") != std::string::npos);
  REQUIRE(storage_text.find("\"storageLocation\":{") != std::string::npos);
  REQUIRE(storage_text.find("\"signedUrl\":") != std::string::npos);
  REQUIRE(storage_text.find("\"fileSource\":\"/osdu-user/1-ts/abc\"") != std::string::npos);
  REQUIRE(storage_text.find("\"createdBy\":\"osdu-user\"") != std::string::npos);
  //  `expiryTime` 与 `CreatedAt` 用同一套 OSDU 时间戳（末尾 +0000，不是 Z）
  REQUIRE(storage_text.find("\"expiryTime\":\"2023-11-14T23:13:20.000+0000\"") !=
          std::string::npos);
  //  ★ DMS 侧**不得**出现 §2.1 的 PascalCase 写法（两套端点来自不同的上游类）
  REQUIRE(storage_text.find("SignedURL") == std::string::npos);
  REQUIRE(storage_text.find("FileSource\"") == std::string::npos);
  REQUIRE(storage_text.find("\"ProviderKey\"") == std::string::npos);

  fss::adapters::http::RetrievalInstructionsResponse retrieval;
  fss::adapters::http::RetrievalInstructionDto item;
  item.dataset_registry_id = "opendes:dataset--File.Generic:abc";
  item.retrieval_properties.signed_url = "https://self.invalid/v1/transfer/g";
  item.provider_key = "POSIX";
  retrieval.datasets.push_back(item);
  const std::string retrieval_text = fss::json::Dump(ToJson(retrieval));
  INFO(retrieval_text);
  REQUIRE(retrieval_text.find("\"datasets\":[") != std::string::npos);
  REQUIRE(retrieval_text.find("\"datasetRegistryId\":\"opendes:dataset--File.Generic:abc\"") !=
          std::string::npos);
  REQUIRE(retrieval_text.find("\"retrievalProperties\":{\"signedUrl\":") != std::string::npos);
  REQUIRE(retrieval_text.find("\"providerKey\":\"POSIX\"") != std::string::npos);

  const std::vector<fss::adapters::http::CopyDmsResponse> copies = {
      {true, "opendes-persistent/osdu-user/1-ts/abc"},
      {false, ""},
  };
  const auto copies_value = ToJson(copies);
  REQUIRE(copies_value.is_array());  // ★ 契约 §2.9：copy 的响应是**数组**
  REQUIRE(copies_value[0]["success"] == true);
  REQUIRE(copies_value[0]["datasetBlobStoragePath"] == "opendes-persistent/osdu-user/1-ts/abc");
  REQUIRE(copies_value[1]["success"] == false);
}

TEST_CASE("★ C4.4 §2.10 delivery：processed 是对象、connectionString 必须显式为 null",
          "[phase4][dto][c4.4]") {
  fss::adapters::http::UrlSigningResponse response;
  fss::adapters::http::SignedUrlDto entry;
  entry.signed_url = "https://self.invalid/v1/transfer/g?exp=1&sig=s";
  entry.unsigned_url = "/osdu-user/1-ts/abc";
  entry.kind = "opendes:wks:dataset--File.Generic:1.0.0";
  response.processed["srn:file/abc"] = entry;
  response.unprocessed = {"srn:file/unknown"};

  const std::string text = fss::json::Dump(ToJson(response));
  INFO(text);
  REQUIRE(text.find("\"processed\":{") != std::string::npos);  // 对象，不是数组
  REQUIRE(text.find("\"srn:file/abc\":{") != std::string::npos);
  REQUIRE(text.find("\"signedUrl\":") != std::string::npos);
  REQUIRE(text.find("\"unsignedUrl\":\"/osdu-user/1-ts/abc\"") != std::string::npos);
  REQUIRE(text.find("\"kind\":\"opendes:wks:dataset--File.Generic:1.0.0\"") !=
          std::string::npos);
  //  ★ 字段必须**存在且为 null**（上游客户端按字段存在性判断）
  REQUIRE(text.find("\"connectionString\":null") != std::string::npos);
  REQUIRE(text.find("\"unprocessed\":[\"srn:file/unknown\"]") != std::string::npos);
}
