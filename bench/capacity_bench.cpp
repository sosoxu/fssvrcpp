// =============================================================================
//  bench/capacity_bench.cpp —— 容量基线的**负载生成器**（C9.11）
// =============================================================================
//  铁律 R2：性能测量必须**独立进程 + 绑核**。本文件是"客户端"那一个进程：
//  由 `scripts/bench_baseline.sh` 用 `taskset` 绑到与服务端**不同**的核上运行，
//  绝不在服务进程内压测（曾因此得出"并发 32 比 4 慢 15 倍"的方向性错误结论）。
//
//  为什么自己写而不是用 `ab`/`wrk`：本仓库的判据要求**同一负载生成器**同时驱动
//  · 控制面 REST（GET/POST，带鉴权头与 partition 头）
//  · 数据面自签 URL（PUT/GET，无鉴权头、可能几百 MiB 的响应体）
//  · ADR-006 的 A/B（httplib 内容提供者 vs 裸 `sendfile`）
//  而且要能把"客户端"钉在指定核上、能按连接数复现同一批数字。外部工具都缺一角。
//
//  诚实声明（R3/R4）
//  ---------------------------------------------------------------------------
//  · 这里的绝对数字来自 **WSL2 虚拟盘 + loopback**，只能作为**本机量级参考**与
//    **同机相对比较**（回归 <20%、A/B 倍数）；**不得**当作生产容量结论（C9.14）。
//  · 协议为 HTTP/1.1 keep-alive，自签 token 走 `Authorization`/查询串两种形态，
//    与生产一致；`--insecure`（不校验签名）等"更快但不安全"的模式**不提供**，
//    避免用不安全协议的数字支撑设计（R3 的由来）。
//
//  用法
//  ---------------------------------------------------------------------------
//    fss_bench_capacity seed --base-url URL --partition opendes
//        [--large-bytes N] [--auth-token T] [--file-name F]
//      → 走真实 API：uploadURL → PUT → POST metadata；可选再造一个大对象。
//        输出 `SEED ...` 行，供脚本取出 metadata/down/put 的 URL。
//
//    fss_bench_capacity load --label L --mode get|put --url URL
//        --connections N --duration-ms T [--warmup-ms W] [--body-bytes B]
//        [--header 'k: v']... [--host H] [--expect-status C]
//      → 输出一行 `RESULT ...`（reqs/errs/rps/mibps/p50_us/p99_us）。
// =============================================================================
#include "domain/model/file_metadata.h"
#include "common/json/json.h"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- URL 解析
struct Url {
  std::string scheme;
  std::string host;
  int port = 80;
  std::string target;  // path + query
};

bool ParseUrl(const std::string& text, Url* out) {
  const auto scheme_end = text.find("://");
  if (scheme_end == std::string::npos) return false;
  out->scheme = text.substr(0, scheme_end);
  if (out->scheme != "http") return false;  // 基准只跑明文（TLS 开销不在本判据内）
  const std::string rest = text.substr(scheme_end + 3);
  const auto slash = rest.find('/');
  const std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  out->target = slash == std::string::npos ? "/" : rest.substr(slash);
  const auto colon = authority.rfind(':');
  if (colon == std::string::npos) {
    out->host = authority;
    out->port = 80;
  } else {
    out->host = authority.substr(0, colon);
    out->port = std::atoi(authority.substr(colon + 1).c_str());
  }
  return !out->host.empty();
}

// ---------------------------------------------------------------- 连接
class Conn {
 public:
  Conn() = default;
  ~Conn() { Close(); }
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;

  bool Connect(const std::string& host, int port, int timeout_ms = 30000) {
    Close();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return false;
    fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd_ < 0) {
      ::freeaddrinfo(res);
      return false;
    }
    int on = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));  // 见 AGENTS 陷阱清单
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    const bool ok = ::connect(fd_, res->ai_addr, res->ai_addrlen) == 0;
    ::freeaddrinfo(res);
    if (!ok) Close();
    buf_.clear();
    return ok;
  }
  void Close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    buf_.clear();
  }
  bool valid() const { return fd_ >= 0; }

  bool SendAll(std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
      const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) return false;
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

  //  读一个完整响应。`body_out` 非空时把响应体拷出来（seed 模式要读 JSON）；
  //  否则只统计字节数（load 模式）。
  bool ReadResponse(int* status_out, std::size_t* bytes_out, std::string* body_out) {
    const auto head_end = ReadUntil("\r\n\r\n");
    if (!head_end.has_value()) return false;
    const std::string head = buf_.substr(0, *head_end);
    buf_.erase(0, *head_end + 4);
    if (status_out != nullptr) *status_out = ParseStatus(head);
    std::size_t remaining = 0;
    bool chunked = false;
    std::size_t content_length = 0;
    for (const auto& line : SplitLines(head)) {
      const auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      const std::string key = Lower(line.substr(0, colon));
      const std::string value = Trim(line.substr(colon + 1));
      if (key == "content-length") content_length = static_cast<std::size_t>(std::stoull(value));
      if (key == "transfer-encoding" && Lower(value).find("chunked") != std::string::npos) {
        chunked = true;
      }
    }
    std::size_t total = 0;
    if (chunked) {
      if (!ReadChunked(&total, body_out)) return false;
    } else {
      remaining = content_length;
      if (body_out != nullptr) body_out->clear();
      while (remaining > 0) {
        if (buf_.empty() && !Fill()) return false;
        const std::size_t take = std::min(remaining, buf_.size());
        if (body_out != nullptr) body_out->append(buf_.data(), take);
        buf_.erase(0, take);
        remaining -= take;
        total += take;
      }
    }
    if (bytes_out != nullptr) *bytes_out = total;
    return true;
  }

 private:
  bool Fill() {
    char tmp[64 * 1024];
    const ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
    if (n <= 0) return false;
    buf_.append(tmp, static_cast<std::size_t>(n));
    return true;
  }
  bool ReadChunked(std::size_t* total, std::string* body_out) {
    for (;;) {
      const auto line_end = ReadUntil("\r\n");
      if (!line_end.has_value()) return false;
      const std::string size_line = Trim(buf_.substr(0, *line_end));
      buf_.erase(0, *line_end + 2);
      const std::size_t size = std::stoull(size_line, nullptr, 16);
      if (size == 0) {
        const auto trailer_end = ReadUntil("\r\n");
        if (trailer_end.has_value()) buf_.erase(0, *trailer_end + 2);
        return true;
      }
      std::size_t remaining = size;
      while (remaining > 0) {
        if (buf_.empty() && !Fill()) return false;
        const std::size_t take = std::min(remaining, buf_.size());
        if (body_out != nullptr) body_out->append(buf_.data(), take);
        buf_.erase(0, take);
        remaining -= take;
        *total += take;
      }
      const auto crlf_end = ReadUntil("\r\n");
      if (!crlf_end.has_value()) return false;
      buf_.erase(0, *crlf_end + 2);
    }
  }
  std::optional<std::size_t> ReadUntil(std::string_view needle) {
    for (;;) {
      const auto pos = buf_.find(needle);
      if (pos != std::string::npos) return pos;
      if (!Fill()) return std::nullopt;
    }
  }
  static int ParseStatus(const std::string& head) {
    const auto space = head.find(' ');
    if (space == std::string::npos) return 0;
    return std::atoi(head.substr(space + 1, 3).c_str());
  }
  static std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
  }
  static std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
  }
  static std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < s.size()) {
      const auto nl = s.find("\r\n", pos);
      if (nl == std::string::npos) break;
      out.push_back(s.substr(pos, nl - pos));
      pos = nl + 2;
    }
    return out;
  }

  int fd_ = -1;
  std::string buf_;
};

// ---------------------------------------------------------------- 一次性 HTTP
struct SimpleResponse {
  int status = 0;
  std::string body;
};

//  在**已建立**的连接上发一次请求（keep-alive 可复用；`close` 决定是否要求服务端关闭）。
//  ★ chain 模式用它把"uploadURL → PUT → POST metadata"跑在同一连接上：
//    否则量到的是"TCP 握手 + 三步业务"，而不是 C9.15 要的端到端吞吐。
SimpleResponse DoRequest(Conn& conn, const Url& url, std::string_view method,
                         const std::string& body, const std::vector<std::string>& headers,
                         bool close) {
  SimpleResponse out;
  std::string req;
  req += method;
  req += ' ';
  req += url.target;
  req += " HTTP/1.1\r\nHost: ";
  req += url.host;
  req += close ? "\r\nConnection: close\r\n" : "\r\nConnection: keep-alive\r\n";
  for (const auto& h : headers) {
    req += h;
    req += "\r\n";
  }
  if (!body.empty() || method == "POST" || method == "PUT") {
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  req += "\r\n";
  req += body;
  if (!conn.SendAll(req)) return out;
  std::size_t bytes = 0;
  if (!conn.ReadResponse(&out.status, &bytes, &out.body)) out.status = 0;
  return out;
}

//  base URL（含 base path，如 `http://h:p/api/file`）上拼一个子路径
Url WithPath(const Url& base, const std::string& suffix) {
  Url out = base;
  out.target = base.target + suffix;
  return out;
}

SimpleResponse HttpOnce(const Url& url, std::string_view method, const std::string& body,
                        const std::vector<std::string>& headers) {
  Conn conn;
  if (!conn.Connect(url.host, url.port)) return SimpleResponse{};
  return DoRequest(conn, url, method, body, headers, /*close=*/true);
}

//  进程（全部线程）累计 CPU 秒数：用来判断"客户端是不是瓶颈"（R4）
double ProcessCpuSeconds() {
  timespec ts{};
  if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0;
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// ---------------------------------------------------------------- 参数
struct Args {
  std::vector<std::string> positional;
  std::vector<std::string> headers;
  std::string value(const std::string& key, const std::string& fallback = "") const {
    for (std::size_t i = 0; i + 1 < positional.size(); ++i) {
      if (positional[i] == key) return positional[i + 1];
    }
    return fallback;
  }
  long long number(const std::string& key, long long fallback) const {
    const std::string v = value(key);
    return v.empty() ? fallback : std::atoll(v.c_str());
  }
  bool has(const std::string& key) const {
    for (std::size_t i = 0; i + 1 < positional.size(); ++i) {
      if (positional[i] == key) return true;
    }
    return false;
  }
};

Args ParseArgs(int argc, char** argv) {
  Args out;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--header" && i + 1 < argc) {
      out.headers.emplace_back(argv[++i]);
    } else {
      out.positional.push_back(arg);
    }
  }
  return out;
}

// ---------------------------------------------------------------- seed
int RunSeed(const Args& args) {
  const std::string base = args.value("--base-url");
  const std::string partition = args.value("--partition", "opendes");
  const std::string token = args.value("--auth-token", "bench-token");
  const long long large_bytes = args.number("--large-bytes", 0);
  const std::string file_name = args.value("--file-name", "bench.bin");
  Url base_url;
  if (base.empty() || !ParseUrl(base, &base_url)) {
    std::cerr << "seed: --base-url 必须是 http://host:port/<base-path>\n";
    return 2;
  }
  const std::vector<std::string> common = {"authorization: Bearer " + token,
                                           "data-partition-id: " + partition,
                                           "content-type: application/json"};
  //  ---- 小对象：uploadURL → PUT → POST metadata ----
  auto upload = HttpOnce(WithPath(base_url, "/v2/files/uploadURL"), "GET", "", common);
  if (upload.status != 200) {
    std::cerr << "seed: uploadURL 失败 status=" << upload.status << " body=" << upload.body
              << "\n";
    return 1;
  }
  const auto upload_json = fss::json::ParseObject(upload.body);
  if (!upload_json.ok()) {
    std::cerr << "seed: uploadURL 响应不是 JSON\n";
    return 1;
  }
  const std::string file_id = upload_json.value()["FileID"].get<std::string>();
  const std::string file_source = upload_json.value()["Location"]["FileSource"].get<std::string>();
  const std::string put_url = upload_json.value()["Location"]["SignedURL"].get<std::string>();
  const std::string small_payload = "bench-small-payload";
  {
    Url url;
    if (!ParseUrl(put_url, &url)) {
      std::cerr << "seed: SignedURL 解析失败\n";
      return 1;
    }
    auto res = HttpOnce(url, "PUT", small_payload,
                        {"data-partition-id: " + partition,
                         "content-type: application/octet-stream"});
    if (res.status != 200) {
      std::cerr << "seed: PUT 失败 status=" << res.status << " body=" << res.body << "\n";
      return 1;
    }
  }
  //  记录：只填必要字段（与 `tests/framework/app_fixture.h::MakeRecord` 一致）
  fss::domain::FileMetadataRecord record;
  record.id = file_id;
  record.kind = "opendes:wks:dataset--File.Generic:1.0.0";
  record.acl.viewers = {"data.default.viewers@opendes.example.com"};
  record.acl.owners = {"data.default.owners@opendes.example.com"};
  record.legal.legaltags = {"opendes-public-1"};
  record.legal.other_relevant_data_countries = {"US"};
  record.legal.status = fss::domain::LegalStatus::kCompliant;
  record.data.name = file_name;
  record.data.endian = "LITTLE";
  record.data.dataset_properties.present = true;
  record.data.dataset_properties.file_source_info.file_source = file_source;
  std::string record_id = file_id;
  {
    const std::string body = fss::json::Dump(fss::domain::ToJson(record));
    auto res = HttpOnce(WithPath(base_url, "/v2/files/metadata"), "POST", body, common);
    if (res.status != 201) {
      std::cerr << "seed: POST metadata 失败 status=" << res.status << " body=" << res.body
                << "\n";
      return 1;
    }
    //  ★ 记录 id 不是上传时的 FileID：服务端按 OSDU 规范生成
    //    `partition:dataset--File.Generic:<uuid>`（P4 的 DTO 映射）。用错 id 会让
    //    "读元数据"这类控制面点位**全部 4xx**（实测 control_read 的 errs 4 万+，
    //    而 rps=0 被基线文件如实记了下来 —— 这正是"数字必须可核对"的价值）。
    const auto created = fss::json::ParseObject(res.body);
    if (created.ok() && created.value().contains("id")) {
      record_id = created.value()["id"].get<std::string>();
    }
  }
  auto download =
      HttpOnce(WithPath(base_url, "/v2/files/" + file_id + "/downloadURL"), "GET", "",
                           {"authorization: Bearer " + token,
                            "data-partition-id: " + partition});
  if (download.status != 200) {
    std::cerr << "seed: downloadURL 失败 status=" << download.status << "\n";
    return 1;
  }
  const auto download_json = fss::json::ParseObject(download.body);
  if (!download_json.ok()) {
    std::cerr << "seed: downloadURL 响应不是 JSON\n";
    return 1;
  }
  const std::string get_url = download_json.value()["SignedUrl"].get<std::string>();
  std::cout << "SEED small_get_url=" << get_url << " small_put_url=" << put_url
            << " record_id=" << record_id << " file_id=" << file_id
            << " file_source=" << file_source << "\n";

  //  ---- 大对象（可选）：同一条链路，只是体积大 ----
  if (large_bytes > 0) {
    auto big_upload = HttpOnce(WithPath(base_url, "/v2/files/uploadURL"), "GET", "", common);
    if (big_upload.status != 200) {
      std::cerr << "seed: 大对象 uploadURL 失败 status=" << big_upload.status << "\n";
      return 1;
    }
    const auto big_json = fss::json::ParseObject(big_upload.body);
    if (!big_json.ok()) return 1;
    const std::string big_id = big_json.value()["FileID"].get<std::string>();
    const std::string big_source = big_json.value()["Location"]["FileSource"].get<std::string>();
    const std::string big_put = big_json.value()["Location"]["SignedURL"].get<std::string>();
    {
      Url url;
      if (!ParseUrl(big_put, &url)) return 1;
      //  分块发送，避免在客户端驻留整份大对象（RSS 断言也在别的用例里盯着）
      Conn conn;
      if (!conn.Connect(url.host, url.port, 300000)) {
        std::cerr << "seed: 大对象连接失败\n";
        return 1;
      }
      std::string head = "PUT " + url.target + " HTTP/1.1\r\nHost: " + url.host +
                         "\r\nConnection: close\r\ndata-partition-id: " + partition +
                         "\r\ncontent-type: application/octet-stream\r\nContent-Length: " +
                         std::to_string(large_bytes) + "\r\n\r\n";
      if (!conn.SendAll(head)) return 1;
      std::string chunk(1024 * 1024, 'b');
      long long sent = 0;
      while (sent < large_bytes) {
        const auto take = std::min<long long>(static_cast<long long>(chunk.size()),
                                             large_bytes - sent);
        if (!conn.SendAll(std::string_view(chunk).substr(0, static_cast<std::size_t>(take)))) {
          std::cerr << "seed: 大对象发送中断（已发 " << sent << "）\n";
          return 1;
        }
        sent += take;
      }
      std::size_t bytes = 0;
      int status = 0;
      if (!conn.ReadResponse(&status, &bytes, nullptr) || status != 200) {
        std::cerr << "seed: 大对象 PUT status=" << status << "\n";
        return 1;
      }
    }
    fss::domain::FileMetadataRecord big_record = record;
    big_record.id = big_id;
    big_record.data.name = "bench-large.bin";
    big_record.data.dataset_properties.file_source_info.file_source = big_source;
    {
      const std::string body = fss::json::Dump(fss::domain::ToJson(big_record));
      auto res = HttpOnce(WithPath(base_url, "/v2/files/metadata"), "POST", body, common);
      if (res.status != 201) {
        std::cerr << "seed: 大对象 POST metadata status=" << res.status << " body=" << res.body
                  << "\n";
        return 1;
      }
    }
    //  大对象的下载地址要按 id 取
    auto big_dl =
        HttpOnce(WithPath(base_url, "/v2/files/" + big_id + "/downloadURL"), "GET", "",
                           {"authorization: Bearer " + token,
                            "data-partition-id: " + partition});
    if (big_dl.status != 200) {
      std::cerr << "seed: 大对象 downloadURL status=" << big_dl.status << "\n";
      return 1;
    }
    const auto big_dl_json = fss::json::ParseObject(big_dl.body);
    if (!big_dl_json.ok()) return 1;
    std::cout << "SEED large_get_url=" << big_dl_json.value()["SignedUrl"].get<std::string>()
              << " large_bytes=" << large_bytes << "\n";
  }
  return 0;
}

struct Stats {
  std::uint64_t requests = 0;
  std::uint64_t errors = 0;
  std::uint64_t bytes = 0;
  std::vector<double> latency_us;
};

//  一次完整的"小文件上传"端到端（C9.15）：uploadURL → PUT → POST metadata。
//  `name` 只用于记录里的文件名；每一步的失败原因写到 `why`。
bool OneUploadChain(Conn& conn, const Url& base, const std::string& partition,
                    const std::string& token, std::string_view payload, const std::string& name,
                    std::string* why) {
  const std::vector<std::string> json_headers = {"authorization: Bearer " + token,
                                                "data-partition-id: " + partition,
                                                "content-type: application/json"};
  const auto upload =
      DoRequest(conn, WithPath(base, "/v2/files/uploadURL"), "GET", "", json_headers,
                /*close=*/false);
  if (upload.status != 200) {
    *why = "uploadURL=" + std::to_string(upload.status);
    return false;
  }
  const auto upload_json = fss::json::ParseObject(upload.body);
  if (!upload_json.ok()) {
    *why = "uploadURL-JSON";
    return false;
  }
  const std::string file_id = upload_json.value()["FileID"].get<std::string>();
  const std::string file_source = upload_json.value()["Location"]["FileSource"].get<std::string>();
  const std::string put_url = upload_json.value()["Location"]["SignedURL"].get<std::string>();
  Url put;
  if (!ParseUrl(put_url, &put)) {
    *why = "SignedURL";
    return false;
  }
  const auto put_res = DoRequest(conn, put, "PUT", std::string(payload),
                                 {"data-partition-id: " + partition,
                                  "content-type: application/octet-stream"},
                                 /*close=*/false);
  if (put_res.status != 200) {
    *why = "PUT=" + std::to_string(put_res.status);
    return false;
  }
  fss::domain::FileMetadataRecord record;
  record.id = file_id;
  record.kind = "opendes:wks:dataset--File.Generic:1.0.0";
  record.acl.viewers = {"data.default.viewers@opendes.example.com"};
  record.acl.owners = {"data.default.owners@opendes.example.com"};
  record.legal.legaltags = {"opendes-public-1"};
  record.legal.other_relevant_data_countries = {"US"};
  record.legal.status = fss::domain::LegalStatus::kCompliant;
  record.data.name = name;
  record.data.endian = "LITTLE";
  record.data.dataset_properties.present = true;
  record.data.dataset_properties.file_source_info.file_source = file_source;
  const std::string record_body = fss::json::Dump(fss::domain::ToJson(record));
  const auto created = DoRequest(conn, WithPath(base, "/v2/files/metadata"), "POST", record_body,
                                 json_headers, /*close=*/false);
  if (created.status != 201) {
    *why = "metadata=" + std::to_string(created.status);
    return false;
  }
  return true;
}

//  `chain` 模式：多连接并发跑完整的"上传一个小文件"（含 token + POSIX + 两处 DB 写）
int RunChain(const Args& args) {
  const std::string label = args.value("--label", "upload_chain");
  const std::string base_text = args.value("--base-url");
  const std::string partition = args.value("--partition", "opendes");
  const std::string token = args.value("--auth-token", "bench-token");
  const int connections = static_cast<int>(args.number("--connections", 1));
  const long long duration_ms = args.number("--duration-ms", 3000);
  const long long warmup_ms = args.number("--warmup-ms", 500);
  const long long body_bytes = args.number("--body-bytes", 4096);
  Url base;
  if (base_text.empty() || !ParseUrl(base_text, &base)) {
    std::cerr << "chain: --base-url 无效\n";
    return 2;
  }
  const std::string payload(static_cast<std::size_t>(body_bytes), 'c');
  std::vector<Stats> per_thread(static_cast<std::size_t>(connections));
  std::vector<std::string> first_error(static_cast<std::size_t>(connections));
  const double cpu_before = ProcessCpuSeconds();
  const auto start = Clock::now();
  const auto deadline = start + std::chrono::milliseconds(duration_ms);
  std::vector<std::thread> threads;
  for (int t = 0; t < connections; ++t) {
    threads.emplace_back([&, t] {
      Stats& st = per_thread[static_cast<std::size_t>(t)];
      Conn conn;
      if (!conn.Connect(base.host, base.port)) {
        ++st.errors;
        return;
      }
      std::uint64_t seq = 0;
      while (Clock::now() < deadline) {
        const bool counting = Clock::now() - start >= std::chrono::milliseconds(warmup_ms);
        const auto t0 = Clock::now();
        std::string why;
        bool ok = false;
        const std::string name = "bench-" + std::to_string(t) + "-" + std::to_string(seq++);
        //  同 RunLoad：服务端按 `keep_alive_max_count` 主动断连是**正常**行为，
        //  重连后重跑整条链（宁可多一次上传，也不能把正常断连记成错误）。
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
          if (!conn.valid() && !conn.Connect(base.host, base.port)) break;
          ok = OneUploadChain(conn, base, partition, token, payload, name, &why);
          if (!ok) conn.Close();
        }
        if (!ok) {
          ++st.errors;
          if (first_error[static_cast<std::size_t>(t)].empty()) {
            first_error[static_cast<std::size_t>(t)] = why;
          }
          if (!conn.Connect(base.host, base.port)) return;
          continue;
        }
        if (counting) {
          const double us =
              static_cast<double>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()) /
              1000.0;
          st.latency_us.push_back(us);
          ++st.requests;
        }
      }
    });
  }
  for (auto& th : threads) th.join();
  const double seconds =
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count() / 1e6;
  const double cpu_pct =
      seconds > 0 ? (ProcessCpuSeconds() - cpu_before) / seconds * 100.0 : 0;

  std::uint64_t requests = 0;
  std::uint64_t errors = 0;
  std::vector<double> all;
  for (auto& st : per_thread) {
    requests += st.requests;
    errors += st.errors;
    all.insert(all.end(), st.latency_us.begin(), st.latency_us.end());
  }
  double p50 = 0;
  double p99 = 0;
  if (!all.empty()) {
    std::sort(all.begin(), all.end());
    p50 = all[static_cast<std::size_t>(static_cast<double>(all.size() - 1) * 0.50)];
    p99 = all[static_cast<std::size_t>(static_cast<double>(all.size() - 1) * 0.99)];
  }
  std::string first_why;
  for (const auto& why : first_error) {
    if (!why.empty()) {
      first_why = why;
      break;
    }
  }
  std::cout.setf(std::ios::fixed);
  std::cout << "RESULT label=" << label << " mode=chain conns=" << connections
            << " dur_s=" << std::setprecision(2) << seconds << " reqs=" << requests
            << " errs=" << errors << " rps=" << std::setprecision(1)
            << (seconds > 0 ? static_cast<double>(requests) / seconds : 0.0)
            << " mibps=0.0 p50_us=" << std::setprecision(0) << p50 << " p99_us=" << p99
            << " files_s=" << std::setprecision(1)
            << (seconds > 0 ? static_cast<double>(requests) / seconds : 0.0)
            << " client_cpu_pct=" << cpu_pct;
  if (!first_why.empty()) std::cout << " first_error=" << first_why;
  std::cout << "\n";
  return errors > 0 && requests == 0 ? 1 : 0;
}

// ---------------------------------------------------------------- load
int RunLoad(const Args& args) {
  const std::string label = args.value("--label", "unnamed");
  const std::string mode = args.value("--mode", "get");
  const std::string url_text = args.value("--url");
  const int connections = static_cast<int>(args.number("--connections", 1));
  const long long duration_ms = args.number("--duration-ms", 3000);
  const long long warmup_ms = args.number("--warmup-ms", 500);
  const long long body_bytes = args.number("--body-bytes", 0);
  const int expect = static_cast<int>(args.number("--expect-status", mode == "put" ? 200 : 200));
  Url url;
  if (url_text.empty() || !ParseUrl(url_text, &url)) {
    std::cerr << "load: --url 无效\n";
    return 2;
  }
  const std::string body(static_cast<std::size_t>(std::max<long long>(0, body_bytes)), 'x');

  std::vector<Stats> per_thread(static_cast<std::size_t>(connections));
  const double cpu_before = ProcessCpuSeconds();
  const auto start = Clock::now();
  const auto deadline = start + std::chrono::milliseconds(duration_ms);
  std::vector<std::thread> threads;
  for (int t = 0; t < connections; ++t) {
    threads.emplace_back([&, t] {
      Stats& st = per_thread[static_cast<std::size_t>(t)];
      st.latency_us.reserve(1u << 16);
      Conn conn;
      auto connect_if_needed = [&] { return conn.valid() || conn.Connect(url.host, url.port); };
      if (!connect_if_needed()) {
        ++st.errors;
        return;
      }
      std::string request;
      request.reserve(512);
      while (Clock::now() < deadline) {
        const bool counting = Clock::now() - start >= std::chrono::milliseconds(warmup_ms);
        request.clear();
        request += mode == "put" ? "PUT " : "GET ";
        request += url.target;
        request += " HTTP/1.1\r\nHost: ";
        request += url.host;
        request += "\r\nConnection: keep-alive\r\n";
        for (const auto& h : args.headers) {
          request += h;
          request += "\r\n";
        }
        if (mode == "put") {
          request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        request += "\r\n";
        if (mode == "put") request += body;
        const auto t0 = Clock::now();
        int status = 0;
        std::size_t bytes = 0;
        bool done = false;
        //  ★ `keep_alive_max_count`（httplib 默认 100）会让服务端**主动关闭**长连接。
        //    那不是错误，而是协议允许的行为 —— 客户端必须重连并重试**同一次请求**，
        //    否则基准里的 `errs` 会把"正常断连"记成失败（实测 c16 时 183 次）。
        for (int attempt = 0; attempt < 3 && !done; ++attempt) {
          if (!connect_if_needed()) break;
          if (conn.SendAll(request) && conn.ReadResponse(&status, &bytes, nullptr)) {
            done = true;
            break;
          }
          conn.Close();
        }
        if (!done) {
          ++st.errors;
          if (!connect_if_needed()) return;
          continue;
        }
        if (status != expect) {
          ++st.errors;
          if (status == 0) return;
          continue;
        }
        if (counting) {
          const double us = static_cast<double>(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                                                   t0)
                                    .count()) /
                            1000.0;
          st.latency_us.push_back(us);
          ++st.requests;
          st.bytes += bytes;
        }
      }
    });
  }
  for (auto& th : threads) th.join();
  const double seconds =
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count() / 1e6;

  std::uint64_t requests = 0;
  std::uint64_t errors = 0;
  std::uint64_t bytes = 0;
  std::vector<double> all;
  for (auto& st : per_thread) {
    requests += st.requests;
    errors += st.errors;
    bytes += st.bytes;
    all.insert(all.end(), st.latency_us.begin(), st.latency_us.end());
  }
  double p50 = 0;
  double p99 = 0;
  if (!all.empty()) {
    std::sort(all.begin(), all.end());
    p50 = all[static_cast<std::size_t>(static_cast<double>(all.size() - 1) * 0.50)];
    p99 = all[static_cast<std::size_t>(static_cast<double>(all.size() - 1) * 0.99)];
  }
  //  ★ R4/R2 的诚实性检查：把**客户端进程**自己消耗的 CPU 时间也报出来。
  //    若 client_cpu_pct 接近"分配给客户端的核数 × 100"，说明这个点位的数字受**客户端**
  //    限制，不能当作服务端容量（该点位应标注为"无结论"）。
  const double cpu_seconds = ProcessCpuSeconds() - cpu_before;
  const double cpu_pct = seconds > 0 ? cpu_seconds / seconds * 100.0 : 0;
  const double rps = seconds > 0 ? static_cast<double>(requests) / seconds : 0;
  const double mibps = seconds > 0 ? static_cast<double>(bytes) / seconds / (1024.0 * 1024.0) : 0;
  std::cout.setf(std::ios::fixed);
  std::cout.precision(1);
  std::cout << "RESULT label=" << label << " mode=" << mode << " conns=" << connections
            << " dur_s=" << std::setprecision(2) << seconds << " reqs=" << requests
            << " errs=" << errors << " rps=" << std::setprecision(1) << rps
            << " mibps=" << mibps << " p50_us=" << std::setprecision(0) << p50
            << " p99_us=" << p99 << " client_cpu_pct=" << std::setprecision(1) << cpu_pct
            << "\n";
  return errors > 0 && requests == 0 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);
  if (args.positional.empty()) {
    std::cerr << "用法：capacity_bench {seed|load} ...（见文件头）\n";
    return 2;
  }
  const std::string mode = args.positional[0];
  if (mode == "seed") return RunSeed(args);
  if (mode == "load") return RunLoad(args);
  if (mode == "chain") return RunChain(args);
  std::cerr << "未知模式：" << mode << "\n";
  return 2;
}
