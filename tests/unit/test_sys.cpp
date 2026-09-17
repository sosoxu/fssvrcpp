// C1.14 / C1.15：io_uring 能力探测（可区分可用 / EPERM / ENOSYS）+ 回退语义
//
// 判据（plan C1.14/C1.15）：
//   · 能区分"可用 / EPERM（seccomp 或 io_uring_disabled）/ ENOSYS（内核不支持）"
//   · 返回**结构化**结果供 P2 的 IIoEngine 与 P3 的引擎选择使用
//   · io_engine=auto 时**成功回退**并记录；io_engine=uring 时**拒绝启动**
//
// ⚠️ 关于"探测结果因环境而异"：本用例对**真实**探测只断言"结果自洽 + 与 errno 一致"，
//    不断言"必须可用"或"必须不可用"（那取决于运行环境，见 R8：部署约束必须在目标环境验证）。
//    "默认容器里必须报告不可用"这一条由 scripts/check_io_uring.sh 在**容器内**验证，
//    证据记入 docs/test-evidence/phase1.md。
#include <catch2/catch.hpp>

#include "common/sys/capability.h"

#include <cerrno>
#include <cstring>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/io_uring.h>
#endif

using fss::sys::DecideIoEngine;
using fss::sys::IoEngineDecision;
using fss::sys::IoEngineMode;
using fss::sys::IoEngineProbe;
using fss::sys::ProbeIoUring;
using fss::sys::UringStatus;

namespace {

IoEngineProbe MakeProbe(UringStatus status, int errno_value = 0, const char* errno_name = "") {
  IoEngineProbe p;
  p.uring = status;
  p.errno_value = errno_value;
  p.errno_name = errno_name;
  p.kernel_release = "5.15.0-test";
  p.syscall_number = 425;
  p.entries_requested = 8;
  p.detail = "合成探测结果（测试用）";
  return p;
}

}  // namespace

TEST_CASE("★ 真实探测：结果必须自洽（status 与 errno 一致）", "[phase1][sys][c1.14]") {
  const auto probe = ProbeIoUring(8);
  INFO(probe.ToString());

  // 结构化字段必须被填充（否则 P2/P3 拿不到判断依据）
  REQUIRE_FALSE(probe.kernel_release.empty());
  REQUIRE(probe.entries_requested == 8);
#if defined(__linux__)
  REQUIRE(probe.syscall_number > 0);

  // 状态与 errno 必须一致 —— 这是"能区分三种情况"的核心断言
  switch (probe.uring) {
    case UringStatus::kAvailable:
      REQUIRE(probe.errno_value == 0);
      REQUIRE(probe.available());
      break;
    case UringStatus::kBlockedByPolicy:
      REQUIRE(probe.errno_value == EPERM);
      REQUIRE(probe.errno_name == "EPERM");
      REQUIRE_FALSE(probe.available());
      // 文案必须提示"两种成因且无法在用户态区分"，避免误导排障方向
      REQUIRE(probe.detail.find("seccomp") != std::string::npos);
      REQUIRE(probe.detail.find("io_uring_disabled") != std::string::npos);
      break;
    case UringStatus::kNotSupported:
      REQUIRE(probe.errno_value == ENOSYS);
      REQUIRE(probe.errno_name == "ENOSYS");
      break;
    case UringStatus::kOtherError:
      REQUIRE(probe.errno_value != 0);
      break;
  }
#else
  REQUIRE(probe.uring == UringStatus::kNotSupported);
#endif

  // 探测必须可重复（幂等、不泄漏 fd）
  const auto again = ProbeIoUring(8);
  REQUIRE(again.uring == probe.uring);
  REQUIRE(again.errno_value == probe.errno_value);
}

TEST_CASE("★ 自证对照：分类结果必须与**独立**的裸 syscall 一致（R1）",
          "[phase1][sys][c1.14]") {
  // 目的：证明 ProbeIoUring 的分类不是"我们自己包装函数的假象"。
  // 这里在测试里**再写一遍**裸 syscall，把 errno 与我们的分类对照。
#if defined(__linux__) && defined(__NR_io_uring_setup)
  errno = 0;
  struct io_uring_params params;
  std::memset(&params, 0, sizeof(params));
  const long fd = ::syscall(__NR_io_uring_setup, 8u, &params);
  const int raw_errno = errno;
  if (fd >= 0) ::close(static_cast<int>(fd));

  const auto probe = ProbeIoUring(8);
  if (fd >= 0) {
    REQUIRE(probe.uring == UringStatus::kAvailable);
    REQUIRE(probe.errno_value == 0);
  } else {
    REQUIRE_FALSE(probe.available());
    REQUIRE(probe.errno_value == raw_errno);
    if (raw_errno == EPERM) REQUIRE(probe.uring == UringStatus::kBlockedByPolicy);
    if (raw_errno == ENOSYS) REQUIRE(probe.uring == UringStatus::kNotSupported);
  }
  INFO("裸 syscall errno=" << raw_errno << " ；我们的分类=" << probe.ToString());
#else
  SUCCEED("非 Linux：跳过裸 syscall 对照");
#endif
}

TEST_CASE("★ 探测足以区分三种失败原因（用合成结果覆盖环境测不到的分支）",
          "[phase1][sys][c1.14]") {
  // 真实环境只可能落在其中一种；另外两种用合成结果验证"分类逻辑"本身。
  // 这不是"假装测过环境"，而是测**我们自己的分类与决策**（环境那部分交给脚本 + 目标环境）。
  REQUIRE(std::string(fss::sys::UringStatusName(UringStatus::kAvailable)) == "available");
  REQUIRE(std::string(fss::sys::UringStatusName(UringStatus::kBlockedByPolicy)) ==
          "blocked_by_policy");
  REQUIRE(std::string(fss::sys::UringStatusName(UringStatus::kNotSupported)) == "not_supported");
  REQUIRE(std::string(fss::sys::UringStatusName(UringStatus::kOtherError)) == "other_error");

  REQUIRE(MakeProbe(UringStatus::kAvailable).available());
  REQUIRE_FALSE(MakeProbe(UringStatus::kBlockedByPolicy, EPERM, "EPERM").available());
  REQUIRE_FALSE(MakeProbe(UringStatus::kNotSupported, ENOSYS, "ENOSYS").available());

  // ToString 必须带上状态与内核版本（/v2/info 与日志直接用）
  const auto text = MakeProbe(UringStatus::kBlockedByPolicy, EPERM, "EPERM").ToString();
  INFO(text);
  REQUIRE(text.find("blocked_by_policy") != std::string::npos);
  REQUIRE(text.find("EPERM") != std::string::npos);
  REQUIRE(text.find("5.15.0-test") != std::string::npos);
}

TEST_CASE("io_engine 配置解析", "[phase1][sys][c1.15]") {
  REQUIRE(fss::sys::ParseIoEngineMode("blocking").value() == IoEngineMode::kBlocking);
  REQUIRE(fss::sys::ParseIoEngineMode("uring").value() == IoEngineMode::kUring);
  REQUIRE(fss::sys::ParseIoEngineMode("auto").value() == IoEngineMode::kAuto);
  REQUIRE_FALSE(fss::sys::ParseIoEngineMode("").has_value());
  REQUIRE_FALSE(fss::sys::ParseIoEngineMode("URing").has_value());  // 大小写敏感（配置项拼错要暴露）
  REQUIRE_FALSE(fss::sys::ParseIoEngineMode("io_uring").has_value());
}

TEST_CASE("★ C1.15 决策矩阵：auto 回退 / uring 拒绝启动 / blocking 恒定",
          "[phase1][sys][c1.15]") {
  const auto available = MakeProbe(UringStatus::kAvailable);
  const auto blocked = MakeProbe(UringStatus::kBlockedByPolicy, EPERM, "EPERM");
  const auto nosys = MakeProbe(UringStatus::kNotSupported, ENOSYS, "ENOSYS");
  const auto other = MakeProbe(UringStatus::kOtherError, EMFILE, "EMFILE");

  SECTION("blocking：无论探测如何都不用 uring，且不拒绝启动") {
    for (const auto& p : {available, blocked, nosys, other}) {
      const auto d = DecideIoEngine(p, IoEngineMode::kBlocking);
      REQUIRE(d.chosen == IoEngineMode::kBlocking);
      REQUIRE_FALSE(d.refuse_start);
      REQUIRE(d.message.find("blocking") != std::string::npos);
      // 探测结果必须出现在 message 里（R11：结果可见）
      REQUIRE(d.message.find("io_uring=") != std::string::npos);
      REQUIRE_FALSE(d.probe_detail.empty());
    }
  }

  SECTION("uring + 可用：采用 uring，不拒绝") {
    const auto d = DecideIoEngine(available, IoEngineMode::kUring);
    REQUIRE(d.chosen == IoEngineMode::kUring);
    REQUIRE_FALSE(d.refuse_start);
  }

  SECTION("★ uring + 不可用：**拒绝启动**（不静默回退），并给出可执行的修复指令") {
    for (const auto& p : {blocked, nosys, other}) {
      const auto d = DecideIoEngine(p, IoEngineMode::kUring);
      REQUIRE(d.refuse_start);
      REQUIRE(d.message.find("拒绝启动") != std::string::npos);
      REQUIRE(d.message.find("修复") != std::string::npos);
      REQUIRE(d.message.find("io_engine=auto") != std::string::npos);
    }
    // EPERM 的修复指令要指向 seccomp
    REQUIRE(DecideIoEngine(blocked, IoEngineMode::kUring).message.find("seccomp") !=
            std::string::npos);
    // ENOSYS 的修复指令要指向内核版本
    REQUIRE(DecideIoEngine(nosys, IoEngineMode::kUring).message.find("内核") != std::string::npos);
  }

  SECTION("★ auto：可用则选 uring，不可用则**成功回退**到 blocking（不拒绝启动）") {
    const auto up = DecideIoEngine(available, IoEngineMode::kAuto);
    REQUIRE(up.chosen == IoEngineMode::kUring);
    REQUIRE_FALSE(up.refuse_start);
    REQUIRE(up.message.find("选用 uring") != std::string::npos);

    for (const auto& p : {blocked, nosys, other}) {
      const auto d = DecideIoEngine(p, IoEngineMode::kAuto);
      REQUIRE(d.chosen == IoEngineMode::kBlocking);
      REQUIRE_FALSE(d.refuse_start);  // ★ 回退是成功路径
      REQUIRE(d.message.find("回退") != std::string::npos);
      REQUIRE(d.message.find("这是预期行为") != std::string::npos);
      REQUIRE(d.message.find("io_uring=") != std::string::npos);
    }
  }

  SECTION("决策是纯函数：同样输入永远同样输出（可重复、无线程状态）") {
    for (int i = 0; i < 3; ++i) {
      const auto a = DecideIoEngine(blocked, IoEngineMode::kAuto);
      REQUIRE(a.chosen == IoEngineMode::kBlocking);
      REQUIRE_FALSE(a.refuse_start);
    }
  }
}
