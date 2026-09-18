// =============================================================================
//  ObjectKeyPolicy（L4）：`FileSource` ↔ `ObjectRef` 的双向映射与安全校验
// =============================================================================
//  契约依据
//    · §1.5：`FileSource = "/" + "<userId>/<epochMillis>-<yyyy-MM-dd-HH-mm-ss-SSS>/<fileID>"`
//             长度上限 **1024**（上游 Azure 常量 `AZURE_MAX_FILEPATH`）
//    · §1.5：旧接口的 `FileID` 正则 `^[\w,\s-]+(\.\w+)?$`，且有长度上限
//    · §2.1 的响应样例：对象存储下 URL 里是**日期分层**的 key（`.../2025/09/16/<fileID>`）
//
//  ★ 为什么"映射"要单独成一层：它是**安全边界**。`FileSource` 由客户端提供
//    （`CreateFileMetadata` 的输入），而它决定"对象写到哪/从哪复制"。任何一个
//    `..`、绝对路径、控制字符没拦住，就是任意文件读写。因此本层：、
//      ① 生成：只允许来自受控组件（user_id 由调用方从 token 取、file_id 由我们生成）
//      ② 解析：**白名单式**逐段校验，任何不符预期的形态一律拒绝（不"尽力而为"）
//  C2.4 要求 ≥10 个恶意输入全部被拒 —— 测试里逐条列出，并配合法对照。
#pragma once

#include "common/result/result.h"
#include "domain/model/types.h"
#include "domain/ports/ports.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace fss::app {

class ObjectKeyPolicy {
 public:
  //  1024 是上游硬上限（Azure 路径）；S3 key 上限同样是 1024 字节
  static constexpr std::size_t kMaxFileSourceLength = 1024;
  //  旧接口 `FileID` 的长度上限。上游只写了"有上限"，这里取一个明确且足够宽松的值：
  //  1024 减去前缀后仍能容纳；同时避免超长 id 撑爆日志与路径。
  static constexpr std::size_t kMaxFileIdLength = 512;

  //  FileSource 的四个组成部分（解析成功时全部非空）
  struct SourcePath {
    std::string user_id;
    std::int64_t epoch_millis = 0;
    std::string timestamp;  // `yyyy-MM-dd-HH-mm-ss-SSS`
    std::string file_id;

    bool operator==(const SourcePath& o) const {
      return user_id == o.user_id && epoch_millis == o.epoch_millis &&
             timestamp == o.timestamp && file_id == o.file_id;
    }
  };

  //  生成：`/` + `<user_id>/<epoch_millis>-<timestamp>/<file_id>`
  //  参数不合法（空、含分隔符/控制字符、超长）→ kInvalidArgument（**不生成**可疑路径）
  static Result<std::string> MakeFileSource(const SourcePath& parts);

  //  解析：白名单校验（恰好 3 段、前导 '/'、时间戳格式、file_id 规则、长度上限）
  static Result<SourcePath> ParseFileSource(std::string_view file_source);

  //  对象键（相对 container）
  //    POSIX：与 FileSource 同形（去掉前导 '/'），便于运维直接按 FileSource 到盘上找文件
  //    S3   ：日期分层 `<yyyy>/<MM>/<dd>/<file_id>`（契约 §2.1 的 URL 样例）
  static std::string MakePosixKey(const SourcePath& parts);
  static std::string MakeS3Key(const SourcePath& parts);

  //  容器名：`<partition>-<zone 小写>`（如 `opendes-staging`）—— **默认命名**，
  //  与接线前逐字一致。
  static Result<std::string> ContainerFor(std::string_view partition, domain::StorageZone zone);

  //  ★ 阶段 10（C10.16 续）：分区级容器名覆盖
  //    `partition.file.<partition>.{staging,persistent}_container`。配置为空串时
  //    **退回上面的默认命名**（逐字一致），非空时用配置值（同样过安全白名单校验）。
  static Result<std::string> ContainerFor(const domain::PartitionConfig& config,
                                          domain::StorageZone zone);

  //  经租户注册表解析容器名：注册表查不到该 partition → 退回默认命名。所有需要
  //  "与分区配置一致的容器名"的调用点（用例 / GC / 组合根）都走这一条。
  static Result<std::string> ContainerFor(domain::IPartitionRegistry& partitions,
                                          std::string_view partition, domain::StorageZone zone);

  //  旧接口 `FileID` 校验：`^[\w,\s-]+(\.\w+)?$` + 长度上限（契约 §1.5）
  static bool IsValidFileId(std::string_view file_id);

  //  `yyyy-MM-dd-HH-mm-ss-SSS`（上游用该格式，注意是 `-` 分隔的日期时间）
  static std::string FormatTimestamp(std::int64_t epoch_seconds, int millis);

 private:
  //  单段白名单：只允许 `[A-Za-z0-9._-]`，且不得是 `.`/`..`、不得含控制字符
  static bool IsSafeSegment(std::string_view segment);
};

}  // namespace fss::app
