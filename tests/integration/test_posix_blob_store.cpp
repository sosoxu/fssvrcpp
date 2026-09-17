// =============================================================================
//  C3.1/C3.2/C3.3/C3.5/C3.6：PosixBlobStore
// =============================================================================
//  ★ 关键点：这里**实例化的是与内存实现相同的契约测试**
//    （`CheckBlobStoreContract`）。"看起来能用"不算数，行为一致才算数（C3.1）。
//  另外覆盖集中存储**特有**的风险：路径穿越/符号链接逃逸（C3.2）、
//  崩溃不产生半截文件（C3.3）、Range 边界（C3.5）、并发（C3.6）。
// =============================================================================
#include <catch2/catch.hpp>

#include "port_contract.h"
#include "temp_dir.h"

#include "common/bytes/bytes.h"
#include "common/fs/fs.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/blob/posix/posix_blob_store.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using fss::domain::ObjectRef;
using fss::domain::PutOptions;
using fss::infra::PosixBlobStore;

namespace {

ObjectRef Ref(const std::string& container, const std::string& key) {
  ObjectRef ref;
  ref.container = container;
  ref.key = key;
  return ref;
}

//  模拟"写到一半源就断了"：put 必须失败，且**目标路径不出现半截文件**（C3.3）
class FailingSource final : public fss::bytes::ByteSource {
 public:
  fss::Result<std::size_t> Read(char* out, std::size_t capacity) override {
    if (first_) {
      first_ = false;
      const std::size_t n = std::min<std::size_t>(capacity, 10000);
      std::memset(out, 'x', n);
      return n;
    }
    return fss::Err(fss::ErrorKind::kUnavailable, "注入的源端故障");
  }

 private:
  bool first_ = true;
};

}  // namespace

TEST_CASE("★ C3.1 PosixBlobStore 通过与内存实现**共用的同一套契约测试**",
          "[phase3][contract][c3.1]") {
  fss::test::TempDir dir("posix_contract");
  fss::ManualClock clock;
  PosixBlobStore store(dir.str(), clock);
  fss::test::CheckBlobStoreContract(store);
}

TEST_CASE("★ C3.2 路径安全：≥12 个恶意键全部被拒", "[phase3][posix][c3.2]") {
  fss::test::TempDir dir("posix_paths");
  fss::ManualClock clock;
  PosixBlobStore store(dir.str(), clock);
  REQUIRE(store.ensure_container("c").ok());

  const std::vector<std::pair<std::string, std::string>> malicious = {
      {"空键", ""},
      {"当前目录", "."},
      {"父目录", ".."},
      {"父目录逃逸", "../escape"},
      {"中层穿越", "a/../../b"},
      {"绝对路径", "/etc/passwd"},
      {"绝对路径（深层）", "a/b//etc/passwd"},
      {"空段", "a//b"},
      {"点段", "a/./b"},
      {"以斜杠结尾", "a/"},
      {"NUL 字节", std::string("a\0b", 3)},
      {"超长键", std::string(4096, 'a')},
      {"尾随点段", "a/.."},
      {"连续穿越", "a/../../../../etc/shadow"},
  };
  REQUIRE(malicious.size() >= 12);

  for (const auto& [name, key] : malicious) {
    fss::bytes::StringSource src("x");
    const auto put = store.put(Ref("c", key), src, PutOptions{});
    INFO("put 恶意键：" << name);
    REQUIRE_FALSE(put.ok());
    REQUIRE(put.error().kind() == fss::ErrorKind::kInvalidArgument);

    fss::bytes::StringSink sink;
    const auto get = store.get(Ref("c", key), sink, fss::domain::ByteRange{});
    INFO("get 恶意键：" << name);
    REQUIRE_FALSE(get.ok());
    REQUIRE(get.error().kind() == fss::ErrorKind::kInvalidArgument);
  }
}

TEST_CASE("★ C3.2 符号链接逃逸：词法合法但真实路径在 root 之外", "[phase3][posix][c3.2]") {
  fss::test::TempDir dir("posix_symlink");
  fss::ManualClock clock;
  PosixBlobStore store(dir.str(), clock);
  REQUIRE(store.ensure_container("c").ok());

  const std::string outside = dir.child("outside");
  std::filesystem::create_directories(outside);
  const std::string link = store.root() + "/c/link";
  std::filesystem::create_directory_symlink(outside, link);

  //  "link/evil" 词法上完全合法（无 `..`、无绝对路径），但解析后落在 root 之外
  fss::bytes::StringSource src("x");
  const auto put = store.put(Ref("c", "link/evil"), src, PutOptions{});
  REQUIRE_FALSE(put.ok());
  REQUIRE(put.error().kind() == fss::ErrorKind::kInvalidArgument);
  //  目标文件确实没有在外部目录里被创建
  REQUIRE_FALSE(std::filesystem::exists(outside + "/evil"));
}

TEST_CASE("★ C3.3 原子写：写入中途失败 → 目标路径不出现半截文件", "[phase3][posix][c3.3]") {
  fss::test::TempDir dir("posix_atomic");
  fss::ManualClock clock;
  PosixBlobStore store(dir.str(), clock);
  REQUIRE(store.ensure_container("c").ok());

  FailingSource source;
  const auto put = store.put(Ref("c", "half.bin"), source, PutOptions{});
  REQUIRE_FALSE(put.ok());
  REQUIRE(put.error().kind() == fss::ErrorKind::kUnavailable);

  //  目标路径不存在（"没有半截文件"）
  const auto st = store.stat(Ref("c", "half.bin"));
  REQUIRE(st.ok());
  REQUIRE_FALSE(st.value().exists);

  //  临时文件也被清理干净（否则会在共享盘上残留垃圾、被 list 看到）
  std::size_t leftovers = 0;
  for (const auto& entry : std::filesystem::directory_iterator(store.root() + "/c")) {
    if (entry.path().filename().string().find(".tmp.") != std::string::npos) ++leftovers;
  }
  REQUIRE(leftovers == 0);

  //  list 不会把内部文件当对象
  const auto page = store.list("c", "", "", 100);
  REQUIRE(page.ok());
  REQUIRE(page.value().entries.empty());
}

TEST_CASE("★ C3.5 Range 边界：首字节/末字节/越界/超长", "[phase3][posix][c3.5]") {
  fss::test::TempDir dir("posix_range");
  fss::ManualClock clock;
  PosixBlobStore store(dir.str(), clock);
  REQUIRE(store.ensure_container("c").ok());

  fss::bytes::StringSource src("0123456789");
  REQUIRE(store.put(Ref("c", "r"), src, PutOptions{}).ok());

  const auto read_range = [&](std::uint64_t offset, std::uint64_t length) {
    fss::bytes::StringSink sink;
    const auto result = store.get(Ref("c", "r"), sink, fss::domain::ByteRange{offset, length});
    return std::make_pair(result.ok(), sink.str());
  };

  REQUIRE(read_range(0, 1) == std::make_pair(true, std::string("0")));   // bytes=0-0
  REQUIRE(read_range(9, 1) == std::make_pair(true, std::string("9")));   // bytes=9-9
  REQUIRE(read_range(9, 0) == std::make_pair(true, std::string("9")));   // bytes=9-
  REQUIRE(read_range(0, 0) == std::make_pair(true, std::string("0123456789")));
  REQUIRE(read_range(5, 100) == std::make_pair(true, std::string("56789")));  // 末端截断

  fss::bytes::StringSink sink;
  const auto out_of_range = store.get(Ref("c", "r"), sink, fss::domain::ByteRange{10, 0});
  REQUIRE_FALSE(out_of_range.ok());
  REQUIRE(out_of_range.error().kind() == fss::ErrorKind::kInvalidArgument);
}

TEST_CASE("★ C3.6 并发：8 线程写不同键 + 并发读同一键，全部正确", "[phase3][posix][c3.6]") {
  fss::test::TempDir dir("posix_concurrency");
  fss::ManualClock clock;
  PosixBlobStore store(dir.str(), clock);
  REQUIRE(store.ensure_container("c").ok());

  //  共享只读对象
  const std::string shared = "shared-payload-0123456789";
  {
    fss::bytes::StringSource src(shared);
    REQUIRE(store.put(Ref("c", "shared"), src, PutOptions{}).ok());
  }

  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&, i] {
      for (int j = 0; j < 20; ++j) {
        const std::string key = "thr" + std::to_string(i) + "/" + std::to_string(j);
        const std::string value = "value-" + std::to_string(i) + "-" + std::to_string(j);
        fss::bytes::StringSource src(value);
        if (!store.put(Ref("c", key), src, PutOptions{}).ok()) {
          ++failures;
          continue;
        }
        fss::bytes::StringSink sink;
        if (!store.get(Ref("c", key), sink, fss::domain::ByteRange{}).ok() ||
            sink.str() != value) {
          ++failures;
        }
        //  同时并发读同一对象（验证 pread 不共享文件偏移）
        fss::bytes::StringSink shared_sink;
        if (!store.get(Ref("c", "shared"), shared_sink, fss::domain::ByteRange{7, 7}).ok() ||
            shared_sink.str() != "payload") {
          ++failures;
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  REQUIRE(failures.load() == 0);

  const auto page = store.list("c", "thr", "", 1000);
  REQUIRE(page.ok());
  REQUIRE(page.value().entries.size() == 8 * 20);
  REQUIRE_FALSE(page.value().truncated);
}
