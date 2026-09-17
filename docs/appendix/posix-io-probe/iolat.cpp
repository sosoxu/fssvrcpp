// 测量 POSIX 存储数据面的真实 I/O 延迟分布（决定"需要多少并发")
// 覆盖：页缓存命中 / O_DIRECT 冷读 / fsync 写
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
using clk=std::chrono::steady_clock;
static double us(clk::time_point a,clk::time_point b){
  return std::chrono::duration<double,std::micro>(b-a).count();}
static void report(const char* tag,std::vector<double>& v){
  std::sort(v.begin(),v.end());
  auto p=[&](double q){return v[(size_t)(q*(v.size()-1))];};
  double sum=0; for(double x:v)sum+=x;
  printf("  %-34s 平均 %8.1f µs  p50 %7.1f  p95 %8.1f  p99 %9.1f  max %10.1f  → 单线程上限 %8.0f ops/s\n",
    tag,sum/v.size(),p(.5),p(.95),p(.99),v.back(),1e6/(sum/v.size()));
}
int main(int argc,char**argv){
  setvbuf(stdout,nullptr,_IONBF,0);
  const size_t FS=1024ull*1024*1024;   // 1 GiB 真实文件
  const size_t BLK=4096;
  const int N=20000;
  const char* path="/tmp/ioprobe/data.bin";
  { int fd=::open(path,O_CREAT|O_WRONLY|O_TRUNC,0644);
    std::vector<char> b(1u<<20); for(size_t i=0;i<b.size();i++) b[i]=(char)(i*2654435761u);
    for(size_t off=0;off<FS;off+=b.size()) if(::write(fd,b.data(),b.size())!=(ssize_t)b.size()){perror("write");break;}
    ::close(fd); }
  printf("文件: %zu MiB, 随机读 %zu B, %d 次\n\n",FS>>20,BLK,N);

  std::mt19937_64 rng(42);
  std::vector<double> lat; lat.reserve(N);
  std::vector<char> buf(BLK);

  // 1) 顺序预热 + 页缓存命中随机读
  { int fd=::open(path,O_RDONLY);
    for(size_t off=0;off<FS;off+=BLK) ::pread(fd,buf.data(),BLK,off);   // 预热
    lat.clear();
    for(int i=0;i<N;i++){ long long off=(rng()%(FS/BLK))*BLK;
      auto t0=clk::now(); ssize_t n=::pread(fd,buf.data(),BLK,off); auto t1=clk::now();
      if(n>0) lat.push_back(us(t0,t1)); }
    report("页缓存命中（4 KiB 随机读）",lat);
    // 2) 同样但每 256 次后 posix_fadvise(DONTNEED) 模拟冷读
    lat.clear();
    for(int i=0;i<N;i++){ long long off=(rng()%(FS/BLK))*BLK;
      ::posix_fadvise(fd,off,BLK,POSIX_FADV_DONTNEED);
      auto t0=clk::now(); ssize_t n=::pread(fd,buf.data(),BLK,off); auto t1=clk::now();
      if(n>0) lat.push_back(us(t0,t1)); }
    report("DONTNEED 后读（→设备）",lat);
    ::close(fd); }

  // 3) O_DIRECT 冷读（绕过页缓存）
  { int fd=::open(path,O_RDONLY|O_DIRECT);
    if(fd<0){ printf("  O_DIRECT 打开失败: %s（WSL2 常见）\n",strerror(errno)); }
    else { void* p=nullptr;
      if(::posix_memalign(&p,4096,BLK)!=0){printf("  memalign 失败\n");}
      else { lat.clear();
        for(int i=0;i<N;i++){ long long off=(rng()%(FS/BLK))*BLK;
          auto t0=clk::now(); ssize_t n=::pread(fd,p,BLK,off); auto t1=clk::now();
          if(n>0) lat.push_back(us(t0,t1)); }
        report("O_DIRECT（绕过页缓存）",lat); free(p);} 
      ::close(fd); } }

  // 4) 写 + fsync 延迟（小文件写路径的关键成本）
  { int fd=::open("/tmp/ioprobe/w.bin",O_CREAT|O_WRONLY|O_TRUNC,0644);
    lat.clear(); std::vector<char> b(BLK,7);
    for(int i=0;i<2000;i++){ auto t0=clk::now();
      ::pwrite(fd,b.data(),BLK,(long long)i*BLK); ::fsync(fd); auto t1=clk::now();
      lat.push_back(us(t0,t1)); }
    report("写 4 KiB + fsync",lat); ::close(fd); }
  // 5) 无 fsync 的写
  { int fd=::open("/tmp/ioprobe/w2.bin",O_CREAT|O_WRONLY|O_TRUNC,0644);
    lat.clear(); std::vector<char> b(BLK,7);
    for(int i=0;i<N;i++){ auto t0=clk::now();
      ::pwrite(fd,b.data(),BLK,(long long)i*BLK); auto t1=clk::now();
      lat.push_back(us(t0,t1)); }
    report("写 4 KiB（无 fsync）",lat); ::close(fd); }
  return 0;
}
