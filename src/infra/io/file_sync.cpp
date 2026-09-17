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
