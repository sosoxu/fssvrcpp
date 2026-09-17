// =============================================================================
//  fss::fs —— 安全路径与原子写（L1）
// =============================================================================
//
//  这是集中存储（POSIX）驱动的安全地基，对应契约里的硬要求：
//    * **路径穿越必须被拒**：`..`、绝对路径、NUL、空段、超长、符号链接逃逸
//      （docs/adr/ADR-003 §待办 / docs/02-design.md §15 T1/T2）
//    * **绝不产生半截文件**：tmp + fsync + rename（ADR-008 的 R3 不变量）
//
//  为什么分两层校验
//  ---------------------------------------------------------------------------
//    ① `LexicalJoin`  —— 纯字符串运算：把 key 规范化后拼到 root 下，
//                        并拒绝一切词法逃逸。**不访问文件系统**，因此可被
//                        单元测试穷举恶意输入。
//    ② `VerifyResolvedWithinRoot` —— 访问文件系统：对**已存在的最长前缀**做
//                        真实路径解析，确认解析结果仍在 root 之内。
//                        这是防符号链接逃逸的关键（词法上完全合法的 key
//                        可能通过一个指向外部的符号链接逃出去）。
//  两者必须同时通过。
#pragma once

#include "common/result/result.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace fss::fs {

// -----------------------------------------------------------------------------
//  路径
// -----------------------------------------------------------------------------
//  词法安全拼接：返回 root + "/" + 规范化后的 key。
//  被拒的形态（ErrorKind = kInvalidArgument，details 里带 "reason"）：
//    空 key、"."、".."、含 NUL、绝对路径、任何 ".." 段、
//    空段（`a//b`）、"." 段（`a/./b`）、超长（> kMaxKeyBytes）、
//    以 '/' 结尾（目录语义，本服务只接受文件键）
inline constexpr std::size_t kMaxKeyBytes = 1024;  // 对齐上游 Azure 的 1024 限制
Result<std::string> LexicalJoin(std::string_view root, std::string_view key);

//  真实路径校验：对 path 的**最长已存在前缀**做 canonical 解析，
//  要求结果位于 root 的 canonical 路径之内。root 不存在时返回错误。
Result<void> VerifyResolvedWithinRoot(std::string_view root, std::string_view path);

//  LexicalJoin + VerifyResolvedWithinRoot
Result<std::string> SafeJoin(std::string_view root, std::string_view key);

// -----------------------------------------------------------------------------
//  原子写（ADR-008：绝不出现"半截文件"）
// -----------------------------------------------------------------------------
struct WriteOptions {
  bool fsync_data = true;   // 写完后 fdatasync（ADR-008 的 R1：数据必须先落盘再改名）
  bool fsync_dir = true;    // rename 后 fsync 目录（R2：改名必须落盘）
  unsigned mode = 0644;
  // 临时文件后缀要含实例标识，避免多实例在同一共享目录上互相踩（ADR-009 M1）
  std::string tmp_suffix;
};

//  写临时文件 → (可选) fdatasync → rename 到目标 → (可选) fsync 目录。
//  失败时尽力删除临时文件；**不会**在目标路径留下半截内容。
Result<void> AtomicWriteFile(std::string_view path, std::string_view data,
                             const WriteOptions& opts = {});

// -----------------------------------------------------------------------------
//  基础操作（薄封装，统一错误语义；不吞 errno）
// -----------------------------------------------------------------------------
Result<void> EnsureDir(std::string_view path, unsigned mode = 0750);
bool Exists(std::string_view path);
Result<std::uint64_t> FileSize(std::string_view path);
Result<std::string> ReadFile(std::string_view path);
Result<void> RemoveFile(std::string_view path);
//  读一段（定位读，天然线程安全 —— 用 pread 而不是 lseek+read）
Result<std::string> ReadAt(std::string_view path, std::uint64_t offset, std::size_t length);

}  // namespace fss::fs
