// SharedMountProbe 实现。协议、顺序约束与清理策略见头文件。
#include "infra/blob/posix/shared_mount_probe.h"

#include "common/fs/fs.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace fss::infra {

namespace {

constexpr const char* kProbePrefix = ".fss_probe.";
constexpr const char* kTmpMarker = ".tmp.";

//  探针文件内容里的实例标识行（写/读两侧的唯一约定）。
std::string InstanceIdLine(const std::string& instance_id) {
  return "instance_id=" + instance_id;
}

}  // namespace

SharedMountProbe::SharedMountProbe(std::string root, std::string instance_id)
    : root_(std::move(root)), instance_id_(std::move(instance_id)) {}

std::string SharedMountProbe::FileNameFor(std::string_view instance_id) {
  return std::string(kProbePrefix) + std::string(instance_id);
}

std::string SharedMountProbe::PathFor(std::string_view peer_id) const {
  return root_ + "/" + FileNameFor(peer_id);
}

fss::Result<void> SharedMountProbe::WriteOwn(std::int64_t now_epoch_millis) {
  if (instance_id_.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "共享挂载探针写入失败：本实例的 instance_id 为空");
  }
  fss::fs::WriteOptions options;
  //  探针文件只表达"此刻这个 root 在共享目录里可见"，不是业务数据：
  //  不需要数据/目录落盘（省掉每次心跳的 fsync）。
  options.fsync_data = false;
  options.fsync_dir = false;
  options.mode = 0644;
  //  ADR-009 M1：临时名必须含实例标识（多实例同目录写探针时不能互踩）。
  options.tmp_suffix = instance_id_;
  const std::string path = OwnPath();
  const std::string content = InstanceIdLine(instance_id_) +
                              "\nwritten_at_epoch_millis=" +
                              std::to_string(now_epoch_millis) + "\n";
  FSS_TRY(fss::fs::AtomicWriteFile(path, content, options));
  //  ★ 读回自证：写成功但读不回来（权限/挂载语义异常）也必须 fail-closed。
  FSS_TRY(read_back, fss::fs::ReadFile(path));
  const std::string line = InstanceIdLine(instance_id_);
  if (read_back != line && read_back.find(line + "\n") == std::string::npos) {
    return Err(fss::ErrorKind::kInternal,
               "共享挂载探针读回校验失败：" + path + " 的内容不是本实例写入的");
  }
  return Ok();
}

bool SharedMountProbe::PeerProbeVisible(std::string_view peer_id) const {
  if (peer_id.empty()) return false;
  const std::string path = PathFor(peer_id);
  const auto content = fss::fs::ReadFile(path);
  if (!content.ok()) return false;
  const std::string line = InstanceIdLine(std::string(peer_id));
  const std::string& text = content.value();
  //  必须是**整行**匹配（防止 peer `ab` 命中 peer `abc` 的文件）。
  return text == line || text.find(line + "\n") != std::string::npos;
}

fss::Result<void> SharedMountProbe::RemoveOwn() { return fss::fs::RemoveFile(OwnPath()); }

fss::Result<int> SharedMountProbe::CleanupStale(std::chrono::seconds max_age) const {
  if (max_age.count() <= 0) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "共享挂载探针清理失败：max_age 必须为正");
  }
  std::error_code error;
  const std::filesystem::path root(root_);
  if (!std::filesystem::is_directory(root, error)) return 0;  // 目录不存在 → 无需清理
  const std::string own = FileNameFor(instance_id_);
  int removed = 0;
  for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
    if (error) break;
    std::error_code type_error;
    if (!entry.is_regular_file(type_error)) continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind(kProbePrefix, 0) != 0) continue;          // 不是探针文件
    if (name.find(kTmpMarker) != std::string::npos) continue;  // 别人正在写的中间态
    if (name == own) continue;                               // 自己的文件由 RemoveOwn 负责
    std::error_code mtime_error;
    const auto mtime = std::filesystem::last_write_time(entry.path(), mtime_error);
    if (mtime_error) continue;
    if (std::filesystem::file_time_type::clock::now() - mtime <= max_age) continue;
    std::error_code remove_error;
    std::filesystem::remove(entry.path(), remove_error);
    if (!remove_error) ++removed;
  }
  return removed;
}

}  // namespace fss::infra
