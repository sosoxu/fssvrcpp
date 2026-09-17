// =============================================================================
//  fss_http（L1）：cpp-httplib 之上的强化包装层
// =============================================================================
//
//  这一层存在的理由（ADR-002 §5.2）
//    · 库的缺陷就在我们的关键路径上：H-1（越界 Range → 下溢的 Content-Length）、
//      H-2（流式请求体超限 → 201 且 0 字节，"上传成功但文件为空"）。
//    · 库的默认行为不足以直接当契约：默认 400、`set_payload_max_length` 的语义、
//      方法不匹配返回 404 等，都必须由**我们**归一化并写进契约（契约 §1.7/§1.8）。
//    · 可测试性：所有上限/Range/并发边界都成为**我们自己的**断言，而不是对第三方行为的信任。
//
//  ★ 铁律：只有 `src/common/http/` 允许 `#include <httplib.h>`（分层护栏强制）。
//    本头文件**不**暴露任何 httplib 类型（用 pimpl），因此换库不触碰上层。
//
//  H-2 的三重防护（都在本层实现，见 `server.cpp`）
//    ① 按 `Content-Length` **前置拒绝**：超路由上限 → 413，且**一个字节都不读**；
//    ② 计数 reader：chunked 超限时**立即中止**（不是读完再判断）；
//    ③ 一致性断言：实际读取字节数 ≠ 声明的 `Content-Length` → 400（防静默截断）。
//    另外：即使 handler 吞掉了读体错误并返回 201，本层也会**强制**改回 400 ——
//    因为"静默数据丢失"比"接口报错"严重得多。
#pragma once

#include "common/bytes/bytes.h"
#include "common/net/net.h"
#include "common/result/result.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fss {

class IClock;
namespace logging {
class ILogger;
}

namespace http {

// =============================================================================
//  头集合
// =============================================================================
//  HTTP 头名大小写不敏感，但**保序**保留原样（便于排障与日志）
class HeaderMap {
 public:
  void Set(std::string name, std::string value);  // 覆盖同名
  void Add(std::string name, std::string value);  // 追加（允许重复）
  bool Remove(std::string_view name);
  std::optional<std::string> Get(std::string_view name) const;
  bool Has(std::string_view name) const;
  // 某个头出现了几次（用于检出重复 Content-Length 这类请求走私形态）
  std::size_t Count(std::string_view name) const;
  const std::vector<std::pair<std::string, std::string>>& entries() const { return entries_; }
  void Clear() { entries_.clear(); }

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
};

// =============================================================================
//  方法 / 请求 / 响应
// =============================================================================
enum class Method { kGet, kPost, kPut, kDelete, kHead, kOptions, kPatch, kUnknown };

Method ParseMethod(std::string_view raw);
std::string_view MethodName(Method method);

struct Request {
  Method method = Method::kUnknown;
  std::string method_raw;  // 原样（未识别的方法也要能记日志/返回 404）
  std::string path;        // 原样路径（不做百分号解码 —— 保持与线上字节一致）
  std::string query;       // 原始 query（不含 `?`）
  net::QueryParams params; // 解析后的 query 参数
  HeaderMap headers;
  std::string remote_addr;
  std::string correlation_id;
  std::string route_name;  // 命中的路由名（日志/指标）

  // 请求体：缓冲模式填 `body`；流式模式填 `body_stream`（二者不会同时存在）
  std::string body;
  std::shared_ptr<bytes::ByteSource> body_stream;
  bool is_chunked = false;
  std::int64_t content_length = -1;  // -1 = 未知（chunked / 无体）
  bool is_head = false;

  std::optional<std::string> Header(std::string_view name) const { return headers.Get(name); }
  std::optional<std::string> Param(std::string_view name) const;
  std::optional<std::string> Query(std::string_view name) const {
    return net::GetQueryParam(params, name);
  }

  // 路由参数（`:id` 形式）由本层填充
  std::vector<std::pair<std::string, std::string>> route_params;
};

struct Response {
  int status = 200;
  HeaderMap headers;
  std::string body;

  // 流式响应体（大文件下载）：与 `body` 二选一
  std::shared_ptr<bytes::ByteSource> stream;
  std::int64_t stream_length = -1;  // 已知长度（用于 Range/Content-Length）；-1 = 未知

  static Response Text(int status, std::string body);
  static Response Json(int status, std::string body);
  static Response Empty(int status);
  static Response Stream(int status, std::string content_type,
                         std::shared_ptr<bytes::ByteSource> source, std::int64_t length);
};

using Handler = std::function<Response(Request&)>;

struct RouteOptions {
  // 请求体上限；0 = 不限（数据面）。超限：有 Content-Length → 413；chunked → 400。
  std::int64_t max_body_bytes = 10 * 1024 * 1024;
  // true → 以 `Request::body_stream` 提供**流式**请求体（不整块驻留内存）
  bool stream_body = false;
  std::string name;         // 日志/指标用；为空时用 "METHOD pattern"
  bool allow_head = true;   // 为该路由自动接受 HEAD（响应体被丢弃，但 Content-Length 保留）
};

// =============================================================================
//  服务端选项
// =============================================================================
struct ServerOptions {
  std::string bind_address = "0.0.0.0";
  int port = 0;  // 0 = 由系统分配（测试用；实际端口见 Server::port()）

  // 线程池大小。0 → 按公式推导（见 DefaultWorkerThreads）
  int worker_threads = 0;
  // 在途请求上限。超过 → **503 + Retry-After**（不排队到超时）。0 → 等于 worker_threads
  int max_connections = 0;

  // ★ 默认开启：httplib 默认 false，实测差 950 倍（docs/05 §1.2）
  bool tcp_nodelay = true;

  int read_timeout_sec = 60;  // 与 config/fss.example.json 的 idle_timeout_seconds 对齐
  int write_timeout_sec = 30;
  int keep_alive_timeout_sec = 5;
  int keep_alive_max_count = 100;

  //  ★ 超时按路由分类（契约 §1.7 / C4.11），三者的语义**不同**：
  //    · `read_timeout_sec`          —— 普通路由的**空闲**读超时（有进展就一直续期）
  //    · `json_request_timeout_sec`  —— 普通路由的**整体**超时（不适用于数据面）
  //    · `transfer_idle_timeout_sec` —— 数据面的**空闲**超时；数据面**没有整体超时**
  //      （TB 级上传/下载不能被整体超时打断，但仍要在"完全无进展"时释放线程）
  //  socket 级 SO_RCVTIMEO 只能取一个值，因此取两者较大者，另一个在
  //  `CountingReceiver` 里按块检查（见 server.cpp）。
  int json_request_timeout_sec = 15;
  int transfer_idle_timeout_sec = 120;

  std::int64_t transfer_buffer_bytes = 256 * 1024;
  std::int64_t transfer_memory_budget_bytes = 256 * 1024 * 1024;

  // 传输层硬上限（与契约 §1.7 对应）
  std::int64_t max_header_bytes = 8192;
  std::int64_t max_uri_bytes = 8192;
  // 库层面的绝对上限（安全网）；按路由的上限由本层负责
  std::int64_t payload_ceiling_bytes = 64 * 1024 * 1024 * 1024LL;

  std::string service_name = "file-service";
  std::string error_format = "apperror";  // apperror | legacy | api_error
};

// 推导默认线程池：max(16, 4 × 硬件并发)，再受传输内存预算约束
int DefaultWorkerThreads(const ServerOptions& options);

// 选项自洽性校验（C1.13 的第二道防线；配置层已校验一次）
Result<void> ValidateOptions(const ServerOptions& options);

struct Stats {
  std::uint64_t requests_total = 0;
  std::uint64_t rejected_busy = 0;       // 503
  std::uint64_t rejected_too_large = 0;  // 413
  std::uint64_t body_limit_aborted = 0;  // 计数 reader 中止（chunked 超限）
  std::uint64_t body_length_mismatch = 0;// 一致性断言触发
  std::uint64_t not_found = 0;
  std::uint64_t request_timeout = 0;     // 请求体超时（408；C4.11）
  int in_flight = 0;
  int peak_in_flight = 0;
  int worker_threads = 0;
  int max_connections = 0;
};

// =============================================================================
//  Server
// =============================================================================
class Server {
 public:
  Server(ServerOptions options, const logging::ILogger& logger, const IClock& clock);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // 注册路由。pattern 支持三种形态（与 httplib 的常用形态对齐）：
  //   `/api/file/v2/files/health`          精确
  //   `/api/file/v2/files/:id/metadata`    命名段（`Request::Param("id")`）
  //   `/static/*`                          前缀通配（`Request::Param("*")`）
  void AddRoute(Method method, std::string pattern, RouteOptions options, Handler handler);
  void Get(std::string pattern, RouteOptions options, Handler handler);
  void Post(std::string pattern, RouteOptions options, Handler handler);
  void Put(std::string pattern, RouteOptions options, Handler handler);
  void Delete(std::string pattern, RouteOptions options, Handler handler);
  void Head(std::string pattern, RouteOptions options, Handler handler);
  void Options(std::string pattern, RouteOptions options, Handler handler);
  void Patch(std::string pattern, RouteOptions options, Handler handler);

  // 绑定并开始监听（不阻塞）。失败原因见 last_error()
  bool Bind();
  // 阻塞式运行（Bind 之后调用）
  bool Listen();
  // 后台线程运行（测试用；Stop 会 join）
  bool Start();
  void Stop();

  // 实际监听端口（port==0 时由系统分配）
  int port() const;
  const std::string& last_error() const;

  Stats stats() const;
  // 清空计数（测试用；不改变正在服务的请求）
  void ResetStats();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace http
}  // namespace fss
