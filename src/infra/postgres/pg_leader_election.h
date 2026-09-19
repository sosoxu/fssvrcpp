// =============================================================================
//  PgLeaderElection（L2，`src/infra/postgres/`）—— 基于 PG 会话级 advisory lock 的领导者选举
// =============================================================================
//  作用：**单例后台任务**（本项目 = GC）在多实例下只能有一个实例在跑。ADR-009 §4.4
//  明确选型：PostgreSQL **会话级** advisory lock（`pg_try_advisory_lock`），
//  不引入 etcd/consul，也不用 NFS 文件锁（NFS 的 `flock`/`fcntl` 语义历来脆弱）。
//
//  ★★ 为什么锁必须有一条**专用** `PgConnection`（不从数据 `PgPool` 借）
//  ---------------------------------------------------------------------------
//  会话级 advisory lock 与"持锁会话的生死"绑定：会话结束（含进程被 kill）才释放。
//  若锁连接从数据池借出，池在归还时可能因 `PQstatus != CONNECTION_OK` **销毁**连接
//  （`PgPool::Return` 的既定语义）→ 锁被静默释放，而进程以为锁还在；反过来，
//  池也绝不能把"持锁的连接"再次借给别处（锁被复用/被无意释放）。因此：
//    · 锁连接由本类独占（不经过池、不归还给池）；
//    · 锁连接**只跑短语句**（见下）；
//    · 数据路径的故障与锁的生死完全解耦（丢失锁连接不会波及数据连接）。
//
//  ★★ 为什么"只跑短语句"是**实测要求**，不是风格偏好（目标引擎 PG 12.6）
//  ---------------------------------------------------------------------------
//  PG 12.6 **没有** `client_connection_check_interval`（14+ 才引入），因此"客户端消失"
//  只能在下一次读写时被发现。实测（`docs/test-evidence/phase10.md` §15.9）：
//     · 空闲持锁会话被 `kill -9`：锁 **51 ms** 释放（PG 14.24：9 ms）；
//     · 持锁会话正在跑一条 **15 s 单语句**：目标库 **≥8 s 仍未释放**，语句结束后才释放
//       （PG 14 因客户端消失侦测 633 ms 释放）。
//  滞留窗口 = **当前语句的剩余时长**。⇒ 锁连接上的每条语句都必须短，并受
//  `statement_timeout` 约束（`PgConnection::Connect` 已按 `metadata.postgres.statement_timeout_ms`
//  下发 `SET statement_timeout`），把"崩溃后锁滞留窗口"夹到 `statement_timeout_ms` 以内。
//  这也是 `db/tests/002_advisory_lock.sh` 的 holder **已改成**"短语句心跳"的原因：改前它用
//  一条 120 s 的 `DO ... LOOP pg_sleep()`，在 PG 12.6 上 A3 会（正确地）判失败（父代理
//  实测并修掉，证据见 `docs/test-evidence/phase10.md` §15.9 与 §17.8）。
//
//  ★★ `IsLeader()` 必须**验证连接仍然健康**（不能返回缓存 bool）
//  ---------------------------------------------------------------------------
//  `PQstatus()` 在 TCP 对端消失后可能仍报 `CONNECTION_OK`（libpq 只在下一次 I/O 时
//  才发现）。因此本实例的判据是：
//    · 已是 leader → 做一次**廉价往返**（`SELECT 1`）；失败即**主动降级**（`leader_ = false`），
//      使本实例不再跑单例任务；
//    · 不是 leader → 每次调用都尝试 `TryAcquire()`（一条短语句）。这样当前 leader
//      崩溃、PG 释放其会话锁之后，**其它实例能在下一轮自动接管**（否则"选举"只是
//      启动期一次性动作，没有 failover）。
//
//  ★ 锁键：`leader_election.lock_key`（int 0..2147483647），交给
//    `pg_try_advisory_lock($1::bigint)` / `pg_advisory_unlock($1::bigint)`。
//    PostgreSQL 的**单参数 bigint** 版本是会话级锁（两参数 int,int 版本也是会话级；
//    这里统一用单参数版本，键值就是配置值）。
//
//  ★ SQL 纪律（机械护栏 tests/unit/test_sql_guardrail.cpp）
//    advisory lock 语句**不访问任何分区表**，因此用独立的 `R"pglock(...)pglock"`
//    原始串定界符写（与 `PgConnection::NowEpochMillis` 用 `R"pgsql(...)pgsql"` 同一理由：
//    C3.9 的启发式只认 `R"sql(...)sql"` 并要求每条 DML 带 `partition_id`，而这条语句
//    本来就没有分区维度 —— 换定界符是为了不产生**假阳性**，不是为了绕过检查）。
// =============================================================================
#pragma once

#include "common/result/result.h"
#include "infra/postgres/pg_connection.h"

#include <cstdint>
#include <memory>
#include <string>

namespace fss::infra {

//  ★ `pg` 是第一个成员（与配置组 `metadata.postgres.*` 的字段顺序一致）。
//    `max_connections` 对本类**无意义**（本类只用一条专用连接），保留它只是因为
//    复用同一个 `PgOptions` 结构体；`Open` 会照常走 `ValidatePgOptions`。
struct PgLeaderElectionOptions {
  PgOptions pg;
  //  会话级 advisory lock 的键（`leader_election.lock_key`）。
  std::int64_t lock_key = 1179865927;
};

class PgLeaderElection {
 public:
  //  建立**专用**锁连接（`PgConnection::Connect`，DSN 不可达 → `Err(kUnavailable,
  //  <libpq 消息>)`，fail-closed，绝不回退）。**不**获取锁 —— 获取由 `TryAcquire()` 完成。
  static fss::Result<std::unique_ptr<PgLeaderElection>> Open(
      PgLeaderElectionOptions options);

  ~PgLeaderElection();
  PgLeaderElection(const PgLeaderElection&) = delete;
  PgLeaderElection& operator=(const PgLeaderElection&) = delete;

  //  获取锁（恰好一条短语句：`SELECT pg_try_advisory_lock($1::bigint)`）。
  //  `true` = 本实例成为 leader。已持有时**不要**重复调用（会话级锁可重入，
  //  重复获取会让 `Release()` 的解锁次数与获取次数不匹配）。
  fss::Result<bool> TryAcquire();

  //  释放锁（恰好一条短语句：`SELECT pg_advisory_unlock($1::bigint)`）。
  //  幂等：未持有时也返回成功（但仍会执行一次解锁语句，PostgreSQL 会回 false）。
  fss::Result<void> Release();

  //  本实例现在是否**真的**是 leader：见文件头的两分支语义（验证健康 / 尝试接管）。
  //  ★ 非 const：健康检查是一次真实往返，且失败会主动降级（写入 `leader_`）。
  bool IsLeader();

  std::int64_t LockKey() const { return options_.lock_key; }

  //  与其它 L2 适配器同风格的就绪判定（连接建立成功 → Ready()）。
  bool Ready() const;
  std::string NotReadyReason() const;

 private:
  PgLeaderElection() = default;

  PgLeaderElectionOptions options_{};
  //  专用锁连接（不从 `PgPool` 借，见文件头）。
  std::unique_ptr<PgConnection> connection_;
  bool leader_ = false;
  std::string last_error_;
};

}  // namespace fss::infra
