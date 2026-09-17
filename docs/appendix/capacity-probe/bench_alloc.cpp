// 定位并发悬崖：对比 [每次请求 malloc 1MiB] vs [线程本地缓冲复用] vs [小段(64KiB, 低于 mmap 阈值)]
#include <httplib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
using clk=std::chrono::steady_clock;
static double secs(clk::time_point a,clk::time_point b){return std::chrono::duration<double>(b-a).count();}
static const long long FS=200LL*1024*1024*1024;

// 模式 0：每请求新分配 buffer；模式 1：线程本地复用（内存池语义）
static httplib::Server* MakeServer(const char* path,int mode){
  auto* svr=new httplib::Server();
  svr->new_task_queue=[](){return new httplib::ThreadPool(256);};
  svr->set_tcp_nodelay(true);
  svr->set_keep_alive_max_count(1000000);
  svr->Get("/big",[path,mode](const httplib::Request&,httplib::Response& res){
    int fd=::open(path,O_RDONLY); struct stat st{}; fstat(fd,&st);
    res.set_header("Accept-Ranges","bytes");
    res.set_content_provider(size_t(st.st_size),"application/octet-stream",
      [fd,mode](size_t off,size_t len,httplib::DataSink& sink){
        static thread_local std::vector<char> tls;          // 复用的缓冲
        const size_t CAP=4*1024*1024;
        std::vector<char> local;
        char* buf; size_t cap;
        if(mode==1){ if(tls.size()<CAP) tls.resize(CAP); buf=tls.data(); cap=tls.size(); }
        else { local.resize(1024*1024); buf=local.data(); cap=local.size(); }
        long long o=(long long)off,rem=(long long)len;
        while(rem>0){ size_t w=(size_t)std::min<long long>(rem,(long long)std::min<size_t>(cap,1024*1024));
          ssize_t n=pread(fd,buf,w,o); if(n<=0){::close(fd);return false;}
          if(!sink.write(buf,(size_t)n)){::close(fd);return false;} o+=n; rem-=n; }
        ::close(fd); return true; });
  });
  return svr;
}
static void Run(const char* tag,httplib::Server* svr,int T,int PER,size_t SEG){
  int port=svr->bind_to_any_port("127.0.0.1");
  std::thread th([&]{svr->listen_after_bind();});
  std::atomic<long long> bytes{0}; std::atomic<int> errs{0};
  std::vector<std::thread> ts; auto t0=clk::now();
  for(int i=0;i<T;i++) ts.emplace_back([&,i]{
    httplib::Client c("127.0.0.1",port); c.set_keep_alive(true); c.set_tcp_nodelay(true);
    c.set_read_timeout(60,0);
    for(int j=0;j<PER;j++){
      long long off=((long long)(i*PER+j)*131071LL)%(FS-2*1024*1024);
      httplib::Headers h{{"Range","bytes="+std::to_string(off)+"-"+std::to_string(off+SEG-1)},
                         {"Accept-Encoding","identity"}};
      auto r=c.Get("/big",h);
      if(r&&r->status==206) bytes+=(long long)r->body.size(); else errs++;
    }});
  for(auto&t:ts)t.join(); double s=secs(t0,clk::now());
  printf("  %-42s T=%-3d %zuKiB -> %6.2f GiB/s  (%.2f ms/req) err=%d\n",
    tag,T,SEG/1024,bytes.load()/1073741824.0/s,1000.0*s/((double)T*PER),errs.load());
  svr->stop(); th.join();
}
int main(){
  setvbuf(stdout,nullptr,_IONBF,0);
  const char* path="/tmp/fss_bench/big.img";
  printf("对比：每次请求分配 1MiB vs 线程本地缓冲复用\n\n");
  for(int T:{4,16,64}){
    httplib::Server* s0=MakeServer(path,0); Run("每次请求 vector(1MiB)",s0,T,30,1024*1024); delete s0;
    httplib::Server* s1=MakeServer(path,1); Run("thread_local 缓冲复用(4MiB)",s1,T,30,1024*1024); delete s1;
  }
  printf("\n小段（64 KiB，低于 malloc mmap 阈值 128KiB）\n\n");
  for(int T:{16,64}){
    httplib::Server* s0=MakeServer(path,0); Run("每次请求 vector(64KiB)",s0,T,200,64*1024); delete s0;
    httplib::Server* s1=MakeServer(path,1); Run("thread_local 缓冲复用",s1,T,200,64*1024); delete s1;
  }
  return 0;
}
