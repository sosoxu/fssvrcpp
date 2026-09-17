// =============================================================================
//  C5.2：**独立验签** —— 我们签出来的东西，得由"别人"验过才算数
// =============================================================================
//  做法：起一个 Python 写的假 S3（`tests/tools/mock_s3.py`），它用**独立的参照实现**
//  （`tests/tools/sigv4_reference.py`，只依赖 hmac/hashlib）重算 SigV4。
//  我们的 C++ 侧只负责**签**，验证完全在 Python 侧完成 —— 不存在"自签自验"。
//
//  用例形状（每条都必须是可执行断言，不能只在文档里说）：
//    ① 预签名 PUT → curl 直传 → mock 验签通过并写入 → 200
//    ② 预签名 GET → curl 取回 → 字节完全一致
//    ③ **篡改矩阵**：改签名/改 key/改有效期/改 SignedHeaders/过期 → 必须 403
//    ④ 头部签名（数据面口径）→ mock 验签通过 → 200
//    ⑤ 反向自证（R1）：把 mock 的密钥换成另一个 → 同一个 URL 必须**被拒**
//       （证明"通过"来自签名正确，而不是来自 mock 根本不检查）
// =============================================================================
#include <catch2/catch.hpp>

#include "http_fixture.h"  // 复用 HttpDo/Reply 的 curl 无关部分（见下方 RunCurl）

#include "common/crypto/crypto.h"
#include "common/json/json.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/blob/s3/s3_blob_store.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using fss::domain::ObjectRef;
using fss::domain::PresignOptions;
using fss::infra::AwsCredentials;
using fss::infra::S3BlobStore;
using fss::infra::S3Options;

constexpr const char* kAccessKey = "AKIDEXAMPLE";
constexpr const char* kSecretKey = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
//  预签名带**真实时间**（`X-Amz-Date`），mock 侧按真实时钟判过期 ——
//  因此这里用系统时钟而不是固定假时间：写死一个"2023 年"的时间在当前容器里（宿主时钟
//  在 2026 年）会被 mock 正确地判为"已过期"，那会把环境差异伪装成签名缺陷（P5-D03 的邻居）。
std::int64_t NowSeconds() { return fss::SystemClock{}.NowEpochSeconds(); }

std::string RepoPath(const std::string& relative) {
  return std::string(FSS_REPO_ROOT) + "/" + relative;
}

struct CurlResult {
  int status = 0;
  std::string body;
};

//  用 curl 执行一个请求（数据面走真实 HTTP；`-w` 拿状态码，`-o` 把体写文件）
CurlResult RunCurl(const std::string& method, const std::string& url,
                   const std::vector<std::string>& headers = {},
                   const std::string& body_file = {}, const std::string& save_to = {}) {
  const std::string out_file = save_to.empty() ? "/dev/null" : save_to;
  std::string command = "curl -sS -X " + method + " -o " + out_file + " -w '%{http_code}' ";
  for (const auto& header : headers) command += "-H '" + header + "' ";
  if (!body_file.empty()) command += "--data-binary @" + body_file + " ";
  command += "'" + url + "' 2>/dev/null";
  std::FILE* pipe = ::popen(command.c_str(), "r");
  REQUIRE(pipe != nullptr);
  char buffer[64] = {0};
  REQUIRE(std::fgets(buffer, sizeof(buffer), pipe) != nullptr);
  ::pclose(pipe);
  CurlResult result;
  result.status = std::atoi(buffer);
  if (!save_to.empty()) {
    std::ifstream in(save_to, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    result.body = ss.str();
  }
  return result;
}

std::string WriteTemp(const std::string& name, const std::string& content) {
  const std::string path = "/tmp/fss_s3_test_" + std::to_string(::getpid()) + "_" + name;
  std::ofstream out(path, std::ios::binary);
  out << content;
  out.close();
  return path;
}

S3BlobStore MakeStore(const std::string& endpoint) {
  S3Options options;
  options.endpoint = endpoint;
  options.region = "us-east-1";
  options.force_path_style = true;
  options.verify_tls = false;  // mock 是明文 HTTP
  options.scheme = "http";
  options.credentials = AwsCredentials{kAccessKey, kSecretKey, ""};
  static fss::SystemClock clock;
  return S3BlobStore(options, clock);
}

//  把 URL 里的某个 query 参数替换掉（用于篡改矩阵）
std::string ReplaceParam(const std::string& url, const std::string& name,
                         const std::string& value) {
  const std::string needle = name + "=";
  const auto pos = url.find(needle);
  REQUIRE(pos != std::string::npos);
  const auto begin = pos + needle.size();
  const auto end = url.find('&', begin);
  std::string out = url;
  out.replace(begin, (end == std::string::npos ? out.size() : end) - begin, value);
  return out;
}

}  // namespace

TEST_CASE("★ C5.2 预签名 PUT/GET 被**独立实现**验签通过（mock-S3 = Python 重算）",
          "[phase5][integration][s3][c5.2]") {
  fss::test::MockS3 mock;
  S3BlobStore store = MakeStore(mock.endpoint());
  //  mock 与真实 S3 一致：对象操作前桶必须存在（本切片起 `ensure_container` 已实现）
  REQUIRE(store.ensure_container("testbucket").ok());

  const std::string payload = "hello from presigned put";
  const std::string payload_file = WriteTemp("payload.bin", payload);
  const ObjectRef ref{"testbucket", "dir/obj.bin"};

  PresignOptions put_options;
  put_options.method = "PUT";
  put_options.expires_in_seconds = 900;
  const auto presigned_put = store.presign_put(ref, put_options);
  REQUIRE(presigned_put.ok());
  INFO("PUT URL: " << presigned_put.value().url);
  //  ★ C5.8 的判据：URL 必须指向**存储端点**（服务不代理字节）
  REQUIRE(presigned_put.value().url.rfind("http://" + mock.endpoint() + "/testbucket/", 0) == 0);
  REQUIRE(presigned_put.value().native);

  const auto put = RunCurl("PUT", presigned_put.value().url, {}, payload_file);
  INFO("PUT 状态码: " << put.status);
  REQUIRE(put.status == 200);  // mock 侧独立验签通过并写入

  PresignOptions get_options;
  get_options.method = "GET";
  get_options.expires_in_seconds = 900;
  const auto presigned_get = store.presign_get(ref, get_options);
  REQUIRE(presigned_get.ok());
  const std::string downloaded = WriteTemp("downloaded.bin", "");
  const auto get = RunCurl("GET", presigned_get.value().url, {}, {}, downloaded);
  REQUIRE(get.status == 200);
  REQUIRE(get.body == payload);  // 字节完全一致（不是"长度对"）
  //  有效期断言：±5s 容差（真实时钟）
  const std::int64_t now = NowSeconds();
  REQUIRE(presigned_get.value().expires_at_epoch_seconds >= now + 895);
  REQUIRE(presigned_get.value().expires_at_epoch_seconds <= now + 900);

  //  ★ 反向自证（R1）：换一个密钥再看同一个 URL —— 必须被拒。
  //    若这里还是 200，说明 mock 根本没验签（"通过"就成了假象）。
  {
    const std::string wrong_key_port_file = "/tmp/fss_wrongkey_port_" + std::to_string(::getpid());
    const std::string script = RepoPath("tests/tools/mock_s3.py");
    const std::string command = "python3 " + script + " --port 0 --access-key " + kAccessKey +
                                " --secret-key ANOTHER-SECRET --region us-east-1 > " +
                                wrong_key_port_file + " 2>/dev/null & echo $!";
    std::FILE* pipe = ::popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buffer[64] = {0};
    REQUIRE(std::fgets(buffer, sizeof(buffer), pipe) != nullptr);
    std::string pid(buffer);
    while (!pid.empty() && (pid.back() == '\n' || pid.back() == ' ')) pid.pop_back();
    ::pclose(pipe);
    int wrong_port = 0;
    for (int attempt = 0; attempt < 100 && wrong_port == 0; ++attempt) {
      std::ifstream in(wrong_key_port_file);
      std::string line;
      if (in.good() && std::getline(in, line)) {
        const auto space = line.find(' ');
        if (space != std::string::npos) wrong_port = std::stoi(line.substr(space + 1));
      }
      if (wrong_port == 0) ::usleep(100 * 1000);
    }
    REQUIRE(wrong_port > 0);
    //  把原 URL 的 host 换成"用另一个密钥的 mock"
    const std::string wrong_url = "http://127.0.0.1:" + std::to_string(wrong_port) +
                                  presigned_get.value().url.substr(
                                      presigned_get.value().url.find('/', 7));
    const auto rejected = RunCurl("GET", wrong_url);
    INFO("用不同密钥的 mock 验签结果: " << rejected.status);
    REQUIRE(rejected.status == 403);
    (void)std::system(("kill " + pid + " 2>/dev/null || true").c_str());
    std::error_code error;
    std::filesystem::remove(wrong_key_port_file, error);
  }

  std::error_code cleanup;
  std::filesystem::remove(payload_file, cleanup);
  std::filesystem::remove(downloaded, cleanup);
}

TEST_CASE("★ C5.2 篡改矩阵：改动签名的任一输入都必须被独立实现拒绝",
          "[phase5][integration][s3][c5.2]") {
  fss::test::MockS3 mock;
  S3BlobStore store = MakeStore(mock.endpoint());
  REQUIRE(store.ensure_container("testbucket").ok());
  const ObjectRef ref{"testbucket", "tamper/obj.bin"};
  const std::string payload_file = WriteTemp("tamper.bin", "tamper-me");

  PresignOptions options;
  options.method = "PUT";
  options.expires_in_seconds = 900;
  const auto presigned = store.presign_put(ref, options);
  REQUIRE(presigned.ok());
  const std::string url = presigned.value().url;

  //  基线：不改任何东西 → 200（证明下面每一条的 403 都来自那一处改动）
  REQUIRE(RunCurl("PUT", url, {}, payload_file).status == 200);

  struct Tamper {
    const char* name;
    std::string url;
  };
  std::vector<Tamper> tampered;

  //  ① 签名被改一个字符
  {
    std::string bad = url;
    const auto pos = bad.find("X-Amz-Signature=") + std::string("X-Amz-Signature=").size();
    bad[pos] = bad[pos] == 'a' ? 'b' : 'a';
    tampered.push_back({"改签名一个字符", bad});
  }
  //  ② 路径里的 key 被改（签名覆盖 canonical URI）
  {
    std::string bad = url;
    bad.replace(bad.find("tamper/obj.bin"), std::string("tamper/obj.bin").size(),
                "tamper/other.bin");
    tampered.push_back({"改 key（签名覆盖 URI）", bad});
  }
  //  ③ 桶名被改
  {
    std::string bad = url;
    bad.replace(bad.find("/testbucket/"), std::string("/testbucket/").size(), "/otherbucket/");
    tampered.push_back({"改 bucket", bad});
  }
  //  ④ 有效期被改（签名覆盖 X-Amz-Expires）
  tampered.push_back({"改 X-Amz-Expires", ReplaceParam(url, "X-Amz-Expires", "86400")});
  //  ⑤ 凭证被改（签名覆盖 X-Amz-Credential）
  tampered.push_back({"改 X-Amz-Credential 的 access key",
                      ReplaceParam(url, "X-Amz-Credential", "AKIDOTHER%2F20231114%2Fus-east-1%2Fs3%2Faws4_request")});
  //  ⑥ SignedHeaders 被改（签名覆盖 X-Amz-SignedHeaders）
  tampered.push_back({"改 X-Amz-SignedHeaders", ReplaceParam(url, "X-Amz-SignedHeaders", "host%3Bx-amz-date")});

  for (const auto& item : tampered) {
    INFO("篡改项：" << item.name);
    const auto result = RunCurl("PUT", item.url, {}, payload_file);
    INFO("URL: " << item.url);
    INFO("状态码: " << result.status);
    REQUIRE(result.status == 403);
  }

  //  ⑦ 过期：用"过去的日期 + 1 秒有效期"签一个 URL → 必须 403（且不能是签名不匹配）
  {
    S3Options options_expired;
    options_expired.endpoint = mock.endpoint();
    options_expired.region = "us-east-1";
    options_expired.verify_tls = false;
    options_expired.scheme = "http";
    options_expired.credentials = AwsCredentials{kAccessKey, kSecretKey, ""};
    fss::ManualClock past_clock{NowSeconds() - 7200};  // 2 小时前
    S3BlobStore expired_store(options_expired, past_clock);
    PresignOptions expired_options;
    expired_options.method = "PUT";
    expired_options.expires_in_seconds = 1;
    const auto expired = expired_store.presign_put(ref, expired_options);
    REQUIRE(expired.ok());
    const auto result = RunCurl("PUT", expired.value().url, {}, payload_file);
    INFO("过期 URL 状态码: " << result.status);
    REQUIRE(result.status == 403);
  }

  std::error_code cleanup;
  std::filesystem::remove(payload_file, cleanup);
}

TEST_CASE("★ C5.2 头部签名（数据面口径）同样被独立实现验过",
          "[phase5][integration][s3][c5.2]") {
  fss::test::MockS3 mock;
  S3BlobStore store = MakeStore(mock.endpoint());
  REQUIRE(store.ensure_container("testbucket").ok());
  const std::string payload = "header-auth-body";
  const std::string payload_file = WriteTemp("header.bin", payload);

  //  数据面口径（与 curl 实测一致）：payload 哈希 = sha256(body)，
  //  且 `x-amz-content-sha256` 必须参与签名（服务端据此校验体未被篡改）
  auto options = store.options();
  fss::infra::SigV4Request request;
  request.method = "PUT";
  request.host = mock.endpoint();
  request.path = "/testbucket/header/obj.bin";
  const std::string payload_hash = fss::crypto::Sha256Hex(payload);
  request.headers = {{"host", request.host},
                     {"x-amz-date", "20231114T221320Z"},
                     {"x-amz-content-sha256", payload_hash}};
  request.payload_sha256_hex = request.headers[2].second;
  const auto signed_request = store.signer().SignRequest(request, "20231114T221320Z");
  REQUIRE(signed_request.ok());
  INFO(signed_request.value().authorization);

  const auto put = RunCurl("PUT", "http://" + mock.endpoint() + request.path,
                           {"Authorization: " + signed_request.value().authorization,
                            "x-amz-date: 20231114T221320Z",
                            "x-amz-content-sha256: " + payload_hash,
                            "Content-Type: application/octet-stream"},
                           payload_file);
  INFO("头部签名 PUT 状态码: " << put.status);
  REQUIRE(put.status == 200);

  //  反例：把 `x-amz-content-sha256` 改成别的值（体没变）→ 签名与头不一致 → 403
  const auto bad = RunCurl("PUT", "http://" + mock.endpoint() + request.path,
                           {"Authorization: " + signed_request.value().authorization,
                            "x-amz-date: 20231114T221320Z",
                            "x-amz-content-sha256: " + std::string(64, '0')},
                           payload_file);
  INFO("篡改 x-amz-content-sha256 后状态码: " << bad.status);
  REQUIRE(bad.status == 403);

  std::error_code cleanup;
  std::filesystem::remove(payload_file, cleanup);
}
