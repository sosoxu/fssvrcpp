// =============================================================================
//  tests/framework/raw_http.h —— 原始 socket HTTP 客户端（测试专用）
// =============================================================================
//  为什么不用现成客户端：
//    C1.2 要验证的是**线缆上的字节**——是否出现下溢的 `Content-Length`、
//    重复 `Content-Length` 会怎样、`CL` 与 `chunked` 并存会怎样、64 KiB 头会怎样。
//    这些都必须能手工构造畸形请求并逐字节读回响应。用被测库自己的客户端去做这件事，
//    既做不到，也失去独立性（R1 的自证对照需要独立观测手段）。
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fss::test {

struct RawResponse {
  int status = 0;
  std::string reason;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::string raw_head;  // 状态行 + 头（原始字节，便于断言"没有下溢长度"）
  bool chunked = false;
  bool body_truncated = false;

  std::optional<std::string> Header(std::string_view name) const {
    for (const auto& [k, v] : headers) {
      if (k.size() == name.size()) {
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
    }
    return std::nullopt;
  }
  std::size_t HeaderCount(std::string_view name) const {
    std::size_t n = 0;
    for (const auto& [k, v] : headers) {
      if (k.size() == name.size()) {
        bool same = true;
        for (std::size_t i = 0; i < k.size(); ++i) {
          if (std::tolower(static_cast<unsigned char>(k[i])) !=
              std::tolower(static_cast<unsigned char>(name[i]))) {
            same = false;
            break;
          }
        }
        if (same) ++n;
      }
    }
    return n;
  }
};

class RawClient {
 public:
  RawClient(int port, bool tcp_nodelay = false) : port_(port), tcp_nodelay_(tcp_nodelay) {}
  ~RawClient() { Close(); }
  RawClient(const RawClient&) = delete;
  RawClient& operator=(const RawClient&) = delete;

  bool Connect(int timeout_ms = 3000) {
    Close();
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    if (tcp_nodelay_) {
      int on = 1;
      ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port_));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      Close();
      return false;
    }
    opened_ = true;
    buf_.clear();
    return true;
  }

  void Close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    opened_ = false;
    buf_.clear();
  }

  bool is_open() const { return opened_; }
  int fd() const { return fd_; }

  bool Send(std::string_view data) {
    if (fd_ < 0) return false;
    std::size_t sent = 0;
    while (sent < data.size()) {
      const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) return false;
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

  // 构造并发送一个请求；`extra_headers` 原样拼进头部（**故意允许畸形**）
  bool SendRequest(std::string_view method, std::string_view target,
                   const std::vector<std::string>& extra_headers = {}, std::string_view body = {},
                   bool close_after = false) {
    std::string req;
    req += method;
    req += ' ';
    req += target;
    req += " HTTP/1.1\r\nHost: h\r\n";
    for (const auto& h : extra_headers) {
      req += h;
      req += "\r\n";
    }
    if (close_after) req += "Connection: close\r\n";
    req += "\r\n";
    req += body;
    return Send(req);
  }

  // 分块发送请求体（客户端负责切块）
  bool SendChunkedRequest(std::string_view method, std::string_view target,
                          const std::vector<std::string>& chunks,
                          const std::vector<std::string>& extra_headers = {}) {
    std::string head;
    head += method;
    head += ' ';
    head += target;
    head += " HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n";
    for (const auto& h : extra_headers) {
      head += h;
      head += "\r\n";
    }
    head += "\r\n";
    if (!Send(head)) return false;
    std::string payload;
    for (const auto& c : chunks) {
      char size_line[32];
      std::snprintf(size_line, sizeof size_line, "%zx\r\n", c.size());
      payload += size_line;
      payload += c;
      payload += "\r\n";
    }
    payload += "0\r\n\r\n";
    return Send(payload);
  }

  // 读一个完整响应（支持 Content-Length / chunked / 读到关闭）
  std::optional<RawResponse> ReadResponse(int timeout_ms = 5000) {
    RawResponse res;
    // 1) 头部
    if (!ReadUntilHead(timeout_ms)) return std::nullopt;
    const auto head_end = buf_.find("\r\n\r\n");
    res.raw_head = buf_.substr(0, head_end + 4);
    ParseHead(res.raw_head, &res);
    buf_.erase(0, head_end + 4);

    // 2) body
    if (res.Header("Transfer-Encoding").has_value()) {
      res.chunked = true;
      if (!ReadChunkedBody(&res, timeout_ms)) res.body_truncated = true;
      return res;
    }
    if (const auto cl = res.Header("Content-Length"); cl.has_value()) {
      const auto want = static_cast<std::size_t>(std::strtoull(cl->c_str(), nullptr, 10));
      if (!ReadExact(want, timeout_ms)) res.body_truncated = true;
      res.body.assign(buf_.data(), std::min(want, buf_.size()));
      buf_.erase(0, std::min(want, buf_.size()));
      return res;
    }
    // 没有长度信息：读到对端关闭
    ReadUntilClosed(timeout_ms);
    res.body = buf_;
    buf_.clear();
    return res;
  }

  // 只读头部；body 留在内部缓冲里（大响应场景：先拿长度，再丢弃）
  std::optional<RawResponse> ReadHead(int timeout_ms = 5000) {
    RawResponse res;
    if (!ReadUntilHead(timeout_ms)) return std::nullopt;
    const auto head_end = buf_.find("\r\n\r\n");
    res.raw_head = buf_.substr(0, head_end + 4);
    ParseHead(res.raw_head, &res);
    buf_.erase(0, head_end + 4);
    return res;
  }

  // 丢弃 body 的剩余部分（**不驻留内存**），返回丢弃的总字节数
  std::uint64_t DiscardBody(std::uint64_t expected_total, int timeout_ms = 60000) {
    std::uint64_t total = 0;
    if (!buf_.empty()) {
      const auto take = std::min<std::uint64_t>(buf_.size(), expected_total);
      buf_.erase(0, static_cast<std::size_t>(take));
      total += take;
    }
    while (total < expected_total) {
      if (!Fill(timeout_ms)) break;
      const auto take = std::min<std::uint64_t>(buf_.size(), expected_total - total);
      buf_.erase(0, static_cast<std::size_t>(take));
      total += take;
    }
    return total;
  }

  // 只为"大响应"准备：把 body 读掉并丢弃，返回字节数（不驻留内存）
  std::uint64_t ReadAndDiscardBody(std::size_t expect_len, int timeout_ms = 60000) {
    std::uint64_t total = 0;
    while (total < expect_len) {
      if (buf_.empty()) {
        if (!Fill(timeout_ms)) break;
        continue;
      }
      total += buf_.size();
      buf_.clear();
    }
    return total;
  }

  const std::string& pending() const { return buf_; }

 private:
  bool Fill(int timeout_ms) {
    if (fd_ < 0) return false;
    pollfd p{fd_, POLLIN, 0};
    const int rc = ::poll(&p, 1, timeout_ms);
    if (rc <= 0) return false;
    char tmp[64 * 1024];
    const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
    if (n <= 0) {
      opened_ = false;
      return false;
    }
    buf_.append(tmp, static_cast<std::size_t>(n));
    return true;
  }

  bool ReadUntilHead(int timeout_ms) {
    while (buf_.find("\r\n\r\n") == std::string::npos) {
      if (!Fill(timeout_ms)) return false;
    }
    return true;
  }

  bool ReadExact(std::size_t n, int timeout_ms) {
    while (buf_.size() < n) {
      if (!Fill(timeout_ms)) return false;
    }
    return true;
  }

  bool ReadChunkedBody(RawResponse* res, int timeout_ms) {
    while (true) {
      // 块头（可能带 chunk extension，忽略之）
      std::size_t line_end = std::string::npos;
      while ((line_end = buf_.find("\r\n")) == std::string::npos) {
        if (!Fill(timeout_ms)) return false;
      }
      const std::string size_line = buf_.substr(0, line_end);
      buf_.erase(0, line_end + 2);
      const auto semi = size_line.find(';');
      const auto hex = semi == std::string::npos ? size_line : size_line.substr(0, semi);
      const auto chunk_len = static_cast<std::size_t>(std::strtoull(hex.c_str(), nullptr, 16));
      if (chunk_len == 0) {
        // trailer（可空）+ 结束的 CRLF
        while (buf_.find("\r\n") == std::string::npos) {
          if (!Fill(timeout_ms)) return false;
        }
        const auto end = buf_.find("\r\n");
        buf_.erase(0, end + 2);
        return true;
      }
      if (!ReadExact(chunk_len + 2, timeout_ms)) return false;
      res->body.append(buf_.data(), chunk_len);
      buf_.erase(0, chunk_len + 2);
    }
  }

  void ReadUntilClosed(int timeout_ms) {
    while (Fill(timeout_ms)) {
    }
  }

  static void ParseHead(const std::string& head, RawResponse* res) {
    std::size_t pos = 0;
    const auto first_end = head.find("\r\n");
    const std::string status_line = head.substr(0, first_end);
    // `HTTP/1.1 416 Range Not Satisfiable`
    const auto sp1 = status_line.find(' ');
    if (sp1 != std::string::npos) {
      const auto sp2 = status_line.find(' ', sp1 + 1);
      res->status = std::atoi(status_line.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
      if (sp2 != std::string::npos) res->reason = status_line.substr(sp2 + 1);
    }
    pos = first_end + 2;
    while (pos < head.size()) {
      const auto end = head.find("\r\n", pos);
      if (end == std::string::npos) break;
      if (end == pos) break;
      const std::string line = head.substr(pos, end - pos);
      const auto colon = line.find(':');
      if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        const auto b = value.find_first_not_of(" \t");
        if (b != std::string::npos) value = value.substr(b);
        res->headers.emplace_back(std::move(key), std::move(value));
      }
      pos = end + 2;
    }
  }

  int port_;
  bool tcp_nodelay_;
  int fd_ = -1;
  bool opened_ = false;
  std::string buf_;
};

// 进程的峰值常驻内存（KiB）—— 用于 C1.3 的"流式不整块驻留"断言
inline std::uint64_t PeakRssKib() {
  std::FILE* f = std::fopen("/proc/self/status", "r");
  if (f == nullptr) return 0;
  char line[256];
  std::uint64_t value = 0;
  while (std::fgets(line, sizeof line, f) != nullptr) {
    if (std::strncmp(line, "VmHWM:", 6) == 0) {
      std::sscanf(line + 6, "%lu", &value);
      break;
    }
  }
  std::fclose(f);
  return value;
}

inline std::uint64_t CurrentRssKib() {
  std::FILE* f = std::fopen("/proc/self/status", "r");
  if (f == nullptr) return 0;
  char line[256];
  std::uint64_t value = 0;
  while (std::fgets(line, sizeof line, f) != nullptr) {
    if (std::strncmp(line, "VmRSS:", 6) == 0) {
      std::sscanf(line + 6, "%lu", &value);
      break;
    }
  }
  std::fclose(f);
  return value;
}

}  // namespace fss::test
