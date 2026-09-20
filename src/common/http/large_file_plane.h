// =============================================================================
//  LargeFilePlane（L1，`fss_http`）—— ADR-006 方案 ③ 的**第二个监听 socket**
// =============================================================================
//  它是什么
//    一个**进程内**的、只服务"大文件下载"的 HTTP/1.1 监听面：解析请求 →
//    调用**控制面同一个已包装 handler**（`Router::BuildTransferGetHandler()`）→
//    把响应按 `sendfile(2)` 送出。它**不是**一个独立的 HTTP 语义实现：
//      · 鉴权 / 租户 / `ErrorToResponse` / HTTP 指标 —— 全部由那个 handler 完成；
//      · 访问日志字段 —— 调 `fss::http::LogAccess`（与 httplib 包装层**同一份**）；
//      · 解析阶段的错误体 —— 调 `fss::http::ErrorBody`（同一份）。
//    本文件只负责"字节怎么从 fd 到 socket"以及"请求行/头怎么解析"这两件
//    httplib 替我们做、而这里必须自己做**且必须同源**的事。
//
//  为什么不做通用 HTTP 服务器（范围：**只下载**）
//    `sendfile(2)` 只能服务"读一个已有对象"的方向；上传（`PUT`）与 `splice`/`receive-file`
//    不在范围内（ADR-006 的实现门槛未要求它们）。因此：
//      · `GET` / `HEAD` 在 `/<base>/v1/transfer/{token}` 上 → 数据面；
//      · 其它方法落在该路径上 → **405**（控制面因为注册了 `PUT` 而返回 404/200，
//        这是"只下载"这一范围决策的**已知差异**，由文档与用例显式登记）；
//      · 未知路径 → 404（与包装层同样的错误体）。
//
//  零拷贝是**可选能力**，不是特例
//    走不走 `sendfile` 只取决于 `Response::stream->NativeFd()`：
//      · `>=0` 且 `use_sendfile=true` → `sendfile`；
//      · 否则 → 用户态 64 KiB 循环（`Read` + `send`），**帧格式完全相同**。
//    因此内存/S3 等"没有原生 fd"的来源在数据面上**功能不缺**，只是少了一次拷贝。
//    ★ `NativeFd()` 由 L1 `bytes::ByteSource` 提供（默认 `-1` = 必须回退用户态）。
//
//  ⚠️ 计数点移动（ADR-006 的诚实登记）
//    `sendfile` **绕过** `ByteSource::Read`（以及 `IBlobStore::get()`），于是
//    `MeteredBlobStore` 对用户态拷贝的字节计数不会再发生。凡是**真的走了 sendfile**
//    的下载，**本类**必须把这些计数补上（组合根通过 `on_native_bytes` 把它接到计量
//    装饰器的**同一份** label 上，见 `MeteredBlobStore::RecordNativeRead`）。
//    用户态 pump 的字节仍由 `MeteredNativeSource` 在 `Read` 上记 —— 两条路互斥。
//    判据见 `tests/integration/test_large_file_plane.cpp` 的 P5 等价性用例。
//
//  ★ 生命周期纪律（AGENTS §4.3）：`Stop()` 在**每一条**退出路径上被调用（析构兜底），
//    先 `shutdown` 监听 fd 打断 `accept`，再 join 全部线程 —— 绝不留下 joinable thread。
// =============================================================================
#pragma once

#include "common/http/http.h"
#include "common/metrics/metrics.h"
#include "common/result/result.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fss {

class IClock;
namespace logging {
class ILogger;
}

namespace http {

struct LargeFilePlaneOptions {
  std::string bind_address = "0.0.0.0";
  int port = 0;

  //  走零拷贝的开关；`false` → 一律用户态 pump（帧格式不变）。
  bool use_sendfile = true;
  //  单次 `sendfile(2)` 的**长度上限**（> 2 GiB 的对象必须靠循环，不能指望一次调用）。
  std::int64_t sendfile_chunk_bytes = 1LL << 30;

  int worker_threads = 0;    // 0 → `DefaultLargeFilePlaneWorkers()`（max(64, 2×核数)）
  int max_connections = 0;   // 0 → 等于 worker_threads

  //  与控制面同源的路径/错误体/超时/限额
  std::string base_path = "/api/file";
  std::string error_format = "apperror";
  int idle_timeout_sec = 60;
  int keep_alive_timeout_sec = 5;
  int keep_alive_max_count = 100;       // 控制面默认 100（httplib 默认值）
  std::int64_t max_uri_bytes = 8192;    // 与 httplib 的 CPPHTTPLIB_REQUEST_URI_MAX_LENGTH 一致
  std::int64_t max_header_bytes = 16384;
  std::int64_t userspace_buffer_bytes = 64 * 1024;

  //  `/metrics` 注册表；null → 不注册数据面的诊断计数器（测试内嵌用法）
  fss::metrics::Registry* metrics = nullptr;
  //  ★ 计数点移动的落点：**只在真的走了 `sendfile` 的那条路上**调用一次
  //  （`bytes` = 实际送出的字节数，`ok` = 是否完整送达）。
  //  组合根把它接到 `MeteredBlobStore::RecordNativeRead`，从而与 `get()` 路径
  //  使用**完全相同**的指标族与标签。用户态 pump 的字节由 `MeteredNativeSource`
  //  在 `Read` 上记 —— 两者互斥，不会重复计数。
  std::function<void(std::int64_t bytes, bool ok)> on_native_bytes;
};

//  与控制面 `server.http.worker_threads=0` 同源的推导纪律：默认并发上限足够大，
//  且不随核数线性膨胀（下载是 IO 密集，线程主要用于阻塞式 sendfile 调用）。
int DefaultLargeFilePlaneWorkers();

struct LargeFilePlaneStats {
  std::uint64_t connections_total = 0;
  std::uint64_t rejected_busy = 0;
  std::uint64_t sendfile_calls = 0;
  std::uint64_t sendfile_bytes = 0;
  std::uint64_t userspace_calls = 0;
  std::uint64_t userspace_bytes = 0;
  int in_flight = 0;
  int worker_threads = 0;
  int max_connections = 0;
};

class LargeFilePlane {
 public:
  LargeFilePlane(LargeFilePlaneOptions options, Handler handler, const logging::ILogger& logger,
                 const IClock& clock);
  ~LargeFilePlane();
  LargeFilePlane(const LargeFilePlane&) = delete;
  LargeFilePlane& operator=(const LargeFilePlane&) = delete;

  //  绑定端口并启动 accept/worker 线程。返回 false 时 `last_error()` 可读
  //  （组合根据此 exit 78，**不得**静默继续）。
  bool Start();
  //  幂等：打断 accept、关闭监听 fd、join 全部线程。
  void Stop();

  int port() const;
  const std::string& last_error() const;
  LargeFilePlaneStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace http
}  // namespace fss
