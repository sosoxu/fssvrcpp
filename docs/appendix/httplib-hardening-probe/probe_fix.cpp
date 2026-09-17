// 验证"用 httplib 做传输 + 自己兜住两道危险路径"的可行修法
#include <httplib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <string>
#include <thread>
static std::string Raw(int port, const std::string& req) {
  int fd=::socket(AF_INET,SOCK_STREAM,0);
  sockaddr_in a{}; a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  ::connect(fd,(sockaddr*)&a,sizeof(a));
  size_t s=0; while(s<req.size()){ssize_t n=::send(fd,req.data()+s,req.size()-s,0); if(n<=0)break; s+=(size_t)n;}
  std::string o; char b[8192]; ssize_t n;
  while((n=::recv(fd,b,sizeof(b),0))>0) o.append(b,(size_t)n);
  ::close(fd); return o;
}
int main(){
  setvbuf(stdout,nullptr,_IONBF,0);
  const size_t kMax = 1u<<20;          // 数据面每路由上限（示例）
  httplib::Server svr;
  svr.set_payload_max_length(kMax);

  // ① 前置拦截：Content-Length 已知时在读体之前就拒（避免 handler 被调用）
  svr.set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
    if (req.method == "PUT" || req.method == "POST") {
      auto cl = req.get_header_value("Content-Length");
      if (!cl.empty()) {
        try { if (std::stoull(cl) > kMax) {
          res.status = 413;
          res.set_content("{\"code\":413,\"reason\":\"Payload Too Large\"}", "application/json");
          return httplib::Server::HandlerResponse::Handled;
        }} catch (...) { res.status = 400; return httplib::Server::HandlerResponse::Handled; }
      }
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  svr.Put("/api/file/v1/transfer/:token",
    [&](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) {
      // ② 自封装计数 reader：兜住 chunked（无 Content-Length）超限与"0 字节假成功"
      size_t total = 0; bool over = false;
      bool stream_ok = reader([&](const char* d, size_t l) {
        (void)d; total += l;
        if (total > kMax) { over = true; return false; }   // 立即中止
        return true;
      });
      if (over) { res.status = 413; res.set_content("{\"code\":413}", "application/json"); return; }
      if (!stream_ok) { res.status = 400; res.set_content("{\"code\":400}", "application/json"); return; }
      auto cl = req.get_header_value("Content-Length");
      if (!cl.empty() && std::stoull(cl) != total) {        // ③ 防御性一致性断言
        res.status = 400;
        res.set_content("{\"code\":400,\"message\":\"body length mismatch\"}", "application/json");
        return;
      }
      res.status = 201;
      res.set_content("{\"bytes\":" + std::to_string(total) + "}", "application/json");
    });

  int port=svr.bind_to_any_port("127.0.0.1");
  std::thread th([&]{svr.listen_after_bind();});
  auto send=[&](const std::string& path,const std::string& body,bool chunked){
    std::string req = "PUT "+path+" HTTP/1.1\r\nHost: h\r\nConnection: close\r\n";
    if (chunked) {
      req += "Transfer-Encoding: chunked\r\n\r\n";
      char hx[32]; snprintf(hx,sizeof(hx),"%zx",body.size());
      req += std::string(hx)+"\r\n"+body+"\r\n0\r\n\r\n";
    } else {
      req += "Content-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
    }
    auto r = Raw(port,req);
    auto e = r.find("\r\n\r\n");
    printf("  %-8s %-8s %s | %s\n", chunked?"chunked":"CL", body.size()>kMax?"2MiB":"5B",
           r.substr(0,r.find("\r\n")).c_str(), e==std::string::npos?"":r.substr(e+4).c_str());
  };
  printf("[A] Content-Length 2MiB (> max)\n");  send("/api/file/v1/transfer/t1", std::string(2u<<20,'x'), false);
  printf("[B] chunked 2MiB (> max)\n");         send("/api/file/v1/transfer/t1", std::string(2u<<20,'x'), true);
  printf("[C] Content-Length 5B (ok)\n");       send("/api/file/v1/transfer/t1", "hello", false);
  printf("[D] chunked 5B (ok)\n");              send("/api/file/v1/transfer/t1", "hello", true);
  svr.stop(); th.join(); return 0;
}
