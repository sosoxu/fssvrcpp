// BlockingIoEngine 实现。并发安全性的依据见头文件。
#include "infra/io/blocking_io_engine.h"

#include <sys/stat.h>
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

RealFileSync& DefaultFileSync() {
  static RealFileSync instance;
  return instance;
}

}  // namespace

IFileSync& BlockingIoEngine::Sync_() {
  return file_sync_ != nullptr ? *file_sync_ : DefaultFileSync();
}

domain::IoEngineCapabilities BlockingIoEngine::capabilities() const {
  domain::IoEngineCapabilities caps;
  caps.async = false;
  caps.direct_io = false;   // 未实现 O_DIRECT（需要对齐缓冲；P9 视需要再加）
  caps.alignment = 0;
  caps.engine_name = "blocking";
  return caps;
}

fss::Result<std::size_t> BlockingIoEngine::ReadAt(int fd, std::uint64_t offset, char* buffer,
                                                  std::size_t length) {
  std::size_t done = 0;
  while (done < length) {
    const ssize_t n = ::pread(fd, buffer + done, length - done,
                              static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return ErrFromErrno("pread 失败");
    }
    if (n == 0) break;  // EOF（短读是允许的，返回值告诉调用方读了多少）
    done += static_cast<std::size_t>(n);
  }
  return done;
}

fss::Result<std::size_t> BlockingIoEngine::WriteAt(int fd, std::uint64_t offset,
                                                   const char* buffer, std::size_t length) {
  std::size_t done = 0;
  while (done < length) {
    const ssize_t n = ::pwrite(fd, buffer + done, length - done,
                               static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return ErrFromErrno("pwrite 失败");
    }
    done += static_cast<std::size_t>(n);
  }
  return done;
}

fss::Result<void> BlockingIoEngine::Sync(int fd) { return Sync_().DataSync(fd); }

fss::Result<void> BlockingIoEngine::SyncFilesystem(int fd) {
  //  ADR-008 的第二阶段：`syncfs` 是**文件系统级**操作（多实例共盘会互相影响，
  //  因此部署建议按 partition 分盘，见 AGENTS.md §4.4）。
  if (::syncfs(fd) != 0) {
    if (errno == EINVAL || errno == ENOSYS || errno == EPERM) {
      return Err(fss::ErrorKind::kUnimplemented,
                 "syncfs 不可用：" + std::string(std::strerror(errno)));
    }
    return ErrFromErrno("syncfs 失败");
  }
  return Ok();
}

fss::Result<std::uint64_t> BlockingIoEngine::FileSize(int fd) {
  struct stat info {};
  if (::fstat(fd, &info) != 0) return ErrFromErrno("fstat 失败");
  return static_cast<std::uint64_t>(info.st_size);
}

}  // namespace fss::infra
