// =============================================================================
//  fss_http 实现 —— 唯一允许 include <httplib.h> 的地方（ADR-002 §5.1）
// =============================================================================
//
//  本文件的结构：
//    ① 小工具（头名比较、错误体、计数 receiver、BodyPump、StreamPump）
//    ② Server::Impl（路由表、匹配、分发、Range 归一化、访问日志、统计）
//    ③ 公开接口（AddRoute / Bind / Start / Stop / stats…）
//
//  三条必须守住的防线：
//    H-2①  `Content-Length` 超限 → 413，**一个字节都不读**（handler 不可能有副作用）
//    H-2②  chunked 超限 → 计数 reader 立即中止 → 400；**即使 handler 吞掉错误返回 201
//           也强制改回 400**（静默数据丢失比报错严重得多）
//    H-2③  实际读取 ≠ 声明的 `Content-Length` → 400（防静默截断）
//    H-1   在把 Range 交给库之前先按契约 §1.8 归一化：不可满足 → 我们自己回 416，
//           语法非法 → 删掉该头。越界路径因此**不可达**。
#include "common/http/http.h"

#include "common/ids/id_generator.h"
#include "common/logging/logging.h"
#include "common/time/clock.h"

// -----------------------------------------------------------------------------
//  第三方库的编译期配置：必须在 include 之前定义
// -----------------------------------------------------------------------------
//  ★ CPPHTTPLIB_LISTEN_BACKLOG：库把它**硬编码为 5**（httplib.h:181），且没有运行时
//    接口。后果实测：50 个连接同时到来时，accept 队列只有 5 个位置 → 内核丢弃 SYN →
//    客户端按 1s/3s/7s 重传。实测 50 个 400ms 的请求总耗时 **1436 ms**（本该 ~400 ms），
//    且只有 34 个请求在 200 ms 内真正开始处理。
//    这对 OSDU 的场景（并发上传/下载、LB 健康检查突发）是硬伤，因此包装层在 include
//    前把它提到 512。只在**这一个 TU** 里定义 —— 全仓库唯一 include httplib.h 的地方
//    （分层护栏强制），所以不存在宏不一致的问题。
//  ⚠️ 不要在这里改 `CPPHTTPLIB_REQUEST_URI_MAX_LENGTH` / `HEADER_MAX_LENGTH`：
//     契约 §1.7 的数值（8191 / 8192）是按默认值**实测**固化的，改了就要重测并改契约。
#ifndef CPPHTTPLIB_LISTEN_BACKLOG
#define CPPHTTPLIB_LISTEN_BACKLOG 512
#endif

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace fss::http {
namespace {

std::string ToLowerAscii(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

std::string JsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// -----------------------------------------------------------------------------
//  错误体归一化（契约 §1.6 / §1.7）
// -----------------------------------------------------------------------------
//  为什么必须自己生成：httplib 在解析失败时会给一个纯文本小页面（"Bad Request"），
//  直接漏出去就破坏了错误体契约。适配层（P4）还没进来，所以这里先按契约形态输出。
//  详细映射（含 OSDU 的固定消息）由 P4 的 ErrorMapping 接管，本层只保证"格式合法 + 状态正确"。
std::string ErrorBody(const ServerOptions& options, int status, std::string_view detail) {
  const char* message = "Error";
  switch (status) {
    case 400: message = "Bad Request"; break;
    case 404: message = "Not Found"; break;
    case 405: message = "Method Not Allowed"; break;
    case 409: message = "Conflict"; break;
    case 413: message = "Payload Too Large"; break;
    case 416: message = "Range Not Satisfiable"; break;
    case 429: message = "Too Many Requests"; break;
    case 500: message = "Internal Server Error"; break;
    case 501: message = "Not Implemented"; break;
    case 502: message = "Bad Gateway"; break;
    case 503: message = "Service Unavailable"; break;
    default: break;
  }
  const std::string esc = JsonEscape(detail);
  if (options.error_format == "legacy") {
    std::string out = "{\"error\":{\"code\":" + std::to_string(status) + ",\"message\":\"" +
                      message + "\",\"errors\":[";
    if (!esc.empty()) out += "{\"reason\":\"" + esc + "\"}";
    out += "]}}";
    return out;
  }
  if (options.error_format == "api_error") {
    std::string out = "{\"code\":" + std::to_string(status) + ",\"reason\":\"" + message +
                      "\",\"message\":\"" + (esc.empty() ? std::string(message) : esc) + "\"}";
    return out;
  }
  std::string out = "{\"code\":" + std::to_string(status) + ",\"message\":\"" + message +
                    "\",\"details\":[";
  if (!esc.empty()) out += "{\"reason\":\"" + esc + "\"}";
  out += "]}";
  return out;
}

Response ErrorResponse(const ServerOptions& options, int status, std::string detail) {
  Response res;
  res.status = status;
  res.body = ErrorBody(options, status, detail);
  res.headers.Set("Content-Type", "application/json");
  return res;
}

// -----------------------------------------------------------------------------
//  计数 receiver：一边读一边判上限（**不缓冲整块**）
// -----------------------------------------------------------------------------
struct CountingReceiver {
  std::string* buffer = nullptr;    // 非空 → 缓冲模式（小 JSON body）
  bool* stop_requested = nullptr;   // 非空 → 流式模式：置位即中止读取
  std::function<bool(const char*, std::size_t)> forward;  // 流式模式的消费者
  std::int64_t limit = 0;           // 0 = 不限
  std::int64_t read = 0;
  bool exceeded = false;

  //  ---- 超时（C4.11）----
  //  `overall_timeout_ms` 为 0 = **没有整体超时**（数据面）；`idle_timeout_ms` 为 0 = 不检查。
  //  两者都在**每次收到数据**时检查：纯"空闲"（一个字节都不来）由 socket 级
  //  SO_RCVTIMEO 兜住，这里兜的是"整体拖太久"和"来一块停很久"。
  std::int64_t overall_timeout_ms = 0;
  std::int64_t idle_timeout_ms = 0;
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point last_progress = started;
  bool timed_out = false;

  bool operator()(const char* data, std::size_t len) {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
    if (overall_timeout_ms > 0 && elapsed_ms > overall_timeout_ms) {
      timed_out = true;
      return false;
    }
    if (idle_timeout_ms > 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress).count() >
            idle_timeout_ms) {
      timed_out = true;
      return false;
    }
    //  ★ 只有**真的收到了字节**才算"有进展"：库在读失败时会回调一次零长度，
    //    若把它当成进展，`HandleBodyError` 里的空闲判定就会算出 0ms 而退化成 400
    //    （实测踩到：空闲超时被报成 "failed to read request body"）。
    if (len > 0) last_progress = now;
    if (limit > 0 && read + static_cast<std::int64_t>(len) > limit) {
      exceeded = true;
      return false;  // ★ H-2②：立即中止，绝不是"读完再判断"
    }
    read += static_cast<std::int64_t>(len);
    if (buffer != nullptr) {
      buffer->append(data, len);
      return true;
    }
    if (forward && !forward(data, len)) return false;
    if (stop_requested != nullptr && *stop_requested) return false;
    return true;
  }
};

// -----------------------------------------------------------------------------
//  BodyPump：把 httplib 的 push 型 ContentReader 适配成 pull 型 ByteSource
// -----------------------------------------------------------------------------
//  为什么需要：`ContentReader` 只能在一次调用里把**整个** body 推给回调，
//  没有"读一段、暂停、再读"的接口；而存储层是 pull 模型（写盘要控节奏、要有背压）。
//  有界队列 + 一个泵线程把 push 变成 pull：
//    · 队列有界 → 内存与 body 大小无关（C1.3 的 1 GiB 上传靠这条）
//    · 队列满时泵线程阻塞 → 天然背压（客户端发太快就被 TCP 窗口压住）
class BodyPump final : public bytes::ByteSource {
 public:
  BodyPump(std::function<bool(std::function<bool(const char*, std::size_t)>)> pump,
           std::size_t capacity)
      : pump_(std::move(pump)), capacity_(capacity == 0 ? 64 * 1024 : capacity) {}

  ~BodyPump() override { Stop(); }

  void Start() {
    thread_ = std::thread([this] {
      bool ok = false;
      try {
        ok = pump_([this](const char* d, std::size_t n) { return Push(d, n); });
      } catch (...) {
        ok = false;
      }
      std::lock_guard<std::mutex> lock(mu_);
      finished_ = true;
      pump_ok_ = ok;
      cv_data_.notify_all();
    });
  }

  bool Push(const char* data, std::size_t len) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_space_.wait(lock, [&] { return stop_ || buffered_ < capacity_; });
    if (stop_) return false;
    buf_.insert(buf_.end(), data, data + len);
    buffered_ += len;
    pushed_ += len;
    cv_data_.notify_all();
    return true;
  }

  Result<std::size_t> Read(char* out, std::size_t capacity) override {
    std::unique_lock<std::mutex> lock(mu_);
    cv_data_.wait(lock, [&] { return !buf_.empty() || finished_ || stop_; });
    if (!buf_.empty()) {
      const auto n = std::min<std::size_t>(capacity, buf_.size());
      std::copy(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(n), out);
      buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(n));
      buffered_ -= n;
      cv_space_.notify_all();
      return n;
    }
    if (!finished_) return std::size_t{0};
    if (!pump_ok_ && !stop_) {
      // 读取失败（含计数 reader 中止）必须报错，绝不能当 EOF ——
      // H-2 的教训就是"把读失败当读完"，于是得到 0 字节的"成功"上传。
      return Err(ErrorKind::kInvalidArgument, "请求体读取失败或被中止");
    }
    return std::size_t{0};
  }

  void SignalStop() {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    cv_data_.notify_all();
    cv_space_.notify_all();
  }
  void Stop() {
    SignalStop();
    if (thread_.joinable()) thread_.join();
  }
  std::uint64_t pushed() const {
    std::lock_guard<std::mutex> lock(mu_);
    return pushed_;
  }

 private:
  std::function<bool(std::function<bool(const char*, std::size_t)>)> pump_;
  std::size_t capacity_;
  std::thread thread_;
  mutable std::mutex mu_;
  std::condition_variable cv_data_;
  std::condition_variable cv_space_;
  std::deque<char> buf_;
  std::size_t buffered_ = 0;
  std::uint64_t pushed_ = 0;
  bool finished_ = false;
  bool pump_ok_ = true;
  bool stop_ = false;
};

// -----------------------------------------------------------------------------
//  StreamPump：把 ByteSource 按 (offset,length) 写给 httplib
// -----------------------------------------------------------------------------
//  httplib 的 provider 契约（`detail::write_content_with_progress`）：
//    循环 `while (offset < end) content_provider(offset, end - offset, sink)`
//    → provider **每次必须至少写入 1 字节后返回 true**，否则死循环；
//    → 写不出来的唯一正确做法是返回 false（库会判定为 Canceled 并关闭连接）。
class StreamPump {
 public:
  StreamPump(std::shared_ptr<bytes::ByteSource> source, std::int64_t total)
      : source_(std::move(source)), total_(total) {
    buf_.resize(64 * 1024);
  }

  bool operator()(std::size_t offset, std::size_t length, httplib::DataSink& sink) {
    if (failed_) return false;
    const auto want_offset = static_cast<std::int64_t>(offset);
    if (want_offset != pos_) {
      if (!JumpTo(want_offset)) {
        failed_ = true;
        return false;
      }
    }
    const auto want = std::min<std::size_t>(length, buf_.size());
    if (want == 0) return true;
    auto n = source_->Read(buf_.data(), want);
    if (!n.ok()) {
      failed_ = true;
      return false;
    }
    if (n.value() == 0) {
      // 声明了长度却提前 EOF：这是**错误**（不是正常结束）。返回 false 让库中止，
      // 而不是返回 true 且不写数据 —— 那会让 httplib 陷入死循环。
      failed_ = true;
      return false;
    }
    pos_ += static_cast<std::int64_t>(n.value());
    if (!sink.write(buf_.data(), n.value())) {
      failed_ = true;
      return false;
    }
    return true;
  }

  std::int64_t position() const { return pos_; }
  bool failed() const { return failed_; }
  std::int64_t total() const { return total_; }

 private:
  bool JumpTo(std::int64_t target) {
    if (source_->Seekable()) {
      if (!source_->Seek(target).ok()) return false;
      pos_ = target;
      return true;
    }
    // 不可寻址的来源：只支持**前向**跳过（单段 Range 的正常情形）
    if (target < pos_) return false;
    std::vector<char> discard(64 * 1024);
    while (pos_ < target) {
      const auto step = static_cast<std::size_t>(
          std::min<std::int64_t>(static_cast<std::int64_t>(discard.size()), target - pos_));
      auto n = source_->Read(discard.data(), step);
      if (!n.ok() || n.value() == 0) return false;
      pos_ += static_cast<std::int64_t>(n.value());
    }
    return true;
  }

  std::shared_ptr<bytes::ByteSource> source_;
  std::int64_t total_ = -1;
  std::int64_t pos_ = 0;
  bool failed_ = false;
  std::vector<char> buf_;
};

}  // namespace

// =============================================================================
//  HeaderMap
// =============================================================================
void HeaderMap::Set(std::string name, std::string value) {
  for (auto& [k, v] : entries_) {
    if (EqualsNoCase(k, name)) {
      v = std::move(value);
      return;
    }
  }
  entries_.emplace_back(std::move(name), std::move(value));
}

void HeaderMap::Add(std::string name, std::string value) {
  entries_.emplace_back(std::move(name), std::move(value));
}

bool HeaderMap::Remove(std::string_view name) {
  const auto before = entries_.size();
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const auto& kv) { return EqualsNoCase(kv.first, name); }),
                 entries_.end());
  return entries_.size() != before;
}

std::optional<std::string> HeaderMap::Get(std::string_view name) const {
  for (const auto& [k, v] : entries_) {
    if (EqualsNoCase(k, name)) return v;
  }
  return std::nullopt;
}

bool HeaderMap::Has(std::string_view name) const { return Get(name).has_value(); }

std::size_t HeaderMap::Count(std::string_view name) const {
  std::size_t n = 0;
  for (const auto& [k, v] : entries_) {
    if (EqualsNoCase(k, name)) ++n;
  }
  return n;
}

// =============================================================================
//  Method
// =============================================================================
Method ParseMethod(std::string_view raw) {
  if (raw == "GET") return Method::kGet;
  if (raw == "POST") return Method::kPost;
  if (raw == "PUT") return Method::kPut;
  if (raw == "DELETE") return Method::kDelete;
  if (raw == "HEAD") return Method::kHead;
  if (raw == "OPTIONS") return Method::kOptions;
  if (raw == "PATCH") return Method::kPatch;
  return Method::kUnknown;
}

std::string_view MethodName(Method method) {
  switch (method) {
    case Method::kGet: return "GET";
    case Method::kPost: return "POST";
    case Method::kPut: return "PUT";
    case Method::kDelete: return "DELETE";
    case Method::kHead: return "HEAD";
    case Method::kOptions: return "OPTIONS";
    case Method::kPatch: return "PATCH";
    case Method::kUnknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

std::optional<std::string> Request::Param(std::string_view name) const {
  for (const auto& [k, v] : route_params) {
    if (k == name) return v;
  }
  return std::nullopt;
}

// =============================================================================
//  Response
// =============================================================================
Response Response::Text(int status, std::string body) {
  Response res;
  res.status = status;
  res.body = std::move(body);
  res.headers.Set("Content-Type", "text/plain");
  return res;
}

Response Response::Json(int status, std::string body) {
  Response res;
  res.status = status;
  res.body = std::move(body);
  res.headers.Set("Content-Type", "application/json");
  return res;
}

Response Response::Empty(int status) {
  Response res;
  res.status = status;
  return res;
}

Response Response::Stream(int status, std::string content_type,
                          std::shared_ptr<bytes::ByteSource> source, std::int64_t length) {
  Response res;
  res.status = status;
  res.stream = std::move(source);
  res.stream_length = length;
  res.headers.Set("Content-Type", std::move(content_type));
  res.headers.Set("Accept-Ranges", "bytes");
  return res;
}

// =============================================================================
//  选项校验与默认值
// =============================================================================
int DefaultWorkerThreads(const ServerOptions& options) {
  const unsigned hw = std::thread::hardware_concurrency();
  const int base = static_cast<int>(hw == 0 ? 8u : hw) * 4;
  int n = std::max(16, base);
  if (options.transfer_buffer_bytes > 0 && options.transfer_memory_budget_bytes > 0) {
    const auto by_mem = options.transfer_memory_budget_bytes / options.transfer_buffer_bytes;
    n = std::min<int>(n, static_cast<int>(std::max<std::int64_t>(1, by_mem)));
  }
  return std::max(1, n);
}

Result<void> ValidateOptions(const ServerOptions& options) {
  if (options.port < 0 || options.port > 65535) {
    return Err(ErrorKind::kInvalidArgument, "server.http.port 必须在 [0, 65535] 内");
  }
  if (options.worker_threads < 0) {
    return Err(ErrorKind::kInvalidArgument, "server.http.worker_threads 不能为负");
  }
  if (options.max_connections < 0) {
    return Err(ErrorKind::kInvalidArgument, "server.http.max_connections 不能为负");
  }
  const int workers = options.worker_threads > 0 ? options.worker_threads
                                                 : DefaultWorkerThreads(options);
  if (options.max_connections > 0 && options.max_connections > workers) {
    // ★ 这条很关键：若在途上限 > 线程数，超限请求会先在库的任务队列里排队，
    //   于是 503 被"延迟"到有线程空出来才返回 —— 背压就失效了。
    return Err(ErrorKind::kInvalidArgument,
               "max_connections(" + std::to_string(options.max_connections) +
                   ") 不得大于 worker_threads(" + std::to_string(workers) +
                   ")，否则超限请求会先排队而不是立刻 503");
  }
  if (options.transfer_buffer_bytes <= 0) {
    return Err(ErrorKind::kInvalidArgument, "server.http.transfer_buffer_bytes 必须为正");
  }
  // C1.13：并发 × 缓冲 ≤ 传输内存预算
  const auto need = static_cast<std::int64_t>(workers) * options.transfer_buffer_bytes;
  if (options.transfer_memory_budget_bytes > 0 && need > options.transfer_memory_budget_bytes) {
    return Err(ErrorKind::kInvalidArgument,
               "worker_threads(" + std::to_string(workers) + ") × transfer_buffer_bytes(" +
                   std::to_string(options.transfer_buffer_bytes) + ") = " + std::to_string(need) +
                   " 超过 transfer_memory_budget_bytes(" +
                   std::to_string(options.transfer_memory_budget_bytes) + ")");
  }
  if (options.error_format != "apperror" && options.error_format != "legacy" &&
      options.error_format != "api_error") {
    return Err(ErrorKind::kInvalidArgument, "http.error_format 取值非法：" + options.error_format);
  }
  return Ok();
}

// =============================================================================
//  Server::Impl
// =============================================================================
struct Server::Impl {
  ServerOptions options;
  const logging::ILogger* logger;
  const IClock* clock;
  httplib::Server server;
  std::string last_error;
  int bound_port = 0;
  std::thread runner;
  std::atomic<bool> stopping{false};
  std::atomic<int> in_flight{0};
  UuidGenerator ids;
  mutable std::mutex stats_mu;
  Stats stats;

  struct Route {
    Method method = Method::kUnknown;
    std::string pattern;
    RouteOptions options;
    Handler handler;
    enum class Kind { kExact, kParam, kPrefix } kind = Kind::kExact;
    std::vector<std::string> segments;
    std::string prefix;
  };
  std::vector<Route> routes;

  Impl(ServerOptions opts, const logging::ILogger& log, const IClock& clk)
      : options(std::move(opts)), logger(&log), clock(&clk) {}

  // ---- 路由 ----
  static std::vector<std::string> SplitPath(std::string_view path) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < path.size()) {
      if (path[i] == '/') {
        ++i;
        continue;
      }
      const auto next = path.find('/', i);
      const auto end = next == std::string_view::npos ? path.size() : next;
      out.emplace_back(path.substr(i, end - i));
      i = end;
    }
    return out;
  }

  static Route MakeRoute(Method method, std::string pattern, RouteOptions options, Handler handler) {
    Route route;
    route.method = method;
    route.pattern = std::move(pattern);
    route.options = std::move(options);
    route.handler = std::move(handler);
    if (route.pattern.size() >= 2 && route.pattern.compare(route.pattern.size() - 2, 2, "/*") == 0) {
      route.kind = Route::Kind::kPrefix;
      route.prefix = route.pattern.substr(0, route.pattern.size() - 1);  // 保留结尾 '/'
    } else if (route.pattern.find("/:") != std::string::npos) {
      route.kind = Route::Kind::kParam;
      route.segments = SplitPath(route.pattern);
    } else {
      route.kind = Route::Kind::kExact;
      route.segments = SplitPath(route.pattern);
    }
    return route;
  }

  struct Match {
    const Route* route = nullptr;
    std::vector<std::pair<std::string, std::string>> params;
  };

  // ★ 方法不匹配 → 404（不是 405）：ADR-002 §4 依据 0.26.0 实测行为固化，契约 §1.7。
  Match MatchRoute(Method method, std::string_view path) const {
    const auto segments = SplitPath(path);
    for (const auto& route : routes) {
      if (route.method != method) continue;
      if (route.kind == Route::Kind::kPrefix) {
        if (path.size() >= route.prefix.size() &&
            path.compare(0, route.prefix.size(), route.prefix) == 0) {
          Match m;
          m.route = &route;
          m.params.emplace_back("*", std::string(path.substr(route.prefix.size())));
          return m;
        }
        continue;
      }
      if (route.segments.size() != segments.size()) continue;
      Match m;
      m.route = &route;
      bool ok = true;
      for (std::size_t i = 0; i < segments.size(); ++i) {
        const std::string& pat = route.segments[i];
        if (!pat.empty() && pat[0] == ':') {
          m.params.emplace_back(pat.substr(1), segments[i]);
        } else if (pat != segments[i]) {
          ok = false;
          break;
        }
      }
      if (ok) return m;
    }
    return {};
  }

  void CountRequest() {
    std::lock_guard<std::mutex> lock(stats_mu);
    ++stats.requests_total;
    const int now = in_flight.load();
    stats.in_flight = now;
    stats.peak_in_flight = std::max(stats.peak_in_flight, now);
  }
  void Bump(std::uint64_t Stats::*field) {
    std::lock_guard<std::mutex> lock(stats_mu);
    ++(stats.*field);
  }

  void AccessLog(const Request& req, const Response& res, std::int64_t duration_ms,
                 std::string_view note) {
    if (!logger->Enabled(logging::Level::kInfo)) return;
    logging::Fields fields;
    fields.emplace_back("method", req.method_raw);
    fields.emplace_back("path", req.path);
    fields.emplace_back("status", static_cast<std::int64_t>(res.status));
    fields.emplace_back("duration_ms", duration_ms);
    if (!req.correlation_id.empty()) fields.emplace_back("correlation_id", req.correlation_id);
    if (!req.remote_addr.empty()) fields.emplace_back("remote_addr", req.remote_addr);
    if (!req.route_name.empty()) fields.emplace_back("route", req.route_name);
    if (req.content_length >= 0) fields.emplace_back("content_length", req.content_length);
    if (!note.empty()) fields.emplace_back("note", std::string(note));
    logging::Info(*logger, "http_request", fields);
  }

  static HeaderMap CopyHeaders(const httplib::Request& src) {
    HeaderMap out;
    for (const auto& [k, v] : src.headers) out.Add(k, v);
    return out;
  }

  Request MakeRequest(const httplib::Request& src) const {
    Request req;
    req.method = ParseMethod(src.method);
    req.method_raw = src.method;
    req.path = src.path;
    // 原始 query 从 `target` 取（httplib 自己的 `params` 是 multimap，会丢掉顺序与重复键；
    // 而顺序与重复键对我们解析 `expiryTime` 之类的参数有语义）。
    if (const auto q = src.target.find('?'); q != std::string::npos) {
      req.query = src.target.substr(q + 1);
    }
    req.params = net::ParseQuery(req.query).value_or(net::QueryParams{});
    req.headers = CopyHeaders(src);
    req.remote_addr = src.remote_addr;
    req.is_chunked = src.has_header("Transfer-Encoding") &&
                     ToLowerAscii(src.get_header_value("Transfer-Encoding")).find("chunked") !=
                         std::string::npos;
    // ★ 必须先把头值绑到具名变量：`get_header_value()` 返回**临时** `std::string`，
    //   它的生命周期只到该完整表达式结束。若写成
    //       strtoll(src.get_header_value(...).c_str(), &end, 10)
    //   再在下一行解引用 `end`，就是在读一块**已析构**的存储 —— ASan 报
    //   `stack-use-after-scope`（实测抓到，见 P1-D19）。
    if (const std::string cl_header = src.get_header_value("Content-Length"); !cl_header.empty()) {
      char* end = nullptr;
      const auto parsed = std::strtoll(cl_header.c_str(), &end, 10);
      if (end != nullptr && *end == '\0') req.content_length = parsed;
    }
    req.is_head = (req.method == Method::kHead);
    if (const auto cid = req.Header("x-correlation-id"); cid.has_value() && !cid->empty()) {
      req.correlation_id = *cid;
    } else {
      req.correlation_id = ids.NewUuid();
    }
    return req;
  }

  // ★ Range 归一化：**在库计算区间之前**按契约 §1.8 处理（H-1 的防线）
  //  ⚠️ 关键细节：httplib 用的是它在解析阶段填好的 `req.ranges`，**不是**请求头。
  //     所以"忽略该头"必须同时 `req.ranges.clear()`，否则我们以为忽略了、库照旧按区间返回。
  bool NormalizeRange(httplib::Request& hreq, Response& res, std::string* note) {
    const auto range_header = hreq.get_header_value("Range");
    if (range_header.empty()) return true;
    if (res.stream == nullptr || res.stream_length < 0) return true;

    const auto total = res.stream_length;
    const auto parsed = bytes::ParseRangeHeader(range_header, total);
    switch (parsed.status) {
      case bytes::RangeStatus::kAbsent:
        return true;
      case bytes::RangeStatus::kSingle:
      case bytes::RangeStatus::kMultiple: {
        if (parsed.status == bytes::RangeStatus::kMultiple && !res.stream->Seekable()) {
          // 多段需要随机访问（multipart 会按段逐个定位）。不可寻址的来源降级为全量 200，
          // 而不是中途写坏 multipart —— 宁可不满足这个可选特性。
          hreq.ranges.clear();
          hreq.headers.erase("Range");
          if (note != nullptr) *note = "multi_range_requires_seekable_source";
          return true;
        }
        // ★ 用**我们归一化后的绝对区间**覆盖库的解析结果：
        //   · 后缀形式（`-N`）与末尾截断由我们按 RFC 7233 §2.1 处理
        //     （库对"后缀 ≥ 总长"会算出负起点并判 416 —— 实测不符合 RFC）
        //   · 库只负责"按绝对 offset/length 切数据"，判定权留在我们手里
        hreq.ranges.clear();
        for (const auto& r : parsed.ranges) {
          hreq.ranges.emplace_back(static_cast<ssize_t>(r.first), static_cast<ssize_t>(r.last));
        }
        return true;
      }
      case bytes::RangeStatus::kMalformed:
        // 语法非法（含区间数超上限）→ 忽略该头，返回 200 全量（416 只留给"语法合法但不可满足"）
        hreq.ranges.clear();
        hreq.headers.erase("Range");
        if (note != nullptr) *note = "malformed_range_ignored";
        return true;
      case bytes::RangeStatus::kUnsatisfiable: {
        // ★ 我们自己回 416：越界的 Range 根本不会到达库的区间计算路径
        hreq.ranges.clear();
        // ⚠️ 必须在构造新 Response **之前**记下总长：ErrorResponse 会把 stream_length 重置为 -1
        res = ErrorResponse(options, 416, parsed.detail);
        res.headers.Set("Content-Range", bytes::FormatUnsatisfiedContentRange(total));
        if (note != nullptr) *note = "range_unsatisfiable_416";
        return false;
      }
    }
    return true;
  }

  // 把我们的 Response 落到 httplib::Response（含流式 body）
  //   `hreq` 用来判断有没有 Range —— httplib 只在"handler 没有自己定状态码"时
  //   才把 200 改成 206（源码：`if (res.status == -1) status = req.ranges.empty() ? 200 : 206`），
  //   所以对**可区间化的流式响应**我们必须把状态留成 -1，交给库决定 200/206/416。
  void ApplyToHttplib(Response& res, httplib::Response& dst, const httplib::Request& hreq) {
    const auto content_type = res.headers.Get("Content-Type");
    // Content-Type 由 set_content / set_content_provider 负责设置，避免出现两个同名头
    for (const auto& [k, v] : res.headers.entries()) {
      if (EqualsNoCase(k, "Content-Type")) continue;
      dst.set_header(k, v);
    }

    if (res.stream != nullptr) {
      if (res.stream_length < 0) {
        // 未知长度的流式响应需要 chunked 传输；P1 未实现（P3 的数据面再加）。
        // 这里必须明确失败：传 0 长度会让库走"写到 provider 调 done() 为止"的分支，
        // 而我们的 provider 永不调 done → **死循环**。
        res = ErrorResponse(options, 500, "unknown-length stream is not supported yet");
        for (const auto& [k, v] : res.headers.entries()) {
          if (EqualsNoCase(k, "Content-Type")) continue;
          dst.set_header(k, v);
        }
        dst.set_content(res.body, "application/json");
        dst.status = 500;
        return;
      }
      const auto total = res.stream_length;
      auto pump = std::make_shared<StreamPump>(res.stream, total);
      const auto ct = content_type.value_or("application/octet-stream");
      // ⚠️ 回调在 handler 返回**之后**才执行 → 必须**按值**捕获 shared_ptr（AGENTS 陷阱清单）
      dst.set_content_provider(
          static_cast<std::size_t>(total), ct,
          [pump](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
            return (*pump)(offset, length, sink);
          });
      // 交给库决定 200/206（有 Range 且校验通过 → 206；否则 200）
      dst.status = hreq.ranges.empty() ? 200 : -1;
      return;
    }

    dst.status = res.status;
    if (content_type.has_value()) {
      dst.set_content(res.body, *content_type);
    } else if (!res.body.empty()) {
      dst.set_content(res.body, "text/plain");
    }
    // 空 body：交给库补 Content-Length: 0
  }

  // 真正分发的核心
  Response Dispatch(const httplib::Request& hreq, const httplib::ContentReader* reader) {
    const auto t0 = clock->NowEpochMillis();
    Request req = MakeRequest(hreq);
    CountRequest();
    std::string note;

    // ---- 请求走私防护（契约 §1.7）----
    //  ⚠️ 必须由包装层做：走 ContentReader 路径时库不会替我们检查这两项
    //  （实测：重复 `Content-Length` 与 `CL`+`chunked` 并存都会正常进 handler）。
    {
      const auto cl_count = req.headers.Count("Content-Length");
      if (cl_count > 1) {
        Response res = ErrorResponse(options, 400, "duplicate Content-Length headers");
        AccessLog(req, res, clock->NowEpochMillis() - t0, "duplicate_content_length");
        return res;
      }
      if (req.is_chunked && cl_count == 1) {
        Response res =
            ErrorResponse(options, 400, "Content-Length and Transfer-Encoding: chunked together");
        AccessLog(req, res, clock->NowEpochMillis() - t0, "content_length_with_chunked");
        return res;
      }
    }

    const auto match = MatchRoute(req.method, req.path);
    if (match.route == nullptr) {
      Bump(&Stats::not_found);
      // 只回显路径的前 256 字节：否则 8 KB 的 URI 会原样反射进响应体（放大与注入面）
      std::string shown = req.path.size() > 256 ? req.path.substr(0, 256) + "…" : req.path;
      Response res = ErrorResponse(options, 404, "no route for " + req.method_raw + " " + shown);
      AccessLog(req, res, clock->NowEpochMillis() - t0, "route_not_found");
      return res;
    }
    const Route& route = *match.route;
    req.route_name = route.options.name.empty()
                         ? std::string(MethodName(req.method)) + " " + route.pattern
                         : route.options.name;
    req.route_params = match.params;

    // ---- H-2①：按 Content-Length 前置拒绝（一个字节都不读）----
    if (reader != nullptr && req.content_length >= 0 && route.options.max_body_bytes > 0 &&
        req.content_length > route.options.max_body_bytes) {
      Bump(&Stats::rejected_too_large);
      Response res = ErrorResponse(options, 413,
                                   "content-length " + std::to_string(req.content_length) +
                                       " exceeds limit " +
                                       std::to_string(route.options.max_body_bytes));
      AccessLog(req, res, clock->NowEpochMillis() - t0, "content_length_too_large");
      return res;
    }

    // ---- 读请求体 ----
    std::shared_ptr<CountingReceiver> counting;
    std::shared_ptr<std::atomic<bool>> reader_ok;
    std::unique_ptr<BodyPump> pump;
    if (reader != nullptr) {
      counting = std::make_shared<CountingReceiver>();
      counting->limit = route.options.max_body_bytes;
      if (route.options.stream_body) {
        //  ★ 数据面：没有整体超时，只有"无进展"的空闲超时（C4.11）
        counting->overall_timeout_ms = 0;
        counting->idle_timeout_ms =
            static_cast<std::int64_t>(options.transfer_idle_timeout_sec) * 1000;
      } else {
        //  普通路由：整体 + 空闲都管
        counting->overall_timeout_ms =
            static_cast<std::int64_t>(options.json_request_timeout_sec) * 1000;
        counting->idle_timeout_ms = static_cast<std::int64_t>(options.read_timeout_sec) * 1000;
      }
      if (route.options.stream_body) {
        const auto capacity = static_cast<std::size_t>(
            std::max<std::int64_t>(4096, options.transfer_buffer_bytes));
        reader_ok = std::make_shared<std::atomic<bool>>(true);
        const auto reader_ok_capture = reader_ok;
        pump = std::make_unique<BodyPump>(
            [reader, counting, reader_ok_capture](
                std::function<bool(const char*, std::size_t)> forward) {
              const bool ok = (*reader)([counting, forward](const char* d, std::size_t n) {
                return (*counting)(d, n) && forward(d, n);
              });
              reader_ok_capture->store(ok);
              return ok;
            },
            capacity);
        pump->Start();
        req.body_stream = std::shared_ptr<bytes::ByteSource>(pump.get(), [](bytes::ByteSource*) {});
      } else {
        counting->buffer = &req.body;
        const bool ok = (*reader)([counting](const char* d, std::size_t n) {
          return (*counting)(d, n);
        });
        if (!HandleBodyError(req, route, *counting, ok, &note)) {
          Response res = last_body_error;
          AccessLog(req, res, clock->NowEpochMillis() - t0, note);
          return res;
        }
      }
    }

    // ---- 调用 handler ----
    Response res;
    try {
      res = route.handler(req);
    } catch (const std::exception& e) {
      res = ErrorResponse(options, 500, std::string("handler threw: ") + e.what());
      note = "handler_exception";
    } catch (...) {
      res = ErrorResponse(options, 500, "handler threw unknown exception");
      note = "handler_exception";
    }

    // ---- 流式请求体的收尾：必须 join，且**强制**纠正被吞掉的读体错误 ----
    if (pump != nullptr) {
      pump->Stop();
      req.body_stream.reset();
      if (!HandleBodyError(req, route, *counting, reader_ok->load(), &note)) {
        res = last_body_error;
      }
    }

    // ---- Range 归一化（在写响应之前；需要能改 httplib 的请求头）----
    {
      auto& mutable_hreq = const_cast<httplib::Request&>(hreq);
      if (!NormalizeRange(mutable_hreq, res, &note)) {
        // 416 已经构造好
      }
    }

    if (!req.correlation_id.empty()) res.headers.Set("x-correlation-id", req.correlation_id);
    AccessLog(req, res, clock->NowEpochMillis() - t0, note);
    return res;
  }

  // 统一处理"读体失败/超限/长度不符"。返回 false 时 `last_body_error` 是最终响应。
  bool HandleBodyError(const Request& req, const Route& route, const CountingReceiver& counting,
                       bool reader_ok, std::string* note) {
    if (counting.timed_out) {
      //  数据面**没有整体超时**，能走到这里只可能是空闲超时（transfer_idle_timeout_sec）
      Bump(&Stats::request_timeout);
      last_body_error =
          ErrorResponse(options, 408, "request body timed out (no progress within " +
                                          std::to_string(route.options.stream_body
                                                             ? options.transfer_idle_timeout_sec
                                                             : options.json_request_timeout_sec) +
                                          "s)");
      if (note != nullptr) *note = "request_body_timeout_408";
      return false;
    }
    if (counting.exceeded) {
      Bump(&Stats::rejected_too_large);
      const int status = req.content_length >= 0 ? 413 : 400;
      last_body_error = ErrorResponse(options, status,
                                      "request body exceeds limit " +
                                          std::to_string(route.options.max_body_bytes) + " bytes");
      if (note != nullptr) *note = status == 413 ? "body_too_large_413" : "body_too_large_400";
      return false;
    }
    //  ★ 空闲超时必须在"长度不符"**之前**判：一个卡住的 PUT 既满足"读失败"也满足
    //    "声明长度没读满"，但根因是超时。若让长度不符先命中，客户端与运维看到的
    //    会是 `content-length mismatch`（去查客户端），而不是 `timed out`（查超时配置）。
    const auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - counting.last_progress)
                             .count();
    if (!reader_ok && counting.idle_timeout_ms > 0 && idle_ms >= counting.idle_timeout_ms) {
      Bump(&Stats::request_timeout);
      const auto budget_sec = counting.idle_timeout_ms / 1000;
      last_body_error = ErrorResponse(
          options, 408,
          "request body timed out (no progress for " + std::to_string(budget_sec) + "s)");
      if (note != nullptr) *note = "request_body_idle_timeout_408";
      return false;
    }
    // 长度不符要在"读失败"之前判：声明 100 只发 50 时底层会报读失败，
    // 但真正的原因（也是排障最需要的）是长度不符。
    if (req.content_length >= 0 && counting.read != req.content_length) {
      Bump(&Stats::body_length_mismatch);
      last_body_error = ErrorResponse(options, 400,
                                      "content-length mismatch: declared " +
                                          std::to_string(req.content_length) + ", read " +
                                          std::to_string(counting.read));
      if (note != nullptr) *note = "content_length_mismatch";
      return false;
    }
    if (!reader_ok) {
      Bump(&Stats::body_limit_aborted);
      last_body_error = ErrorResponse(options, 400, "failed to read request body");
      if (note != nullptr) *note = "body_read_failed";
      return false;
    }
    return true;
  }

  Response last_body_error;

  void InstallHandlers() {
    // ---- 并发上限（C1.12）：pre 阶段计数并在超限时立刻 503 ----
    server.set_pre_routing_handler([this](const httplib::Request&, httplib::Response& res) {
      const int now = ++in_flight;
      const int limit = options.max_connections > 0 ? options.max_connections
                                                    : (options.worker_threads > 0
                                                           ? options.worker_threads
                                                           : DefaultWorkerThreads(options));
      if (now > limit) {
        // ⚠️ 不要在这里手工归还计数：post_routing 处理器**无条件**会跑（它在
        //    write_response_core 里，pre 返回 Handled 也会走到），再减一次就会把
        //    计数搞成负数 —— 那会让后续请求绕过上限（实测过：in_flight == -1）。
        Bump(&Stats::rejected_busy);
        res.status = 503;
        res.set_header("Retry-After", "1");
        res.set_header("Content-Type", "application/json");
        res.set_content(ErrorBody(options, 503, "too many in-flight requests (limit " +
                                                    std::to_string(limit) + ")"),
                        "application/json");
        return httplib::Server::HandlerResponse::Handled;
      }
      return httplib::Server::HandlerResponse::Unhandled;
    });
    // ★ post_routing 在 write_response_core 里无条件调用（包括 pre 返回 Handled 的情况），
    //   所以在途计数一定会被归还 —— 否则几次 503 之后服务器会永久 503。
    server.set_post_routing_handler([this](const httplib::Request&, httplib::Response&) {
      --in_flight;
      std::lock_guard<std::mutex> lock(stats_mu);
      stats.in_flight = in_flight.load();
    });

    // ---- 库自身产生的错误（解析失败等）归一化为契约形态 ----
    server.set_error_handler([this](const httplib::Request& hreq, httplib::Response& res) {
      if (res.status < 400) return httplib::Server::HandlerResponse::Unhandled;

      // ★ 实测的库行为：**解析阶段**遇到非法 `Range` 会直接回 416 并跳过路由
      //   （httplib.h:8337 `if (!parse_range_header(...)) { res.status = 416; return write_response(...); }`）。
      //   但 RFC 7233 §4.4 与契约 §1.8 都要求"语法非法 → 忽略该头 → 200 全量"，
      //   否则 `bytes = 0-1`（带空格）、`bytes=10-5` 这类写法会让客户端拿到 416 并可能陷入重试。
      //   这里在错误处理器里**重新分发一次**（只限 GET/HEAD —— Range 只对下载有意义），
      //   并把这件"纠正"记进日志，便于排障时看出我们改写过库的行为。
      if (res.status == 416 && (hreq.method == "GET" || hreq.method == "HEAD") &&
          hreq.has_header("Range")) {
        const auto parsed = bytes::ParseRangeHeader(hreq.get_header_value("Range"), -1);
        if (parsed.status == bytes::RangeStatus::kMalformed) {
          auto sanitized = hreq;          // 用副本：不改动库持有的请求对象
          sanitized.ranges.clear();       // 库用它做区间计算 → 必须清掉
          sanitized.headers.erase("Range");
          Response out = Dispatch(sanitized, nullptr);
          res.headers.clear();
          ApplyToHttplib(out, res, sanitized);
          if (logger->Enabled(logging::Level::kWarn)) {
            logging::Fields fields;
            fields.emplace_back("path", sanitized.path);
            fields.emplace_back("range", hreq.get_header_value("Range"));
            fields.emplace_back("note", "malformed_range_ignored_by_wrapper");
            logging::Warn(*logger, "range_header_ignored", fields);
          }
          return httplib::Server::HandlerResponse::Handled;
        }
      }

      const auto ct = res.get_header_value("Content-Type");
      if (ct.find("application/json") != std::string::npos) {
        return httplib::Server::HandlerResponse::Unhandled;  // 已经是我们生成的
      }
      std::string detail = res.body;
      if (detail.size() > 200) detail.resize(200);
      res.set_content(ErrorBody(options, res.status, detail), "application/json");
      return httplib::Server::HandlerResponse::Handled;
    });

    // ---- 一个 catch-all 正则路由 + 自己的路由表 ----
    //  为什么不用库的路由：我们需要"方法不匹配 → 404""路由级 body 上限""自己的 404 错误体"
    //  这些语义；把它们放在自己的表里更可控、也更好测。
    auto plain = [this](const httplib::Request& req, httplib::Response& res) {
      Response out = Dispatch(req, nullptr);
      ApplyToHttplib(out, res, req);
    };
    auto with_reader = [this](const httplib::Request& req, httplib::Response& res,
                              const httplib::ContentReader& reader) {
      Response out = Dispatch(req, &reader);
      ApplyToHttplib(out, res, req);
    };
    server.Get("/.*", plain);   // HEAD 也由 GET 处理器承担（httplib 的既定行为）
    server.Options("/.*", plain);
    server.Post("/.*", with_reader);
    server.Put("/.*", with_reader);
    server.Patch("/.*", with_reader);
    server.Delete("/.*", with_reader);

    // ---- 传输层参数 ----
    server.set_tcp_nodelay(options.tcp_nodelay);  // ★ 默认 true（httplib 默认 false）
    server.set_keep_alive_max_count(static_cast<size_t>(options.keep_alive_max_count));
    server.set_keep_alive_timeout(options.keep_alive_timeout_sec);
    //  ★ socket 级只能设一个读超时：取"普通路由空闲"与"数据面空闲"的较大者，
    //    较小的那个由 `CountingReceiver` 按块检查（C4.11）
    server.set_read_timeout(
        std::max(options.read_timeout_sec, options.transfer_idle_timeout_sec), 0);
    server.set_write_timeout(options.write_timeout_sec, 0);
    server.set_payload_max_length(static_cast<size_t>(options.payload_ceiling_bytes));
    const int workers =
        options.worker_threads > 0 ? options.worker_threads : DefaultWorkerThreads(options);
    server.new_task_queue = [workers] { return new httplib::ThreadPool(workers); };
    {
      std::lock_guard<std::mutex> lock(stats_mu);
      stats.worker_threads = workers;
      stats.max_connections =
          options.max_connections > 0 ? options.max_connections : workers;
    }
  }
};

// =============================================================================
//  Server 公开接口
// =============================================================================
Server::Server(ServerOptions options, const logging::ILogger& logger, const IClock& clock)
    : impl_(std::make_unique<Impl>(std::move(options), logger, clock)) {
  const auto valid = ValidateOptions(impl_->options);
  if (!valid.ok()) {
    impl_->last_error = valid.error().message();
    return;  // Bind() 会因 last_error 非空而失败
  }
  impl_->InstallHandlers();
}

Server::~Server() { Stop(); }

void Server::AddRoute(Method method, std::string pattern, RouteOptions options, Handler handler) {
  impl_->routes.push_back(Impl::MakeRoute(method, std::move(pattern), std::move(options),
                                          std::move(handler)));
  const auto& added = impl_->routes.back();
  if (added.options.allow_head && method == Method::kGet) {
    // HEAD 与 GET 同路由：响应体由库丢弃，但 Content-Length 保留（契约要求 HEAD 可探测长度）
    impl_->routes.push_back(
        Impl::MakeRoute(Method::kHead, added.pattern, added.options, added.handler));
  }
}

void Server::Get(std::string pattern, RouteOptions options, Handler handler) {
  AddRoute(Method::kGet, std::move(pattern), std::move(options), std::move(handler));
}
void Server::Post(std::string pattern, RouteOptions options, Handler handler) {
  AddRoute(Method::kPost, std::move(pattern), std::move(options), std::move(handler));
}
void Server::Put(std::string pattern, RouteOptions options, Handler handler) {
  AddRoute(Method::kPut, std::move(pattern), std::move(options), std::move(handler));
}
void Server::Delete(std::string pattern, RouteOptions options, Handler handler) {
  AddRoute(Method::kDelete, std::move(pattern), std::move(options), std::move(handler));
}
void Server::Head(std::string pattern, RouteOptions options, Handler handler) {
  AddRoute(Method::kHead, std::move(pattern), std::move(options), std::move(handler));
}
void Server::Options(std::string pattern, RouteOptions options, Handler handler) {
  AddRoute(Method::kOptions, std::move(pattern), std::move(options), std::move(handler));
}
void Server::Patch(std::string pattern, RouteOptions options, Handler handler) {
  AddRoute(Method::kPatch, std::move(pattern), std::move(options), std::move(handler));
}

bool Server::Bind() {
  if (!impl_->last_error.empty()) return false;
  if (impl_->options.port == 0) {
    // 端口 0 → 交给内核分配，并把真实端口记下来（测试全靠它）
    const int assigned = impl_->server.bind_to_any_port(impl_->options.bind_address);
    if (assigned <= 0) {
      impl_->last_error = "bind 失败：无法在 " + impl_->options.bind_address + " 上分配端口";
      return false;
    }
    impl_->bound_port = assigned;
    return true;
  }
  impl_->bound_port = impl_->options.port;
  if (!impl_->server.bind_to_port(impl_->options.bind_address, impl_->options.port)) {
    impl_->last_error = "bind 失败：" + impl_->options.bind_address + ":" +
                        std::to_string(impl_->options.port) + "（端口被占用？权限不足？）";
    return false;
  }
  return true;
}

bool Server::Listen() {
  if (!impl_->last_error.empty()) return false;
  return impl_->server.listen_after_bind();
}

bool Server::Start() {
  if (!Bind()) return false;
  impl_->runner = std::thread([this] { impl_->server.listen_after_bind(); });
  // 等端口真正可连（轮询而不是 sleep：AGENTS §4.3）
  for (int i = 0; i < 500; ++i) {
    if (impl_->server.is_running()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  impl_->last_error = "服务端未能在 1 秒内进入运行状态";
  return false;
}

void Server::Stop() {
  if (impl_ == nullptr) return;
  if (impl_->stopping.exchange(true)) return;
  impl_->server.stop();
  if (impl_->runner.joinable()) impl_->runner.join();
}

int Server::port() const { return impl_->bound_port; }

const std::string& Server::last_error() const { return impl_->last_error; }

Stats Server::stats() const {
  std::lock_guard<std::mutex> lock(impl_->stats_mu);
  Stats out = impl_->stats;
  out.in_flight = impl_->in_flight.load();
  return out;
}

void Server::ResetStats() {
  std::lock_guard<std::mutex> lock(impl_->stats_mu);
  const int workers = impl_->stats.worker_threads;
  const int max_conn = impl_->stats.max_connections;
  impl_->stats = Stats{};
  impl_->stats.worker_threads = workers;
  impl_->stats.max_connections = max_conn;
}

}  // namespace fss::http
