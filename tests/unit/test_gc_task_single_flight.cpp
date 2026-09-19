// =============================================================================
//  C9.31（P9 补交，P10 期间完成）：`GcTask::Run` 的**单飞护栏**（L4 单测，确定性）
// =============================================================================
//  判据原文（docs/04-implementation-plan.md 的 C9.31）：
//    "`GcTask::Run` 必须自带单飞护栏（周期调度与按需端点共享）：已有一轮在跑时，
//     第二次调用**不排队、不并行**，立刻返回 `kUnavailable`（HTTP 503），消息可读；
//     释放后再次调用必须成功。"
//
//  为什么必须**确定性**（不用 sleep 猜时序）：
//    "第二次调用立刻返回"这句话只有在"第一次确实还在跑"的前提下才有意义。这里用
//    `fss::test::BlockingTempSweepBlobStore`（tests/framework/fake_ports.h）把
//    `GcTask::Run` 尾部的 `remove_temp_files` 变成一扇门：`WaitEntered()` 之后，
//    护栏**必然**被 A 线程持有；不需要任何 sleep/概率。
//
//  ⚠️ 反向（R1 自证）：把 `GcTask::Run` 里的 `try_lock` 换成 `lock()`（或删掉护栏），
//    下面 `returned_while_first_round_running` 的断言必须失败 —— 因为 B 会**排队**到
//    A 被释放之后才返回。见 docs/test-evidence/phase9.md 的 C9.31 自证小节。
// =============================================================================
#include <catch2/catch.hpp>

#include "app_fixture.h"

#include "app/tasks/gc_task.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace {

using fss::app::GcOptions;
using fss::app::GcReport;
using fss::app::GcTask;
using fss::domain::StorageZone;
using fss::test::AppFixture;
using fss::test::BlockingTempSweepBlobStore;
using fss::test::InMemoryLeaseRepository;

}  // namespace

TEST_CASE("★ C9.31：GcTask::Run 的单飞护栏（在跑时立刻 kUnavailable，不排队不并行）",
          "[phase9][unit][c9.31]") {
  AppFixture app;
  //  ★ 两个 zone 都指向"会阻塞的装饰器"：`GcTask::Run` 的临时文件清扫按 zone 调
  //    `remove_temp_files`，第一次调用即被拦住（第二次在放行后正常返回）。
  fss::infra::InMemoryBlobStore inner(app.clock);
  BlockingTempSweepBlobStore blocking(inner);
  app.factory.SetZoneStore(StorageZone::kStaging, blocking);
  app.factory.SetZoneStore(StorageZone::kPersistent, blocking);
  InMemoryLeaseRepository leases;
  GcTask task(*app.ports, leases, "gc-instance-a");

  GcOptions options;  // 默认 dry_run=true，对本用例无影响（护栏与删除无关）

  //  ---- 线程 A：进入 Run 并停在 remove_temp_files（= 已持有护栏）----
  std::atomic<bool> a_done{false};
  bool a_ok = false;
  std::string a_error;
  std::thread a([&] {
    const auto report = task.Run("opendes", options);
    a_ok = report.ok();
    if (!report.ok()) a_error = report.error().ToString();
    a_done.store(true);
  });
  blocking.WaitEntered();
  REQUIRE_FALSE(a_done.load());  // A 确实还在跑（不是"已经悄悄结束"）

  //  ---- 线程 B：在 A 仍在跑时调用 Run ----
  std::atomic<bool> b_done{false};
  bool b_ok = true;
  fss::ErrorKind b_kind = fss::ErrorKind::kInternal;
  std::string b_message;
  std::thread b([&] {
    const auto report = task.Run("opendes", options);
    b_ok = report.ok();
    if (!report.ok()) {
      b_kind = report.error().kind();
      b_message = report.error().message();
    }
    b_done.store(true);
  });

  //  ★ 判据 ①：B **不排队** —— 给一个宽裕窗口（500ms）让 B 返回；护栏正确时 B 在微秒级
  //    返回。护栏缺失时 B 会阻塞到 A 被放行之后（`b_done` 仍为 false）。
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!b_done.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const bool returned_while_first_round_running = b_done.load();
  CAPTURE(returned_while_first_round_running, b_ok, a_done.load());

  //  无论 B 是否已返回，都必须放行 A（否则用例会挂死，而不是"失败"）
  blocking.Release();
  a.join();
  b.join();

  REQUIRE(returned_while_first_round_running);  // ★ 不排队（去掉护栏时这一条失败）
  REQUIRE_FALSE(b_ok);                          // ★ 不并行
  REQUIRE(b_kind == fss::ErrorKind::kUnavailable);
  REQUIRE(b_message.find("已在运行") != std::string::npos);  // 消息可读

  //  A 本身正常完成（护栏不能把正常的一轮判成失败）
  REQUIRE(a_done.load());
  REQUIRE(a_ok);
  CAPTURE(a_error);
  REQUIRE(a_error.empty());

  //  ---- 判据 ②：上一轮结束后，再调一次必须**成功**（护栏不是"一次性"的）----
  const auto after = task.Run("opendes", options);
  CAPTURE(after.ok());
  REQUIRE(after.ok());
}

TEST_CASE("★ C9.31：单飞护栏与 options 无关（dry-run/真跑都受同一把锁保护）",
          "[phase9][unit][c9.31]") {
  AppFixture app;
  fss::infra::InMemoryBlobStore inner(app.clock);
  BlockingTempSweepBlobStore blocking(inner);
  app.factory.SetZoneStore(StorageZone::kStaging, blocking);
  app.factory.SetZoneStore(StorageZone::kPersistent, blocking);
  InMemoryLeaseRepository leases;
  GcTask task(*app.ports, leases, "gc-instance-b");

  GcOptions real_run;
  real_run.dry_run = false;

  std::thread a([&] { (void)task.Run("opendes", real_run); });
  blocking.WaitEntered();

  //  第二轮用**另一份** options（dry_run=true）：护栏是 `GcTask` 的性质，与 options 无关。
  //  ★ 仍然在独立线程里调用 + 有界窗口：**去掉护栏时这一条会"超窗失败"，而不是挂死**
  //    （挂死只会得到 ctest 超时，无法区分"护栏缺失"与"测试写错"）。
  std::atomic<bool> second_done{false};
  bool second_ok = true;
  fss::ErrorKind second_kind = fss::ErrorKind::kInternal;
  std::thread b([&] {
    const auto second = task.Run("opendes", GcOptions{});
    second_ok = second.ok();
    if (!second.ok()) second_kind = second.error().kind();
    second_done.store(true);
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!second_done.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const bool returned = second_done.load();
  blocking.Release();
  a.join();
  b.join();

  REQUIRE(returned);
  REQUIRE_FALSE(second_ok);
  REQUIRE(second_kind == fss::ErrorKind::kUnavailable);
}
