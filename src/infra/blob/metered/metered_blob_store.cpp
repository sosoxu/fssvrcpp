// MeteredBlobStore 实现。理由见头文件。
#include "infra/blob/metered/metered_blob_store.h"

#include <atomic>

#include <cstdint>
#include <string_view>

namespace fss::infra {

namespace {

//  计数用的 ByteSource：只统计"从上游读进来多少字节"（put 的入流量）
class CountingSource final : public fss::bytes::ByteSource {
 public:
  CountingSource(fss::bytes::ByteSource& inner, std::atomic<std::int64_t>& counter)
      : inner_(inner), counter_(counter) {}
  fss::Result<std::size_t> Read(char* out, std::size_t capacity) override {
    FSS_TRY(read, inner_.Read(out, capacity));
    counter_.fetch_add(static_cast<std::int64_t>(read), std::memory_order_relaxed);
    return read;
  }
  std::optional<std::int64_t> Size() const override { return inner_.Size(); }
  bool Seekable() const override { return inner_.Seekable(); }
  fss::Result<void> Seek(std::int64_t offset) override { return inner_.Seek(offset); }

 private:
  fss::bytes::ByteSource& inner_;
  std::atomic<std::int64_t>& counter_;
};

//  计数用的 ByteSink：统计"写给下游多少字节"（get 的出流量）
class CountingSink final : public fss::bytes::ByteSink {
 public:
  CountingSink(fss::bytes::ByteSink& inner, std::atomic<std::int64_t>& counter)
      : inner_(inner), counter_(counter) {}
  fss::Result<void> Write(std::string_view data) override {
    FSS_TRY(inner_.Write(data));
    counter_.fetch_add(static_cast<std::int64_t>(data.size()), std::memory_order_relaxed);
    return Ok();
  }
  fss::Result<void> Close() override { return inner_.Close(); }

 private:
  fss::bytes::ByteSink& inner_;
  std::atomic<std::int64_t>& counter_;
};

//  ★ ADR-006：给"持有原生 fd 的来源"套上**同样的**存储计量。
//    为什么必须有它：组合根优先返回原生 fd 来源之后，控制面的 `StreamPump` 走的是
//    `ByteSource::Read`（`pread`），**不再**经过 `IBlobStore::get()` —— 若不在这里
//    计数，控制面下载的 `fss_storage_bytes_total{direction="out"}` 会**静默归零**。
//    规则（与数据面的 `RecordNativeRead` 精确互补，二者**互斥**）：
//      · 用户态拷贝（Read 被调用）→ 这里记字节，并在析构时记一次 `op=get`；
//      · `sendfile`（Read 从不被调用）→ 这里什么都不记，由数据面调 `RecordNativeRead`。
//    因此"计数点移动"只发生在 **sendfile 这一条路**上，其余路径的计数语义与接线前一致。
class MeteredNativeSource final : public fss::bytes::ByteSource {
 public:
  MeteredNativeSource(std::shared_ptr<fss::bytes::ByteSource> inner,
                      fss::metrics::Registry& registry, std::string driver)
      : inner_(std::move(inner)), registry_(registry), driver_(std::move(driver)) {}

  ~MeteredNativeSource() override {
    //  只有真的发生过用户态拷贝才记 `op=get`：sendfile 路径由数据面记（否则会重复）。
    if (read_calls_ > 0) {
      registry_.Increment("fss_storage_operations_total",
                          {{"driver", driver_}, {"op", "get"}, {"outcome", "ok"}});
    }
  }

  fss::Result<std::size_t> Read(char* out, std::size_t capacity) override {
    ++read_calls_;
    FSS_TRY(read, inner_->Read(out, capacity));
    if (read > 0) {
      const auto bytes = static_cast<std::int64_t>(read);
      registry_.Increment("fss_storage_bytes_total", {{"direction", "out"}}, bytes);
      bytes_read_ += bytes;
    }
    return read;
  }
  std::optional<std::int64_t> Size() const override { return inner_->Size(); }
  bool Seekable() const override { return inner_->Seekable(); }
  fss::Result<void> Seek(std::int64_t offset) override { return inner_->Seek(offset); }
  //  ★ 必须转发：否则数据面拿不到 fd，零拷贝被**静默**关掉（P5 会抓到）。
  int NativeFd() const override { return inner_->NativeFd(); }

 private:
  std::shared_ptr<fss::bytes::ByteSource> inner_;
  fss::metrics::Registry& registry_;
  std::string driver_;
  std::int64_t bytes_read_ = 0;
  int read_calls_ = 0;
};

}  // namespace

MeteredBlobStore::MeteredBlobStore(domain::IBlobStore& inner, fss::metrics::Registry& registry,
                                   std::string driver_name)
    : inner_(inner), registry_(registry), driver_(std::move(driver_name)) {
  //  注册（幂等）：没有 HELP/TYPE 的指标会被 `/metrics` 的测试判失败
  registry_.Register("fss_storage_operations_total", fss::metrics::Registry::Kind::kCounter,
                     "存储操作次数（按驱动/操作/结果）");
  registry_.Register("fss_storage_bytes_total", fss::metrics::Registry::Kind::kCounter,
                     "流经存储的字节数（direction=in|out）");
}

void MeteredBlobStore::Record(std::string_view op, bool ok) const {
  registry_.Increment("fss_storage_operations_total",
                      {{"driver", driver_}, {"op", std::string(op)},
                       {"outcome", ok ? "ok" : "error"}});
}

fss::Result<void> MeteredBlobStore::ensure_container(const std::string& container) {
  const auto result = inner_.ensure_container(container);
  Record("ensure_container", result.ok());
  return result;
}

fss::Result<domain::SignedLocation> MeteredBlobStore::presign_put(
    const domain::ObjectRef& ref, const domain::PresignOptions& options) {
  const auto result = inner_.presign_put(ref, options);
  Record("presign_put", result.ok());
  return result;
}

fss::Result<domain::SignedLocation> MeteredBlobStore::presign_get(
    const domain::ObjectRef& ref, const domain::PresignOptions& options) {
  const auto result = inner_.presign_get(ref, options);
  Record("presign_get", result.ok());
  return result;
}

fss::Result<void> MeteredBlobStore::put(const domain::ObjectRef& ref,
                                        fss::bytes::ByteSource& source,
                                        const domain::PutOptions& options) {
  //  ★ 用一个"每次调用私有"的计数器：并发写入不会互相干扰
  std::atomic<std::int64_t> bytes{0};
  CountingSource counting(source, bytes);
  const auto result = inner_.put(ref, counting, options);
  if (bytes.load(std::memory_order_relaxed) > 0) {
    registry_.Increment("fss_storage_bytes_total", {{"direction", "in"}},
                        bytes.load(std::memory_order_relaxed));
  }
  Record("put", result.ok());
  return result;
}

fss::Result<void> MeteredBlobStore::get(const domain::ObjectRef& ref, fss::bytes::ByteSink& sink,
                                        const domain::ByteRange& range) {
  std::atomic<std::int64_t> bytes{0};
  CountingSink counting(sink, bytes);
  const auto result = inner_.get(ref, counting, range);
  if (bytes.load(std::memory_order_relaxed) > 0) {
    registry_.Increment("fss_storage_bytes_total", {{"direction", "out"}},
                        bytes.load(std::memory_order_relaxed));
  }
  Record("get", result.ok());
  return result;
}

fss::Result<std::shared_ptr<fss::bytes::ByteSource>> MeteredBlobStore::OpenNativeRead(
    const domain::ObjectRef& ref) {
  //  ★ ADR-006：**只转发**能力，但把返回的来源包一层计量（见 MeteredNativeSource）：
  //    用户态拷贝的字节/操作在装饰器里记，`sendfile` 的字节/操作由数据面记。
  FSS_TRY(source, inner_.OpenNativeRead(ref));
  if (source == nullptr) {
    return Err(fss::ErrorKind::kInternal, "OpenNativeRead 返回了空来源");
  }
  std::shared_ptr<fss::bytes::ByteSource> wrapped(
      new MeteredNativeSource(std::move(source), registry_, driver_));
  return wrapped;
}

void MeteredBlobStore::RecordNativeRead(std::int64_t bytes, bool ok) const {
  //  ★ 与 `get()` **逐字相同**的标签值、op 名与阈值（`>0` 才记字节）——
  //    这是"sendfile 路径与 get() 路径的存储指标等价"的落点（用例 P5 双向断言）。
  //    ⚠️ 只允许在**真的用了 sendfile** 的那条路上调用；用户态拷贝由
  //    `MeteredNativeSource` 记，否则会重复计数。
  if (bytes > 0) {
    registry_.Increment("fss_storage_bytes_total", {{"direction", "out"}}, bytes);
  }
  Record("get", ok);
}

fss::Result<domain::ObjectStat> MeteredBlobStore::stat(const domain::ObjectRef& ref) {
  const auto result = inner_.stat(ref);
  Record("stat", result.ok());
  return result;
}

fss::Result<void> MeteredBlobStore::remove(const domain::ObjectRef& ref) {
  const auto result = inner_.remove(ref);
  Record("remove", result.ok());
  return result;
}

fss::Result<domain::ObjectStat> MeteredBlobStore::copy(const domain::ObjectRef& from,
                                                       const domain::ObjectRef& to) {
  const auto result = inner_.copy(from, to);
  Record("copy", result.ok());
  return result;
}

fss::Result<domain::ListPage> MeteredBlobStore::list(const std::string& container,
                                                     const std::string& prefix,
                                                     const std::string& continuation_token,
                                                     int limit) {
  const auto result = inner_.list(container, prefix, continuation_token, limit);
  Record("list", result.ok());
  return result;
}

fss::Result<domain::TempSweepResult> MeteredBlobStore::remove_temp_files(
    const std::string& container, std::int64_t older_than_epoch_seconds, bool dry_run) {
  const auto result = inner_.remove_temp_files(container, older_than_epoch_seconds, dry_run);
  Record("remove_temp_files", result.ok());
  return result;
}

}  // namespace fss::infra
