// PosixFileByteSource 实现。语义与所有权契约见头文件。
#include "infra/transfer/posix_file_byte_source.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

namespace fss::infra {

namespace {

fss::Error ErrFromErrno(std::string_view what) {
  return Err(fss::ErrorKind::kInternal, std::string(what) + "：" + std::strerror(errno));
}

}  // namespace

fss::Result<std::shared_ptr<PosixFileByteSource>> PosixFileByteSource::Open(
    std::string path, bool fadvise_random) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) {
      return Err(fss::ErrorKind::kNotFound, "对象不存在：" + path);
    }
    return ErrFromErrno("打开对象失败");
  }
  //  ★ 与 `PosixBlobStore::get()` 的读路径**同一条提示**（`storage.posix.fadvise_random`）。
  if (fadvise_random) {
    (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    const auto error = ErrFromErrno("fstat 失败");
    ::close(fd);
    return error;
  }
  return std::shared_ptr<PosixFileByteSource>(
      new PosixFileByteSource(fd, static_cast<std::int64_t>(info.st_size), std::move(path)));
}

PosixFileByteSource::~PosixFileByteSource() {
  if (fd_ >= 0) ::close(fd_);
}

fss::Result<std::size_t> PosixFileByteSource::Read(char* out, std::size_t capacity) {
  if (out == nullptr || capacity == 0 || offset_ >= static_cast<std::uint64_t>(size_)) {
    return std::size_t{0};
  }
  const auto remaining = static_cast<std::uint64_t>(size_) - offset_;
  const auto want = static_cast<std::size_t>(
      capacity < remaining ? capacity : static_cast<std::size_t>(remaining));
  while (true) {
    //  `pread`（不共享文件偏移）→ 多线程并发读同一 fd 互不干扰（与 `PosixBlobStore::get` 同）。
    const ssize_t n = ::pread(fd_, out, want, static_cast<off_t>(offset_));
    if (n < 0) {
      if (errno == EINTR) continue;
      return ErrFromErrno("pread 失败");
    }
    if (n == 0) return std::size_t{0};  // 读取期间被截断 → 视作 EOF
    offset_ += static_cast<std::uint64_t>(n);
    return static_cast<std::size_t>(n);
  }
}

fss::Result<void> PosixFileByteSource::Seek(std::int64_t offset) {
  if (offset < 0 || offset > size_) {
    return Err(fss::ErrorKind::kInvalidArgument, "Seek 越界");
  }
  offset_ = static_cast<std::uint64_t>(offset);
  return Ok();
}

}  // namespace fss::infra
