// =============================================================================
//  C5.8：S3 模式端到端 —— 客户端**直连存储端点**，服务不代理字节
// =============================================================================
//  与 POSIX 模式端到端的本质区别（这正是判据要钉的东西）：
//    · POSIX：`SignedURL` 指向**本服务**的 `/api/file/v1/transfer/{token}`，字节经服务转发；
//    · S3   ：`SignedURL` 指向**存储端点**（`<endpoint>/<bucket>/<key>?X-Amz-...`），
//             客户端 PUT/GET 直接打到对象存储 —— 服务只签发地址与复制元数据。
//  因此这里断言的第一件事就是 URL 的 host：**它必须是 mock-S3，而不是本服务**。
//  第二件事是字节：客户端写进 mock-S3 的内容，服务（通过 S3 数据面）能原样读回/复制。
//
//  ⚠️ mock-S3 用 Python 独立验签（`sigv4_reference.py`），所以"预签名 URL 能用"这件事
//     同时也证明了签名被独立实现接受。
// =============================================================================
#include <catch2/catch.hpp>

#include "http_fixture.h"
#include "mock_s3.h"

#include "common/crypto/crypto.h"
#include "common/json/json.h"
#include "domain/model/file_metadata.h"
#include "infra/blob/s3/s3_blob_store.h"

#include <string>
#include <vector>

namespace {

using fss::test::Authed;
using fss::test::HttpDo;
using fss::test::Reply;
using fss::test::TargetOf;

//  直接用 curl 访问**存储端点**（不是本服务）：这是"客户端直连"的可执行形式
Reply DirectToStorage(const std::string& method, const std::string& url,
                      const std::string& body = {}) {
  const std::string body_file = "/tmp/fss_s3_e2e_" + std::to_string(::getpid()) + ".bin";
  if (!body.empty()) {
    std::ofstream out(body_file, std::ios::binary);
    out << body;
    out.close();
  }
  const std::string download = body_file + ".out";
  std::string command = "curl -sS -X " + method + " -o " + download + " -w '%{http_code}' ";
  if (!body.empty()) command += "--data-binary @" + body_file + " ";
  command += "'" + url + "' 2>/dev/null";
  std::FILE* pipe = ::popen(command.c_str(), "r");
  REQUIRE(pipe != nullptr);
  char buffer[64] = {0};
  REQUIRE(std::fgets(buffer, sizeof(buffer), pipe) != nullptr);
  ::pclose(pipe);
  Reply reply;
  reply.status = std::atoi(buffer);
  std::ifstream in(download, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  reply.body = ss.str();
  std::error_code error;
  std::filesystem::remove(body_file, error);
  std::filesystem::remove(download, error);
  return reply;
}

}  // namespace

TEST_CASE("★ C5.8 S3 模式端到端：uploadURL → 直连存储 PUT → metadata → downloadURL → 直连 GET → DELETE",
          "[phase5][integration][s3][c5.8]") {
  fss::test::S3StackFixture fixture;
  const int port = fixture.port();
  const std::string storage_host = fixture.mock.endpoint();

  //  ① uploadURL：`SignedURL` 必须指向存储端点（服务不代理字节）
  const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(upload.status == 200);
  const auto upload_json = fss::json::ParseObject(upload.body);
  REQUIRE(upload_json.ok());
  const std::string file_id = upload_json.value()["FileID"].get<std::string>();
  const std::string file_source =
      upload_json.value()["Location"]["FileSource"].get<std::string>();
  const std::string put_url = upload_json.value()["Location"]["SignedURL"].get<std::string>();
  INFO("SignedURL: " << put_url);
  REQUIRE(put_url.rfind("http://" + storage_host + "/", 0) == 0);  // ★ 存储端点
  REQUIRE(put_url.find("/api/file/") == std::string::npos);        // ★ 不是本服务
  REQUIRE(put_url.find("/v1/transfer/") == std::string::npos);     // ★ 不是自签数据面
  REQUIRE(put_url.find("X-Amz-Signature=") != std::string::npos);  // 原生 SigV4 预签名

  //  ② 客户端**直连存储**上传（不经过本服务）
  const std::string payload = "s3-mode-payload-\x01\x02\x03-0123456789";
  const auto put = DirectToStorage("PUT", put_url, payload);
  INFO("直连 PUT 状态码: " << put.status);
  REQUIRE(put.status == 200);

  //  ③ 登记元数据：服务端自己做 staging→persistent 的**服务端复制**并用 S3 的
  //     原生校验和覆写记录（S3 的 ETag；不是 SHA-256 —— 见证据文件的说明）
  auto record = fss::test::AppFixture::MakeRecord(file_source, "s3-e2e.bin");
  const auto created = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(),
                              fss::json::Dump(fss::domain::ToJson(record)));
  INFO("metadata 响应: " << created.body);
  REQUIRE(created.status == 201);
  const auto created_json = fss::json::ParseObject(created.body);
  REQUIRE(created_json.ok());
  const std::string record_id = created_json.value()["id"].get<std::string>();

  const auto fetched = HttpDo(port, "GET", "/api/file/v2/files/" + record_id + "/metadata",
                              Authed());
  REQUIRE(fetched.status == 200);
  const auto stored = fss::json::ParseObject(fetched.body);
  REQUIRE(stored.ok());
  //  校验和必须被服务端写回，且 record 的两处 Checksum 必须一致（契约 §3.4）
  const std::string checksum = stored.value()["data"]["Checksum"].get<std::string>();
  REQUIRE_FALSE(checksum.empty());
  REQUIRE(checksum != fss::crypto::Sha256Hex(""));  // 不是"空对象的哈希"占位
  REQUIRE(stored.value()["data"]["DatasetProperties"]["FileSourceInfo"]["Checksum"]
              .get<std::string>() == checksum);
  INFO("S3 原生校验和算法: "
       << stored.value()["data"]["ChecksumAlgorithm"].get<std::string>());

  //  ④ downloadURL：同样是存储端点的原生预签名
  const auto download =
      HttpDo(port, "GET", "/api/file/v2/files/" + file_id + "/downloadURL", Authed());
  REQUIRE(download.status == 200);
  const auto download_json = fss::json::ParseObject(download.body);
  REQUIRE(download_json.ok());
  const std::string get_url = download_json.value()["SignedUrl"].get<std::string>();
  INFO("SignedUrl: " << get_url);
  REQUIRE(get_url.rfind("http://" + storage_host + "/", 0) == 0);

  //  ⑤ 客户端直连下载：字节必须完全一致（不是"长度对"）
  const auto got = DirectToStorage("GET", get_url);
  REQUIRE(got.status == 200);
  REQUIRE(got.body == payload);

  //  ⑥ 删除元数据；随后该记录读不到（404）
  const auto removed = HttpDo(port, "DELETE", "/api/file/v2/files/" + record_id + "/metadata",
                              Authed());
  REQUIRE(removed.status == 204);
  const auto gone = HttpDo(port, "GET", "/api/file/v2/files/" + record_id + "/metadata", Authed());
  REQUIRE(gone.status == 404);
}

TEST_CASE("★ C5.8 S3 模式：`Range` 直连下载（断点续传路径同样直连存储）",
          "[phase5][integration][s3][c5.8]") {
  fss::test::S3StackFixture fixture;
  const int port = fixture.port();

  const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(upload.status == 200);
  const auto upload_json = fss::json::ParseObject(upload.body);
  const std::string file_id = upload_json.value()["FileID"].get<std::string>();
  const std::string file_source =
      upload_json.value()["Location"]["FileSource"].get<std::string>();

  const std::string payload = "0123456789abcdefghijklmnopqrstuvwxyz";
  REQUIRE(DirectToStorage("PUT",
                          upload_json.value()["Location"]["SignedURL"].get<std::string>(),
                          payload)
              .status == 200);
  auto record = fss::test::AppFixture::MakeRecord(file_source, "range.bin");
  REQUIRE(HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(),
                 fss::json::Dump(fss::domain::ToJson(record)))
              .status == 201);

  const auto download =
      HttpDo(port, "GET", "/api/file/v2/files/" + file_id + "/downloadURL", Authed());
  const auto download_json = fss::json::ParseObject(download.body);
  const std::string get_url = download_json.value()["SignedUrl"].get<std::string>();

  //  取尾部 8 字节：`CURLOPT_RANGE` 会自发一个未参与签名的 `Range` 头（与 S3 的实测一致）
  const std::string tail_file = "/tmp/fss_s3_range_" + std::to_string(::getpid()) + ".bin";
  const std::string command = "curl -sS -o " + tail_file + " -w '%{http_code}' -r -8 '" +
                              get_url + "' 2>/dev/null";
  std::FILE* pipe = ::popen(command.c_str(), "r");
  REQUIRE(pipe != nullptr);
  char buffer[64] = {0};
  REQUIRE(std::fgets(buffer, sizeof(buffer), pipe) != nullptr);
  ::pclose(pipe);
  REQUIRE(std::atoi(buffer) == 206);
  std::ifstream in(tail_file, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  REQUIRE(ss.str() == payload.substr(payload.size() - 8));
  std::error_code error;
  std::filesystem::remove(tail_file, error);
}

TEST_CASE("★ C5.4 virtual-host 形态的数据面：同一个对象，两种寻址都能读写",
          "[phase5][integration][s3][c5.4]") {
  //  做法：预签名 URL 用 **virtual-host** 形态（`bucket.s3.amazonaws.com/<key>`），
  //  再用 `curl --connect-to` 把该主机指到 mock —— 这样 `Host` 头带桶前缀，
  //  而**生产代码里不需要任何测试钩子**（真实部署里 DNS 自然会解析到存储端点）。
  fss::test::S3StackFixture fixture;
  fss::infra::S3Options vhost_options = fss::test::S3StackFixture::MakeOptions(fixture.mock);
  vhost_options.endpoint = "s3.amazonaws.com";
  vhost_options.force_path_style = false;  // ★ 只有这一处不同
  fss::infra::S3BlobStore vhost_store(vhost_options, fixture.clock);

  const std::string bucket = "vhostbucket";
  const std::string key = "dir/obj.bin";
  const std::string payload = "virtual-host-payload";
  const fss::domain::ObjectRef ref{bucket, key};

  //  ① 建桶 + path-style 写入。
  //  ★ 注意：`vhost_store` 的**服务端**调用（put/get/ensure_container）在这里用不了 ——
  //    它的 host 是 `bucket.s3.amazonaws.com`，在测试环境里**无法 DNS 解析**（生产环境自然可以）。
  //    因此 virtual-host 只走"预签名 URL + 客户端直连"这条路径（也正是真实用法），
  //    服务端访问复用同一 mock 上的 path-style store（两者指向同一份存储）。
  REQUIRE(fixture.store().ensure_container(bucket).ok());
  fss::bytes::StringSource source(payload);
  REQUIRE(fixture.store().put(ref, source, fss::domain::PutOptions{}).ok());

  //  ② virtual-host 预签名 GET → 用 --connect-to 直连 mock
  fss::domain::PresignOptions options;
  options.method = "GET";
  options.expires_in_seconds = 900;
  const auto presigned = vhost_store.presign_get(ref, options);
  REQUIRE(presigned.ok());
  INFO("virtual-host URL: " << presigned.value().url);
  //  ★ 主机是 `bucket.s3.amazonaws.com`，路径里**不含**桶名
  REQUIRE(presigned.value().url.rfind("http://" + bucket + ".s3.amazonaws.com/" + key + "?", 0) ==
          0);

  const std::string out_file = "/tmp/fss_s3_vhost_" + std::to_string(::getpid()) + ".bin";
  const std::string command = "curl -sS -o " + out_file + " -w '%{http_code}' " +
                              "--connect-to " + bucket +
                              ".s3.amazonaws.com:80:127.0.0.1:" + std::to_string(fixture.mock.port()) +
                              " '" + presigned.value().url + "' 2>/dev/null";
  std::FILE* pipe = ::popen(command.c_str(), "r");
  REQUIRE(pipe != nullptr);
  char buffer[64] = {0};
  REQUIRE(std::fgets(buffer, sizeof(buffer), pipe) != nullptr);
  ::pclose(pipe);
  INFO("virtual-host GET 状态码: " << buffer);
  REQUIRE(std::atoi(buffer) == 200);
  std::ifstream in(out_file, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  REQUIRE(ss.str() == payload);  // 与 path-style 写入的是**同一个对象**
  std::error_code error;
  std::filesystem::remove(out_file, error);

  //  ③ virtual-host 预签名 PUT 也能写（写进去的 key 与 path-style 视图中一致）
  fss::domain::PresignOptions put_options;
  put_options.method = "PUT";
  put_options.expires_in_seconds = 900;
  const auto presigned_put = vhost_store.presign_put(fss::domain::ObjectRef{bucket, "vhost/new.txt"},
                                                     put_options);
  REQUIRE(presigned_put.ok());
  const std::string put_file = "/tmp/fss_s3_vhost_put_" + std::to_string(::getpid()) + ".bin";
  {
    std::ofstream out(put_file, std::ios::binary);
    out << "written-via-virtual-host";
  }
  const std::string put_command =
      "curl -sS -o /dev/null -w '%{http_code}' -X PUT --data-binary @" + put_file +
      " --connect-to " + bucket + ".s3.amazonaws.com:80:127.0.0.1:" +
      std::to_string(fixture.mock.port()) + " '" + presigned_put.value().url + "' 2>/dev/null";
  pipe = ::popen(put_command.c_str(), "r");
  REQUIRE(pipe != nullptr);
  REQUIRE(std::fgets(buffer, sizeof(buffer), pipe) != nullptr);
  ::pclose(pipe);
  REQUIRE(std::atoi(buffer) == 200);
  std::filesystem::remove(put_file, error);

  //  用 path-style 读回来，证明两种寻址指向同一份存储
  fss::bytes::StringSink sink;
  REQUIRE(fixture.store().get(fss::domain::ObjectRef{bucket, "vhost/new.txt"}, sink,
                              fss::domain::ByteRange{})
              .ok());
  REQUIRE(sink.str() == "written-via-virtual-host");
}
