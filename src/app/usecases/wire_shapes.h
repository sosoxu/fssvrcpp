// =============================================================================
//  wire_shapes（L4）—— 两条协议**共用**的线上 JSON 形状
// =============================================================================
//  为什么放在 L4 而不是某个适配层
//    DMS 的 `storageLocation` / `retrievalProperties` 是"以 JSON 对象为载荷"的字段：
//      · REST 把它直接写进响应体（`docs/03-api-contract.md` §2.9）；
//      · gRPC 的同名字段是 `google.protobuf.Struct` —— **同一个 JSON 对象**。
//    键名与集合版/单文件版的差异属于**契约语义**，不是某个协议的格式细节；放在这里
//    才能让两条链路共用一份定义（C6.7 的键集合 + C7.3 的等价性都依赖这一点）。
//    若各适配层各写一份，"files 用 fileSource、collections 用 fileCollectionSource"
//    这种差异必然会在其中一条链路上漏掉。
//
//  ★ 只放"形状"，不放协议：返回 `json::Value`，不含 HTTP 状态码/gRPC 状态。
// =============================================================================
#pragma once

#include "common/json/json.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fss::app {

//  路径最后一段（`fileNames` 里的"文件名"）
std::string FileNameOfPath(std::string_view path);

//  DMS 的"位置对象"：
//    · 单文件版（`/v2/files/*`）      → `signedUrl` / `fileSource` / `createdBy` / `expiryTime`
//    · 集合版（`/v2/file-collections/*`）→ `signedUrl` / **`fileCollectionSource`** /
//      `fileCount` / `fileNames` / `createdBy` / `expiryTime`（**没有** `fileSource`）
//  依据：上游 `AzureFileDmsUploadLocation` / `AzureFileCollectionDmsUploadLocation`
//  （见契约 §2.9 的逐键表）。
//  `CopyDmsRequest.datasetSources` 的元素既可能是**记录对象**（上游形态：
//  `{"data":{"DatasetProperties":{"FileSourceInfo":{"FileSource":...}}}}`），
//  也可能是**直接的路径字符串**（部分客户端这样调）。两条协议必须用**同一套**取值规则，
//  否则"同一份输入"在 REST 与 gRPC 上会得到不同结果（C7.3 的等价性）。
//  取不到时返回空串，由用例给出契约 §5 的固定消息（不在这里静默跳过）。
std::string FileSourceFromRecordNode(const json::Value& node);

json::Value DmsLocationJson(std::string_view signed_url, std::string_view source,
                            std::string_view created_by, std::int64_t expires_at_epoch_seconds,
                            bool collection, int file_count,
                            const std::vector<std::string>& file_names);

}  // namespace fss::app
