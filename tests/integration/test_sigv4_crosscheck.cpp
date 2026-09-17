// =============================================================================
//  C5.1 / C5.5（独立实现对拍）：我们签的东西，**别人**也能算出同一个签名
// =============================================================================
//  为什么必须有这一层：`tests/unit/test_sigv4_signer.cpp` 的期望值全部由"我们自己的规则"
//  推出 —— 如果规则本身理解错了（例如 canonical URI 该不该二次编码、query 怎么排序、
//  头值怎么压缩空白），单元测试会**一起错**，全绿也没意义（P1-D07 的教训）。
//
//  两条**独立**路径：
//    ① **libcurl 的 `--aws-sigv4`**（curl 7.81，第三方实现）：它按自己的代码签同一个请求，
//       我们逐字节比对 `Authorization` 头。为公平比对，我们把"curl 实际签的那组输入"
//       （方法/路径/查询/头/日期/payload 哈希）原样喂给我们的签名器。
//    ② **Python 参照实现**（`tests/tools/sigv4_reference.py`，只用 hmac/hashlib 重写）：
//       同一组向量两边各算一次，签名必须相同 —— 这条还顺带覆盖 C5.5 的编码边界。
//
//  ⚠️ 实测到的 **libcurl 行为偏差**（记录在 `docs/test-evidence/phase5.md`，我们不跟随）：
//    · 它把 `host` 签成 **URL 主机名（去掉端口）**，而请求里发出去的 `Host:` 是带端口的；
//      真实 S3/MinIO 是按收到的 `Host` 重算的，所以我们的实现保留端口（并显式对拍时
//      把 curl 用的那个 host 传进去）。
//    · GET 的 payload 哈希用空串的 SHA-256；带体 PUT 用 `sha256(body)`，且**不**发
//      `x-amz-content-sha256` 头。
// =============================================================================
#include <catch2/catch.hpp>

#include "http_fixture.h"

#include "common/crypto/crypto.h"
#include "common/json/json.h"
#include "infra/blob/s3/sigv4_signer.h"
#include "temp_dir.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using fss::infra::AwsCredentials;
using fss::infra::MakeSigV4Request;
using fss::infra::SigV4Options;
using fss::infra::SigV4Request;
using fss::infra::SigV4Signer;

constexpr std::string_view kAmzDate = "20130524T000000Z";
constexpr const char* kAccessKey = "AKIDEXAMPLE";
constexpr const char* kSecretKey = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";

SigV4Options Options() {
  SigV4Options options;
  options.region = "us-east-1";
  options.service = "s3";
  options.force_path_style = true;
  options.endpoint = "s3.amazonaws.com";
  options.scheme = "https";
  return options;
}

bool ToolAvailable(const std::string& command) {
  std::FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) return false;
  return ::pclose(pipe) == 0;
}

std::string RunCommand(const std::string& command) {
  std::FILE* pipe = ::popen(command.c_str(), "r");
  REQUIRE(pipe != nullptr);
  std::string out;
  char buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    out.append(buffer, read);
  }
  const int status = ::pclose(pipe);
  INFO("命令退出码 " << status << "：" << command);
  REQUIRE(status == 0);
  return out;
}

//  ---- 只接受一次连接的迷你监听器（用于捕获 curl 真正发出去的请求）----
int ListenOnce(int* out_port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  int on = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  REQUIRE(::listen(fd, 1) == 0);
  socklen_t len = sizeof(addr);
  REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
  *out_port = ntohs(addr.sin_port);
  return fd;
}

struct CapturedRequest {
  std::string method;
  std::string target;
  std::string host;
  std::string amz_date;
  std::string authorization;
  std::string body;
  std::vector<std::pair<std::string, std::string>> signed_headers;  // 按 Authorization 声明
};

CapturedRequest AcceptAndCapture(int listen_fd) {
  pollfd waiting{listen_fd, POLLIN, 0};
  REQUIRE(::poll(&waiting, 1, 5000) > 0);
  const int client = ::accept(listen_fd, nullptr, nullptr);
  REQUIRE(client >= 0);

  std::string raw;
  char buffer[4096];
  bool have_head = false;
  std::size_t header_end = 0;
  while (true) {
    pollfd readable{client, POLLIN, 0};
    if (::poll(&readable, 1, 5000) <= 0) break;
    const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
    if (n <= 0) break;
    raw.append(buffer, static_cast<std::size_t>(n));
    if (!have_head) {
      header_end = raw.find("\r\n\r\n");
      if (header_end != std::string::npos) have_head = true;
    }
    if (have_head) {
      const std::size_t body_have = raw.size() - (header_end + 4);
      const auto length_pos = raw.find("Content-Length: ");
      std::size_t need = 0;
      if (length_pos != std::string::npos && length_pos < header_end) {
        need = static_cast<std::size_t>(
            std::stoul(raw.substr(length_pos + 16, raw.find("\r\n", length_pos) - length_pos - 16)));
      }
      if (body_have >= need) break;
    }
  }
  ::close(client);
  ::close(listen_fd);

  CapturedRequest captured;
  std::istringstream stream(raw);
  std::string line;
  std::getline(stream, line);
  {
    std::istringstream first(line);
    std::string version;
    first >> captured.method >> captured.target >> version;
  }
  while (std::getline(stream, line) && line != "\r") {
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ')) value.erase(value.begin());
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name == "host") captured.host = value;
    if (name == "x-amz-date") captured.amz_date = value;
    if (name == "authorization") captured.authorization = value;
  }
  if (have_head) captured.body = raw.substr(header_end + 4);
  return captured;
}

//  从 `Authorization` 头里取出 "SignedHeaders=a;b;c"
//  ★ `signed_host` 必须显式传入：wire 上的 `Host:` 带端口，而 curl 签名时用的是**去端口**的主机名
//    （见本文件头部记录的偏差）。如果这里用 captured.host 去签，canonical host 就多一个 `:port`，
//    两边必然不一致 —— 那不是被测实现的缺陷，而是对拍脚手架把输入搞错了。
std::vector<std::pair<std::string, std::string>> DeclaredHeaders(
    const CapturedRequest& captured, const std::string& signed_host) {
  const auto pos = captured.authorization.find("SignedHeaders=");
  REQUIRE(pos != std::string::npos);
  const auto begin = pos + std::string("SignedHeaders=").size();
  const auto end = captured.authorization.find(',', begin);
  const std::string names = captured.authorization.substr(begin, end - begin);
  std::vector<std::pair<std::string, std::string>> headers;
  std::istringstream stream(names);
  std::string name;
  while (std::getline(stream, name, ';')) {
    if (name == "host") {
      headers.emplace_back("host", signed_host);
    } else if (name == "x-amz-date") {
      headers.emplace_back("x-amz-date", captured.amz_date);
    } else if (name == "x-amz-content-sha256") {
      headers.emplace_back("x-amz-content-sha256", fss::crypto::Sha256Hex(captured.body));
    } else {
      FAIL("对拍遇到未覆盖的签名头：" << name);
    }
  }
  return headers;
}

std::string SignatureOf(const std::string& authorization) {
  const auto pos = authorization.rfind("Signature=");
  REQUIRE(pos != std::string::npos);
  return authorization.substr(pos + std::string("Signature=").size());
}

//  用 Python 参照实现算签名（把向量写成 JSON，让 Python 自己解析）
std::string PythonSignature(const std::filesystem::path& vector_path) {
  const std::string script =
      std::string(FSS_REPO_ROOT) + "/tests/tools/sigv4_reference.py";
  REQUIRE(std::filesystem::exists(script));
  const std::string output =
      RunCommand("python3 " + script + " --sign-vector " + vector_path.string() + " 2>/dev/null");
  const auto parsed = fss::json::ParseObject(output);
  REQUIRE(parsed.ok());
  return parsed.value()["signature"].get<std::string>();
}

std::string PythonPresignedUrl(const std::filesystem::path& vector_path) {
  const std::string script =
      std::string(FSS_REPO_ROOT) + "/tests/tools/sigv4_reference.py";
  const std::string output = RunCommand("python3 " + script + " --presign-vector " +
                                        vector_path.string() + " 2>/dev/null");
  const auto parsed = fss::json::ParseObject(output);
  REQUIRE(parsed.ok());
  return parsed.value()["url"].get<std::string>();
}

fss::json::Value VectorToJson(const SigV4Request& request, std::string_view amz_date) {
  fss::json::Value vector = fss::json::Value::object();
  vector["method"] = request.method;
  vector["host"] = request.host;
  vector["path"] = request.path;
  fss::json::Value query = fss::json::Value::array();
  for (const auto& [key, value] : request.query) query.push_back(fss::json::Value::array({key, value}));
  vector["query"] = std::move(query);
  fss::json::Value headers = fss::json::Value::array();
  for (const auto& [key, value] : request.headers) {
    headers.push_back(fss::json::Value::array({key, value}));
  }
  vector["headers"] = std::move(headers);
  vector["payload_sha256_hex"] = request.payload_sha256_hex;
  vector["access_key"] = kAccessKey;
  vector["secret_key"] = kSecretKey;
  vector["region"] = "us-east-1";
  vector["service"] = "s3";
  vector["amz_date"] = std::string(amz_date);
  return vector;
}

}  // namespace

TEST_CASE("★ C5.1（独立实现①）libcurl `--aws-sigv4` 对拍：同一请求必须得到同一签名",
          "[phase5][integration][sigv4][c5.1]") {
  REQUIRE(ToolAvailable("curl --version >/dev/null 2>&1"));
  //  curl 7.75+ 才有 `--aws-sigv4`；缺失时**不能静默跳过**（那会让这条证据消失）
  REQUIRE(ToolAvailable("curl --help all 2>/dev/null | grep -q aws-sigv4"));

  const std::string credentials =
      std::string(kAccessKey) + ":" + std::string(kSecretKey);

  struct Case {
    const char* name;
    std::string url_suffix;      // 追加在 http://127.0.0.1:<port> 之后
    std::vector<std::string> extra_args;
    bool has_body;
  };
  //  ⚠️ query **必须已排序**：实测 libcurl 7.81 的 `--aws-sigv4` **不排序** canonical query
  //    （AWS 规范要求字典序）。所以这里只对"curl 自己也算对"的输入做等价比对；
  //    这条偏差本身记录在 `docs/test-evidence/phase5.md`（连同复现命令），
  //    并且我们**不跟随**它 —— 我们的签名器始终排序（单元测试有正例断言）。
  const std::vector<Case> cases = {
      {"GET 带已排序 query", "/bucket/test.txt?max-keys=2&prefix=J", {"-X", "GET"}, false},
      {"GET 无 query", "/bucket/plain.txt", {"-X", "GET"}, false},
      {"PUT 带体（payload 哈希 = sha256(body)）", "/bucket/key.bin",
       {"-X", "PUT", "--data-binary", "hello-body"}, true},
  };

  for (const auto& test_case : cases) {
    INFO("对拍用例：" << test_case.name);
    int port = 0;
    const int listener = ListenOnce(&port);
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + test_case.url_suffix;

    std::string command = "curl -sS -o /dev/null --aws-sigv4 'aws:amz:us-east-1:s3' -u '" +
                          credentials + "' ";
    for (const auto& arg : test_case.extra_args) command += "'" + arg + "' ";
    command += "'" + url + "' 2>/dev/null";
    std::FILE* pipe = ::popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    const CapturedRequest captured = AcceptAndCapture(listener);
    ::pclose(pipe);  // curl 会因"空响应"报错，这里只关心它发出去的请求

    //  ① 方法/路径/查询必须与分析的一致（否则我们比的不是同一个请求）
    const auto question = captured.target.find('?');
    const std::string path = captured.target.substr(0, question);
    std::vector<std::pair<std::string, std::string>> query;
    if (question != std::string::npos) {
      std::istringstream stream(captured.target.substr(question + 1));
      std::string pair;
      while (std::getline(stream, pair, '&')) {
        const auto equals = pair.find('=');
        query.emplace_back(pair.substr(0, equals),
                           equals == std::string::npos ? "" : pair.substr(equals + 1));
      }
    }

    //  ② ⚠️ 实测：curl 7.81 用 **URL 主机名（去端口）** 签名，而请求里发出的 `Host:` 带端口。
    //    真实 S3/MinIO 按收到的 `Host` 重算，所以我们的签名器**保留端口**（不跟随 curl）；
    //    对拍时显式把 curl 用的那个 host 传进去，保证比的是"同一组输入 + 同一个算法"。
    const std::string signed_host = "127.0.0.1";
    SigV4Request request;
    request.method = captured.method;
    request.host = signed_host;
    request.path = path;
    request.query = query;
    request.headers = DeclaredHeaders(captured, signed_host);
    REQUIRE(request.headers.size() >= 2);  // host + x-amz-date 必须都在
    //  curl 的口径：GET 用空串哈希；带体用 sha256(body)
    request.payload_sha256_hex = captured.body.empty() ? fss::crypto::Sha256Hex("")
                                                       : fss::crypto::Sha256Hex(captured.body);

    SigV4Signer signer(AwsCredentials{kAccessKey, kSecretKey, ""}, Options());
    const auto ours = signer.SignRequest(request, captured.amz_date);
    REQUIRE(ours.ok());
    INFO("我们: " << ours.value().authorization);
    INFO("curl: " << captured.authorization);

    //  ③ 逐字节比对：签名、scope、SignedHeaders
    REQUIRE(ours.value().signature == SignatureOf(captured.authorization));
    REQUIRE(ours.value().authorization == captured.authorization);
  }
}

TEST_CASE("★ C5.1/C5.5（独立实现②）Python 参照实现对拍：含编码边界的向量矩阵",
          "[phase5][integration][sigv4][c5.1][c5.5]") {
  REQUIRE(ToolAvailable("python3 -c 'import hashlib,hmac' >/dev/null 2>&1"));
  fss::test::TempDir dir{"sigv4_vectors"};
  SigV4Signer signer(AwsCredentials{kAccessKey, kSecretKey, ""}, Options());

  struct Vector {
    const char* name;
    std::string method;
    std::string bucket;
    std::string key;
    std::vector<std::pair<std::string, std::string>> query;
    std::vector<std::pair<std::string, std::string>> extra_headers;
    std::string payload;
  };
  const std::string empty_hash = fss::crypto::Sha256Hex("");
  const std::vector<Vector> vectors = {
      {"path-style GET + 未排序 query", "GET", "examplebucket", "test.txt",
       {{"prefix", "J"}, {"max-keys", "2"}}, {}, empty_hash},
      {"payload = UNSIGNED-PAYLOAD", "PUT", "examplebucket", "dir/obj.bin", {},
       {{"content-type", "application/octet-stream"}}, "UNSIGNED-PAYLOAD"},
      {"key 含空格与 `+`", "GET", "examplebucket", "a b+c.txt", {}, {}, empty_hash},
      {"key 含 `~`（unreserved，不编码）", "GET", "examplebucket", "dir/~user/file.txt", {},
       {}, empty_hash},
      {"key 含 `%` 与 `=`", "GET", "examplebucket", "100%=done.txt", {}, {}, empty_hash},
      {"key 含非 ASCII（UTF-8 逐字节）", "GET", "examplebucket", "目录/文件.txt", {}, {},
       empty_hash},
      {"key 含多层 `/`", "GET", "examplebucket", "a/b/c/d.txt", {}, {}, empty_hash},
      {"query 值含空格与 `/`", "GET", "examplebucket", "k.txt",
       {{"prefix", "a b/c"}, {"response-content-type", "text/plain"}}, {}, empty_hash},
      {"头值含多余空白（必须压缩）", "GET", "examplebucket", "k.txt", {},
       {{"x-amz-meta-note", "  a   b  "}}, empty_hash},
      {"virtual-host 形态", "GET", "", "dir/obj.bin", {}, {}, empty_hash},
  };

  for (const auto& vector : vectors) {
    INFO("向量：" << vector.name);
    auto options = Options();
    if (vector.bucket.empty()) options.force_path_style = false;
    auto request = MakeSigV4Request(options, vector.method, vector.bucket, vector.key,
                                    vector.query, vector.extra_headers, vector.payload, kAmzDate);
    const auto ours = signer.SignRequest(request, kAmzDate);
    REQUIRE(ours.ok());

    const auto path = dir.child("vector.json");
    {
      std::ofstream out(path);
      REQUIRE(out.good());
      out << fss::json::Dump(VectorToJson(request, kAmzDate));
    }
    const std::string theirs = PythonSignature(path);
    INFO("我们: " << ours.value().signature);
    INFO("Python: " << theirs);
    REQUIRE(ours.value().signature == theirs);
  }

  SECTION("预签名 URL：两边产出的 URL 必须逐字节相同（C5.2 的准备）") {
    auto request = MakeSigV4Request(Options(), "PUT", "examplebucket", "dir/obj.bin", {},
                                    {}, "UNSIGNED-PAYLOAD", kAmzDate);
    request.headers = {{"host", request.host}};
    const auto ours = signer.PresignUrl(request, kAmzDate, 3600);
    REQUIRE(ours.ok());

    const auto path = dir.child("presign.json");
    {
      auto vector = VectorToJson(request, kAmzDate);
      vector["scheme"] = "https";
      vector["expires"] = 3600;
      std::ofstream out(path);
      REQUIRE(out.good());
      out << fss::json::Dump(vector);
    }
    const std::string theirs = PythonPresignedUrl(path);
    INFO("我们:   " << ours.value());
    INFO("Python: " << theirs);
    REQUIRE(ours.value() == theirs);
  }
}
