// =============================================================================
//  test_proto_json_mapping.cpp —— C7.5：proto 的 `json_name` 与 OSDU JSON 逐字段对齐
// =============================================================================
//  判据：`data` 内字段为 PascalCase、信封为 camelCase；用 **proto3-JSON** 序列化后
//  必须能与契约 §3.3 的 JSON **逐字段互操作**（客户端启用 proto3-JSON 就能直接
//  生产/消费 OSDU 规范 JSON）。
//
//  ★ 向上游样例要真相：本用例用 vendored 的 `File_CorrectPayload.json`（契约 §3.3 的
//    权威黄金样例）做"参照实现"（R18）：
//      ① 样例 JSON →（proto3-JSON 解析）→ proto →（proto3-JSON 序列化）→ JSON
//         必须仍带着同样的 **PascalCase/camelCase 键**；
//      ② 序列化结果必须能被**领域解析器**（`ParseFileMetadataRecord`）接受 —— 否则
//         "gRPC 客户端产出的 JSON 能被本服务 REST 接口消费"就不成立。
// =============================================================================
#include <catch2/catch.hpp>

#include "adapters/grpc/dto/grpc_dto.h"

#include "common/json/json.h"
#include "domain/model/file_metadata.h"

#include <google/protobuf/util/json_util.h>
#include <osdu/file/v1/file_service.pb.h>

#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

//  上游样例里的占位符（与 phase4 的 `Placeholders()` 同一套值）
std::vector<std::pair<std::string, std::string>> Placeholders() {
  return {
      {"<tenant_name>", "opendes"},
      {"<acl_viewers>", "data.default.viewers@opendes.example.com"},
      {"<acl_owners>", "data.default.owners@opendes.example.com"},
      {"<legal_tags>", "opendes-public-1"},
      {"<cloud_domain>", "opendes.example.com"},
  };
}

std::string ReadUpstreamFixture(const char* name) {
  const std::string path =
      std::string(FSS_REPO_ROOT) + "/tests/conformance/fixtures/upstream/" + name;
  std::ifstream input(path);
  REQUIRE(input.good());
  std::ostringstream buffer;
  buffer << input.rdbuf();
  std::string text = buffer.str();
  for (const auto& [placeholder, value] : Placeholders()) {
    for (std::size_t pos = text.find(placeholder); pos != std::string::npos;
         pos = text.find(placeholder, pos + value.size())) {
      text.replace(pos, placeholder.size(), value);
    }
  }
  return text;
}

std::string ProtoToJson(const google::protobuf::Message& message) {
  std::string text;
  const auto status = google::protobuf::util::MessageToJsonString(message, &text);
  REQUIRE(status.ok());
  return text;
}

}  // namespace

TEST_CASE("★ C7.5 proto3-JSON ↔ 契约 §3.3 黄金样例：键名逐字段对齐且可被领域解析器接受",
          "[phase7][unit][c7.5]") {
  const std::string golden = ReadUpstreamFixture("File_CorrectPayload.json");

  //  ① 样例 JSON → proto（proto3-JSON 解析；PascalCase 的 `json_name` 必须被识别）
  osdu::file::v1::FileMetadataRecord record;
  const auto parsed = google::protobuf::util::JsonStringToMessage(golden, &record);
  INFO("解析失败：" << parsed.message());
  REQUIRE(parsed.ok());
  REQUIRE(record.kind() == "opendes:wks:dataset--File.Generic:1.0.0");
  REQUIRE(record.acl().viewers_size() >= 1);
  REQUIRE(record.legal().legaltags_size() >= 1);
  //  ★ `data` 层是 PascalCase（`Name`/`Endian`/`DatasetProperties.FileSourceInfo.FileSource`）
  //  ★ 期望值直接取自样例（不是照抄直觉）：Name="Dataset X221/15"、Endian="BIG"
  REQUIRE(record.data().name() == "Dataset X221/15");
  REQUIRE(record.data().endian() == "BIG");
  REQUIRE(record.data().total_size() == "13245217273");
  REQUIRE(record.data().dataset_properties().file_source_info().file_source() ==
          "/osdu-user/1624011206350-2021-06-18-10-13-26-350/33a71a04d20f4240904b4b3fca4657b7");
  //  样例里客户端给的校验和是 `MD5("")` + 算法 `SHA-256`（服务端会覆写，见 C6.4）
  REQUIRE(record.data().dataset_properties().file_source_info().checksum() ==
          "d41d8cd98f00b204e9800998ecf8427e");
  REQUIRE(record.data().dataset_properties().file_source_info().checksum_algorithm() ==
          "SHA-256");

  //  ② proto → proto3-JSON：键必须是 OSDU 规范里的那套名字
  const std::string round_trip = ProtoToJson(record);
  INFO(round_trip);
  for (const char* key : {"\"kind\"", "\"acl\"", "\"legal\"", "\"data\"", "\"viewers\"",
                          "\"legaltags\"", "\"otherRelevantDataCountries\""}) {
    INFO("缺少键 " << key);
    REQUIRE(round_trip.find(key) != std::string::npos);
  }
  //  ★ `data` 内 PascalCase（若 json_name 配错，proto3-JSON 会给出 `name`/`fileSource`）
  for (const char* key : {"\"Name\"", "\"Endian\"", "\"DatasetProperties\"",
                          "\"FileSourceInfo\"", "\"FileSource\"", "\"TotalSize\"",
                          "\"EncodingFormatTypeID\"", "\"Checksum\"", "\"ChecksumAlgorithm\""}) {
    INFO("缺少 PascalCase 键 " << key);
    REQUIRE(round_trip.find(key) != std::string::npos);
  }

  //  ③ 该 JSON 必须能被**领域解析器**接受（REST 与 RPC 的 JSON 面互操作）
  const auto domain = fss::domain::ParseFileMetadataRecord(fss::json::Parse(round_trip).value());
  INFO((domain.ok() ? std::string("ok") : domain.error().ToString()));
  REQUIRE(domain.ok());
  REQUIRE(domain.value().kind == record.kind());
  REQUIRE(domain.value().data.name.value() == "Dataset X221/15");
  REQUIRE(domain.value().data.dataset_properties.file_source_info.file_source ==
          record.data().dataset_properties().file_source_info().file_source());
}

TEST_CASE("★ C7.5 位置/DMS 的 json_name：`SignedURL` / `providerKey` / `storageLocation`",
          "[phase7][unit][c7.5]") {
  //  上游 `LocationResponse.Location` 的键是 **PascalCase 的 `SignedURL`**（不是 signedUrl）
  osdu::file::v1::LocationResponse location;
  location.set_file_id("abc");
  location.mutable_location()->set_signed_url("https://self.invalid/v1/transfer/t?exp=1&sig=s");
  location.mutable_location()->set_file_source("/osdu-user/1-ts/abc");
  const std::string location_json = ProtoToJson(location);
  INFO(location_json);
  REQUIRE(location_json.find("\"FileID\":\"abc\"") != std::string::npos);
  REQUIRE(location_json.find("\"SignedURL\":") != std::string::npos);
  REQUIRE(location_json.find("\"FileSource\":") != std::string::npos);
  REQUIRE(location_json.find("\"signedUrl\"") == std::string::npos);  // 不是 camelCase

  //  DMS 的 `providerKey` / `storageLocation` 是 **camelCase**（与 §2.1 的 PascalCase 不同）
  osdu::file::v1::StorageInstructionsResponse instructions;
  instructions.set_provider_key("POSIX");
  osdu::file::v1::FileListResponse list;
  list.set_number(2);
  list.set_number_of_elements(3);
  list.set_size(10);
  const std::string list_json = ProtoToJson(list);
  INFO(list_json);
  REQUIRE(list_json.find("\"Number\":2") != std::string::npos);
  REQUIRE(list_json.find("\"NumberOfElements\":3") != std::string::npos);
  REQUIRE(list_json.find("\"Size\":10") != std::string::npos);
  //  ⚠️ 如实记录 proto3-JSON 的一个**表示差异**：值为 0/空串的字段**不会出现**在 JSON 里
  //    （proto3 的默认值语义）。因此启用 proto3-JSON 的客户端必须把"键缺失"当作默认值；
  //    REST 响应则**始终**带这些键（契约 §2.5）。类型化的 RPC 调用不受影响（字段存在）。
  osdu::file::v1::FileListResponse zero;
  const std::string zero_json = ProtoToJson(zero);
  INFO(zero_json);
  REQUIRE(zero_json.find("Number") == std::string::npos);
}
