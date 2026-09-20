// PartitionFilesystemCheck 实现。规则、刻意不检查的条目与只读保证见头文件。
#include "infra/blob/posix/partition_filesystem_check.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fss::infra {

namespace {

constexpr const char* kKey = "storage.posix.one_filesystem_per_partition";

//  规则判定与错误文案共用的一句话（让"为什么重要"只有一处、不会两处漂移）。
const char* kWhy =
    "syncfs(2) 是**文件系统级**操作：批提交会 flush 整个文件系统，与该 partition 共盘的"
    "任何写入都会互相牵连（ADR-009 §6.3/§8.3；docs/operations.md §10.3）";
const char* kFix =
    "把该目录挂到自己的文件系统（独立卷 / 独立 export）后重启；"
    "若部署上确实不打算按 partition 分盘，就把 storage.posix.one_filesystem_per_partition "
    "设回 false（放弃这条断言）";

bool IsAbsolute(const std::string& path) { return !path.empty() && path.front() == '/'; }

//  `::stat` **跟随符号链接** —— 这正是"目录是挂载点/软链"的分辨方式。
//  返回 false 时 `reason` 里带 errno 文本（调用方拼进错误消息）。
bool StatDevice(const std::string& path, unsigned long long* device, std::string* reason) {
  struct ::stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    *reason = std::strerror(errno);
    return false;
  }
  *device = static_cast<unsigned long long>(info.st_dev);
  return true;
}

std::string DeviceText(unsigned long long device) { return std::to_string(device); }

}  // namespace

fss::Result<PartitionFilesystemReport> CheckOneFilesystemPerPartition(
    const std::string& root, const std::vector<PartitionDirSpec>& partitions) {
  //  ★ 空输入必须**报错**，不能是"静默恒真通过"（AGENTS §4.3 的"检查恒真"陷阱）。
  if (partitions.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument,
               std::string(kKey) +
                   " 校验失败：没有 partition 可检查。空输入下「全部通过」是恒真命题，"
                   "不是证据；组合根必须至少给出一个 partition 的目录。"
                   "下一步：确认 partition 注册表非空（组合根当前由 "
                   "partition.file.opendes.* 构造唯一的 partition）。");
  }

  //  ★ 绝对路径：否则 `::stat` 会相对**当前工作目录**解析 → 检查结果随 CWD 漂移。
  if (!IsAbsolute(root)) {
    return Err(fss::ErrorKind::kInvalidArgument,
               std::string(kKey) +
                   " 校验失败：storage.posix.root 必须是绝对路径（实际：\"" + root +
                   "\"）。相对路径会随进程的工作目录解析，检查结果不可复现。"
                   "下一步：把 storage.posix.root 写成绝对路径。");
  }
  for (const auto& spec : partitions) {
    if (spec.partition.empty()) {
      return Err(fss::ErrorKind::kInvalidArgument,
                 std::string(kKey) +
                     " 校验失败：存在 partition 名为空的条目（无法定位责任分区）。");
    }
    if (!IsAbsolute(spec.staging_dir) || !IsAbsolute(spec.persistent_dir)) {
      return Err(fss::ErrorKind::kInvalidArgument,
                 std::string(kKey) + " 校验失败：partition \"" + spec.partition +
                     "\" 的目录必须是绝对路径（staging=\"" + spec.staging_dir +
                     "\"，persistent=\"" + spec.persistent_dir +
                     "\"）。相对路径会随进程的工作目录解析，检查结果不可复现。");
    }
  }

  //  ---- root：必须存在且可 stat（探针文件/元数据也落在它上面）----
  PartitionFilesystemReport report;
  std::string root_reason;
  if (!StatDevice(root, &report.root_device, &root_reason)) {
    return Err(fss::ErrorKind::kInvalidArgument,
               std::string(kKey) + " 校验失败：storage.posix.root 不存在或无法 stat：" + root +
                   "（" + root_reason + "）。下一步：先创建/挂载该目录再启动。");
  }

  //  ---- 逐 partition 解析两个目录；目录必须**已经存在**（只读，不替运维建）----
  //  同时记下每个 (partition, zone) 的路径与设备，规则 A/B 与错误文案共用，
  //  避免二次 `stat` 让"报告的路径"与"判定的路径"出现两套。
  struct ResolvedDir {
    std::string zone;  // "staging" / "persistent"
    std::string path;
    unsigned long long device = 0;
  };
  std::vector<std::vector<ResolvedDir>> resolved(partitions.size());
  //  device → 使用它的 partition 集合（规则 B 的唯一判据来源）。
  std::map<unsigned long long, std::set<std::string>> device_owners;
  for (std::size_t i = 0; i < partitions.size(); ++i) {
    const auto& spec = partitions[i];
    const auto stat_one = [&](const std::string& dir,
                              const char* zone) -> fss::Result<unsigned long long> {
      std::string reason;
      unsigned long long device = 0;
      if (!StatDevice(dir, &device, &reason)) {
        return Err(fss::ErrorKind::kInvalidArgument,
                   std::string(kKey) + " 校验失败：partition \"" + spec.partition + "\" 的 " +
                       zone + " 目录不存在或无法 stat：" + dir + "（" + reason +
                       "）。本检查**只读**，不会替运维创建目录 —— 请先创建/挂载它再启动"
                       "（POSIX 驱动本身也要求容器目录存在）。");
      }
      return device;
    };
    FSS_TRY(staging_device, stat_one(spec.staging_dir, "staging"));
    FSS_TRY(persistent_device, stat_one(spec.persistent_dir, "persistent"));

    PartitionFilesystemReport::Entry entry;
    entry.partition = spec.partition;
    entry.device = staging_device;
    entry.sample_path = spec.staging_dir;
    entry.persistent_device = persistent_device;
    entry.persistent_sample_path = spec.persistent_dir;
    report.entries.push_back(std::move(entry));
    resolved[i] = {{"staging", spec.staging_dir, staging_device},
                   {"persistent", spec.persistent_dir, persistent_device}};
    device_owners[staging_device].insert(spec.partition);
    device_owners[persistent_device].insert(spec.partition);
  }

  //  ---- 规则 A：partition 的目录必须与 root **分盘** ----
  std::vector<std::pair<std::string, ResolvedDir>> same_as_root;  // (partition, dir)
  for (std::size_t i = 0; i < partitions.size(); ++i) {
    for (const auto& dir : resolved[i]) {
      if (dir.device == report.root_device) {
        same_as_root.emplace_back(partitions[i].partition, dir);
      }
    }
  }

  //  ---- 规则 B：同一个文件系统不得同时属于两个**不同**的 partition ----
  std::vector<std::pair<unsigned long long, std::set<std::string>>> shared_devices;
  for (const auto& [device, owners] : device_owners) {
    if (owners.size() > 1) shared_devices.emplace_back(device, owners);
  }

  if (!same_as_root.empty() || !shared_devices.empty()) {
    std::ostringstream message;
    message << kKey << " 校验失败：部署断言「每个 partition 独占文件系统」不成立。\n"
            << "  storage.posix.root : " << root << "（st_dev=" << DeviceText(report.root_device)
            << "）\n";

    if (!same_as_root.empty()) {
      message << "  ---- 规则 A：partition 的目录必须与 root 分盘 ----\n";
      for (const auto& [partition, dir] : same_as_root) {
        message << "  partition \"" << partition << "\" 的 " << dir.zone << " 目录 " << dir.path
                << "（st_dev=" << DeviceText(dir.device)
                << "）与 storage.posix.root 在**同一个文件系统**上。\n";
      }
    }
    if (!shared_devices.empty()) {
      message << "  ---- 规则 B：两个 partition 不得共用一个文件系统 ----\n";
      for (const auto& [device, owners] : shared_devices) {
        message << "  st_dev=" << DeviceText(device) << " 同时被 " << owners.size()
                << " 个 partition 使用：";
        bool first = true;
        for (const auto& owner : owners) {
          if (!first) message << "、";
          first = false;
          message << "\"" << owner << "\"";
          //  把该 partition 落在该设备上的**具体路径**列出来（运维可直接 `ls`/`stat`）。
          for (std::size_t i = 0; i < partitions.size(); ++i) {
            if (partitions[i].partition != owner) continue;
            for (const auto& dir : resolved[i]) {
              if (dir.device == device) message << "（" << dir.path << "）";
            }
          }
        }
        message << "\n";
      }
    }

    message << "  为什么重要：" << kWhy << "。\n"
            << "  修法：" << kFix << "。";
    return Err(fss::ErrorKind::kInvalidArgument, message.str());
  }

  return report;
}

}  // namespace fss::infra
