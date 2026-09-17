// 对比大数据面两种实现：pread+write（httplib 唯一可行）vs sendfile（需自持 socket）
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/sendfile.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
using clk=std::chrono::steady_clock;
static double secs(clk::time_point a,clk::time_point b){return std::chrono::duration<double>(b-a).count();}
static double cpu(){rusage r{};getrusage(RUSAGE_SELF,&r);return r.ru_utime.tv_sec+r.ru_utime.tv_usec/1e6+r.ru_stime.tv_sec+r.ru_stime.tv_usec/1e6;}
static const long long FS=200LL*1024*1024*1024;

// 极简 HTTP/1.1：解析 GET + Range，按 mode 发送
static void serve_one(int cfd,int ffd,bool use_sendfile,long long* bytes){
  char req[2048]; ssize_t n=::recv(cfd,req,sizeof(req)-1,0); if(n<=0)return; req[n]=0;
  long long off=0,len=FS;
  const char* r=strstr(req,"Range: bytes=");
  if(r){ long long a=0,b=0; if(sscanf(r+13,"%lld-%lld",&a,&b)==2){off=a;len=b-a+1;} }
  char hdr[512];
  int hn=snprintf(hdr,sizeof(hdr),
    "HTTP/1.1 206 Partial Content\r\nContent-Length: %lld\r\n"
    "Content-Range: bytes %lld-%lld/%lld\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n",
    len,off,off+len-1,FS);
  ::send(cfd,hdr,(size_t)hn,MSG_NOSIGNAL);
  if(use_sendfile){
    off_t o=off; long long rem=len;
    while(rem>0){ size_t chunk=(size_t)std::min<long long>(rem,1LL<<30);   // sendfile 单次上限约 2GiB
      ssize_t s=::sendfile(cfd,ffd,&o,chunk); if(s<=0)break; rem-=s; }
  } else {
    char* buf=(char*)malloc(1<<20); long long rem=len; off_t o=off;
    while(rem>0){ size_t w=(size_t)std::min<long long>(rem,1<<20);
      ssize_t g=::pread(ffd,buf,w,o); if(g<=0)break;
      ssize_t s=::send(cfd,buf,(size_t)g,MSG_NOSIGNAL); if(s<=0)break; o+=g; rem-=g; }
    free(buf);
  }
  *bytes+=len; ::close(cfd);
}
static void Run(bool sf,const char* path,int nreq,long long seg,const char* tag){
  int lfd=::socket(AF_INET,SOCK_STREAM,0); int on=1;
  ::setsockopt(lfd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on));
  sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=0;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  ::bind(lfd,(sockaddr*)&a,sizeof(a)); ::listen(lfd,64);
  socklen_t sl=sizeof(a); ::getsockname(lfd,(sockaddr*)&a,&sl); int port=ntohs(a.sin_port);
  int ffd=::open(path,O_RDONLY);
  std::thread srv([&]{ long long b=0; for(int i=0;i<nreq;i++){ int c=::accept(lfd,nullptr,nullptr); if(c<0)break; serve_one(c,ffd,sf,&b);} });
  // 客户端：顺序发 nreq 个请求
  double c0=cpu(); auto t0=clk::now();
  long long got=0;
  for(int i=0;i<nreq;i++){
    int fd=::socket(AF_INET,SOCK_STREAM,0);
    int nd=1; ::setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&nd,sizeof(nd));
    sockaddr_in s{};s.sin_family=AF_INET;s.sin_port=htons(port);s.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    ::connect(fd,(sockaddr*)&s,sizeof(s));
    long long off=((long long)i*131071LL)%(FS-2*1024*1024);
    char rq[256]; int rn=snprintf(rq,sizeof(rq),"GET /big HTTP/1.1\r\nHost: h\r\nRange: bytes=%lld-%lld\r\n\r\n",off,off+seg-1);
    ::send(fd,rq,(size_t)rn,0);
    char buf[65536]; ssize_t n; long long body=0; bool inhdr=true; std::string tail;
    while((n=::recv(fd,buf,sizeof(buf),0))>0){
      if(inhdr){ tail.append(buf,(size_t)n); size_t p=tail.find("\r\n\r\n");
        if(p!=std::string::npos){ body+=(long long)(tail.size()-p-4); inhdr=false; } }
      else body+=n;
    }
    ::close(fd); got+=body;
  }
  auto t1=clk::now(); double c1=cpu();
  srv.join(); ::close(lfd); ::close(ffd);
  double s=secs(t0,t1);
  printf("  %-28s %6.2f GiB/s  每请求 %.3f ms  CPU=%.0f%%  每 GiB CPU=%.2f s\n",
    tag,got/1073741824.0/s,1000.0*s/nreq,100*(c1-c0)/s,(c1-c0)/(got/1073741824.0));
}
int main(){
  setvbuf(stdout,nullptr,_IONBF,0);
  const char* p="/tmp/fss_bench/big.img";
  printf("段=1 MiB，顺序单连接（客户端/服务端同机，CPU 数据是两者合计）\n");
  Run(false,p,300,1024*1024,"pread + write");
  Run(true ,p,300,1024*1024,"sendfile");
  printf("\n段=8 MiB\n");
  Run(false,p,60,8*1024*1024,"pread + write");
  Run(true ,p,60,8*1024*1024,"sendfile");
  return 0;
}
