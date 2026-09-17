// =============================================================================
//  IFileSync（L2 内部接缝）—— 把「何时真正落盘」变成可观测、可注入的策略
// =============================================================================
//  为什么需要这个接缝
//    R15 / C3.11：`fsync_policy=by_size` 的语义是"小于阈值不 fsync、大于阈值必须 fsync"。
//    如果只能用真实系统调用，测试就只能"猜"（要么统计 `/proc`，要么假设实现正确）。
//    注入 `IFileSync` 之后，测试可以**直接断言**"这条写入路径到底调了几次 DataSync"。
//
//  ⚠️ 这不是 L3 的端口：它没有领域语义，也不跨层，只服务于 L2 的写入路径。
//     因此它**不**计入"15 个端口"（C2.11），也不暴露给应用层。
//
//  与 ADR-008 的关系
//    ADR-008 的 R1/R2 要求"数据先落盘再改名、改名后 fsync 目录"。
//    本接缝把这两步分别映射到 `DataSync(fd)` 与 `SyncDirectory(dir)`。
#pragma once

#include "common/result/result.h"

#include <cstdint>
#include <string>

namespace fss::infra {

//  `fsync` 分级策略（`docs/02-design.md` §13.4；配置项 `storage.posix.fsync_policy`）
//  ⚠️ 命名沿革：ADR-008 把配置项改名为 `durability: batch | per_file`（00-final §5 第 237 行）。
//     本枚举表达的是**更细的第三档**（按大小），C3.11 明确按 `by_size` 验收；P9 的配置层
//     再把 `durability` 映射到本枚举。
enum class FsyncPolicy {
  kAlways,   // 每次写入都落盘（最强耐久性，小文件吞吐最低）
  kBySize,   // 小于阈值的文件不 fsync（靠批提交/组提交摊销），达到阈值必须 fsync
  kNever,    // 不 fsync（仅用于"可重建数据"或测试）
};

class IFileSync {
 public:
  virtual ~IFileSync() = default;
  //  把文件数据刷到持久介质（对应 `fdatasync`）
  virtual fss::Result<void> DataSync(int fd) = 0;
  //  把目录项刷到持久介质（对应"打开目录 + fsync"；rename 之后必须做）
  virtual fss::Result<void> SyncDirectory(const std::string& directory) = 0;
};

//  生产实现。失败时把 errno 映射成结构化错误（不抛异常）。
class RealFileSync final : public IFileSync {
 public:
  fss::Result<void> DataSync(int fd) override;
  fss::Result<void> SyncDirectory(const std::string& directory) override;
};

//  按策略判断"这次写入是否需要 DataSync"
bool ShouldFsync(FsyncPolicy policy, std::int64_t object_bytes,
                 std::int64_t threshold_bytes);

}  // namespace fss::infra
