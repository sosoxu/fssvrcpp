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

#include "common/bytes/bytes.h"
#include "common/crypto/crypto.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

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

//  "重复模式"字节源的 SHA-256：**流式**算出，测试用它做独立期望值
//  （不抄实现里的任何常量；`bytes::RepeatingSource` 的默认模式与生产代码无关）
inline std::string RepeatingPatternDigest(std::int64_t total) {
  fss::crypto::Hasher hasher(fss::crypto::ChecksumAlgorithm::kSha256);
  fss::bytes::RepeatingSource source(total);
  std::vector<char> buffer(1024 * 1024);
  while (true) {
    const auto read = source.Read(buffer.data(), buffer.size());
    if (!read.ok() || read.value() == 0) break;
    hasher.Update(std::string_view(buffer.data(), read.value()));
  }
  return hasher.HexDigest();
}

//  `RepeatingSource` 在 `[offset, offset+length)` 上的 SHA-256（区间读的期望值）
inline std::string RepeatingPatternDigestAt(std::int64_t offset, std::int64_t length) {
  fss::crypto::Hasher hasher(fss::crypto::ChecksumAlgorithm::kSha256);
  fss::bytes::RepeatingSource source(offset + length);
  std::vector<char> buffer(1024 * 1024);
  std::int64_t skip = offset;
  while (skip > 0) {
    const auto want = std::min<std::int64_t>(skip, static_cast<std::int64_t>(buffer.size()));
    const auto read = source.Read(buffer.data(), static_cast<std::size_t>(want));
    if (!read.ok() || read.value() == 0) return {};
    skip -= static_cast<std::int64_t>(read.value());
  }
  std::int64_t remaining = length;
  while (remaining > 0) {
    const auto want = std::min<std::int64_t>(remaining, static_cast<std::int64_t>(buffer.size()));
    const auto read = source.Read(buffer.data(), static_cast<std::size_t>(want));
    if (!read.ok() || read.value() == 0) break;
    hasher.Update(std::string_view(buffer.data(), read.value()));
    remaining -= static_cast<std::int64_t>(read.value());
  }
  return hasher.HexDigest();
}

}  // namespace fss::test
