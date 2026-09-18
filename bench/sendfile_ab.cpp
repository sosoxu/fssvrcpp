// =============================================================================
//  bench/sendfile_ab.cpp —— ADR-006 受控复核的 A/B **服务端**（C9.12）
// =============================================================================
//  一个二进制、两种模式，服务**同一个文件**、同一组端点（`GET /blob`）：
//
//    --mode httplib   产品路径：`fss::http::Server` + `Response::Stream(...)`
//                     （cpp-httplib 的 `set_content_provider` → 64 KiB 一块的
//                      `pread` + `sink.write()`）
//    --mode sendfile  裸 socket + `sendfile(2)`：极简请求头解析 + 固定响应头，
//                     文件字节**不进用户态**（零拷贝）
//
//  ★ 铁律 R2：本进程是**服务端**，绑核由 `scripts/bench_sendfile_ab.sh` 用
//    `taskset` 完成（代码里**不**绑核）。客户端是另一个进程
//    （`fss_bench_capacity load`），两者绑到**互不重叠**的核集合。
//
//  ★ 铁律 R3（协议与安全性，必须随数字一起引用）：
//    · 两侧都是 **HTTP/1.1**、显式 `Content-Length`、**无 chunked**、
//      `Connection: keep-alive`，客户端与服务端都开 `TCP_NODELAY`；
//    · 本原型**没有** Range / 鉴权 / 限流 / 日志 / 超时 / 连接上限 ——
//      它测的是 **sendfile 路径的上界（upper bound）**，
//      **不是**一个可上线的数据面（也不打算变成）。
//
//  ★ 两种模式的并发上限**相同**：`--threads`（默认 4）个连接处理线程，
//    httplib 侧同时是 `worker_threads=4` / `max_connections=4`。
//    ⚠️ 因此当客户端连接数 > 4 时，超出的 keep-alive 连接会等待（在
//    httplib 侧是任务队列，在 sendfile 侧是内核 accept 队列）—— 两侧对称，
//    但该点位的有效并发只有 4（详见 docs/test-evidence/phase9-adr006.md 的诚实声明）。
//
//  用法
//  ---------------------------------------------------------------------------
//    fss_bench_sendfile_ab --mode httplib|sendfile [--file PATH] [--size N]
//                          [--dir DIR] [--threads N]
//      → stdout **第一行**是 `PORT <实际端口>`（端口 0 = 系统分配）；
//        其余诊断信息走 stderr，便于调用方抓取端口而不被污染。
//      → 未给 `--file` 时，若 `--size` 字节的文件不存在则在 `--dir` 下
//        生成 `<dir>/ab-blob-<size>.bin`（**非稀疏**：真实写入 + fsync）。
//      → 收到 SIGTERM/SIGINT 干净退出（关监听、join 工作线程）。
// =============================================================================
#include "common/http/http.h"
#include "common/logging/logging.h"
#include "common/time/clock.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------- 信号
//  sig_atomic_t 是信号处理器里唯一安全的可写状态；listen fd 只做 shutdown（异步信号安全）。
volatile sig_atomic_t g_stop = 0;
volatile sig_atomic_t g_waiting = 0;
int g_listen_fd = -1;

void OnSignal(int /*sig*/) {
  g_stop = 1;
  if (g_waiting != 0) {
    // 只 shutdown 不 close：close 一个"别的线程正阻塞在 accept 上"的 fd 在 Linux 上
    // 不保证唤醒 accept（fd 仍被阻塞调用引用），而 shutdown(SHUT_RDWR) 会让它返回错误。
    if (g_listen_fd >= 0) ::shutdown(g_listen_fd, SHUT_RDWR);
  }
}

// ---------------------------------------------------------------- 结构体日志
//  fss_http 的访问日志在数据面热路径上：这里用"永远 Disabled"的 logger，
//  让 A/B 两侧都不把日志开销算进数字（日志属于 fss_http 的固定成本，不是本判据）。
class SilentLogger final : public fss::logging::ILogger {
 public:
  bool Enabled(fss::logging::Level /*level*/) const override { return false; }
  void Log(fss::logging::Level /*level*/, std::string_view /*msg*/,
           const fss::logging::Fields& /*fields*/) const noexcept override {}
};

// ---------------------------------------------------------------- 参数
struct Options {
  std::string mode;   // httplib | sendfile
  std::string file;   // 显式文件（优先）
  std::string dir = ".";  // 自动生成文件落在哪里
  std::int64_t size = 0;  // --size（字节）
  int threads = 4;
};

[[noreturn]] void Die(const std::string& msg) {
  std::cerr << "错误：" << msg << "\n";
  std::exit(2);
}

Options ParseArgs(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) Die("参数 " + a + " 缺少取值");
      return argv[++i];
    };
    if (a == "--mode") {
      opt.mode = next();
    } else if (a == "--file") {
      opt.file = next();
    } else if (a == "--dir") {
      opt.dir = next();
    } else if (a == "--size") {
      opt.size = std::strtoll(next().c_str(), nullptr, 10);
    } else if (a == "--threads") {
      opt.threads = std::atoi(next().c_str());
    } else if (a == "-h" || a == "--help") {
      std::cout << "PORT <n>\n用法：fss_bench_sendfile_ab --mode httplib|sendfile"
                   " [--file PATH] [--size N] [--dir DIR] [--threads N]\n";
      std::exit(0);
    } else {
      Die("未知参数：" + a);
    }
  }
  if (opt.mode != "httplib" && opt.mode != "sendfile") Die("--mode 必须是 httplib 或 sendfile");
  if (opt.threads <= 0) opt.threads = 1;
  if (opt.file.empty() && opt.size <= 0) Die("必须给 --file，或给 --size 让本程序生成文件");
  return opt;
}

std::string JoinPath(const std::string& dir, const std::string& name) {
  if (!dir.empty() && dir.back() == '/') return dir + name;
  return dir + "/" + name;
}

// 确保存在一份**非稀疏**的 `size` 字节文件（AGENTS §4.2：稀疏文件只验证语义，
// 不能代表存储；这里的性能数字必须来自真实写入的数据）。
std::string EnsureFile(const Options& opt, std::int64_t* actual_size) {
  if (!opt.file.empty()) {
    struct stat st {};
    if (::stat(opt.file.c_str(), &st) != 0) Die("--file 不存在：" + opt.file);
    if (!S_ISREG(st.st_mode)) Die("--file 不是普通文件：" + opt.file);
    if (opt.size > 0 && st.st_size != opt.size) {
      std::cerr << "警告：--file 的实际大小 " << st.st_size << " 与 --size " << opt.size
                << " 不一致，以实际大小为准\n";
    }
    *actual_size = static_cast<std::int64_t>(st.st_size);
    std::cerr << "FILE " << opt.file << " (复用，实际 " << *actual_size << " 字节)\n";
    return opt.file;
  }

  const std::string path = JoinPath(opt.dir, "ab-blob-" + std::to_string(opt.size) + ".bin");
  struct stat st {};
  if (::stat(path.c_str(), &st) == 0 && st.st_size == opt.size) {
    *actual_size = opt.size;
    std::cerr << "FILE " << path << " (已存在同尺寸文件，复用)\n";
    return path;
  }
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) Die("无法创建 " + path + "：" + std::strerror(errno));
  constexpr std::size_t kChunk = 1u << 20;
  std::vector<char> buf(kChunk);
  std::uint64_t x = 0x9e3779b97f4a7c15ULL;  // 确定性伪随机（不可压缩、非全零、非稀疏）
  for (std::size_t i = 0; i < kChunk; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    buf[i] = static_cast<char>((x >> 24) & 0xFF);
  }
  std::int64_t written = 0;
  while (written < opt.size) {
    const std::size_t want =
        static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(kChunk),
                                                        opt.size - written));
    ssize_t n = ::write(fd, buf.data(), want);
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      Die("写 " + path + " 失败：" + std::strerror(errno));
    }
    written += n;
  }
  if (::fsync(fd) != 0) std::cerr << "警告：fsync 失败（" << std::strerror(errno) << "）\n";
  ::close(fd);
  *actual_size = opt.size;
  std::cerr << "FILE " << path << " (新生成 " << opt.size << " 字节，非稀疏)\n";
  return path;
}

// ---------------------------------------------------------------- 文件字节源
//  httplib 侧用：**共享一个只读 fd**，每个响应各自持有偏移（`pread` 不改变文件偏移，
//  天然线程安全）。Seekable=true（Range 需要；本实验不发 Range，但保持产品语义）。
class FileSource final : public fss::bytes::ByteSource {
 public:
  FileSource(int fd, std::int64_t size) : fd_(fd), size_(size) {}

  fss::Result<std::size_t> Read(char* out, std::size_t capacity) override {
    if (pos_ >= size_) return std::size_t{0};
    const auto want = static_cast<std::size_t>(
        std::min<std::int64_t>(static_cast<std::int64_t>(capacity), size_ - pos_));
    const ssize_t n = ::pread(fd_, out, want, static_cast<off_t>(pos_));
    if (n < 0) return fss::Err(fss::ErrorKind::kInternal, "pread 失败");
    if (n == 0) return std::size_t{0};
    pos_ += n;
    return static_cast<std::size_t>(n);
  }
  std::optional<std::int64_t> Size() const override { return size_; }
  bool Seekable() const override { return true; }
  fss::Result<void> Seek(std::int64_t offset) override {
    pos_ = offset;
    return fss::Ok();
  }

 private:
  int fd_;
  std::int64_t size_;
  std::int64_t pos_ = 0;
};

// ---------------------------------------------------------------- 低层收发
bool SendAll(int fd, std::string_view data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

void SetNodelay(int fd) {
  const int on = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));  // AGENTS 陷阱清单
}

// ---------------------------------------------------------------- sendfile 工作线程
//  只支持 `GET /blob`（原型边界，见文件头）。同一连接上支持多个请求（keep-alive）：
//  读到下一个完整请求头就重发一次。**不做** Range/HEAD/压缩/流水线。
void ServeConn(int fd, int file_fd, std::int64_t size) {
  std::string buf;
  std::vector<char> chunk(64 * 1024);
  for (;;) {
    std::size_t pos = buf.find("\r\n\r\n");
    while (pos == std::string::npos) {
      const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
      if (n <= 0) return;  // 客户端关闭/出错 → 结束这条连接
      buf.append(chunk.data(), static_cast<std::size_t>(n));
      if (buf.size() > 64 * 1024) return;  // 请求头异常大 → 直接断开（原型无该上限特性）
      pos = buf.find("\r\n\r\n");
    }
    const std::string head = buf.substr(0, pos);
    buf.erase(0, pos + 4);

    // 极简请求行解析：METHOD SP TARGET SP VERSION
    const auto sp1 = head.find(' ');
    const auto sp2 = sp1 == std::string::npos ? std::string::npos : head.find(' ', sp1 + 1);
    const std::string method = sp1 == std::string::npos ? std::string() : head.substr(0, sp1);
    const std::string target =
        (sp1 == std::string::npos || sp2 == std::string::npos)
            ? std::string()
            : head.substr(sp1 + 1, sp2 - sp1 - 1);
    if (method != "GET") {
      static const char kBody[] = "method not allowed";
      SendAll(fd, std::string("HTTP/1.1 405 Method Not Allowed\r\nContent-Length: ") +
                      std::to_string(sizeof(kBody) - 1) + "\r\nConnection: close\r\n\r\n" + kBody);
      return;
    }
    if (target != "/blob") {
      static const char kBody[] = "not found";
      SendAll(fd, std::string("HTTP/1.1 404 Not Found\r\nContent-Length: ") +
                      std::to_string(sizeof(kBody) - 1) + "\r\nConnection: close\r\n\r\n" + kBody);
      return;
    }

    // 固定响应头：HTTP/1.1 + 精确 Content-Length + keep-alive（无 chunked）
    std::string header =
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
        std::to_string(size) + "\r\nConnection: keep-alive\r\n\r\n";
    if (!SendAll(fd, header)) return;

    // sendfile：文件 → socket，字节不进用户态。count 按 1 GiB 切分（>2 GiB 对象的分段循环），
    // 本实验的 256 MiB 文件一次调用即可发完，但保持"可扩展到 TB 级"的形态。
    off_t offset = 0;
    std::int64_t remaining = size;
    while (remaining > 0) {
      const auto want = static_cast<std::size_t>(
          std::min<std::int64_t>(remaining, 1LL << 30));
      const ssize_t n = ::sendfile(fd, file_fd, &offset, want);
      if (n < 0) {
        if (errno == EINTR) continue;
        return;  // EPIPE/ECONNRESET：客户端断开
      }
      if (n == 0) return;  // 文件提前 EOF（不可能，除非文件被截断）
      remaining -= n;
    }
  }
}

void WorkerLoop(int listen_fd, int file_fd, std::int64_t size) {
  while (g_stop == 0) {
    const int c = ::accept(listen_fd, nullptr, nullptr);
    if (c < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      if (g_stop != 0) return;
      // EMFILE/ENFILE 等：短暂退避，避免忙等（不是 busy loop）
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    SetNodelay(c);
    ServeConn(c, file_fd, size);
    ::close(c);
  }
}

// ---------------------------------------------------------------- 两种模式
int RunHttplib(const Options& opt, int file_fd, std::int64_t size) {
  fss::http::ServerOptions so;
  so.bind_address = "127.0.0.1";
  so.port = 0;  // 系统分配
  so.worker_threads = opt.threads;
  so.max_connections = opt.threads;  // 与 sendfile 侧相同的并发上限（C1.12：不得大于线程数）
  so.tcp_nodelay = true;
  // 基准时长内远大于请求数，避免服务端主动断连干扰（默认 100 会让 c16 出现大量重连）
  so.keep_alive_max_count = 1000000;

  SilentLogger logger;
  fss::SystemClock clock;
  fss::http::Server server(so, logger, clock);

  server.Get("/blob", fss::http::RouteOptions{.name = "ab_blob"},
             [file_fd, size](fss::http::Request& /*req*/) {
               return fss::http::Response::Stream(
                   200, "application/octet-stream",
                   std::make_shared<FileSource>(file_fd, size), size);
             });

  if (!server.Start()) {
    std::cerr << "httplib 服务端启动失败：" << server.last_error() << "\n";
    return 1;
  }
  std::cout << "PORT " << server.port() << std::endl;  // ★ stdout 第一行
  std::cerr << "READY mode=httplib threads=" << opt.threads << " max_connections="
            << opt.threads << " tcp_nodelay=1 keep_alive=1 content_length=" << size << "\n";

  while (g_stop == 0) ::pause();
  server.Stop();
  std::cerr << "STOP mode=httplib\n";
  return 0;
}

int RunSendfile(const Options& opt, int file_fd, std::int64_t size) {
  const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) {
    std::cerr << "socket 失败：" << std::strerror(errno) << "\n";
    return 1;
  }
  int on = 1;
  ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = 0;  // 系统分配
  addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "bind 失败：" << std::strerror(errno) << "\n";
    return 1;
  }
  // backlog 512：与 fss_http 包装层的修正值一致（ADR-002 补充），避免 c16 时丢 SYN
  if (::listen(lfd, 512) != 0) {
    std::cerr << "listen 失败：" << std::strerror(errno) << "\n";
    return 1;
  }
  socklen_t alen = sizeof(addr);
  if (::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &alen) != 0) {
    std::cerr << "getsockname 失败：" << std::strerror(errno) << "\n";
    return 1;
  }
  // 让阻塞的 accept 每秒醒一次检查 g_stop（配合 shutdown 双保险）
  timeval tv{};
  tv.tv_sec = 1;
  ::setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  g_listen_fd = lfd;
  g_waiting = 1;
  std::cout << "PORT " << ::ntohs(addr.sin_port) << std::endl;  // ★ stdout 第一行
  std::cerr << "READY mode=sendfile threads=" << opt.threads
            << " backlog=512 tcp_nodelay=1 keep_alive=1 content_length=" << size
            << " zero_copy=sendfile(2)\n";

  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(opt.threads));
  for (int i = 0; i < opt.threads; ++i) {
    workers.emplace_back([lfd, file_fd, size] { WorkerLoop(lfd, file_fd, size); });
  }

  while (g_stop == 0) ::pause();
  g_waiting = 0;
  ::shutdown(lfd, SHUT_RDWR);  // 唤醒仍阻塞在 accept 上的线程
  for (auto& t : workers) t.join();
  ::close(lfd);
  std::cerr << "STOP mode=sendfile\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  ::signal(SIGPIPE, SIG_IGN);  // 客户端断开时 sendfile/send 不得打死进程
  const Options opt = ParseArgs(argc, argv);

  std::int64_t size = 0;
  const std::string path = EnsureFile(opt, &size);
  const int file_fd = ::open(path.c_str(), O_RDONLY);
  if (file_fd < 0) Die("无法打开 " + path + "：" + std::strerror(errno));

  struct sigaction sa {};
  sa.sa_handler = OnSignal;
  ::sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // 不要 SA_RESTART：pause()/accept() 需要能看到 EINTR
  ::sigaction(SIGTERM, &sa, nullptr);
  ::sigaction(SIGINT, &sa, nullptr);

  const int rc = opt.mode == "sendfile" ? RunSendfile(opt, file_fd, size)
                                        : RunHttplib(opt, file_fd, size);
  ::close(file_fd);
  return rc;
}
