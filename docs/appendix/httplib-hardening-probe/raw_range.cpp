// 用原始 socket 观察越界 Range 的真实线上响应
#include <httplib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char* p = "/tmp/httplib_probe/stored.bin";
  httplib::Server svr;
  svr.Get("/d", [p](const httplib::Request&, httplib::Response& res) {
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
  auto raw = [&](const std::string& req) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::connect(fd, (sockaddr*)&a, sizeof(a));
    ::send(fd, req.data(), req.size(), 0);
    std::string out; char buf[4096]; ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) out.append(buf, (size_t)n);
    ::close(fd); return out;
  };
  for (const char* r : {"bytes=999999999-", "bytes=0-1,5-6"}) {
    std::string req = std::string("GET /d HTTP/1.1\r\nHost: h\r\nConnection: close\r\nRange: ") + r + "\r\n\r\n";
    std::string resp = raw(req);
    printf("Range: %-18s -> %s\n", r, resp.substr(0, resp.find("\r\n")).c_str());
    size_t hdr_end = resp.find("\r\n\r\n");
    if (hdr_end != std::string::npos)
      printf("    headers: %s\n", resp.substr(0, hdr_end).c_str());
  }
  svr.stop(); th.join();
  return 0;
}
