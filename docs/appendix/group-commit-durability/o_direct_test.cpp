// 用 O_DIRECT 读回（绕过页缓存）区分"已落盘"与"仅在内存"
// 预期：ext4 对 O_DIRECT 读与缓冲写有连贯性处理，可能读回正确数据 → 该实验可能无法区分（如实报告）
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static void fill(std::vector<char>& b){for(size_t i=0;i<b.size();i++)b[i]=(char)((i*2654435761u)>>13);}
int main(int argc,char**argv){
  const char* path=argv[1]; const char* mode=argv[2];
  const size_t N=4096;
  std::vector<char> data(N); fill(data);
  int wfd=::open(path,O_CREAT|O_WRONLY|O_TRUNC,0644);
  if(wfd<0){printf("  open 失败 %s\n",strerror(errno));return 2;}
  if(::write(wfd,data.data(),N)!=(ssize_t)N){printf("  write 失败\n");return 2;}
  if(strcmp(mode,"sync")==0) ::fdatasync(wfd);
  ::close(wfd);

  int fd=::open(path,O_RDONLY|O_DIRECT);
  if(fd<0){ printf("  %-8s O_DIRECT 打不开: %s\n",mode,strerror(errno)); return 2; }
  void* buf=nullptr;
  if(::posix_memalign(&buf,4096,N)!=0){ ::close(fd); return 2; }
  memset(buf,0xAB,N);
  ssize_t n=::pread(fd,buf,N,0);
  int same=0; if(n==(ssize_t)N && memcmp(buf,data.data(),N)==0) same=1;
  int allzero=1; for(size_t i=0;i<N;i++){ if(((unsigned char*)buf)[i]){ allzero=0; break; } }
  printf("  %-8s O_DIRECT 读回: n=%zd  内容%s%s\n", mode, n,
         same ? "一致" : "不一致", allzero ? " (全 0)" : "");
  ::close(fd); free(buf); return same?0:1;
}
