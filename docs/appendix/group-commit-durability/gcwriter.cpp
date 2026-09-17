// 组提交耐久性验证用参考实现
// 三种候选协议（可通过 argv[1] 选择），每文件内容 = 由序号决定的确定性数据
//   P1 严格:  write(tmp) → fdatasync(tmp) → rename → fsync(dir)   [每文件]
//   P2 组提交: write(tmp) → fdatasync(tmp) → rename → 批量 fsync(dir)
//   P3 松散:  write(tmp) → rename → 批量 syncfs()                 [无数据 fsync]
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const size_t PAYLOAD = 4096;         // 每文件 4 KiB
static std::string g_dir;

// 确定性内容：便于崩溃后校验"是否存在部分写入"
static void FillPayload(std::vector<char>& b, unsigned idx) {
  for (size_t i = 0; i < b.size(); ++i)
    b[i] = (char)((idx * 1315423911u + i * 2654435761u) >> 13);
}
static std::string PayloadSha(unsigned idx) {   // 简化：用内容本身做比对
  std::vector<char> b(PAYLOAD); FillPayload(b, idx);
  return std::string(b.data(), b.size());
}

struct Stats { long long files=0, fsync_file=0, fsync_dir=0, syncfs_calls=0, batches=0; };
static Stats g_st;

static void FsyncFd(int fd, Stats& st, bool is_dir) {
  if (::fdatasync(fd) != 0) { /* ignore */ }
  if (is_dir) st.fsync_dir++; else st.fsync_file++;
}

// 写一个文件（不含最终耐久性提交）
static bool WriteOne(unsigned idx, int protocol, std::vector<std::string>& dirs_to_sync,
                     Stats& st, bool& need_syncfs) {
  const std::string final_path = g_dir + "/f_" + std::to_string(idx);
  const std::string tmp_path   = g_dir + "/.tmp_" + std::to_string(idx);
  std::vector<char> buf(PAYLOAD); FillPayload(buf, idx);

  // 协议 9：直接写最终文件（无 tmp/rename）——用于验证"检测器能否发现残缺文件"
  const std::string& target = (protocol == 9) ? final_path : tmp_path;
  int fd = ::open(target.c_str(), O_CREAT|O_WRONLY|O_TRUNC, 0644);
  if (fd < 0) return false;
  if (protocol == 9) {
    // 分两半写，增大被 kill 打断的概率
    size_t half = buf.size()/2;
    if (::write(fd, buf.data(), half) != (ssize_t)half) { ::close(fd); return false; }
    if (::write(fd, buf.data()+half, buf.size()-half) != (ssize_t)(buf.size()-half)) { ::close(fd); return false; }
    ::close(fd); st.files++; return true;
  }
  ssize_t w = ::write(fd, buf.data(), buf.size());
  if (w != (ssize_t)buf.size()) { ::close(fd); return false; }

  if (protocol == 1 || protocol == 2) {   // 协议 0/3 不做数据 fsync
    FsyncFd(fd, st, false);              // ★ 数据先落盘，再让它可见
  }
  ::close(fd);

  if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) return false;

  if (protocol == 1) {
    int dfd = ::open(g_dir.c_str(), O_RDONLY|O_DIRECTORY);
    if (dfd >= 0) { FsyncFd(dfd, st, true); ::close(dfd); }
  } else if (protocol == 2) {
    need_syncfs = true;                  // ★ 只需标记"本批有改名"，去重后每批 1 次
  } else if (protocol == 3) {
    need_syncfs = true;                  // 延迟到批量 syncfs
  }
  (void)dirs_to_sync;
  st.files++;
  return true;
}

// 批提交：把这一批的元数据（以及协议 3 的数据）一次性落盘
static void CommitBatch(int protocol, std::vector<std::string>& dirs,
                        bool& need_syncfs, Stats& st) {
  if (protocol == 2) {
    if (need_syncfs) {                   // ★ 去重：每批仅 1 次目录 fsync
      int dfd = ::open(g_dir.c_str(), O_RDONLY|O_DIRECTORY);
      if (dfd >= 0) { ::fdatasync(dfd); st.fsync_dir++; ::close(dfd); }
    }
  } else if (protocol == 3) {
    int dfd = ::open(g_dir.c_str(), O_RDONLY|O_DIRECTORY);
    if (dfd >= 0) { ::syncfs(dfd); st.syncfs_calls++; ::close(dfd); }
    need_syncfs = false;
  }
  dirs.clear();
  st.batches++;
}

// ★ 协议 4：两阶段批提交 —— 既满足"数据先落盘再改名"，又能批量摊销
//   A) 写整批 tmp 文件      B) syncfs()  一次让全批数据落盘
//   C) 统一 rename 到最终名  D) fsync(dir) 一次让全批改名落盘    E) 对外承诺
static bool WriteTmpOnly(unsigned idx, std::vector<std::string>& tmps) {
  const std::string tmp_path = g_dir + "/.tmp_" + std::to_string(idx);
  std::vector<char> buf(PAYLOAD); FillPayload(buf, idx);
  int fd = ::open(tmp_path.c_str(), O_CREAT|O_WRONLY|O_TRUNC, 0644);
  if (fd < 0) return false;
  ssize_t w = ::write(fd, buf.data(), buf.size());
  ::close(fd);
  if (w != (ssize_t)buf.size()) return false;
  tmps.push_back(tmp_path);
  return true;
}
static bool RenameBatch(unsigned start, int n) {
  for (int k = 0; k < n; ++k) {
    unsigned idx = (unsigned)(start + k);
    const std::string tmp_path = g_dir + "/.tmp_" + std::to_string(idx);
    const std::string final_path = g_dir + "/f_" + std::to_string(idx);
    if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) return false;
  }
  return true;
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 4) { fprintf(stderr, "usage: %s <protocol 1|2|3> <dir> <total_files> [batch]\n", argv[0]); return 2; }
  const int protocol = atoi(argv[1]);
  g_dir = argv[2];
  const int total = atoi(argv[3]);
  const int batch = argc > 4 ? atoi(argv[4]) : 500;

  ::mkdir(g_dir.c_str(), 0755);
  if (protocol == 4) {
    for (int start = 0; start < total; start += batch) {
      int n = (start + batch <= total) ? batch : (total - start);
      std::vector<std::string> tmps;
      for (int k = 0; k < n; ++k)
        if (!WriteTmpOnly((unsigned)(start+k), tmps)) { fprintf(stderr,"tmp write fail\n"); return 1; }
      { int dfd = ::open(g_dir.c_str(), O_RDONLY|O_DIRECTORY);
        if (dfd >= 0) { ::syncfs(dfd); g_st.syncfs_calls++; ::close(dfd); } }   // B) 数据落盘
      if (!RenameBatch(start, n)) { fprintf(stderr,"rename fail\n"); return 1; }  // C) 改名
      { int dfd = ::open(g_dir.c_str(), O_RDONLY|O_DIRECTORY);
        if (dfd >= 0) { ::fdatasync(dfd); g_st.fsync_dir++; ::close(dfd); } }      // D) 改名落盘
      g_st.files += n; g_st.batches++;
      printf("COMMIT %d\n", start+n-1);                                          // E) 承诺
    }
    printf("STATS files=%lld fsync_file=%lld fsync_dir=%lld syncfs=%lld batches=%lld\n",
           g_st.files, g_st.fsync_file, g_st.fsync_dir, g_st.syncfs_calls, g_st.batches);
    return 0;
  }
  std::vector<std::string> dirs; bool need_syncfs=false;
  int in_batch = 0;
  for (int i = 0; i < total; ++i) {
    if (!WriteOne((unsigned)i, protocol, dirs, g_st, need_syncfs)) {
      fprintf(stderr, "write %d failed: %s\n", i, strerror(errno)); return 1;
    }
    if (++in_batch >= batch) {
      CommitBatch(protocol, dirs, need_syncfs, g_st);
      in_batch = 0;
      // 告知父进程：截至此序号的文件已承诺耐久
      printf("COMMIT %d\n", i);
    }
  }
  CommitBatch(protocol, dirs, need_syncfs, g_st);
  printf("COMMIT %d\n", total-1);
  printf("STATS files=%lld fsync_file=%lld fsync_dir=%lld syncfs=%lld batches=%lld\n",
         g_st.files, g_st.fsync_file, g_st.fsync_dir, g_st.syncfs_calls, g_st.batches);
  return 0;
}
