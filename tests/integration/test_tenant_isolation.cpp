// =============================================================================
//  P8 切片 2：跨租户隔离（C8.2）—— A 的凭证不能碰 B 的任何东西
// =============================================================================
//  威胁模型 T6 的完整验证：**两个租户、两条协议、真实 HTTP 端口**。
//
//  三道防线各自都要被独立证明（缺一条都不是"隔离"）：
//    ① **token 绑定**：A 的 token 配 B 的 `data-partition-id` → 403（ADR-012 §3）
//    ② **仓储分区隔离**：B 读/删/列 A 的记录 → 404 / 空集（**不得**返回 A 的数据）
//    ③ **签名 URL 绑对象**：A 的下载 URL 配 B 的 partition 头 → 403；把 URL 改成
//      指向 B 的对象 → 验签失败 401（对象键在载荷里，改不了）
//
//  ★ 本用例**只**用真实 JWT（`LocalJwtAuthorizer`）与真实 HTTP 端口：
//    "跨租户被拒"如果只在用例层测，就漏掉了"适配层把租户弄丢"这一类缺陷。
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
using fss::test::TargetOf;

constexpr char kSecret[] = "tenant-isolation-secret";
constexpr std::int64_t kNow = 1700000000;
//  ★ 用 `std::string` 而不是字符数组：这些值要与 "data-partition-id: " 之类拼接
const std::string kTenantA = "opendes";
const std::string kTenantB = "tenant-b";

std::string MintToken(const std::string& partition) {
  fss::json::Value header;
  header["alg"] = "HS256";
  fss::json::Value payload;
  payload["sub"] = "user-1";
  payload["email"] = "user@example.com";
  payload["data-partition-id"] = partition;
  payload["roles"] = fss::json::Value::array(
      {"service.file.editors", "service.file.viewers", "service.file.admin"});
  payload["exp"] = kNow + 3600;
  const std::string header_b64 = fss::crypto::Base64UrlEncode(fss::json::Dump(header));
  const std::string payload_b64 = fss::crypto::Base64UrlEncode(fss::json::Dump(payload));
  const std::string signing_input = header_b64 + "." + payload_b64;
  const auto digest = fss::crypto::HmacSha256(kSecret, signing_input);
  return signing_input + "." + fss::crypto::Base64UrlEncode(digest);
}

struct Reply {
  int status = 0;
  std::string body;
};

//  发一个请求：token 与 partition 头**分别**给（跨租户用例的关键）
Reply Request(int port, const std::string& method, const std::string& target,
              const std::string& token, const std::string& partition,
              const std::string& body = "") {
  std::vector<std::string> headers;
  if (!token.empty()) headers.push_back("authorization: " + token);
  if (!partition.empty()) headers.push_back("data-partition-id: " + partition);
  if (!body.empty() || method == "POST" || method == "PUT") {
    headers.push_back("content-type: application/json");
    headers.push_back("Content-Length: " + std::to_string(body.size()));
  }
  RawClient client(port, true);
  REQUIRE(client.Connect());
  REQUIRE(client.SendRequest(method, target, headers, body));
  const auto response = client.ReadResponse(15000);
  REQUIRE(response.has_value());
  return Reply{response->status, response->body};
}

//  A 租户完整写入一条记录：uploadURL → PUT → metadata
struct CreatedRecord {
  std::string file_id;
  std::string record_id;
  std::string file_source;
  std::string signed_url;
};

CreatedRecord SeedTenantA(int port) {
  const std::string token = "Bearer " + MintToken(kTenantA);
  const auto upload = Request(port, "GET", "/api/file/v2/files/uploadURL", token, kTenantA);
  REQUIRE(upload.status == 200);
  const auto json = fss::json::ParseObject(upload.body);
  REQUIRE(json.ok());
  CreatedRecord created;
  created.file_id = json.value()["FileID"].get<std::string>();
  created.file_source = json.value()["Location"]["FileSource"].get<std::string>();
  created.signed_url = json.value()["Location"]["SignedURL"].get<std::string>();

  const std::string payload = "tenant-a-secret-payload";
  {
    RawClient client(port, true);
    REQUIRE(client.Connect());
    std::vector<std::string> headers = {"authorization: " + token,
                                        "data-partition-id: " + kTenantA,
                                        "Content-Length: " + std::to_string(payload.size())};
    REQUIRE(client.SendRequest("PUT", TargetOf(created.signed_url), headers, payload));
    const auto response = client.ReadResponse(15000);
    REQUIRE(response.has_value());
    REQUIRE(response->status == 200);
  }

  auto record = fss::test::AppFixture::MakeRecord(created.file_source, "tenant-a.bin");
  const auto metadata = Request(port, "POST", "/api/file/v2/files/metadata", token, kTenantA,
                                fss::json::Dump(fss::domain::ToJson(record)));
  INFO("A 登记元数据 → " << metadata.status << " " << metadata.body);
  REQUIRE(metadata.status == 201);
  const auto created_json = fss::json::ParseObject(metadata.body);
  REQUIRE(created_json.ok());
  created.record_id = created_json.value()["id"].get<std::string>();
  return created;
}

}  // namespace

TEST_CASE("★ C8.2 跨租户：A 的 token + B 的请求头 → 403；B 读/删/列 A 的记录 → 404/空集",
          "[phase8][integration][c8.2]") {
  fss::ManualClock clock(kNow);
  LocalJwtOptions options;
  options.hmac_secret = kSecret;
  LocalJwtAuthorizer authorizer(options, clock);
  fss::test::HttpFixture fx(authorizer);
  const int port = fx.port();

  const auto record = SeedTenantA(port);
  const std::string token_a = "Bearer " + MintToken(kTenantA);
  const std::string token_b = "Bearer " + MintToken(kTenantB);

  //  ---- ① token 绑定：A 的 token 不能配 B 的头（否则"改个头就换租户"）----
  {
    const auto cross =
        Request(port, "GET", "/api/file/v2/files/" + record.record_id + "/metadata", token_a, kTenantB);
    INFO("A token + B header → " << cross.status << " " << cross.body);
    REQUIRE(cross.status == 403);
    //  ★ 403 而不是 404：说明拦在鉴权层，**没有**去 B 的分区里查（不泄露任何存在性）
  }

  //  ---- ② 仓储分区隔离：B 用自己的 token 读 A 的记录 → 404，且响应里没有 A 的数据 ----
  {
    const auto read =
        Request(port, "GET", "/api/file/v2/files/" + record.record_id + "/metadata", token_b, kTenantB);
    INFO("B 读 A 的记录 → " << read.status);
    REQUIRE(read.status == 404);
    REQUIRE(read.body.find(record.file_source) == std::string::npos);
    REQUIRE(read.body.find("tenant-a-secret-payload") == std::string::npos);
  }
  {
    //  B 删 A 的记录 → 404，且 A 的记录**仍然存在**
    const auto remove =
        Request(port, "DELETE", "/api/file/v2/files/" + record.record_id + "/metadata", token_b, kTenantB);
    INFO("B 删 A 的记录 → " << remove.status);
    REQUIRE(remove.status == 404);
    const auto still_there = Request(port, "GET",
                                     "/api/file/v2/files/" + record.record_id + "/metadata",
                                     token_a, kTenantA);
    REQUIRE(still_there.status == 200);
  }
  {
    //  B 的列表里不得出现 A 的记录（`getFileList` 走仓储的 partition 过滤）
    const std::string body = R"({"Items": 100, "PageNum": 0})";
    const auto list = Request(port, "POST", "/api/file/v2/getFileList", token_b, kTenantB, body);
    INFO("B 列自己的记录 → " << list.status << " " << list.body);
    //  B 没有任何记录 → 契约 §2.5 的"无匹配"是 400（不是空 200）
    REQUIRE(list.status == 400);
    REQUIRE(list.body.find(record.file_id) == std::string::npos);

    //  A 列自己的记录必须能看到它（正例：证明上面那个 400 不是"列表坏了"）
    const auto list_a = Request(port, "POST", "/api/file/v2/getFileList", token_a, kTenantA, body);
    INFO("A 列自己的记录 → " << list_a.status);
    REQUIRE(list_a.status == 200);
    REQUIRE(list_a.body.find(record.file_id) != std::string::npos);
  }
  {
    //  按 file_id 取下载地址：跨租户 → 404（B 的分区里没有这条记录）
    const auto url = Request(port, "GET", "/api/file/v2/files/" + record.file_id + "/downloadURL",
                             token_b, kTenantB);
    REQUIRE(url.status == 404);
  }
  {
    //  取 A 的上传地址时指定 A 的 file_id（幂等冲突语义）：B 用它 → 不会命中 A 的记录
    const auto upload = Request(port, "GET", "/api/file/v2/files/uploadURL", token_b, kTenantB);
    REQUIRE(upload.status == 200);
    const auto json = fss::json::ParseObject(upload.body);
    REQUIRE(json.ok());
    //  B 拿到的 file_id 与 A 的不同（不同分区各自生成，且不共享记录）
    REQUIRE(json.value()["FileID"].get<std::string>() != record.file_id);
  }
}

TEST_CASE("★ C8.2 签名 URL 绑对象：跨租户头 → 403；改 URL 指向 B 的对象 → 401",
          "[phase8][integration][c8.2]") {
  fss::ManualClock clock(kNow);
  LocalJwtOptions options;
  options.hmac_secret = kSecret;
  LocalJwtAuthorizer authorizer(options, clock);
  fss::test::HttpFixture fx(authorizer);
  const int port = fx.port();

  const auto record = SeedTenantA(port);
  //  A 的下载 URL（自签 token 绑定了 partition / container / object_key / op）
  const std::string token_a = "Bearer " + MintToken(kTenantA);
  const auto url_response =
      Request(port, "GET", "/api/file/v2/files/" + record.file_id + "/downloadURL", token_a, kTenantA);
  REQUIRE(url_response.status == 200);
  const auto url_json = fss::json::ParseObject(url_response.body);
  REQUIRE(url_json.ok());
  const std::string signed_url = url_json.value()["SignedUrl"].get<std::string>();
  const std::string target = TargetOf(signed_url);

  //  ---- ① 正常使用（正例）：A 的 URL + A 的 partition 头 → 200 ----
  {
    RawClient client(port, true);
    REQUIRE(client.Connect());
    REQUIRE(client.SendRequest("GET", target,
                               {"authorization: " + token_a, "data-partition-id: " + kTenantA}, ""));
    const auto response = client.ReadResponse(15000);
    REQUIRE(response.has_value());
    INFO("A 用 A 的签名 URL → " << response->status);
    REQUIRE(response->status == 200);
    REQUIRE(response->body == "tenant-a-secret-payload");
  }

  //  ---- ② 跨租户头：B 的 partition 头配 A 的 URL → 403（内核的租户绑定）----
  {
    RawClient client(port, true);
    REQUIRE(client.Connect());
    REQUIRE(client.SendRequest("GET", target,
                               {"authorization: Bearer " + MintToken(kTenantB),
                                "data-partition-id: " + kTenantB},
                               ""));
    const auto response = client.ReadResponse(15000);
    INFO("B 的 partition 头 + A 的 URL → " << response->status);
    REQUIRE(response->status == 403);
    REQUIRE(response->body.find("tenant-a-secret-payload") == std::string::npos);
  }

  //  ---- ③ 篡改 URL：签名/载荷不符 → 401（对象键在载荷里，改不了）----
  const auto get_with_headers = [&](const std::string& path) {
    RawClient client(port, true);
    REQUIRE(client.Connect());
    REQUIRE(client.SendRequest("GET", path,
                               {"authorization: " + token_a, "data-partition-id: " + kTenantA}, ""));
    const auto response = client.ReadResponse(15000);
    REQUIRE(response.has_value());
    return Reply{response->status, response->body};
  };
  {
    //  ③a 改 token 段里的一个字符：解码/验签失败 → 401
    const std::string marker = "/v1/transfer/";
    const auto at = target.find(marker);
    REQUIRE(at != std::string::npos);
    std::string tampered = target;
    const std::size_t token_start = at + marker.size();
    tampered[token_start] = tampered[token_start] == 'A' ? 'B' : 'A';
    const auto response = get_with_headers(tampered);
    INFO("篡改 token → " << response.status);
    REQUIRE(response.status == 401);
    REQUIRE(response.body.find("tenant-a-secret-payload") == std::string::npos);
  }
  {
    //  ③b 改 `sig=` 参数 → 401
    const auto sig_at = target.find("sig=");
    REQUIRE(sig_at != std::string::npos);
    std::string tampered = target;
    tampered[sig_at + 4] = tampered[sig_at + 4] == 'a' ? 'b' : 'a';
    const auto response = get_with_headers(tampered);
    INFO("篡改 sig → " << response.status);
    REQUIRE(response.status == 401);
  }
  {
    //  ③c 把路径改成"不存在的位置"（插入一段）→ 路由层面直接 404
    //     —— 注意它**不是** 401：请求根本没到 token 校验那一步。两者都不泄露数据，
    //     但错误类别不同，这里如实断言（否则会误以为"篡改一律 401"）。
    const auto slash = target.rfind('/');
    const std::string reshaped = target.substr(0, slash) + "/x" + target.substr(slash);
    const auto response = get_with_headers(reshaped);
    INFO("改路径形状 → " << response.status);
    REQUIRE(response.status == 404);
    REQUIRE(response.body.find("tenant-a-secret-payload") == std::string::npos);
  }

  //  ---- ④ B 的 token 拿不到 A 记录的下载 URL（分区里没有这条记录）----
  {
    const auto b_url = Request(port, "GET", "/api/file/v2/files/" + record.file_id + "/downloadURL",
                               "Bearer " + MintToken(kTenantB), kTenantB);
    REQUIRE(b_url.status == 404);
  }
}
