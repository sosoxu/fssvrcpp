// 独立进程负载生成器：原始 socket，正确的响应分帧，连接被关时重连
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
struct Conn{
  int fd=-1; std::string buf; std::string req;
  bool connect_(int port){
    fd=::socket(AF_INET,SOCK_STREAM,0);
    int on=1; ::setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&on,sizeof(on));
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(::connect(fd,(sockaddr*)&a,sizeof(a))!=0){::close(fd);fd=-1;return false;}
    return true;
  }
  // 发一个 Range 请求并读回完整响应，返回 body 字节数；-1 表示需重连
  long long fetch(long long off,size_t seg){
    char r[256];
    int n=snprintf(r,sizeof(r),
      "GET /big HTTP/1.1\r\nHost: h\r\nAccept-Encoding: identity\r\nRange: bytes=%lld-%lld\r\n\r\n",
      off,off+(long long)seg-1);
    if(::send(fd,r,(size_t)n,0)<0) return -1;
    for(;;){
      size_t he=buf.find("\r\n\r\n");
      if(he==std::string::npos){ if(!pump()) return -1; continue; }
      std::string head=buf.substr(0,he);
      if(head.find(" 200 ")==std::string::npos && head.find(" 206 ")==std::string::npos){
        buf.clear(); return -1; }
      size_t p=head.find("Content-Length: ");
      if(p==std::string::npos){ buf.clear(); return -1; }
      size_t cl=strtoul(head.c_str()+p+16,nullptr,10);
      size_t need=he+4+cl;
      while(buf.size()<need) if(!pump()) return -1;
      long long got=(long long)cl; buf.erase(0,need); return got;
    }
  }
  bool pump(){ char t[65536]; ssize_t n=::recv(fd,t,sizeof(t),0); if(n<=0) return false; buf.append(t,(size_t)n); return true; }
};
int main(int argc,char**argv){
  int port=atoi(argv[1]); int T=atoi(argv[2]); int secs=atoi(argv[3]); size_t seg=(size_t)atoi(argv[4])*1024;
  const long long FS=200LL*1024*1024*1024;
  std::atomic<bool> stop{false}; std::atomic<long long> bytes{0},reqs{0},errs{0};
  std::vector<std::thread> ts;
  for(int i=0;i<T;i++) ts.emplace_back([&,i]{
    Conn c; if(!c.connect_(port)){errs++;return;}
    unsigned long long seed=12345+i*7919;
    while(!stop.load(std::memory_order_relaxed)){
      seed=seed*6364136223846793005ULL+1442695040888963407ULL;
      long long off=(long long)((seed>>17)%(unsigned long long)(FS-2*1024*1024));
      long long got=c.fetch(off,seg);
      if(got<0){ errs++; ::close(c.fd); c.buf.clear(); if(!c.connect_(port)){std::this_thread::sleep_for(std::chrono::milliseconds(10));} continue; }
      bytes+=got; reqs++;
    }
    if(c.fd>=0)::close(c.fd);
  });
  std::this_thread::sleep_for(std::chrono::seconds(secs)); stop=true;
  for(auto&t:ts)t.join();
  double s=secs;
  printf("T=%-4d seg=%-5zuKiB -> %7.2f GiB/s  %8.0f req/s  bytes=%lld reqs=%lld err=%lld\n",
    T,seg/1024,bytes.load()/1073741824.0/s,reqs.load()/s,bytes.load(),reqs.load(),errs.load());
  return 0;
}
