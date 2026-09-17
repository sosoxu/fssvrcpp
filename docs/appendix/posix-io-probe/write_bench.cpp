// 写入路径三条路线对比（小文件：pwrite 4KiB + fsync）
//  A) 线程模型：N 线程，每文件 write+fsync
//  B) io_uring：单/少线程，队列深度 N，write+fsync 链式提交
//  C) 组提交：N 线程写文件不 fsync，每批一次 fsync（摊销）
#define _GNU_SOURCE
#include <liburing.h>
#include <fcntl.h>
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
static double secs(clk::time_point a,clk::time_point b){return std::chrono::duration<double>(b-a).count();}
static const char* DIR="/tmp/ioprobe/wbench";

static void A_threads(int T,int per){
  std::atomic<long long> done{0};
  std::vector<std::thread> ts; auto t0=clk::now();
  for(int t=0;t<T;t++) ts.emplace_back([&,t]{
    std::string p=std::string(DIR)+"/a"+std::to_string(t);
    for(int i=0;i<per;i++){
      std::string f=p+"_"+std::to_string(i);
      int fd=::open(f.c_str(),O_CREAT|O_WRONLY|O_TRUNC,0644);
      char b[4096]; memset(b,i&0xff,sizeof(b));
      if(::write(fd,b,sizeof(b))==(ssize_t)sizeof(b)){ ::fsync(fd); done++; }
      ::close(fd);
    }});
  for(auto&x:ts)x.join();
  double s=secs(t0,clk::now());
  printf("  A) 线程 %-4d 每文件 fsync        → %8.0f 文件/s  (%.2f ms/文件)\n",T,done.load()/s,1000.0*s/done.load());
}
static void B_uring(int D,int total){
  struct io_uring ring;
  if(io_uring_queue_init((unsigned)D+16,&ring,0)<0){printf("  B) io_uring init 失败\n");return;}
  char b[4096]; memset(b,9,sizeof(b));
  auto t0=clk::now(); long long done=0; int issued=0; int idx=0;
  while(idx<total){
    struct io_uring_sqe* sqe=io_uring_get_sqe(&ring);
    std::string f=std::string(DIR)+"/b"+std::to_string(idx);
    int fd=::open(f.c_str(),O_CREAT|O_WRONLY|O_TRUNC|O_DIRECT,0644);
    if(fd<0){ // 回退到普通写
      fd=::open(f.c_str(),O_CREAT|O_WRONLY|O_TRUNC,0644); }
    io_uring_prep_write(sqe,fd,b,4096,0);
    io_uring_sqe_set_data(sqe,(void*)(intptr_t)fd);
    io_uring_sqe_set_flags(sqe,IOSQE_IO_LINK);
    struct io_uring_sqe* fq=io_uring_get_sqe(&ring);
    io_uring_prep_fsync(fq,fd,IORING_FSYNC_DATASYNC);
    io_uring_sqe_set_data(fq,(void*)(intptr_t)fd);
    io_uring_submit(&ring);
    issued++; idx++;
    // 收掉一批完成事件
    while(issued>0){
      struct io_uring_cqe* cqe;
      if(io_uring_peek_cqe(&ring,&cqe)!=0) break;
      int fd2=(int)(intptr_t)io_uring_cqe_get_data(cqe);
      if(fd2>0){ ::close(fd2); }
      io_uring_cqe_seen(&ring,cqe); issued--; done++;
    }
  }
  while(issued>0){ struct io_uring_cqe* cqe; if(io_uring_wait_cqe(&ring,&cqe)<0)break;
    int fd2=(int)(intptr_t)io_uring_cqe_get_data(cqe); if(fd2>0)::close(fd2);
    io_uring_cqe_seen(&ring,cqe); issued--; done++; }
  double s=secs(t0,clk::now());
  io_uring_queue_exit(&ring);
  printf("  B) io_uring 深度 %-4d write+fsync → %8.0f 文件/s  (%.2f ms/文件)\n",D,done/s,1000.0*s/done);
}
static void C_group(int T,int per){
  // 每线程写 per 个文件后只 fsync 一次（摊销），并 fsync 目录以持久化文件名
  std::atomic<long long> done{0};
  std::vector<std::thread> ts; auto t0=clk::now();
  for(int t=0;t<T;t++) ts.emplace_back([&,t]{
    std::string p=std::string(DIR)+"/c"+std::to_string(t);
    char b[4096]; memset(b,3,sizeof(b));
    for(int i=0;i<per;i++){
      std::string f=p+"_"+std::to_string(i);
      int fd=::open(f.c_str(),O_CREAT|O_WRONLY|O_TRUNC,0644);
      if(::write(fd,b,sizeof(b))==(ssize_t)sizeof(b)) done++;
      ::close(fd);
    }
    int dfd=::open(DIR,O_RDONLY|O_DIRECTORY); ::fsync(dfd); ::close(dfd);
  });
  for(auto&x:ts)x.join();
  double s=secs(t0,clk::now());
  printf("  C) 线程 %-4d 组提交(每线程1次fsync) → %8.0f 文件/s  (%.3f ms/文件)\n",
         T,done.load()/s,1000.0*s/done.load());
}
int main(){
  setvbuf(stdout,nullptr,_IONBF,0);
  ::system("rm -rf /tmp/ioprobe/wbench && mkdir -p /tmp/ioprobe/wbench");
  printf("每文件 4 KiB 写；总文件数 8000\n\n");
  A_threads(1,2000); A_threads(16,500); A_threads(64,125);
  printf("\n");
  B_uring(16,2000); B_uring(64,2000); B_uring(256,4000);
  printf("\n");
  C_group(16,500); C_group(64,125);
  ::system("rm -rf /tmp/ioprobe/wbench");
  return 0;
}
