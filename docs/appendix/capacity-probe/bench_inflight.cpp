// 直接测量服务端实际并发度：in-flight 计数器 + 最大并发 + 每请求服务端耗时
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
int main(){
  setvbuf(stdout,nullptr,_IONBF,0);
  const char* path="/tmp/fss_bench/big.img";
  std::atomic<int> inflight{0}, max_inflight{0};
  std::atomic<long long> srv_ns{0}, nreq{0};
  httplib::Server svr;
  svr.new_task_queue=[](){return new httplib::ThreadPool(256);};
  svr.set_tcp_nodelay(true); svr.set_keep_alive_max_count(1000000);
  svr.Get("/big",[&](const httplib::Request&,httplib::Response& res){
    int fd=::open(path,O_RDONLY); struct stat st{}; fstat(fd,&st);
    res.set_header("Accept-Ranges","bytes");
    res.set_content_provider(size_t(st.st_size),"application/octet-stream",
      [fd,&inflight,&max_inflight,&srv_ns,&nreq](size_t off,size_t len,httplib::DataSink& sink){
        auto t0=clk::now();
        int cur=++inflight;
        int prev=max_inflight.load();
        while(cur>prev && !max_inflight.compare_exchange_weak(prev,cur)){}
        std::vector<char> buf(1024*1024);
        long long o=(long long)off,rem=(long long)len;
        while(rem>0){ size_t w=(size_t)std::min<long long>(rem,1024*1024);
          ssize_t n=pread(fd,buf.data(),w,o); if(n<=0){::close(fd);--inflight;return false;}
          if(!sink.write(buf.data(),(size_t)n)){::close(fd);--inflight;return false;} o+=n; rem-=n; }
        ::close(fd); --inflight;
        srv_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now()-t0).count();
        ++nreq; return true; });
  });
  int port=svr.bind_to_any_port("127.0.0.1");
  std::thread th([&]{svr.listen_after_bind();});
  const int PER=30; const size_t SEG=1024*1024;
  printf("每线程 %d 次 × 1 MiB 段；服务端 pool=256\n\n",PER);
  printf("  并发T   max_in-flight   服务端平均处理    客户端总耗时   聚合吞吐\n");
  for(int T:{1,2,4,8,16,32,64}){
    inflight=0; max_inflight=0; srv_ns=0; nreq=0;
    std::atomic<long long> bytes{0};
    std::vector<std::thread> ts; auto t0=clk::now();
    for(int i=0;i<T;i++) ts.emplace_back([&,i]{
      httplib::Client c("127.0.0.1",port); c.set_keep_alive(true); c.set_tcp_nodelay(true);
      c.set_read_timeout(60,0);
      for(int j=0;j<PER;j++){
        long long off=((long long)(i*PER+j)*131071LL)%(FS-2*1024*1024);
        httplib::Headers h{{"Range","bytes="+std::to_string(off)+"-"+std::to_string(off+SEG-1)},
                           {"Accept-Encoding","identity"}};
        auto r=c.Get("/big",h);
        if(r&&r->status==206) bytes+=(long long)r->body.size();
      }});
    for(auto&t:ts)t.join(); double s=secs(t0,clk::now());
    printf("  %-6d  %-13d  %8.2f ms      %7.3f s   %6.2f GiB/s\n",
      T, max_inflight.load(), double(srv_ns.load())/1e6/std::max(1LL,nreq.load()), s,
      bytes.load()/1073741824.0/s);
  }
  svr.stop(); th.join(); return 0;
}
