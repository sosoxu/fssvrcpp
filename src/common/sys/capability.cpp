#include "common/sys/capability.h"

#include <cerrno>
#include <cstring>
#include <string>

#if defined(__linux__)
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <linux/io_uring.h>
#endif

namespace fss::sys {
namespace {

std::string ErrnoName(int e) {
  switch (e) {
    case 0: return "OK";
#ifdef EPERM
    case EPERM: return "EPERM";
#endif
#ifdef ENOSYS
    case ENOSYS: return "ENOSYS";
#endif
#ifdef EMFILE
    case EMFILE: return "EMFILE";
#endif
#ifdef ENOMEM
    case ENOMEM: return "ENOMEM";
#endif
#ifdef EINVAL
    case EINVAL: return "EINVAL";
#endif
#ifdef EAGAIN
    case EAGAIN: return "EAGAIN";
#endif
    default: return "errno=" + std::to_string(e);
  }
}

std::string KernelRelease() {
#if defined(__linux__)
  struct utsname u;
  if (::uname(&u) == 0) return std::string(u.release);
#endif
  return "unknown";
}

}  // namespace

const char* UringStatusName(UringStatus status) {
  switch (status) {
    case UringStatus::kAvailable: return "available";
    case UringStatus::kBlockedByPolicy: return "blocked_by_policy";
    case UringStatus::kNotSupported: return "not_supported";
    case UringStatus::kOtherError: return "other_error";
  }
  return "?";
}

std::string IoEngineProbe::ToString() const {
  std::string out = "io_uring=";
  out += UringStatusName(uring);
  if (!available()) {
    out += " (" + errno_name + ")";
  }
  out += " kernel=" + (kernel_release.empty() ? std::string("unknown") : kernel_release);
  if (!detail.empty()) out += " — " + detail;
  return out;
}

IoEngineProbe ProbeIoUring(int entries) {
  IoEngineProbe probe;
  probe.entries_requested = entries;
  probe.kernel_release = KernelRelease();

#if defined(__linux__) && defined(__NR_io_uring_setup)
  probe.syscall_number = __NR_io_uring_setup;
  struct io_uring_params params;
  std::memset(&params, 0, sizeof(params));
  errno = 0;
  const long fd = ::syscall(__NR_io_uring_setup, static_cast<unsigned>(entries), &params);
  if (fd >= 0) {
    ::close(static_cast<int>(fd));  // 探测完立刻还回去
    probe.uring = UringStatus::kAvailable;
    probe.errno_value = 0;
    probe.errno_name = "OK";
    probe.detail = "io_uring_setup 成功（已关闭探测 fd）";
    return probe;
  }
  probe.errno_value = errno;
  probe.errno_name = ErrnoName(errno);
  switch (errno) {
    case EPERM:
      // 两种成因都会给 EPERM，且**无法**在用户态区分：
      //   ① 容器 seccomp profile 拦截 io_uring_*；
      //   ② 宿主设置了 kernel.io_uring_disabled=1/2。
      // 所以文案必须把两种可能都说出来，并给出各自的确认方式。
      probe.uring = UringStatus::kBlockedByPolicy;
      probe.detail =
          "被策略阻断（EPERM）：可能是容器 seccomp 拦截 io_uring_*，"
          "也可能是宿主 kernel.io_uring_disabled 非 0；"
          "用 scripts/check_io_uring.sh 在目标环境确认";
      break;
    case ENOSYS:
      probe.uring = UringStatus::kNotSupported;
      probe.detail = "内核不支持 io_uring_setup（ENOSYS）";
      break;
    default:
      probe.uring = UringStatus::kOtherError;
      probe.detail = std::string("io_uring_setup 失败：") + std::strerror(probe.errno_value);
      break;
  }
  return probe;
#else
  probe.syscall_number = -1;
  probe.uring = UringStatus::kNotSupported;
  probe.detail = "编译目标不支持 io_uring（非 Linux 或缺少 __NR_io_uring_setup）";
  return probe;
#endif
}

std::optional<IoEngineMode> ParseIoEngineMode(std::string_view text) {
  if (text == "blocking") return IoEngineMode::kBlocking;
  if (text == "uring") return IoEngineMode::kUring;
  if (text == "auto") return IoEngineMode::kAuto;
  return std::nullopt;
}

const char* IoEngineModeName(IoEngineMode mode) {
  switch (mode) {
    case IoEngineMode::kBlocking: return "blocking";
    case IoEngineMode::kUring: return "uring";
    case IoEngineMode::kAuto: return "auto";
  }
  return "?";
}

IoEngineDecision DecideIoEngine(const IoEngineProbe& probe, IoEngineMode configured) {
  IoEngineDecision d;
  d.requested = configured;
  d.probe_detail = probe.ToString();

  switch (configured) {
    case IoEngineMode::kBlocking:
      // 即使探测可用也不用：配置说得很清楚
      d.chosen = IoEngineMode::kBlocking;
      d.message = "io_engine=blocking（配置指定）；探测结果：" + d.probe_detail;
      return d;

    case IoEngineMode::kUring:
      d.chosen = IoEngineMode::kUring;
      if (probe.available()) {
        d.message = "io_engine=uring 已启用；探测结果：" + d.probe_detail;
      } else {
        // ★ fail-fast：不回退。理由见 ADR-010 §4 与 R-29。
        d.refuse_start = true;
        d.message = "io_engine=uring 被显式要求，但环境不可用 → 拒绝启动。";
        switch (probe.uring) {
          case UringStatus::kBlockedByPolicy:
            d.message +=
                "修复：在容器里放行 io_uring_* 系统调用（seccomp=unconfined 仅用于排障），"
                "或改用 io_engine=auto/blocking。";
            break;
          case UringStatus::kNotSupported:
            d.message +=
                "修复：内核过旧（io_uring 需要 Linux 5.1+），改用 io_engine=auto/blocking。";
            break;
          default:
            d.message += "修复：先排除资源类错误（" + probe.errno_name +
                         "），或改用 io_engine=auto/blocking。";
            break;
        }
        d.message += " 探测结果：" + d.probe_detail;
      }
      return d;

    case IoEngineMode::kAuto:
      if (probe.available()) {
        d.chosen = IoEngineMode::kUring;
        d.message = "io_engine=auto：探测到 io_uring 可用 → 选用 uring。" + d.probe_detail;
      } else {
        // ★ 回退是**成功**路径，不是错误：必须记录（R11），但不拒绝启动
        d.chosen = IoEngineMode::kBlocking;
        d.message = "io_engine=auto：io_uring 不可用（" + probe.errno_name +
                    "）→ 已回退到 blocking。这是预期行为，不影响功能。" + d.probe_detail;
      }
      return d;
  }
  d.message = "未知的 io_engine 配置";
  d.refuse_start = true;
  return d;
}

}  // namespace fss::sys
