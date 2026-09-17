// =============================================================================
//  C3.4：大文件流式写读 —— 进程 RSS 峰值增长 < 64 MiB（证明不整块驻留）
// =============================================================================
//  这条判据要区分两种实现：
//    · 流式：固定缓冲循环搬运 → RSS 与对象大小**无关**；
//    · 整块驻留：把整个对象读进内存/字符串 → RSS ≈ 对象大小（1 GiB 会直接把预算打爆）。
//  因此断言不能只看"能写能读"（两者都能），必须**测 RSS 峰值增长**。
//
//  ⚠️ sanitizer 构建下用 `FSS_TEST_BIG_BYTES` 缩小规模（影子内存会抬高基线），
//     该次运行不作为 C3.4 证据；证据是正常构建下的 1 GiB 运行。
// =============================================================================
#include <catch2/catch.hpp>

#include "big_file.h"
#include "raw_http.h"
#include "temp_dir.h"

#include "common/bytes/bytes.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/blob/posix/posix_blob_store.h"

#include <algorithm>
#include <cstdint>
#include <string>

using fss::domain::ObjectRef;
using fss::domain::PutOptions;
using fss::infra::PosixBlobStore;

namespace {

ObjectRef Ref(const std::string& key) {
  ObjectRef ref;
  ref.container = "c";
  ref.key = key;
  return ref;
}

}  // namespace

TEST_CASE("★ C3.4 大文件：流式写入 + 读回，RSS 峰值增长 < 64 MiB", "[phase3][posix][c3.4]") {
  fss::test::TempDir dir("posix_bigfile");
  fss::ManualClock clock;
  PosixBlobStore store(dir.str(), clock);
  REQUIRE(store.ensure_container("c").ok());

  const std::int64_t bytes = fss::test::BigFileBytes();
  INFO("对象大小 " << bytes << " 字节");
  REQUIRE(bytes >= 1);  // 默认 1 GiB；sanitizer 下可缩小（见 framework/big_file.h）

  //  ① 写入：数据来自 RepeatingSource（确定性、**不分配整个对象**）
  const std::uint64_t baseline_hwm = fss::test::PeakRssKib();
  {
    fss::bytes::RepeatingSource source(bytes);
    const auto put = store.put(Ref("big.bin"), source, PutOptions{});
    REQUIRE(put.ok());
  }
  const std::uint64_t after_put_hwm = fss::test::PeakRssKib();
  REQUIRE(after_put_hwm >= baseline_hwm);  // VmHWM 单调不减（前置条件显式断言）

  //  ② 读回：CountingSink 只计数、不保留数据（否则测试自己会占 1 GiB）
  {
    fss::bytes::CountingSink sink;
    const auto get = store.get(Ref("big.bin"), sink, fss::domain::ByteRange{});
    REQUIRE(get.ok());
    REQUIRE(sink.bytes_written() == static_cast<std::uint64_t>(bytes));
  }
  const std::uint64_t after_get_hwm = fss::test::PeakRssKib();

  //  ③ 大小与校验和（校验和是**边写边算**的，证明不是写完后整块读回来算）
  const auto st = store.stat(Ref("big.bin"));
  REQUIRE(st.ok());
  REQUIRE(st.value().exists);
  REQUIRE(st.value().size == bytes);
  REQUIRE_FALSE(st.value().checksum.empty());

  //  ④ 只读区间也要正确（不能因为超过某个内部分块而错位）
  {
    fss::bytes::StringSink tail;
    const std::uint64_t offset = static_cast<std::uint64_t>(bytes) - 16;
    const auto read = store.get(Ref("big.bin"), tail, fss::domain::ByteRange{offset, 16});
    REQUIRE(read.ok());
    REQUIRE(tail.str().size() == 16);
  }

  //  VmHWM 单调不减，after_get 即整个过程（写 + 读）的峰值
  const std::uint64_t growth_kib = after_get_hwm - baseline_hwm;
  INFO("RSS 峰值增长 " << growth_kib << " KiB（上限 " << fss::test::RssLimitKib() << " KiB）");
  REQUIRE(growth_kib < fss::test::RssLimitKib());
}
