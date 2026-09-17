// =============================================================================
//  BlobByteSource（L2）—— 把 `IBlobStore` 的对象包成"可定位读的 ByteSource"
// =============================================================================
//  数据面下载用它：`Response::Stream` 需要的是 `bytes::ByteSource`，而存储侧只有
//  `get(ref, sink, range)`。这里把两者接起来，并且**每次只读一段**
//  （固定 64 KiB 缓冲），因此 RSS 与对象大小无关（C1.3/C4.8）。
//
//  为什么放在 L2：它依赖 `IBlobStore`（L3 端口）与 `bytes`（L1），是**存储侧**的适配。
//  适配层（L5）不能依赖 L2，因此适配层只持有 `std::shared_ptr<bytes::ByteSource>`
//  —— 由组合根把两者绑起来。
#pragma once

#include "common/bytes/bytes.h"
#include "common/result/result.h"
#include "domain/ports/ports.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

namespace fss::infra {

class BlobByteSource final : public bytes::ByteSource {
 public:
  BlobByteSource(domain::IBlobStore& store, domain::ObjectRef ref, std::int64_t size)
      : store_(store), ref_(std::move(ref)), size_(size) {}

  fss::Result<std::size_t> Read(char* out, std::size_t capacity) override {
    if (out == nullptr || capacity == 0 || offset_ >= static_cast<std::uint64_t>(size_)) {
      return std::size_t{0};
    }
    bytes::BufferSink sink(out, capacity);
    FSS_TRY(store_.get(ref_, sink, domain::ByteRange{offset_, capacity}));
    offset_ += sink.written();
    return sink.written();
  }

  std::optional<std::int64_t> Size() const override { return size_; }
  bool Seekable() const override { return true; }

  fss::Result<void> Seek(std::int64_t offset) override {
    if (offset < 0 || offset > size_) {
      return Err(fss::ErrorKind::kInvalidArgument, "Seek 越界");
    }
    offset_ = static_cast<std::uint64_t>(offset);
    return Ok();
  }

 private:
  domain::IBlobStore& store_;
  domain::ObjectRef ref_;
  std::int64_t size_ = 0;
  std::uint64_t offset_ = 0;
};

}  // namespace fss::infra
