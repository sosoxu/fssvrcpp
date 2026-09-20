// =============================================================================
//  PosixFileByteSource（L2）—— 持有**原生 fd** 的字节源（ADR-006 的零拷贝来源）
// =============================================================================
//  与 `BlobByteSource` 的分工
//    · `BlobByteSource` 把 `IBlobStore::get()` 包成 `ByteSource`：语义完整、可寻址，
//      但每次 `Read` 都经过"推式 sink"这一层，且**没有**可 `sendfile` 的 fd。
//    · 本类是 POSIX 驱动在"对象就是一个本地文件"时给出的**直读**来源：`pread` 读，
//      并暴露 fd 供传输层 `sendfile(2)` 零拷贝送出。
//  两者对上层是**同一个** `bytes::ByteSource`：控制面只调 `Read`/`Seek`，因此
//  组合根优先取原生源时控制面行为不变（用例 P1/P8 的等价性就是这条的判据）。
//
//  语义必须与 `BlobByteSource` **逐条一致**（否则两条来源会在 Range 上分叉）
//    · `Size()` = `fstat` 的大小；`Seekable()` = true；
//    · `Seek(offset)`：`offset < 0 || offset > size` → `kInvalidArgument` "Seek 越界"
//      （`== size` 合法，读到 0 字节）；
//    · `Read` 短读是允许的；返回 0 = EOF；EINTR 重试；`capacity == 0` → 0。
//
//  ★ `NativeFd()` 的所有权契约（见 `common/bytes/bytes.h`）：fd 归本对象所有，
//    调用方**不得** close；本对象析构时 RAII `::close`。
//  ★ `storage.posix.fadvise_random`：与 `PosixBlobStore::get()` 的读路径一样，
//    打开后（配置为 true 时）下发一次 `POSIX_FADV_RANDOM`；false = no-op。
//    （驱动里的可注入 `IFadviseSink` 是**测试接缝**，不跨层传到这里 —— 真实系统调用
//    与 `get()` 路径完全相同。）
// =============================================================================
#pragma once

#include "common/bytes/bytes.h"
#include "common/result/result.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace fss::infra {

class PosixFileByteSource final : public bytes::ByteSource {
 public:
  //  打开 `path`（`O_RDONLY | O_CLOEXEC`）+ `fstat`。失败 → `Err`（ENOENT → kNotFound）。
  static fss::Result<std::shared_ptr<PosixFileByteSource>> Open(std::string path,
                                                                bool fadvise_random);

  ~PosixFileByteSource() override;
  PosixFileByteSource(const PosixFileByteSource&) = delete;
  PosixFileByteSource& operator=(const PosixFileByteSource&) = delete;

  fss::Result<std::size_t> Read(char* out, std::size_t capacity) override;
  std::optional<std::int64_t> Size() const override { return size_; }
  bool Seekable() const override { return true; }
  fss::Result<void> Seek(std::int64_t offset) override;
  int NativeFd() const override { return fd_; }

  const std::string& path() const { return path_; }

 private:
  PosixFileByteSource(int fd, std::int64_t size, std::string path)
      : fd_(fd), size_(size), path_(std::move(path)) {}

  int fd_ = -1;
  std::int64_t size_ = 0;
  std::uint64_t offset_ = 0;
  std::string path_;
};

}  // namespace fss::infra
