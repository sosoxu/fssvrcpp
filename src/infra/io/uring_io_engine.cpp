// UringIoEngine 骨架实现。启用条件见头文件（ADR-010 U1–U4）。
#include "infra/io/uring_io_engine.h"

#include <utility>

namespace fss::infra {

fss::Error UringIoEngine::NotEnabled() const {
  return Err(fss::ErrorKind::kUnimplemented,
             "io_uring 引擎尚未启用（ADR-010 U1–U4 未满足）；探测结果：" + probe_.ToString());
}

domain::IoEngineCapabilities UringIoEngine::capabilities() const {
  domain::IoEngineCapabilities caps;
  //  如实：即使探测可用，本骨架也没有启用真异步路径
  caps.async = false;
  caps.direct_io = false;
  caps.alignment = 0;
  caps.engine_name = "uring";
  return caps;
}

fss::Result<std::size_t> UringIoEngine::ReadAt(int fd, std::uint64_t offset, char* buffer,
                                               std::size_t length) {
  (void)fd;
  (void)offset;
  (void)buffer;
  (void)length;
  return NotEnabled();
}

fss::Result<std::size_t> UringIoEngine::WriteAt(int fd, std::uint64_t offset, const char* buffer,
                                                std::size_t length) {
  (void)fd;
  (void)offset;
  (void)buffer;
  (void)length;
  return NotEnabled();
}

fss::Result<void> UringIoEngine::Sync(int fd) {
  (void)fd;
  return NotEnabled();
}

fss::Result<void> UringIoEngine::SyncFilesystem(int fd) {
  (void)fd;
  return NotEnabled();
}

fss::Result<std::uint64_t> UringIoEngine::FileSize(int fd) {
  (void)fd;
  return NotEnabled();
}

}  // namespace fss::infra
