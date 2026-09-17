// C1.x：common/bytes —— Range 归一化 + 流抽象 + 缓冲 + 限流
//
// Range 的判据来自 RFC 7233 与 docs/03-api-contract.md §1.8；每个"拒绝/降级"断言
// 都配了正向对照（合法输入必须被接受），否则无法区分"实现正确"与"一律拒绝"（R16）。
#include <catch2/catch.hpp>

#include "common/bytes/bytes.h"
#include "common/time/clock.h"

#include <string>
#include <vector>

using fss::bytes::AlignedBuffer;
using fss::bytes::ByteRange;
using fss::bytes::CountingSink;
using fss::bytes::FixedBuffer;
using fss::bytes::LimitedSource;
using fss::bytes::ParseRangeHeader;
using fss::bytes::RangeStatus;
using fss::bytes::RepeatingSource;
using fss::bytes::StringSink;
using fss::bytes::StringSource;
using fss::bytes::ThrottledSource;

namespace {

// 把解析结果压成便于比较的形式：状态 + 区间列表
std::string Describe(const fss::bytes::RangeResult& r) {
  std::string out = fss::bytes::RangeStatusName(r.status);
  for (const auto& br : r.ranges) {
    out += " [" + std::to_string(br.first) + "," + std::to_string(br.last) + "]";
  }
  return out;
}

}  // namespace

TEST_CASE("★ Range：三种合法形式（a-b / a- / -N）", "[phase1][bytes][contract]") {
  constexpr std::int64_t kTotal = 1000;

  SECTION("bytes=0-499 → 单个区间") {
    const auto r = ParseRangeHeader("bytes=0-499", kTotal);
    REQUIRE(r.status == RangeStatus::kSingle);
    REQUIRE(r.ranges.size() == 1);
    REQUIRE(r.ranges[0] == ByteRange{0, 499});
    REQUIRE(r.ranges[0].Length() == 500);
  }

  SECTION("bytes=500- → 到末尾") {
    const auto r = ParseRangeHeader("bytes=500-", kTotal);
    REQUIRE(r.status == RangeStatus::kSingle);
    REQUIRE(r.ranges[0] == ByteRange{500, 999});
  }

  SECTION("bytes=-200 → 最后 200 字节") {
    const auto r = ParseRangeHeader("bytes=-200", kTotal);
    REQUIRE(r.status == RangeStatus::kSingle);
    REQUIRE(r.ranges[0] == ByteRange{800, 999});
    REQUIRE(r.ranges[0].Length() == 200);
  }

  SECTION("末尾越界自动截断（bytes=900-5000 → 900-999）") {
    const auto r = ParseRangeHeader("bytes=900-5000", kTotal);
    REQUIRE(r.status == RangeStatus::kSingle);
    REQUIRE(r.ranges[0] == ByteRange{900, 999});
  }

  SECTION("后缀长度 ≥ 总长 → 整个表示（bytes=-5000）") {
    const auto r = ParseRangeHeader("bytes=-5000", kTotal);
    REQUIRE(r.status == RangeStatus::kSingle);
    REQUIRE(r.ranges[0] == ByteRange{0, 999});
  }

  SECTION("单字节 bytes=0-0 与最后单字节 bytes=999-999") {
    REQUIRE(ParseRangeHeader("bytes=0-0", kTotal).ranges[0] == ByteRange{0, 0});
    REQUIRE(ParseRangeHeader("bytes=999-999", kTotal).ranges[0] == ByteRange{999, 999});
  }
}

TEST_CASE("★ Range：越界 → 416（kUnsatisfiable），与 H-1 缺陷直接相关",
          "[phase1][bytes][contract]") {
  constexpr std::int64_t kTotal = 1000;

  SECTION("起点 == 总长 → 不可满足（不是 0 字节的 206）") {
    const auto r = ParseRangeHeader("bytes=1000-", kTotal);
    REQUIRE(r.status == RangeStatus::kUnsatisfiable);
    REQUIRE(r.ranges.empty());
  }
  SECTION("起点 > 总长") {
    REQUIRE(ParseRangeHeader("bytes=1001-2000", kTotal).status == RangeStatus::kUnsatisfiable);
    REQUIRE(ParseRangeHeader("bytes=999999999-", kTotal).status == RangeStatus::kUnsatisfiable);
  }
  SECTION("后缀长度 0 → 不可满足（RFC 7233 §2.1）") {
    REQUIRE(ParseRangeHeader("bytes=-0", kTotal).status == RangeStatus::kUnsatisfiable);
  }
  SECTION("空文件（total=0）→ 任何区间都不可满足") {
    REQUIRE(ParseRangeHeader("bytes=0-", 0).status == RangeStatus::kUnsatisfiable);
    REQUIRE(ParseRangeHeader("bytes=0-10", 0).status == RangeStatus::kUnsatisfiable);
    REQUIRE(ParseRangeHeader("bytes=-10", 0).status == RangeStatus::kUnsatisfiable);
  }
  SECTION("★ 对照：同一批语法在**不越界**时必须成功（否则上面的断言证明不了什么）") {
    REQUIRE(ParseRangeHeader("bytes=999-", kTotal).status == RangeStatus::kSingle);
    REQUIRE(ParseRangeHeader("bytes=999-999", kTotal).status == RangeStatus::kSingle);
    REQUIRE(ParseRangeHeader("bytes=-1", kTotal).status == RangeStatus::kSingle);
  }
}

TEST_CASE("Range：多段与'部分不可满足'的合并语义", "[phase1][bytes][contract]") {
  constexpr std::int64_t kTotal = 1000;

  SECTION("两个可满足区间 → kMultiple，且保持客户端给的顺序") {
    const auto r = ParseRangeHeader("bytes=0-99, 200-299", kTotal);
    REQUIRE(r.status == RangeStatus::kMultiple);
    REQUIRE(r.ranges.size() == 2);
    REQUIRE(r.ranges[0] == ByteRange{0, 99});
    REQUIRE(r.ranges[1] == ByteRange{200, 299});
  }

  SECTION("部分不可满足 → 丢掉不可满足的，剩下的照常返回 206") {
    const auto r = ParseRangeHeader("bytes=0-99, 5000-6000, 200-299", kTotal);
    REQUIRE(r.status == RangeStatus::kMultiple);
    REQUIRE(r.ranges.size() == 2);
    REQUIRE(r.ranges[0] == ByteRange{0, 99});
    REQUIRE(r.ranges[1] == ByteRange{200, 299});
  }

  SECTION("全部不可满足 → kUnsatisfiable") {
    REQUIRE(ParseRangeHeader("bytes=5000-6000,7000-8000", kTotal).status ==
            RangeStatus::kUnsatisfiable);
  }

  SECTION("重叠区间原样保留（由上层决定是否合并，本层不改语义）") {
    const auto r = ParseRangeHeader("bytes=0-499,100-599", kTotal);
    REQUIRE(r.status == RangeStatus::kMultiple);
    REQUIRE(r.ranges.size() == 2);
  }

  SECTION("区间数量上限：超过 kMaxRanges → 按 malformed 忽略（防御巨型 multipart）") {
    std::string header = "bytes=";
    for (std::size_t i = 0; i < fss::bytes::kMaxRanges + 1; ++i) {
      if (i) header += ",";
      header += std::to_string(i) + "-" + std::to_string(i);
    }
    REQUIRE(ParseRangeHeader(header, kTotal).status == RangeStatus::kMalformed);
    // 对照：正好等于上限时必须被接受
    std::string ok_header = "bytes=";
    for (std::size_t i = 0; i < fss::bytes::kMaxRanges; ++i) {
      if (i) ok_header += ",";
      ok_header += std::to_string(i) + "-" + std::to_string(i);
    }
    REQUIRE(ParseRangeHeader(ok_header, kTotal).status == RangeStatus::kMultiple);
  }
}

TEST_CASE("★ Range：语法非法 → kMalformed（忽略该头），**不是** 416", "[phase1][bytes][contract]") {
  // 判据（RFC 7233 §4.4）：416 只用于"语法合法但不可满足"。
  // 把 malformed 也当 416 会让某些客户端陷入重试循环，所以两者必须分开。
  constexpr std::int64_t kTotal = 1000;
  const std::vector<std::string> malformed = {
      "bytes=",             // 空区间集
      "bytes=,",            // 只有逗号
      "bytes=abc-def",      // 非数字
      "bytes=-",            // 两侧都空
      "bytes=100",          // 没有 '-'
      "bytes=200-100",      // ★ last < first（RFC 7233 §2.1 判为非法）
      "bytes=0-1-2",        // 多了一个 '-'
      "bytes=99999999999999999999-",  // 溢出（不能被静默截断成一个大数）
      "items=0-10",         // 未知单位
      "bytes=0 - 1 . 2",    // 夹杂奇怪字符
  };
  for (const auto& h : malformed) {
    INFO("header: " << h);
    REQUIRE(ParseRangeHeader(h, kTotal).status == RangeStatus::kMalformed);
  }
  // 对照：合法输入不能被误判为 malformed
  for (const char* h : {"bytes=0-1", "bytes=0-", "bytes=-1", "bytes = 0-1", "BYTES=0-1"}) {
    INFO("header: " << h);
    REQUIRE(ParseRangeHeader(h, kTotal).status == RangeStatus::kSingle);
  }
}

TEST_CASE("Range：缺省与宽容处理", "[phase1][bytes][contract]") {
  constexpr std::int64_t kTotal = 1000;
  SECTION("没有 Range 头 → kAbsent") {
    REQUIRE(ParseRangeHeader("", kTotal).status == RangeStatus::kAbsent);
    REQUIRE(ParseRangeHeader("   ", kTotal).status == RangeStatus::kAbsent);
  }
  SECTION("尾随逗号宽容，但不能全靠逗号") {
    REQUIRE(ParseRangeHeader("bytes=0-1,", kTotal).status == RangeStatus::kSingle);
    REQUIRE(ParseRangeHeader("bytes=0-1, , 5-6", kTotal).status == RangeStatus::kMultiple);
  }
  SECTION("总长未知（-1）时只做语法校验，开区间保持开放") {
    const auto r = ParseRangeHeader("bytes=100-", -1);
    REQUIRE(r.status == RangeStatus::kSingle);
    REQUIRE(r.ranges[0].first == 100);
    REQUIRE(r.ranges[0].last == std::numeric_limits<std::int64_t>::max());
    // 后缀形式在总长未知时无法换算 → 明确报 malformed（不猜）
    REQUIRE(ParseRangeHeader("bytes=-100", -1).status == RangeStatus::kMalformed);
  }
}

TEST_CASE("Content-Range / multipart 头部格式", "[phase1][bytes][contract]") {
  REQUIRE(fss::bytes::FormatContentRange(ByteRange{0, 499}, 1000) == "bytes 0-499/1000");
  REQUIRE(fss::bytes::FormatContentRange(ByteRange{500, 999}, 1000) == "bytes 500-999/1000");
  REQUIRE(fss::bytes::FormatUnsatisfiedContentRange(1000) == "bytes */1000");
  REQUIRE(fss::bytes::FormatUnsatisfiedContentRange(-1) == "bytes */*");

  const auto part = fss::bytes::FormatMultipartRangeHeader(
      "BOUNDARY", ByteRange{0, 99}, 1000, "text/plain");
  REQUIRE(part == "--BOUNDARY\r\nContent-Type: text/plain\r\nContent-Range: bytes 0-99/1000\r\n\r\n");
  // 未指定 content-type 时的默认值
  REQUIRE(fss::bytes::FormatMultipartRangeHeader("B", ByteRange{0, 0}, 1, "").find(
              "application/octet-stream") != std::string::npos);
}

TEST_CASE("ByteSource/ByteSink：读满、读全、搬运", "[phase1][bytes]") {
  SECTION("StringSource 短读语义：每次最多给 capacity 字节，EOF 返回 0") {
    StringSource src("hello world");
    char buf[4];
    REQUIRE(src.Read(buf, 4).value() == 4);
    REQUIRE(std::string(buf, 4) == "hell");
    REQUIRE(src.Read(buf, 4).value() == 4);
    REQUIRE(src.Read(buf, 4).value() == 3);
    REQUIRE(src.Read(buf, 4).value() == 0);  // ★ EOF 之后继续读仍然是 0（可重复）
    REQUIRE(src.Read(buf, 4).value() == 0);
    REQUIRE(src.Size().value() == 11);
  }

  SECTION("ReadFully：不足时明确报错（不能静默返回短数据）") {
    StringSource src("abc");
    char buf[5];
    auto r = src.ReadFully(buf, 5);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().kind() == fss::ErrorKind::kUnavailable);
    REQUIRE(r.error().message().find("提前结束") != std::string::npos);
  }

  SECTION("ReadAll：超过上限立即拒绝") {
    StringSource src(std::string(5000, 'x'));
    REQUIRE(src.ReadAll(4096).ok() == false);
    REQUIRE(src.ReadAll(4096).error().kind() == fss::ErrorKind::kInvalidArgument);
    REQUIRE(src.ReadAll(8192).value().size() == 5000);
  }

  SECTION("PumpTo：固定缓冲搬运，内容与长度都正确") {
    const std::string payload(200 * 1024, 'q');
    StringSource src(payload);
    StringSink sink;
    const auto r = src.PumpTo(sink, 4096);
    REQUIRE(r.ok());
    REQUIRE(r.value() == payload.size());
    REQUIRE(sink.str() == payload);
  }

  SECTION("RepeatingSource：确定性内容，可复现") {
    RepeatingSource a(20, "abc");
    RepeatingSource b(20, "abc");
    char ba[20] = {};
    char bb[20] = {};
    REQUIRE(a.ReadFully(ba, 20).ok());
    REQUIRE(b.ReadFully(bb, 20).ok());
    REQUIRE(std::string(ba, 20) == "abcabcabcabcabcabcab");
    REQUIRE(std::string(ba, 20) == std::string(bb, 20));
  }

  SECTION("StringSink 关闭后拒绝写入") {
    StringSink sink;
    REQUIRE(sink.Write("a").ok());
    REQUIRE(sink.Close().ok());
    auto r = sink.Write("b");
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().kind() == fss::ErrorKind::kInternal);
  }
}

TEST_CASE("★ LimitedSource：超限**立即**中止（H-2 的计数 reader）", "[phase1][bytes][http]") {
  SECTION("刚好等于上限：必须成功（不能把'读满'误判成超限）") {
    StringSource inner(std::string(100, 'x'));
    LimitedSource src(inner, 100, "body");
    char buf[64];
    std::size_t total = 0;
    while (true) {
      auto r = src.Read(buf, sizeof buf);
      REQUIRE(r.ok());
      if (r.value() == 0) break;
      total += r.value();
    }
    REQUIRE(total == 100);
    REQUIRE_FALSE(src.exceeded());
    REQUIRE(src.VerifyConsumed(100).ok());
  }

  SECTION("超过上限 1 字节：读到超限即报错，且不再继续读") {
    StringSource inner(std::string(101, 'x'));
    LimitedSource src(inner, 100, "body");
    char buf[64];
    bool failed = false;
    for (int i = 0; i < 10 && !failed; ++i) {
      auto r = src.Read(buf, sizeof buf);
      if (!r.ok()) {
        failed = true;
        REQUIRE(r.error().kind() == fss::ErrorKind::kInvalidArgument);
        REQUIRE(r.error().message().find("超过上限") != std::string::npos);
      }
    }
    REQUIRE(failed);
    REQUIRE(src.exceeded());
  }

  SECTION("远超上限（1000 倍）时，读取量被限制在上限附近 —— 不会先吃掉整个流") {
    StringSource inner(std::string(100 * 1024, 'x'));
    LimitedSource src(inner, 64, "body");
    char buf[8192];
    while (true) {
      auto r = src.Read(buf, sizeof buf);
      if (!r.ok()) break;
      if (r.value() == 0) break;
    }
    REQUIRE(src.exceeded());
    // ★ 关键：读入量必须被钳在上限附近（H-2 的教训是"读完才发现超限"）
    REQUIRE(src.bytes_read() <= 64 + 1);
  }

  SECTION("长度不符（承诺 100 实际 50）→ VerifyConsumed 必须失败") {
    StringSource inner(std::string(50, 'x'));
    LimitedSource src(inner, 1000, "body");
    char buf[1024];
    while (true) {
      auto r = src.Read(buf, sizeof buf);
      REQUIRE(r.ok());
      if (r.value() == 0) break;
    }
    auto v = src.VerifyConsumed(100);
    REQUIRE_FALSE(v.ok());
    REQUIRE(v.error().message().find("长度不符") != std::string::npos);
  }

  SECTION("limit <= 0 表示不限") {
    StringSource inner(std::string(10, 'x'));
    LimitedSource src(inner, 0, "body");
    REQUIRE(src.ReadAll(1024).value().size() == 10);
  }
}

TEST_CASE("ThrottledSource：可注入时钟的令牌桶（测试不 sleep）", "[phase1][bytes]") {
  using fss::ManualClock;

  SECTION("初始满桶：第一轮可以突发 burst 字节") {
    ManualClock clock(1700000000);
    StringSource inner(std::string(10000, 'x'));
    ThrottledSource src(inner, clock, /*rate=*/1000, /*burst=*/2048);
    char buf[4096];
    auto r = src.Read(buf, sizeof buf);
    REQUIRE(r.ok());
    REQUIRE(r.value() == 2048);  // 受 burst 限制
  }

  SECTION("令牌耗尽后报 kUnavailable（不是返回 0 被误判成 EOF）") {
    ManualClock clock(1700000000);
    StringSource inner(std::string(10000, 'x'));
    ThrottledSource src(inner, clock, 1000, 1024);
    char buf[8192];
    REQUIRE(src.Read(buf, sizeof buf).value() == 1024);  // 用光桶
    auto exhausted = src.Read(buf, sizeof buf);
    REQUIRE_FALSE(exhausted.ok());
    REQUIRE(exhausted.error().kind() == fss::ErrorKind::kUnavailable);
    // 必须给出等待时间，否则调用方只能猜
    REQUIRE(exhausted.error().Find("wait_ms").has_value());
  }

  SECTION("推进时钟后令牌按速率补充") {
    ManualClock clock(1700000000);
    StringSource inner(std::string(10000, 'x'));
    ThrottledSource src(inner, clock, /*rate=*/1000, /*burst=*/1000);
    char buf[8192];
    REQUIRE(src.Read(buf, sizeof buf).value() == 1000);
    clock.AdvanceSeconds(1);  // 1 秒 → 补 1000 字节
    REQUIRE(src.Read(buf, sizeof buf).value() == 1000);
    clock.AdvanceSeconds(10);  // 补满但不超过 burst
    REQUIRE(src.Read(buf, sizeof buf).value() == 1000);
    REQUIRE(src.Read(buf, sizeof buf).ok() == false);
  }

  SECTION("rate <= 0 → 关闭限流，直接透传") {
    ManualClock clock(1700000000);
    StringSource inner(std::string(5000, 'x'));
    ThrottledSource src(inner, clock, 0, 1024);
    REQUIRE(src.ReadAll(8192).value().size() == 5000);
  }

  SECTION("WaitMillis 向上取整，且不忙等") {
    ManualClock clock(1700000000);
    StringSource inner(std::string(100000, 'x'));
    ThrottledSource src(inner, clock, /*rate=*/1000, /*burst=*/1000);
    char buf[8192];
    REQUIRE(src.Read(buf, sizeof buf).value() == 1000);
    REQUIRE(src.WaitMillis(500) == 500);  // 需 500 字节 → 0.5 秒 → 500 ms
    REQUIRE(src.WaitMillis(1000) == 1000);
  }
}

TEST_CASE("缓冲：定长缓冲不隐式增长；对齐缓冲地址真对齐", "[phase1][bytes]") {
  SECTION("FixedBuffer 容量语义") {
    FixedBuffer buf(16);
    REQUIRE(buf.capacity() == 16);
    REQUIRE(buf.size() == 0);
    buf.set_size(8);
    REQUIRE(buf.size() == 8);
    buf.set_size(999);          // 越界被钳住
    REQUIRE(buf.size() == 16);
    buf.clear();
    REQUIRE(buf.size() == 0);
  }

  SECTION("FixedBuffer 移动后原对象安全（不 double free）") {
    FixedBuffer a(32);
    a.set_size(4);
    FixedBuffer b(std::move(a));
    REQUIRE(b.capacity() == 32);
    REQUIRE(b.size() == 4);
    REQUIRE(a.capacity() == 0);
    REQUIRE(a.size() == 0);
  }

  SECTION("AlignedBuffer 按页对齐，可读写") {
    AlignedBuffer buf(4096, 0);
    REQUIRE(buf.size() == 4096);
    REQUIRE(buf.IsAligned());
    REQUIRE(reinterpret_cast<std::uintptr_t>(buf.data()) % AlignedBuffer::PageSize() == 0);
    buf.data()[0] = 'x';
    buf.data()[4095] = 'y';
    REQUIRE(buf.data()[0] == 'x');
    REQUIRE(buf.data()[4095] == 'y');
  }

  SECTION("AlignedBuffer 指定 512 对齐（O_DIRECT 常见要求）") {
    AlignedBuffer buf(1024, 512);
    REQUIRE(buf.IsAligned());
    REQUIRE(reinterpret_cast<std::uintptr_t>(buf.data()) % 512 == 0);
  }
}

TEST_CASE("CountingSink：只计数不留数据（基准用）", "[phase1][bytes]") {
  CountingSink sink;
  StringSource src(std::string(100000, 'z'));
  const auto r = src.PumpTo(sink, 8192);
  REQUIRE(r.ok());
  REQUIRE(sink.bytes_written() == 100000);
  REQUIRE(sink.write_calls() == 13);  // ceil(100000/8192) —— 顺带锁定缓冲语义
}
