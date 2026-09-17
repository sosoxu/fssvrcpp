// =============================================================================
//  BlockingIoEngine（L2）—— 默认的 IIoEngine 实现（ADR-010）
// =============================================================================
//  ★ 为什么是 `pread`/`pwrite`，而不是 `lseek` + `read`
//    `lseek+read` 依赖**文件描述符上的共享偏移**：两个线程用同一个 fd 并发读不同区间
//    会互相踩偏移，结果错乱且难以复现。`pread`/`pwrite` 把偏移作为参数传入，
//    不共享任何可变状态 → 天然并发安全（C3.10 的实测点）。
//
//  ★ 为什么默认是 blocking 而不是 io_uring
//    ADR-010 实测：容器默认 seccomp 下 io_uring 返回 `EPERM`。因此默认路径必须
//    无依赖可用；`UringIoEngine` 只在探测通过时启用（P3 后续切片，可先不启用）。
//
//  ★ 64 位偏移
//    `IIoEngine` 的签名就是 `uint64_t`（> 2 GiB 文件是硬要求，C2.11）。
//    `pread`/`pwrite` 用 `off_t`（64 位），本实现显式转换。
#pragma once

#include "common/result/result.h"
#include "domain/ports/ports.h"
#include "infra/io/file_sync.h"

#include <cstddef>
#include <cstdint>

namespace fss::infra {

class BlockingIoEngine final : public domain::IIoEngine {
 public:
  //  `file_sync` 为 null 时使用真实的 `fdatasync`/`fsync`
  explicit BlockingIoEngine(IFileSync* file_sync = nullptr) : file_sync_(file_sync) {}

  domain::IoEngineCapabilities capabilities() const override;
  fss::Result<std::size_t> ReadAt(int fd, std::uint64_t offset, char* buffer,
                                  std::size_t length) override;
  fss::Result<std::size_t> WriteAt(int fd, std::uint64_t offset, const char* buffer,
                                   std::size_t length) override;
  fss::Result<void> Sync(int fd) override;
  fss::Result<void> SyncFilesystem(int fd) override;
  fss::Result<std::uint64_t> FileSize(int fd) override;

 private:
  IFileSync& Sync_();

  IFileSync* file_sync_ = nullptr;  // null → 内部 RealFileSync
};

}  // namespace fss::infra
