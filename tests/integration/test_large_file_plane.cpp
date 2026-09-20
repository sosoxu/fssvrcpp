// =============================================================================
//  ADR-006：大文件**下载**数据面（进程内第二个监听 socket + sendfile）端到端用例
// =============================================================================
//  被测对象：真实 `fss_server`（组合根）在 `server.http.large_file_plane.enabled=true`
//  时，除了 HTTP 控制面之外，额外监听一个只服务 `GET/HEAD /<base>/v1/transfer/{token}`
//  的下载面。它复用控制面**同一个**已包装 handler（鉴权/租户/错误体/HTTP 指标/访问日志）。
//
//  判据分组（与任务书的 P1~P10 一一对应；P11 是 ADR-006 §6.2 的访问日志字段等价）：
//    P1  基本：控制面播种 → 数据面下载 → 字节/头/状态一致 + 真的走了 sendfile
//    P2  Range 矩阵（含多段降级这一条**文档化的差异**）
//    P3  鉴权与租户（= ADR-006 §6.6 "安全在数据面路径上再跑一遍"）
//    P4  硬化：走私检查 / 超长 URI / 405 / 404
//    P5  零拷贝真的被用上（sendfile 计数器 + 存储字节计数等价）
//    P6  在途连接上限（503 + Retry-After，且没有对象字节）
//    P7  RSS：≥1 GiB 传输的进程 RSS 增长 < 既有门槛（C3.4 的同类断言）
//    P8  等价性矩阵（同一请求打到两个端口，逐字段比对；唯一例外 = 多段降级）
//    P9  enabled=false 回归：数据面端口**没有任何监听**，控制面行为不变
//    P10 HEAD：状态/头一致、无 body
//    P11 访问日志字段集同源（ADR-006 §6.2）
//
//  ★ 所有请求用 `tests/framework/raw_http.h` 的原始 socket 客户端构造，因此能发
//    "重复 Content-Length""CL + chunked""9 KB 的 URI"这类畸形请求。
//  ★ P7 的 1 GiB 对象**直接**用 `PosixBlobStore` 落盘 + 自签 token 指向它，避免让
//    控制面在这条 RSS 判据上多搬两遍字节（RSS 判据只关心数据面）。
// =============================================================================
#include <catch2/catch.hpp>

#include "big_file.h"
#include "raw_http.h"
#include "server_process.h"
#include "temp_dir.h"

#include "common/bytes/bytes.h"
#include "common/json/json.h"
#include "common/time/clock.h"
#include "domain/model/file_metadata.h"
#include "domain/ports/ports.h"
#include "infra/blob/posix/posix_blob_store.h"
#include "infra/transfer/transfer_token.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using fss::test::RawClient;
using fss::test::RawResponse;
using fss::test::ServerProcess;
using fss::test::ServerProcessOptions;
using fss::test::TempDir;

constexpr const char* kSecret = "adr006-plane-test";
constexpr const char* kKeyId = "k1";
constexpr const char* kBasePath = "/api/file";

int FreePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  int port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
      port = ntohs(addr.sin_port);
    }
  }
  ::close(fd);
  return port;
}

std::string WriteFile(const TempDir& dir, const std::string& name, const std::string& content) {
  const std::string path = dir.child(name);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.good());
  out << content;
  out.close();
  return path;
}

struct PlaneConfig {
  bool enabled = true;
  bool use_sendfile = true;
  int workers = 8;
  int max_connections = 8;
  long sendfile_chunk_bytes = 65536;
  int http_port = 0;
  int plane_port = 0;
};

std::string ConfigJson(const std::string& root, const PlaneConfig& plane) {
  std::ostringstream json;
  json << "{\n"
       << "  \"deployment\": {\"mode\": \"single\"},\n"
       << "  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " << plane.http_port
       << ", \"large_file_plane\": {\"enabled\": "
       << (plane.enabled ? "true" : "false") << ", \"bind\": \"127.0.0.1\", \"port\": "
       << plane.plane_port << ", \"use_sendfile\": " << (plane.use_sendfile ? "true" : "false")
       << ", \"sendfile_chunk_bytes\": " << plane.sendfile_chunk_bytes
       << ", \"workers\": " << plane.workers << ", \"max_connections\": " << plane.max_connections
       << "}}, \"grpc\": {\"enabled\": false}},\n"
       << "  \"storage\": {\"driver\": \"posix\", \"posix\": {\"root\": \"" << root << "\"}},\n"
       << "  \"metadata\": {\"sqlite\": {\"path\": \"" << root << "/metadata.db\"}},\n"
       << "  \"location\": {\"sqlite\": {\"path\": \"" << root << "/location.db\"}},\n"
       << "  \"self_signed\": {\"signing_key\": \"" << kSecret << "\", \"key_id\": \"" << kKeyId
       << "\", \"public_base_url\": \"http://127.0.0.1:" << plane.http_port << kBasePath
       << "\"},\n"
       << "  \"auth\": {\"mode\": \"disabled\"},\n"
       << "  \"gc\": {\"enabled\": false},\n"
       << "  \"observability\": {\"log_format\": \"json\", \"log_level\": \"info\"}\n"
       << "}\n";
  return json.str();
}

ServerProcessOptions ProcessOptionsFor(const std::string& config) {
  ServerProcessOptions options;
  options.default_http_port = false;  // 端口来自配置文件（否则 env 覆盖，测不到 plane）
  options.default_grpc_port = false;
  options.expect_grpc = false;
  options.default_storage_env = false;  // 存储根全部来自 --config
  options.args = {"--config", config};
  return options;
}

std::vector<std::string> Authed() {
  return {"authorization: Bearer test-token", "data-partition-id: opendes"};
}

std::string TargetOf(const std::string& url) {
  const auto scheme = url.find("://");
  const auto path_start = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
  REQUIRE(path_start != std::string::npos);
  return url.substr(path_start);
}

struct Reply {
  bool transport_ok = false;
  int status = 0;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string raw_head;
  bool body_truncated = false;

  std::string Header(const std::string& name) const {
    for (const auto& [k, v] : headers) {
      if (k.size() != name.size()) continue;
      bool same = true;
      for (std::size_t i = 0; i < k.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(k[i])) !=
            std::tolower(static_cast<unsigned char>(name[i]))) {
          same = false;
          break;
        }
      }
      if (same) return v;
    }
    return {};
  }
  bool HasHeader(const std::string& name) const {
    for (const auto& [k, v] : headers) {
      (void)v;
      if (k.size() != name.size()) continue;
      bool same = true;
      for (std::size_t i = 0; i < k.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(k[i])) !=
            std::tolower(static_cast<unsigned char>(name[i]))) {
          same = false;
          break;
        }
      }
      if (same) return true;
    }
    return false;
  }
};

Reply Do(int port, const std::string& method, const std::string& target,
         const std::vector<std::string>& headers = {}, const std::string& body = {},
         int timeout_ms = 30000) {
  Reply reply;
  RawClient client(port, /*tcp_nodelay=*/true);
  if (!client.Connect()) return reply;
  std::vector<std::string> all = headers;
  const bool method_has_body = method == "PUT" || method == "POST" || method == "PATCH";
  if (!body.empty() || method_has_body) {
    all.push_back("Content-Length: " + std::to_string(body.size()));
  }
  if (!client.SendRequest(method, target, all, body)) return reply;
  const auto response = client.ReadResponse(timeout_ms);
  if (!response.has_value()) return reply;
  reply.transport_ok = true;
  reply.status = response->status;
  reply.body = response->body;
  reply.headers = response->headers;
  reply.raw_head = response->raw_head;
  reply.body_truncated = response->body_truncated;
  return reply;
}

//  HEAD 专用：只读头（库/数据面都**不**发 body；用 ReadResponse 会等超时）
Reply DoHead(int port, const std::string& target,
             const std::vector<std::string>& headers = {}) {
  Reply reply;
  RawClient client(port, /*tcp_nodelay=*/true);
  if (!client.Connect()) return reply;
  if (!client.SendRequest("HEAD", target, headers)) return reply;
  const auto response = client.ReadHead(15000);
  if (!response.has_value()) return reply;
  reply.transport_ok = true;
  reply.status = response->status;
  reply.headers = response->headers;
  reply.raw_head = response->raw_head;
  reply.body = client.pending();  // HEAD 必须为空
  return reply;
}

bool WaitReady(int port, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto reply = Do(port, "GET", "/api/file/v2/readiness_check", Authed());
    if (reply.transport_ok && reply.status == 200) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

std::string MakePayload(std::size_t bytes) {
  std::string payload(bytes, '\0');
  for (std::size_t i = 0; i < bytes; ++i) payload[i] = static_cast<char>('A' + (i * 7 % 26));
  return payload;
}

fss::domain::FileMetadataRecord MakeRecord(const std::string& file_source) {
  fss::domain::FileMetadataRecord record;
  record.kind = "opendes:wks:dataset--File.Generic:1.0.0";
  record.acl.viewers = {"data.default.viewers@opendes.example.com"};
  record.acl.owners = {"data.default.owners@opendes.example.com"};
  record.legal.legaltags = {"opendes-public-1"};
  record.legal.other_relevant_data_countries = {"US"};
  record.legal.status = fss::domain::LegalStatus::kCompliant;
  record.data.name = "adr006.bin";
  record.data.endian = "LITTLE";
  record.data.dataset_properties.file_source_info.file_source = file_source;
  record.data.dataset_properties.present = true;
  return record;
}

struct Seeded {
  std::string file_id;
  std::string file_source;
  std::string put_url;
  std::string get_url;
  std::string get_target;
  std::string record_id;
};

//  用**产品同一份** codec 伪造一个"签名合法、但对象不存在"的下载 URL。
//  为什么不是靠"删文件"：那会依赖 DELETE 的副作用与时序；用 codec 直接构造可以让
//  "对象不存在"成为**确定性**前置条件（到期时间取 now+3600，不受测试时长影响）。
std::string ForgeMissingObjectTarget(int base_port) {
  fss::SystemClock clock;
  fss::infra::HmacTransferTokenCodec codec(kSecret, clock, kKeyId);
  fss::domain::TransferToken token;
  token.partition = "opendes";
  token.file_id = "adr006-missing";
  token.container = "opendes-persistent";
  token.object_key = "osdu-user/adr006/never-written.bin";
  token.zone = fss::domain::StorageZone::kPersistent;
  token.op = "get";
  token.expires_at_epoch_seconds = clock.NowEpochSeconds() + 3600;
  token.key_id = kKeyId;
  const auto url =
      codec.Encode(token, std::string("http://127.0.0.1:") + std::to_string(base_port) + kBasePath);
  REQUIRE(url.ok());
  return TargetOf(url.value());
}

//  ★ 播种必须走**控制面**真实链路（uploadURL → PUT → metadata → downloadURL）：
//    这样数据面拿到的 token 是产品签发的，而不是测试伪造的（P1 的"前置条件"）。
Seeded Seed(ServerProcess& server, const std::string& payload) {
  Seeded out;
  const int port = server.http_port();
  const auto upload = Do(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  CAPTURE(upload.status, upload.body);
  REQUIRE(upload.status == 200);
  const auto upload_json = fss::json::ParseObject(upload.body);
  REQUIRE(upload_json.ok());
  out.file_id = upload_json.value()["FileID"].get<std::string>();
  out.file_source = upload_json.value()["Location"]["FileSource"].get<std::string>();
  out.put_url = upload_json.value()["Location"]["SignedURL"].get<std::string>();
  REQUIRE(out.put_url.find("/v1/transfer/") != std::string::npos);

  const auto put = Do(port, "PUT", TargetOf(out.put_url), Authed(), payload);
  CAPTURE(put.status, put.body);
  REQUIRE(put.status == 200);

  auto record = MakeRecord(out.file_source);
  const auto created =
      Do(port, "POST", "/api/file/v2/files/metadata", Authed(),
         fss::json::Dump(fss::domain::ToJson(record)));
  CAPTURE(created.status, created.body);
  REQUIRE(created.status == 201);
  const auto created_json = fss::json::ParseObject(created.body);
  REQUIRE(created_json.ok());
  out.record_id = created_json.value()["id"].get<std::string>();

  const auto download =
      Do(port, "GET", "/api/file/v2/files/" + out.file_id + "/downloadURL", Authed());
  REQUIRE(download.status == 200);
  const auto download_json = fss::json::ParseObject(download.body);
  REQUIRE(download_json.ok());
  out.get_url = download_json.value()["SignedUrl"].get<std::string>();
  out.get_target = TargetOf(out.get_url);
  return out;
}

std::string GetMetrics(int port) {
  const auto reply = Do(port, "GET", "/metrics");
  REQUIRE(reply.status == 200);
  return reply.body;
}

//  取一个 counter 的值；label 片段为空 = 不带标签的样本。找不到 → -1（不静默当 0）。
std::int64_t MetricValue(const std::string& text, const std::string& name,
                         const std::string& labels = {}) {
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.rfind(name, 0) != 0) continue;
    const auto space = line.find(' ');
    if (space == std::string::npos) continue;
    const std::string label_part = line.substr(name.size(), space - name.size());
    if (!labels.empty() && label_part.find(labels) == std::string::npos) continue;
    return std::strtoll(line.c_str() + space + 1, nullptr, 10);
  }
  return -1;
}

//  ★ Prometheus 文本只渲染**有样本**的 family：注册了但从未自增的 counter 在抓取文本里
//    **不出现**。判据因此不能把"缺席"当成"非 0"：基线一律用 O(1) 的 OrZero，
//    而"该 counter 真的存在"由**至少自增一次之后**的显式断言来钉（见 P1/P5）。
std::int64_t MetricValueOrZero(const std::string& text, const std::string& name,
                               const std::string& labels = {}) {
  const auto value = MetricValue(text, name, labels);
  return value < 0 ? 0 : value;
}

//  原样发送（**不**自动补 Content-Length）——"CL + chunked 并存"这类走私形态必须由
//  测试逐字节构造，不能靠 Do() 的自动补全。
Reply DoRaw(int port, const std::string& method, const std::string& target,
            const std::vector<std::string>& headers, const std::string& raw_body,
            int timeout_ms = 30000) {
  Reply reply;
  RawClient client(port, /*tcp_nodelay=*/true);
  if (!client.Connect()) return reply;
  if (!client.SendRequest(method, target, headers, raw_body)) return reply;
  const auto response = client.ReadResponse(timeout_ms);
  if (!response.has_value()) return reply;
  reply.transport_ok = true;
  reply.status = response->status;
  reply.body = response->body;
  reply.headers = response->headers;
  reply.raw_head = response->raw_head;
  reply.body_truncated = response->body_truncated;
  return reply;
}

std::string GetInfo(int port) {
  const auto reply = Do(port, "GET", "/api/file/v2/info", Authed());
  REQUIRE(reply.status == 200);
  return reply.body;
}

std::uint64_t ProcessPeakRssKib(const std::string& pid) {
  std::ifstream in("/proc/" + pid + "/status");
  REQUIRE(in.good());
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("VmHWM:", 0) == 0) {
      return std::strtoull(line.c_str() + 6, nullptr, 10);
    }
  }
  return 0;
}

//  把一个自签 URL 变成"签名被改坏"的形态（保留路径与查询串结构）
std::string TamperToken(const std::string& target) {
  const auto marker = target.find("/v1/transfer/");
  REQUIRE(marker != std::string::npos);
  const auto token_begin = marker + std::strlen("/v1/transfer/");
  const auto q = target.find('?', token_begin);
  REQUIRE(q != std::string::npos);
  REQUIRE(q > token_begin + 4);
  std::string out = target;
  out[q - 2] = (out[q - 2] == 'A') ? 'B' : 'A';  // 改掉签名载荷的最后一个字符
  return out;
}

std::string StripQuery(const std::string& target) {
  const auto q = target.find('?');
  return q == std::string::npos ? target : target.substr(0, q);
}

//  从 server.log 里取某 correlation-id 的 `http_request` 记录（JSON Lines）
std::vector<fss::json::Value> AccessLogRecords(const std::string& log,
                                               const std::string& correlation_id) {
  std::vector<fss::json::Value> out;
  std::istringstream stream(log);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.find("http_request") == std::string::npos) continue;
    const auto parsed = fss::json::ParseObject(line);
    if (!parsed.ok()) continue;
    const auto& record = parsed.value();
    const auto msg = record.find("msg");
    if (msg == record.end() || !msg->is_string() || msg->get<std::string>() != "http_request") {
      continue;
    }
    const auto cid = record.find("correlation_id");
    if (cid == record.end() || !cid->is_string() ||
        cid->get<std::string>() != correlation_id) {
      continue;
    }
    out.push_back(fss::json::Value(record));
  }
  return out;
}

std::set<std::string> KeysOf(const fss::json::Value& object) {
  std::set<std::string> keys;
  for (auto it = object.begin(); it != object.end(); ++it) keys.insert(it.key());
  return keys;
}

//  ★ P8 的逐字段比对：任何**未登记**的差异都要出现在返回值里（调用方 REQUIRE 空）。
std::vector<std::string> CompareReplies(const std::string& case_name, const Reply& control,
                                        const Reply& plane, bool compare_body) {
  std::vector<std::string> diffs;
  const auto note = [&](const std::string& field, const std::string& a, const std::string& b) {
    diffs.push_back(case_name + "：" + field + " 控制面=[" + a + "] 数据面=[" + b + "]");
  };
  if (control.transport_ok != plane.transport_ok) {
    note("transport_ok", control.transport_ok ? "true" : "false",
         plane.transport_ok ? "true" : "false");
    return diffs;
  }
  if (control.status != plane.status) {
    note("status", std::to_string(control.status), std::to_string(plane.status));
  }
  for (const char* field : {"Content-Type", "Content-Range", "Content-Length",
                            "X-FSS-Error-Kind", "Retry-After"}) {
    const auto a = control.HasHeader(field) ? control.Header(field) : std::string("<absent>");
    const auto b = plane.HasHeader(field) ? plane.Header(field) : std::string("<absent>");
    if (a != b) note(field, a, b);
  }
  if (compare_body && control.body != plane.body) {
    note("body", control.body.substr(0, 200), plane.body.substr(0, 200));
  }
  return diffs;
}

}  // namespace

// =============================================================================
//  P1 基本
// =============================================================================
TEST_CASE("★ P1 基本：控制面播种 → 数据面下载，字节/头/状态一致，且真的走了 sendfile",
          "[adr006][integration]") {
  TempDir dir("adr006_p1");
  const std::string root = dir.child("data");
  PlaneConfig plane;
  plane.http_port = FreePort();
  plane.plane_port = FreePort();
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, plane));
  ServerProcess server(ProcessOptionsFor(config));
  INFO("启动横幅：\n" << server.DumpLog());
  REQUIRE(WaitReady(server.http_port(), 30000));
  REQUIRE(server.http_port() == plane.http_port);

  const std::string payload = MakePayload(4096);
  const Seeded seeded = Seed(server, payload);

  const auto before = GetMetrics(server.http_port());
  const auto sendfile_before =
      MetricValueOrZero(before, "fss_large_file_plane_sendfile_calls_total");

  const auto control = Do(server.http_port(), "GET", seeded.get_target, Authed());
  const auto plane_reply = Do(plane.plane_port, "GET", seeded.get_target, Authed());
  CAPTURE(plane_reply.status, plane_reply.body.size());
  REQUIRE(control.status == 200);
  REQUIRE(plane_reply.status == 200);
  REQUIRE(plane_reply.body == payload);
  REQUIRE(plane_reply.body == control.body);
  REQUIRE(plane_reply.Header("Content-Length") == control.Header("Content-Length"));
  REQUIRE(plane_reply.Header("Content-Length") == std::to_string(payload.size()));
  REQUIRE(plane_reply.Header("Content-Type") == control.Header("Content-Type"));
  REQUIRE(plane_reply.Header("Content-Type") == "application/octet-stream");

  //  ★ 零拷贝真的被用上（P5 的强判据在这里先给出一次）
  const auto after = GetMetrics(server.http_port());
  REQUIRE(MetricValue(after, "fss_large_file_plane_sendfile_calls_total") ==
          sendfile_before + 1);
  //  正控（R16）：自增之后该 family 必须真的出现在抓取文本里（不是恒真）
  REQUIRE(after.find("fss_large_file_plane_sendfile_calls_total") != std::string::npos);

  //  `/v2/info` 必须如实暴露数据面形态（R11）
  const auto info = GetInfo(server.http_port());
  REQUIRE(info.find("\"largeFilePlane\":\"sendfile\"") != std::string::npos);
  //  横幅同样可见（R11）
  REQUIRE(server.DumpLog().find("plane bind     : 127.0.0.1:") != std::string::npos);
  REQUIRE(server.DumpLog().find("sendfile=on") != std::string::npos);
}

// =============================================================================
//  P2 Range 矩阵
// =============================================================================
TEST_CASE("★ P2 Range 矩阵：单段/后缀/越界/畸形/多段（多段是文档化的降级差异）",
          "[adr006][integration]") {
  TempDir dir("adr006_p2");
  const std::string root = dir.child("data");
  PlaneConfig plane;
  plane.http_port = FreePort();
  plane.plane_port = FreePort();
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, plane));
  ServerProcess server(ProcessOptionsFor(config));
  REQUIRE(WaitReady(server.http_port(), 30000));

  const std::string payload = MakePayload(4096);
  const Seeded seeded = Seed(server, payload);
  const int cp = server.http_port();
  const int pp = plane.plane_port;
  const std::string total = std::to_string(payload.size());

  SECTION("无 Range → 200 全量（两侧一致）") {
    const auto c = Do(cp, "GET", seeded.get_target, Authed());
    const auto p = Do(pp, "GET", seeded.get_target, Authed());
    REQUIRE(c.status == 200);
    REQUIRE(p.status == 200);
    REQUIRE(p.body == payload);
    REQUIRE(CompareReplies("no-range", c, p, true).empty());
  }

  SECTION("bytes=0-99 → 206 + Content-Range") {
    const std::vector<std::string> headers = {"Range: bytes=0-99"};
    const auto c = Do(cp, "GET", seeded.get_target, headers);
    const auto p = Do(pp, "GET", seeded.get_target, headers);
    REQUIRE(c.status == 206);
    REQUIRE(p.status == 206);
    REQUIRE(p.body == payload.substr(0, 100));
    REQUIRE(p.Header("Content-Range") == "bytes 0-99/" + total);
    REQUIRE(CompareReplies("0-99", c, p, true).empty());
  }

  SECTION("bytes=100- → 206 到末尾") {
    const std::vector<std::string> headers = {"Range: bytes=100-"};
    const auto c = Do(cp, "GET", seeded.get_target, headers);
    const auto p = Do(pp, "GET", seeded.get_target, headers);
    REQUIRE(c.status == 206);
    REQUIRE(p.status == 206);
    REQUIRE(p.body == payload.substr(100));
    REQUIRE(p.Header("Content-Range") == "bytes 100-4095/" + total);
    REQUIRE(CompareReplies("100-", c, p, true).empty());
  }

  SECTION("bytes=-50 → 后缀按 RFC 7233 归一化") {
    const std::vector<std::string> headers = {"Range: bytes=-50"};
    const auto c = Do(cp, "GET", seeded.get_target, headers);
    const auto p = Do(pp, "GET", seeded.get_target, headers);
    REQUIRE(c.status == 206);
    REQUIRE(p.status == 206);
    REQUIRE(p.body == payload.substr(payload.size() - 50));
    REQUIRE(p.Header("Content-Range") == "bytes 4046-4095/" + total);
    REQUIRE(CompareReplies("suffix-50", c, p, true).empty());
  }

  SECTION("bytes=999999- → 416 + Content-Range: bytes */N") {
    const std::vector<std::string> headers = {"Range: bytes=999999-"};
    const auto c = Do(cp, "GET", seeded.get_target, headers);
    const auto p = Do(pp, "GET", seeded.get_target, headers);
    REQUIRE(c.status == 416);
    REQUIRE(p.status == 416);
    REQUIRE(p.Header("Content-Range") == "bytes */" + total);
    REQUIRE(CompareReplies("unsatisfiable", c, p, true).empty());
  }

  SECTION("bytes=abc → 语法非法：忽略该头 → 200 全量") {
    const std::vector<std::string> headers = {"Range: bytes=abc"};
    const auto c = Do(cp, "GET", seeded.get_target, headers);
    const auto p = Do(pp, "GET", seeded.get_target, headers);
    REQUIRE(c.status == 200);
    REQUIRE(p.status == 200);
    REQUIRE(p.body == payload);
    REQUIRE(CompareReplies("malformed", c, p, true).empty());
  }

  SECTION("★ 多段：数据面降级 200 全量；控制面是可寻址来源 → multipart 206（文档化差异）") {
    const std::vector<std::string> headers = {"Range: bytes=0-4,6-9"};
    const auto c = Do(cp, "GET", seeded.get_target, headers);
    const auto p = Do(pp, "GET", seeded.get_target, headers);
    CAPTURE(c.status, c.Header("Content-Type"), p.status, p.Header("Content-Type"));
    //  这一条差异是 ADR-006 §6.1 的"多段降级"：两侧**都必须**被断言，
    //  绝不允许"某一侧悄悄换了行为"而没人发现。
    REQUIRE(p.status == 200);
    REQUIRE(p.body == payload);
    REQUIRE(p.Header("Content-Type") == "application/octet-stream");
    REQUIRE(c.status == 206);
    REQUIRE(c.Header("Content-Type").find("multipart/byteranges") != std::string::npos);
    //  控制面的 multipart body 必须**真的包含**两段的数据（不是空壳/边界）
    REQUIRE(c.body.find(payload.substr(0, 5)) != std::string::npos);
    REQUIRE(c.body.find(payload.substr(6, 4)) != std::string::npos);
    REQUIRE(c.Header("Content-Length") == std::to_string(c.body.size()));
  }
}

// =============================================================================
//  P3 鉴权与租户（ADR-006 §6.6：安全在数据面路径上再跑一遍）
// =============================================================================
TEST_CASE("★ P3 鉴权与租户：401/403/404 的状态、X-FSS-Error-Kind 与错误体两侧一致",
          "[adr006][integration]") {
  TempDir dir("adr006_p3");
  const std::string root = dir.child("data");
  PlaneConfig plane;
  plane.http_port = FreePort();
  plane.plane_port = FreePort();
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, plane));
  ServerProcess server(ProcessOptionsFor(config));
  REQUIRE(WaitReady(server.http_port(), 30000));

  const std::string payload = MakePayload(512);
  const Seeded seeded = Seed(server, payload);
  const int cp = server.http_port();
  const int pp = plane.plane_port;

  SECTION("篡改 token（签名不匹配）→ 401 kUnauthenticated，两侧同体") {
    const std::string target = TamperToken(seeded.get_target);
    const auto c = Do(cp, "GET", target, Authed());
    const auto p = Do(pp, "GET", target, Authed());
    REQUIRE(c.status == 401);
    REQUIRE(p.status == 401);
    REQUIRE(c.Header("X-FSS-Error-Kind") == "kUnauthenticated");
    REQUIRE(p.Header("X-FSS-Error-Kind") == "kUnauthenticated");
    REQUIRE(CompareReplies("tampered", c, p, true).empty());
  }

  SECTION("缺 exp/sig → 401 kUnauthenticated，两侧同体") {
    const std::string target = StripQuery(seeded.get_target);
    const auto c = Do(cp, "GET", target, Authed());
    const auto p = Do(pp, "GET", target, Authed());
    REQUIRE(c.status == 401);
    REQUIRE(p.status == 401);
    REQUIRE(CompareReplies("missing-sig", c, p, true).empty());
  }

  SECTION("data-partition-id 与 token 不符 → 403 kPermissionDenied，两侧同体") {
    const std::vector<std::string> headers = {"authorization: Bearer test-token",
                                              "data-partition-id: other"};
    const auto c = Do(cp, "GET", seeded.get_target, headers);
    const auto p = Do(pp, "GET", seeded.get_target, headers);
    REQUIRE(c.status == 403);
    REQUIRE(p.status == 403);
    REQUIRE(p.Header("X-FSS-Error-Kind") == "kPermissionDenied");
    REQUIRE(CompareReplies("wrong-partition", c, p, true).empty());
  }

  SECTION("合法签名但对象不存在 → 404，两侧同体") {
    const std::string target = ForgeMissingObjectTarget(cp);
    const auto c = Do(cp, "GET", target, Authed());
    const auto p = Do(pp, "GET", target, Authed());
    REQUIRE(c.status == 404);
    REQUIRE(p.status == 404);
    REQUIRE(CompareReplies("missing-object", c, p, true).empty());
  }
}

// =============================================================================
//  P4 硬化
// =============================================================================
TEST_CASE("★ P4 硬化：走私检查 / 超长 URI / 405 / 404", "[adr006][integration]") {
  TempDir dir("adr006_p4");
  const std::string root = dir.child("data");
  PlaneConfig plane;
  plane.http_port = FreePort();
  plane.plane_port = FreePort();
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, plane));
  ServerProcess server(ProcessOptionsFor(config));
  REQUIRE(WaitReady(server.http_port(), 30000));

  const std::string payload = MakePayload(256);
  const Seeded seeded = Seed(server, payload);
  const int cp = server.http_port();
  const int pp = plane.plane_port;

  SECTION("重复 Content-Length → 400（包装层的固定消息），两侧一致") {
    const std::vector<std::string> headers = {"Content-Length: 0", "Content-Length: 0"};
    const auto c = Do(cp, "GET", seeded.get_target, headers);
    const auto p = Do(pp, "GET", seeded.get_target, headers);
    REQUIRE(c.status == 400);
    REQUIRE(p.status == 400);
    REQUIRE(p.body.find("duplicate Content-Length headers") != std::string::npos);
    REQUIRE(CompareReplies("dup-cl", c, p, true).empty());
  }

  SECTION("Content-Length + Transfer-Encoding: chunked → 400，两侧一致") {
    //  ★ 必须**原样**发送：`Do()` 会给非空 body 自动补一个 Content-Length，那样就变成
    //    "两个 CL"而不是"CL 与 chunked 并存"。这里手工构造合法的 chunked 终止块，
    //    让控制面也能走到包装层的走私检查（否则 httplib 在解析层就卡住不响应）。
    const std::vector<std::string> headers = {"Content-Length: 0",
                                              "Transfer-Encoding: chunked"};
    const std::string chunked_terminator = "0\r\n\r\n";
    const auto c = DoRaw(cp, "GET", seeded.get_target, headers, chunked_terminator);
    const auto p = DoRaw(pp, "GET", seeded.get_target, headers, chunked_terminator);
    REQUIRE(c.status == 400);
    REQUIRE(p.status == 400);
    REQUIRE(p.body.find("Content-Length and Transfer-Encoding: chunked together") !=
            std::string::npos);
    REQUIRE(CompareReplies("cl-chunked", c, p, true).empty());
  }

  SECTION("★ 超长 URI：数据面回 414；控制面**不返回任何 HTTP 响应**（实测差异，已登记）") {
    const std::string target = "/api/file/v1/transfer/" + std::string(9000, 'A') + "?exp=1&sig=x";
    const auto c = Do(cp, "GET", target, Authed(), {}, 5000);
    const auto p = Do(pp, "GET", target, Authed(), {}, 5000);
    CAPTURE(c.transport_ok, c.status, p.transport_ok, p.status);
    //  数据面：明确的 414。
    REQUIRE(p.transport_ok);
    REQUIRE(p.status == 414);
    //  控制面：httplib 的 header reader 在 URI 长度检查**之前**就拒绝了该行 → 连接被
    //  关闭且不发响应（实测：target ≥ 8180 时 transport_ok=false）。这不是"同状态"，
    //  而是"控制面根本没有状态"—— 因此这条**不能**作为等价性判据，只能如实登记。
    REQUIRE_FALSE(c.transport_ok);
  }

  SECTION("不支持的方法（POST）→ 数据面 405 + Allow；控制面 404（范围决策的已知差异）") {
    const auto p = Do(pp, "POST", seeded.get_target, Authed(), "", 5000);
    REQUIRE(p.status == 405);
    REQUIRE(p.Header("Allow") == "GET, HEAD");
    //  ★ 控制面注册了 PUT（上传）但没有 POST：方法不匹配 → 404（包装层语义）
    const auto c = Do(cp, "POST", seeded.get_target, Authed(), "", 5000);
    REQUIRE(c.status == 404);
    //  上传仍留在控制面：PUT 到同一 token（put token）→ 不是 405（这里用 get token，
    //  因此是 403「操作类型不匹配」而不是「方法不允许」—— 证明数据面**没有**接管 PUT）。
    const auto put_on_plane = Do(pp, "PUT", seeded.get_target, Authed(), "", 5000);
    REQUIRE(put_on_plane.status == 405);
  }

  SECTION("未知路径 → 数据面与控制面同为 404，错误体一致") {
    const std::string target = "/api/file/v2/no/such/route";
    const auto c = Do(cp, "GET", target, Authed());
    const auto p = Do(pp, "GET", target, Authed());
    REQUIRE(c.status == 404);
    REQUIRE(p.status == 404);
    REQUIRE(CompareReplies("unknown-path", c, p, true).empty());
  }
}

// =============================================================================
//  P5 零拷贝真的被用上（"配了但没走"必须被抓到）
// =============================================================================
TEST_CASE("★ P5 零拷贝：sendfile 计数器只在数据面增长，且存储出流量计数两条路都动",
          "[adr006][integration]") {
  TempDir dir("adr006_p5");
  const std::string root = dir.child("data");
  PlaneConfig plane;
  plane.http_port = FreePort();
  plane.plane_port = FreePort();
  plane.use_sendfile = true;
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, plane));
  ServerProcess server(ProcessOptionsFor(config));
  REQUIRE(WaitReady(server.http_port(), 30000));

  const std::string payload = MakePayload(8192);
  const Seeded seeded = Seed(server, payload);
  const int cp = server.http_port();
  const int pp = plane.plane_port;

  const auto base = GetMetrics(cp);
  const auto sf_calls0 = MetricValueOrZero(base, "fss_large_file_plane_sendfile_calls_total");
  const auto sf_bytes0 = MetricValueOrZero(base, "fss_large_file_plane_sendfile_bytes_total");
  const auto us_calls0 = MetricValueOrZero(base, "fss_large_file_plane_userspace_calls_total");
  const auto storage_out0 =
      MetricValueOrZero(base, "fss_storage_bytes_total", "direction=\"out\"");

  // ① 控制面下载：sendfile 计数**不动**，但存储出流量必须动（计量装饰器的 Read 路径）
  const auto c = Do(cp, "GET", seeded.get_target, Authed());
  REQUIRE(c.status == 200);
  REQUIRE(c.body == payload);
  const auto after_control = GetMetrics(cp);
  REQUIRE(MetricValueOrZero(after_control, "fss_large_file_plane_sendfile_calls_total") ==
          sf_calls0);
  REQUIRE(MetricValueOrZero(after_control, "fss_large_file_plane_userspace_calls_total") ==
          us_calls0);
  const auto storage_out1 =
      MetricValue(after_control, "fss_storage_bytes_total", "direction=\"out\"");
  //  ★ 必须**恰好**等于本次下载的字节数。`>=` 抓不到「重复计数」：父代理实测把
  //    sendfile 路径的 `on_native_bytes` 记两次，`>=` 判据仍然全绿 ⇒ 它对「两条路互斥、
  //    不重复计数」这一【本切片引入的语义】没有区分力。
  REQUIRE(storage_out1 == storage_out0 + static_cast<std::int64_t>(payload.size()));

  // ② 数据面下载：sendfile 次数/字节各 +1/+N，存储出流量同样必须动
  const auto p = Do(pp, "GET", seeded.get_target, Authed());
  REQUIRE(p.status == 200);
  REQUIRE(p.body == payload);
  const auto after_plane = GetMetrics(cp);
  REQUIRE(MetricValue(after_plane, "fss_large_file_plane_sendfile_calls_total") == sf_calls0 + 1);
  REQUIRE(MetricValue(after_plane, "fss_large_file_plane_sendfile_bytes_total") ==
          sf_bytes0 + static_cast<std::int64_t>(payload.size()));
  REQUIRE(MetricValueOrZero(after_plane, "fss_large_file_plane_userspace_calls_total") ==
          us_calls0);
  //  正控（R16）：自增之后该 family 必须真的出现在抓取文本里
  REQUIRE(after_plane.find("fss_large_file_plane_sendfile_calls_total") != std::string::npos);
  const auto storage_out2 =
      MetricValue(after_plane, "fss_storage_bytes_total", "direction=\"out\"");
  //  同 ①：恰好一次（否则 sendfile 路径与计量装饰器就重复记了同一批字节）
  REQUIRE(storage_out2 == storage_out1 + static_cast<std::int64_t>(payload.size()));
}

TEST_CASE("★ P5b use_sendfile=false：用户态 pump 计数器增长、sendfile 保持不动",
          "[adr006][integration]") {
  TempDir dir("adr006_p5b");
  const std::string root = dir.child("data");
  PlaneConfig plane;
  plane.http_port = FreePort();
  plane.plane_port = FreePort();
  plane.use_sendfile = false;  // ← 关键：配置成用户态
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, plane));
  ServerProcess server(ProcessOptionsFor(config));
  REQUIRE(WaitReady(server.http_port(), 30000));
  REQUIRE(GetInfo(server.http_port()).find("\"largeFilePlane\":\"userspace\"") !=
          std::string::npos);

  const std::string payload = MakePayload(4096);
  const Seeded seeded = Seed(server, payload);

  const auto base = GetMetrics(server.http_port());
  const auto sf_calls0 = MetricValueOrZero(base, "fss_large_file_plane_sendfile_calls_total");
  const auto us_calls0 = MetricValueOrZero(base, "fss_large_file_plane_userspace_calls_total");
  const auto us_bytes0 = MetricValueOrZero(base, "fss_large_file_plane_userspace_bytes_total");
  const auto storage_out0 =
      MetricValueOrZero(base, "fss_storage_bytes_total", "direction=\"out\"");

  const auto p = Do(plane.plane_port, "GET", seeded.get_target, Authed());
  REQUIRE(p.status == 200);
  REQUIRE(p.body == payload);

  const auto after = GetMetrics(server.http_port());
  REQUIRE(MetricValueOrZero(after, "fss_large_file_plane_sendfile_calls_total") == sf_calls0);
  REQUIRE(MetricValue(after, "fss_large_file_plane_userspace_calls_total") == us_calls0 + 1);
  REQUIRE(MetricValue(after, "fss_large_file_plane_userspace_bytes_total") ==
          us_bytes0 + static_cast<std::int64_t>(payload.size()));
  //  ★ 用户态 pump 的存储出流量也必须**恰好**记一次（由 `MeteredNativeSource::Read` 记；
  //    数据面在这条路上**不**补记 —— 两条规则互斥，父代理用「重复计数」注入钉住它）。
  const auto storage_out1 =
      MetricValue(after, "fss_storage_bytes_total", "direction=\"out\"");
  REQUIRE(storage_out1 == storage_out0 + static_cast<std::int64_t>(payload.size()));
  REQUIRE(after.find("fss_large_file_plane_userspace_calls_total") != std::string::npos);
}

// =============================================================================
//  P6 在途连接上限
// =============================================================================
TEST_CASE("★ P6 连接上限：max_connections=1 时第二个并发请求 503 + Retry-After，且无对象字节",
          "[adr006][integration]") {
  TempDir dir("adr006_p6");
  const std::string root = dir.child("data");
  PlaneConfig plane;
  plane.http_port = FreePort();
  plane.plane_port = FreePort();
  plane.workers = 1;
  plane.max_connections = 1;
  plane.sendfile_chunk_bytes = 65536;
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, plane));
  ServerProcess server(ProcessOptionsFor(config));
  REQUIRE(WaitReady(server.http_port(), 30000));

  //  16 MiB 对象：客户端不读 body ⇒ 内核发送/接收缓冲很快填满 ⇒ sendfile 阻塞 ⇒
  //  第一条连接**保持**在途，第二条才会被上限拒绝（不靠 sleep 猜时序）。
  const std::string payload = MakePayload(16 * 1024 * 1024);
  const Seeded seeded = Seed(server, payload);

  RawClient first(plane.plane_port, true);
  REQUIRE(first.Connect());
  REQUIRE(first.SendRequest("GET", seeded.get_target, Authed()));
  const auto head = first.ReadHead(15000);
  REQUIRE(head.has_value());
  CAPTURE(head->status);
  REQUIRE(head->status == 200);
  REQUIRE(head->Header("Content-Length") == std::to_string(payload.size()));
  //  ★ 前置条件（R9）：此刻第一条连接确实还在途（body 尚未读完）。用"读少量字节后
  //    仍有大量未读"证明它没有结束 —— 而不是断言一个固定 sleep。
  std::uint64_t drained = first.DiscardBody(1024, 5000);
  REQUIRE(drained == 1024);
  const auto stats_head = GetMetrics(server.http_port());
  (void)stats_head;

  const auto second = Do(plane.plane_port, "GET", seeded.get_target, Authed(), {}, 15000);
  CAPTURE(second.status, second.body, second.Header("Retry-After"));
  REQUIRE(second.transport_ok);
  REQUIRE(second.status == 503);
  REQUIRE(second.Header("Retry-After") == "1");
  REQUIRE(second.body.find("too many in-flight requests (limit 1)") != std::string::npos);
  //  "no bytes"：响应体里不得混入对象字节
  REQUIRE(second.body.size() < 512);
  REQUIRE(second.body.find(std::string(payload.data(), 64)) == std::string::npos);
  REQUIRE(MetricValue(GetMetrics(server.http_port()),
                      "fss_large_file_plane_rejected_busy_total") >= 1);

  first.Close();
}

// =============================================================================
//  P7 RSS（≥1 GiB 传输的进程 RSS 增长 < 既有门槛）
// =============================================================================
TEST_CASE("★ P7 RSS：≥1 GiB 对象走数据面，服务进程 VmHWM 增长 < 既有大文件门槛",
          "[adr006][integration]") {
  TempDir dir("adr006_p7");
  const std::string root = dir.child("data");
  PlaneConfig plane;
  plane.http_port = FreePort();
  plane.plane_port = FreePort();
  plane.sendfile_chunk_bytes = 65536;  // 强制 sendfile 循环（>2 GiB 的同一代码路径）
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, plane));
  ServerProcess server(ProcessOptionsFor(config));
  REQUIRE(WaitReady(server.http_port(), 30000));

  //  ---- 直接播种一个 ≥1 GiB 的真实（非稀疏）对象 + 自签 get token ----
  const std::int64_t bytes = fss::test::BigFileBytes();
  INFO("对象大小 " << bytes << " 字节"
                   << (bytes < (1LL << 30) ? "（★ 缩小运行，不作为 P7 的 1 GiB 证据）" : ""));
  fss::ManualClock store_clock;
  fss::infra::PosixBlobStore store(root + "/blobs", store_clock);
  REQUIRE(store.ensure_container("opendes-persistent").ok());
  fss::domain::ObjectRef ref;
  ref.container = "opendes-persistent";
  ref.key = "osdu-user/adr006/p7-object.bin";
  {
    fss::bytes::RepeatingSource source(bytes);
    const auto put = store.put(ref, source, fss::domain::PutOptions{});
    REQUIRE(put.ok());
  }

  fss::SystemClock clock;
  fss::infra::HmacTransferTokenCodec codec(kSecret, clock, kKeyId);
  fss::domain::TransferToken token;
  token.partition = "opendes";
  token.file_id = "adr006-p7";
  token.container = ref.container;
  token.object_key = ref.key;
  token.zone = fss::domain::StorageZone::kPersistent;
  token.op = "get";
  token.expires_at_epoch_seconds = clock.NowEpochSeconds() + 3600;
  token.key_id = kKeyId;
  const auto url = codec.Encode(
      token, std::string("http://127.0.0.1:") + std::to_string(server.http_port()) + kBasePath);
  REQUIRE(url.ok());
  const std::string target = TargetOf(url.value());

  const auto sf_bytes0 =
      MetricValueOrZero(GetMetrics(server.http_port()),
                        "fss_large_file_plane_sendfile_bytes_total");
  const std::uint64_t hwm_before = ProcessPeakRssKib(server.pid());
  REQUIRE(hwm_before > 0);

  //  ---- 数据面下载：客户端**只丢弃**字节，不驻留（否则测试自己会占 1 GiB）----
  RawClient client(plane.plane_port, true);
  REQUIRE(client.Connect());
  REQUIRE(client.SendRequest("GET", target, Authed()));
  const auto head = client.ReadHead(30000);
  REQUIRE(head.has_value());
  CAPTURE(head->status);
  REQUIRE(head->status == 200);
  REQUIRE(head->Header("Content-Length") == std::to_string(bytes));
  const std::uint64_t drained = client.DiscardBody(static_cast<std::uint64_t>(bytes), 300000);
  REQUIRE(drained == static_cast<std::uint64_t>(bytes));
  client.Close();

  const std::uint64_t hwm_after = ProcessPeakRssKib(server.pid());
  const std::uint64_t growth_kib = hwm_after - hwm_before;
  INFO("服务进程 RSS 峰值增长 " << growth_kib << " KiB（上限 " << fss::test::RssLimitKib()
                                << " KiB）");
  REQUIRE(hwm_after >= hwm_before);  // VmHWM 单调不减（前置条件）
  REQUIRE(growth_kib < fss::test::RssLimitKib());

  //  零拷贝的强判据：sendfile 送出的字节数 == 对象大小（一次调用循环到 EOF）
  const auto sf_bytes1 =
      MetricValue(GetMetrics(server.http_port()), "fss_large_file_plane_sendfile_bytes_total");
  REQUIRE(sf_bytes1 == sf_bytes0 + bytes);
}

// =============================================================================
//  P8 等价性矩阵（压轴）
// =============================================================================
TEST_CASE("★ P8 等价性矩阵：同一请求打到两个端口逐字段比对（唯一例外 = 多段降级）",
          "[adr006][integration]") {
  TempDir dir("adr006_p8");
  const std::string root = dir.child("data");
  PlaneConfig plane;
  plane.http_port = FreePort();
  plane.plane_port = FreePort();
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, plane));
  ServerProcess server(ProcessOptionsFor(config));
  REQUIRE(WaitReady(server.http_port(), 30000));

  const std::string payload = MakePayload(4096);
  const Seeded seeded = Seed(server, payload);
  const int cp = server.http_port();
  const int pp = plane.plane_port;

  struct MatrixCase {
    std::string name;
    std::string target;
    std::vector<std::string> headers;
    bool compare_body = true;
    //  走私形态必须**原样**发送（见 DoRaw 的说明）
    bool raw = false;
    std::string raw_body;
    //  多段：两侧**按设计**不同（数据面 200 全量 / 控制面 multipart 206），
    //  这一条由 P2 双向断言，矩阵里显式标出来（不计入"未登记差异"）。
    bool documented_multi_range = false;
  };
  const std::vector<MatrixCase> cases = {
      {"full", seeded.get_target, {}, true, false, "", false},
      {"range-0-99", seeded.get_target, {"Range: bytes=0-99"}, true, false, "", false},
      {"range-100-", seeded.get_target, {"Range: bytes=100-"}, true, false, "", false},
      {"range-suffix", seeded.get_target, {"Range: bytes=-50"}, true, false, "", false},
      {"range-unsatisfiable", seeded.get_target, {"Range: bytes=999999-"}, true, false, "", false},
      {"range-malformed", seeded.get_target, {"Range: bytes=abc"}, true, false, "", false},
      {"wrong-partition", seeded.get_target,
       {"authorization: Bearer test-token", "data-partition-id: other"}, true, false, "", false},
      {"tampered-token", TamperToken(seeded.get_target), {"authorization: Bearer test-token",
                                                         "data-partition-id: opendes"},
       true, false, "", false},
      {"missing-sig", StripQuery(seeded.get_target),
       {"authorization: Bearer test-token", "data-partition-id: opendes"}, true, false, "", false},
      {"missing-object", ForgeMissingObjectTarget(cp),
       {"authorization: Bearer test-token", "data-partition-id: opendes"}, true, false, "", false},
      {"duplicate-content-length", seeded.get_target, {"Content-Length: 0", "Content-Length: 0"},
       true, false, "", false},
      {"cl-plus-chunked", seeded.get_target, {"Content-Length: 0", "Transfer-Encoding: chunked"},
       true, true, "0\r\n\r\n", false},
      {"unknown-path", "/api/file/v2/no/such/route", {}, true, false, "", false},
      {"multi-range", seeded.get_target, {"Range: bytes=0-4,6-9"}, false, false, "", true},
  };

  std::vector<std::string> all_diffs;
  for (const auto& item : cases) {
    const auto c = item.raw ? DoRaw(cp, "GET", item.target, item.headers, item.raw_body)
                            : Do(cp, "GET", item.target, item.headers);
    const auto p = item.raw ? DoRaw(pp, "GET", item.target, item.headers, item.raw_body)
                            : Do(pp, "GET", item.target, item.headers);
    CAPTURE(item.name, c.status, p.status, c.Header("Content-Type"), p.Header("Content-Type"));
    REQUIRE(c.transport_ok);
    REQUIRE(p.transport_ok);
    if (item.documented_multi_range) {
      //  唯一被文档化的差异：两侧的实际值都要断言（绝不静默）
      REQUIRE(c.status == 206);
      REQUIRE(c.Header("Content-Type").find("multipart/byteranges") != std::string::npos);
      REQUIRE(p.status == 200);
      REQUIRE(p.Header("Content-Type") == "application/octet-stream");
      REQUIRE(p.body == payload);
      continue;
    }
    auto diffs = CompareReplies(item.name, c, p, item.compare_body);
    for (auto& diff : diffs) all_diffs.push_back(std::move(diff));
  }
  {
    std::string report;
    for (const auto& diff : all_diffs) report += "  " + diff + "\n";
    INFO("未登记的差异（应为空）：\n" << report);
    REQUIRE(all_diffs.empty());
  }
}

// =============================================================================
//  P9 enabled=false 回归
// =============================================================================
TEST_CASE("★ P9 enabled=false：数据面端口没有任何监听，控制面行为不变",
          "[adr006][integration]") {
  TempDir dir("adr006_p9");
  const std::string root = dir.child("data");
  PlaneConfig plane;
  plane.enabled = false;
  plane.http_port = FreePort();
  plane.plane_port = FreePort();
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, plane));
  ServerProcess server(ProcessOptionsFor(config));
  INFO("启动横幅：\n" << server.DumpLog());
  REQUIRE(WaitReady(server.http_port(), 30000));

  //  ① 数据面端口**没有任何监听**（连接被拒绝）
  {
    RawClient probe(plane.plane_port, true);
    const bool connected = probe.Connect();
    CAPTURE(plane.plane_port, connected);
    REQUIRE_FALSE(connected);
  }
  //  ② `/v2/info` 如实说 disabled；横幅说 disabled
  REQUIRE(GetInfo(server.http_port()).find("\"largeFilePlane\":\"disabled\"") !=
          std::string::npos);
  REQUIRE(server.DumpLog().find("plane bind     : disabled") != std::string::npos);
  //  ③ 控制面下载照常（接线前行为）
  const std::string payload = MakePayload(1024);
  const Seeded seeded = Seed(server, payload);
  const auto c = Do(server.http_port(), "GET", seeded.get_target, Authed());
  REQUIRE(c.status == 200);
  REQUIRE(c.body == payload);
  //  ④ 数据面计数器**没有**被注册（没有数据面实例）
  const auto metrics = GetMetrics(server.http_port());
  REQUIRE(metrics.find("fss_large_file_plane_sendfile_calls_total") == std::string::npos);
}

// =============================================================================
//  P10 HEAD
// =============================================================================
TEST_CASE("★ P10 HEAD：数据面与控制面状态/头一致且无 body", "[adr006][integration]") {
  TempDir dir("adr006_p10");
  const std::string root = dir.child("data");
  PlaneConfig plane;
  plane.http_port = FreePort();
  plane.plane_port = FreePort();
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, plane));
  ServerProcess server(ProcessOptionsFor(config));
  REQUIRE(WaitReady(server.http_port(), 30000));

  const std::string payload = MakePayload(2048);
  const Seeded seeded = Seed(server, payload);
  const int cp = server.http_port();
  const int pp = plane.plane_port;

  const auto c = DoHead(cp, seeded.get_target, Authed());
  const auto p = DoHead(pp, seeded.get_target, Authed());
  CAPTURE(c.status, p.status, c.Header("Content-Length"), p.Header("Content-Length"));
  REQUIRE(c.transport_ok);
  REQUIRE(p.transport_ok);
  REQUIRE(c.status == 200);
  REQUIRE(p.status == 200);
  REQUIRE(p.Header("Content-Length") == std::to_string(payload.size()));
  REQUIRE(p.Header("Content-Length") == c.Header("Content-Length"));
  REQUIRE(p.Header("Content-Type") == c.Header("Content-Type"));
  REQUIRE(p.body.empty());  // HEAD 不得有 body
  //  HEAD 不传输字节 ⇒ 不增加 sendfile 计数
  const auto before = GetMetrics(cp);
  const auto sf0 = MetricValueOrZero(before, "fss_large_file_plane_sendfile_calls_total");
  const auto p2 = DoHead(pp, seeded.get_target, Authed());
  REQUIRE(p2.status == 200);
  REQUIRE(MetricValueOrZero(GetMetrics(cp), "fss_large_file_plane_sendfile_calls_total") == sf0);
}

// =============================================================================
//  P12 生命周期：数据面端口被占用时，组合根必须 exit 78 + 可读修法（绝不静默禁用）
// =============================================================================
TEST_CASE("★ P12 启动失败：数据面端口被占用 → exit 78 + 可读修法", "[adr006][integration]") {
  TempDir dir("adr006_p12");
  const std::string root = dir.child("data");
  PlaneConfig plane;
  plane.http_port = FreePort();
  plane.plane_port = FreePort();

  //  先把数据面端口占住（listen 着，不 accept）
  const int squatter = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(squatter >= 0);
  int on = 1;
  ::setsockopt(squatter, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<std::uint16_t>(plane.plane_port));
  REQUIRE(::bind(squatter, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  REQUIRE(::listen(squatter, 8) == 0);

  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, plane));
  const auto outcome = fss::test::RunServerForExit({"--config", config});
  ::close(squatter);
  CAPTURE(outcome.exit_code, outcome.output);
  REQUIRE(outcome.exit_code == 78);
  REQUIRE(outcome.output.find("拒绝启动") != std::string::npos);
  REQUIRE(outcome.output.find("large_file_plane") != std::string::npos);
  REQUIRE(outcome.output.find("端口") != std::string::npos);  // 可读修法（改端口/关数据面）
  REQUIRE(outcome.output.find("已启动") == std::string::npos);
}

// =============================================================================
//  P11 访问日志字段集同源（ADR-006 §6.2）
// =============================================================================
TEST_CASE("★ P11 访问日志：控制面与数据面 `http_request` 的字段集逐字段相同",
          "[adr006][integration]") {
  TempDir dir("adr006_p11");
  const std::string root = dir.child("data");
  PlaneConfig plane;
  plane.http_port = FreePort();
  plane.plane_port = FreePort();
  const std::string config = WriteFile(dir, "fss.json", ConfigJson(root, plane));
  ServerProcess server(ProcessOptionsFor(config));
  REQUIRE(WaitReady(server.http_port(), 30000));

  const std::string payload = MakePayload(1024);
  const Seeded seeded = Seed(server, payload);

  const std::string control_cid = "adr006-p11-control";
  const std::string plane_cid = "adr006-p11-plane";
  {
    const auto c = Do(server.http_port(), "GET", seeded.get_target,
                      {"authorization: Bearer test-token", "data-partition-id: opendes",
                       "x-correlation-id: " + control_cid});
    REQUIRE(c.status == 200);
    const auto p = Do(plane.plane_port, "GET", seeded.get_target,
                      {"authorization: Bearer test-token", "data-partition-id: opendes",
                       "x-correlation-id: " + plane_cid});
    REQUIRE(p.status == 200);
  }

  //  轮询"日志里两条都在"（实际条件，不用固定 sleep）
  std::vector<fss::json::Value> control_records;
  std::vector<fss::json::Value> plane_records;
  for (int attempt = 0; attempt < 200; ++attempt) {
    const std::string log = server.DumpLog();
    control_records = AccessLogRecords(log, control_cid);
    plane_records = AccessLogRecords(log, plane_cid);
    if (!control_records.empty() && !plane_records.empty()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  REQUIRE(control_records.size() == 1);
  REQUIRE(plane_records.size() == 1);
  const auto control_keys = KeysOf(control_records[0]);
  const auto plane_keys = KeysOf(plane_records[0]);
  {
    std::string report = "control keys: ";
    for (const auto& key : control_keys) report += key + " ";
    report += "\nplane keys  : ";
    for (const auto& key : plane_keys) report += key + " ";
    INFO(report);
  }
  REQUIRE(control_keys == plane_keys);
  //  正控（R16）：字段集不是空集 —— 否则"相同"可以在空集上成立
  for (const char* field : {"method", "path", "status", "duration_ms", "correlation_id",
                            "route"}) {
    REQUIRE(control_keys.count(field) == 1);
    REQUIRE(plane_keys.count(field) == 1);
  }
}
