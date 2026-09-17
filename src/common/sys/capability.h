// =============================================================================
//  sys（L1）：系统能力探测（io_uring）
// =============================================================================
//
//  为什么探测必须是一个**独立、可测试、结果结构化**的东西
//    ADR-010 的结论是"阻塞线程池为默认，io_uring 为可选加速引擎"，理由是
//    **默认容器 seccomp 会阻断 io_uring_setup（实测 EPERM）**。这一个事实带来三个约束：
//      ① 默认路径绝不能依赖 io_uring（否则容器里启动就挂）；
//      ② 探测结果必须**可见**（日志 + `/v2/info`），否则线上永远不知道该不该开；
//      ③ "探测失败"与"配置要求用 uring"的**决策**必须是纯函数，能被穷举测试。
//    所以本模块把"探测（有副作用）"与"决策（纯函数）"分开：
//      ProbeIoUring()      → 真实调用 io_uring_setup 并分类失败原因
//      DecideIoEngine(...) → 给定探测结果与配置，输出"用什么 / 是否拒绝启动 / 记录什么"
//
//  ⚠️ 本模块**不**链接 liburing：只用 `syscall(__NR_io_uring_setup)`。
//     理由与 `scripts/check_io_uring.sh` 相同：容器里不该为了探测而多一个动态库依赖。
//     （ADR-010 §2 的 D2 候选评估：liburing 的价值在 P3 实现引擎时再评估。）
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fss::sys {

// -----------------------------------------------------------------------------
//  探测结果
// -----------------------------------------------------------------------------
enum class UringStatus {
  kAvailable,      // io_uring_setup 成功
  kBlockedByPolicy,  // EPERM：seccomp 拦截 **或** `kernel.io_uring_disabled`
  kNotSupported,   // ENOSYS：内核没有该系统调用
  kOtherError,     // 其它 errno（EMFILE/ENOMEM…）—— 不当作"不支持"，但也不可用
};

const char* UringStatusName(UringStatus status);

struct IoEngineProbe {
  UringStatus uring = UringStatus::kOtherError;
  int errno_value = 0;        // 仅当 !ok 时有意义
  std::string errno_name;     // 例如 "EPERM"
  std::string detail;         // 人类可读（会进日志）
  std::string kernel_release; // uname -r
  long syscall_number = -1;   // __NR_io_uring_setup（-1 = 该架构没有）
  int entries_requested = 0;

  bool available() const { return uring == UringStatus::kAvailable; }
  // 一行摘要，用于日志/`/v2/info`
  std::string ToString() const;
};

// 真实探测：调用一次 io_uring_setup(entries, &params)，成功则立刻关闭 fd。
// 不会抛异常，任何失败都反馈在返回值里。
IoEngineProbe ProbeIoUring(int entries = 8);

// -----------------------------------------------------------------------------
//  配置 → 决策
// -----------------------------------------------------------------------------
enum class IoEngineMode { kBlocking, kUring, kAuto };

std::optional<IoEngineMode> ParseIoEngineMode(std::string_view text);
const char* IoEngineModeName(IoEngineMode mode);

struct IoEngineDecision {
  IoEngineMode requested = IoEngineMode::kBlocking;
  IoEngineMode chosen = IoEngineMode::kBlocking;
  // true = 配置显式要求 uring 但环境不可用 → **拒绝启动**（fail-fast）
  //  为什么拒绝而不是静默回退：ADR-010 的 R-29 —— 静默回退会让"以为开了加速"
  //  变成"其实一直在跑阻塞路径"，性能回归只能等线上发现。
  bool refuse_start = false;
  // 无论哪种情况都必须记录（R11：探测结果要可见）
  std::string message;
  // 供 `/v2/info` 与指标使用的结构化字段
  std::string probe_detail;
};

// 纯函数：给定探测结果与配置的引擎模式，产出决策。可穷举测试。
IoEngineDecision DecideIoEngine(const IoEngineProbe& probe, IoEngineMode configured);

}  // namespace fss::sys
