// LargeFilePlane 实现。设计理由见头文件。
#include "common/http/large_file_plane.h"

#include "common/ids/id_generator.h"
#include "common/logging/logging.h"
#include "common/net/net.h"
#include "common/time/clock.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fss::http {

namespace {

constexpr int kListenBacklog = 512;         // 与包装层一致（httplib 默认 5 实测丢 SYN）
constexpr std::size_t kHeadChunk = 16 * 1024;
constexpr std::size_t kPathEchoBytes = 256;  // 404 回显路径的上限（与包装层一致）

std::string ToLowerAscii(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string& TrimInPlace(std::string& s) {
  const auto first = s.find_first_not_of(" \t");
  if (first == std::string::npos) {
    s.clear();
    return s;
  }
  const auto last = s.find_last_not_of(" \t");
  s = s.substr(first, last - first + 1);
  return s;
}

//  httplib 在同一张表里给出 reason phrase；这里只用于状态行（用例只断言状态码）。
std::string_view ReasonPhrase(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Error";
  }
}

bool SendAll(int fd, const char* data, std::size_t length) {
  std::size_t sent = 0;
  while (sent < length) {
    const ssize_t n = ::send(fd, data + sent, length - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;  // EPIPE / ECONNRESET / 其它：对端已不可达
    }
    if (n == 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

std::string RemoteAddr(int fd) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return {};
  char host[INET_ADDRSTRLEN] = {0};
  if (::inet_ntop(AF_INET, &addr.sin_addr, host, sizeof(host)) == nullptr) return {};
  return std::string(host) + ":" + std::to_string(ntohs(addr.sin_port));
}

}  // namespace

int DefaultLargeFilePlaneWorkers() {
  const unsigned hw = std::thread::hardware_concurrency();
  const int cores = static_cast<int>(hw == 0 ? 8u : hw);
  return std::max(64, 2 * cores);
}

struct LargeFilePlane::Impl {
  LargeFilePlaneOptions options;
  Handler handler;
  const logging::ILogger* logger = nullptr;
  const IClock* clock = nullptr;
  fss::UuidGenerator ids;

  int listener = -1;
  int bound_port = 0;
  std::string last_error;

  std::thread accept_thread;
  std::vector<std::thread> workers;
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<int> queue;
  bool stopping = false;
  std::atomic<bool> stop_requested{false};

  std::atomic<int> in_flight{0};
  std::atomic<std::uint64_t> connections_total{0};
  std::atomic<std::uint64_t> rejected_busy{0};
  std::atomic<std::uint64_t> sendfile_calls{0};
  std::atomic<std::uint64_t> sendfile_bytes{0};
  std::atomic<std::uint64_t> userspace_calls{0};
  std::atomic<std::uint64_t> userspace_bytes{0};

  int worker_threads = 0;
  int max_connections = 0;

  Impl(LargeFilePlaneOptions opts, Handler h, const logging::ILogger& log, const IClock& clk)
      : options(std::move(opts)), handler(std::move(h)), logger(&log), clock(&clk) {
    worker_threads = options.worker_threads > 0 ? options.worker_threads
                                                : DefaultLargeFilePlaneWorkers();
    max_connections = options.max_connections > 0 ? options.max_connections : worker_threads;
    RegisterMetrics();
  }

  //  ★ ADR-006 §6.2 / §4.4：数据面的诊断计数器必须可见（R11）。
  //    ★ 其中 `_sendfile_*` / `_userspace_*` 是 P5 的判据：只有真的走了对应路径才增长。
  void RegisterMetrics() {
    if (options.metrics == nullptr) return;
    using Kind = fss::metrics::Registry::Kind;
    options.metrics->Register("fss_large_file_plane_sendfile_calls_total", Kind::kCounter,
                              "大文件数据面用 sendfile(2) 服务的下载次数（零拷贝路径）");
    options.metrics->Register("fss_large_file_plane_sendfile_bytes_total", Kind::kCounter,
                              "大文件数据面用 sendfile(2) 送出的字节数");
    options.metrics->Register("fss_large_file_plane_userspace_calls_total", Kind::kCounter,
                              "大文件数据面用用户态 pump 服务的下载次数（含 use_sendfile=false "
                              "与无原生 fd 的来源）");
    options.metrics->Register("fss_large_file_plane_userspace_bytes_total", Kind::kCounter,
                              "大文件数据面用用户态 pump 送出的字节数");
    options.metrics->Register("fss_large_file_plane_rejected_busy_total", Kind::kCounter,
                              "大文件数据面因在途连接上限被 503 拒绝的连接数");
    options.metrics->Register("fss_large_file_plane_accept_errors_total", Kind::kCounter,
                              "大文件数据面 accept(2) 的瞬时错误数（EMFILE/ENFILE 等）");
  }

  bool Bind() {
    listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
      last_error = std::string("socket() 失败：") + std::strerror(errno);
      return false;
    }
    int on = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(options.port));
    if (::inet_pton(AF_INET, options.bind_address.c_str(), &addr.sin_addr) != 1) {
      last_error = "server.http.large_file_plane.bind 不是合法 IPv4 地址：" + options.bind_address;
      ::close(listener);
      listener = -1;
      return false;
    }
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      last_error = "绑定 " + options.bind_address + ":" + std::to_string(options.port) +
                   " 失败：" + std::strerror(errno) +
                   "（该端口可能已被占用；请改 server.http.large_file_plane.port，"
                   "或把 server.http.large_file_plane.enabled 设为 false）";
      ::close(listener);
      listener = -1;
      return false;
    }
    if (::listen(listener, kListenBacklog) != 0) {
      last_error = std::string("listen() 失败：") + std::strerror(errno);
      ::close(listener);
      listener = -1;
      return false;
    }
    sockaddr_in actual{};
    socklen_t len = sizeof(actual);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&actual), &len) == 0) {
      bound_port = ntohs(actual.sin_port);
    } else {
      bound_port = options.port;
    }
    return true;
  }

  bool Start() {
    if (listener >= 0) return true;
    if (!Bind()) return false;
    stop_requested.store(false);
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = false;
    }
    accept_thread = std::thread([this] { AcceptLoop(); });
    workers.reserve(static_cast<std::size_t>(worker_threads));
    for (int i = 0; i < worker_threads; ++i) {
      workers.emplace_back([this] { WorkerLoop(); });
    }
    return true;
  }

  void Stop() {
    if (stop_requested.exchange(true)) return;
    if (listener >= 0) {
      //  先 shutdown 打断阻塞中的 accept，再关 fd（顺序反了会有 fd 复用竞态）。
      ::shutdown(listener, SHUT_RDWR);
      ::close(listener);
      listener = -1;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    cv.notify_all();
    if (accept_thread.joinable()) accept_thread.join();
    for (auto& worker : workers) {
      if (worker.joinable()) worker.join();
    }
    workers.clear();
    std::deque<int> leftover;
    {
      std::lock_guard<std::mutex> lock(mutex);
      leftover.swap(queue);
    }
    for (const int fd : leftover) ::close(fd);
  }

  void AcceptLoop() {
    while (!stop_requested.load()) {
      sockaddr_in addr{};
      socklen_t len = sizeof(addr);
      const int fd = ::accept(listener, reinterpret_cast<sockaddr*>(&addr), &len);
      if (fd < 0) {
        if (errno == EINTR) continue;
        if (stop_requested.load()) break;
        if (errno == ECONNABORTED || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        //  EMFILE/ENFILE 等瞬时资源问题：短暂退避，绝不忙等烧 CPU
        if (options.metrics != nullptr) options.metrics->Increment("fss_large_file_plane_accept_errors_total");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      ConfigureSocket(fd);
      connections_total.fetch_add(1, std::memory_order_relaxed);
      const int now = in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (now > max_connections) {
        rejected_busy.fetch_add(1, std::memory_order_relaxed);
        if (options.metrics != nullptr) {
          options.metrics->Increment("fss_large_file_plane_rejected_busy_total");
        }
        SendBusy(fd);
        ::close(fd);
        in_flight.fetch_sub(1, std::memory_order_acq_rel);
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push_back(fd);
      }
      cv.notify_one();
    }
  }

  static void ConfigureSocket(int fd) {
    int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    SetRecvTimeout(fd, 60);
  }

  static void SetRecvTimeout(int fd, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds > 0 ? seconds : 1;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  //  在途上限：与控制面**同一个** 503 体与 `Retry-After: 1`（ADR-006 §6.1）。
  void SendBusy(int fd) {
    Response response;
    response.status = 503;
    response.headers.Set("Content-Type", "application/json");
    response.headers.Set("Retry-After", "1");
    response.body = fss::http::ErrorBody(
        503, "too many in-flight requests (limit " + std::to_string(max_connections) + ")",
        options.error_format);
    std::string head = "HTTP/1.1 503 Service Unavailable\r\n";
    head += "Content-Type: application/json\r\n";
    head += "Retry-After: 1\r\n";
    head += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    head += "Connection: close\r\n\r\n";
    (void)SendAll(fd, head.data(), head.size());
    (void)SendAll(fd, response.body.data(), response.body.size());
  }

  void WorkerLoop() {
    //  ★ 本线程**只**阻塞 SIGPIPE：`sendfile(2)` 没有 MSG_NOSIGNAL 之类的参数，
    //    对端中途断开时内核会向本线程投递 SIGPIPE；默认动作会**杀掉整个进程**。
    //    在 worker 线程里屏蔽它就足够（同步 SIGPIPE 投递给触发的那个线程），
    //    且不会改变进程内其它线程的信号语义。
    //  ⚠️ 实测（I5）：当前组合根链接的库已经把 SIGPIPE 设成 `SIG_IGN`（`/proc/<pid>/status`
    //     的 `SigIgn` 含 bit12），因此**仅删掉本段并不可观测**（进程不死）；本段是
    //     纵深防御：一旦那个进程级 `SIG_IGN` 的来源消失/改变，它仍然是唯一防线。
    {
      sigset_t blocked;
      ::sigemptyset(&blocked);
      ::sigaddset(&blocked, SIGPIPE);
      ::pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
    }
    while (true) {
      int fd = -1;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return stopping || !queue.empty(); });
        if (queue.empty()) {
          if (stopping) return;
          continue;
        }
        fd = queue.front();
        queue.pop_front();
      }
      if (stop_requested.load()) {
        ::close(fd);
        in_flight.fetch_sub(1, std::memory_order_acq_rel);
        continue;
      }
      ServeConnection(fd);
      ::close(fd);
      in_flight.fetch_sub(1, std::memory_order_acq_rel);
    }
  }

  // ---------------------------------------------------------------------------
  //  请求解析
  // ---------------------------------------------------------------------------
  enum class HeadStatus { kOk, kClosed, kTimeout, kTooLarge };

  HeadStatus ReadHead(int fd, std::string& pending, std::size_t* head_end) {
    while (true) {
      const auto pos = pending.find("\r\n\r\n");
      if (pos != std::string::npos) {
        *head_end = pos;
        return HeadStatus::kOk;
      }
      if (static_cast<std::int64_t>(pending.size()) >
          options.max_header_bytes + options.max_uri_bytes) {
        return HeadStatus::kTooLarge;
      }
      char buffer[kHeadChunk];
      const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
      if (n == 0) return HeadStatus::kClosed;
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return HeadStatus::kTimeout;
        return HeadStatus::kClosed;
      }
      pending.append(buffer, static_cast<std::size_t>(n));
    }
  }

  struct Parsed {
    bool ok = false;
    int error_status = 0;
    std::string error_detail;
    Method method = Method::kUnknown;
    std::string method_raw;
    std::string target;
    std::string path;
    std::string query;
    HeaderMap headers;
    std::int64_t content_length = -1;
    bool is_chunked = false;
  };

  Parsed ParseHead(const std::string& head) const {
    Parsed parsed;
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (true) {
      const auto end = head.find("\r\n", start);
      if (end == std::string::npos) {
        lines.push_back(head.substr(start));
        break;
      }
      lines.push_back(head.substr(start, end - start));
      start = end + 2;
    }
    if (lines.empty() || lines[0].empty()) {
      parsed.error_status = 400;
      parsed.error_detail = "malformed request line";
      return parsed;
    }
    const std::string& request_line = lines[0];
    const auto sp1 = request_line.find(' ');
    const auto sp2 = sp1 == std::string::npos ? std::string::npos : request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
      parsed.error_status = 400;
      parsed.error_detail = "malformed request line";
      return parsed;
    }
    parsed.method_raw = request_line.substr(0, sp1);
    parsed.target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    parsed.method = ParseMethod(parsed.method_raw);
    if (static_cast<std::int64_t>(parsed.target.size()) > options.max_uri_bytes) {
      parsed.error_status = 414;
      parsed.error_detail = "request target exceeds " + std::to_string(options.max_uri_bytes) +
                            " bytes";
      return parsed;
    }
    if (const auto q = parsed.target.find('?'); q != std::string::npos) {
      parsed.path = parsed.target.substr(0, q);
      parsed.query = parsed.target.substr(q + 1);
    } else {
      parsed.path = parsed.target;
    }
    for (std::size_t i = 1; i < lines.size(); ++i) {
      const std::string& line = lines[i];
      if (line.empty()) continue;
      if (line[0] == ' ' || line[0] == '\t') {
        parsed.error_status = 400;
        parsed.error_detail = "obsolete line folding is not accepted";
        return parsed;
      }
      const auto colon = line.find(':');
      if (colon == std::string::npos || colon == 0) {
        parsed.error_status = 400;
        parsed.error_detail = "malformed header line";
        return parsed;
      }
      std::string name = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      parsed.headers.Add(name, TrimInPlace(value));
    }
    const auto cl = parsed.headers.Get("Content-Length");
    if (cl.has_value()) {
      char* end = nullptr;
      const auto value = std::strtoll(cl->c_str(), &end, 10);
      if (end != nullptr && *end == '\0' && value >= 0) parsed.content_length = value;
    }
    const auto te = parsed.headers.Get("Transfer-Encoding");
    if (te.has_value()) {
      parsed.is_chunked = ToLowerAscii(*te).find("chunked") != std::string::npos;
    }
    parsed.ok = true;
    return parsed;
  }

  // ---------------------------------------------------------------------------
  //  响应渲染与写出
  // ---------------------------------------------------------------------------
  struct Outgoing {
    int status = 200;
    HeaderMap headers;
    std::string body;
    std::shared_ptr<bytes::ByteSource> stream;
    std::int64_t offset = 0;
    std::int64_t length = 0;
    bool head_only = false;
    bool close_connection = false;
    std::string note;
  };

  Response HandlerError(int status, const std::string& detail) const {
    Response response;
    response.status = status;
    response.headers.Set("Content-Type", "application/json");
    response.body = fss::http::ErrorBody(status, detail, options.error_format);
    return response;
  }

  //  Range 由数据面**自己**成帧（控制面交给 httplib）；这里只用 L1 的 Range 原语。
  //  ★ 多段（multipart/byteranges）在此**显式降级为 200 全量**（ADR-006 §6.1 的"多段降级"），
  //    而控制面对可寻址来源会返回 multipart 206 —— 这是**唯一**被文档化的行为差异，
  //    用例 P2/P8 同时断言两侧的实际值（绝不静默）。
  void Prepare(const Request& request, Response& response, Outgoing* out) const {
    out->status = response.status;
    out->headers = response.headers;
    out->body = std::move(response.body);
    out->stream = response.stream;
    out->head_only = request.is_head;

    std::int64_t total = response.stream_length;
    if (out->stream != nullptr && total < 0) {
      //  未知长度的流式响应需要 chunked；与包装层一样明确失败，绝不猜长度。
      out->stream = nullptr;
      out->status = 500;
      out->headers.Clear();
      out->headers.Set("Content-Type", "application/json");
      out->body = fss::http::ErrorBody(500, "unknown-length stream is not supported yet",
                                       options.error_format);
      return;
    }
    if (out->stream == nullptr) return;

    out->length = total;
    const auto range_header = request.headers.Get("Range");
    if (!range_header.has_value()) return;
    const auto parsed = bytes::ParseRangeHeader(*range_header, total);
    switch (parsed.status) {
      case bytes::RangeStatus::kAbsent:
        return;
      case bytes::RangeStatus::kSingle: {
        const auto& range = parsed.ranges.front();
        out->status = 206;
        out->offset = range.first;
        out->length = range.Length();
        out->headers.Set("Content-Range", bytes::FormatContentRange(range, total));
        return;
      }
      case bytes::RangeStatus::kMultiple:
        //  降级：见上方说明。刻意**不**写 note（保持访问日志字段集与控制面一致）。
        out->status = 200;
        out->offset = 0;
        out->length = total;
        return;
      case bytes::RangeStatus::kMalformed:
        //  RFC 7233 §4.4：语法非法 → 忽略该头 → 200 全量（与包装层一致）。
        out->status = 200;
        out->offset = 0;
        out->length = total;
        return;
      case bytes::RangeStatus::kUnsatisfiable: {
        out->stream = nullptr;
        out->status = 416;
        out->headers.Clear();
        out->headers.Set("Content-Type", "application/json");
        out->headers.Set("Content-Range", bytes::FormatUnsatisfiedContentRange(total));
        out->body = fss::http::ErrorBody(416, parsed.detail, options.error_format);
        out->length = 0;
        out->note = "range_unsatisfiable_416";
        return;
      }
    }
  }

  void AddMissingHeader(HeaderMap& headers, std::string_view name, std::string value) const {
    if (!headers.Has(name)) headers.Set(std::string(name), std::move(value));
  }

  bool WriteResponse(int fd, const Request& request, Outgoing& out, std::size_t served) {
    const bool body_expected = out.stream != nullptr;
    if (!body_expected && out.headers.Get("Content-Type") == std::nullopt && !out.body.empty()) {
      out.headers.Set("Content-Type", "text/plain");
    }
    const std::int64_t content_length =
        body_expected ? out.length : static_cast<std::int64_t>(out.body.size());

    out.headers.Remove("Content-Length");
    out.headers.Set("Content-Length", std::to_string(content_length));
    const bool close_after =
        out.close_connection ||
        static_cast<int>(served) + 1 >= std::max(1, options.keep_alive_max_count) ||
        request.headers.Get("Connection").value_or("") == "close";
    out.close_connection = close_after;
    if (close_after) out.headers.Set("Connection", "close");

    std::string head = "HTTP/1.1 " + std::to_string(out.status) + " " +
                       std::string(ReasonPhrase(out.status)) + "\r\n";
    for (const auto& [name, value] : out.headers.entries()) {
      head += name;
      head += ": ";
      head += value;
      head += "\r\n";
    }
    head += "\r\n";
    if (!SendAll(fd, head.data(), head.size())) return false;
    if (out.head_only) return true;

    if (out.stream != nullptr) {
      return PumpStream(fd, out);
    }
    if (!out.body.empty() && !SendAll(fd, out.body.data(), out.body.size())) return false;
    return true;
  }

  Response CallHandler(Request& request) {
    try {
      return handler(request);
    } catch (const std::exception& error) {
      return HandlerError(500, std::string("未预期异常：") + error.what());
    } catch (...) {
      return HandlerError(500, "未预期异常");
    }
  }

  bool PumpStream(int fd, Outgoing& out) {
    if (out.length <= 0) return true;
    const auto seeked = out.stream->Seek(out.offset);
    if (!seeked.ok()) {
      //  已经发过响应头，无法再改状态码；如实记日志并断开。
      if (logger != nullptr) {
        logging::Warn(*logger, "large_file_plane_seek_failed",
                      {{"error", seeked.error().message()}});
      }
      return false;
    }
    const int native_fd = out.stream->NativeFd();
    std::int64_t sent = 0;
    bool ok = true;
    bool used_sendfile = false;

    if (native_fd >= 0 && options.use_sendfile) {
      used_sendfile = true;
      off_t offset = static_cast<off_t>(out.offset);
      std::int64_t remaining = out.length;
      const std::int64_t chunk =
          options.sendfile_chunk_bytes > 0 ? options.sendfile_chunk_bytes : out.length;
      while (remaining > 0) {
        const auto want = static_cast<std::size_t>(
            std::min<std::int64_t>(remaining, std::max<std::int64_t>(1, chunk)));
        const ssize_t n = ::sendfile(fd, native_fd, &offset, want);
        if (n < 0) {
          if (errno == EINTR) continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // 阻塞 socket 下罕见
          ok = false;
          break;
        }
        if (n == 0) break;
        remaining -= static_cast<std::int64_t>(n);
        sent += static_cast<std::int64_t>(n);
      }
    } else {
      const std::size_t capacity = options.userspace_buffer_bytes > 0
                                       ? static_cast<std::size_t>(options.userspace_buffer_bytes)
                                       : 64 * 1024;
      std::vector<char> buffer(capacity);
      std::int64_t remaining = out.length;
      while (remaining > 0) {
        const auto want = static_cast<std::size_t>(
            std::min<std::int64_t>(remaining, static_cast<std::int64_t>(buffer.size())));
        const auto read = out.stream->Read(buffer.data(), want);
        if (!read.ok()) {
          ok = false;
          break;
        }
        if (read.value() == 0) break;  // 提前 EOF：与"声明长度"不符 → 不算完整送达
        if (!SendAll(fd, buffer.data(), read.value())) {
          ok = false;
          break;
        }
        remaining -= static_cast<std::int64_t>(read.value());
        sent += static_cast<std::int64_t>(read.value());
      }
    }

    const bool complete = ok && sent == out.length;
    //  ★ 计数点移动：**只有 `sendfile` 这一条路**绕过了 `ByteSource::Read`（以及计量
    //    装饰器对用户态拷贝的计数），所以只有它需要在这里补记同一个指标族/标签
    //    （由组合根接到 `MeteredBlobStore::RecordNativeRead`）。用户态 pump 的字节由
    //    `MeteredNativeSource` 记 —— 二者互斥，绝不重复计数。
    if (used_sendfile && options.on_native_bytes) options.on_native_bytes(sent, complete);
    if (used_sendfile) {
      sendfile_calls.fetch_add(1, std::memory_order_relaxed);
      sendfile_bytes.fetch_add(static_cast<std::uint64_t>(sent), std::memory_order_relaxed);
      if (options.metrics != nullptr) {
        options.metrics->Increment("fss_large_file_plane_sendfile_calls_total");
        if (sent > 0) {
          options.metrics->Increment("fss_large_file_plane_sendfile_bytes_total", {}, sent);
        }
      }
    } else {
      userspace_calls.fetch_add(1, std::memory_order_relaxed);
      userspace_bytes.fetch_add(static_cast<std::uint64_t>(sent), std::memory_order_relaxed);
      if (options.metrics != nullptr) {
        options.metrics->Increment("fss_large_file_plane_userspace_calls_total");
        if (sent > 0) {
          options.metrics->Increment("fss_large_file_plane_userspace_bytes_total", {}, sent);
        }
      }
    }
    return complete;
  }

  // ---------------------------------------------------------------------------
  //  单条连接（keep-alive 循环）
  // ---------------------------------------------------------------------------
  void ServeConnection(int fd) {
    std::string pending;
    std::size_t served = 0;
    while (!stop_requested.load()) {
      SetRecvTimeout(fd, served == 0 ? options.idle_timeout_sec : options.keep_alive_timeout_sec);
      std::size_t head_end = 0;
      const auto head_status = ReadHead(fd, pending, &head_end);
      if (head_status == HeadStatus::kTimeout) {
        if (!pending.empty() || served == 0) {
          Response response = HandlerError(
              408, "request head idle timeout (no progress within " +
                       std::to_string(options.idle_timeout_sec) + "s)");
          Outgoing out;
          Request request;
          request.method_raw = "";
          Prepare(request, response, &out);
          out.close_connection = true;
          out.note = "request_head_idle_timeout_408";
          (void)WriteResponse(fd, request, out, served);
          if (logger != nullptr) {
            fss::http::LogAccess(*logger, request, response,
                                 static_cast<std::int64_t>(0), out.note);
          }
        }
        return;
      }
      if (head_status == HeadStatus::kClosed) return;
      if (head_status == HeadStatus::kTooLarge) {
        Response response = HandlerError(431, "request head exceeds header limit");
        Outgoing out;
        Request request;
        Prepare(request, response, &out);
        out.close_connection = true;
        (void)WriteResponse(fd, request, out, served);
        return;
      }

      const std::string head = pending.substr(0, head_end);
      pending.erase(0, head_end + 4);

      const auto started = clock != nullptr ? clock->NowEpochMillis() : 0;
      Request request;
      request.remote_addr = RemoteAddr(fd);
      const auto parsed = ParseHead(head);
      const auto finish = [&](Outgoing& out, Response& response, std::string_view note,
                              bool force_close) {
        out.close_connection = out.close_connection || force_close;
        const bool delivered = WriteResponse(fd, request, out, served);
        if (logger != nullptr && clock != nullptr) {
          response.status = out.status;
          fss::http::LogAccess(*logger, request, response, clock->NowEpochMillis() - started,
                               note);
        }
        ++served;
        if (!delivered || out.close_connection || !pending.empty()) {
          //  写失败 / 显式关闭 / 有流水线残包：统一断开，避免"以为还在同一请求边界"。
          out.close_connection = true;
        }
      };

      if (!parsed.ok) {
        Response response = HandlerError(parsed.error_status, parsed.error_detail);
        Outgoing out;
        Prepare(request, response, &out);
        //  走私检查/解析错误的 note 与控制面一致（字段集同源）。
        std::string note;
        if (parsed.error_status == 414) note = "uri_too_long_414";
        finish(out, response, note, /*force_close=*/true);
        return;
      }

      request.method = parsed.method;
      request.method_raw = parsed.method_raw;
      request.path = parsed.path;
      request.query = parsed.query;
      request.params = net::ParseQuery(parsed.query).value_or(net::QueryParams{});
      request.headers = parsed.headers;
      request.content_length = parsed.content_length;
      request.is_chunked = parsed.is_chunked;
      request.is_head = parsed.method == Method::kHead;
      if (const auto cid = request.headers.Get("x-correlation-id");
          cid.has_value() && !cid->empty()) {
        request.correlation_id = *cid;
      } else if (clock != nullptr) {
        request.correlation_id = ids.NewUuid();
      }

      //  ---- 请求走私防护（与控制面**逐字同源**的消息）----
      const auto cl_count = request.headers.Count("Content-Length");
      if (cl_count > 1) {
        Response response = HandlerError(400, "duplicate Content-Length headers");
        Outgoing out;
        Prepare(request, response, &out);
        Response logged = response;
        finish(out, logged, "duplicate_content_length", /*force_close=*/true);
        return;
      }
      if (request.is_chunked && cl_count == 1) {
        Response response =
            HandlerError(400, "Content-Length and Transfer-Encoding: chunked together");
        Outgoing out;
        Prepare(request, response, &out);
        Response logged = response;
        finish(out, logged, "content_length_with_chunked", /*force_close=*/true);
        return;
      }

      //  ---- 路由：本数据面只认 `/<base>/v1/transfer/{token}` 上的 GET/HEAD ----
      const std::string prefix = options.base_path + "/v1/transfer/";
      const bool on_transfer_path =
          request.path.rfind(prefix, 0) == 0 &&
          request.path.find('/', prefix.size()) == std::string::npos;
      if (!on_transfer_path) {
        std::string shown = request.path.size() > kPathEchoBytes
                                ? request.path.substr(0, kPathEchoBytes) + "…"
                                : request.path;
        Response response =
            HandlerError(404, "no route for " + request.method_raw + " " + shown);
        Outgoing out;
        Prepare(request, response, &out);
        Response logged = response;
        finish(out, logged, "route_not_found", /*force_close=*/true);
        return;
      }
      if (request.method != Method::kGet && request.method != Method::kHead) {
        //  ★ 只下载：`PUT`/`POST`/... 一律 405（控制面对 `PUT` 有上传语义 ——
        //    这是"范围决策"的已知差异，登记在 ADR-006 与本用例的文档里）。
        Response response = HandlerError(
            405, "method not allowed on the download plane: " + request.method_raw +
                     " (only GET/HEAD)");
        response.headers.Set("Allow", "GET, HEAD");
        Outgoing out;
        Prepare(request, response, &out);
        Response logged = response;
        finish(out, logged, "method_not_allowed_405", /*force_close=*/true);
        return;
      }

      request.route_name = "transfer.get";
      request.route_params.clear();
      request.route_params.emplace_back("token", request.path.substr(prefix.size()));

      Response response = CallHandler(request);
      Outgoing out;
      Prepare(request, response, &out);
      const std::string note = out.note;
      const bool keep_alive = out.status < 400;
      Response logged = response;
      finish(out, logged, note, /*force_close=*/!keep_alive);
      if (out.close_connection || !keep_alive) return;
    }
  }
};

LargeFilePlane::LargeFilePlane(LargeFilePlaneOptions options, Handler handler,
                               const logging::ILogger& logger, const IClock& clock)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(handler), logger, clock)) {}

LargeFilePlane::~LargeFilePlane() { Stop(); }

bool LargeFilePlane::Start() { return impl_->Start(); }
void LargeFilePlane::Stop() { impl_->Stop(); }
int LargeFilePlane::port() const { return impl_->bound_port; }
const std::string& LargeFilePlane::last_error() const { return impl_->last_error; }

LargeFilePlaneStats LargeFilePlane::stats() const {
  LargeFilePlaneStats out;
  out.connections_total = impl_->connections_total.load();
  out.rejected_busy = impl_->rejected_busy.load();
  out.sendfile_calls = impl_->sendfile_calls.load();
  out.sendfile_bytes = impl_->sendfile_bytes.load();
  out.userspace_calls = impl_->userspace_calls.load();
  out.userspace_bytes = impl_->userspace_bytes.load();
  out.in_flight = impl_->in_flight.load();
  out.worker_threads = impl_->worker_threads;
  out.max_connections = impl_->max_connections;
  return out;
}

}  // namespace fss::http
