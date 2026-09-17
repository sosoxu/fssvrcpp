// 大文件分段读（修正版）：声明完整文件大小，由 httplib 把 Range 翻译为 (offset,length)
#include <httplib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
using clk=std::chrono::steady_clock;
static double secs(clk::time_point a,clk::time_point b){return std::chrono::duration<double>(b-a).count();}
static double cpu_secs(){rusage r{};getrusage(RUSAGE_SELF,&r);
  return r.ru_utime.tv_sec+r.ru_utime.tv_usec/1e6+r.ru_stime.tv_sec+r.ru_stime.tv_usec/1e6;}

int main(int argc,char**argv){
  setvbuf(stdout,nullptr,_IONBF,0);
  const char* path=argv[1];
  const long long FILESIZE=200LL*1024*1024*1024;
  const size_t BUF=1024*1024;                       // 1 MiB 读缓冲
  const long long MARK_OFF=150LL*1024*1024*1024;
  { int fd=::open(path,O_CREAT|O_RDWR|O_TRUNC,0644);
    if(ftruncate(fd,FILESIZE)){perror("ftruncate");return 1;}
    const char* m="FSS-MARKER-AT-150GiB";
    if(pwrite(fd,m,strlen(m),MARK_OFF)!=(ssize_t)strlen(m)){perror("pwrite");return 1;}
    ::close(fd); }

  httplib::Server svr;
  svr.new_task_queue=[](){return new httplib::ThreadPool(64);};
  svr.set_tcp_nodelay(true);
  svr.set_keep_alive_max_count(1000000);
  svr.Get("/big",[&](const httplib::Request&,httplib::Response& res){
    int fd=::open(path,O_RDONLY); if(fd<0){res.status=404;return;}
    struct stat st{}; fstat(fd,&st);
    res.set_header("Accept-Ranges","bytes");
    // ★ 声明**完整**大小；httplib 会据 Range 头把 (offset,length) 传进来
    res.set_content_provider(size_t(st.st_size),"application/octet-stream",
      [fd,BUF](size_t offset,size_t length,httplib::DataSink& sink){
        std::vector<char> buf(BUF);
        long long off=(long long)offset, rem=(long long)length;
        while(rem>0){ size_t w=(size_t)std::min<long long>(rem,(long long)BUF);
          ssize_t n=pread(fd,buf.data(),w,off);
          if(n<=0){::close(fd);return false;}
          if(!sink.write(buf.data(),(size_t)n)){::close(fd);return false;}
          off+=n; rem-=n; }
        ::close(fd); return true; });
  });
  int port=svr.bind_to_any_port("127.0.0.1");
  std::thread th([&]{svr.listen_after_bind();});

  printf("文件: 200 GiB 稀疏（实际占盘 4 KiB），读缓冲 1 MiB\n\n");

  // [1] 64 位偏移
  {
    httplib::Client c("127.0.0.1",port); c.set_read_timeout(60,0); c.set_tcp_nodelay(true);
    httplib::Headers h{{"Range","bytes="+std::to_string(MARK_OFF)+"-"+std::to_string(MARK_OFF+35)},
                       {"Accept-Encoding","identity"}};
    auto r=c.Get("/big",h);
    std::string got=r?r->body:"";
    printf("[1] 150 GiB 偏移读 36B -> status=%d \"%s\" %s\n", r?r->status:-1, got.c_str(),
           (got=="FSS-MARKER-AT-150GiB")?"✅ 64 位偏移正确":"❌");
  }
  // [2] 吞吐：单流读 2 GiB（8 MiB 段）
  {
    const long long TOTAL=2LL*1024*1024*1024, SEG=8LL*1024*1024;
    httplib::Client c("127.0.0.1",port); c.set_read_timeout(120,0); c.set_tcp_nodelay(true);
    long long done=0; double c0=cpu_secs(); auto t0=clk::now();
    while(done<TOTAL){
      long long off=done, end=std::min(done+SEG,TOTAL)-1;
      httplib::Headers h{{"Range","bytes="+std::to_string(off)+"-"+std::to_string(end)},
                         {"Accept-Encoding","identity"}};
      auto r=c.Get("/big",h);
      if(!r||r->status!=206){printf("  失败 status=%d\n",r?r->status:-1);break;}
      if(r->body.size()!=size_t(SEG)){printf("  长度异常 %zu\n",r->body.size());break;}
      done+=SEG;
    }
    auto t1=clk::now(); double c1=cpu_secs(); double s=secs(t0,t1);
    if(done>0) printf("[2] 单流吞吐 %.2f GiB / %.2fs -> %.2f GiB/s, 服务端+客户端 CPU=%.0f%%, 每 GiB %.2f CPU-s\n",
      done/1073741824.0,s,done/1073741824.0/s,100*(c1-c0)/s,(c1-c0)/(done/1073741824.0));
  }
  // [3] 小段读不放大：读 1 MiB @ 93 GiB
  {
    httplib::Client c("127.0.0.1",port); c.set_read_timeout(30,0); c.set_tcp_nodelay(true);
    long long off=100LL*1000*1000*1000;
    httplib::Headers h{{"Range","bytes="+std::to_string(off)+"-"+std::to_string(off+1048575)},
                       {"Accept-Encoding","identity"}};
    double c0=cpu_secs(); auto t0=clk::now(); auto r=c.Get("/big",h);
    auto t1=clk::now(); double c1=cpu_secs();
    printf("[3] 读 1 MiB 段 -> status=%d bytes=%zu %.1f ms CPU %.1f ms %s\n",
      r?r->status:-1, r?r->body.size():0, 1000*secs(t0,t1), 1000*(c1-c0),
      (r&&r->body.size()==1048576)?"✅ 未放大":"❌");
  }
  // [4] 越界 -> 416
  {
    httplib::Client c("127.0.0.1",port); c.set_read_timeout(30,0); c.set_tcp_nodelay(true);
    httplib::Headers h{{"Range","bytes="+std::to_string(FILESIZE+1000)+"-"},{"Accept-Encoding","identity"}};
    auto r=c.Get("/big",h);
    printf("[4] 越界 Range -> status=%d %s\n", r?r->status:-1,(r&&r->status==416)?"✅":"❌");
  }
  // [5] 并发小段读：32 线程 × 每线程 20 次 1 MiB 随机段
  {
    const int T=32, PER=20; std::atomic<long long> bytes{0}; std::atomic<int> errs{0};
    std::vector<std::thread> ts; double c0=cpu_secs(); auto t0=clk::now();
    for(int i=0;i<T;i++) ts.emplace_back([&,i]{
      httplib::Client c("127.0.0.1",port); c.set_read_timeout(60,0); c.set_tcp_nodelay(true);
      for(int j=0;j<PER;j++){
        long long off=((long long)(i*PER+j)*7LL*1024*1024) % (FILESIZE-2*1024*1024);
        httplib::Headers h{{"Range","bytes="+std::to_string(off)+"-"+std::to_string(off+1048575)},
                           {"Accept-Encoding","identity"}};
        auto r=c.Get("/big",h);
        if(r&&r->status==206) bytes+=r->body.size(); else errs++;
      }});
    for(auto&t:ts)t.join(); auto t1=clk::now(); double c1=cpu_secs();
    double s=secs(t0,t1);
    printf("[5] %d 并发 × %d 次 1 MiB 段 -> %.2f GiB/s, %lld 请求, err=%d, CPU=%.0f%%\n",
      T,PER,bytes.load()/1073741824.0/s, (long long)T*PER, errs.load(), 100*(c1-c0)/s);
  }
  svr.stop(); th.join(); return 0;
}
