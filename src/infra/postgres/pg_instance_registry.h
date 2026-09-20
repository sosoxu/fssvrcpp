// =============================================================================
//  instance_registry —— 实例心跳与"配置/版本一致性"（L2，`src/infra/postgres/`）
// =============================================================================
//  为什么需要这个模块（ADR-009 §5.3 / §8.1 item 4，B2b 切片）
//  ---------------------------------------------------------------------------
//  `deployment.mode=multi`（以及 single + PG 仓储）下所有实例共享同一个数据库。
//  两类"共享状态不一致"必须被 readiness 抓住，否则 LB 会把流量发给一个与其它实例
//  语义不同的进程：
//    ① **滚动升级**：新旧实例同时在线、配置或版本不同。旧实例用旧 schema/旧语义
//       写共享状态，新实例按新语义读 —— 这类问题不会在启动时报错，只在运行期
//       表现为偶发丢数据/约束冲突（ADR-009 §5.3）。
//    ② **存储不是共享挂载**：multi 的前提是 `storage.posix.root` 落在所有实例都能
//       看到的挂载上。若各实例看到的是本地目录，`.tmp.*` 改名、GC 扫描、租约回收
//       全部形同虚设（ADR-009 §8.1 item 4）。
//
//  本模块只负责**注册表侧的读写**（表由 `db/migrations/001_init.sql` 创建）：
//    · 启动时 upsert 本实例行（`instance_id` / `service_version` / `config_hash` /
//      `started_at=now()` / `heartbeat_at=now()`）；
//    · 心跳：`heartbeat_at=now()`（由组合根的后台线程每 10s 调用一次）；
//    · 列出**live peer**：`heartbeat_at > now() - liveness`（liveness = 3× 心跳周期）；
//    · 退出时删除自己的行；启动时清理**过老**的行（不清理别的 live 实例）。
//  "探针文件是否可见"属于文件系统（`infra/blob/posix/shared_mount_probe.h`）；
//  组合根把两者拼成同一个 readiness 判据（`ports.shared_state_probe`）。
//
//  ★ 连接预算（C9.28）：本模块**不建自己的连接池**，而是复用组合根已有的
//    "共享状态池"（元数据池优先，否则位置池）。因此"每实例最坏连接数"的公式与
//    B2a 完全一致 —— 心跳/一致性检查不会悄悄吃掉 PG 的 `max_connections`。
//
//  ★ SQL 纪律：本表**没有 partition 维度**（它是实例级共享状态，不是租户数据），
//    因此按 `pg_connection.cpp` / `pg_schema.cpp` 的先例用独立的 `R"pgsql(...)pgsql"`
//    原始串 —— C3.9 的 partition_id 扫描器只认 `R"sql(...)sql"`，让它看见这些语句
//    会产生**假阳性**。（本文件不是 `*_repository.*`，不受定界符护栏约束。）
//
//  ★ PG 版本兼容：目标库是 **12.6**。`INSERT ... ON CONFLICT`（9.5+）、
//    `now() - ($n::int * INTERVAL '1 second')` 都可用；**不**依赖任何 13+ 特性。
// =============================================================================
#pragma once

#include "common/result/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fss::infra {

class PgPool;

//  注册表里的一行（只取一致性判定需要的列）。
struct InstancePeer {
  std::string instance_id;
  std::string service_version;
  std::string config_hash;
};

// -----------------------------------------------------------------------------
//  版本兼容规则（判定"滚动升级"）
// -----------------------------------------------------------------------------
//  取 `major.minor`（去掉 `-rc1` / `+build` 后缀）比较：**前两段相等 = 兼容**，
//  第三段（patch）允许不同 —— 这正是滚动升级允许"同 minor 内换 patch"的语义；
//  一旦 major/minor 不同，新旧实例的语义可能已经分叉 ⇒ **不兼容**。
//  若任一侧不是纯数字点分版本（例如构建未注入版本），退化为**逐字相等**比较，
//  宁可比对方更严（fail-closed），也不放过"看起来相等其实不是"的情况。
bool ServiceVersionCompatible(std::string_view self, std::string_view peer);

//  把"这个 live peer 与本实例不一致"渲染成**给运维看的可读原因**；
//  一致（或兼容）→ 返回空串。原因里必须出现 peer 的 `instance_id` 与"差在哪"。
std::string PeerConsistencyReason(const InstancePeer& peer,
                                  std::string_view self_service_version,
                                  std::string_view self_config_hash);

// -----------------------------------------------------------------------------
//  PgInstanceRegistry —— 复用组合根的共享状态连接池（不拥有它）
// -----------------------------------------------------------------------------
class PgInstanceRegistry {
 public:
  PgInstanceRegistry(PgPool& pool, std::string instance_id, std::string service_version,
                     std::string config_hash);
  //  ★ RAII 注销：**每条退出路径**（正常 SIGTERM、启动期 exit 78、未捕获异常）都必须
  //    把自己的行删掉，否则"半截注册"会作为 live peer 干扰别的实例。析构里只做
  //    尽力而为（绝不抛异常、绝不让清理失败掩盖真正的原因）；连接池的生命周期
  //    由组合根保证长于本对象。
  ~PgInstanceRegistry();
  PgInstanceRegistry(const PgInstanceRegistry&) = delete;
  PgInstanceRegistry& operator=(const PgInstanceRegistry&) = delete;

  //  `INSERT ... ON CONFLICT (instance_id) DO UPDATE`：首次写入 `started_at`，
  //  之后只刷新版本/配置/心跳（`started_at` 保留"本进程首次注册"的时刻）。
  fss::Result<void> UpsertSelf();

  //  `heartbeat_at=now()`。若行不存在（被别的实例的"过老清理"删掉、或测试手工删除），
  //  自动重新 upsert，避免"心跳一直失败 → 永久 not ready"。
  fss::Result<void> TouchHeartbeat();

  //  当前 **live** peer（`heartbeat_at` 在 `liveness_seconds` 内；不含本实例），
  //  按 `instance_id` 排序（判定顺序确定 ⇒ 原因稳定）。
  fss::Result<std::vector<InstancePeer>> ListLivePeers(int liveness_seconds) const;

  //  删除本实例的行（优雅退出路径；失败只应记录，不应让退出挂住）。
  fss::Result<void> RemoveSelf();

  //  删除心跳**过老**的行（含别的实例；返回删除行数）。阈值远大于 liveness 窗口，
  //  保证"只是晚了几个心跳周期"的实例不会被误删；崩溃实例的行最终会被幸存者清掉。
  fss::Result<int> CleanupStale(int stale_seconds);

  const std::string& instance_id() const { return instance_id_; }
  const std::string& service_version() const { return service_version_; }
  const std::string& config_hash() const { return config_hash_; }

 private:
  PgPool& pool_;
  std::string instance_id_;
  std::string service_version_;
  std::string config_hash_;
};

}  // namespace fss::infra
