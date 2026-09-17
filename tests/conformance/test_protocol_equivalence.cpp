// =============================================================================
//  test_protocol_equivalence.cpp —— C7.3（契约 §6 的 12 行矩阵）+ C7.4（签名 URL 结构等价）
// =============================================================================
//  判据（契约 §6）：对每个操作，用**同一份输入**分别经 REST 与 gRPC 调用，断言
//    「领域结果 / 错误分类 / 存储副作用 / 位置记录 / 元数据记录」逐项相等。
//
//  ★ 两条协议挂在**同一份状态**上（`DualProtocolFixture`）：同一个 metadata/location/
//    blob 实例，因此"读同一资源"可以逐字段**字面比对**，写操作则比对副作用增量。
//  ★ 生成型字段（file_id / 签名 URL）两次调用必然不同 → 比对"结构/前缀/键集合/±5s"，
//    这正是契约 §6 对 SignedUrlEquivalent 的定义（C7.4，含反向测试）。
// =============================================================================
#include <catch2/catch.hpp>

#include "grpc_fixture.h"

#include "adapters/grpc/dto/grpc_dto.h"

#include "common/time/time_format.h"
#include "infra/transfer/transfer_token.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using fss::test::Authed;
using fss::test::DualProtocolFixture;
using fss::test::HttpDo;
using fss::test::TargetOf;
using osdu::file::v1::FileService;

// -----------------------------------------------------------------------------
//  C7.4：签名 URL 的结构等价（契约 §6 的判据）
// -----------------------------------------------------------------------------
struct ParsedUrl {
  std::string scheme;
  std::string host;
  std::string path;
  std::set<std::string> query_keys;
  std::map<std::string, std::string> query;
};

ParsedUrl ParseUrl(const std::string& url) {
  ParsedUrl out;
  const auto scheme_end = url.find("://");
  REQUIRE(scheme_end != std::string::npos);
  out.scheme = url.substr(0, scheme_end);
  const auto path_start = url.find('/', scheme_end + 3);
  REQUIRE(path_start != std::string::npos);
  out.host = url.substr(scheme_end + 3, path_start - scheme_end - 3);
  const auto query_start = url.find('?', path_start);
  out.path = url.substr(path_start, query_start - path_start);
  if (query_start == std::string::npos) return out;
  std::string query = url.substr(query_start + 1);
  std::size_t pos = 0;
  while (pos <= query.size() && !query.empty()) {
    const auto amp = query.find('&', pos);
    const std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos
                                                                        : amp - pos);
    const auto eq = pair.find('=');
    const std::string key = pair.substr(0, eq);
    out.query_keys.insert(key);
    out.query[key] = eq == std::string::npos ? "" : pair.substr(eq + 1);
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return out;
}

//  ★ 契约 §6 的等价判据：scheme/host 相同 ∧ **路径等价** ∧ query 参数名集合相同 ∧ 过期 ±5s
//
//  ⚠️ 一处必须说清的细节（本用例实现后才发现，已同步到契约 §6）：
//    自签数据面 URL 的 path **含 token**（`/v1/transfer/<base64url(payload)>`），而
//    payload 里有每次调用都变的 `nonce` —— 所以"path 字面相同"对**自签 URL** 不成立。
//    正确的判据是：
//      · 原生预签名（S3）：path = 对象路径 → 字面比较；
//      · 自签 URL：path 前缀相同 + **token 载荷的语义字段**相同（忽略 nonce/exp）。
//    这里两种都实现，并各配一条反向测试（改 query 名 / 改 token 里的 op 都必须判不等）。
bool SignedUrlEquivalent(const std::string& a, const std::string& b,
                         fss::infra::HmacTransferTokenCodec& codec,
                         std::string* why = nullptr) {
  const auto left = ParseUrl(a);
  const auto right = ParseUrl(b);
  const auto fail = [&](const std::string& reason) {
    if (why != nullptr) *why = reason;
    return false;
  };
  if (left.scheme != right.scheme) return fail("scheme 不同");
  if (left.host != right.host) return fail("host 不同");
  if (left.query_keys != right.query_keys) return fail("query 参数名集合不同");
  const auto exp_a = left.query.find("exp");
  const auto exp_b = right.query.find("exp");
  if (exp_a != left.query.end() && exp_b != right.query.end()) {
    if (std::llabs(std::stoll(exp_a->second) - std::stoll(exp_b->second)) > 5) {
      return fail("过期时间相差 > 5s");
    }
  }

  //  ---- 路径：先试"自签 token"形态，再退回字面比较 ----
  const std::string marker = "/v1/transfer/";
  const auto marker_pos = left.path.find(marker);
  const auto right_marker_pos = right.path.find(marker);
  if (marker_pos == std::string::npos || right_marker_pos == std::string::npos) {
    if (left.path != right.path) return fail("path 不同");
    return true;
  }
  if (left.path.substr(0, marker_pos + marker.size()) !=
      right.path.substr(0, right_marker_pos + marker.size())) {
    return fail("path 前缀不同");
  }
  const auto decode = [&](const ParsedUrl& url, std::size_t pos,
                          fss::domain::TransferToken* out) {
    const std::string token = url.path.substr(pos + marker.size());
    const auto exp = url.query.find("exp");
    const auto sig = url.query.find("sig");
    if (exp == url.query.end() || sig == url.query.end()) return false;
    const auto decoded = codec.Decode(token, exp->second, sig->second);
    if (!decoded.ok()) return false;
    *out = decoded.value();
    return true;
  };
  fss::domain::TransferToken left_token;
  fss::domain::TransferToken right_token;
  if (!decode(left, marker_pos, &left_token) || !decode(right, right_marker_pos, &right_token)) {
    return fail("token 无法解码/验签");
  }
  if (left_token.partition != right_token.partition) return fail("token.partition 不同");
  if (left_token.file_id != right_token.file_id) return fail("token.file_id 不同");
  if (left_token.container != right_token.container) return fail("token.container 不同");
  if (left_token.object_key != right_token.object_key) return fail("token.object_key 不同");
  if (left_token.zone != right_token.zone) return fail("token.zone 不同");
  if (left_token.op != right_token.op) return fail("token.op 不同");
  return true;
}

// -----------------------------------------------------------------------------
//  两条协议的辅助调用
// -----------------------------------------------------------------------------
std::string RestUpload(fss::test::HttpFixture& http, const std::string& payload,
                       std::string* file_id_out = nullptr) {
  const auto upload = HttpDo(http.port(), "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(upload.status == 200);
  const auto json = fss::json::ParseObject(upload.body);
  REQUIRE(json.ok());
  const auto file_id = json.value()["FileID"].get<std::string>();
  const auto file_source = json.value()["Location"]["FileSource"].get<std::string>();
  const auto put = HttpDo(http.port(), "PUT",
                          TargetOf(json.value()["Location"]["SignedURL"].get<std::string>()),
                          Authed(), payload);
  REQUIRE(put.status == 200);
  if (file_id_out != nullptr) *file_id_out = file_id;
  return file_source;
}

//  经 gRPC 走一遍"取上传地址 → 客户端上传 → 登记元数据"，返回 (记录 id, FileSource)
struct RpcRegistered {
  std::string id;
  std::string file_source;
};

RpcRegistered RpcUploadAndRegister(DualProtocolFixture& fx, const std::string& payload,
                                   const std::string& name) {
  auto context = fx.Context();  // ★ 两条链路都要带鉴权元数据（契约 §4.3）
  osdu::file::v1::GetUploadLocationRequest request;
  osdu::file::v1::LocationResponse response;
  REQUIRE(fx.stub->GetUploadLocation(context.get(), request, &response).ok());
  const std::string file_source = response.location().file_source();
  //  gRPC 侧拿到的是自签 URL（集中存储模式）→ 用 HTTP PUT 把字节送进数据面
  const auto put = HttpDo(fx.http_port(), "PUT", TargetOf(response.location().signed_url()),
                          Authed(), payload);
  REQUIRE(put.status == 200);

  auto context2 = fx.Context();
  auto record = fss::test::AppFixture::MakeRecord(file_source, name);
  osdu::file::v1::FileMetadataRecord proto;
  fss::adapters::grpc::FillMetadataProto(fss::domain::ParseFileMetadataRecord(
                                             fss::domain::ToJson(record))
                                             .value(),
                                         &proto);
  osdu::file::v1::CreateFileMetadataResponse created;
  const auto status = fx.stub->CreateFileMetadata(context2.get(), proto, &created);
  INFO("CreateFileMetadata → " << status.error_code() << " " << status.error_message());
  REQUIRE(status.ok());
  return RpcRegistered{created.id(), file_source};
}

std::string RestRegister(DualProtocolFixture& fx, const std::string& file_source,
                         const std::string& name) {
  auto record = fss::test::AppFixture::MakeRecord(file_source, name);
  const auto created = HttpDo(fx.http_port(), "POST", "/api/file/v2/files/metadata", Authed(),
                              fss::json::Dump(fss::domain::ToJson(record)));
  REQUIRE(created.status == 201);
  return fss::json::ParseObject(created.body).value()["id"].get<std::string>();
}

std::set<std::string> KeysOf(const fss::json::Value& object) {
  std::set<std::string> keys;
  for (auto it = object.begin(); it != object.end(); ++it) keys.insert(it.key());
  return keys;
}

}  // namespace

TEST_CASE("★ C7.4 签名 URL 等价判据本身：结构等价 + **反向测试**（改 query 名/改 token 声明必须判不等）",
          "[phase7][conformance][c7.4]") {
  fss::test::DualProtocolFixture fx;
  auto& codec = fx.http.codec;

  //  同一个"业务对象"的两次签名（nonce/时间戳不同）→ 必须等价
  const std::string self_signed_a =
      codec.Encode(fss::domain::TransferToken{"opendes", "file-1", "opendes-staging",
                                              "osdu-user/1-ts/file-1", fss::domain::StorageZone::kStaging,
                                              "get", 1700003600},
                   "http://127.0.0.1/api/file")
          .value();
  const std::string self_signed_b =
      codec.Encode(fss::domain::TransferToken{"opendes", "file-1", "opendes-staging",
                                              "osdu-user/1-ts/file-1", fss::domain::StorageZone::kStaging,
                                              "get", 1700003600},
                   "http://127.0.0.1/api/file")
          .value();
  REQUIRE(self_signed_a != self_signed_b);  // 字面不同（nonce）
  REQUIRE(SignedUrlEquivalent(self_signed_a, self_signed_b, codec));

  //  ★ 反向测试 ①：同一个对象但 op 不同（put vs get）→ 必须判不等
  const std::string self_signed_put =
      codec.Encode(fss::domain::TransferToken{"opendes", "file-1", "opendes-staging",
                                              "osdu-user/1-ts/file-1", fss::domain::StorageZone::kStaging,
                                              "put", 1700003600},
                   "http://127.0.0.1/api/file")
          .value();
  { std::string why; REQUIRE_FALSE(SignedUrlEquivalent(self_signed_a, self_signed_put, codec, &why));
    REQUIRE(why.find("token.op") != std::string::npos); }

  //  ★ 反向测试 ②：改一个 query 参数名 → 必须判不等
  {
    std::string tampered = self_signed_a;
    const auto exp_pos = tampered.find("exp=");
    REQUIRE(exp_pos != std::string::npos);
    tampered.replace(exp_pos, 3, "expires=");
    std::string why;
    REQUIRE_FALSE(SignedUrlEquivalent(self_signed_a, tampered, codec, &why));
    REQUIRE(why.find("query") != std::string::npos);
  }

  //  ★ 反向测试 ③：host / scheme 不同 → 不等
  REQUIRE_FALSE(SignedUrlEquivalent(self_signed_a,
                                    std::string("https://127.0.0.1") + ParseUrl(self_signed_a).path +
                                        "?exp=1700003600&sig=x",
                                    codec));
  //  ★ 反向测试 ④：token 里的 object_key 不同（换了对象）→ 不等
  const std::string other_object =
      codec.Encode(fss::domain::TransferToken{"opendes", "file-1", "opendes-staging",
                                              "osdu-user/1-ts/file-2", fss::domain::StorageZone::kStaging,
                                              "get", 1700003600},
                   "http://127.0.0.1/api/file")
          .value();
  REQUIRE_FALSE(SignedUrlEquivalent(self_signed_a, other_object, codec));
}

TEST_CASE("★ C7.3 契约 §6 矩阵：位置/列表/元数据/DMS/Delivery 逐行等价",
          "[phase7][conformance][c7.3]") {
  DualProtocolFixture fx;

  // ---- ① GetUploadLocation：形态 + 副作用（各新增 1 条位置记录）----
  const std::size_t locations_before = fx.http.locations.size("opendes");
  const auto rest_upload = HttpDo(fx.http_port(), "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(rest_upload.status == 200);
  const auto rest_upload_json = fss::json::ParseObject(rest_upload.body);
  REQUIRE(rest_upload_json.ok());
  const auto rest_file_id = rest_upload_json.value()["FileID"].get<std::string>();
  const auto rest_file_source = rest_upload_json.value()["Location"]["FileSource"].get<std::string>();

  auto rpc_context = fx.Context();
  osdu::file::v1::GetUploadLocationRequest rpc_upload_request;
  osdu::file::v1::LocationResponse rpc_upload;
  REQUIRE(fx.stub->GetUploadLocation(rpc_context.get(), rpc_upload_request, &rpc_upload).ok());
  //  领域结果：id 形态（32 位去横线 uuid）与 file_source 形态必须一致
  REQUIRE(rest_file_id.size() == 32);
  REQUIRE(rpc_upload.file_id().size() == 32);
  REQUIRE(rest_file_source.rfind("/", 0) == 0);
  REQUIRE(rpc_upload.location().file_source().rfind("/", 0) == 0);
  REQUIRE(rpc_upload.location().file_source().find(rpc_upload.file_id()) != std::string::npos);
  REQUIRE(rpc_upload.zone() == osdu::file::v1::STORAGE_ZONE_STAGING);
  //  副作用：两条链路各新增 1 条位置记录
  REQUIRE(fx.http.locations.size("opendes") == locations_before + 2);

  //  把字节送进 staging，供后续元数据登记使用（两条链路都用 HTTP 数据面）
  REQUIRE(HttpDo(fx.http_port(), "PUT", TargetOf(rest_upload_json.value()["Location"]["SignedURL"]
                                                     .get<std::string>()),
                 Authed(), "equivalence-payload").status == 200);

  // ---- ② GetFileLocation：同一资源、两条协议的领域结果必须逐字段相同 ----
  const auto rest_location = HttpDo(fx.http_port(), "POST", "/api/file/v2/getFileLocation",
                                    Authed(), std::string("{\"FileID\":\"") + rest_file_id + "\"}");
  REQUIRE(rest_location.status == 200);
  const auto rest_location_json = fss::json::ParseObject(rest_location.body);
  REQUIRE(rest_location_json.ok());
  auto rpc_location_context = fx.Context();
  osdu::file::v1::GetFileLocationRequest rpc_location_request;
  rpc_location_request.set_file_id(rest_file_id);
  osdu::file::v1::GetFileLocationResponse rpc_location;
  REQUIRE(fx.stub->GetFileLocation(rpc_location_context.get(), rpc_location_request, &rpc_location).ok());
  REQUIRE(rpc_location.driver() == rest_location_json.value()["Driver"].get<std::string>());
  REQUIRE(rpc_location.location() == rest_location_json.value()["Location"].get<std::string>());
  REQUIRE(rpc_location.file_source() == rest_file_source);

  // ---- ③ GetDownloadLocation：signed_url 结构等价（C7.4）----
  const auto rest_download = HttpDo(fx.http_port(), "GET",
                                    "/api/file/v2/files/" + rest_file_id + "/downloadURL", Authed());
  REQUIRE(rest_download.status == 200);
  const auto rest_download_json = fss::json::ParseObject(rest_download.body);
  REQUIRE(rest_download_json.ok());
  auto rpc_download_context = fx.Context();
  osdu::file::v1::GetDownloadLocationRequest rpc_download_request;
  rpc_download_request.set_id(rest_file_id);
  osdu::file::v1::DownloadUrlResponse rpc_download;
  REQUIRE(fx.stub->GetDownloadLocation(rpc_download_context.get(), rpc_download_request, &rpc_download).ok());
  std::string why;
  INFO("REST: " << rest_download_json.value()["SignedUrl"].get<std::string>()
                << " / RPC: " << rpc_download.signed_url());
  REQUIRE(SignedUrlEquivalent(rest_download_json.value()["SignedUrl"].get<std::string>(),
                              rpc_download.signed_url(), fx.http.codec, &why));
  REQUIRE(why.empty());

  // ---- ④ CreateFileMetadata：格式 + 副作用（两条链路各 1 条 v1 记录、各 1 份 persistent 对象）----
  const auto rpc_registered = RpcUploadAndRegister(fx, "rpc-payload", "rpc.bin");
  const std::string rpc_record_id = rpc_registered.id;
  const std::string rest_record_id = RestRegister(fx, rest_file_source, "rest.bin");
  const std::string id_prefix = "opendes:dataset--File.Generic:";
  const auto id_shape = [&](const std::string& id) {
    if (id.rfind(id_prefix, 0) != 0) return false;
    const std::string uuid = id.substr(id_prefix.size());
    if (uuid.size() != 32) return false;
    return std::all_of(uuid.begin(), uuid.end(), [](unsigned char c) {
      return std::isxdigit(c) != 0;  // 无横线的 uuid（契约 §2.6）
    });
  };
  REQUIRE(id_shape(rest_record_id));
  REQUIRE(id_shape(rpc_record_id));
  fss::domain::MetadataQuery query;
  REQUIRE(fx.http.metadata.List("opendes", query).value().total == 2);  // 恰好两条，无重复
  //  位置记录都迁到 persistent
  //  ★ 只检查**真正登记过元数据**的两条：各自的上传对象都应迁到 persistent
  const std::vector<std::string> registered_sources = {rest_file_source,
                                                       rpc_registered.file_source};
  for (const auto& source : registered_sources) {
    const auto location = fx.http.locations.FindByFileSource("opendes", source);
    INFO("source=" << source << " zone="
                   << (location.ok() && location.value().zone == fss::domain::StorageZone::kPersistent
                           ? "persistent"
                           : "STAGING/缺失"));
    REQUIRE(location.ok());
    REQUIRE(location.value().zone == fss::domain::StorageZone::kPersistent);
  }

  // ---- ⑤ GetFileMetadata：**交叉读**（REST 建的记录用 RPC 读，反之亦然）----
  {
    auto context = fx.Context();
    osdu::file::v1::GetFileMetadataRequest request;
    request.set_id(rest_record_id);
    osdu::file::v1::FileMetadataRecord response;
    REQUIRE(fx.stub->GetFileMetadata(context.get(), request, &response).ok());
    REQUIRE(response.id() == rest_record_id);
    REQUIRE(response.version() == 1);
    REQUIRE(response.kind() == "opendes:wks:dataset--File.Generic:1.0.0");
    REQUIRE(response.data().name() == "rest.bin");
    REQUIRE(response.data().dataset_properties().file_source_info().file_source() ==
            rest_file_source);
  }
  {
    const auto fetched = HttpDo(fx.http_port(), "GET",
                                "/api/file/v2/files/" + rpc_record_id + "/metadata", Authed());
    REQUIRE(fetched.status == 200);
    const auto json = fss::json::ParseObject(fetched.body);
    REQUIRE(json.ok());
    REQUIRE(json.value()["id"].get<std::string>() == rpc_record_id);
    REQUIRE(json.value()["version"].get<int>() == 1);
    REQUIRE(json.value()["data"]["Name"].get<std::string>() == "rpc.bin");
  }

  // ---- ⑥ GetFileList：分页字段与内容集合相同 ----
  {
    const auto rest_list = HttpDo(fx.http_port(), "POST", "/api/file/v2/getFileList", Authed(),
                                  R"({"PageNum":0,"Items":10})");
    REQUIRE(rest_list.status == 200);
    const auto rest_json = fss::json::ParseObject(rest_list.body);
    REQUIRE(rest_json.ok());
    auto context = fx.Context();
    osdu::file::v1::GetFileListRequest request;
    request.mutable_request()->set_page_num(0);
    request.mutable_request()->set_items(10);
    osdu::file::v1::FileListResponse response;
    REQUIRE(fx.stub->GetFileList(context.get(), request, &response).ok());
    REQUIRE(response.number() == rest_json.value()["Number"].get<int>());
    REQUIRE(response.size() == rest_json.value()["Size"].get<int>());
    REQUIRE(response.number_of_elements() ==
            rest_json.value()["NumberOfElements"].get<int>());
    REQUIRE(response.content_size() == static_cast<int>(rest_json.value()["Content"].size()));
    for (int i = 0; i < response.content_size(); ++i) {
      REQUIRE(response.content(i).file_id() ==
              rest_json.value()["Content"][i]["FileID"].get<std::string>());
    }
  }

  // ---- ⑦ GetStorageInstructions：键集合与 REST 相同（共用 L4 的 JSON 形状）----
  {
    const auto rest_instructions = HttpDo(fx.http_port(), "POST",
                                          "/api/file/v2/files/storageInstructions", Authed());
    REQUIRE(rest_instructions.status == 200);
    const auto rest_json = fss::json::ParseObject(rest_instructions.body);
    REQUIRE(rest_json.ok());
    auto context = fx.Context();
    osdu::file::v1::GetStorageInstructionsRequest request;
    osdu::file::v1::StorageInstructionsResponse response;
    REQUIRE(fx.stub->GetStorageInstructions(context.get(), request, &response).ok());
    REQUIRE(response.provider_key() == rest_json.value()["providerKey"].get<std::string>());
    //  把 Struct 转回 JSON 比对键集合（两侧必须逐键一致）
    const auto struct_json =
        fss::json::ParseObject(fss::json::Dump(fss::json::Value::parse("{}")));
    REQUIRE(struct_json.ok());
    std::string text;
    REQUIRE(google::protobuf::util::MessageToJsonString(response.storage_location(), &text).ok());
    const auto rpc_location_json = fss::json::ParseObject(text);
    REQUIRE(rpc_location_json.ok());
    REQUIRE(KeysOf(rpc_location_json.value()) == KeysOf(rest_json.value()["storageLocation"]));
    //  ★ 两次调用会生成**各自**的上传指令（file_id/fileSource 必然不同）——契约 §6 这一行
    //    比的是"键集合"，不是值。这里再钉住形态：前导斜杠 + 末段是 32 位 hex（file id）。
    const auto source_shape_ok = [](const std::string& source) {
      if (source.rfind("/", 0) != 0) return false;
      const std::string tail = source.substr(source.rfind('/') + 1);
      return tail.size() == 32 &&
             std::all_of(tail.begin(), tail.end(),
                         [](unsigned char c) { return std::isxdigit(c) != 0; });
    };
    REQUIRE(source_shape_ok(rpc_location_json.value()["fileSource"].get<std::string>()));
    REQUIRE(source_shape_ok(rest_json.value()["storageLocation"]["fileSource"].get<std::string>()));
  }

  // ---- ⑧ GetRetrievalInstructions：同一 datassetRegistryId 的集合与 providerKey ----
  {
    const auto rest_retrieval = HttpDo(
        fx.http_port(), "POST", "/api/file/v2/files/retrievalInstructions", Authed(),
        std::string("{\"datasetRegistryIds\":[\"") + rest_record_id + "\"]}");
    REQUIRE(rest_retrieval.status == 200);
    const auto rest_json = fss::json::ParseObject(rest_retrieval.body);
    REQUIRE(rest_json.ok());
    auto context = fx.Context();
    osdu::file::v1::GetRetrievalInstructionsRequest request;
    request.add_dataset_registry_ids(rest_record_id);
    osdu::file::v1::RetrievalInstructionsResponse response;
    REQUIRE(fx.stub->GetRetrievalInstructions(context.get(), request, &response).ok());
    REQUIRE(response.datasets_size() == rest_json.value()["datasets"].size());
    REQUIRE(response.datasets(0).dataset_registry_id() ==
            rest_json.value()["datasets"][0]["datasetRegistryId"].get<std::string>());
    REQUIRE(response.datasets(0).provider_key() ==
            rest_json.value()["datasets"][0]["providerKey"].get<std::string>());
    std::string text;
    REQUIRE(google::protobuf::util::MessageToJsonString(
                response.datasets(0).retrieval_properties(), &text)
                .ok());
    const auto rpc_props = fss::json::ParseObject(text);
    REQUIRE(rpc_props.ok());
    REQUIRE(KeysOf(rpc_props.value()) ==
            KeysOf(rest_json.value()["datasets"][0]["retrievalProperties"]));
  }

  // ---- ⑨ CopyFilesToPersistent：同一源、两条链路结果相同 ----
  {
    const auto rest_copy = HttpDo(fx.http_port(), "POST", "/api/file/v2/files/copy", Authed(),
                                  std::string("{\"datasetSources\":[\"") + rest_file_source + "\"]}");
    REQUIRE(rest_copy.status == 200);
    const auto rest_json = fss::json::Parse(rest_copy.body);
    REQUIRE(rest_json.ok());
    auto context = fx.Context();
    osdu::file::v1::CopyDmsRequest request;
    //  ★ 上游形态：datasetSources 是**记录对象**（不是路径字符串）——
    //    两条协议必须用同一套取值规则解析它
    auto* node = request.add_dataset_sources();
    const auto parsed = google::protobuf::util::JsonStringToMessage(
        std::string("{\"data\":{\"DatasetProperties\":{\"FileSourceInfo\":{\"FileSource\":\"") +
            rest_file_source + "\"}}}}",
        node);
    REQUIRE(parsed.ok());
    osdu::file::v1::CopyDmsResponseList response;
    REQUIRE(fx.stub->CopyFilesToPersistent(context.get(), request, &response).ok());
    REQUIRE(response.results_size() == 1);
    REQUIRE(response.results(0).success() == rest_json.value()[0]["success"].get<bool>());
    REQUIRE(response.results(0).dataset_blob_storage_path() ==
            rest_json.value()[0]["datasetBlobStoragePath"].get<std::string>());
  }

  // ---- ⑩ GetFileSignedUrl：processed/unprocessed 与 URL 结构等价 ----
  {
    const std::string srn = "srn:file/" + rest_file_id;
    const std::string unknown = "srn:file/ffffffffffffffffffffffffffffffff";
    const auto rest_delivery = HttpDo(fx.http_port(), "POST",
                                      "/api/file/v2/delivery/GetFileSignedUrl", Authed(),
                                      std::string("{\"srns\":[\"") + srn + "\",\"" + unknown + "\"]}");
    REQUIRE(rest_delivery.status == 200);
    const auto rest_json = fss::json::ParseObject(rest_delivery.body);
    REQUIRE(rest_json.ok());
    auto context = fx.Context();
    osdu::file::v1::UrlSigningRequest request;
    request.add_srns(srn);
    request.add_srns(unknown);
    osdu::file::v1::UrlSigningResponse response;
    REQUIRE(fx.stub->GetFileSignedUrl(context.get(), request, &response).ok());
    REQUIRE(response.unprocessed_size() == static_cast<int>(rest_json.value()["unprocessed"].size()));
    REQUIRE(response.unprocessed(0) == rest_json.value()["unprocessed"][0].get<std::string>());
    REQUIRE(response.processed().count(srn) == 1);
    const auto& rpc_entry = response.processed().at(srn);
    const auto& rest_entry = rest_json.value()["processed"][srn];
    REQUIRE(rpc_entry.kind() == rest_entry["kind"].get<std::string>());
    REQUIRE(rpc_entry.unsigned_url() == rest_entry["unsignedUrl"].get<std::string>());
    //  ★ 表示差异（如实断言）：REST 输出 `connectionString: null`，proto3 的 string
    //    空值不会出现在 JSON 里 → 两侧都表示"没有 connection string"
    REQUIRE(rest_entry["connectionString"].is_null());
    REQUIRE(rpc_entry.connection_string().empty());
    REQUIRE(SignedUrlEquivalent(rest_entry["signedUrl"].get<std::string>(),
                                rpc_entry.signed_url(), fx.http.codec));
  }

  // ---- ⑪ RevokeUrl：两条链路都成功且**无副作用** ----
  {
    const std::size_t before = fx.http.locations.size("opendes");
    const auto rest_revoke = HttpDo(fx.http_port(), "POST", "/api/file/v2/files/revokeURL",
                                    Authed(), "{}");
    REQUIRE(rest_revoke.status == 204);
    auto context = fx.Context();
    osdu::file::v1::RevokeUrlRequest request;
    ::google::protobuf::Empty response;
    REQUIRE(fx.stub->RevokeUrl(context.get(), request, &response).ok());
    REQUIRE(fx.http.locations.size("opendes") == before);
  }

  // ---- ⑫ DeleteFileMetadata：两条链路各删一条，副作用相同 ----
  {
    const auto rest_delete = HttpDo(fx.http_port(), "DELETE",
                                    "/api/file/v2/files/" + rest_record_id + "/metadata", Authed());
    REQUIRE(rest_delete.status == 204);
    auto context = fx.Context();
    osdu::file::v1::DeleteFileMetadataRequest request;
    request.set_id(rpc_record_id);
    ::google::protobuf::Empty response;
    REQUIRE(fx.stub->DeleteFileMetadata(context.get(), request, &response).ok());
    //  两条链路删除后 read 都必须 NOT_FOUND / 404
    const auto rest_after = HttpDo(fx.http_port(), "GET",
                                   "/api/file/v2/files/" + rest_record_id + "/metadata", Authed());
    REQUIRE(rest_after.status == 404);
    auto context2 = fx.Context();
    osdu::file::v1::GetFileMetadataRequest missing;
    missing.set_id(rpc_record_id);
    osdu::file::v1::FileMetadataRecord missing_response;
    REQUIRE(fx.stub->GetFileMetadata(context2.get(), missing, &missing_response).error_code() ==
            ::grpc::StatusCode::NOT_FOUND);
    REQUIRE(fx.http.metadata.List("opendes", fss::domain::MetadataQuery{}).value().total == 0);
  }
}

TEST_CASE("★ C7.3 错误分类等价：同一份非法输入在两条链路上落到同一行（契约 §5）",
          "[phase7][conformance][c7.3]") {
  DualProtocolFixture fx;
  const std::string missing_id = "opendes:dataset--File.Generic:ffffffffffffffffffffffffffffffff";

  //  未知记录：REST 404 ↔ gRPC NOT_FOUND
  {
    const auto rest = HttpDo(fx.http_port(), "GET",
                             "/api/file/v2/files/" + missing_id + "/metadata", Authed());
    REQUIRE(rest.status == 404);
    auto context = fx.Context();
    osdu::file::v1::GetFileMetadataRequest request;
    request.set_id(missing_id);
    osdu::file::v1::FileMetadataRecord response;
    REQUIRE(fx.stub->GetFileMetadata(context.get(), request, &response).error_code() ==
            ::grpc::StatusCode::NOT_FOUND);
  }
  //  缺 token：REST 401 ↔ gRPC UNAUTHENTICATED
  {
    const auto rest = HttpDo(fx.http_port(), "GET",
                             "/api/file/v2/files/" + missing_id + "/metadata",
                             {"data-partition-id: opendes"});
    REQUIRE(rest.status == 401);
    auto context = fx.Context(/*token=*/"", /*partition=*/"opendes");
    osdu::file::v1::GetFileMetadataRequest request;
    request.set_id(missing_id);
    osdu::file::v1::FileMetadataRecord response;
    REQUIRE(fx.stub->GetFileMetadata(context.get(), request, &response).error_code() ==
            ::grpc::StatusCode::UNAUTHENTICATED);
  }
  //  缺 partition：REST 401 ↔ gRPC UNAUTHENTICATED
  {
    const auto rest = HttpDo(fx.http_port(), "GET",
                             "/api/file/v2/files/" + missing_id + "/metadata",
                             {"authorization: Bearer test-token"});
    REQUIRE(rest.status == 401);
    auto context = fx.Context(/*token=*/"Bearer test-token", /*partition=*/"");
    osdu::file::v1::GetFileMetadataRequest request;
    request.set_id(missing_id);
    osdu::file::v1::FileMetadataRecord response;
    REQUIRE(fx.stub->GetFileMetadata(context.get(), request, &response).error_code() ==
            ::grpc::StatusCode::UNAUTHENTICATED);
  }
  //  非法 kind：REST 400 ↔ gRPC INVALID_ARGUMENT
  {
    auto record = fss::test::AppFixture::MakeRecord("/osdu-user/x/y", "bad.bin");
    record.kind = "opendes:wks:dataset--File.Wrong:1.0.0";
    const auto rest = HttpDo(fx.http_port(), "POST", "/api/file/v2/files/metadata", Authed(),
                             fss::json::Dump(fss::domain::ToJson(record)));
    REQUIRE(rest.status == 400);
    auto context = fx.Context();
    osdu::file::v1::FileMetadataRecord proto;
    fss::adapters::grpc::FillMetadataProto(record, &proto);
    osdu::file::v1::CreateFileMetadataResponse response;
    REQUIRE(fx.stub->CreateFileMetadata(context.get(), proto, &response).error_code() ==
            ::grpc::StatusCode::INVALID_ARGUMENT);
  }
  //  列表：无记录 → REST 400 ↔ gRPC INVALID_ARGUMENT（同一条"无记录"规则）
  {
    const auto rest = HttpDo(fx.http_port(), "POST", "/api/file/v2/getFileList", Authed(),
                             R"({"PageNum":0,"Items":10})");
    REQUIRE(rest.status == 400);
    auto context = fx.Context();
    osdu::file::v1::GetFileListRequest request;
    request.mutable_request()->set_page_num(0);
    request.mutable_request()->set_items(10);
    osdu::file::v1::FileListResponse response;
    REQUIRE(fx.stub->GetFileList(context.get(), request, &response).error_code() ==
            ::grpc::StatusCode::INVALID_ARGUMENT);
  }
}
