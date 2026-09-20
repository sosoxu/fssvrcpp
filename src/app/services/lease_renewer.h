// =============================================================================
//  LeaseRenewer（L4）—— 长耗时复制/校验和期间的在途租约**续租器**
// =============================================================================
//  设计依据：ADR-009 §4.2/§4.3（在途上传必须持有租约；GC 只回收"租约到期且被原子
//  领取"的对象）。`GetUploadLocation` 在发地址时 `Acquire` 了租约，但随后的
//  staging→persistent 复制 + 校验和可能超过 TTL —— 没有续租，租约会在复制**进行中**
//  到期，GC 就可能把它当成"崩溃者留下的孤儿"回收（数据丢失）。本类就是那道护栏。
//
//  ★ 纪律（AGENTS §4.3 的"提前 return 留下 joinable thread → terminate"）
//    · 构造即启动（`enabled=false` 时**不创建任何线程**）；
//    · 析构 / `Stop()` 统一 `stop + notify + join`，**任何退出路径**都不会留下
//      joinable 线程（`Stop()` 幂等）；
//    · 线程创建失败（资源耗尽）不抛出：捕获 `std::system_error` 记进 `last_error()`，
//      调用方据此 fail-closed（绝不因线程创建失败而 terminate/139）。
//
//  ★ 失败语义（本切片的判断记录）
//    · 线程每轮 `Renew`；**成功**清空错误、**失败**记录**最近一次**错误。
//    · 瞬时错误（如 PG 抖动）会在下一轮成功时自愈，调用方在**步骤边界**看到的是
//      "最近一次续租是否成功"；持续失败（租约被抢 / 表被删）会一直保留错误 →
//      调用方 fail-closed。这与"绝不静默忽略续租失败"的要求一致。
//    · 本次续租是**同步**调用（在后台线程里），不与用例线程共享可变状态（除本类的
//      mutex 保护的错误字段）。
// =============================================================================
#pragma once

#include "common/result/result.h"
#include "domain/ports/ports.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace fss::app {

class LeaseRenewer {
 public:
  struct Options {
    //  ★ `false` → 不创建线程（单实例 / `leases.enabled=false` 的行为**逐字不变**）。
    bool enabled = false;
    std::int64_t ttl_millis = 60000;             // `leases.ttl_seconds * 1000`
    std::int64_t renew_interval_millis = 20000;  // `leases.renew_interval_seconds * 1000`
  };

  LeaseRenewer(domain::ILeaseRepository& leases, std::string partition, std::string lease_key,
               std::string instance_id, Options options);
  ~LeaseRenewer();
  LeaseRenewer(const LeaseRenewer&) = delete;
  LeaseRenewer& operator=(const LeaseRenewer&) = delete;

  //  线程是否真的在跑。`enabled=false` 或线程创建失败 → false。
  bool active() const;
  //  最近一次续租是否失败（空 = 至今全部成功 / 未启用）。线程安全。
  std::optional<fss::Error> last_error() const;
  bool failed() const;

  //  幂等停止：置位 + 唤醒 + join。析构也会调用（先 `Stop()` 再 `join()` 的统一退出路径）。
  void Stop();

 private:
  void Loop();

  domain::ILeaseRepository& leases_;
  std::string partition_;
  std::string lease_key_;
  std::string instance_id_;
  Options options_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::optional<fss::Error> last_error_;
  std::thread thread_;
};

}  // namespace fss::app
