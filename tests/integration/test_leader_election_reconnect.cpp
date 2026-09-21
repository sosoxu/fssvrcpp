// =============================================================================
//  ADR-009 §4.4 回归：**锁连接被对端切断后必须能重新当选**
// =============================================================================
//  为什么需要这条用例（本缺陷是实验室实测发现的，见 docs/lab-nfs-multiinstance.md §8.1）：
//    `PgLeaderElection` 的锁是**会话级** advisory lock —— 会话断了，锁就没了。真实世界里
//    "会话断"最常见的触发不是 kill -9，而是 **PG 重启 / 主备切换 / 网络抖动**：
//      ① PG 重启 → 所有实例的锁连接一起被切断；
//      ② 老代码在"已是 leader 且连接不健康 / ping 失败"时 `connection_.reset()` 降级（方向正确），
//         但接下来的 `TryAcquire()` 在 `connection_ == nullptr` 时直接返回 `kUnavailable`，
//         **没有任何重连**；非 leader 实例的死连接同样不会被重建；
//      ③ 结果：`pg_locks` 里 0 行 advisory lock，**所有实例**都答"我不是 leader"，
//         GC 全集群停摆，而 readiness 仍是 200（静默失败）。
//
//  判据（每一步都是真实往返 / 轮询，不用固定 sleep）：
//    ① 正控：E1 `TryAcquire()` 成功（我们确实持锁，且 `pg_locks` 能看到该 key）；
//    ② 负控：同键的 E2 在同一时刻**不得**自称 leader（防"恒真"）；
//    ③ 从**另一条连接** `pg_terminate_backend(持锁后端)` —— 等价于 PG 重启把锁会话切断；
//    ④ 轮询 E1->IsLeader()：**必须在有限时间内重新为 true**（老代码在这里永远 false）；
//    ⑤ 恢复后 E2 仍为 false（不是"谁都说自己是 leader"）；
//    ⑥ 收尾：Release 后 `pg_locks` 里该 key 的行消失（不留下脏锁）。
//
//  ★ 连不上 PG 就失败（REQUIRE），绝不静默跳过；DSN 取自 `FSS_PG_DSN`。
// =============================================================================
#include <catch2/catch.hpp>

#include "infra/postgres/pg_connection.h"
#include "infra/postgres/pg_leader_election.h"

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>

#include <unistd.h>

namespace {

using fss::infra::PgConnection;
using fss::infra::PgLeaderElection;
using fss::infra::PgLeaderElectionOptions;
using fss::infra::PgOptions;

//  与 test_multi_mode.cpp 同源：默认指向本机 dev PG，可用 `FSS_PG_DSN` 覆盖（实验室/远端）。
constexpr const char* kDefaultDsn = "postgresql://fss@127.0.0.1:15432/fss";

std::string TestDsn() {
  const char* env = std::getenv("FSS_PG_DSN");
  if (env != nullptr && *env != '\0') return std::string(env);
  return kDefaultDsn;
}

PgOptions PgOptionsFor(const std::string& dsn) {
  PgOptions options;
  options.dsn = dsn;
  options.max_connections = 2;
  return options;
}

//  每次运行换一个锁键：避免与其它用例/其它实例抢同一把锁（真实部署里是同一个 key）。
//  ★ 用 pid + 自增计数（**不用** `std::random_device`）：可复现、可重放，且断言条数不随
//    运行漂移（本仓库其它 id 生成也走"实例标识 + 计数"这条路，见 AGENTS §4.4 第一条）。
std::int64_t UniqueLockKey() {
  static std::atomic<std::int64_t> counter{0};
  const auto pid = static_cast<std::int64_t>(::getpid());
  const auto salt = counter.fetch_add(1);
  return 1 + ((pid * 2654435761LL + salt * 7919LL) % 2147483646LL);
}

//  该 key 当前被哪个后端持有（空串 = 无人持锁）。
std::string LockHolderPid(PgConnection& connection, std::int64_t key) {
  const auto rows = connection.ExecParams(
      R"pglock(SELECT pid::text FROM pg_locks WHERE locktype = 'advisory' AND objid = $1::bigint)pglock",
      {std::to_string(key)});
  REQUIRE(rows.ok());
  if (rows.value().RowCount() < 1) return {};
  return rows.value().Value(0, 0);
}

//  轮询到条件成立（有界超时）。返回实际耗时毫秒数；超时返回 -1。
template <typename Fn>
long PollUntil(Fn&& predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto started = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - started)
          .count();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return -1;
}

}  // namespace

TEST_CASE("★ ADR-009：锁连接被对端切断后必须重连并重新当选（PG 重启/网络抖动的等价场景）",
          "[pg][leader-election][regression]") {
  const std::string dsn = TestDsn();
  const auto options = PgOptionsFor(dsn);
  const std::int64_t key = UniqueLockKey();

  PgLeaderElectionOptions election_options;
  election_options.pg = options;
  election_options.lock_key = key;

  auto first = PgLeaderElection::Open(election_options);
  //  前置条件失败时要把 DSN 打出来：这条 REQUIRE 失败 = PG 当时不可达（不是逻辑抖动）。
  INFO("FSS_PG_DSN = " << dsn << "（连不上 PG ⇒ 用例如实失败，绝不静默跳过）");
  REQUIRE(first.ok());  // 连不上 PG ⇒ 用例如实失败（不静默跳过）
  auto second = PgLeaderElection::Open(election_options);
  REQUIRE(second.ok());
  auto observer = PgConnection::Connect(options);
  REQUIRE(observer.ok());

  //  ---- ① 正控：E1 真的持锁，且 pg_locks 能看到 ----
  REQUIRE(first.value()->TryAcquire().value());
  REQUIRE_FALSE(LockHolderPid(*observer.value(), key).empty());

  //  ---- ② 负控：同键的另一个实例不得自称 leader ----
  REQUIRE_FALSE(second.value()->TryAcquire().value());
  REQUIRE_FALSE(second.value()->IsLeader());
  REQUIRE(first.value()->IsLeader());

  //  ---- ③ 从另一条连接切断持锁后端（= PG 重启 / 主备切换 / 网络断开）----
  const std::string holder_pid = LockHolderPid(*observer.value(), key);
  REQUIRE_FALSE(holder_pid.empty());
  //  防注入：传给 pg_terminate_backend 的必须是纯数字（单条断言 ⇒ 断言条数确定）
  const bool pid_is_numeric = holder_pid.find_first_not_of("0123456789") == std::string::npos;
  REQUIRE(pid_is_numeric);
  REQUIRE(observer.value()->ExecSimple("SELECT pg_terminate_backend(" + holder_pid + ")").ok());

  //  ---- ④ 修复的核心判据：有限时间内重新当选（老代码在这里永远 false）----
  const long recovered_ms =
      PollUntil([&] { return first.value()->IsLeader(); }, std::chrono::milliseconds(15000));
  INFO("重新当选耗时(ms) = " << recovered_ms << "（-1 = 超时未恢复）");
  REQUIRE(recovered_ms >= 0);
  REQUIRE_FALSE(LockHolderPid(*observer.value(), key).empty());  // 锁真的回来了

  //  ---- ⑤ 恢复之后仍然只有一个 leader ----
  REQUIRE_FALSE(second.value()->IsLeader());

  //  ---- ⑥ 收尾：释放后不留脏锁 ----
  REQUIRE(first.value()->Release().ok());
  const long released_ms =
      PollUntil([&] { return LockHolderPid(*observer.value(), key).empty(); },
                std::chrono::milliseconds(5000));
  REQUIRE(released_ms >= 0);
}
