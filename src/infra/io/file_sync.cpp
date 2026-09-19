// IFileSync 实现（真实系统调用）。
#include "infra/io/file_sync.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace fss::infra {

namespace {

fss::Error ErrFromErrno(const std::string& what) {
  const int e = errno;
  fss::ErrorKind kind = fss::ErrorKind::kInternal;
  if (e == ENOENT) {
    kind = fss::ErrorKind::kNotFound;
  } else if (e == EACCES || e == EPERM) {
    kind = fss::ErrorKind::kPermissionDenied;
  } else if (e == ENOSPC || e == EDQUOT) {
    kind = fss::ErrorKind::kUnavailable;
  } else if (e == EINVAL || e == ENOSYS) {
    kind = fss::ErrorKind::kUnimplemented;
  }
  return fss::Err(kind, what + "：" + std::strerror(e));
}

}  // namespace

fss::Result<void> RealFileSync::DataSync(int fd) {
  if (::fdatasync(fd) != 0) return ErrFromErrno("fdatasync 失败");
  return Ok();
}

fss::Result<void> RealFileSync::SyncDirectory(const std::string& directory) {
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return ErrFromErrno("打开目录失败");
  const int rc = ::fsync(fd);
  const int saved = errno;
  ::close(fd);
  if (rc != 0) {
    errno = saved;
    return ErrFromErrno("fsync 目录失败");
  }
  return Ok();
}

//  ADR-008 的 P4 阶段 B：`syncfs(2)` 是**文件系统级** flush。
//  `directory` 只用来取得该文件系统上的任意一个 fd（Linux 的 `syncfs` 按 fd 所在的
//  superblock 工作，不按目录）。真实实现必须走系统调用，不能"假装成功"：一次失败的
//  `syncfs` 意味着**全批数据未 durable** → 调用方必须放弃整批的 rename（R1）。
fss::Result<void> RealFileSync::SyncFilesystem(const std::string& directory) {
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return ErrFromErrno("打开目录失败");
  const int rc = ::syncfs(fd);
  const int saved = errno;
  ::close(fd);
  if (rc != 0) {
    errno = saved;
    return ErrFromErrno("syncfs 失败");
  }
  return Ok();
}

bool ShouldFsync(FsyncPolicy policy, std::int64_t object_bytes, std::int64_t threshold_bytes) {
  switch (policy) {
    case FsyncPolicy::kAlways:
      return true;
    case FsyncPolicy::kNever:
      return false;
    case FsyncPolicy::kBySize:
      //  阈值 <= 0 视为"所有对象都要 fsync"（比"永不 fsync"安全的方向）
      return threshold_bytes <= 0 || object_bytes >= threshold_bytes;
  }
  return true;
}

}  // namespace fss::infra
