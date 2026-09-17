// PosixBlobStore 实现。设计要点（sidecar / 流式 / 路径安全）见头文件。
#include "infra/blob/posix/posix_blob_store.h"

#include "common/crypto/crypto.h"
#include "common/fs/fs.h"
#include "common/json/json.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fss::infra {

namespace {

constexpr std::size_t kIoBufferBytes = 64 * 1024;

fss::Error ErrFromErrno(const std::string& what) {
  const int e = errno;
  fss::ErrorKind kind = fss::ErrorKind::kInternal;
  if (e == ENOENT) {
    kind = fss::ErrorKind::kNotFound;
  } else if (e == EACCES || e == EPERM) {
    kind = fss::ErrorKind::kPermissionDenied;
  } else if (e == ENOSPC || e == EDQUOT) {
    kind = fss::ErrorKind::kUnavailable;
  }
  return fss::Err(kind, what + "：" + std::strerror(e));
}

std::string ToLower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string NormalizeAlgorithm(std::string_view text) {
  std::string out;
  for (char c : text) {
    if (c == '-' || c == '_') continue;
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

bool WriteAll(int fd, const char* data, std::size_t length) {
  std::size_t written = 0;
  while (written < length) {
    const ssize_t n = ::write(fd, data + written, length - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    written += static_cast<std::size_t>(n);
  }
  return true;
}

//  优先 `copy_file_range`（同文件系统零拷贝）；不支持时回退到流式 read/write。
bool CopyFd(int in, int out) {
  off_t in_off = 0;
  off_t out_off = 0;
  bool used_copy_range = true;
  while (true) {
    const ssize_t n = ::copy_file_range(in, &in_off, out, &out_off, kIoBufferBytes, 0);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      used_copy_range = false;  // EXDEV / EINVAL / ENOSYS …
      break;
    }
  }
  if (used_copy_range) return true;

  //  回退：清空目标，从头流式复制（跨文件系统必须可用 —— 计划任务 3）
  if (::lseek(in, 0, SEEK_SET) != 0 || ::ftruncate(out, 0) != 0) return false;
  std::array<char, kIoBufferBytes> buffer{};
  while (true) {
    const ssize_t n = ::read(in, buffer.data(), buffer.size());
    if (n == 0) return true;
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (!WriteAll(out, buffer.data(), static_cast<std::size_t>(n))) return false;
  }
}

}  // namespace

PosixBlobStore::PosixBlobStore(std::string root, const fss::IClock& clock,
                               PosixBlobStoreOptions options)
    : root_(std::move(root)), clock_(clock), options_(std::move(options)) {}

domain::BlobCapabilities PosixBlobStore::capabilities() const {
  domain::BlobCapabilities caps;
  caps.native_presign = false;  // 文件系统没有"客户端直连签名 URL"（ADR-003 §1）
  caps.server_side_copy = true;
  caps.range_read = true;
  caps.streaming_put = true;
  caps.recommended_part_size = 0;
  caps.driver_name = "posix";
  return caps;
}

bool PosixBlobStore::IsInternalKey(std::string_view relative_key) {
  if (relative_key.size() >= kSidecarSuffix.size() &&
      relative_key.compare(relative_key.size() - kSidecarSuffix.size(), kSidecarSuffix.size(),
                           kSidecarSuffix) == 0) {
    return true;
  }
  return relative_key.find(kTempMarker) != std::string_view::npos;
}

fss::Result<std::string> PosixBlobStore::ContainerDir(std::string_view container) const {
  return fs::LexicalJoin(root_, container);
}

fss::Result<std::string> PosixBlobStore::ObjectPath(std::string_view container,
                                                    std::string_view key) const {
  FSS_TRY(dir, ContainerDir(container));
  if (!fs::Exists(dir)) {
    return Err(fss::ErrorKind::kNotFound, "容器不存在：" + std::string(container));
  }
  return fs::SafeJoin(dir, key);
}

fss::Result<std::string> PosixBlobStore::WritableObjectPath(std::string_view container,
                                                            std::string_view key) const {
  FSS_TRY(path, ObjectPath(container, key));
  const auto slash = path.rfind('/');
  if (slash != std::string::npos) {
    FSS_TRY(fs::EnsureDir(path.substr(0, slash)));
  }
  //  建目录后**重新**做真实路径校验（见头文件说明）
  return ObjectPath(container, key);
}

//  进程级序号（定义见头文件；这里只做一次定义）
std::atomic<std::uint64_t> PosixBlobStore::tmp_counter_{0};

std::string PosixBlobStore::TempPathFor(const std::string& target) const {
  const auto counter = tmp_counter_.fetch_add(1);
  return target + std::string(kTempMarker) + options_.instance_id + "." +
         std::to_string(static_cast<long>(::getpid())) + "." + std::to_string(counter);
}

fss::Result<void> PosixBlobStore::WriteSidecar(const std::string& object_path,
                                               const domain::ObjectStat& st, bool fsync) const {
  json::Value meta = json::Value::object();
  meta["content_type"] = st.content_type;
  meta["checksum"] = st.checksum;
  meta["checksum_algorithm"] = st.checksum_algorithm;
  meta["size"] = st.size;
  meta["last_modified_epoch_seconds"] = st.last_modified_epoch_seconds;

  fs::WriteOptions write_options;
  //  sidecar 的耐久性与对象本体**同一档**（否则会出现"对象已落盘、元数据没落盘"的不一致）
  write_options.fsync_data = fsync;
  write_options.fsync_dir = fsync;
  write_options.tmp_suffix = options_.instance_id;
  return fs::AtomicWriteFile(object_path + std::string(kSidecarSuffix), json::Dump(meta),
                             write_options);
}

IFileSync& PosixBlobStore::FileSync() const {
  static RealFileSync real;
  return options_.file_sync != nullptr ? *options_.file_sync : real;
}

fss::Result<domain::ObjectStat> PosixBlobStore::ReadSidecar(const std::string& object_path) const {
  FSS_TRY(text, fs::ReadFile(object_path + std::string(kSidecarSuffix)));
  FSS_TRY(parsed, json::ParseObject(text));
  const json::Value& value = parsed;

  domain::ObjectStat out;
  out.exists = true;
  if (auto it = value.find("content_type"); it != value.end() && it->is_string()) {
    out.content_type = it->get<std::string>();
  }
  if (auto it = value.find("checksum"); it != value.end() && it->is_string()) {
    out.checksum = it->get<std::string>();
  }
  if (auto it = value.find("checksum_algorithm"); it != value.end() && it->is_string()) {
    out.checksum_algorithm = it->get<std::string>();
  }
  return out;
}

fss::Result<void> PosixBlobStore::ensure_container(const std::string& container) {
  FSS_TRY(dir, ContainerDir(container));
  return fs::EnsureDir(dir);
}

fss::Result<domain::SignedLocation> PosixBlobStore::presign_put(
    const domain::ObjectRef& ref, const domain::PresignOptions& options) {
  (void)ref;
  (void)options;
  // capabilities().native_presign == false → 由 LocationIssuer 走自签 URL（ADR-003 §1）
  return Err(fss::ErrorKind::kUnimplemented, "POSIX 存储不提供原生预签名 URL");
}

fss::Result<domain::SignedLocation> PosixBlobStore::presign_get(
    const domain::ObjectRef& ref, const domain::PresignOptions& options) {
  (void)ref;
  (void)options;
  return Err(fss::ErrorKind::kUnimplemented, "POSIX 存储不提供原生预签名 URL");
}

fss::Result<void> PosixBlobStore::put(const domain::ObjectRef& ref, bytes::ByteSource& source,
                                      const domain::PutOptions& options) {
  FSS_TRY(path, WritableObjectPath(ref.container, ref.key));
  const std::string tmp = TempPathFor(path);

  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) return ErrFromErrno("创建临时文件失败");
  const auto cleanup = [&]() {
    ::close(fd);
    (void)::unlink(tmp.c_str());
  };

  //  边读流、边写盘、边增量算 SHA-256 —— 内存与对象大小无关（C3.4）
  crypto::Sha256Hasher hasher;
  std::array<char, kIoBufferBytes> buffer{};
  std::uint64_t total = 0;
  while (true) {
    const auto read = source.Read(buffer.data(), buffer.size());
    if (!read.ok()) {
      cleanup();
      return read.error();
    }
    if (read.value() == 0) break;
    if (!WriteAll(fd, buffer.data(), read.value())) {
      const auto error = ErrFromErrno("写入失败");
      cleanup();
      return error;
    }
    hasher.Update(std::string_view(buffer.data(), read.value()));
    total += read.value();
  }

  if (options.expected_size >= 0 &&
      static_cast<std::int64_t>(total) != options.expected_size) {
    cleanup();
    return Err(fss::ErrorKind::kInvalidArgument, "写入长度与 expected_size 不符")
        .With("expected", std::to_string(options.expected_size))
        .With("actual", std::to_string(total));
  }

  const std::string checksum = hasher.HexDigest();
  if (!options.expected_checksum.empty()) {
    if (options.checksum_algorithm.empty()) {
      cleanup();
      return Err(fss::ErrorKind::kInvalidArgument,
                 "提供 expected_checksum 时必须同时给 checksum_algorithm");
    }
    if (NormalizeAlgorithm(options.checksum_algorithm) != "sha256") {
      cleanup();
      return Err(fss::ErrorKind::kInvalidArgument,
                 "不支持的校验和算法：" + options.checksum_algorithm);
    }
    if (ToLower(checksum) != ToLower(options.expected_checksum)) {
      cleanup();
      return Err(fss::ErrorKind::kChecksumMismatch, "校验和不符")
          .With("expected", options.expected_checksum)
          .With("actual", checksum);
    }
  }

  //  ADR-008：数据先落盘，再改名；改名后 fsync 目录。
  //  是否落盘由 `fsync_policy` 决定（C3.11）：`by_size` 下小文件靠批提交摊销。
  const bool need_sync =
      ShouldFsync(options_.fsync_policy, static_cast<std::int64_t>(total),
                  options_.fsync_threshold_bytes);
  if (need_sync) {
    const auto sync = FileSync().DataSync(fd);
    if (!sync.ok()) {
      cleanup();
      return sync.error();
    }
  }
  if (::close(fd) != 0) {
    const auto error = ErrFromErrno("close 失败");
    (void)::unlink(tmp.c_str());
    return error;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    const auto error = ErrFromErrno("rename 失败");
    (void)::unlink(tmp.c_str());
    return error;
  }
  if (need_sync) {
    //  目录 fsync 失败不致命（文件已可见）；但如实记录到 stderr 之外由调用方决定是否重试。
    (void)FileSync().SyncDirectory(path.substr(0, path.rfind('/')));
  }

  domain::ObjectStat st;
  st.exists = true;
  st.size = static_cast<std::int64_t>(total);
  st.content_type = options.content_type;
  st.checksum = checksum;
  st.checksum_algorithm = "SHA256";
  st.last_modified_epoch_seconds = clock_.NowEpochSeconds();
  FSS_TRY(WriteSidecar(path, st, need_sync));
  return Ok();
}

fss::Result<void> PosixBlobStore::get(const domain::ObjectRef& ref, bytes::ByteSink& sink,
                                      const domain::ByteRange& range) {
  FSS_TRY(path, ObjectPath(ref.container, ref.key));

  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) return Err(fss::ErrorKind::kNotFound, "对象不存在：" + ref.ToString());
    return ErrFromErrno("打开对象失败");
  }

  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    const auto error = ErrFromErrno("fstat 失败");
    ::close(fd);
    return error;
  }
  const std::uint64_t size = static_cast<std::uint64_t>(info.st_size);

  std::uint64_t offset = 0;
  std::uint64_t count = size;
  if (!range.IsWholeObject()) {
    if (!capabilities().range_read) {
      ::close(fd);
      return Err(fss::ErrorKind::kUnimplemented, "该存储不支持区间读");
    }
    if (range.offset >= size) {
      ::close(fd);
      //  offset == size 视为不可满足（与 S3 416 对齐）
      return Err(fss::ErrorKind::kInvalidArgument, "区间起点越界");
    }
    offset = range.offset;
    const std::uint64_t available = size - offset;
    count = range.length == 0 ? available : std::min<std::uint64_t>(range.length, available);
  }

  //  ★ 用 pread（不共享文件偏移）→ 多线程并发读同一 fd/文件互不干扰（C3.10 的基础）
  std::array<char, kIoBufferBytes> buffer{};
  std::uint64_t done = 0;
  while (done < count) {
    const std::size_t want =
        static_cast<std::size_t>(std::min<std::uint64_t>(kIoBufferBytes, count - done));
    const ssize_t n = ::pread(fd, buffer.data(), want, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      const auto error = ErrFromErrno("pread 失败");
      ::close(fd);
      return error;
    }
    if (n == 0) break;  // 对象在读取过程中被截断
    FSS_TRY(sink.Write(std::string_view(buffer.data(), static_cast<std::size_t>(n))));
    done += static_cast<std::uint64_t>(n);
  }
  ::close(fd);
  FSS_TRY(sink.Close());
  return Ok();
}

fss::Result<domain::ObjectStat> PosixBlobStore::stat(const domain::ObjectRef& ref) {
  domain::ObjectStat out;
  const auto dir = ContainerDir(ref.container);
  if (!dir.ok() || !fs::Exists(dir.value())) {
    out.exists = false;  // 容器不存在 == 对象不存在（不是错误）
    return out;
  }
  FSS_TRY(path, fs::SafeJoin(dir.value(), ref.key));

  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    out.exists = false;
    return out;
  }
  out.exists = true;
  out.size = static_cast<std::int64_t>(info.st_size);
  out.last_modified_epoch_seconds = static_cast<std::int64_t>(info.st_mtime);
  const auto meta = ReadSidecar(path);
  if (meta.ok()) {
    out.content_type = meta.value().content_type;
    out.checksum = meta.value().checksum;
    out.checksum_algorithm = meta.value().checksum_algorithm;
  }
  return out;
}

fss::Result<void> PosixBlobStore::remove(const domain::ObjectRef& ref) {
  FSS_TRY(dir, ContainerDir(ref.container));
  if (!fs::Exists(dir)) return Ok();  // 幂等
  FSS_TRY(path, fs::SafeJoin(dir, ref.key));

  //  ENOENT 视为成功（对象存储的删除天然幂等）
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    return ErrFromErrno("删除对象失败");
  }
  (void)::unlink((path + std::string(kSidecarSuffix)).c_str());
  return Ok();
}

fss::Result<domain::ObjectStat> PosixBlobStore::copy(const domain::ObjectRef& from,
                                                     const domain::ObjectRef& to) {
  if (!capabilities().server_side_copy) {
    return Err(fss::ErrorKind::kUnimplemented, "该存储不支持服务端复制");
  }
  FSS_TRY(from_path, ObjectPath(from.container, from.key));
  FSS_TRY(to_dir, ContainerDir(to.container));
  if (!fs::Exists(to_dir)) {
    return Err(fss::ErrorKind::kNotFound, "目标容器不存在：" + to.container);
  }
  FSS_TRY(to_path, WritableObjectPath(to.container, to.key));

  const int in = ::open(from_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) {
    if (errno == ENOENT) return Err(fss::ErrorKind::kNotFound, "复制源不存在：" + from.ToString());
    return ErrFromErrno("打开复制源失败");
  }
  const std::string tmp = TempPathFor(to_path);
  const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (out < 0) {
    const auto error = ErrFromErrno("创建复制目标失败");
    ::close(in);
    return error;
  }

  struct stat source_info {};
  const bool have_size = ::fstat(in, &source_info) == 0;
  const std::int64_t copied_bytes =
      have_size ? static_cast<std::int64_t>(source_info.st_size) : 0;
  const bool need_sync =
      ShouldFsync(options_.fsync_policy, copied_bytes, options_.fsync_threshold_bytes);

  bool ok = CopyFd(in, out);
  if (ok && need_sync) {
    ok = FileSync().DataSync(out).ok();
  }
  if (::close(in) != 0) ok = false;
  if (::close(out) != 0) ok = false;
  if (!ok || ::rename(tmp.c_str(), to_path.c_str()) != 0) {
    const auto error = ok ? ErrFromErrno("rename 失败")
                          : Err(fss::ErrorKind::kInternal, "复制失败");
    (void)::unlink(tmp.c_str());
    return error;
  }
  if (need_sync) {
    (void)FileSync().SyncDirectory(to_path.substr(0, to_path.rfind('/')));
  }

  //  sidecar 一并复制（content_type / checksum）
  const auto from_meta = ReadSidecar(from_path);
  if (from_meta.ok()) {
    domain::ObjectStat meta = from_meta.value();
    meta.last_modified_epoch_seconds = clock_.NowEpochSeconds();
    FSS_TRY(WriteSidecar(to_path, meta, need_sync));
  }
  return stat(to);
}

fss::Result<domain::ListPage> PosixBlobStore::list(const std::string& container,
                                                   const std::string& prefix,
                                                   const std::string& continuation_token,
                                                   int limit) {
  if (limit <= 0) return Err(fss::ErrorKind::kInvalidArgument, "list 的 limit 必须 > 0");
  FSS_TRY(dir, ContainerDir(container));
  if (!fs::Exists(dir)) return Err(fss::ErrorKind::kNotFound, "容器不存在：" + container);

  namespace stdfs = std::filesystem;
  std::vector<std::pair<std::string, std::int64_t>> found;  // key → last_modified
  std::error_code ec;
  for (stdfs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    const std::string relative = stdfs::relative(it->path(), dir, ec).generic_string();
    if (ec) break;
    if (IsInternalKey(relative)) continue;
    found.emplace_back(relative, static_cast<std::int64_t>(it->last_write_time(ec).time_since_epoch().count() / 1000000000LL));
  }
  std::sort(found.begin(), found.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  domain::ListPage page;
  auto it = continuation_token.empty()
                ? std::lower_bound(found.begin(), found.end(), prefix,
                                   [](const auto& entry, const std::string& value) {
                                     return entry.first < value;
                                   })
                : std::upper_bound(found.begin(), found.end(), continuation_token,
                                   [](const std::string& value, const auto& entry) {
                                     return value < entry.first;
                                   });
  int count = 0;
  for (; it != found.end(); ++it) {
    if (it->first.rfind(prefix, 0) != 0) break;
    if (count == limit) {
      page.truncated = true;
      page.continuation_token = page.entries.back().key;
      break;
    }
    domain::ListEntry entry;
    entry.key = it->first;
    entry.size = static_cast<std::int64_t>(fs::FileSize(dir + "/" + it->first).value_or(0));
    entry.last_modified_epoch_seconds = it->second;
    page.entries.push_back(std::move(entry));
    ++count;
  }
  return page;
}

}  // namespace fss::infra
