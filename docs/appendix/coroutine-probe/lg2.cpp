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
struct C{int fd=-1;std::string buf;
  bool conn(int p){fd=::socket(AF_INET,SOCK_STREAM,0);int on=1;::setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&on,sizeof(on));
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(p);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(::connect(fd,(sockaddr*)&a,sizeof(a))!=0){::close(fd);fd=-1;return false;}return true;}
  bool pump(){char t[65536];ssize_t n=::recv(fd,t,sizeof(t),0);if(n<=0)return false;buf.append(t,(size_t)n);return true;}
  bool one(){const char* r="GET /s HTTP/1.1\r\nHost: h\r\n\r\n";
    if(::send(fd,r,strlen(r),0)<0)return false;
    for(;;){size_t he=buf.find("\r\n\r\n");if(he==std::string::npos){if(!pump())return false;continue;}
      size_t p=buf.find("Content-Length: ");if(p==std::string::npos){buf.clear();return false;}
      size_t cl=strtoul(buf.c_str()+p+16,nullptr,10);size_t need=he+4+cl;
      while(buf.size()<need)if(!pump())return false; buf.erase(0,need);return true;}}
};
int main(int argc,char**argv){
  int port=atoi(argv[1]),T=atoi(argv[2]),secs=atoi(argv[3]);
  std::atomic<bool> stop{false};std::atomic<long long> cnt{0},err{0};
  std::vector<std::thread> ts;
  for(int i=0;i<T;i++)ts.emplace_back([&]{C c;if(!c.conn(port)){err++;return;}
    long long l=0;
    while(!stop.load(std::memory_order_relaxed)){if(c.one())l++;else{err++;if(!c.conn(port))break;}}
    cnt+=l;if(c.fd>=0)::close(c.fd);});
  std::this_thread::sleep_for(std::chrono::seconds(secs));stop=true;
  for(auto&t:ts)t.join();
  printf("  T=%-5d → %9.0f req/s (err=%lld)\n",T,double(cnt.load())/secs,err.load());
  return 0;
}
