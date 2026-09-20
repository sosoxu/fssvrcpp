// =============================================================================
//  SharedMountProbe —— `storage.posix.shared_mount_required` 的探针文件（L2）
// =============================================================================
//  为什么需要它（ADR-009 §8.1 item 4）
//  ---------------------------------------------------------------------------
//  `deployment.mode=multi` 的前提是 `storage.posix.root` 落在**所有实例都能看到**
//  的挂载上。若各实例实际看到的是各自的本地目录，那么：
//    · `staging → persistent` 的 rename 只在写它的实例上可见（GC/读路径看不到）；
//    · 临时文件的实例标识毫无意义（根本不会撞在同一目录上）；
//    · 租约/回收流程看起来"能跑"，实际在多实例下静默失效。
//  这类误配在单机手工验证里**不会暴露**（两个进程共享本地目录也能跑通 E2E），
//  只有"一个实例写、另一个实例读"才能证明共享性。
//
//  本模块实现 ADR-009 §8.1 item 4 的协议里**文件系统那一半**：
//    · 写 `<root>/.fss_probe.<instance_id>`（内容含 instance id + 时间戳）；
//    · 读回自证（写不进去/读不回来 = 这个 root 不可用）；
//    · 判定"某个 **live peer** 的探针文件在本实例的 root 下是否可见"；
//    · 退出时删掉自己的探针文件；启动时按 mtime 清理**过老**的探针文件。
//  "哪些 peer 是 live 的"来自 `instance_registry`（PG 协作，见 `pg_instance_registry.h`）
//  —— 探针文件本身不参与"谁是活的"判定（否则崩溃实例留下的旧文件会永远看起来像 peer）。
//
//  ★ 顺序约束（组合根必须遵守）：**先写自己的探针文件，再注册 instance_registry 行**。
//    这样"对端注册后立刻查我"时，我的探针一定已经在共享目录里（哪怕对端自己的
//    探针还没写）—— 否则两个同时启动的实例会互相判为"不可见"，误报不是共享挂载。
//
//  ★ 清理策略：`CleanupStale` 只删 mtime 过老、且**不是** `.tmp.` 中间态的文件
//    （`fs::AtomicWriteFile` 的临时名也以 `.fss_probe.` 开头，但含 `.tmp.`）。
//    从不删自己的文件（自己的文件由 `RemoveOwn` 负责）。
// =============================================================================
#pragma once

#include "common/result/result.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace fss::infra {

class SharedMountProbe {
 public:
  SharedMountProbe(std::string root, std::string instance_id);

  //  `.fss_probe.<instance_id>`（同一约定的单一来源：写、读、清理都走它）。
  static std::string FileNameFor(std::string_view instance_id);

  const std::string& root() const { return root_; }
  const std::string& instance_id() const { return instance_id_; }
  std::string OwnPath() const { return PathFor(instance_id_); }
  //  `<root>/.fss_probe.<peer_id>` —— 错误原因里要给出完整路径（运维需要能 `ls`）。
  std::string PathFor(std::string_view peer_id) const;

  //  写自己的探针文件（内容 = instance id + 写入时刻）+ **读回自证**。
  //  任一失败 → 该 root 不可用（组合根 fail-closed → exit 78）。
  fss::Result<void> WriteOwn(std::int64_t now_epoch_millis);

  //  对端探针文件是否在本实例的 root 下可见：能读到，且内容里的 `instance_id=`
  //  确为该 peer（防止把"名字对了但内容不是"的半截/陈旧文件当成可见）。
  //  任何 IO 错误 → false（不可见）。
  bool PeerProbeVisible(std::string_view peer_id) const;

  //  删除自己的探针文件（不存在也算成功；优雅退出路径）。
  fss::Result<void> RemoveOwn();

  //  删除 root 下 mtime 早于 `max_age` 的 `.fss_probe.*`（不含自己的、不含 `.tmp.`），
  //  返回删除个数。目录不存在 → 0（调用方在 WriteOwn 阶段已 fail-closed）。
  fss::Result<int> CleanupStale(std::chrono::seconds max_age) const;

 private:
  std::string root_;
  std::string instance_id_;
};

}  // namespace fss::infra
