// =============================================================================
//  PosixBlobStore（L2）—— 集中存储驱动（ADR-003 / ADR-008）
// =============================================================================
//  布局（ADR-003 §4）：
//      <root>/<container>/<key>           对象本体
//      <root>/<container>/<key>.fssmeta   元数据 sidecar（content_type / checksum / …）
//
//  ★ 为什么需要 sidecar
//    共享契约（`tests/framework/port_contract.h`）要求 `stat()` 回显 put 时的
//    `content_type` 与校验和。POSIX 文件系统本身不存这些；不落 sidecar 就必须
//    "按扩展名猜"，那等于让契约退化成"碰巧"。sidecar 是**内部键**：
//      · `list` 跳过它（以及 `.tmp.*` 临时文件）；
//      · `remove`/`copy` 与对象本体一起处理；
//      · 原子性：本体先 `rename`，再写 sidecar（最坏情况是"有对象、无元数据"，
//        不会出现"半截对象"，见 C3.3）。
//
//  ★ 安全（T1/T2）
//    容器名与键都必须经过 `fs::SafeJoin`（词法校验 + 真实路径解析）。
//    路径穿越、绝对路径、NUL、符号链接逃逸一律 `kInvalidArgument`。
//
//  ★ 流式（C3.4）
//    `put` 边读流边写盘边增量算 SHA-256；`get` 用 `pread` 循环 + 固定缓冲。
//    内存占用与对象大小**无关**（默认 64 KiB 缓冲）。
#pragma once

#include "common/result/result.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/io/file_sync.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace fss::infra {

//  ★ 阶段 10（C10.16 续）：把 `storage.posix.fadvise_*` 变成**可断言**的事实。
//    `posix_fadvise` 是"尽力而为"的提示，没有可依赖的返回值，真实系统调用也无法在单测里
//    被观察到。因此用一个**可注入的接缝**（与 `IFileSync` 同族）：生产实现调
//    `::posix_fadvise`，测试注入计数实现 → "配置被读取且该路径被执行"成为硬断言（R15）。
class IFadviseSink {
 public:
  virtual ~IFadviseSink() = default;
  virtual void Random(int fd) = 0;
  virtual void DontNeed(int fd) = 0;
};

struct PosixBlobStoreOptions {
  //  何时真正落盘（ADR-008 的 R1/R2；语义见 `infra/io/file_sync.h`）。
  //  默认 `kAlways`：库层保持最保守的耐久性；部署层按配置选 `kBySize`（默认值见
  //  `docs/02-design.md` §13.4）。
  FsyncPolicy fsync_policy = FsyncPolicy::kAlways;
  std::int64_t fsync_threshold_bytes = 1024 * 1024;  // 1 MiB
  //  临时文件名里的实例标识（ADR-009 M1：多实例共盘时不得互相踩）
  std::string instance_id = "local";
  //  可注入的落盘接缝（测试用）；null → 真实的 fdatasync/fsync
  IFileSync* file_sync = nullptr;

  //  ---- 阶段 10：`storage.posix.{dir_mode,file_mode,atomic_write,fadvise_*}` ----
  //  ★ 默认值 = **接线前的行为**（否则直接构造本结构体的既有驱动/契约测试会变）：
  //    · 目录 0750 与既有 `fs::EnsureDir` 默认逐位一致；
  //    · 文件 0644 与既有 `::open(..., 0644)` 一致（schema 默认 `0640` 由组合根显式传入）；
  //    · `atomic_write=true`（tmp + rename）与既有实现一致；
  //    · 两个 fadvise 默认 `false`（接线前从不下发提示）。
  unsigned dir_mode = 0750;
  unsigned file_mode = 0644;
  //  `false` → **直接写目标文件**（不做 tmp + rename）。失败时删除目标，绝不留下半成品。
  bool atomic_write = true;
  bool fadvise_random = false;
  //  读出的字节数**大于** 1 MiB 阈值时才下发 `POSIX_FADV_DONTNEED`（见 .cpp 的常量）。
  bool fadvise_dontneed_after_large_read = false;
  //  可注入的 fadvise 接缝（测试用）；null → 真实 `::posix_fadvise`
  IFadviseSink* fadvise_sink = nullptr;
};

class PosixBlobStore final : public domain::IBlobStore {
 public:
  PosixBlobStore(std::string root, const fss::IClock& clock,
                 PosixBlobStoreOptions options = {});

  domain::BlobCapabilities capabilities() const override;
  fss::Result<void> ensure_container(const std::string& container) override;
  fss::Result<domain::SignedLocation> presign_put(
      const domain::ObjectRef& ref, const domain::PresignOptions& options) override;
  fss::Result<domain::SignedLocation> presign_get(
      const domain::ObjectRef& ref, const domain::PresignOptions& options) override;
  fss::Result<void> put(const domain::ObjectRef& ref, bytes::ByteSource& source,
                        const domain::PutOptions& options) override;
  fss::Result<void> get(const domain::ObjectRef& ref, bytes::ByteSink& sink,
                        const domain::ByteRange& range) override;
  fss::Result<domain::ObjectStat> stat(const domain::ObjectRef& ref) override;
  fss::Result<void> remove(const domain::ObjectRef& ref) override;
  fss::Result<domain::ObjectStat> copy(const domain::ObjectRef& from,
                                       const domain::ObjectRef& to) override;
  fss::Result<domain::ListPage> list(const std::string& container, const std::string& prefix,
                                     const std::string& continuation_token, int limit) override;

  //  ★ C9.25：清理**残留的临时文件**（键含 `.tmp.`）。`dry_run` 只统计。
  fss::Result<domain::TempSweepResult> remove_temp_files(const std::string& container,
                                                 std::int64_t older_than_epoch_seconds,
                                                 bool dry_run) override;


  //  测试可观测性
  const std::string& root() const { return root_; }
  static constexpr std::string_view kSidecarSuffix = ".fssmeta";
  static constexpr std::string_view kTempMarker = ".tmp.";

 private:
  fss::Result<std::string> ContainerDir(std::string_view container) const;
  fss::Result<std::string> ObjectPath(std::string_view container, std::string_view key) const;
  // 可写对象路径：ObjectPath + 创建中间目录 + **重新**做真实路径校验。
  // 键可以含 '/'（ObjectKeyPolicy 的 `<user>/<ts>/<fileID>`），因此父目录要先建出来；
  // 建完再校验一次，堵住"两次校验之间把某段换成符号链接"的窗口（TOCTOU 加固）。
  fss::Result<std::string> WritableObjectPath(std::string_view container,
                                              std::string_view key) const;
  fss::Result<void> WriteSidecar(const std::string& object_path, const domain::ObjectStat& st,
                                 bool fsync) const;
  fss::Result<domain::ObjectStat> ReadSidecar(const std::string& object_path) const;
  IFileSync& FileSync() const;
  IFadviseSink& Fadvise() const;
  //  `fadvise_random=true` 时对打开的 fd 下发一次随机访问提示（配置为 false 时 no-op）
  void AdviseRandom(int fd) const;
  //  `fadvise_dontneed_after_large_read=true` 且本次读出 > 阈值时下发 DONTNEED
  void AdviseDontNeedAfterRead(int fd, std::uint64_t bytes_read) const;
  static bool IsInternalKey(std::string_view relative_key);
  std::string TempPathFor(const std::string& target) const;

  std::string root_;
  const fss::IClock& clock_;
  PosixBlobStoreOptions options_;
  //  ★ 临时文件名的序号必须是**进程级**的，不能是"每个 store 对象各自从 0 开始"：
  //    同一个进程里两个 store 对象（不同 zone / 不同 partition 但同一个 root，
  //    或同一份配置被装配两次）如果共享 `instance_id`，各自从 0 开始的序号会让
  //    它们算出**同一个临时路径** → 后者 `O_CREAT|O_EXCL` 直接失败，或（若不用 O_EXCL）
  //    两个写入者交错写同一份临时文件，改名后得到**两份数据混在一起**的对象（ADR-009 M1）。
  //    进程级序号 + pid（跨进程）+ instance_id（跨实例）三者合起来才能保证唯一。
  static std::atomic<std::uint64_t> tmp_counter_;
};

}  // namespace fss::infra
