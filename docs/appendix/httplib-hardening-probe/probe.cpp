// 验证 cpp-httplib(上游原版单头文件) 是否满足本项目的"数据面"需求
#include <httplib.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <openssl/sha.h>

static std::string Sha256Hex(const std::string& data) {
  unsigned char md[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), md);
  static const char* h = "0123456789abcdef";
  std::string out;
  for (unsigned char c : md) { out.push_back(h[c >> 4]); out.push_back(h[c & 15]); }
  return out;
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const std::string stored_path = "/tmp/httplib_probe/stored.bin";

  httplib::Server svr;
  svr.new_task_queue = [] { return new httplib::ThreadPool(8); };
  svr.set_payload_max_length(1ull << 30);   // 1 GiB 上限

  // ---- 1) 流式接收 PUT：不整块驻留内存 ----
  svr.Put("/upload", [&](const httplib::Request&, httplib::Response& res,
                         const httplib::ContentReader& reader) {
    std::ofstream out(stored_path, std::ios::binary | std::ios::trunc);
    unsigned long long total = 0;
    reader([&](const char* data, size_t len) {
      out.write(data, static_cast<std::streamsize>(len));
      total += len;
      return true;
    });
    out.close();
    res.status = 201;
    res.set_content("{\"bytes\":" + std::to_string(total) + "}", "application/json");
  });

  // ---- 2) 流式发送 GET（由 httplib 处理 Range）----
  svr.Get("/download", [&](const httplib::Request& req, httplib::Response& res) {
    auto f = std::make_shared<std::ifstream>(stored_path, std::ios::binary | std::ios::ate);
    if (!*f) { res.status = 404; return; }
    const size_t size = static_cast<size_t>(f->tellg());
    res.set_header("Accept-Ranges", "bytes");
    res.set_content_provider(size, "application/octet-stream",
      [f](size_t offset, size_t length, httplib::DataSink& sink) {
        f->clear();
        f->seekg(static_cast<std::streamoff>(offset));
        std::vector<char> buf(length);
        f->read(buf.data(), static_cast<std::streamsize>(length));
        std::streamsize got = f->gcount();
        if (got <= 0) return false;
        sink.write(buf.data(), static_cast<size_t>(got));
        return true;
      });
  });

  int port = svr.bind_to_any_port("127.0.0.1");
  std::thread th([&] { svr.listen_after_bind(); });
  httplib::Client cli("127.0.0.1", port);
  cli.set_read_timeout(30, 0);
  cli.set_write_timeout(30, 0);

  // ================= 用例 1：流式上传 8 MiB =================
  std::string payload;
  payload.reserve(8u << 20);
  for (size_t i = 0; i < (8u << 20); ++i) payload.push_back(static_cast<char>('A' + (i % 26)));
  const std::string want_sha = Sha256Hex(payload);

  auto up = cli.Put("/upload", payload.data(), payload.size(), "application/octet-stream");
  std::cout << "[1] streaming PUT  status=" << (up ? up->status : -1)
            << " body=" << (up ? up->body : "NULL") << "\n";
  if (!up || up->status != 201) { std::cout << "FAIL\n"; svr.stop(); th.join(); return 1; }

  // 落盘内容与原文一致？
  { std::ifstream f(stored_path, std::ios::binary); std::string got((std::istreambuf_iterator<char>(f)), {});
    std::cout << "[1] stored sha match = " << (Sha256Hex(got) == want_sha ? "YES" : "NO")
              << "  size=" << got.size() << "\n";
    if (Sha256Hex(got) != want_sha) { std::cout << "FAIL\n"; svr.stop(); th.join(); return 1; } }

  // ================= 用例 2：Range 请求 =================
  {
    httplib::Headers h{{"Range", "bytes=100-199"}};
    auto r = cli.Get("/download", h);
    std::cout << "[2] Range bytes=100-199  status=" << (r ? r->status : -1)
              << " len=" << (r ? r->body.size() : 0)
              << " Content-Range=" << (r && r->has_header("Content-Range") ? r->get_header_value("Content-Range") : "(none)")
              << " Accept-Ranges=" << (r && r->has_header("Accept-Ranges") ? r->get_header_value("Accept-Ranges") : "(none)")
              << "\n";
    bool ok = r && r->status == 206 && r->body.size() == 100 &&
              r->body == payload.substr(100, 100) &&
              r->has_header("Content-Range");
    std::cout << "[2] " << (ok ? "PASS" : "FAIL") << "\n";
    if (!ok) { std::cout << "FAIL\n"; svr.stop(); th.join(); return 1; }
  }
  {  // bytes=-50 （末尾 50 字节）
    httplib::Headers h{{"Range", "bytes=-50"}};
    auto r = cli.Get("/download", h);
    std::cout << "[3] Range bytes=-50      status=" << (r ? r->status : -1)
              << " len=" << (r ? r->body.size() : 0) << "\n";
    bool ok = r && r->status == 206 && r->body.size() == 50 &&
              r->body == payload.substr(payload.size() - 50);
    std::cout << "[3] " << (ok ? "PASS" : "FAIL") << "\n";
    if (!ok) { std::cout << "FAIL\n"; svr.stop(); th.join(); return 1; }
  }
  {  // 越界 → 416
    httplib::Headers h{{"Range", "bytes=999999999-"}};
    auto r = cli.Get("/download", h);
    std::cout << "[4] Range out-of-bounds  status=" << (r ? r->status : -1)
              << "  -> " << ((r && r->status == 416) ? "PASS" : "FAIL") << "\n";
    if (!r || r->status != 416) { std::cout << "FAIL\n"; svr.stop(); th.join(); return 1; }
  }
  {  // 无 Range → 200 全量
    auto r = cli.Get("/download");
    bool ok = r && r->status == 200 && r->body.size() == payload.size() && r->body == payload;
    std::cout << "[5] full GET             status=" << (r ? r->status : -1)
              << " len=" << (r ? r->body.size() : 0) << "  -> " << (ok ? "PASS" : "FAIL") << "\n";
    if (!ok) { std::cout << "FAIL\n"; svr.stop(); th.join(); return 1; }
  }

  // ================= 用例 6：并发 =================
  {
    std::atomic<int> ok{0}, bad{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 50; ++t) ts.emplace_back([&] {
      httplib::Client c("127.0.0.1", port);
      auto r = c.Get("/download");
      if (r && r->status == 200 && r->body.size() == payload.size()) ok++; else bad++;
    });
    for (auto& t : ts) t.join();
    std::cout << "[6] 50 concurrent GET    ok=" << ok << " bad=" << bad
              << "  -> " << (bad == 0 ? "PASS" : "FAIL") << "\n";
    if (bad != 0) { std::cout << "FAIL\n"; svr.stop(); th.join(); return 1; }
  }

  // ================= 用例 7：畸形请求被拒 =================
  {
    // 原始 socket 手工发一个"重复 Content-Length"请求
    auto raw = [&](const std::string& req) {
      int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
      a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
      ::send(fd, req.data(), req.size(), 0);
      char buf[512]{}; ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
      ::close(fd);
      return std::string(buf, n > 0 ? static_cast<size_t>(n) : 0);
    };
    const std::string req =
        "GET /download HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Content-Length: 0\r\nContent-Length: 5\r\n\r\n";
    const std::string resp = raw(req);
    const std::string first = resp.substr(0, resp.find("\r\n"));
    std::cout << "[7] duplicate CL         -> " << first << "\n";
  }

  svr.stop();
  th.join();
  std::cout << "\nALL PROBE CASES DONE\n";
  return 0;
}
