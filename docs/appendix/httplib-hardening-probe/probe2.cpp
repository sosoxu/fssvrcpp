// 补充边界：chunked 请求体 / HEAD / 超限拒绝 / base path / 关闭与超时
#include <httplib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
static std::string Raw(int port, const std::string& req) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::connect(fd, (sockaddr*)&a, sizeof(a));
  ::send(fd, req.data(), req.size(), 0);
  std::string out; char b[8192]; ssize_t n;
  while ((n = ::recv(fd, b, sizeof(b), 0)) > 0) out.append(b, (size_t)n);
  ::close(fd); return out;
}
static std::string StatusLine(const std::string& s) {
  auto e = s.find("\r\n"); return e == std::string::npos ? s : s.substr(0, e);
}
int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char* p = "/tmp/httplib_probe/stored.bin";
  httplib::Server svr;
  svr.new_task_queue = [] { return new httplib::ThreadPool(8); };
  svr.set_payload_max_length(1u << 20);          // 1 MiB，便于测 413
  // 模拟 base_path 前缀
  svr.Put("/api/file/v1/transfer/:token", [](const httplib::Request& req, httplib::Response& res,
                                            const httplib::ContentReader& reader) {
    size_t total = 0;
    reader([&](const char* d, size_t l) { total += l; (void)d; return true; });
    res.status = 201;
    res.set_content("{\"chunked_ok\":true,\"bytes\":" + std::to_string(total) + "}", "application/json");
  });
  svr.Get("/api/file/v1/transfer/:token", [p](const httplib::Request&, httplib::Response& res) {
    auto f = std::make_shared<std::ifstream>(p, std::ios::binary | std::ios::ate);
    const size_t size = static_cast<size_t>(f->tellg());
    res.set_header("Accept-Ranges", "bytes");
    res.set_content_provider(size, "application/octet-stream",
      [f](size_t off, size_t len, httplib::DataSink& sink) {
        f->clear(); f->seekg((std::streamoff)off);
        std::string b(len, 0); f->read(b.data(), (std::streamsize)len);
        auto got = f->gcount(); if (got <= 0) return false;
        sink.write(b.data(), (size_t)got); return true;
      });
  });
  int port = svr.bind_to_any_port("127.0.0.1");
  std::thread th([&] { svr.listen_after_bind(); });

  // [A] chunked 请求体
  {
    std::string body = "hello-chunked-world";
    char hex[32]; snprintf(hex, sizeof(hex), "%zx", body.size());
    std::string req = "PUT /api/file/v1/transfer/tok1 HTTP/1.1\r\nHost: h\r\n"
                      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
    req += hex; req += "\r\n"; req += body; req += "\r\n0\r\n\r\n";
    auto r = Raw(port, req);
    printf("[A] chunked PUT          -> %s | %s\n", StatusLine(r).c_str(),
           r.substr(r.find("\r\n\r\n") + 4).c_str());
  }
  // [B] HEAD
  {
    auto r = Raw(port, "HEAD /api/file/v1/transfer/tok1 HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    printf("[B] HEAD                 -> %s (body_len=%zu)\n", StatusLine(r).c_str(),
           r.size() - r.find("\r\n\r\n") - 4);
  }
  // [C] 超限请求体 -> 413？
  {
    std::string big(2u << 20, 'x');
    std::string req = "PUT /api/file/v1/transfer/tok1 HTTP/1.1\r\nHost: h\r\nConnection: close\r\n"
                      "Content-Length: " + std::to_string(big.size()) + "\r\n\r\n";
    req += big;
    auto r = Raw(port, req);
    printf("[C] body > max_length   -> %s\n", StatusLine(r).c_str());
  }
  // [D] 超长请求头
  {
    std::string req = "GET /api/file/v1/transfer/tok1 HTTP/1.1\r\nHost: h\r\nX-Big: ";
    req += std::string(64 * 1024, 'a');
    req += "\r\nConnection: close\r\n\r\n";
    auto r = Raw(port, req);
    printf("[D] 64KiB header        -> %s\n", StatusLine(r).c_str());
  }
  // [E] 未知方法 / 不存在的路径
  {
    auto r1 = Raw(port, "DELETE /api/file/v1/transfer/tok1 HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    auto r2 = Raw(port, "GET /nope HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    printf("[E] unknown method      -> %s ; unknown path -> %s\n", StatusLine(r1).c_str(), StatusLine(r2).c_str());
  }
  // [F] keep-alive 两次请求同一连接
  {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::connect(fd, (sockaddr*)&a, sizeof(a));
    const char* r1 = "GET /api/file/v1/transfer/tok1 HTTP/1.1\r\nHost: h\r\nRange: bytes=0-9\r\n\r\n";
    ::send(fd, r1, strlen(r1), 0);
    char b[4096]; ssize_t n = ::recv(fd, b, sizeof(b), 0);
    std::string first(b, n > 0 ? (size_t)n : 0);
    const char* r2 = "GET /api/file/v1/transfer/tok1 HTTP/1.1\r\nHost: h\r\nRange: bytes=0-9\r\nConnection: close\r\n\r\n";
    ::send(fd, r2, strlen(r2), 0);
    n = ::recv(fd, b, sizeof(b), 0);
    std::string second(b, n > 0 ? (size_t)n : 0);
    ::close(fd);
    printf("[F] keep-alive 1st=%s 2nd=%s\n", StatusLine(first).c_str(), StatusLine(second).c_str());
  }
  svr.stop(); th.join();
  return 0;
}
