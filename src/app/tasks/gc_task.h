// =============================================================================
//  GcTask（L4）—— 超期 staging 对象 / 孤儿对象 / 过期租约的回收
// =============================================================================
//  设计依据：`docs/02-design.md` §9.3（回收表）、ADR-009 §3 M3、契约里的 GC 配置项。
//
//  ★★ 铁律：**禁止"无元数据记录即删"**。
//     ADR-009 M3 实测：多实例下按"没有元数据记录"判定 → **20/20 误删在途上传**
//     （上传请求已经落盘、元数据还没写，这个窗口是所有并发上传的必经状态）。
//     多实例路径只能是"**租约到期 + 原子领取**"：
//       ① `ClaimExpired()` 原子领取（并发 GC 下同一条租约只会被一个 GC 领走）；
//       ② 领到之后仍然要**再看一次元数据记录** —— 有记录的对象**永不删除**（纵深防御）。
//     `GcOptions::require_lease_expiry = false` 是**单实例**的显式降级（按 TTL 判定），
//     多实例下被启动校验强制为 true（`gc.require_lease_expiry`，见 config schema）。
//
//  ★ 默认 `dry_run = true`：只报告候选、不删除。生产要显式开启（`gc.dry_run=false`）。
//
//  ⚠️ 未做（如实登记，见 `docs/test-evidence/phase6.md` 与计划 C9.25）：
//     · `.tmp_*` 残留文件的识别与清理（需要 `IBlobStore` 暴露"列出内部键"，属于 P9 C9.25）；
//     · 过期 transfer token 的清理（token 目前是自签、无状态，无表可清）；
//     · 调度（`gc.interval_seconds`）、领导者选举与指标暴露（P9 C9.26 与交付项）。
// =============================================================================
#pragma once

#include "app/usecases/usecases.h"
#include "common/metrics/metrics.h"
#include "common/result/result.h"
#include "domain/ports/ports.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fss::app {

//  与 `config/fss.example.json` 的 `gc.*` 一一对应
struct GcOptions {
  bool dry_run = true;                  // `gc.dry_run`：默认只记录不删除
  bool require_lease_expiry = true;     // `gc.require_lease_expiry`：多实例必须 true
  std::int64_t staging_ttl_hours = 24;  // `gc.staging_ttl_hours`
  std::int64_t orphan_grace_hours = 72; // `gc.orphan_grace_hours`
  int claim_limit = 100;                // 单轮最多领取多少条过期租约
};

//  一轮 GC 的结果。既给测试断言，也给 `/metrics`（P9 暴露）。
struct GcReport {
  bool dry_run = true;
  std::int64_t expired_leases_claimed = 0;  // 本轮原子领取到的过期租约数
  std::int64_t deleted_objects = 0;
  std::int64_t deleted_locations = 0;
  std::int64_t skipped_has_record = 0;    // ★ 有元数据记录 → 永不删
  std::int64_t skipped_no_location = 0;   // 租约指向的位置记录已经没了
  std::int64_t skipped_too_young = 0;     // 还没到 TTL / 宽限期
  //  ★ C9.25：`.tmp_*` 的清理计数（**绝不**把临时文件当成有效对象）
  std::int64_t tmp_removed = 0;
  std::int64_t tmp_skipped_too_young = 0;
  std::int64_t tmp_skipped_unknown_mtime = 0;  // 取不到 mtime → 保守保护（P9-D04 的邻居）
  std::int64_t errors = 0;
  std::vector<std::string> deleted_file_ids;  // 排障与"不重复删"断言用
};

class GcTask {
 public:
  //  `registry` 可选（P9/C9.6）：非空时把 GC 的扫描/删除/跳过计进 `/metrics`
  GcTask(UseCasePorts& ports, domain::ILeaseRepository& leases, std::string instance_id,
         fss::metrics::Registry* registry = nullptr)
      : ports_(ports),
        leases_(leases),
        instance_id_(std::move(instance_id)),
        registry_(registry) {}

  //  `partition` 必填：所有仓储访问都必须带 partition（R6/R9 的护栏）。
  //
  //  ★ C9.31：**单飞护栏**（single-flight）是 `Run` 的**通用**性质，不是 HTTP 层的：
  //    周期调度（组合根的 `GcScheduler`）与按需端点（`ops.gc_run`）共享同一个 `GcTask`。
  //    · 已经在跑时**不排队、不并行**：`try_lock` 立刻失败 → `kUnavailable`（HTTP 503），
  //      并带一条可读消息。排队会让"磁盘满时点一下"变成"等上一轮一小时的扫描结束"，
  //      并行则会让两轮 GC 同时扫同一份目录（删除数/跳过数互相污染，指标失真）。
  //    · 用互斥量的 `try_lock`（而不是原子标志）是**为了 RAII**：任何提前 return / 异常
  //      都会在 `unique_lock` 析构时释放，不会留下"永久卡住"的假锁。
  Result<GcReport> Run(std::string_view partition, const GcOptions& options = {});

 private:
  //  ① 租约过期路径（多实例的唯一正路）
  void CollectExpiredLeases(std::string_view partition, const GcOptions& options,
                            domain::IBlobStore& staging, domain::IBlobStore& persistent,
                            std::int64_t now_seconds, GcReport& report);
  //  ② 单实例降级：按 staging TTL 扫描（`require_lease_expiry=false`）
  void CollectExpiredStaging(std::string_view partition, const GcOptions& options,
                             domain::IBlobStore& staging, std::int64_t now_seconds,
                             GcReport& report);
  //  ③ persistent 孤儿：没有任何位置记录引用它，且超过宽限期
  void CollectOrphanObjects(std::string_view partition, const GcOptions& options,
                            domain::IBlobStore& persistent, std::int64_t now_seconds,
                            GcReport& report);
  //  删对象（+ 可选的位置记录）；`dry_run` 时只计数
  void DeleteCandidate(const GcOptions& options, domain::IBlobStore& store,
                       const domain::ObjectRef& ref, std::string_view file_id,
                       bool delete_location, std::string_view partition, GcReport& report);
  //  某个 FileSource 是否已经有元数据记录（有 → 永不删）
  bool HasMetadataRecord(std::string_view partition, std::string_view file_source);
  //  (container,key) → (file_id, file_source) 的**全量**映射。
  //  ★ 必须分页取全量：只取第一页会把"其实被引用"的对象误判成孤儿 → 删掉活数据。
  Result<std::map<std::string, std::pair<std::string, std::string>>> ReferencedRefs(
      std::string_view partition);

  UseCasePorts& ports_;
  domain::ILeaseRepository& leases_;
  std::string instance_id_;
  fss::metrics::Registry* registry_ = nullptr;
  //  ★ C9.31 的单飞护栏（见 `Run` 的注释）。`mutable` 不需要：`Run` 非 const。
  std::mutex run_mutex_;
};

}  // namespace fss::app
