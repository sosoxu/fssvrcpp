// 对 /io 端点发 N 个并发请求，测吞吐与总耗时
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
using clk=std::chrono::steady_clock;
int main(int argc,char**argv){
  int port=atoi(argv[1]),T=atoi(argv[2]),PER=atoi(argv[3]);
  std::atomic<long long> ok{0},err{0};
  std::vector<std::thread> ts; auto t0=clk::now();
  for(int i=0;i<T;i++)ts.emplace_back([&]{
    for(int j=0;j<PER;j++){
      int fd=::socket(AF_INET,SOCK_STREAM,0); int on=1;
      ::setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&on,sizeof(on));
      sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
      if(::connect(fd,(sockaddr*)&a,sizeof(a))!=0){err++;::close(fd);continue;}
      const char* r="GET /io HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";
      ::send(fd,r,strlen(r),0);
      // 按 Content-Length 分帧，不能等 recv 返回 0（服务端可能保持连接）
      std::string buf; char b[65536]; bool got=false;
      for(;;){
        size_t he=buf.find("\r\n\r\n");
        if(he!=std::string::npos){
          size_t cp=buf.find("Content-Length: ");
          if(cp!=std::string::npos){
            size_t cl=strtoul(buf.c_str()+cp+16,nullptr,10);
            while(buf.size()<he+4+cl){ ssize_t n=::recv(fd,b,sizeof(b),0); if(n<=0)break; buf.append(b,(size_t)n); }
            if(buf.size()>=he+4+cl){got=true;}
          } else got=true;
          break;
        }
        ssize_t n=::recv(fd,b,sizeof(b),0); if(n<=0) break; buf.append(b,(size_t)n);
      }
      if(got) ok++; else err++;
      ::close(fd);
    }});
  for(auto&t:ts)t.join();
  double s=std::chrono::duration<double>(clk::now()-t0).count();
  printf("  并发=%-4d 每连接 %d 次 → %6.1f req/s, 总耗时 %.2f s, ok=%lld err=%lld\n",
         T,PER,(double)(T*PER)/s,s,ok.load(),err.load());
  return 0;
}
