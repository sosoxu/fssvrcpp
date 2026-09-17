#include <httplib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
static const long long FS=200LL*1024*1024*1024;
int main(int argc,char**argv){
  int pool=atoi(argv[1]); const char* path=argv[2]; int port=atoi(argv[3]);
  std::atomic<int> inflight{0}; std::atomic<int> maxif{0};
  httplib::Server svr;
  svr.new_task_queue=[pool](){return new httplib::ThreadPool(size_t(pool));};
  svr.set_tcp_nodelay(true); svr.set_keep_alive_max_count(1000000);
  svr.Get("/big",[&](const httplib::Request&,httplib::Response& res){
    int fd=::open(path,O_RDONLY); if(fd<0){res.status=404;return;}
    struct stat st{}; fstat(fd,&st);
    res.set_header("Accept-Ranges","bytes");
    res.set_content_provider(size_t(st.st_size),"application/octet-stream",
      [fd,&inflight,&maxif](size_t off,size_t len,httplib::DataSink& sink){
        int c=++inflight; int p=maxif.load();
        while(c>p&&!maxif.compare_exchange_weak(p,c)){}
        std::vector<char> b(1024*1024);
        long long o=(long long)off,rem=(long long)len;
        while(rem>0){ size_t w=(size_t)std::min<long long>(rem,1024*1024);
          ssize_t n=pread(fd,b.data(),w,o); if(n<=0){::close(fd);--inflight;return false;}
          if(!sink.write(b.data(),(size_t)n)){::close(fd);--inflight;return false;} o+=n; rem-=n; }
        ::close(fd); --inflight; return true; });
  });
  svr.Get("/stat",[&](const httplib::Request&,httplib::Response& res){
    res.set_content("{\"max_inflight\":"+std::to_string(maxif.load())+"}", "application/json"); });
  fprintf(stderr,"READY port=%d pool=%d\n",port,pool);
  svr.listen("127.0.0.1",port);
  return 0;
}
