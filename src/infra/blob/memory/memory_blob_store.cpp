// InMemoryBlobStore 实现。语义登记表见头文件与 tests/framework/port_contract.h。
#include "infra/blob/memory/memory_blob_store.h"

#include "common/crypto/crypto.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace fss::infra {

namespace {

constexpr std::size_t OpIndex(InMemoryBlobStore::Op op) noexcept {
  return static_cast<std::size_t>(op);
}

std::string ToLower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

// 归一化算法名：大小写无关，忽略 '-' 与 '_'（"SHA-256"/"sha256"/"sha_256" 等价）
std::string NormalizeAlgorithm(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if (c == '-' || c == '_') continue;
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

}  // namespace

const char* InMemoryBlobStore::OpName(Op op) noexcept {
  switch (op) {
    case Op::kEnsureContainer: return "ensure_container";
    case Op::kPresignPut: return "presign_put";
    case Op::kPresignGet: return "presign_get";
    case Op::kPut: return "put";
    case Op::kGet: return "get";
    case Op::kStat: return "stat";
    case Op::kRemove: return "remove";
    case Op::kCopy: return "copy";
    case Op::kList: return "list";
    case Op::kCount: break;
  }
  return "unknown";
}

domain::BlobCapabilities InMemoryBlobStore::capabilities() const {
  domain::BlobCapabilities caps;
  // ★ 这份声明与契约测试的能力门控**互为约束**：声明为 true 的能力必须真的可用，
  //   声明为 false 的操作必须返回 kUnimplemented（而不是"碰巧不支持"）。
  caps.native_presign = false;   // 内存里没有"客户端直连"这回事
  caps.server_side_copy = true;
  caps.range_read = true;
  caps.streaming_put = true;
  caps.recommended_part_size = 0;  // 0 = 不支持分片上传
  caps.driver_name = "memory";
  return caps;
}

fss::Result<void> InMemoryBlobStore::BeginOp(Op op) {
  ++op_counts_[OpIndex(op)];
  if (fault_.op == op) {
    injected_latency_millis_ += fault_.latency_millis;
    if (fault_.fail_count > 0) {
      --fault_.fail_count;
      return Err(fault_.error, fault_.message);
    }
  }
  return Ok();
}

fss::Result<void> InMemoryBlobStore::ValidateRef(const domain::ObjectRef& ref) {
  if (ref.container.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "ObjectRef.container 不能为空");
  }
  if (ref.key.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "ObjectRef.key 不能为空");
  }
  return Ok();
}

fss::Result<void> InMemoryBlobStore::ensure_container(const std::string& container) {
  FSS_TRY(BeginOp(Op::kEnsureContainer));
  if (container.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "容器名不能为空");
  }
  containers_[container];  // 已存在时什么都不做 → 幂等
  return Ok();
}

fss::Result<domain::SignedLocation> InMemoryBlobStore::presign_put(
    const domain::ObjectRef& ref, const domain::PresignOptions& options) {
  (void)ref;
  (void)options;
  FSS_TRY(BeginOp(Op::kPresignPut));
  // capabilities().native_presign == false：按 ADR-003 §1，集中存储类后端在此返回 Unsupported，
  // 由 LocationIssuer 走自签 URL 兜底。契约测试会核对"声明 false ⇒ 必须报 kUnimplemented"。
  return Err(fss::ErrorKind::kUnimplemented, "内存存储不提供原生预签名 URL（native_presign=false）");
}

fss::Result<domain::SignedLocation> InMemoryBlobStore::presign_get(
    const domain::ObjectRef& ref, const domain::PresignOptions& options) {
  (void)ref;
  (void)options;
  FSS_TRY(BeginOp(Op::kPresignGet));
  return Err(fss::ErrorKind::kUnimplemented, "内存存储不提供原生预签名 URL（native_presign=false）");
}

fss::Result<void> InMemoryBlobStore::put(const domain::ObjectRef& ref,
                                         bytes::ByteSource& source,
                                         const domain::PutOptions& options) {
  FSS_TRY(BeginOp(Op::kPut));
  FSS_TRY(ValidateRef(ref));

  const auto container_it = containers_.find(ref.container);
  if (container_it == containers_.end()) {
    return Err(fss::ErrorKind::kNotFound,
               "容器不存在：" + ref.container + "（调用方必须先 ensure_container）");
  }

  // 注意 FSS_TRY 的两参数形态自带 `auto`，不要把类型写成 `auto data`（会展开成 `auto auto`）
  FSS_TRY(data, source.ReadAll(kMaxObjectBytes));

  if (options.expected_size >= 0 &&
      static_cast<std::int64_t>(data.size()) != options.expected_size) {
    // 契约 §1.7 / C1.2 H-2：承诺长度与实际不符必须**立刻**报错，不能静默接受短对象
    return Err(fss::ErrorKind::kInvalidArgument, "写入长度与 expected_size 不符")
        .With("expected", std::to_string(options.expected_size))
        .With("actual", std::to_string(data.size()));
  }

  if (!options.expected_checksum.empty()) {
    if (options.checksum_algorithm.empty()) {
      return Err(fss::ErrorKind::kInvalidArgument, "提供 expected_checksum 时必须同时给 checksum_algorithm");
    }
    if (NormalizeAlgorithm(options.checksum_algorithm) != "sha256") {
      return Err(fss::ErrorKind::kInvalidArgument,
                 "不支持的校验和算法：" + options.checksum_algorithm);
    }
    const std::string actual = crypto::Sha256Hex(data);
    if (ToLower(actual) != ToLower(options.expected_checksum)) {
      return Err(fss::ErrorKind::kChecksumMismatch, "校验和不符")
          .With("expected", options.expected_checksum)
          .With("actual", actual);
    }
  }

  // 故障注入：只存前 N 字节却返回成功（模拟"静默截断"，供 P6 做缺陷对照）
  if (fault_.op == Op::kPut && fault_.truncate_put_to_bytes >= 0 &&
      static_cast<std::size_t>(fault_.truncate_put_to_bytes) < data.size()) {
    data.resize(static_cast<std::size_t>(fault_.truncate_put_to_bytes));
  }

  Object obj;
  obj.data = std::move(data);
  obj.content_type = options.content_type;
  obj.checksum = crypto::Sha256Hex(obj.data);
  obj.checksum_algorithm = "SHA256";
  obj.last_modified_epoch_seconds = clock_.NowEpochSeconds();
  container_it->second[ref.key] = std::move(obj);
  return Ok();
}

fss::Result<void> InMemoryBlobStore::get(const domain::ObjectRef& ref, bytes::ByteSink& sink,
                                         const domain::ByteRange& range) {
  FSS_TRY(BeginOp(Op::kGet));
  FSS_TRY(ValidateRef(ref));

  const auto container_it = containers_.find(ref.container);
  if (container_it == containers_.end()) {
    return Err(fss::ErrorKind::kNotFound, "对象不存在：" + ref.ToString());
  }
  const auto object_it = container_it->second.find(ref.key);
  if (object_it == container_it->second.end()) {
    return Err(fss::ErrorKind::kNotFound, "对象不存在：" + ref.ToString());
  }

  const std::string& data = object_it->second.data;
  if (range.IsWholeObject()) {
    FSS_TRY(sink.Write(data));
    FSS_TRY(sink.Close());
    return Ok();
  }

  if (!capabilities().range_read) {
    return Err(fss::ErrorKind::kUnimplemented, "该存储不支持区间读（range_read=false）");
  }
  if (range.offset >= data.size()) {
    // offset == size 不是"空区间"而是不可满足（与 S3 Range 的 416 语义对齐）
    return Err(fss::ErrorKind::kInvalidArgument, "区间起点越界");
  }
  const std::size_t available = data.size() - static_cast<std::size_t>(range.offset);
  const std::size_t count = range.length == 0
                                ? available
                                : std::min<std::size_t>(static_cast<std::size_t>(range.length),
                                                        available);
  FSS_TRY(sink.Write(std::string_view(data).substr(static_cast<std::size_t>(range.offset), count)));
  FSS_TRY(sink.Close());
  return Ok();
}

fss::Result<domain::ObjectStat> InMemoryBlobStore::stat(const domain::ObjectRef& ref) {
  FSS_TRY(BeginOp(Op::kStat));
  FSS_TRY(ValidateRef(ref));

  domain::ObjectStat out;
  const auto container_it = containers_.find(ref.container);
  if (container_it == containers_.end()) {
    out.exists = false;  // 容器不存在 == 对象不存在（不是错误）
    return out;
  }
  const auto object_it = container_it->second.find(ref.key);
  if (object_it == container_it->second.end()) {
    out.exists = false;
    return out;
  }
  const Object& obj = object_it->second;
  out.exists = true;
  out.size = static_cast<std::int64_t>(obj.data.size());
  out.content_type = obj.content_type;
  out.checksum = (fault_.op == Op::kStat && !fault_.checksum_override.empty())
                     ? fault_.checksum_override
                     : obj.checksum;
  out.checksum_algorithm = obj.checksum_algorithm;
  out.last_modified_epoch_seconds = obj.last_modified_epoch_seconds;
  return out;
}

fss::Result<void> InMemoryBlobStore::remove(const domain::ObjectRef& ref) {
  FSS_TRY(BeginOp(Op::kRemove));
  FSS_TRY(ValidateRef(ref));
  const auto container_it = containers_.find(ref.container);
  if (container_it != containers_.end()) {
    container_it->second.erase(ref.key);  // 不存在也是 Ok → 幂等
  }
  return Ok();
}

fss::Result<domain::ObjectStat> InMemoryBlobStore::copy(const domain::ObjectRef& from,
                                                        const domain::ObjectRef& to) {
  FSS_TRY(BeginOp(Op::kCopy));
  FSS_TRY(ValidateRef(from));
  FSS_TRY(ValidateRef(to));
  if (!capabilities().server_side_copy) {
    return Err(fss::ErrorKind::kUnimplemented, "该存储不支持服务端复制（server_side_copy=false）");
  }

  const auto src_container = containers_.find(from.container);
  if (src_container == containers_.end()) {
    return Err(fss::ErrorKind::kNotFound, "复制源不存在：" + from.ToString());
  }
  const auto src_object = src_container->second.find(from.key);
  if (src_object == src_container->second.end()) {
    return Err(fss::ErrorKind::kNotFound, "复制源不存在：" + from.ToString());
  }
  auto dst_container = containers_.find(to.container);
  if (dst_container == containers_.end()) {
    return Err(fss::ErrorKind::kNotFound, "目标容器不存在：" + to.container);
  }

  Object copied = src_object->second;
  copied.last_modified_epoch_seconds = clock_.NowEpochSeconds();
  auto& slot = dst_container->second[to.key];
  slot = std::move(copied);  // 目标已存在时**覆盖**

  domain::ObjectStat out;
  out.exists = true;
  out.size = static_cast<std::int64_t>(slot.data.size());
  out.content_type = slot.content_type;
  out.checksum = slot.checksum;
  out.checksum_algorithm = slot.checksum_algorithm;
  out.last_modified_epoch_seconds = slot.last_modified_epoch_seconds;
  return out;
}

fss::Result<domain::ListPage> InMemoryBlobStore::list(const std::string& container,
                                                      const std::string& prefix,
                                                      const std::string& continuation_token,
                                                      int limit) {
  FSS_TRY(BeginOp(Op::kList));
  if (limit <= 0) {
    return Err(fss::ErrorKind::kInvalidArgument, "list 的 limit 必须 > 0");
  }
  const auto container_it = containers_.find(container);
  if (container_it == containers_.end()) {
    return Err(fss::ErrorKind::kNotFound, "容器不存在：" + container);
  }
  const Objects& objects = container_it->second;

  domain::ListPage page;
  auto it = continuation_token.empty() ? objects.lower_bound(prefix)
                                       : objects.upper_bound(continuation_token);
  int count = 0;
  while (it != objects.end()) {
    if (it->first.rfind(prefix, 0) != 0) break;  // 已按字典序，越出前缀即可停
    if (count == limit) {
      // 还有下一条匹配项 → 本页被截断，token = 本页最后一个 key
      page.truncated = true;
      page.continuation_token = page.entries.back().key;
      break;
    }
    domain::ListEntry entry;
    entry.key = it->first;
    entry.size = static_cast<std::int64_t>(it->second.data.size());
    entry.last_modified_epoch_seconds = it->second.last_modified_epoch_seconds;
    page.entries.push_back(std::move(entry));
    ++count;
    ++it;
  }
  return page;
}

std::int64_t InMemoryBlobStore::TakeInjectedLatencyMillis() {
  const std::int64_t value = injected_latency_millis_;
  injected_latency_millis_ = 0;
  return value;
}

std::size_t InMemoryBlobStore::object_count() const {
  std::size_t total = 0;
  for (const auto& [name, objects] : containers_) {
    (void)name;
    total += objects.size();
  }
  return total;
}

bool InMemoryBlobStore::HasContainer(const std::string& container) const {
  return containers_.find(container) != containers_.end();
}

}  // namespace fss::infra
