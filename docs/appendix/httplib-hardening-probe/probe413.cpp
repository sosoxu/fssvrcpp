#include <httplib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <string>
#include <thread>
static std::string Raw(int port, const std::string& req) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  ::connect(fd,(sockaddr*)&a,sizeof(a));
  size_t sent=0; while(sent<req.size()){ ssize_t n=::send(fd,req.data()+sent,req.size()-sent,0); if(n<=0)break; sent+=(size_t)n; }
  std::string out; char b[8192]; ssize_t n;
  while((n=::recv(fd,b,sizeof(b),0))>0) out.append(b,(size_t)n);
  ::close(fd); return out;
}
int main(){
  setvbuf(stdout,nullptr,_IONBF,0);
  httplib::Server svr;
  svr.set_payload_max_length(1u<<20);
  // (a) ContentReader 流式 handler
  svr.Put("/reader",[](const httplib::Request&,httplib::Response& res,const httplib::ContentReader& r){
    size_t t=0; r([&](const char* d,size_t l){ (void)d; t+=l; return true; });
    res.status=201; res.set_content("reader_bytes="+std::to_string(t),"text/plain");
  });
  // (b) 缓冲 body handler
  svr.Put("/buffered",[](const httplib::Request& req,httplib::Response& res){
    res.status=201; res.set_content("buffered_bytes="+std::to_string(req.body.size()),"text/plain");
  });
  int port=svr.bind_to_any_port("127.0.0.1");
  std::thread th([&]{svr.listen_after_bind();});
  for (const char* path : {"/reader","/buffered"}) {
    std::string big((2u<<20),'x');
    std::string req = std::string("PUT ")+path+" HTTP/1.1\r\nHost: h\r\nConnection: close\r\nContent-Length: "
                    + std::to_string(big.size()) + "\r\n\r\n" + big;
    auto r = Raw(port, req);
    auto e = r.find("\r\n\r\n");
    printf("%-10s 2MiB vs limit 1MiB -> %s | %s\n", path, r.substr(0,r.find("\r\n")).c_str(),
           e==std::string::npos?"(no body)":r.substr(e+4).c_str());
  }
  { // 正常大小
    std::string small="hello";
    std::string req = std::string("PUT /reader HTTP/1.1\r\nHost: h\r\nConnection: close\r\nContent-Length: ")
                    + std::to_string(small.size()) + "\r\n\r\n" + small;
    auto r = Raw(port, req); auto e=r.find("\r\n\r\n");
    printf("%-10s 5B    vs limit 1MiB -> %s | %s\n","/reader", r.substr(0,r.find("\r\n")).c_str(),
           e==std::string::npos?"(no body)":r.substr(e+4).c_str());
  }
  svr.stop(); th.join(); return 0;
}
