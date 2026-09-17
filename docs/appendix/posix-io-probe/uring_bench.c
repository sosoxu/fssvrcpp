#define _GNU_SOURCE
// io_uring：单线程 + 可配置队列深度，随机 O_DIRECT 4 KiB 读
#include <liburing.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static double now(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec+ts.tv_nsec/1e9;}
int main(int argc,char**argv){
  const char* path="/tmp/ioprobe/data.bin";
  const size_t FS=1024ull*1024*1024, BLK=4096;
  printf("  队列深度  IOPS        提交线程   聚合带宽\n");
  int depths[]={4,16,64,256,1024};
  for(unsigned di=0;di<sizeof(depths)/sizeof(depths[0]);di++){
    int D=depths[di];
    struct io_uring ring;
    if(io_uring_queue_init((unsigned)D+8,&ring,0)<0){printf("  init 失败 D=%d\n",D);continue;}
    int fd=open(path,O_RDONLY|O_DIRECT);
    if(fd<0){printf("  open 失败\n");continue;}
    void** bufs=calloc(D,sizeof(void*));
    for(int i=0;i<D;i++) if(posix_memalign(&bufs[i],4096,BLK)!=0) return 1;
    unsigned long long seed=88172645463325252ULL;
    double t0=now(); long long done=0; int inflight=0;
    for(int i=0;i<D;i++){
      struct io_uring_sqe* sqe=io_uring_get_sqe(&ring);
      seed^=seed<<13; seed^=seed>>7; seed^=seed<<17;
      long long off=(long long)(seed%(FS/BLK))*BLK;
      io_uring_prep_read(sqe,fd,bufs[i],BLK,off);
      io_uring_sqe_set_data(sqe,bufs[i]);
      inflight++;
    }
    io_uring_submit(&ring);
    double deadline=t0+2.0;
    while(now()<deadline){
      struct io_uring_cqe* cqe;
      if(io_uring_wait_cqe(&ring,&cqe)<0) break;
      if(cqe->res>0) done++;
      void* b=io_uring_cqe_get_data(cqe);
      io_uring_cqe_seen(&ring,cqe);
      struct io_uring_sqe* sqe=io_uring_get_sqe(&ring);
      seed^=seed<<13; seed^=seed>>7; seed^=seed<<17;
      long long off=(long long)(seed%(FS/BLK))*BLK;
      io_uring_prep_read(sqe,fd,b,BLK,off);
      io_uring_sqe_set_data(sqe,b);
      io_uring_submit(&ring);
    }
    double el=now()-t0;
    printf("  %-9d  %9.0f   %-9s  %6.1f MiB/s\n",D,done/el,"1 (单线程)",done*BLK/el/1048576.0);
    io_uring_queue_exit(&ring);
    for(int i=0;i<D;i++) free(bufs[i]); free(bufs); close(fd);
  }
  return 0;
}
