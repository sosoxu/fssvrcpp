// =============================================================================
//  日志热路径基准（AGENTS.md R2：独立进程 + 绑核；R3：标注协议与安全性）
// =============================================================================
//  用法：scripts/bench_logging.sh [迭代数]
//
//  为什么把它放进仓库：ADR-011 的取舍建立在"框架部分只占约 3% 的成本"这个实测之上。
//  数字如果不能再现，就等于没有。任何改动 logging 热路径的提交都应重跑本基准，
//  并把新数字写回 docs/test-evidence/phase1.md。
//
//  诚实声明（R3/R4）：
//    · 这里测的是**同一台机器、/dev/null sink** 的相对比较，不是生产容量结论；
//    · spdlog 对照项**不做 JSON、不做脱敏、也不逐行 flush**，因此它不是"同等工作量"
//      的对照 —— 它的用途是标出"日志框架本身"那一格的量级（约 95 ns）。
#include "common/logging/logging.h"
#include "common/time/clock.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace fss;

namespace {

constexpr const char* kRedactKeys =
    "secret_key,access_key,token,sig,signature,authorization,password";

double BenchOnce(int iters, bool do_io, bool text_format, bool redact,
                 std::size_t* bytes_out) {
  logging::LogOptions o;
  o.format = text_format ? "text" : "json";
  if (redact) o.redact_keys = logging::Redactor::FromCommaSeparated(kRedactKeys).keys();
  ManualClock clock(1700000000);
  std::ofstream devnull("/dev/null");
  logging::StreamLogger lg(o, clock, do_io ? static_cast<std::ostream&>(devnull) : std::cerr);
  const logging::Fields fields{{"method", "GET"},
                               {"path", "/api/file/v2/files/tenant-1/file-1"},
                               {"status", 200},
                               {"duration_ms", 3},
                               {"correlation_id", "cid-0001"}};
  const auto t0 = std::chrono::steady_clock::now();
  std::size_t bytes = 0;
  for (int i = 0; i < iters; ++i) {
    if (do_io) {
      lg.Log(logging::Level::kInfo, "request completed", fields);
    } else {
      bytes += lg.Render(logging::Level::kInfo, "request completed", fields).size();
    }
  }
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
          .count();
  if (bytes_out) *bytes_out = bytes / static_cast<std::size_t>(iters);
  return static_cast<double>(ns) / iters;
}

void Report(const char* name, double ns, std::size_t bytes) {
  std::printf("%-26s %8.0f ns/rec  %10.0f rec/s", name, ns, 1e9 / ns);
  if (bytes) std::printf("  (payload %zu B)", bytes);
  std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  const int iters = argc > 1 ? std::atoi(argv[1]) : 200000;
  const int rounds = 3;

  struct Case {
    const char* name;
    bool io;
    bool text;
    bool redact;
  };
  const std::vector<Case> cases = {
      {"json + 脱敏 + 落盘", true, false, true},
      {"text + 脱敏 + 落盘", true, true, true},
      {"json + 脱敏（无 I/O）", false, false, true},
      {"text + 脱敏（无 I/O）", false, true, true},
      {"json 无脱敏 + 落盘", true, false, false},
  };

  for (const auto& c : cases) {
    double best = 0;
    std::size_t bytes = 0;
    for (int r = 0; r < rounds; ++r) {
      std::size_t b = 0;
      const double ns = BenchOnce(iters, c.io, c.text, c.redact, &b);
      if (best == 0 || ns < best) {
        best = ns;
        bytes = b;
      }
    }
    Report(c.name, best, bytes);
  }

  // 脱敏子项（定位"贵在哪"，便于判断优化是否有据）
  const auto r = logging::Redactor::FromCommaSeparated(kRedactKeys);
  const std::string clean = "request completed for partition dp1 tenant t1 no secrets";
  const std::string query = "GET /b?X-Amz-Signature=abcdef0123456789&X-Amz-Date=20240101";
  const std::string fname = "correlation_id";
  double t = 0;
  std::size_t sink = 0;
  for (int i = 0; i < iters; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    sink += r.Matches(fname) ? 1 : 0;
    t += static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
            .count());
  }
  std::printf("%-26s %8.1f ns/call\n", "Matches（无命中）", t / iters);
  for (const auto& [label, text] : std::vector<std::pair<const char*, std::string>>{
           {"ScrubText（无密钥）", clean}, {"ScrubText（含签名）", query}}) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) sink += r.ScrubText(text).size();
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    std::printf("%-26s %8.1f ns/call\n", label, static_cast<double>(ns) / iters);
  }
  (void)sink;
  return 0;
}
