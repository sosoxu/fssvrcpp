#include "common/bytes/bytes.h"

#include "common/time/clock.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unistd.h>

namespace fss::bytes {
namespace {

// 解析无符号十进制；拒绝空串、符号、非数字、溢出。
// ★ 不用 strtoll 的"部分解析 + 静默截断"：`bytes=99999999999999999999-` 必须被判为
//   malformed，而不是悄悄变成一个巨大的数（那会算出错误区间并返回错的内容）。
bool ParseUint64(std::string_view text, std::int64_t* out) {
  if (text.empty()) return false;
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
    value = value * 10 + digit;
  }
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
  *out = static_cast<std::int64_t>(value);
  return true;
}

// 去掉首尾空格/制表（RFC 7233 的语法里没有 OWS，但实测客户端会写 `bytes = 0-1`，
// 宽容接受比返回 416 更符合"不要因为无关紧要的格式差异惩罚客户端"）
std::string_view Trim(std::string_view s) {
  const auto b = s.find_first_not_of(" \t");
  if (b == std::string_view::npos) return {};
  const auto e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

RangeResult Malformed(std::string detail) {
  RangeResult out;
  out.status = RangeStatus::kMalformed;
  out.detail = std::move(detail);
  return out;
}

}  // namespace

const char* RangeStatusName(RangeStatus status) {
  switch (status) {
    case RangeStatus::kAbsent: return "absent";
    case RangeStatus::kSingle: return "single";
    case RangeStatus::kMultiple: return "multiple";
    case RangeStatus::kUnsatisfiable: return "unsatisfiable";
    case RangeStatus::kMalformed: return "malformed";
  }
  return "?";
}

RangeResult ParseRangeHeader(std::string_view header_value, std::int64_t total_size) {
  RangeResult out;

  const std::string_view header = Trim(header_value);
  if (header.empty()) return out;  // kAbsent：空头等价于没给

  const auto eq = header.find('=');
  if (eq == std::string_view::npos) return Malformed("缺少 '='");

  std::string unit(Trim(header.substr(0, eq)));
  std::transform(unit.begin(), unit.end(), unit.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (unit != "bytes") {
    // 未知单位按 RFC 7233 §4.4 忽略该头（不是 416）
    return Malformed("未知单位 '" + unit + "'");
  }

  const std::string_view set = Trim(header.substr(eq + 1));
  if (set.empty()) return Malformed("空的区间集合");

  std::vector<ByteRange> satisfiable;
  std::size_t unsatisfiable = 0;
  std::size_t specs = 0;

  std::size_t pos = 0;
  while (pos < set.size()) {
    const auto comma = set.find(',', pos);
    const auto end = comma == std::string_view::npos ? set.size() : comma;
    const std::string_view spec = Trim(set.substr(pos, end - pos));
    pos = (comma == std::string_view::npos) ? set.size() : comma + 1;

    if (spec.empty()) {
      // `bytes=0-1,` 的尾随逗号可以宽容；但 `bytes=,`（全空）要报 malformed
      if (specs == 0 && comma == std::string_view::npos) return Malformed("空区间");
      continue;
    }
    if (++specs > kMaxRanges) {
      // 防御：拒绝病态的区间数量，否则会构造出巨大的 multipart 响应。
      // 按 malformed 处理 → 忽略该头返回全量（比 416 更保守，也更安全）。
      return Malformed("区间数量超过上限 " + std::to_string(kMaxRanges));
    }

    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) return Malformed("缺少 '-'：'" + std::string(spec) + "'");

    const std::string_view first_text = Trim(spec.substr(0, dash));
    const std::string_view last_text = Trim(spec.substr(dash + 1));
    if (first_text.empty() && last_text.empty()) return Malformed("'-' 两侧都为空");

    if (first_text.empty()) {
      // 后缀形式 `-N`：最后 N 字节
      std::int64_t suffix = 0;
      if (!ParseUint64(last_text, &suffix)) {
        return Malformed("后缀长度非法：'" + std::string(last_text) + "'");
      }
      if (suffix == 0) {  // RFC 7233 §2.1：suffix-length 为 0 → 不可满足
        ++unsatisfiable;
        continue;
      }
      if (total_size < 0) return Malformed("总长未知时无法解析后缀区间");
      if (total_size == 0) {
        ++unsatisfiable;
        continue;
      }
      const std::int64_t first = suffix >= total_size ? 0 : total_size - suffix;
      satisfiable.push_back(ByteRange{first, total_size - 1});
      continue;
    }

    std::int64_t first = 0;
    if (!ParseUint64(first_text, &first)) {
      return Malformed("区间起点非法：'" + std::string(first_text) + "'");
    }

    std::int64_t last = 0;
    if (last_text.empty()) {
      if (total_size < 0) {
        last = std::numeric_limits<std::int64_t>::max();  // 未知长度：留给调用方
      } else {
        if (first >= total_size) {
          ++unsatisfiable;
          continue;
        }
        last = total_size - 1;
      }
    } else {
      if (!ParseUint64(last_text, &last)) {
        return Malformed("区间终点非法：'" + std::string(last_text) + "'");
      }
      // ★ RFC 7233 §2.1：last < first 的 byte-range-spec **非法** → 整个头无效。
      //   这是 malformed（忽略）而不是 416 —— 416 只留给"语法合法但不可满足"。
      if (last < first) {
        return Malformed("终点小于起点：" + std::to_string(first) + "-" + std::to_string(last));
      }
      if (total_size >= 0) {
        if (first >= total_size) {
          ++unsatisfiable;
          continue;
        }
        last = std::min(last, total_size - 1);  // 截到末尾
      }
    }
    satisfiable.push_back(ByteRange{first, last});
  }

  if (satisfiable.empty()) {
    if (unsatisfiable > 0) {
      out.status = RangeStatus::kUnsatisfiable;
      out.detail = "全部区间不可满足（total=" + std::to_string(total_size) + "）";
      return out;
    }
    return Malformed("没有可解析的区间");
  }
  out.ranges = std::move(satisfiable);
  out.status = out.ranges.size() == 1 ? RangeStatus::kSingle : RangeStatus::kMultiple;
  return out;
}

std::string FormatContentRange(const ByteRange& range, std::int64_t total_size) {
  return "bytes " + std::to_string(range.first) + "-" + std::to_string(range.last) + "/" +
         (total_size >= 0 ? std::to_string(total_size) : std::string("*"));
}

std::string FormatUnsatisfiedContentRange(std::int64_t total_size) {
  return "bytes */" + (total_size >= 0 ? std::to_string(total_size) : std::string("*"));
}

std::string FormatMultipartRangeHeader(std::string_view boundary, const ByteRange& range,
                                      std::int64_t total_size, std::string_view content_type) {
  std::string out;
  out += "--";
  out += boundary;
  out += "\r\nContent-Type: ";
  out += content_type.empty() ? "application/octet-stream" : std::string(content_type);
  out += "\r\nContent-Range: ";
  out += FormatContentRange(range, total_size);
  out += "\r\n\r\n";
  return out;
}

// =============================================================================
//  ByteSource / ByteSink 便捷方法
// =============================================================================
Result<void> ByteSource::ReadFully(char* out, std::size_t n) {
  std::size_t got = 0;
  while (got < n) {
    FSS_TRY(read, Read(out + got, n - got));
    if (read == 0) {
      return Err(ErrorKind::kUnavailable, "流提前结束：期望 " + std::to_string(n) + " 字节，实际 " +
                                             std::to_string(got));
    }
    got += read;
  }
  return Ok();
}

Result<std::string> ByteSource::ReadAll(std::size_t max_bytes) {
  std::string out;
  if (const auto size = Size(); size.has_value() && *size >= 0) {
    const auto wanted = static_cast<std::size_t>(*size);
    if (wanted > max_bytes) {
      return Err(ErrorKind::kInvalidArgument,
                 "流长度 " + std::to_string(wanted) + " 超过上限 " + std::to_string(max_bytes));
    }
    out.reserve(wanted);  // 已知长度时只扩容一次
  }
  char buf[16 * 1024];
  while (true) {
    FSS_TRY(n, Read(buf, sizeof buf));
    if (n == 0) break;
    if (out.size() + n > max_bytes) {
      return Err(ErrorKind::kInvalidArgument,
                 "流内容超过上限 " + std::to_string(max_bytes) + " 字节");
    }
    out.append(buf, n);
  }
  return out;
}

Result<std::uint64_t> ByteSource::PumpTo(ByteSink& sink, std::size_t buffer_size) {
  if (buffer_size == 0) buffer_size = 64 * 1024;
  // 固定缓冲：RSS 与流长度无关（C1.3 的 1 GiB 流式断言靠的就是这一条）
  std::vector<char> buf(buffer_size);
  std::uint64_t total = 0;
  while (true) {
    FSS_TRY(n, Read(buf.data(), buf.size()));
    if (n == 0) break;
    FSS_TRY(sink.Write(std::string_view(buf.data(), n)));
    total += n;
  }
  FSS_TRY(sink.Close());
  return total;
}

// ---------------- StringSource / StringSink / CountingSink ----------------
Result<std::size_t> StringSource::Read(char* out, std::size_t capacity) {
  const auto remaining = data_.size() - pos_;
  const auto n = std::min(capacity, remaining);
  if (n > 0) {
    std::memcpy(out, data_.data() + pos_, n);
    pos_ += n;
  }
  return n;
}

Result<void> StringSource::Seek(std::int64_t offset) {
  if (offset < 0 || static_cast<std::size_t>(offset) > data_.size()) {
    return Err(ErrorKind::kInvalidArgument, "StringSource 越界 seek");
  }
  pos_ = static_cast<std::size_t>(offset);
  return Ok();
}

Result<void> StringSink::Write(std::string_view data) {
  if (closed_) return Err(ErrorKind::kInternal, "向已关闭的 StringSink 写入");
  data_.append(data);
  return Ok();
}

Result<void> StringSink::Close() {
  closed_ = true;
  return Ok();
}

Result<void> CountingSink::Write(std::string_view data) {
  written_ += data.size();
  ++calls_;
  return Ok();
}

// ---------------- RepeatingSource ----------------
RepeatingSource::RepeatingSource(std::int64_t total, std::string pattern)
    : total_(total), pattern_(std::move(pattern)) {
  if (pattern_.empty()) pattern_ = "0";
}

Result<std::size_t> RepeatingSource::Read(char* out, std::size_t capacity) {
  if (pos_ >= total_) return std::size_t{0};
  const auto remaining = static_cast<std::size_t>(total_ - pos_);
  const auto n = std::min(capacity, remaining);
  for (std::size_t i = 0; i < n; ++i) {
    const auto index = static_cast<std::size_t>(pos_) + i;
    out[i] = pattern_[index % pattern_.size()];
  }
  pos_ += static_cast<std::int64_t>(n);
  return n;
}

Result<void> RepeatingSource::Seek(std::int64_t offset) {
  if (offset < 0 || offset > total_) {
    return Err(ErrorKind::kInvalidArgument, "RepeatingSource 越界 seek");
  }
  pos_ = offset;
  return Ok();
}

// =============================================================================
//  FixedBuffer / AlignedBuffer
// =============================================================================
FixedBuffer::FixedBuffer(std::size_t capacity) : capacity_(capacity) {
  if (capacity_ > 0) data_ = new char[capacity_];
}

FixedBuffer::~FixedBuffer() { delete[] data_; }

FixedBuffer::FixedBuffer(FixedBuffer&& o) noexcept
    : data_(o.data_), capacity_(o.capacity_), size_(o.size_) {
  o.data_ = nullptr;
  o.capacity_ = 0;
  o.size_ = 0;
}

FixedBuffer& FixedBuffer::operator=(FixedBuffer&& o) noexcept {
  if (this != &o) {
    delete[] data_;
    data_ = o.data_;
    capacity_ = o.capacity_;
    size_ = o.size_;
    o.data_ = nullptr;
    o.capacity_ = 0;
    o.size_ = 0;
  }
  return *this;
}

void FixedBuffer::set_size(std::size_t n) {
  // 越界钳住："容量绝不增长"是显式的内存预算语义；静默增长会让预算失效
  size_ = std::min(n, capacity_);
}

std::size_t AlignedBuffer::PageSize() {
  const long page = ::sysconf(_SC_PAGESIZE);
  return page > 0 ? static_cast<std::size_t>(page) : 4096u;
}

AlignedBuffer::AlignedBuffer(std::size_t size, std::size_t alignment)
    : size_(size), alignment_(alignment == 0 ? PageSize() : alignment) {
  if (size_ == 0) return;
  void* raw = nullptr;
  if (::posix_memalign(&raw, alignment_, size_) != 0) raw = nullptr;
  data_ = static_cast<char*>(raw);
}

AlignedBuffer::~AlignedBuffer() { ::free(data_); }

AlignedBuffer::AlignedBuffer(AlignedBuffer&& o) noexcept
    : data_(o.data_), size_(o.size_), alignment_(o.alignment_) {
  o.data_ = nullptr;
  o.size_ = 0;
}

AlignedBuffer& AlignedBuffer::operator=(AlignedBuffer&& o) noexcept {
  if (this != &o) {
    ::free(data_);
    data_ = o.data_;
    size_ = o.size_;
    alignment_ = o.alignment_;
    o.data_ = nullptr;
    o.size_ = 0;
  }
  return *this;
}

bool AlignedBuffer::IsAligned() const {
  return data_ != nullptr && (reinterpret_cast<std::uintptr_t>(data_) % alignment_) == 0;
}

// =============================================================================
//  LimitedSource / ThrottledSource
// =============================================================================
LimitedSource::LimitedSource(ByteSource& inner, std::int64_t limit, std::string what)
    : inner_(inner), limit_(limit), what_(std::move(what)) {}

Result<std::size_t> LimitedSource::Read(char* out, std::size_t capacity) {
  if (exceeded_) return Err(ErrorKind::kInvalidArgument, what_ + " 已超限，拒绝继续读取");

  if (limit_ > 0) {
    const auto remaining = limit_ - read_;
    if (remaining <= 0) {
      // 恰好读满上限：还要再看一眼是否真的结束，否则"刚好等于上限"会被误判超限。
      // ★ 这一步是 H-2 的关键：只在**读满之后**才可能发现超限，而发现时必须
      //   立刻报错 —— 上层据此拒绝请求，handler 不得产生副作用。
      char probe = 0;
      FSS_TRY(extra, inner_.Read(&probe, 1));
      if (extra == 0) return std::size_t{0};
      exceeded_ = true;
      return Err(ErrorKind::kInvalidArgument,
                 what_ + " 超过上限 " + std::to_string(limit_) + " 字节（读到第 " +
                     std::to_string(read_ + 1) + " 字节）");
    }
    capacity = std::min<std::size_t>(capacity, static_cast<std::size_t>(remaining));
  }

  FSS_TRY(n, inner_.Read(out, capacity));
  read_ += static_cast<std::int64_t>(n);
  return n;
}

std::optional<std::int64_t> LimitedSource::Size() const {
  const auto inner = inner_.Size();
  if (!inner.has_value()) return std::nullopt;
  // 已知长度但已超过上限：不承诺这个长度（上层会先因超限被拒）
  if (limit_ > 0 && *inner > limit_) return std::nullopt;
  return inner;
}

Result<void> LimitedSource::VerifyConsumed(std::int64_t expected) const {
  if (read_ != expected) {
    // "静默截断"的守护断言：底层提前 EOF 时必须显式失败，而不是当成正常结束
    return Err(ErrorKind::kInvalidArgument, what_ + " 长度不符：Content-Length=" +
                                                std::to_string(expected) + "，实际读取=" +
                                                std::to_string(read_));
  }
  return Ok();
}

ThrottledSource::ThrottledSource(ByteSource& inner, const IClock& clock,
                                 std::int64_t rate_bytes_per_sec, std::int64_t burst_bytes)
    : inner_(inner), clock_(clock), rate_(rate_bytes_per_sec), burst_(burst_bytes) {
  if (burst_ <= 0) burst_ = 64 * 1024;
  tokens_ = burst_;  // 初始满桶：允许一开始就突发一个 burst
  last_refill_ms_ = clock_.NowEpochMillis();
}

void ThrottledSource::Refill() const {
  if (rate_ <= 0) return;
  const auto now = clock_.NowEpochMillis();
  const auto elapsed = now - last_refill_ms_;
  if (elapsed <= 0) return;
  const auto add = (rate_ * elapsed) / 1000;
  if (add > 0) {
    tokens_ = std::min(burst_, tokens_ + add);
    last_refill_ms_ = now;
  }
}

std::int64_t ThrottledSource::WaitMillis(std::size_t want) const {
  if (rate_ <= 0) return 0;
  Refill();
  if (static_cast<std::int64_t>(want) <= tokens_) return 0;
  const auto deficit = static_cast<std::int64_t>(want) - tokens_;
  return (deficit * 1000 + rate_ - 1) / rate_;  // 向上取整
}

Result<std::size_t> ThrottledSource::Read(char* out, std::size_t capacity) {
  if (rate_ <= 0) {  // 关闭限流 → 直接透传
    FSS_TRY(n, inner_.Read(out, capacity));
    read_ += static_cast<std::int64_t>(n);
    return n;
  }
  Refill();
  if (tokens_ <= 0) {
    // 返回 0 会让调用方误判 EOF，所以显式报"稍后重试"并给出等待时间
    Error err(ErrorKind::kUnavailable, "限流：令牌不足");
    err.With("wait_ms", std::to_string(WaitMillis(1)));
    return err;
  }
  const auto allowed = static_cast<std::size_t>(std::min<std::int64_t>(tokens_, static_cast<std::int64_t>(capacity)));
  FSS_TRY(n, inner_.Read(out, allowed));
  tokens_ -= static_cast<std::int64_t>(n);
  read_ += static_cast<std::int64_t>(n);
  return n;
}

std::optional<std::int64_t> ThrottledSource::Size() const { return inner_.Size(); }

}  // namespace bytes
