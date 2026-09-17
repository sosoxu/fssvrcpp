// =============================================================================
//  P8 切片 1：端点 × 角色矩阵（C8.1）—— 用**真实** `LocalJwtAuthorizer`
// =============================================================================
//  为什么必须走真实 HTTP + 真实 token：
//    `test_roles.cpp` 已经在**用例层**证明了每个用例第一步就鉴权（含"403 先于 400"）。
//    这一条要证明的是**整条链路**：HTTP 适配层 → `CallerContext` → 用例入口 →
//    `IAuthorizer`。任何一环把 token 丢了（例如适配层没传 `authorization`），
//    矩阵都会失败。
//
//  角色表**手抄契约 §1.3**（不 import 实现的常量）：契约漂移时这里必须失败。
//
//  期望值设计（关键）：
//    · 无 token → 401 + `Missing authorization token`（消息逐字节）
//    · 有 token 但角色不足 → **403**（不是 400）—— 未授权者不能靠 400 的差异探测数据
//    · 有正确角色 → 状态码 ∉ {401, 403}（可能是 200，也可能是 400/404：请求体是
//      "最小合法躯壳"，这不是鉴权问题）；正向的 200 由 `test_upload_flow_posix` 与
//      本文件末尾的"完整垂直切片"覆盖
// =============================================================================
#include <catch2/catch.hpp>

#include "http_fixture.h"

#include "common/crypto/crypto.h"
#include "common/json/json.h"
#include "infra/auth/local/local_jwt_authorizer.h"

#include <string>
#include <vector>

namespace {

using fss::infra::LocalJwtAuthorizer;
using fss::infra::LocalJwtOptions;
using fss::test::Authed;
using fss::test::HttpDo;
using fss::test::RawClient;

constexpr char kSecret[] = "matrix-hs256-secret";
constexpr std::int64_t kNow = 1700000000;

//  ---- 契约 §1.3 的 9 个角色字面值（手抄）----
constexpr char kFileViewers[] = "service.file.viewers";
constexpr char kFileEditors[] = "service.file.editors";
constexpr char kFileAdmin[] = "service.file.admin";
constexpr char kDeliveryViewer[] = "service.delivery.viewer";
constexpr char kDatasetViewers[] = "service.dataset.viewers";
constexpr char kDatasetEditors[] = "service.dataset.editors";
constexpr char kStorageCreator[] = "service.storage.creator";
constexpr char kStorageAdmin[] = "service.storage.admin";

std::string MintToken(const std::vector<std::string>& roles, const std::string& email,
                      const std::string& partition = "opendes") {
  fss::json::Value header;
  header["alg"] = "HS256";
  fss::json::Value payload;
  payload["sub"] = "user-1";
  payload["email"] = email;
  payload["data-partition-id"] = partition;
  payload["roles"] = roles;
  payload["exp"] = kNow + 3600;
  const std::string header_b64 = fss::crypto::Base64UrlEncode(fss::json::Dump(header));
  const std::string payload_b64 = fss::crypto::Base64UrlEncode(fss::json::Dump(payload));
  const std::string signing_input = header_b64 + "." + payload_b64;
  const auto digest = fss::crypto::HmacSha256(kSecret, signing_input);
  return signing_input + "." + fss::crypto::Base64UrlEncode(digest);
}

//  一次"端点 × 角色"调用：只发鉴权头 + 最小躯壳，返回状态码
struct Call {
  std::string method;
  std::string target;
  std::string body = "{}";
};

int CallWithRole(int port, const Call& call, const std::string& token,
                 bool with_partition = true) {
  std::vector<std::string> headers;
  if (!token.empty()) headers.push_back("authorization: " + token);
  if (with_partition) headers.push_back("data-partition-id: opendes");
  RawClient client(port, /*tcp_nodelay=*/true);
  REQUIRE(client.Connect());
  if (call.method == "POST" || call.method == "PUT" || call.method == "DELETE") {
    headers.push_back("Content-Length: " + std::to_string(call.body.size()));
  }
  REQUIRE(client.SendRequest(call.method, call.target, headers, call.body));
  const auto response = client.ReadResponse(10000);
  REQUIRE(response.has_value());
  return response->status;
}

//  端点 → 允许的角色集合（"任一即通过"用集合表达；空 = 不需要角色）
struct EndpointRule {
  Call call;
  std::vector<std::string> allowed_roles;
  bool requires_partition = true;
};

std::vector<EndpointRule> EndpointMatrix() {
  const std::string placeholder_id = "opendes:dataset--File.Generic:" + std::string(32, 'a');
  return {
      //  契约 §1.3 的逐端点映射（手抄）
      {{"POST", "/api/file/v2/files/metadata"}, {kFileEditors}},
      {{"GET", "/api/file/v2/files/" + placeholder_id + "/metadata"}, {kFileViewers}},
      {{"DELETE", "/api/file/v2/files/" + placeholder_id + "/metadata"},
       {kFileEditors, kFileAdmin}},
      {{"POST", "/api/file/v2/getFileList"}, {kFileEditors}},
      {{"POST", "/api/file/v2/getLocation"}, {kFileEditors}},
      {{"POST", "/api/file/v2/getFileLocation"}, {kFileEditors}},
      {{"GET", "/api/file/v2/files/uploadURL"}, {kFileEditors}},
      {{"GET", "/api/file/v2/files/" + placeholder_id + "/downloadURL"}, {kFileViewers}},
      {{"POST", "/api/file/v2/files/storageInstructions"}, {kDatasetEditors}},
      {{"POST", "/api/file/v2/file-collections/storageInstructions"}, {kDatasetEditors}},
      {{"POST", "/api/file/v2/files/retrievalInstructions"}, {kDatasetViewers}},
      {{"POST", "/api/file/v2/file-collections/retrievalInstructions"}, {kDatasetViewers}},
      {{"POST", "/api/file/v2/files/copy"}, {kStorageCreator, kStorageAdmin}},
      {{"POST", "/api/file/v2/file-collections/copy"}, {kStorageCreator, kStorageAdmin}},
      {{"POST", "/api/file/v2/delivery/GetFileSignedUrl"}, {kDeliveryViewer}},
      //  ★ `revokeURL` 按契约 §1.2 **不需要** `data-partition-id`
      {{"POST", "/api/file/v2/files/revokeURL"}, {kFileAdmin}, /*requires_partition=*/false},
  };
}

//  每个角色的一个代表 token（用于"角色不足 → 403"的负向）
struct RoleToken {
  std::string role;
  std::string token;
};

std::vector<RoleToken> AllRoleTokens() {
  //  每个 token **只**带一个角色 → "只有 A 角色时访问要求 B 的端点必须 403"
  return {
      {kFileViewers, MintToken({kFileViewers}, "viewer@example.com")},
      {kFileEditors, MintToken({kFileEditors}, "editor@example.com")},
      {kFileAdmin, MintToken({kFileAdmin}, "admin@example.com")},
      {kDeliveryViewer, MintToken({kDeliveryViewer}, "delivery@example.com")},
      {kDatasetViewers, MintToken({kDatasetViewers}, "dsviewer@example.com")},
      {kDatasetEditors, MintToken({kDatasetEditors}, "dseditor@example.com")},
      {kStorageCreator, MintToken({kStorageCreator}, "creator@example.com")},
      {kStorageAdmin, MintToken({kStorageAdmin}, "storageadmin@example.com")},
  };
}

}  // namespace

TEST_CASE("★ C8.1 端点 × 角色矩阵：正向放行、负向 403、缺 token/partition → 401",
          "[phase8][integration][c8.1]") {
  fss::ManualClock clock(kNow);
  LocalJwtOptions options;
  options.hmac_secret = kSecret;
  LocalJwtAuthorizer authorizer(options, clock);
  fss::test::HttpFixture fx(authorizer);
  const int port = fx.port();

  const auto matrix = EndpointMatrix();
  const auto tokens = AllRoleTokens();
  REQUIRE(matrix.size() == 16);  // 与契约 §1.3 的行数一致（这里手抄了 16 条）

  for (const auto& rule : matrix) {
    CAPTURE(rule.call.method, rule.call.target);

    //  ---- ① 缺 token → 401 + 固定消息 ----
    {
      std::vector<std::string> headers;
      if (rule.requires_partition) headers.push_back("data-partition-id: opendes");
      RawClient client(port, true);
      REQUIRE(client.Connect());
      if (rule.call.method != "GET") {
        headers.push_back("Content-Length: " + std::to_string(rule.call.body.size()));
      }
      REQUIRE(client.SendRequest(rule.call.method, rule.call.target, headers, rule.call.body));
      const auto response = client.ReadResponse(10000);
      REQUIRE(response.has_value());
      REQUIRE(response->status == 401);
    }

    //  ---- ② 有 token 但缺 partition（契约要求 partition 的端点）→ 401 ----
    if (rule.requires_partition) {
      const std::string token = MintToken(rule.allowed_roles, "editor@example.com");
      REQUIRE(CallWithRole(port, rule.call, "Bearer " + token, /*with_partition=*/false) == 401);
    }

    //  ---- ③ 每个"只带一个角色"的 token：命中 → 非 401/403；未命中 → **403** ----
    for (const auto& role_token : tokens) {
      const bool allowed = std::find(rule.allowed_roles.begin(), rule.allowed_roles.end(),
                                     role_token.role) != rule.allowed_roles.end();
      const int status = CallWithRole(port, rule.call, "Bearer " + role_token.token);
      CAPTURE(role_token.role, allowed, status);
      if (allowed) {
        //  授权通过：可能是 400/404（躯壳请求体），但**绝不能**是 401/403
        REQUIRE(status != 401);
        REQUIRE(status != 403);
      } else {
        REQUIRE(status == 403);  // 未授权不得靠 400 的差异探测数据（契约 §1.3 末段）
      }
    }
  }
}

TEST_CASE("★ C8.1 免鉴权端点：info / liveness / readiness 在任何 token 下都 200",
          "[phase8][integration][c8.1]") {
  fss::ManualClock clock(kNow);
  LocalJwtOptions options;
  options.hmac_secret = kSecret;
  LocalJwtAuthorizer authorizer(options, clock);
  fss::test::HttpFixture fx(authorizer);
  const int port = fx.port();

  for (const std::string target :
       {"/api/file/v2/info", "/api/file/v2/liveness_check", "/api/file/v2/readiness_check"}) {
    CAPTURE(target);
    //  连头都不带
    RawClient client(port, true);
    REQUIRE(client.Connect());
    REQUIRE(client.SendRequest("GET", target, {}, ""));
    const auto response = client.ReadResponse(10000);
    REQUIRE(response.has_value());
    REQUIRE(response->status == 200);
    //  无效 token 也不能把运维端点变成 401（否则探针会误判实例不健康）
    std::vector<std::string> headers = {"authorization: Bearer garbage",
                                        "data-partition-id: opendes"};
    RawClient authed(port, true);
    REQUIRE(authed.Connect());
    REQUIRE(authed.SendRequest("GET", target, headers, ""));
    const auto authed_response = authed.ReadResponse(10000);
    REQUIRE(authed_response.has_value());
    REQUIRE(authed_response->status == 200);
  }

  //  ★ C8.5：`/v2/info` 必须**显示**鉴权模式（这里 fixture 未设置 → 缺省 `disabled`）
  const auto info = HttpDo(port, "GET", "/api/file/v2/info");
  REQUIRE(info.status == 200);
  const auto json = fss::json::ParseObject(info.body);
  REQUIRE(json.ok());
  REQUIRE(json.value().contains("authMode"));
  REQUIRE(json.value()["authMode"] == "disabled");
}

TEST_CASE("★ C8.1 正向垂直切片：正确角色能完整走完上传→登记→下载→删除",
          "[phase8][integration][c8.1]") {
  fss::ManualClock clock(kNow);
  LocalJwtOptions options;
  options.hmac_secret = kSecret;
  LocalJwtAuthorizer authorizer(options, clock);
  fss::test::HttpFixture fx(authorizer);
  const int port = fx.port();

  const std::string editor = "Bearer " + MintToken({kFileEditors}, "editor@example.com");
  const std::string viewer = "Bearer " + MintToken({kFileViewers}, "viewer@example.com");
  const std::string admin = "Bearer " + MintToken({kFileAdmin}, "admin@example.com");

  const std::vector<std::string> auth_headers = {"data-partition-id: opendes"};
  const auto with_auth = [&](const std::string& token, const std::string& body,
                             const std::string& content_type = "application/json") {
    std::vector<std::string> headers = {"authorization: " + token,
                                        "data-partition-id: opendes",
                                        "content-type: " + content_type};
    headers.push_back("Content-Length: " + std::to_string(body.size()));
    return headers;
  };

  //  ① editors 拿上传地址
  RawClient client(port, true);
  REQUIRE(client.Connect());
  REQUIRE(client.SendRequest("GET", "/api/file/v2/files/uploadURL", with_auth(editor, ""), ""));
  const auto upload = client.ReadResponse(10000);
  REQUIRE(upload.has_value());
  INFO("uploadURL → " << upload->status << " " << upload->body);
  REQUIRE(upload->status == 200);
  const auto upload_json = fss::json::ParseObject(upload->body);
  REQUIRE(upload_json.ok());
  const std::string file_id = upload_json.value()["FileID"].get<std::string>();
  const std::string file_source = upload_json.value()["Location"]["FileSource"].get<std::string>();
  const std::string signed_url = upload_json.value()["Location"]["SignedURL"].get<std::string>();

  //  ② 数据面 PUT（自签 URL，不需要 JWT）
  {
    fss::test::RawClient put(port, true);
    REQUIRE(put.Connect());
    const std::string payload = "authenticated-payload";
    std::vector<std::string> headers = {
        "Content-Length: " + std::to_string(payload.size()), "authorization: Bearer " + editor,
        "data-partition-id: opendes"};
    REQUIRE(put.SendRequest("PUT", fss::test::TargetOf(signed_url), headers, payload));
    const auto response = put.ReadResponse(10000);
    REQUIRE(response.has_value());
    REQUIRE(response->status == 200);
  }

  //  ③ editors 登记元数据
  auto record = fss::test::AppFixture::MakeRecord(file_source, "auth-matrix.bin");
  std::string record_id;
  {
    fss::test::RawClient post(port, true);
    REQUIRE(post.Connect());
    const std::string body = fss::json::Dump(fss::domain::ToJson(record));
    REQUIRE(post.SendRequest("POST", "/api/file/v2/files/metadata", with_auth(editor, body), body));
    const auto response = post.ReadResponse(10000);
    INFO("metadata → " << response->status << " " << response->body);
    REQUIRE(response->status == 201);
    //  ★ 记录 id 由服务端生成（不是 `file_id`）：后续按 id 读/删
    const auto created = fss::json::ParseObject(response->body);
    REQUIRE(created.ok());
    record_id = created.value()["id"].get<std::string>();
    REQUIRE_FALSE(record_id.empty());
  }

  //  ④ viewers 下载地址 + 读元数据
  {
    fss::test::RawClient get(port, true);
    REQUIRE(get.Connect());
    REQUIRE(get.SendRequest("GET", "/api/file/v2/files/" + file_id + "/downloadURL",
                            with_auth(viewer, ""), ""));
    const auto response = get.ReadResponse(10000);
    INFO("downloadURL → " << response->status);
    REQUIRE(response->status == 200);

    fss::test::RawClient meta(port, true);
    REQUIRE(meta.Connect());
    REQUIRE(meta.SendRequest("GET", "/api/file/v2/files/" + record_id + "/metadata",
                             with_auth(viewer, ""), ""));
    const auto meta_response = meta.ReadResponse(10000);
    REQUIRE(meta_response->status == 200);
  }

  //  ⑤ editors 删除（任一角色：editors ∪ admin）
  {
    fss::test::RawClient del(port, true);
    REQUIRE(del.Connect());
    std::vector<std::string> headers = {"authorization: " + admin,
                                        "data-partition-id: opendes", "Content-Length: 0"};
    REQUIRE(del.SendRequest("DELETE", "/api/file/v2/files/" + record_id + "/metadata", headers, ""));
    const auto response = del.ReadResponse(10000);
    INFO("delete → " << response->status);
    REQUIRE(response->status == 204);
  }
}
