#include <httplib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <string>
#include <thread>
static std::string Raw(int port,const std::string& req){
  int fd=::socket(AF_INET,SOCK_STREAM,0);
  sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  ::connect(fd,(sockaddr*)&a,sizeof(a));
  size_t s=0;while(s<req.size()){ssize_t n=::send(fd,req.data()+s,req.size()-s,0);if(n<=0)break;s+=(size_t)n;}
  std::string o;char b[8192];ssize_t n;
  while((n=::recv(fd,b,sizeof(b),0))>0)o.append(b,(size_t)n);
  ::close(fd);return o;
}
int main(){
  setvbuf(stdout,nullptr,_IONBF,0);
  const size_t kMax=1u<<20;
  httplib::Server svr; svr.set_payload_max_length(kMax);
  svr.Put("/t/:tok",[&](const httplib::Request& req,httplib::Response& res,const httplib::ContentReader& reader){
    size_t total=0; bool over=false;
    bool ok = reader([&](const char* d,size_t l){
      (void)d; total+=l;
      if(total>kMax){ over=true; res.status=413;               // ← 在回调内设定
                      res.set_content("{\"code\":413}", "application/json"); return false; }
      return true; });
    if(over) return;                                            // 让回调里设的状态保留
    if(!ok){ res.status=400; res.set_content("{\"code\":400}","application/json"); return; }
    res.status=201; res.set_content("{\"bytes\":"+std::to_string(total)+"}","application/json");
  });
  int port=svr.bind_to_any_port("127.0.0.1");
  std::thread th([&]{svr.listen_after_bind();});
  auto go=[&](const std::string& body,bool chunked){
    std::string req="PUT /t/x HTTP/1.1\r\nHost: h\r\nConnection: close\r\n";
    if(chunked){ char hx[32]; snprintf(hx,sizeof(hx),"%zx",body.size());
      req+="Transfer-Encoding: chunked\r\n\r\n"+std::string(hx)+"\r\n"+body+"\r\n0\r\n\r\n"; }
    else req+="Content-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
    auto r=Raw(port,req); auto e=r.find("\r\n\r\n");
    printf("  %-8s %-6s %s | %s\n", chunked?"chunked":"CL", body.size()>kMax?"2MiB":"5B",
           r.substr(0,r.find("\r\n")).c_str(), e==std::string::npos?"":r.substr(e+4).c_str());
  };
  printf("chunked 2MiB -> 期望 413:\n"); go(std::string(2u<<20,'x'),true);
  printf("chunked 5B   -> 期望 201:\n"); go("hello",true);
  printf("CL 2MiB      -> 期望 413（前置拦截）:\n"); go(std::string(2u<<20,'x'),false);
  svr.stop(); th.join(); return 0;
}
