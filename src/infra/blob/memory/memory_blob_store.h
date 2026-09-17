// =============================================================================
//  InMemoryBlobStore（L2，测试用）—— IBlobStore 的内存实现 + 可控故障注入
// =============================================================================
//  为什么需要它
//    · P2 的目标是"用内存实现驱动全部应用层逻辑"（docs/04-implementation-plan.md 阶段 2）；
//    · 它是**契约测试基准**（ADR-003 §后果）：memory / POSIX / S3 共用
//      `tests/framework/port_contract.h` 的同一套断言；
//    · P6 的故障注入门槛（C6.3，6 个故障点）需要一个"能按需失败/延迟/截断"的存储，
//      否则只能靠真实磁盘打满/断网来制造故障 —— 又慢又不可重复。
//
//  ★ 语义（与 port_contract.h 顶部的登记表逐条对应）
//    · container 必须显式 `ensure_container`（不隐式建桶 —— S3 做不到，契约取交集）
//    · stat(缺失) → Ok + exists=false；get(缺失) → kNotFound；remove(缺失) → Ok
//    · 区间读：offset >= size → kInvalidArgument；末端超界 → 截断
//    · 时间来自注入的 `IClock`（默认 SystemClock 由组合根给；测试用 ManualClock）——
//      契约测试要求"无真实睡眠"（C2.8）就必须能控制时间
//
//  ★ 故障注入只属于本实现（不属于 IBlobStore 契约）：契约基类不得依赖它，
//    否则 memory 的测试就无法套到 POSIX/S3 上。
#pragma once

#include "common/result/result.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace fss::infra {

class InMemoryBlobStore final : public domain::IBlobStore {
 public:
  // 可注入故障的操作（与端口的 9 个方法一一对应）
  enum class Op {
    kEnsureContainer = 0,
    kPresignPut,
    kPresignGet,
    kPut,
    kGet,
    kStat,
    kRemove,
    kCopy,
    kList,
    kCount,
  };
  static constexpr std::size_t kOpCount = static_cast<std::size_t>(Op::kCount);
  static const char* OpName(Op op) noexcept;

  struct FaultPlan {
    Op op = Op::kPut;
    // 让接下来 `fail_count` 次该操作失败（<=0 = 不注入错误）
    int fail_count = 0;
    fss::ErrorKind error = fss::ErrorKind::kUnavailable;
    std::string message = "injected fault";
    // put 专用：只存前 N 字节却**返回成功**（>=0 生效）—— 模拟"静默截断"。
    // 这正是契约 §1.7 与 C1.2 的 H-2 要防的失败模式，P6 用它做对照。
    std::int64_t truncate_put_to_bytes = -1;
    // stat 返回的校验和被替换（非空生效）—— 模拟"存储侧校验和不可信"
    std::string checksum_override;
    // 每次该操作"本应等待"的毫秒数。**不真的 sleep**（C2.8 禁止真实睡眠），
    // 只累计到可观测计数；需要真实等待的测试由调用方消费该计数后自行决定。
    std::int64_t latency_millis = 0;
  };

  explicit InMemoryBlobStore(const fss::IClock& clock) : clock_(clock) {}

  // ---- IBlobStore ----
  domain::BlobCapabilities capabilities() const override;
  fss::Result<void> ensure_container(const std::string& container) override;
  fss::Result<domain::SignedLocation> presign_put(
      const domain::ObjectRef& ref, const domain::PresignOptions& options) override;
  fss::Result<domain::SignedLocation> presign_get(
      const domain::ObjectRef& ref, const domain::PresignOptions& options) override;
  fss::Result<void> put(const domain::ObjectRef& ref, bytes::ByteSource& source,
                        const domain::PutOptions& options) override;
  fss::Result<void> get(const domain::ObjectRef& ref, bytes::ByteSink& sink,
                        const domain::ByteRange& range) override;
  fss::Result<domain::ObjectStat> stat(const domain::ObjectRef& ref) override;
  fss::Result<void> remove(const domain::ObjectRef& ref) override;
  fss::Result<domain::ObjectStat> copy(const domain::ObjectRef& from,
                                       const domain::ObjectRef& to) override;
  fss::Result<domain::ListPage> list(const std::string& container, const std::string& prefix,
                                     const std::string& continuation_token, int limit) override;

  // ---- 故障注入（仅测试用） ----
  void Inject(const FaultPlan& plan) { fault_ = plan; }
  void ClearFaults() { fault_ = FaultPlan{}; }
  // 取走累计的"本应延迟"毫秒数（取走后归零）
  std::int64_t TakeInjectedLatencyMillis();
  int OpCount(Op op) const { return op_counts_[static_cast<std::size_t>(op)]; }
  std::size_t object_count() const;
  bool HasContainer(const std::string& container) const;

  // 单对象上限：内存实现不可能无限大，显式给出上限而不是让它 OOM
  static constexpr std::size_t kMaxObjectBytes = std::size_t{1} << 30;  // 1 GiB

 private:
  struct Object {
    std::string data;
    std::string content_type;
    std::string checksum;             // 小写十六进制 SHA-256
    std::string checksum_algorithm;   // "SHA256"
    std::int64_t last_modified_epoch_seconds = 0;
  };

  // 每个操作的第一步：计数 + 累计"延迟" + 按计划注入错误
  fss::Result<void> BeginOp(Op op);
  static fss::Result<void> ValidateRef(const domain::ObjectRef& ref);

  using Objects = std::map<std::string, Object>;  // key → object（std::map 保证字典序）
  std::map<std::string, Objects> containers_;

  const fss::IClock& clock_;
  FaultPlan fault_{};
  std::array<int, kOpCount> op_counts_{};
  std::int64_t injected_latency_millis_ = 0;
};

}  // namespace fss::infra
