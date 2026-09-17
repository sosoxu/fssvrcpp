#include <liburing.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
int main(void){
  struct io_uring ring;
  int r = io_uring_queue_init(8, &ring, 0);
  if (r < 0) { printf("  io_uring_queue_init 失败: %s (errno=%d)\n", strerror(-r), -r); return 1; }
  printf("  io_uring_queue_init OK (entries=8)\n");
  int fd = open("/tmp/ioprobe/data.bin", O_RDONLY);
  if (fd < 0) { printf("  open 失败\n"); return 1; }
  void *buf; if (posix_memalign(&buf, 4096, 4096)) return 1;
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  io_uring_prep_read(sqe, fd, buf, 4096, 8192);
  int sub = io_uring_submit(&ring);
  printf("  io_uring_submit 返回 %d\n", sub);
  struct io_uring_cqe *cqe;
  r = io_uring_wait_cqe(&ring, &cqe);
  if (r < 0) { printf("  wait_cqe 失败: %s\n", strerror(-r)); return 1; }
  printf("  ★ 异步读完成: res=%d 字节, 首字节=%d\n", cqe->res, ((unsigned char*)buf)[0]);
  io_uring_cqe_seen(&ring, cqe);
  io_uring_queue_exit(&ring);
  return 0;
}
