#include "common/sys/capability.h"

#include <cerrno>
#include <cstdlib>
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
  if (injected) {
    out += available() ? "available(injected)" : "blocked_by_policy(injected)";
  } else {
    out += UringStatusName(uring);
  }
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

  //  ★ C9.30 的**探测注入接缝**（测试/演练用，环境变量，**不是配置键**）：
  //    `FSS_IO_PROBE_INJECT=available` → 探测结果强制为**可用**；
  //    `FSS_IO_PROBE_INJECT=blocked`   → 探测结果强制为**被策略阻断（EPERM）**。
  //
  //  为什么需要它（区分力论证，R1）：**接缝让"字段真的来自探测"可被证伪**。
  //    ① `availability` 方向：若实现把 `ioUringAvailable` 硬编码成 `false`，
  //       `=available` 这条用例必然失败；
  //    ② `blocked` 方向：若实现把它硬编码成 `true`（或"用了就恒 true"），
  //       `=blocked` 这条用例必然失败。
  //    两个方向都测 ⇒ 该字段既不是恒 false 也不是恒 true。
  //  ⚠️ 本机的**真实**真值由宿主决定，不要假设：本工作机（WSL2，kernel.io_uring_disabled=0）
  //     实测 `available()==true`（`io_uring_setup` 直接成功），而 **Docker 默认 seccomp**
  //     下同一调用是 `EPERM`（两种环境都已实测，见 `docs/test-evidence/phase9.md` 的 C9.30 节）。
  //     因此"不注入 → 必须为 false"这条**不能**当判据：它会把正确实现判失败。
  //     判据必须是"字段随探测结果变化"，这正是本接缝提供的。
  //
  //  ⚠️ 它**只**改"探测结果"，**不**改 `UringIoEngine::enabled()`（恒 false）：
  //     ① `/v2/info` 的 `ioEngine` 仍是 `blocking`（生效引擎不是探测结果）；
  //     ② `storage.io_engine=uring` 仍**拒绝启动**（ADR-010 的 U1~U4 未满足）。
  //     `=available` 注入下拒绝启动的理由会明确写成"内核探测通过，但引擎实现尚未启用"
  //     （就是 `=blocked` 之外的那条分支）—— 注入**不放宽任何启动判据**。
  //
  //  为什么不做成配置键（与 `FSS_STARTUP_FAULT_INJECT` / `FSS_AUDIT_FAULT_INJECT`
  //  同一理由）：① `docs/operations.md` 的 156 个叶子键三态清单由
  //  `test_operations_doc` 与 `config/fss.example.json` **机械比对**，"让探测说假话"
  //  不是运维语义；② 生产上改写能力探测的结果只会误导 R11 的可观测性，
  //  没有任何合法用途（真正要开 uring 得先满足 U1~U4 并交付引擎实现）。
  //  登记：`docs/runbook.md` 的测试/演练小节（明确写"不要在生产设置"）。
  //  未知取值 → 不注入（宽容策略，与 `FSS_AUDIT_FAULT_INJECT` 只认 "1" 一致）。
  const char* const inject_env = std::getenv("FSS_IO_PROBE_INJECT");
  const std::string inject = inject_env == nullptr ? std::string() : std::string(inject_env);
  if (inject == "available") {
    probe.injected = true;
    probe.uring = UringStatus::kAvailable;
    probe.errno_value = 0;
    probe.errno_name = "OK";
    probe.syscall_number = -1;
    probe.detail =
        "★ 探测注入（FSS_IO_PROBE_INJECT=available）：结果被强制为 available；"
        "仅用于测试/演练，**不**代表真实宿主能力，**也不**启用 UringIoEngine";
    return probe;
  }
  if (inject == "blocked") {
    probe.injected = true;
    probe.uring = UringStatus::kBlockedByPolicy;
    probe.errno_value = EPERM;
    probe.errno_name = "EPERM";
    probe.syscall_number = -1;
    probe.detail =
        "★ 探测注入（FSS_IO_PROBE_INJECT=blocked）：结果被强制为 blocked_by_policy(EPERM)，"
        "用于复刻默认容器 seccomp 的形态；仅用于测试/演练";
    return probe;
  }

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
