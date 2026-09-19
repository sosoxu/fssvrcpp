// =============================================================================
//  C3.11：`fsync` 分级策略（`fsync_policy=by_size`）的**可观测**验证
// =============================================================================
//  判据要求"用系统调用计数或注入的 `IFileSync` 端口断言，不靠猜测"。
//  因此这里注入 `CountingFileSync`：测试直接数"数据落盘做了几次 / 目录落盘做了几次"。
//
//  依据：实测 `synchronous=FULL` 比 `NORMAL` 差 **21 倍**（R-15）—— 逐文件 fsync
//  会把小文件写入上限压到 ~1.2k/s，所以策略必须能在配置层选择。
// =============================================================================
#include <catch2/catch.hpp>

#include "temp_dir.h"

#include "common/bytes/bytes.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/blob/posix/posix_blob_store.h"
#include "infra/io/file_sync.h"

#include <string>

using fss::domain::ObjectRef;
using fss::domain::PutOptions;
using fss::infra::FsyncPolicy;
using fss::infra::IFileSync;
using fss::infra::PosixBlobStore;
using fss::infra::PosixBlobStoreOptions;
using fss::infra::ShouldFsync;

namespace {

class CountingFileSync final : public IFileSync {
 public:
  fss::Result<void> DataSync(int fd) override {
    (void)fd;
    ++data_sync_calls;
    return fss::Ok();
  }
  fss::Result<void> SyncDirectory(const std::string& directory) override {
    (void)directory;
    ++dir_sync_calls;
    return fss::Ok();
  }
  //  ADR-008 的 P4 接缝：本用例不进入批提交路径，因此它**不应**被调用。
  fss::Result<void> SyncFilesystem(const std::string& directory) override {
    (void)directory;
    ++syncfs_calls;
    return fss::Ok();
  }
  int data_sync_calls = 0;
  int dir_sync_calls = 0;
  int syncfs_calls = 0;
};

ObjectRef Ref(const std::string& key) {
  ObjectRef ref;
  ref.container = "c";
  ref.key = key;
  return ref;
}

}  // namespace

TEST_CASE("★ C3.11 纯策略：ShouldFsync 的阈值语义（含边界与非法阈值）",
          "[phase3][fsync][c3.11]") {
  //  always / never 与大小无关
  REQUIRE(ShouldFsync(FsyncPolicy::kAlways, 1, 1024));
  REQUIRE_FALSE(ShouldFsync(FsyncPolicy::kNever, 1 << 30, 1024));
  //  by_size：严格小于阈值 → 不落盘；**等于**阈值 → 落盘（边界取">="）
  REQUIRE_FALSE(ShouldFsync(FsyncPolicy::kBySize, 1023, 1024));
  REQUIRE(ShouldFsync(FsyncPolicy::kBySize, 1024, 1024));
  REQUIRE(ShouldFsync(FsyncPolicy::kBySize, 4096, 1024));
  //  阈值 <= 0 视为"全都要落盘"（朝更安全的方向退化，而不是"永不落盘"）
  REQUIRE(ShouldFsync(FsyncPolicy::kBySize, 1, 0));
  REQUIRE(ShouldFsync(FsyncPolicy::kBySize, 1, -5));
}

TEST_CASE("★ C3.11 by_size：小文件不落盘、大文件（≥阈值）必须落盘",
          "[phase3][fsync][c3.11]") {
  fss::test::TempDir dir("fsync_by_size");
  fss::ManualClock clock;
  CountingFileSync counter;

  PosixBlobStoreOptions options;
  options.fsync_policy = FsyncPolicy::kBySize;
  options.fsync_threshold_bytes = 1024;
  options.file_sync = &counter;
  PosixBlobStore store(dir.str(), clock, options);
  REQUIRE(store.ensure_container("c").ok());

  //  小文件：< 阈值 → 一次都不落盘
  fss::bytes::StringSource small(std::string(100, 's'));
  REQUIRE(store.put(Ref("small.bin"), small, PutOptions{}).ok());
  REQUIRE(counter.data_sync_calls == 0);
  REQUIRE(counter.dir_sync_calls == 0);

  //  大文件：>= 阈值 → 数据与目录各落盘一次
  fss::bytes::StringSource large(std::string(4096, 'L'));
  REQUIRE(store.put(Ref("large.bin"), large, PutOptions{}).ok());
  REQUIRE(counter.data_sync_calls == 1);
  REQUIRE(counter.dir_sync_calls == 1);

  //  策略只影响"何时落盘"，不影响可读性（R16：正例对照）
  fss::bytes::StringSink sink;
  REQUIRE(store.get(Ref("small.bin"), sink, fss::domain::ByteRange{}).ok());
  REQUIRE(sink.str() == std::string(100, 's'));
}

TEST_CASE("★ C3.11 always / never 两档：与对象大小无关", "[phase3][fsync][c3.11]") {
  {
    fss::test::TempDir dir("fsync_always");
    fss::ManualClock clock;
    CountingFileSync counter;
    PosixBlobStoreOptions options;
    options.fsync_policy = FsyncPolicy::kAlways;
    options.file_sync = &counter;
    PosixBlobStore store(dir.str(), clock, options);
    REQUIRE(store.ensure_container("c").ok());
    fss::bytes::StringSource source(std::string(10, 'x'));
    REQUIRE(store.put(Ref("tiny.bin"), source, PutOptions{}).ok());
    REQUIRE(counter.data_sync_calls == 1);
    REQUIRE(counter.dir_sync_calls == 1);
  }
  {
    fss::test::TempDir dir("fsync_never");
    fss::ManualClock clock;
    CountingFileSync counter;
    PosixBlobStoreOptions options;
    options.fsync_policy = FsyncPolicy::kNever;
    options.file_sync = &counter;
    PosixBlobStore store(dir.str(), clock, options);
    REQUIRE(store.ensure_container("c").ok());
    fss::bytes::StringSource source(std::string(1 << 16, 'y'));
    REQUIRE(store.put(Ref("big.bin"), source, PutOptions{}).ok());
    REQUIRE(counter.data_sync_calls == 0);
    REQUIRE(counter.dir_sync_calls == 0);
  }
}

TEST_CASE("C3.11 复制路径同样遵守策略", "[phase3][fsync][c3.11]") {
  fss::test::TempDir dir("fsync_copy");
  fss::ManualClock clock;
  CountingFileSync counter;
  PosixBlobStoreOptions options;
  options.fsync_policy = FsyncPolicy::kBySize;
  options.fsync_threshold_bytes = 1024;
  options.file_sync = &counter;
  PosixBlobStore store(dir.str(), clock, options);
  REQUIRE(store.ensure_container("c").ok());
  REQUIRE(store.ensure_container("d").ok());

  fss::bytes::StringSource source(std::string(4096, 'C'));
  REQUIRE(store.put(Ref("src.bin"), source, PutOptions{}).ok());
  const int before = counter.data_sync_calls;

  ObjectRef destination = Ref("dst.bin");
  destination.container = "d";
  REQUIRE(store.copy(Ref("src.bin"), destination).ok());
  REQUIRE(counter.data_sync_calls == before + 1);  // 大对象复制同样落盘
}
