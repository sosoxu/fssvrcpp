// =============================================================================
//  IIdGenerator / UuidGenerator / SequentialIdGenerator（L1）
// =============================================================================
//
//  为什么在 L1 而不是 L3 的 ports/：与 IClock 同理 —— 无依赖、无领域语义。
//  设计文档 §5.2 的端口表已标注声明位置。
//
//  两个生成器：
//    * UuidGenerator        —— 生产用。UUIDv4（RFC 4122），熵来自 OpenSSL RAND_bytes。
//    * SequentialIdGenerator —— 测试用。产出**确定性** ID，让断言可写死。
//
//  ★ 关于 fileID 的格式：OSDU 的 fileID 必须匹配 `^[\w,\s-]+(\.\w+)?$`
//    （契约 §1.5），而 UUID 的连字符是 `\w` 允许的字符，因此直接可用。
//    记录 ID 的另一层格式（`<partition>:dataset--File.Generic:<uuid 去横线>`）
//    属于**领域规则**，由应用层的 ObjectKeyPolicy 组装，不在 L1 这里。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace fss {

class IIdGenerator {
 public:
  virtual ~IIdGenerator() = default;
  // 标准 UUID 字符串（36 字符，含 4 个连字符）
  virtual std::string NewUuid() const = 0;
  // 去掉连字符的 32 字符十六进制（OSDU 的记录 ID 用这种形式）
  virtual std::string NewUuidNoDash() const = 0;
};

class UuidGenerator final : public IIdGenerator {
 public:
  std::string NewUuid() const override;
  std::string NewUuidNoDash() const override;
};

// 测试用：确定性、可预期
//  ★ `next_` 是**原子**的：并发用例（如 P9 的 100 并发 JSON）会在多个服务线程里同时取 id，
//    普通 `uint64_t` 会让两个请求拿到同一个 id —— 在"按 id 建唯一约束"的仓储上表现为
//    偶发的唯一键冲突（与 P6 把 `ManualClock` 改成原子同一条理由）。
class SequentialIdGenerator final : public IIdGenerator {
 public:
  explicit SequentialIdGenerator(std::uint64_t start = 1) : next_(start) {}
  std::string NewUuid() const override;          // 00000000-0000-4000-8000-<12 位递增>
  std::string NewUuidNoDash() const override;    // <32 位递增>
  void Reset(std::uint64_t start = 1) { next_.store(start); }

 private:
  mutable std::atomic<std::uint64_t> next_;
};

}  // namespace fss
