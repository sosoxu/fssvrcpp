// =============================================================================
//  tests/framework/big_file.h —— 大文件用例的规模与 RSS 上限（可被构建方式覆盖）
// =============================================================================
//  为什么可覆盖：
//    ASan/UBSan 构建下，1 GiB 的流式用例会非常慢，且**影子内存/redzone 会抬高 RSS 基线**，
//    于是"RSS < 64 MiB"这条断言在插桩构建下不再度量同一件事。
//    因此门槛脚本（`scripts/run_sanitizers.sh`）用
//      FSS_TEST_BIG_BYTES=64MiB、FSS_TEST_RSS_LIMIT_KIB=512MiB
//    缩小规模并放宽上限；**正常构建仍是 1 GiB / 64 MiB**（那才是 C1.3/C3.4 的要求）。
//
//  ⚠️ 这是刻意的取舍，不是"绕过失败"：被缩小的那次运行不能作为 C3.4 的证据，
//     证据以 `ctest -L phase3`（正常构建）为准。
#pragma once

#include <cstdint>
#include <cstdlib>

namespace fss::test {

inline constexpr std::int64_t kGiB = 1024LL * 1024 * 1024;

inline std::int64_t BigFileBytes() {
  if (const char* env = std::getenv("FSS_TEST_BIG_BYTES")) {
    const auto value = std::strtoll(env, nullptr, 10);
    if (value > 0) return value;
  }
  return kGiB;
}

inline std::uint64_t RssLimitKib() {
  if (const char* env = std::getenv("FSS_TEST_RSS_LIMIT_KIB")) {
    const auto value = std::strtoull(env, nullptr, 10);
    if (value > 0) return value;
  }
  return 64 * 1024;  // 64 MiB
}

}  // namespace fss::test
