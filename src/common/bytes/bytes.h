// =============================================================================
//  bytes（L1）：Range 归一化、字节流抽象、缓冲、限流
// =============================================================================
//
//  为什么要单独一个模块
//    ① **Range 归一化是被真实缺陷驱动的**：cpp-httplib 的越界 Range 会算出下溢的
//       `Content-Length`（ADR-002 的 H-1）。修它需要一份"我们自己的、可独立测试的"
//       Range 语义，而不是把判断散落在 handler 里。
//    ② 数据面（P3/P9）需要"从哪读、往哪写"的抽象，且**不得整文件驻留内存**
//       （C1.3 要求 ≥1 GiB 虚拟文件流式收发期间 RSS 增长 < 64 MiB）。
//    ③ 上传的体积上限（C1.2 的 H-2 三重防护之一）需要一个"计数 reader"：
//       超限立即中止，而不是读完再判断。
//
//  ⚠️ Range 语义的权威是 RFC 7233（§2.1 语法、§4.4 416 条件），
//     本实现与 `docs/03-api-contract.md` §1.8 的表逐条对应。
#pragma once

#include "common/result/result.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fss {

class IClock;

namespace bytes {

// =============================================================================
//  一、Range
// =============================================================================
//  闭区间 [first, last]，与 HTTP `Content-Range: bytes first-last/total` 一致。
struct ByteRange {
  std::int64_t first = 0;
  std::int64_t last = 0;  // **含**端点

  std::int64_t Length() const { return last - first + 1; }
  bool operator==(const ByteRange& o) const { return first == o.first && last == o.last; }
};

enum class RangeStatus {
  kAbsent,         // 没有 Range 头 → 200 + 全量
  kSingle,         // 1 个可满足区间 → 206
  kMultiple,       // ≥2 个可满足区间 → 206 multipart/byteranges
  kUnsatisfiable,  // 语法合法但全部不可满足 → 416（`Content-Range: bytes */total`）
  kMalformed,      // 语法非法/无法理解 → **忽略该头**，按 200 全量处理（RFC 7233 §4.4）
};

const char* RangeStatusName(RangeStatus status);

struct RangeResult {
  RangeStatus status = RangeStatus::kAbsent;
  std::vector<ByteRange> ranges;  // status ∈ {kSingle,kMultiple} 时非空
  std::string detail;             // 排障/日志用（不发给客户端）

  bool IsPartial() const {
    return status == RangeStatus::kSingle || status == RangeStatus::kMultiple;
  }
};

// 解析 `Range: bytes=...`。
//   total_size < 0        → 未知长度：只做**语法**校验，不做可满足性判定
//                            （此时无法判断越界，调用方需自行处理）
//   total_size == 0       → 任何区间都不可满足
// 上限：单次最多解析 kMaxRanges 个区间，超过按 kMalformed 处理（避免构造巨型 multipart）
inline constexpr std::size_t kMaxRanges = 32;
RangeResult ParseRangeHeader(std::string_view header_value, std::int64_t total_size);

// `Content-Range: bytes 0-499/1234`；不可满足时是 `bytes */1234`
std::string FormatContentRange(const ByteRange& range, std::int64_t total_size);
std::string FormatUnsatisfiedContentRange(std::int64_t total_size);

// 每段 multipart/byteranges 的头部（不含 body）
std::string FormatMultipartRangeHeader(std::string_view boundary, const ByteRange& range,
                                      std::int64_t total_size, std::string_view content_type);

// =============================================================================
//  二、ByteSource / ByteSink
// =============================================================================
//  源是**拉**模式（Read），汇是**推**模式（Write）：这样"发送 1 GiB 文件"不需要
//  把文件放进内存 —— 上层用固定大小的缓冲循环搬运即可（C1.3 的 RSS 上限）。
class ByteSink;

class ByteSource {
 public:
  virtual ~ByteSource() = default;

  // 读最多 `capacity` 字节到 `out`。返回实际读到的字节数；**0 表示 EOF**。
  // 不保证读满（短读是允许的）；需要读满时用 ReadFully。
  virtual Result<std::size_t> Read(char* out, std::size_t capacity) = 0;

  // 已知总长度时给出（未知返回 nullopt）
  virtual std::optional<std::int64_t> Size() const { return std::nullopt; }

  // 随机访问（可选）：HTTP `Range`（尤其是**多段**）要求能按偏移读。
  // 默认不支持 —— 不支持的来源只能做"前向跳过"，且多段请求会被上层降级为全量。
  virtual bool Seekable() const { return false; }
  virtual Result<void> Seek(std::int64_t offset) {
    (void)offset;
    return Err(ErrorKind::kUnimplemented, "该来源不支持随机访问");
  }

  // 读满 n 字节；不足则报 `kUnavailable`（只在"长度已承诺"的场景用）
  Result<void> ReadFully(char* out, std::size_t n);
  // 读到 EOF；超过 max_bytes 立即报错（不做无界累积）
  Result<std::string> ReadAll(std::size_t max_bytes);
  // 以固定缓冲循环搬运到 sink（默认 64 KiB，RSS 与文件大小无关）
  Result<std::uint64_t> PumpTo(ByteSink& sink, std::size_t buffer_size = 64 * 1024);
};

class ByteSink {
 public:
  virtual ~ByteSink() = default;

  virtual Result<void> Write(std::string_view data) = 0;
  // 幂等收尾；默认无操作
  virtual Result<void> Close() { return Ok(); }
};

// ---------------- 内存实现（测试与小块数据） ----------------
class StringSource final : public ByteSource {
 public:
  explicit StringSource(std::string data) : data_(std::move(data)) {}
  Result<std::size_t> Read(char* out, std::size_t capacity) override;
  std::optional<std::int64_t> Size() const override {
    return static_cast<std::int64_t>(data_.size());
  }
  bool Seekable() const override { return true; }
  Result<void> Seek(std::int64_t offset) override;
  std::string_view view() const { return data_; }

 private:
  std::string data_;
  std::size_t pos_ = 0;
};

class StringSink final : public ByteSink {
 public:
  Result<void> Write(std::string_view data) override;
  Result<void> Close() override;
  const std::string& str() const { return data_; }
  std::size_t bytes_written() const { return data_.size(); }

 private:
  std::string data_;
  bool closed_ = false;
};

// 丢弃全部数据但计数（用于"只测吞吐/长度"的场景）
//  写进调用方提供的**固定缓冲**（L1 通用件）。
//  ★ 用途：把 `IBlobStore` 的"推式"读取包成"拉式"字节源（一次只取一块），
//    这样复制/校验和都是 O(1) 内存（C6.9）。此前该逻辑在 `BlobByteSource` 里是一份私有副本。
class BufferSink final : public ByteSink {
 public:
  BufferSink(char* out, std::size_t capacity) : out_(out), capacity_(capacity) {}

  fss::Result<void> Write(std::string_view data) override {
    if (written_ + data.size() > capacity_) {
      return Err(fss::ErrorKind::kInternal, "BufferSink 缓冲溢出");
    }
    std::memcpy(out_ + written_, data.data(), data.size());
    written_ += data.size();
    return Ok();
  }

  std::size_t written() const { return written_; }

 private:
  char* out_;
  std::size_t capacity_;
  std::size_t written_ = 0;
};

class CountingSink final : public ByteSink {
 public:
  Result<void> Write(std::string_view data) override;
  std::uint64_t bytes_written() const { return written_; }
  std::uint64_t write_calls() const { return calls_; }

 private:
  std::uint64_t written_ = 0;
  std::uint64_t calls_ = 0;
};

// 生成 N 字节的确定性伪随机内容（基准/测试用；不用于任何安全用途）
class RepeatingSource final : public ByteSource {
 public:
  RepeatingSource(std::int64_t total, std::string pattern = "0123456789abcdef");
  Result<std::size_t> Read(char* out, std::size_t capacity) override;
  std::optional<std::int64_t> Size() const override { return total_; }
  bool Seekable() const override { return true; }
  Result<void> Seek(std::int64_t offset) override;

 private:
  std::int64_t total_;
  std::int64_t pos_ = 0;
  std::string pattern_;
};

// =============================================================================
//  三、缓冲
// =============================================================================
//  定长缓冲：容量固定、不隐式增长 —— 让"内存占用"在代码里显式可见
class FixedBuffer {
 public:
  explicit FixedBuffer(std::size_t capacity);
  ~FixedBuffer();
  FixedBuffer(FixedBuffer&&) noexcept;
  FixedBuffer& operator=(FixedBuffer&&) noexcept;
  FixedBuffer(const FixedBuffer&) = delete;
  FixedBuffer& operator=(const FixedBuffer&) = delete;

  char* data() { return data_; }
  const char* data() const { return data_; }
  std::size_t capacity() const { return capacity_; }
  std::size_t size() const { return size_; }
  void set_size(std::size_t n);
  std::string_view view() const { return std::string_view(data_, size_); }
  void clear() { size_ = 0; }

 private:
  char* data_ = nullptr;
  std::size_t capacity_ = 0;
  std::size_t size_ = 0;
};

// 页对齐缓冲（P3/P9 的 O_DIRECT / io_uring 需要对齐的地址与长度）
class AlignedBuffer {
 public:
  AlignedBuffer(std::size_t size, std::size_t alignment);
  ~AlignedBuffer();
  AlignedBuffer(AlignedBuffer&&) noexcept;
  AlignedBuffer& operator=(AlignedBuffer&&) noexcept;
  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;

  char* data() { return data_; }
  const char* data() const { return data_; }
  std::size_t size() const { return size_; }
  std::size_t alignment() const { return alignment_; }
  bool IsAligned() const;

  static std::size_t PageSize();

 private:
  char* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t alignment_ = 0;
};

// =============================================================================
//  四、包装（限流 / 上限）
// =============================================================================
//  硬上限：读满 limit 字节后**立即**报 `kInvalidArgument`。
//  这是 C1.2 的 H-2 三重防护之一："计数 reader 超限立即中止"。
//  ⚠️ 为什么不在读完后再判断：那样 1 GiB 的恶意 body 已经占满内存/磁盘了。
class LimitedSource final : public ByteSource {
 public:
  LimitedSource(ByteSource& inner, std::int64_t limit, std::string what = "body");
  Result<std::size_t> Read(char* out, std::size_t capacity) override;
  std::optional<std::int64_t> Size() const override;
  std::int64_t bytes_read() const { return read_; }
  bool exceeded() const { return exceeded_; }
  // 已读字节数 == 承诺长度 的断言（防御"静默截断"，见契约 §1.7）
  Result<void> VerifyConsumed(std::int64_t expected) const;

 private:
  ByteSource& inner_;
  std::int64_t limit_;
  std::int64_t read_ = 0;
  bool exceeded_ = false;
  std::string what_;
};

// 令牌桶限流（可注入时钟 → 测试不用 sleep）。
//   rate_bytes_per_sec <= 0 或 burst == 0 → 直接透传（相当于关闭）
//   ⚠️ 只做"长期平均速率"限制；桶满时允许突发 burst 字节。
class ThrottledSource final : public ByteSource {
 public:
  ThrottledSource(ByteSource& inner, const IClock& clock, std::int64_t rate_bytes_per_sec,
                  std::int64_t burst_bytes = 256 * 1024);
  Result<std::size_t> Read(char* out, std::size_t capacity) override;
  std::optional<std::int64_t> Size() const override;
  // 需要等待才能继续时返回建议等待的毫秒数（0 = 可以立即读）
  std::int64_t WaitMillis(std::size_t want) const;
  std::int64_t bytes_read() const { return read_; }

 private:
  void Refill() const;
  ByteSource& inner_;
  const IClock& clock_;
  std::int64_t rate_;
  std::int64_t burst_;
  mutable std::int64_t tokens_ = 0;
  mutable std::int64_t last_refill_ms_ = 0;
  std::int64_t read_ = 0;
};

}  // namespace bytes
}  // namespace fss
