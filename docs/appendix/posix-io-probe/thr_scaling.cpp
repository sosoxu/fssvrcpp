// 线程模型：N 线程并发随机 O_DIRECT 4 KiB 读（非缓存），测 IOPS 与扩展性
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>
using clk=std::chrono::steady_clock;
int main(int argc,char**argv){
  const char* path="/tmp/ioprobe/data.bin";
  const size_t FS=1024ull*1024*1024, BLK=4096;
  printf("  线程数   IOPS        每线程 IOPS   聚合带宽\n");
  for(int T : {1,2,4,8,16,32,64,128}){
    std::atomic<long long> ops{0}; std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for(int t=0;t<T;t++) ts.emplace_back([&,t]{
      int fd=::open(path,O_RDONLY|O_DIRECT);
      void* buf=nullptr; if(::posix_memalign(&buf,4096,BLK)!=0)return;
      std::mt19937_64 rng(1234+t);
      long long local=0;
      while(!stop.load(std::memory_order_relaxed)){
        long long off=(long long)(rng()%(FS/BLK))*BLK;
        ssize_t n=::pread(fd,buf,BLK,off);
        if(n>0) local++;
      }
      ops+=local; ::close(fd); free(buf);
    });
    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop=true; for(auto&x:ts)x.join();
    double iops=ops.load()/2.0;
    printf("  %-6d  %9.0f    %8.0f      %6.1f MiB/s\n",T,iops,iops/T,iops*BLK/1048576.0);
  }
  return 0;
}
