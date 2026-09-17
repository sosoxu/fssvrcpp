// =============================================================================
//  test_dms_delivery.cpp —— C6.7：DMS 6 端点 + Delivery 的**响应键集合**与端到端流程
// =============================================================================
//  判据 C6.7：状态码、角色、**响应键集合**全部断言通过。
//  角色与"403 先于 400"由 `tests/unit/test_roles.cpp` 覆盖（12 个用例逐一驱动），
//  本文件负责：① 每个响应的**键集合**（多一个少一个都失败）；② 上游 DMS 的**端到端场景**
//  （storageInstructions → 上传 → 登记元数据 → retrievalInstructions → 取回字节比对）；
//  ③ 状态码。
//
//  上游一手依据（vendored 到 `/home/ll/osdu-file-upstream`，commit d7c25c2d）：
//    · `provider/file-azure/.../service/StorageServiceImpl.java:293`
//        `AzureFileDmsUploadLocation{signedUrl, fileSource, createdBy, expiryTime}`
//    · `provider/file-azure/.../service/StorageServiceImpl.java:268`
//        `AzureFileDmsDownloadLocation{signedUrl, fileSource, createdBy, expiryTime}`
//    · `provider/file-azure/.../service/FileCollectionStorageServiceImpl.java:103/179`
//        `AzureFileCollectionDmsUploadLocation{signedUrl, **fileCollectionSource**, fileCount,
//        fileNames, createdBy, expiryTime}`（下载侧同一形状）
//    · `file-core/.../service/FileDmsServiceImpl.java:99`（copy）→
//        `CopyDmsResponse{success, datasetBlobStoragePath}`，路径是**目标（persistent）**位置
//    · `testing/file-test-baremetal/.../features/IntegrationTest_DMS.feature`（端到端场景）
//      + `stepdefs/FileDMSStepdefs.java`（用 `storageLocation.fileSource` / `.signedUrl`、
//      `datasets[].retrievalProperties.signedUrl` 这些**键名**驱动）
//
//  ★ 最容易错的一处：`/v2/files/*` 与 `/v2/file-collections/*` **不是同一套键**——
//    集合版用 `fileCollectionSource`（并带 `fileCount`/`fileNames`），**没有** `fileSource`。
//    此前两条路由共用同一个 handler，集合版返回了 `fileSource`（P6-D11）。
// =============================================================================
#include <catch2/catch.hpp>

#include "http_fixture.h"

#include "common/time/time_format.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace {

using fss::test::AppFixture;
using fss::test::HttpFixture;

std::set<std::string> KeysOf(const fss::json::Value& object) {
  std::set<std::string> keys;
  for (auto it = object.begin(); it != object.end(); ++it) keys.insert(it.key());
  return keys;
}

fss::test::Reply Post(int port, const std::string& target, const std::string& body = {},
                      const std::string& partition = "opendes") {
  return fss::test::HttpDo(port, "POST", target, fss::test::Authed(partition), body);
}

//  DMS 的上传指令 → 用 signedUrl 真上传 → 返回 fileSource
struct StorageInstruction {
  std::string signed_url;
  std::string file_source;
  fss::json::Value storage_location;
  fss::json::Value body;
};

StorageInstruction GetStorageInstruction(const std::string& prefix, int port,
                                        const std::string& payload = "dms-payload") {
  const auto reply = Post(port, "/api/file/v2" + prefix + "/storageInstructions");
  INFO("storageInstructions(" << prefix << ") → " << reply.status << " " << reply.body);
  REQUIRE(reply.status == 200);
  const auto body = fss::json::ParseObject(reply.body);
  REQUIRE(body.ok());
  StorageInstruction out;
  out.body = body.value();
  out.storage_location = body.value()["storageLocation"];
  out.signed_url = out.storage_location["signedUrl"].get<std::string>();
  //  集合版的键是 `fileCollectionSource`（没有 `fileSource`）——同一个 helper 两种都读
  out.file_source = out.storage_location.contains("fileSource")
                        ? out.storage_location["fileSource"].get<std::string>()
                        : out.storage_location["fileCollectionSource"].get<std::string>();

  const auto put = fss::test::HttpDo(port, "PUT", fss::test::TargetOf(out.signed_url),
                                    fss::test::Authed(), payload);
  REQUIRE(put.status == 200);
  return out;
}

std::string RegisterMetadata(HttpFixture& fx, const std::string& file_source,
                             const std::string& name = "dms.bin") {
  auto record = AppFixture::MakeRecord(file_source, name);
  const auto created = Post(fx.port(), "/api/file/v2/files/metadata",
                            fss::json::Dump(fss::domain::ToJson(record)));
  INFO("POST metadata → " << created.status << " " << created.body);
  REQUIRE(created.status == 201);
  return fss::json::ParseObject(created.body).value()["id"].get<std::string>();
}

//  时间戳形态：`yyyy-MM-ddTHH:mm:ss.SSS+0000`
bool LooksLikeOsduTime(const std::string& value) {
  return value.size() == 28 && value.substr(4, 1) == "-" && value.substr(10, 1) == "T" &&
         value.substr(19, 1) == "." && value.substr(23) == "+0000";
}

}  // namespace

TEST_CASE("★ C6.7 files/storageInstructions：键集合 + signedUrl 真能上传",
          "[phase6][conformance][c6.7]") {
  HttpFixture fx;
  const auto instruction = GetStorageInstruction("/files", fx.port());

  //  顶层恰好两个键
  REQUIRE(KeysOf(instruction.body) == std::set<std::string>{"providerKey", "storageLocation"});
  //  ★ 上传位置（AzureFileDmsUploadLocation）恰好 4 个键
  REQUIRE(KeysOf(instruction.storage_location) ==
          std::set<std::string>{"signedUrl", "fileSource", "createdBy", "expiryTime"});
  REQUIRE(instruction.body["providerKey"].get<std::string>() == "MEMORY");  // 真实驱动，大写
  REQUIRE(instruction.storage_location["createdBy"].get<std::string>() == "osdu-user");
  REQUIRE(LooksLikeOsduTime(instruction.storage_location["expiryTime"].get<std::string>()));
  REQUIRE(instruction.file_source.rfind("/", 0) == 0);  // 相对路径形态

  //  指令里没有 fileCollectionSource（那是集合版才有的键）
  REQUIRE(instruction.storage_location.find("fileCollectionSource") ==
          instruction.storage_location.end());

  //  ★ 端到端：signedUrl 必须真的能把字节写进 staging（`GetStorageInstruction` 已断言 200）
  const auto location = fx.locations.FindByFileSource("opendes", instruction.file_source);
  REQUIRE(location.ok());
  REQUIRE(location.value().zone == fss::domain::StorageZone::kStaging);
}

TEST_CASE("★ C6.7 file-collections/storageInstructions：用 fileCollectionSource（不是 fileSource）",
          "[phase6][conformance][c6.7]") {
  HttpFixture fx;
  const auto instruction = GetStorageInstruction("/file-collections", fx.port());

  REQUIRE(KeysOf(instruction.body) == std::set<std::string>{"providerKey", "storageLocation"});
  //  ★ 集合版（AzureFileCollectionDmsUploadLocation）：fileCollectionSource + fileCount + fileNames
  REQUIRE(KeysOf(instruction.storage_location) ==
          std::set<std::string>{"signedUrl", "fileCollectionSource", "fileCount", "fileNames",
                                "createdBy", "expiryTime"});
  REQUIRE(instruction.storage_location.find("fileSource") == instruction.storage_location.end());
  REQUIRE(instruction.storage_location["fileCollectionSource"].get<std::string>() ==
          instruction.file_source);
  //  本项目一个指令 = 一个对象：count 1、names 恰一项（上游是列出目录内全部文件）
  REQUIRE(instruction.storage_location["fileCount"].get<int>() == 1);
  REQUIRE(instruction.storage_location["fileNames"].size() == 1);
  REQUIRE(instruction.storage_location["fileNames"][0].get<std::string>() ==
          instruction.file_source.substr(instruction.file_source.rfind('/') + 1));
}

TEST_CASE("★ C6.7 DMS 端到端：storageInstructions → 上传 → 登记 → retrievalInstructions → 取回同一份字节",
          "[phase6][conformance][c6.7]") {
  HttpFixture fx;
  const std::string payload = "dms-end-to-end-bytes";
  const auto instruction = GetStorageInstruction("/files", fx.port(), payload);
  const std::string record_id = RegisterMetadata(fx, instruction.file_source, "e2e-dms.bin");

  //  ---- retrievalInstructions ----
  fss::json::Value request = fss::json::Value::object();
  request["datasetRegistryIds"] = fss::json::Value::array({record_id});
  const auto reply = Post(fx.port(), "/api/file/v2/files/retrievalInstructions",
                          fss::json::Dump(request));
  INFO("retrievalInstructions → " << reply.status << " " << reply.body);
  REQUIRE(reply.status == 200);
  const auto body = fss::json::ParseObject(reply.body);
  REQUIRE(body.ok());

  REQUIRE(KeysOf(body.value()) == std::set<std::string>{"datasets"});
  REQUIRE(body.value()["datasets"].size() == 1);
  const auto& dataset = body.value()["datasets"][0];
  REQUIRE(KeysOf(dataset) ==
          std::set<std::string>{"datasetRegistryId", "retrievalProperties", "providerKey"});
  REQUIRE(dataset["datasetRegistryId"].get<std::string>() == record_id);
  REQUIRE(dataset["providerKey"].get<std::string>() == "MEMORY");
  //  ★ 下载位置的键集合 = AzureFileDmsDownloadLocation（不只是 signedUrl）
  const auto& properties = dataset["retrievalProperties"];
  REQUIRE(KeysOf(properties) ==
          std::set<std::string>{"signedUrl", "fileSource", "createdBy", "expiryTime"});
  REQUIRE(properties["fileSource"].get<std::string>() == instruction.file_source);
  REQUIRE(LooksLikeOsduTime(properties["expiryTime"].get<std::string>()));

  //  ★ 用返回的 signedUrl 取回字节，必须与上传的一模一样（上游 feature 的最后一步）
  const auto download = fss::test::HttpDo(
      fx.port(), "GET", fss::test::TargetOf(properties["signedUrl"].get<std::string>()),
      fss::test::Authed());
  REQUIRE(download.status == 200);
  REQUIRE(download.body == payload);
}

TEST_CASE("★ C6.7 file-collections/retrievalInstructions：retrievalProperties 用集合键集合",
          "[phase6][conformance][c6.7]") {
  HttpFixture fx;
  const auto instruction = GetStorageInstruction("/file-collections", fx.port(), "collection-body");
  const std::string record_id = RegisterMetadata(fx, instruction.file_source, "coll.bin");

  fss::json::Value request = fss::json::Value::object();
  request["datasetRegistryIds"] = fss::json::Value::array({record_id});
  const auto reply = Post(fx.port(), "/api/file/v2/file-collections/retrievalInstructions",
                          fss::json::Dump(request));
  INFO("collection retrievalInstructions → " << reply.status << " " << reply.body);
  REQUIRE(reply.status == 200);
  const auto body = fss::json::ParseObject(reply.body);
  REQUIRE(body.ok());
  REQUIRE(body.value()["datasets"].size() == 1);
  const auto& properties = body.value()["datasets"][0]["retrievalProperties"];
  REQUIRE(KeysOf(properties) ==
          std::set<std::string>{"signedUrl", "fileCollectionSource", "fileCount", "fileNames",
                                "createdBy", "expiryTime"});
  REQUIRE(properties.find("fileSource") == properties.end());
  REQUIRE(properties["fileCount"].get<int>() == 1);
  REQUIRE(properties["fileNames"].size() == 1);

  //  取回的字节同样必须一致
  const auto download = fss::test::HttpDo(
      fx.port(), "GET", fss::test::TargetOf(properties["signedUrl"].get<std::string>()),
      fss::test::Authed());
  REQUIRE(download.status == 200);
  REQUIRE(download.body == "collection-body");
}

TEST_CASE("★ C6.7 files|file-collections/copy：数组元素恰好 {success, datasetBlobStoragePath}",
          "[phase6][conformance][c6.7]") {
  HttpFixture fx;
  const auto instruction = GetStorageInstruction("/files", fx.port(), "copy-me");
  (void)RegisterMetadata(fx, instruction.file_source, "copy.bin");

  fss::json::Value request = fss::json::Value::object();
  request["datasetSources"] = fss::json::Value::array(
      {fss::json::Value::object({{"data",
                                 fss::json::Value::object(
                                     {{"DatasetProperties",
                                       fss::json::Value::object(
                                           {{"FileSourceInfo",
                                             fss::json::Value::object(
                                                 {{"FileSource", instruction.file_source}})}})}})}})});
  const auto reply = Post(fx.port(), "/api/file/v2/files/copy", fss::json::Dump(request));
  INFO("copy → " << reply.status << " " << reply.body);
  REQUIRE(reply.status == 200);

  const auto body = fss::json::Parse(reply.body);
  REQUIRE(body.ok());
  REQUIRE(body.value().is_array());
  REQUIRE(body.value().size() == 1);
  REQUIRE(KeysOf(body.value()[0]) ==
          std::set<std::string>{"success", "datasetBlobStoragePath"});
  REQUIRE(body.value()[0]["success"].get<bool>());
  //  `datasetBlobStoragePath` 是**目标（persistent）**路径（FileDmsServiceImpl:113）
  const std::string path = body.value()[0]["datasetBlobStoragePath"].get<std::string>();
  REQUIRE(path.find(instruction.file_source) != std::string::npos);

  //  file-collections 的 copy 是同一形状（数组元素键集合相同）
  const auto collection_reply =
      Post(fx.port(), "/api/file/v2/file-collections/copy", fss::json::Dump(request));
  REQUIRE(collection_reply.status == 200);
  const auto collection_body = fss::json::Parse(collection_reply.body);
  REQUIRE(collection_body.ok());
  REQUIRE(KeysOf(collection_body.value()[0]) ==
          std::set<std::string>{"success", "datasetBlobStoragePath"});
}

TEST_CASE("★ C6.7 delivery/GetFileSignedUrl：processed/unprocessed 与显式 null connectionString",
          "[phase6][conformance][c6.7]") {
  HttpFixture fx;
  const auto instruction = GetStorageInstruction("/files", fx.port(), "delivery-body");
  const std::string record_id = RegisterMetadata(fx, instruction.file_source, "delivery.bin");
  const std::string file_id =
      fx.locations.FindByFileSource("opendes", instruction.file_source).value().file_id;

  const std::string known = "srn:file/" + file_id;
  const std::string unknown = "srn:file/00000000000000000000000000000000";
  fss::json::Value request = fss::json::Value::object();
  request["srns"] = fss::json::Value::array({known, unknown});
  const auto reply = Post(fx.port(), "/api/file/v2/delivery/GetFileSignedUrl",
                          fss::json::Dump(request));
  INFO("delivery → " << reply.status << " " << reply.body);
  REQUIRE(reply.status == 200);
  const auto body = fss::json::ParseObject(reply.body);
  REQUIRE(body.ok());

  REQUIRE(KeysOf(body.value()) == std::set<std::string>{"processed", "unprocessed"});
  //  已知 SRN：processed 里恰好 4 个键，且 `connectionString` 必须**存在且为 null**
  REQUIRE(body.value()["processed"].size() == 1);
  const auto& entry = body.value()["processed"][known];
  REQUIRE(KeysOf(entry) ==
          std::set<std::string>{"signedUrl", "unsignedUrl", "kind", "connectionString"});
  REQUIRE(entry["connectionString"].is_null());
  REQUIRE(entry["kind"].get<std::string>() == "opendes:wks:dataset--File.Generic:1.0.0");
  REQUIRE(entry["unsignedUrl"].get<std::string>() == instruction.file_source);
  //  未知 SRN：进 unprocessed（不是错误）
  REQUIRE(body.value()["unprocessed"].size() == 1);
  REQUIRE(body.value()["unprocessed"][0].get<std::string>() == unknown);

  //  signedUrl 同样必须能取回字节
  const auto download = fss::test::HttpDo(
      fx.port(), "GET", fss::test::TargetOf(entry["signedUrl"].get<std::string>()),
      fss::test::Authed());
  REQUIRE(download.status == 200);
  REQUIRE(download.body == "delivery-body");
}

TEST_CASE("★ C6.7 DMS 状态码：非法请求体 → 400；未授权的角色 → 403（角色集合见 test_roles）",
          "[phase6][conformance][c6.7]") {
  HttpFixture fx;

  //  datasetRegistryIds 不是数组 → 400
  REQUIRE(Post(fx.port(), "/api/file/v2/files/retrievalInstructions", R"({"datasetRegistryIds":"x"})")
              .status == 400);
  //  datasetSources 缺失 → 400
  REQUIRE(Post(fx.port(), "/api/file/v2/files/copy", "{}").status == 400);
  //  srns 不是数组 → 400
  REQUIRE(Post(fx.port(), "/api/file/v2/delivery/GetFileSignedUrl", R"({"srns":"x"})").status == 400);

  //  正例对照（R16）：空数组是**合法**请求 → 200（不是 400）
  REQUIRE(Post(fx.port(), "/api/file/v2/delivery/GetFileSignedUrl", R"({"srns":[]})").status == 200);
  REQUIRE(Post(fx.port(), "/api/file/v2/files/copy", R"({"datasetSources":[]})").status == 200);
}
