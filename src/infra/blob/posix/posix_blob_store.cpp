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
#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
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

//  ★ P9-D04：`std::filesystem::last_write_time()` 的时基是 **file_clock**，不是 Unix 纪元。
//  在 g++ 11 / libstdc++ 上 `time_since_epoch().count() / 1e9` 得到的是 **负数**
//  （实测某文件 `st_mtime = 1699999995` 时得到 `-4737664005`），
//  与 `domain::ListEntry::last_modified_epoch_seconds` 声明的"Unix 纪元秒"相差半个世纪。
//  危害不是"显示不对"而是**判定反了**：`GcTask` 用 `entry.last_modified_epoch_seconds > cutoff`
//  判断"太新 → 保护"，负值让它**恒假** ⇒ 刚落地的对象被当成过期回收；
//  `remove_temp_files` 同理会把**在途**上传的临时文件删掉。
//  正解：与 `stat()` 端口操作同源，用 `::stat().st_mtime`。
//  失败时返回 `nullopt`（stat 失败意味着这个文件此刻无法访问：竞态删除、EMFILE、EIO…）：
//  调用方必须按"未知"处理 —— 删除路径一律**不动**，列表路径按"最新"上报（对 GC 而言是保守方向）。
std::optional<std::int64_t> MtimeEpochSeconds(const std::filesystem::path& path) {
  struct ::stat info {};
  if (::stat(path.c_str(), &info) != 0) return std::nullopt;
  return static_cast<std::int64_t>(info.st_mtime);
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

//  真实的 fadvise 实现：`posix_fadvise` 只会在参数非法时硬失败，提示本身不保证任何
//  行为，因此与 Linux 习惯一致地忽略返回值（与 ADR-008 的"尽力而为"语义一致）。
class RealFadviseSink final : public IFadviseSink {
 public:
  void Random(int fd) override { (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM); }
  void DontNeed(int fd) override { (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED); }
};

//  `storage.posix.fadvise_dontneed_after_large_read` 的阈值：**大于** 1 MiB 的读
//  才丢弃页缓存（小文件重读命中缓存的收益更高）。
constexpr std::uint64_t kDontNeedThresholdBytes = 1024 * 1024;

//  ★ ADR-008 的 P4：组提交的**有界等待窗口**。
//  为什么是这个值（且为什么是常量而不是配置键）：
//    · `storage.posix` 没有 wait 键，也**不新增**配置键（ADR-008 只定义了
//      `durability` / `group_commit_max_batch` / `fsync_threshold_bytes`）；
//    · 太小（例如 50 µs）在 WSL2 / 高负载下批常常凑不满 → 摊销退化成"每文件一次
//      syncfs"，等于没实现 P4；
//    · 太大（例如 10 ms）给每个"孤立写入"加上固定延迟；
//    · 2 ms 足以让并发写入者（各自写 tmp 只需几十 µs）汇入同一批，同时把单文件
//      延迟上界钉在 2 ms。**顺序单文件写不受影响**：领队发现没有别的写入者时立即提交。
//  ⚠️ 本窗口只影响"批能凑多大"；它**不是**耐久性参数 —— 耐久性粒度始终是"批"。
constexpr auto kBatchWaitWindow = std::chrono::microseconds(2000);

}  // namespace

PosixBlobStore::PosixBlobStore(std::string root, const fss::IClock& clock,
                               PosixBlobStoreOptions options)
    : root_(std::move(root)),
      clock_(clock),
      options_(std::move(options)),
      //  ★ ADR-009 §4.5：每 store 一次的随机后缀（M1 依据见头文件）。
      //    构造时生成一次即可：同一 store 内还有进程级递增计数保证不重复；
      //    随机 token 解决的是"跨主机 instance_id / pid / 计数全部相同"的确定性撞车。
      tmp_token_(crypto::RandomHex(8)) {}

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
    FSS_TRY(fs::EnsureDir(path.substr(0, slash), options_.dir_mode));
  }
  //  建目录后**重新**做真实路径校验（见头文件说明）
  return ObjectPath(container, key);
}

//  进程级序号（定义见头文件；这里只做一次定义）
std::atomic<std::uint64_t> PosixBlobStore::tmp_counter_{0};

std::string PosixBlobStore::TempPathFor(const std::string& target) const {
  const auto counter = tmp_counter_.fetch_add(1);
  //  名字形态：`<target>.tmp.<instance_id>.<pid>.<counter>.<random>`
  //  · 前四段是接线前的既有形态（`.tmp.` 前缀与 `IsInternalKey`/C9.25 的清理判据依赖它）；
  //  · 第五段是 ADR-009 §4.5 要求的**随机后缀**（见头文件：默认 `instance_id=local`
  //    时前四段在多实例共享挂载上会确定性撞名 → M1 静默串数据）。
  return target + std::string(kTempMarker) + options_.instance_id + "." +
         std::to_string(static_cast<long>(::getpid())) + "." + std::to_string(counter) + "." +
         tmp_token_;
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

IFadviseSink& PosixBlobStore::Fadvise() const {
  static RealFadviseSink real;
  return options_.fadvise_sink != nullptr ? *options_.fadvise_sink : real;
}

void PosixBlobStore::AdviseRandom(int fd) const {
  if (!options_.fadvise_random) return;
  Fadvise().Random(fd);
}

void PosixBlobStore::AdviseDontNeedAfterRead(int fd, std::uint64_t bytes_read) const {
  if (!options_.fadvise_dontneed_after_large_read) return;
  if (bytes_read <= kDontNeedThresholdBytes) return;
  Fadvise().DontNeed(fd);
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
  //  ★ `storage.posix.dir_mode`：显式权限位（接线前是 `fs::EnsureDir` 的默认 0750）。
  return fs::EnsureDir(dir, options_.dir_mode);
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
  //  ★ `storage.posix.atomic_write=false` → **直接写目标文件**（不做 tmp + rename）。
  //    代价是"打开即截断"，因此失败路径必须删除目标：宁可"没有文件"，也绝不留下半成品
  //    （ADR-008 的 R3 不变量在两种模式下都必须成立）。
  const bool atomic = options_.atomic_write;
  //  ★ ADR-008 的 P4 只在"原子写 + durability=batch"时成立：直写目标没有 rename 阶段，
  //    因此**不参与批**（逐字保持接线前的直写语义）。
  const bool batch_phase = atomic && options_.batch_commit;
  //  入批计数（仅 batch 档）：领队用它判断"还有没有别的写入者值得等"。任何早退路径
  //  （源端读失败 / 校验不符 / open 失败）都必须递减，否则领队会空等窗口。
  struct WritersGuard {
    std::atomic<std::size_t>* counter;
    bool active;
    ~WritersGuard() {
      if (active) counter->fetch_sub(1);
    }
  } writers_guard{&batch_writers_, batch_phase};
  if (batch_phase) batch_writers_.fetch_add(1);

  const std::string tmp = atomic ? TempPathFor(path) : path;
  const int open_flags = atomic
                             ? (O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC)
                             : (O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC);
  //  ★ `storage.posix.file_mode`：显式权限位（接线前的默认值是 0644）。
  const int fd = ::open(tmp.c_str(), open_flags, static_cast<mode_t>(options_.file_mode));
  if (fd < 0) return ErrFromErrno(atomic ? "创建临时文件失败" : "创建目标文件失败");
  const auto cleanup = [&]() {
    ::close(fd);
    (void)::unlink(tmp.c_str());  // 非原子模式下 tmp == path：失败不留半成品
  };
  //  ★ `storage.posix.fadvise_random`：写入路径的随机访问提示（默认关闭 → no-op）
  AdviseRandom(fd);

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

  //  事件：数据已进入 `.tmp_*`（阶段 A）。顺序判据要求它早于本批的 `syncfs`。
  if (Observer() != nullptr) Observer()->OnWriteTmp(tmp);

  //  ADR-008：数据先落盘，再改名；改名后 fsync 目录。
  //  · `batch` 档的小对象（< 阈值）**不**在这里单文件落盘 —— 靠批次末的 `syncfs` 摊销；
  //  · `>= 阈值` 的对象**强制单独 fdatasync**（不靠批摊销，与 schema 描述一致）；
  //  · `per_file` 档（kAlways）每个对象都单独落盘；`never`（旧别名，kNever）都不落盘。
  const bool need_sync =
      ShouldFsync(options_.fsync_policy, static_cast<std::int64_t>(total),
                  options_.fsync_threshold_bytes);
  if (need_sync) {
    if (Observer() != nullptr) Observer()->OnDataSync(tmp);
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

  domain::ObjectStat st;
  st.exists = true;
  st.size = static_cast<std::int64_t>(total);
  st.content_type = options.content_type;
  st.checksum = checksum;
  st.checksum_algorithm = "SHA256";
  st.last_modified_epoch_seconds = clock_.NowEpochSeconds();

  if (atomic && batch_phase && !need_sync) {
    //  ★ ADR-008 的 P4：把「对象本体 + sidecar」一起入批。
    //    为什么 sidecar 也进批：否则会出现"对象已 durable、元数据没 durable"的窗口；
    //    而 `syncfs` 是文件系统级操作，把 sidecar 的 tmp 也写出来一起同步**代价为零**。
    json::Value meta = json::Value::object();
    meta["content_type"] = st.content_type;
    meta["checksum"] = st.checksum;
    meta["checksum_algorithm"] = st.checksum_algorithm;
    meta["size"] = st.size;
    meta["last_modified_epoch_seconds"] = st.last_modified_epoch_seconds;
    const std::string sidecar_final = path + std::string(kSidecarSuffix);
    const std::string sidecar_tmp = TempPathFor(sidecar_final);
    const auto wrote_sidecar = WriteTmpFile(sidecar_tmp, json::Dump(meta));
    if (!wrote_sidecar.ok()) {
      (void)::unlink(tmp.c_str());  // 对象 tmp 从未入批 → 这里必须清掉
      return wrote_sidecar.error();
    }
    if (Observer() != nullptr) Observer()->OnWriteTmp(sidecar_tmp);
    std::vector<PendingRename> renames;
    renames.push_back(PendingRename{tmp, path});
    renames.push_back(PendingRename{sidecar_tmp, sidecar_final});
    return JoinBatch(std::move(renames));
  }

  //  单文件提交（直写 / per_file / never / batch 档的大对象例外）：
  //  `rename` →（可选）`fsync(目录)`。逐字保持接线前的顺序与语义。
  if (atomic && ::rename(tmp.c_str(), path.c_str()) != 0) {
    const auto error = ErrFromErrno("rename 失败");
    (void)::unlink(tmp.c_str());
    return error;
  }
  if (atomic && Observer() != nullptr) Observer()->OnRename(tmp, path);
  if (need_sync && options_.sync_dir_after_batch) {
    //  目录 fsync 失败不致命（文件已可见）；但如实记录到 stderr 之外由调用方决定是否重试。
    const std::string dir = path.substr(0, path.rfind('/'));
    if (Observer() != nullptr) Observer()->OnDirectorySync(dir);
    (void)FileSync().SyncDirectory(dir);
  }

  FSS_TRY(WriteSidecar(path, st, need_sync));
  return Ok();
}

// =============================================================================
//  ADR-008 的 P4：两阶段批提交（`write all tmp → syncfs → rename all → fsync(dir)`）
// =============================================================================
//  批如何形成（**并发驱动的组提交**，没有独立的批量写 API）：
//    每个 put 先写自己的 `.tmp_*`（+ 大对象单独 fdatasync），然后在 store 内入批。
//    本批第 1 个入批者成为领队，等"批满 `group_commit_max_batch`"或"有界窗口到期"
//    （`kBatchWaitWindow`）；任何人把批填满时由**他**成为提交者（这样批不会超过上限）。
//    提交者执行 ② `syncfs` → ③ 统一 `rename` → ④ `fsync(目录)`，然后唤醒全批返回。
//  顺序不变量（R1/R2，机器可检查）：
//    · 本批**所有** rename 都在 `syncfs` **之后**（R1：可见即完整）；
//    · `fsync(目录)` 在**所有** rename 之后（R2：承诺即可靠）。
//  失败规则（先定义再断言）：
//    · `syncfs` 失败 → **整批**都失败，一个 rename 都不做（宁可没有对象，也不留残缺）；
//    · 某个对象的 rename 失败 → **只有那个对象**失败（返回错误给它的调用者），
//      同批其它对象照常提交；失败对象的所有 tmp 被清理（绝不留半个对象）。
//  ⚠️ 诚实标注的偏差：ADR-008 的 31,478 文件/秒 / 82.3x 是在**显式批量写 5000 文件 /
//     批 500** 的场景测的。本实现是**并发驱动**的组提交，因此**顺序单文件写**仍然是
//     "一文件一次提交"（领队发现没有别的写入者时立即提交，拿不到那个摊销），
//     只有**并发**写才有摊销。不要把 ADR 的吞吐数字当成本实现的成绩。
// =============================================================================
fss::Result<void> PosixBlobStore::JoinBatch(std::vector<PendingRename> renames) {
  //  门控（测试）：在"tmp 已写好、尚未入批"处对齐并发写入者，让批大小成为确定性事实。
  if (options_.batch_gate != nullptr) options_.batch_gate->ArriveAndWait();

  std::unique_lock<std::mutex> lock(batch_mutex_);
  if (current_batch_ == nullptr || current_batch_->sealed) {
    current_batch_ = std::make_shared<Batch>();
  }
  std::shared_ptr<Batch> batch = current_batch_;
  const std::size_t my_index = batch->entries.size();
  batch->entries.push_back(BatchEntry{std::move(renames)});

  const std::size_t max_batch =
      options_.group_commit_max_batch == 0 ? 1 : options_.group_commit_max_batch;
  bool committer = false;
  if (batch->entries.size() >= max_batch) {
    //  批满：**当前入批者**成为提交者（不能只让领队提交 —— 否则领队被调度延迟时
    //  批会继续变大，超过 `group_commit_max_batch`）。
    batch->sealed = true;
    committer = true;
    batch_cv_.notify_all();
  } else if (my_index == 0) {
    //  领队：等"批满"或"有界窗口到期"或"没有别的写入者还在路上"。
    const auto deadline = std::chrono::steady_clock::now() + kBatchWaitWindow;
    batch_cv_.wait_until(lock, deadline, [&] {
      return batch->sealed || batch_writers_.load() <= batch->entries.size();
    });
    if (!batch->sealed) {
      batch->sealed = true;
      committer = true;
    }
  }

  if (committer) {
    if (current_batch_ == batch) current_batch_.reset();
    std::vector<BatchEntry> committed = std::move(batch->entries);
    batch->entries.clear();
    lock.unlock();  //  提交期间不持 `batch_mutex_`：新写入者可以开始凑下一批
    {
      //  串行化提交（理由见头文件 `batch_commit_mutex_`）：保证"逐批事件序列"可切分。
      std::lock_guard<std::mutex> commit_lock(batch_commit_mutex_);
      CommitBatch(committed, &batch->results);
    }
    lock.lock();
    batch->done = true;
    batch_cv_.notify_all();
  } else {
    batch_cv_.wait(lock, [&] { return batch->done; });
  }

  if (my_index < batch->results.size()) return batch->results[my_index];
  return Ok();
}

void PosixBlobStore::CommitBatch(const std::vector<BatchEntry>& entries,
                                 std::vector<fss::Result<void>>* results) {
  results->assign(entries.size(), Ok());
  //  同一批的 rename 目标通常在同一容器目录；`syncfs` 是文件系统级操作，取一个目录
  //  作为"该文件系统上的 fd"即可覆盖整批（即使跨目录也一样）。
  std::string directory;
  if (!entries.empty() && !entries.front().renames.empty()) {
    const std::string& final_path = entries.front().renames.front().final_path;
    const auto slash = final_path.rfind('/');
    if (slash != std::string::npos) directory = final_path.substr(0, slash);
  }

  //  阶段 ②：一次 `syncfs` 让**全批数据** durable（R1 的前提）。
  if (Observer() != nullptr) Observer()->OnSyncFilesystem(directory, entries.size());
  const auto synced = FileSync().SyncFilesystem(directory);
  if (Metrics() != nullptr) Metrics()->Increment("fss_posix_syncfs_total");
  if (!synced.ok()) {
    //  ★ R1：数据未 durable → **一个 rename 都不做**。整批失败 + 清理所有 tmp。
    for (auto& result : *results) result = synced.error();
    for (const auto& entry : entries) {
      for (const auto& rename : entry.renames) (void)::unlink(rename.tmp.c_str());
    }
    if (Metrics() != nullptr) Metrics()->Increment("fss_posix_group_commits_total");
    return;
  }

  //  阶段 ③：统一 rename（**全批都在 syncfs 之后** —— 顺序不变量 R1）。
  for (std::size_t i = 0; i < entries.size(); ++i) {
    for (const auto& rename : entries[i].renames) {
      if (::rename(rename.tmp.c_str(), rename.final_path.c_str()) != 0) {
        (*results)[i] = ErrFromErrno("rename 失败");
        //  该对象失败：清掉它**所有** tmp（含刚失败的那条）—— 绝不留下半个对象。
        //  同批其它对象不受影响，继续按协议提交。
        for (const auto& other : entries[i].renames) (void)::unlink(other.tmp.c_str());
        break;
      }
      if (Observer() != nullptr) Observer()->OnRename(rename.tmp, rename.final_path);
    }
  }

  //  阶段 ④：`fsync(目录)` 让**全批改名** durable（R2）。`sync_dir_after_batch=false`
  //  会破坏 R2，因此它是**不变量**：组合根对 false 拒绝启动（见 server_main.cpp）。
  if (options_.sync_dir_after_batch && !directory.empty()) {
    if (Observer() != nullptr) Observer()->OnDirectorySync(directory);
    (void)FileSync().SyncDirectory(directory);
  }
  if (Metrics() != nullptr) {
    Metrics()->Increment("fss_posix_group_commits_total");
    Metrics()->Increment("fss_posix_batch_objects_total", {},
                         static_cast<std::int64_t>(entries.size()));
  }
}

fss::Result<void> PosixBlobStore::WriteTmpFile(const std::string& path,
                                               std::string_view data) const {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                        static_cast<mode_t>(options_.file_mode));
  if (fd < 0) return ErrFromErrno("创建临时文件失败");
  if (!WriteAll(fd, data.data(), data.size())) {
    const auto error = ErrFromErrno("写入临时文件失败");
    ::close(fd);
    (void)::unlink(path.c_str());
    return error;
  }
  if (::close(fd) != 0) {
    const auto error = ErrFromErrno("close 失败");
    (void)::unlink(path.c_str());
    return error;
  }
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
  //  ★ `storage.posix.fadvise_random`：读取路径的随机访问提示（默认关闭 → no-op）
  AdviseRandom(fd);

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
  //  ★ `storage.posix.fadvise_dontneed_after_large_read`：读出的字节数 > 1 MiB 时
  //    丢弃这段页缓存（只对大段读做；区间读不满阈值则不触发）。
  AdviseDontNeedAfterRead(fd, done);
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
  const std::string tmp = options_.atomic_write ? TempPathFor(to_path) : to_path;
  const int out = ::open(tmp.c_str(),
                         options_.atomic_write ? (O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC)
                                               : (O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC),
                         static_cast<mode_t>(options_.file_mode));
  if (out < 0) {
    const auto error = ErrFromErrno("创建复制目标失败");
    ::close(in);
    return error;
  }

  struct stat source_info {};
  const bool have_size = ::fstat(in, &source_info) == 0;
  const std::int64_t copied_bytes =
      have_size ? static_cast<std::int64_t>(source_info.st_size) : 0;
  //  ★ `copy` 路径**不参与** ADR-008 的 P4 组提交（它不走 put 的入批逻辑）。
  //    因此 `batch_commit=true` 时也按"单文件落盘"处理（安全方向）：少一次摊销，
  //    但绝不留下"已确认、数据未 durable"的副本。诚实偏差见 ADR-008 §6。
  const bool need_sync =
      options_.batch_commit ||
      ShouldFsync(options_.fsync_policy, copied_bytes, options_.fsync_threshold_bytes);

  bool ok = CopyFd(in, out);
  if (ok && need_sync) {
    ok = FileSync().DataSync(out).ok();
  }
  if (::close(in) != 0) ok = false;
  if (::close(out) != 0) ok = false;
  if (!ok || (options_.atomic_write && ::rename(tmp.c_str(), to_path.c_str()) != 0)) {
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
    found.emplace_back(relative, MtimeEpochSeconds(it->path()).value_or(clock_.NowEpochSeconds()));
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


//  ★ C9.25：清理残留的临时文件（键含 `.tmp.`）。
//  为什么不能用 `list()`：`list()` 用 `IsInternalKey` **跳过**临时文件与侧车文件 ——
//  那是"临时文件永远不是对象"的表达。GC 需要一个**专用**入口来清残留。
//  判据与 staging 的 TTL 扫描同源：mtime 早于 `older_than_epoch_seconds` 才动。
fss::Result<domain::TempSweepResult> PosixBlobStore::remove_temp_files(
    const std::string& container, std::int64_t older_than_epoch_seconds, bool dry_run) {
  FSS_TRY(dir, ContainerDir(container));
  domain::TempSweepResult result;
  if (!fs::Exists(dir)) return result;  // 容器不存在 == 没有残留

  namespace stdfs = std::filesystem;
  std::error_code ec;
  for (stdfs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    const std::string relative = stdfs::relative(it->path(), dir, ec).generic_string();
    if (ec) break;
    //  只处理**临时文件**（侧车文件随对象一起删，绝不能在这里删）
    if (relative.find(kTempMarker) == std::string::npos) continue;
    const auto mtime = MtimeEpochSeconds(it->path());
    if (!mtime.has_value()) {
      ++result.skipped_unknown_mtime;  // 状态未知 → 保护，不动它（删除方向必须保守）
      continue;
    }
    if (*mtime > older_than_epoch_seconds) {
      ++result.skipped_too_young;  // 太新 → 在途上传，保护
      continue;
    }
    ++result.removed;
    if (dry_run) continue;
    std::error_code unlink_error;
    stdfs::remove(it->path(), unlink_error);
    if (unlink_error) {
      return Err(fss::ErrorKind::kUnavailable,
                 "删除临时文件失败：" + it->path().string() + "：" + unlink_error.message());
    }
  }
  return result;
}

}  // namespace fss::infra
