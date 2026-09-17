// =============================================================================
//  C3.10：BlockingIoEngine 的 pread 定位读并发安全
// =============================================================================
//  判据要求："8 线程并发读同一大文件的不同区间，结果与单线程逐一读取完全一致
//  （证明不使用共享文件偏移）"。
//
//  因此本测试显式做**基线对照**（R1）：
//    ① 单线程逐区间读  → 得到参照结果；
//    ② 8 线程并发读同一 fd 的不同区间 → 结果必须逐字节等于①。
//  如果实现用的是 `lseek`+`read`（共享偏移），并发下必然出现错位/串读。
// =============================================================================
#include <catch2/catch.hpp>

#include "temp_dir.h"

#include "common/fs/fs.h"
#include "domain/ports/ports.h"
#include "infra/io/blocking_io_engine.h"
#include "infra/io/file_sync.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using fss::infra::BlockingIoEngine;

namespace {

//  可计数的落盘接缝（C3.11 也用它；这里用于 Sync 的"真的调了"断言）
class CountingFileSync final : public fss::infra::IFileSync {
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
  int data_sync_calls = 0;
  int dir_sync_calls = 0;
};

std::string MakeContent(std::size_t bytes) {
  std::string out(bytes, '\0');
  for (std::size_t i = 0; i < bytes; ++i) {
    out[i] = static_cast<char>('A' + (i % 26));
  }
  return out;
}

}  // namespace

TEST_CASE("★ C3.10 ReadAt：8 线程并发读不同区间 == 单线程基线（不共享文件偏移）",
          "[phase3][io][c3.10]") {
  fss::test::TempDir dir("io_readat");
  const std::string path = dir.child("big.bin");
  const std::string content = MakeContent(256 * 1024);
  REQUIRE(fss::fs::AtomicWriteFile(path, content).ok());

  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE(fd >= 0);

  BlockingIoEngine engine;
  constexpr std::size_t kChunk = 32 * 1024;
  constexpr int kThreads = 8;

  //  ① 单线程参照（逐区间顺序读）
  std::vector<std::string> baseline(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    baseline[i].resize(kChunk);
    const auto read = engine.ReadAt(fd, static_cast<std::uint64_t>(i) * kChunk,
                                    baseline[i].data(), kChunk);
    REQUIRE(read.ok());
    REQUIRE(read.value() == kChunk);
    REQUIRE(baseline[i] == content.substr(static_cast<std::size_t>(i) * kChunk, kChunk));
  }

  //  ② 8 线程并发读同一 fd 的不同区间
  std::vector<std::string> concurrent(kThreads);
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i] {
      std::string buffer(kChunk, '\0');
      //  每个线程反复读自己的区间，制造足够的交错机会
      for (int round = 0; round < 50; ++round) {
        const auto read = engine.ReadAt(fd, static_cast<std::uint64_t>(i) * kChunk,
                                        buffer.data(), kChunk);
        if (!read.ok() || read.value() != kChunk) {
          ++failures;
          return;
        }
        if (buffer != baseline[i]) {
          ++failures;
          return;
        }
      }
      concurrent[i] = buffer;
    });
  }
  for (auto& thread : threads) thread.join();
  REQUIRE(failures.load() == 0);
  for (int i = 0; i < kThreads; ++i) {
    REQUIRE(concurrent[i] == baseline[i]);
  }

  ::close(fd);
}

TEST_CASE("C3.10 ReadAt/WriteAt 的 64 位偏移与短读语义", "[phase3][io][c3.10]") {
  fss::test::TempDir dir("io_offsets");
  const std::string path = dir.child("obj.bin");
  REQUIRE(fss::fs::AtomicWriteFile(path, std::string(64, '\0')).ok());

  const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  REQUIRE(fd >= 0);
  BlockingIoEngine engine;

  //  写入超过 4 GiB 的偏移（不落盘数据，只验证签名与算术是 64 位）
  const char probe = 'Z';
  const std::uint64_t far_offset = (std::uint64_t{5} << 30);  // 5 GiB
  const auto written = engine.WriteAt(fd, far_offset, &probe, 1);
  REQUIRE(written.ok());
  REQUIRE(written.value() == 1);

  char read_back = '\0';
  const auto read = engine.ReadAt(fd, far_offset, &read_back, 1);
  REQUIRE(read.ok());
  REQUIRE(read.value() == 1);
  REQUIRE(read_back == 'Z');

  //  短读：越过 EOF 只返回实际字节数（不是错误）
  const auto eof = engine.ReadAt(fd, far_offset + 1, &read_back, 8);
  REQUIRE(eof.ok());
  REQUIRE(eof.value() == 0);

  //  文件大小反映了远端写入
  const auto size = engine.FileSize(fd);
  REQUIRE(size.ok());
  REQUIRE(size.value() == far_offset + 1);

  //  Sync 走注入的接缝（"真的调了"可被观察）
  CountingFileSync counter;
  BlockingIoEngine counted(&counter);
  REQUIRE(counted.Sync(fd).ok());
  REQUIRE(counter.data_sync_calls == 1);

  ::close(fd);
}

TEST_CASE("C3.10 capabilities：默认引擎声明为 blocking / 非异步 / 无对齐要求",
          "[phase3][io][c3.10]") {
  BlockingIoEngine engine;
  const auto caps = engine.capabilities();
  REQUIRE(caps.engine_name == "blocking");
  REQUIRE_FALSE(caps.async);
  REQUIRE_FALSE(caps.direct_io);
  REQUIRE(caps.alignment == 0);
}
