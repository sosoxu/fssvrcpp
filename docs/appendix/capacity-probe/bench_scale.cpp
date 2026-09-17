// 分段读的并发扩展性：T 线程各读 PER 次 SEG 大小的段，看聚合吞吐随 T 的变化
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

static double Case(const char* path,int port,int pool_unused,int T,int PER,size_t SEG,bool raw_socket);

int main(){
  setvbuf(stdout,nullptr,_IONBF,0);
  const char* path="/tmp/fss_bench/big.img";
  httplib::Server svr;
  svr.new_task_queue=[](){return new httplib::ThreadPool(256);};
  svr.set_tcp_nodelay(true);
  svr.set_keep_alive_max_count(1000000);
  svr.Get("/big",[&](const httplib::Request&,httplib::Response& res){
    int fd=::open(path,O_RDONLY); struct stat st{}; fstat(fd,&st);
    res.set_header("Accept-Ranges","bytes");
    res.set_content_provider(size_t(st.st_size),"application/octet-stream",
      [fd](size_t off,size_t len,httplib::DataSink& sink){
        std::vector<char> buf(1024*1024);
        long long o=(long long)off,rem=(long long)len;
        while(rem>0){ size_t w=(size_t)std::min<long long>(rem,1024*1024);
          ssize_t n=pread(fd,buf.data(),w,o); if(n<=0){::close(fd);return false;}
          if(!sink.write(buf.data(),(size_t)n)){::close(fd);return false;} o+=n; rem-=n; }
        ::close(fd); return true; });
  });
  int port=svr.bind_to_any_port("127.0.0.1");
  std::thread th([&]{svr.listen_after_bind();});

  const int PER=30; const size_t SEG=1024*1024;
  printf("段大小=%zu KiB, 每线程 %d 次；pool=256, TCP_NODELAY=ON\n\n", SEG/1024, PER);
  printf("   并发T    聚合吞吐     每请求等效延迟   说明\n");
  for(int T : {1,4,16,32,64}){
    std::atomic<long long> bytes{0}; std::atomic<int> errs{0};
    std::vector<std::thread> ts;
    auto t0=clk::now();
    for(int i=0;i<T;i++) ts.emplace_back([&,i]{
      httplib::Client c("127.0.0.1",port); c.set_keep_alive(true); c.set_tcp_nodelay(true);
      c.set_read_timeout(60,0);
      for(int j=0;j<PER;j++){
        long long off=((long long)(i*PER+j)*131071LL) % (FS-2*1024*1024);
        httplib::Headers h{{"Range","bytes="+std::to_string(off)+"-"+std::to_string(off+SEG-1)},
                           {"Accept-Encoding","identity"}};
        auto r=c.Get("/big",h);
        if(r&&r->status==206) bytes+= (long long)r->body.size(); else errs++;
      }});
    for(auto&t:ts)t.join();
    double s=secs(t0,clk::now());
    long long reqs=(long long)T*PER;
    printf("   %-6d  %7.2f GiB/s   %7.2f ms        err=%d\n",
      T, bytes.load()/1073741824.0/s, 1000.0*s/ (reqs/double(T)), errs.load());
  }
  svr.stop(); th.join(); return 0;
}
