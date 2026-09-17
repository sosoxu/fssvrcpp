// =============================================================================
//  UringIoEngine（L2）—— io_uring 引擎的**探测骨架**（ADR-010）
// =============================================================================
//  ADR-010 的结论：默认走 `BlockingIoEngine`；io_uring 是**可选加速**，且启用前必须
//  满足 U1–U4（真异步收益、seccomp 可用性、复杂度可控、可回退）。默认容器 seccomp
//  下 `io_uring_setup` 会 `EPERM`（阶段 0/1 已实测），所以本类当前**不启用**。
//
//  它的价值在于把"能力探测 + 拒绝语义"结构化地接进 `IIoEngine` 家族：
//    · `capabilities()` 如实反映探测结果（`engine_name = "uring"`，`async` **仍为 false**）；
//    · 所有 I/O 原语返回 `kUnimplemented`，并附上探测摘要 —— 这样"误配 uring"
//      不会静默退化成阻塞路径（ADR-010 R-29），而是显式失败。
//    · "拒绝启动 vs 回退"的**决策**是 L1 的纯函数 `sys::DecideIoEngine`（P1 已交付），
//      这里只提供引擎侧的行为。
//
//  ⚠️ 不链接 liburing：探测只用 `syscall(__NR_io_uring_setup)`（`common/sys`）。
#pragma once

#include "common/result/result.h"
#include "common/sys/capability.h"
#include "domain/ports/ports.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace fss::infra {

class UringIoEngine final : public domain::IIoEngine {
 public:
  explicit UringIoEngine(sys::IoEngineProbe probe) : probe_(std::move(probe)) {}

  //  骨架不启用：即使在探测可用的机器上也不报告 async=true（U1–U4 未满足）
  domain::IoEngineCapabilities capabilities() const override;
  fss::Result<std::size_t> ReadAt(int fd, std::uint64_t offset, char* buffer,
                                  std::size_t length) override;
  fss::Result<std::size_t> WriteAt(int fd, std::uint64_t offset, const char* buffer,
                                   std::size_t length) override;
  fss::Result<void> Sync(int fd) override;
  fss::Result<void> SyncFilesystem(int fd) override;
  fss::Result<std::uint64_t> FileSize(int fd) override;

  const sys::IoEngineProbe& probe() const { return probe_; }
  //  为将来的真实实现留出入口；当前恒为 false
  static bool enabled() { return false; }

 private:
  fss::Error NotEnabled() const;

  sys::IoEngineProbe probe_;
};

}  // namespace fss::infra
