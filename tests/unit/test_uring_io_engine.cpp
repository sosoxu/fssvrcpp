// =============================================================================
//  ADR-010：UringIoEngine 探测骨架 —— 不启用、如实声明、显式拒绝
// =============================================================================
//  ADR-010 的 R-29：**禁止静默回退** —— "以为开了加速、其实一直在跑阻塞路径"
//  只能等线上发现性能回归。因此骨架必须在被调用时显式报 `kUnimplemented`，
//  并把探测结果放进错误消息（R11：探测结果要可见）。
// =============================================================================
#include <catch2/catch.hpp>

#include "common/result/result.h"
#include "common/sys/capability.h"
#include "infra/io/uring_io_engine.h"

#include <string>

using fss::infra::UringIoEngine;
using fss::sys::IoEngineMode;
using fss::sys::ProbeIoUring;

TEST_CASE("★ ADR-010 UringIoEngine 骨架：capabilities 如实 + 原语显式拒绝",
          "[phase3][io][uring]") {
  const auto probe = ProbeIoUring();
  UringIoEngine engine(probe);

  const auto caps = engine.capabilities();
  REQUIRE(caps.engine_name == "uring");
  REQUIRE_FALSE(caps.async);  // 骨架未启用：即使探测可用也不谎报 async
  REQUIRE_FALSE(UringIoEngine::enabled());

  char buffer[8] = {};
  const auto read = engine.ReadAt(0, 0, buffer, sizeof buffer);
  REQUIRE_FALSE(read.ok());
  REQUIRE(read.error().kind() == fss::ErrorKind::kUnimplemented);

  const auto write = engine.WriteAt(0, 0, buffer, sizeof buffer);
  REQUIRE_FALSE(write.ok());
  REQUIRE(write.error().kind() == fss::ErrorKind::kUnimplemented);

  const auto sync = engine.Sync(0);
  REQUIRE_FALSE(sync.ok());
  REQUIRE(sync.error().kind() == fss::ErrorKind::kUnimplemented);

  const auto syncfs = engine.SyncFilesystem(0);
  REQUIRE_FALSE(syncfs.ok());
  REQUIRE(syncfs.error().kind() == fss::ErrorKind::kUnimplemented);

  const auto size = engine.FileSize(0);
  REQUIRE_FALSE(size.ok());
  REQUIRE(size.error().kind() == fss::ErrorKind::kUnimplemented);

  //  探测摘要必须可见（进入错误消息 + 非空），否则"为什么不可用"无从排查
  const std::string detail = probe.ToString();
  REQUIRE_FALSE(detail.empty());
  REQUIRE(read.error().message().find(detail) != std::string::npos);
}

TEST_CASE("★ ADR-010 引擎决策：auto 可能选 uring，但骨架未启用 → 组合根必须再确认",
          "[phase3][io][uring]") {
  const auto probe = ProbeIoUring();

  const auto auto_decision = fss::sys::DecideIoEngine(probe, IoEngineMode::kAuto);
  REQUIRE_FALSE(auto_decision.refuse_start);  // auto 永远不拒绝启动
  REQUIRE_FALSE(auto_decision.message.empty());

  if (probe.available()) {
    //  本机实测：宿主机探测**可用**（与"默认容器一定阻断"的笼统说法不同 —— R8：
    //  部署约束必须在目标环境验证）。
    REQUIRE(auto_decision.chosen == IoEngineMode::kUring);
    //  ★ P3-D08：L1 的策略选了 uring，但 L2 的骨架**没有启用**（U1–U4 未满足）。
    //    组合根若盲信 `chosen`，就会装上一个"每个调用都返回 kUnimplemented"的引擎。
    //    因此组合根（P4）必须在 `chosen==kUring` 时再查 `UringIoEngine::enabled()`，
    //    未启用则回退 blocking 并**记录日志**（auto 允许回退；显式 uring 才 fail-fast）。
    REQUIRE_FALSE(UringIoEngine::enabled());
    WARN("本机 io_uring 探测可用：'容器不可用 → fail-fast' 分支未在本机验证");
  } else {
    REQUIRE(auto_decision.chosen == IoEngineMode::kBlocking);
    const auto explicit_uring = fss::sys::DecideIoEngine(probe, IoEngineMode::kUring);
    REQUIRE(explicit_uring.refuse_start);  // fail-fast，而不是静默回退（R-29）
  }
}
