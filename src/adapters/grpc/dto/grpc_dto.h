// =============================================================================
//  grpc_dto（L5）—— proto ↔ 领域模型的双向转换
// =============================================================================
//  约束（docs/02-design.md §3.2、契约 §4.2）
//    · proto 类型**只允许**出现在 `src/adapters/grpc/` 内（护栏 C7.7 强制执行）；
//      领域层不知道 protobuf 的存在。
//    · proto 的 `json_name` 已对齐 OSDU JSON（`data` 内 PascalCase、信封 camelCase），
//      因此"启用 proto3-JSON 的客户端"能直接生产/消费规范 JSON（C7.5）。
//
//  ⚠️ 一处**已知的表达力差异**（如实登记，不静默丢失）
//    proto3 没有"未知字段保留"：领域模型用 `extra`（`json::Value`）承载的**未识别字段**
//    无法经 proto 往返。规则：
//      · 领域 → proto：`data.extra["ExtensionProperties"]` → `data.extension_properties`，
//        `data.extra` 里的其它键**不会**出现在 proto 里；
//      · proto → 领域：`data.extension_properties` → `data.extra["ExtensionProperties"]`。
//    因此"全字段无损往返"只在 REST 链路上成立（C6.1 的语境）；RPC 链路按**已建模字段**
//    比对（C7.3 的等价性矩阵比较的是**领域结果**，不是 JSON 字符串）。
// =============================================================================
#pragma once

#include "app/usecases/usecases.h"
#include "common/result/result.h"
#include "domain/model/file_metadata.h"

#include <osdu/file/v1/file_service.pb.h>

namespace fss::adapters::grpc {

//  `InfoResponse`（运维端点，无错误路径）
void FillInfoProto(const fss::app::VersionInfo& info, osdu::file::v1::InfoResponse* out);

//  元数据记录：领域 → proto（`version` 只在响应里有值）
void FillMetadataProto(const fss::domain::FileMetadataRecord& record,
                       osdu::file::v1::FileMetadataRecord* out);
//  proto → 领域。结构性错误（缺必需段等）交给领域校验（`ValidateMetadataRecord`）报 400。
fss::Result<fss::domain::FileMetadataRecord> MetadataFromProto(
    const osdu::file::v1::FileMetadataRecord& proto);

}  // namespace fss::adapters::grpc
