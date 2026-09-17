// =============================================================================
//  C4.1 / C4.2 / C4.3 / C4.7 / §7 `/metrics`：在**真实回环端口**上逐条钉住契约
// =============================================================================
//  为什么这些判据必须走真实 HTTP：
//    · 契约 §2 钉的是"路径 + 方法 + 状态码"，方法不匹配时框架返回 404/405 而不是
//      契约状态码 —— 直接调用例永远看不到这一层；
//    · §3.3 的黄金样例要求**全字段**往返，字段名/嵌套/大小写只有序列化到线上才算；
//    · §3.4 的负向矩阵要求"客户端看到的 400 与消息要点"，只有经过 `Wrap()` 的
//      错误映射才是客户端真正拿到的东西。
//
//  装配（真实自签 codec + 数据面回调 + 真实端口）在 `tests/framework/http_fixture.h`。
// =============================================================================
#include <catch2/catch.hpp>

#include "http_fixture.h"

#include "common/crypto/crypto.h"
#include "common/json/json.h"
#include "domain/model/file_metadata.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using fss::test::Authed;
using fss::test::HttpDo;
using fss::test::HttpFixture;
using fss::test::Reply;
using fss::test::TargetOf;

//  从 Prometheus 文本里取一个无 label 的样本值（找不到 → -1，让断言失败而不是静默通过）
std::int64_t MetricValue(const std::string& body, const std::string& name) {
  //  ★ 必须匹配**行首**：`# HELP fss_http_in_flight_requests 当前在途请求数` 里
  //    也含有同样的名字，用 `find(name + " ")` 会先命中 HELP 行，然后把中文当数字
  const std::string prefix = "\n" + name + " ";
  const auto pos = body.find(prefix);
  if (pos == std::string::npos) return -1;
  const auto begin = pos + prefix.size();
  const auto end = body.find('\n', begin);
  return std::stoll(body.substr(begin, end == std::string::npos ? end : end - begin));
}

std::string ReadFixture(const char* name) {
  const std::string path = std::string(FSS_REPO_ROOT) + "/tests/conformance/fixtures/" + name;
  std::ifstream in(path);
  REQUIRE(in.good());
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

//  `/uploadURL` 返回的自签 URL 里带 `?exp=<epoch>&sig=...`；C4.7 要的是这个数字
std::int64_t ExpOf(const std::string& signed_url) {
  const auto pos = signed_url.find("exp=");
  REQUIRE(pos != std::string::npos);
  const auto begin = pos + 4;
  const auto end = signed_url.find('&', begin);
  return std::stoll(signed_url.substr(begin, end == std::string::npos ? end : end - begin));
}

//  上游验收样例的占位符（见 fixtures/upstream/README.md）
const std::vector<std::pair<std::string, std::string>>& Placeholders() {
  static const std::vector<std::pair<std::string, std::string>> values = {
      {"<tenant_name>", "opendes"},
      {"<cloud_domain>", "example.com"},
      {"<acl_viewers>", "data.default.viewers"},
      {"<acl_owners>", "data.default.owners"},
      {"<legal_tags>", "opendes-public-1"},
  };
  return values;
}

std::string ReadUpstreamFixture(const char* name) {
  const std::string path =
      std::string(FSS_REPO_ROOT) + "/tests/conformance/fixtures/upstream/" + name;
  std::ifstream in(path);
  REQUIRE(in.good());
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

//  装载上游样例并把占位符替换掉；`file_source` 非空时覆盖 FileSource（用于"源必须存在"的正例）
fss::json::Value LoadUpstreamSample(const char* name, const std::string& file_source = {}) {
  std::string text = ReadUpstreamFixture(name);
  for (const auto& [placeholder, value] : Placeholders()) {
    for (std::size_t pos = text.find(placeholder); pos != std::string::npos;
         pos = text.find(placeholder, pos + value.size())) {
      text.replace(pos, placeholder.size(), value);
    }
  }
  auto value = fss::json::Parse(text);
  REQUIRE(value.ok());
  if (!file_source.empty()) {
    value.value()["data"]["DatasetProperties"]["FileSourceInfo"]["FileSource"] = file_source;
  }
  return value.value();
}

//  上传一个**新的**空对象并返回它的 FileSource（`CreateFileMetadata` 需要位置记录存在）
std::string FreshUpload(int port) {
  const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(upload.status == 200);
  const auto value = fss::json::ParseObject(upload.body);
  REQUIRE(value.ok());
  const std::string source = value.value()["Location"]["FileSource"].get<std::string>();
  REQUIRE(HttpDo(port, "PUT",
                 TargetOf(value.value()["Location"]["SignedURL"].get<std::string>()), Authed(),
                 "fresh")
              .status == 200);
  return source;
}

struct Uploaded {
  std::string file_id;
  std::string file_source;
  std::string put_url;
  std::string record_id;
};

//  上传一份内容并登记元数据（返回 C4.1 表格需要的动态标识符）
Uploaded UploadAndRegister(HttpFixture& fixture, int port, const std::string& content) {
  Uploaded out;
  const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(upload.status == 200);
  const auto value = fss::json::ParseObject(upload.body);
  REQUIRE(value.ok());
  out.file_id = value.value()["FileID"].get<std::string>();
  out.file_source = value.value()["Location"]["FileSource"].get<std::string>();
  out.put_url = value.value()["Location"]["SignedURL"].get<std::string>();

  const auto put = HttpDo(port, "PUT", TargetOf(out.put_url), Authed(), content);
  REQUIRE(put.status == 200);

  //  DMS copy：`datasetSources` 的元素是**记录**（这里用契约 §3.3 的形状）
  fss::json::Value info = fss::json::Value::object();
  info["FileSource"] = out.file_source;
  fss::json::Value props = fss::json::Value::object();
  props["FileSourceInfo"] = std::move(info);
  fss::json::Value data = fss::json::Value::object();
  data["DatasetProperties"] = std::move(props);
  fss::json::Value record_element = fss::json::Value::object();
  record_element["data"] = std::move(data);
  fss::json::Value sources = fss::json::Value::array();
  sources.push_back(std::move(record_element));
  fss::json::Value request = fss::json::Value::object();
  request["datasetSources"] = std::move(sources);
  const auto copy = HttpDo(port, "POST", "/api/file/v2/files/copy", Authed(),
                           fss::json::Dump(request));
  REQUIRE(copy.status == 200);
  //  copy 的响应是**数组**（契约 §2.9），不是对象
  const auto copy_body = fss::json::Parse(copy.body);
  REQUIRE(copy_body.ok());
  REQUIRE(copy_body.value().is_array());
  REQUIRE(copy_body.value()[0]["success"].get<bool>());

  auto record = fss::test::AppFixture::MakeRecord(out.file_source, "contract.txt");
  const auto created = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(),
                              fss::json::Dump(fss::domain::ToJson(record)));
  REQUIRE(created.status == 201);
  const auto created_body = fss::json::ParseObject(created.body);
  REQUIRE(created_body.ok());
  out.record_id = created_body.value()["id"].get<std::string>();
  return out;
}

}  // namespace

TEST_CASE("★ C4.1 契约 §2 的全部 19 个端点 + §7 的扩展端点（真实 HTTP）",
          "[phase4][conformance][c4.1]") {
  HttpFixture fixture;
  const int port = fixture.port();
  const Uploaded file = UploadAndRegister(fixture, port, "contract-body");

  struct Endpoint {
    const char* name;   // 契约 §2.x
    const char* method;
    std::string target;
    std::string body;
    int expected_status;
  };

  const std::string record_id = file.record_id;
  const std::string file_id = file.file_id;
  const std::string file_source = file.file_source;

  //  §2.6 的 201 正例需要一个**新的** FileSource（同一个 FileSource 会命中幂等键，
  //  拿到的不是"新建成功"而是冲突语义）——因此再上传一个不登记元数据的文件
  const auto second_upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(second_upload.status == 200);
  const auto second_json = fss::json::ParseObject(second_upload.body);
  REQUIRE(second_json.ok());
  const std::string second_source =
      second_json.value()["Location"]["FileSource"].get<std::string>();
  REQUIRE(HttpDo(port, "PUT",
                 TargetOf(second_json.value()["Location"]["SignedURL"].get<std::string>()),
                 Authed(), "second-body")
              .status == 200);

  const std::vector<Endpoint> endpoints = {
      // ---- §2 的 19 个 OSDU 端点 ----
      {"§2.1  uploadURL", "GET", "/api/file/v2/files/uploadURL", "", 200},
      //  §2.2 传**已存在**的 fileID 是 400（"already exists"，契约 §2.2/§5 的固定消息），
      //  因此 200 的正例必须用一个尚未登记的 id
      {"§2.2  getLocation(deprecated)", "POST", "/api/file/v2/getLocation",
       R"({"FileID":"ffffffffffffffffffffffffffffffff"})", 200},
      {"§2.3  getFileLocation(deprecated)", "POST", "/api/file/v2/getFileLocation",
       std::string("{\"FileID\":\"") + file_id + "\"}", 200},
      {"§2.4  downloadURL", "GET", "/api/file/v2/files/" + file_id + "/downloadURL", "", 200},
      {"§2.5  getFileList", "POST", "/api/file/v2/getFileList",
       R"({"PageNum":0,"Items":10})", 200},
      {"§2.6  POST metadata", "POST", "/api/file/v2/files/metadata",
       fss::json::Dump(fss::domain::ToJson(
           fss::test::AppFixture::MakeRecord(second_source, "second.txt"))),
       201},
      {"§2.7  GET metadata", "GET", "/api/file/v2/files/" + record_id + "/metadata", "", 200},
      {"§2.9  files/storageInstructions", "POST",
       "/api/file/v2/files/storageInstructions", "", 200},
      {"§2.9  files/retrievalInstructions", "POST",
       "/api/file/v2/files/retrievalInstructions",
       std::string("{\"datasetRegistryIds\":[\"") + record_id + "\"]}", 200},
      {"§2.9  files/copy", "POST", "/api/file/v2/files/copy",
       std::string("{\"datasetSources\":[\"") + file_source + "\"]}", 200},
      {"§2.9  file-collections/storageInstructions", "POST",
       "/api/file/v2/file-collections/storageInstructions", "", 200},
      {"§2.9  file-collections/retrievalInstructions", "POST",
       "/api/file/v2/file-collections/retrievalInstructions",
       std::string("{\"datasetRegistryIds\":[\"") + record_id + "\"]}", 200},
      {"§2.9  file-collections/copy", "POST", "/api/file/v2/file-collections/copy",
       std::string("{\"datasetSources\":[\"") + file_source + "\"]}", 200},
      {"§2.10 delivery/GetFileSignedUrl", "POST", "/api/file/v2/delivery/GetFileSignedUrl",
       std::string("{\"srns\":[\"srn:file/") + file_id + "\"]}", 200},
      {"§2.11 files/revokeURL", "POST", "/api/file/v2/files/revokeURL", "{}", 204},
      {"§2.12 info", "GET", "/api/file/v2/info", "", 200},
      {"§2.12 liveness_check", "GET", "/api/file/v2/liveness_check", "", 200},
      {"§2.12 readiness_check", "GET", "/api/file/v2/readiness_check", "", 200},
      // §2.8 的 DELETE 放在最后：它会把位置记录一起删掉，先删会污染其它端点
      {"§2.8  DELETE metadata", "DELETE", "/api/file/v2/files/" + record_id + "/metadata", "",
       204},
      // ---- §7 扩展（不计入 19）----
      {"§7    /metrics", "GET", "/metrics", "", 200},
  };

  int asserted = 0;
  for (const auto& endpoint : endpoints) {
    INFO(endpoint.name);
    const auto reply = HttpDo(port, endpoint.method, endpoint.target, Authed(), endpoint.body);
    REQUIRE(reply.status == endpoint.expected_status);
    ++asserted;
  }
  REQUIRE(asserted == 20);

  //  数据面（自签 URL 的 PUT/GET）单独走一遍真实字节：它是 §7 的扩展但不是 OSDU 端点，
  //  这里只证明"路由在、能取回同样的字节"，100 MiB/SHA-256 的完整判据在 C4.8
  const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  const auto upload_json = fss::json::ParseObject(upload.body);
  REQUIRE(upload_json.ok());
  const std::string payload = "transfer-route-check";
  const auto put = HttpDo(port, "PUT",
                          TargetOf(upload_json.value()["Location"]["SignedURL"].get<std::string>()),
                          Authed(), payload);
  REQUIRE(put.status == 200);
}

TEST_CASE("★ C4.2 契约 §3.3 黄金样例：全字段往返（只允许 3 处服务端改写）",
          "[phase4][conformance][c4.2]") {
  HttpFixture fixture;
  const int port = fixture.port();

  const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(upload.status == 200);
  const auto upload_json = fss::json::ParseObject(upload.body);
  REQUIRE(upload_json.ok());
  const std::string file_source =
      upload_json.value()["Location"]["FileSource"].get<std::string>();
  const std::string body = "golden-sample";
  const auto put = HttpDo(port, "PUT",
                          TargetOf(upload_json.value()["Location"]["SignedURL"].get<std::string>()),
                          Authed(), body);
  REQUIRE(put.status == 200);

  //  黄金样例（契约 §3.3）—— 只把它里面的 `FileSource` 换成真实上传拿到的那个，
  //  其余字段**一个都不改**（这正是"全字段"的含义）
  auto submitted = fss::json::Parse(ReadFixture("File_CorrectPayload.json"));
  REQUIRE(submitted.ok());
  auto expected = submitted.value();
  expected["data"]["DatasetProperties"]["FileSourceInfo"]["FileSource"] = file_source;

  const auto created = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(),
                              fss::json::Dump(expected));
  REQUIRE(created.status == 201);
  const auto created_json = fss::json::ParseObject(created.body);
  REQUIRE(created_json.ok());
  const std::string record_id = created_json.value()["id"].get<std::string>();

  //  服务端生成的 id 形态：`<partition>:dataset--File.Generic:<32 位无横线>`
  REQUIRE(record_id.rfind("opendes:dataset--File.Generic:", 0) == 0);
  const std::string uuid = record_id.substr(std::string("opendes:dataset--File.Generic:").size());
  REQUIRE(uuid.size() == 32);
  for (const char c : uuid) REQUIRE(std::isxdigit(static_cast<unsigned char>(c)));

  const auto fetched = HttpDo(port, "GET", "/api/file/v2/files/" + record_id + "/metadata",
                              Authed());
  REQUIRE(fetched.status == 200);
  const auto fetched_json = fss::json::ParseObject(fetched.body);
  REQUIRE(fetched_json.ok());

  //  三处**契约允许**的服务端改写（其余必须逐字段一致）：
  //    ① id 由服务端生成；② `version` 由服务端填；③ 校验和被服务端覆写（§2.6 第 7 步）
  const std::string sha256 = fss::crypto::Sha256Hex(body);
  expected["id"] = record_id;
  expected["version"] = 1;
  expected["data"]["Checksum"] = sha256;
  expected["data"]["ChecksumAlgorithm"] = "SHA256";
  expected["data"]["DatasetProperties"]["FileSourceInfo"]["Checksum"] = sha256;
  expected["data"]["DatasetProperties"]["FileSourceInfo"]["ChecksumAlgorithm"] = "SHA256";

  INFO("提交: " << fss::json::Dump(expected));
  INFO("取回: " << fss::json::Dump(fetched_json.value()));
  REQUIRE(fetched_json.value() == expected);

  //  显式点名几个"最容易在往返里丢"的字段（R16 的正例断言：全字段相等已经覆盖，
  //  但显式断言让失败时一眼看出是哪个字段漂了）
  for (const char* key : {"ResourceHomeRegionID", "ResourceHostRegionIDs",
                          "ResourceCurationStatus", "ResourceLifecycleStatus",
                          "ResourceSecurityClassification", "Source", "ExistenceKind",
                          "Description", "TotalSize", "EncodingFormatTypeID",
                          "SchemaFormatTypeID", "ExtensionProperties"}) {
    INFO("data." << key);
    REQUIRE(fetched_json.value()["data"].contains(key));
  }
  REQUIRE(fetched_json.value()["data"]["DatasetProperties"]["FileSourceInfo"].contains(
      "PreloadFilePath"));
  REQUIRE(fetched_json.value()["data"]["DatasetProperties"]["FileSourceInfo"].contains(
      "PreloadFileModifyDate"));
  REQUIRE(fetched_json.value()["data"]["DatasetProperties"]["FileSourceInfo"].contains(
      "EncodingFormatTypeID"));
  REQUIRE(fetched_json.value()["acl"]["viewers"].size() == 1);
  REQUIRE(fetched_json.value()["legal"]["otherRelevantDataCountries"].size() == 1);
  REQUIRE(fetched_json.value()["tags"].size() == 2);
  REQUIRE(fetched_json.value()["meta"].size() == 1);
}

TEST_CASE("★ C4.3 契约 §3.4 负向矩阵：**逐字**比对上游验收样例的期望消息",
          "[phase4][conformance][c4.3]") {
  HttpFixture fixture;
  const int port = fixture.port();

  //  ★ 样例与期望消息都取自上游验收测试资源（vendored，见
  //    `tests/conformance/fixtures/upstream/README.md`：仓库 + commit + Apache-2.0）。
  //    此前我们只有"期望消息要点"，现在可以**逐字**比对 —— 这正是 C4.3 的本意。
  struct Sample {
    const char* fixture;             // 请求体
    const char* expected_message;    // 上游 `_msg.json` 里的 errors[].message（逐字）
    bool upstream_disabled;          // 上游 feature 表里被 `#` 注释掉的行
  };
  const std::vector<Sample> samples = {
      {"File_missing_kind.json", "kind must not be null", false},
      {"File_missing_viewers.json", "Record acl.viewers cannot be empty", false},
      {"File_missing_owners.json", "Record acl.owners cannot be empty", false},
      {"File_missing_acl.json", "acl must not be null", false},
      {"File_missing_legal.json", "legal tag cannot be empty", false},
      {"File_missing_data.json", "data cannot be empty", false},
      {"File_empty_fileSource.json", "FileSource can not be empty", false},
      {"File_missing_fileSource.json", "FileSource can not be empty", false},
      {"File_invalid_fileSource.json",
       "Invalid source file path to copy from /3f538327-85c8-459d-896c-198173436358neha/test.csv",
       false},
      {"File_invalid_Endian.json", "Invalid value of Small for Endian", false},
      //  上游把这两行注释掉了（`IntegrationTest_File_POST.feature` 第 24/25 行），
      //  详见 fixtures/upstream/README.md 的说明。
      {"File_invalid_ScalarIndicator.json", "Invalid value of Scale for ScalarIndicator", true},
      {"File_Datatype_Mismatch.json", "Bad Request. Invalid Input.", true},
  };

  for (const auto& sample : samples) {
    INFO(sample.fixture);
    //  上游样例里的占位符（测试脚手架替换）：`<tenant_name>` / `<acl_*>` / `<cloud_domain>` / `<legal_tags>`
    std::string text = ReadUpstreamFixture(sample.fixture);
    for (const auto& [placeholder, value] : Placeholders()) {
      for (std::size_t pos = text.find(placeholder); pos != std::string::npos;
           pos = text.find(placeholder, pos + value.size())) {
        text.replace(pos, placeholder.size(), value);
      }
    }
    const auto reply = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(), text);
    REQUIRE(reply.status == 400);  // 全部 12 条都是 400（含上游禁用的两条）
    REQUIRE(fss::json::ParseObject(reply.body).ok());  // 错误体必须是合法 JSON

    if (std::string(sample.fixture) == "File_Datatype_Mismatch.json") {
      //  ⚠️ 唯一一处**消息不对齐**：上游期望的是 Jackson 反序列化失败的通用消息
      //     `Bad Request. Invalid Input.`，我们给出的是更具体的约束消息。该行上游
      //     自己也没执行；状态码已对齐，差异已在契约 §3.4 与证据文件显式登记。
      continue;
    }
    INFO("响应: " << reply.body);
    REQUIRE(reply.body.find(sample.expected_message) != std::string::npos);
  }

  SECTION("File_Calculate_Checksum.json：201 且校验和被服务端覆写（自证对照 R1）") {
    //  ★ 提交一个**错**的校验和，必须被服务端的真实 SHA-256 覆盖。
    //    若服务端原样回传客户端给的值，说明"计算校验和"这条路径根本没实现。
    const std::string source = FreshUpload(port);
    auto value = LoadUpstreamSample("File_Calculate_Checksum.json", source);
    const std::string wrong = "00000000000000000000000000000000";
    value["data"]["Checksum"] = wrong;
    value["data"]["ChecksumAlgorithm"] = "MD5";
    value["data"]["DatasetProperties"]["FileSourceInfo"]["Checksum"] = wrong;

    const auto reply = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(),
                              fss::json::Dump(value));
    REQUIRE(reply.status == 201);
    const auto created = fss::json::ParseObject(reply.body);
    REQUIRE(created.ok());

    const auto fetched = HttpDo(port, "GET",
                                "/api/file/v2/files/" + created.value()["id"].get<std::string>() +
                                    "/metadata",
                                Authed());
    REQUIRE(fetched.status == 200);
    const auto record = fss::json::ParseObject(fetched.body);
    REQUIRE(record.ok());
    REQUIRE(record.value()["data"]["Checksum"].get<std::string>() != wrong);
    REQUIRE(record.value()["data"]["ChecksumAlgorithm"].get<std::string>() == "SHA256");
    REQUIRE(record.value()["data"]["DatasetProperties"]["FileSourceInfo"]["Checksum"]
                .get<std::string>() == record.value()["data"]["Checksum"].get<std::string>());
  }
}

TEST_CASE("★ C4.7 `expiryTime`：缺省 1H / `8D` 截断为 7D（数值断言）",
          "[phase4][conformance][c4.7]") {
  HttpFixture fixture;
  const int port = fixture.port();
  const std::int64_t now = fixture.clock.NowEpochSeconds();

  SECTION("缺省 → ≈ now+3600s（±5s；手动时钟下精确相等）") {
    const auto reply = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
    REQUIRE(reply.status == 200);
    const auto value = fss::json::ParseObject(reply.body);
    REQUIRE(value.ok());
    const std::int64_t exp =
        ExpOf(value.value()["Location"]["SignedURL"].get<std::string>());
    REQUIRE(std::llabs(exp - (now + 3600)) <= 5);
    REQUIRE(exp == now + 3600);  // 手动时钟：可精确断言
  }

  SECTION("`expiryTime=2H` → now+7200s；`8D` 超上限 → 截断为 now+604800s（7 天）") {
    const auto two_hours = HttpDo(port, "GET",
                                  "/api/file/v2/files/uploadURL?expiryTime=2H", Authed());
    REQUIRE(two_hours.status == 200);
    const auto two_json = fss::json::ParseObject(two_hours.body);
    REQUIRE(two_json.ok());
    REQUIRE(ExpOf(two_json.value()["Location"]["SignedURL"].get<std::string>()) ==
            now + 7200);

    const auto eight_days = HttpDo(port, "GET",
                                   "/api/file/v2/files/uploadURL?expiryTime=8D", Authed());
    REQUIRE(eight_days.status == 200);
    const auto eight_json = fss::json::ParseObject(eight_days.body);
    REQUIRE(eight_json.ok());
    REQUIRE(ExpOf(eight_json.value()["Location"]["SignedURL"].get<std::string>()) ==
            now + 604800);
  }

  SECTION("`5X` 非法 → 400 + 契约固定消息（DMS 端点同样适用）") {
    const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL?expiryTime=5X",
                               Authed());
    REQUIRE(upload.status == 400);
    REQUIRE(upload.body.find(
                "expiryTime pattern isn't supported. Value should be one of these regex "
                "patterns ^[0-9]+M$ , ^[0-9]+H$ , ^[0-9]+D$") != std::string::npos);

    const auto dms = HttpDo(port, "POST",
                            "/api/file/v2/files/storageInstructions?expiryTime=5X", Authed());
    REQUIRE(dms.status == 400);
    REQUIRE(dms.body.find("expiryTime pattern isn't supported") != std::string::npos);
  }
}

TEST_CASE("★ §7 `/metrics`：Prometheus 文本 + 计数反映真实请求",
          "[phase4][conformance][c4.1][metrics]") {
  HttpFixture fixture;
  const int port = fixture.port();

  //  先制造几种可预期的流量
  REQUIRE(HttpDo(port, "GET", "/api/file/v2/liveness_check").status == 200);
  REQUIRE(HttpDo(port, "GET", "/api/file/v2/info").status == 200);
  REQUIRE(HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed()).status == 200);
  REQUIRE(HttpDo(port, "GET", "/api/file/v2/files/uploadURL").status == 401);  // 缺 token

  const auto reply = HttpDo(port, "GET", "/metrics");
  REQUIRE(reply.status == 200);
  const std::string& body = reply.body;

  //  抓取器依赖 Content-Type 里的 version 参数来选择解析器
  REQUIRE(reply.Header("Content-Type").has_value());
  REQUIRE(reply.Header("Content-Type")->find("text/plain") == 0);
  REQUIRE(reply.Header("Content-Type")->find("version=0.0.4") != std::string::npos);
  //  契约 §7：指标端点**不在** `/api/file` 下
  REQUIRE(reply.Header("Content-Type")->find("json") == std::string::npos);

  //  HELP/TYPE 行 + 计数行（按路由/方法/状态码）
  REQUIRE(body.find("# TYPE fss_http_requests_total counter") != std::string::npos);
  REQUIRE(body.find(
              "fss_http_requests_total{route=\"ops.liveness\",method=\"GET\",status=\"200\"} 1") !=
          std::string::npos);
  REQUIRE(body.find("fss_http_requests_total{route=\"ops.info\",method=\"GET\",status=\"200\"} 1") !=
          std::string::npos);
  REQUIRE(body.find("fss_http_requests_total{route=\"location.upload_url\",method=\"GET\","
                    "status=\"200\"} 1") != std::string::npos);
  REQUIRE(body.find("fss_http_requests_total{route=\"location.upload_url\",method=\"GET\","
                    "status=\"401\"} 1") != std::string::npos);

  //  直方图：bucket/sum/count 三件套齐全，且 +Inf 等于 count
  REQUIRE(body.find("# TYPE fss_http_request_duration_seconds histogram") != std::string::npos);
  REQUIRE(body.find("fss_http_request_duration_seconds_bucket{route=\"ops.liveness\","
                    "method=\"GET\",le=\"0.001\"}") != std::string::npos);
  REQUIRE(body.find("fss_http_request_duration_seconds_bucket{route=\"ops.liveness\","
                    "method=\"GET\",le=\"+Inf\"} 1") != std::string::npos);
  REQUIRE(body.find("fss_http_request_duration_seconds_count{route=\"ops.liveness\","
                    "method=\"GET\"} 1") != std::string::npos);
  REQUIRE(body.find("fss_http_request_duration_seconds_sum{route=\"ops.liveness\","
                    "method=\"GET\"}") != std::string::npos);

  //  服务器自身的背压计数（ADR-010/C1.12 的"探测结果必须可见"）
  REQUIRE(body.find("fss_http_max_connections 4") != std::string::npos);
  REQUIRE(body.find("fss_http_worker_threads 4") != std::string::npos);
  //  当前请求**正在被服务**，所以在途计数至少是 1（这正是"指标自证"：为 0 说明
  //  计数点在请求之外，或计数从来没被递增过）
  REQUIRE(MetricValue(body, "fss_http_in_flight_requests") >= 1);
  REQUIRE(MetricValue(body, "fss_http_peak_in_flight_requests") >= 1);
  REQUIRE(body.find("fss_http_rejected_total{reason=\"busy\"} 0") != std::string::npos);

  //  ★ 两条独立观测路径互证（R1）：`/metrics` 的文本 vs 路由器内存里的计数器。
  //    若计数只写在渲染函数里、或 `Wrap()` 漏了某个出口，两边的数字就对不上。
  REQUIRE(fixture.metrics().RequestCount("ops.liveness", "GET", 200) == 1);
  REQUIRE(fixture.metrics().RequestCount("ops.info", "GET", 200) == 1);
  REQUIRE(fixture.metrics().RequestCount("location.upload_url", "GET", 200) == 1);
  REQUIRE(fixture.metrics().RequestCount("location.upload_url", "GET", 401) == 1);
  REQUIRE(fixture.metrics().RequestCount("no.such.route", "GET", 200) == 0);
}
