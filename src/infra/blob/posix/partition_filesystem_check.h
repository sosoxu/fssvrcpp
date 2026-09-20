// =============================================================================
//  PartitionFilesystemCheck —— `storage.posix.one_filesystem_per_partition` 的启动期校验（L2）
// =============================================================================
//  为什么需要它（ADR-009 §6.3 / §8.3；`docs/operations.md` §10.3）
//  ---------------------------------------------------------------------------
//  `syncfs(2)` 是**文件系统级**操作：`PosixBlobStore` 的批提交 flush 的是"该文件系统
//  上的所有写入"，不只是本批、不只是本 partition、不只是本实例。多 partition 共盘时，
//  一个 partition 的提交会牵连另一个 partition（乃至**非本服务**的写入）的 IO。
//  部署建议因此是"每个 partition（或每组 partition）独立文件系统"。
//
//  配置键 `storage.posix.one_filesystem_per_partition` 是**部署方的断言**："我已经
//  按 partition 分盘了"。本模块在启动期**验证**这条断言，不成立就 fail-closed
//  （组合根 → `exit 78`）—— 而不是读了配置却什么都不做
//  （AGENTS §4.3："组合根没读的配置 = 不存在的配置"）。
//
//  规则（本切片钉住，不多不少）
//  ---------------------------------------------------------------------------
//    · **规则 A**：每个 partition 的目录（`staging` / `persistent`）解析到的文件系统
//      （`st_dev`）必须**不同于** `storage.posix.root` 所在的文件系统。`root` 上是探针
//      文件（`shared_mount_required`）、SQLite 元数据等；对象盘不该与它共盘。
//      解析一律用 `::stat(2)`，它**跟随符号链接** —— 这正是"目录是挂载点/软链"的
//      分辨方式（`std::filesystem` 没有 `st_dev` API，见
//      `src/infra/blob/posix/posix_blob_store.cpp` 的同类用法）。
//    · **规则 B**：**两个不同的 partition 不得解析到同一个文件系统**（同一个 `st_dev`
//      不能同时属于两个 partition）。这才是"按 partition 分盘"的字面含义。
//      ⚠️ 今天的组合根只从 `partition.file.opendes.*` 构造**一个** partition
//      （`StaticPartitionRegistry` 单例），所以规则 B 在**当前生产形态下是空集**；
//      它仍然实现（是该键的核心语义），并由**合成多 partition 输入**的用例钉住
//      （`tests/integration/test_partition_filesystem_check.cpp` U3）。
//
//  ★ **刻意不检查**：一个 partition 的 `staging_dir` 与 `persistent_dir` 是否在
//    **同一个**文件系统上。`docs/operations.md` §10.3 的布局表就为 staging 与
//    persistent 各列了一个独立卷。加一条"staging == persistent"的规则会把这种
//    合法部署误判为违规，因此**禁止**添加（U5 专门钉住这一点）。
//  ★ **刻意不检查（登记为未验证，不声称）**：该文件系统上是否还有**非本服务**的负载。
//    进程内无法观测（需要挂载表 / 外部审计），属部署纪律；本模块不声称能校验它。
//
//  只读保证
//  ---------------------------------------------------------------------------
//  本模块**只做 `stat`**：不创建、不修改、不删除任何东西。目录不存在时**报错**
//  （告诉运维先创建/挂载），而**不是**替它建目录 —— 否则"挂载点"会被一个空目录顶掉，
//  规则判定也随之失去意义（`PosixBlobStore::ObjectPath` 对缺失容器同样返回 `kNotFound`，
//  因此"要求目录存在"是与驱动一致的，不是更严）。
//  ★ 注意"谁在什么时候建目录"：组合根在调用本模块**之前**会先用既有的幂等
//    `IBlobStore::ensure_container`（GC 段本来就在做，见 `src/main/server_main.cpp`）
//    保证容器目录存在 —— 于是产品路径上的失败原因是"同盘/共盘"（规则 A/B）而不是
//    "目录缺失"。本模块自身仍然绝不建目录：该分支是**模块契约**，由直接调用者的用例
//    （U4）与"无副作用"断言钉住，避免将来有人把 ensure 逻辑搬进 L2。
// =============================================================================
#pragma once

#include "common/result/result.h"

#include <string>
#include <vector>

namespace fss::infra {

//  POSIX 驱动下，一个 partition 的两个物理目录（= `<storage.posix.root>/blobs/<container>`）。
struct PartitionDirSpec {
  std::string partition;       // 用于错误原因（例如 "opendes"）
  std::string staging_dir;     // 绝对路径
  std::string persistent_dir;  // 绝对路径
};

struct PartitionFilesystemReport {
  struct Entry {
    std::string partition;
    //  `staging_dir` 的 `st_dev` 与决定这个设备号的路径（`sample_path` 可 `ls`）。
    unsigned long long device = 0;
    std::string sample_path;
    //  `persistent_dir` 的 `st_dev` 与路径。★ 与本条的 `device` **可能不同**：
    //  §10.3 允许一个 partition 用两个独立卷；本模块**不**要求两者相等
    //  （那会把合法部署误判为违规，见文件头）。
    unsigned long long persistent_device = 0;
    std::string persistent_sample_path;
  };
  unsigned long long root_device = 0;
  std::vector<Entry> entries;  // 与入参同序
};

//  规则 A + 规则 B（见文件头注释）；违反 → `Err(kInvalidArgument, 可读原因)`。
//  `root` 不存在 / 不可 stat → 错误；`partitions` 为空 → 错误（不许"检查恒真"）。
//  路径必须是**绝对路径**（本模块不依赖当前工作目录）。
fss::Result<PartitionFilesystemReport> CheckOneFilesystemPerPartition(
    const std::string& root, const std::vector<PartitionDirSpec>& partitions);

}  // namespace fss::infra
