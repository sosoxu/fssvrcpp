// =============================================================================
//  grpc_error_mapper（L5）—— `ErrorKind` → `::grpc::Status`
// =============================================================================
//  契约依据：§5 的**唯一权威表**（数据在 L3：`domain/contract/error_table.h`）。
//  ★ 与 REST 的 `HttpErrorMapper` **共用同一张表**：两处只有"渲染方式"不同，
//    映射关系不得各写一份（ADR-001 的双协议等价性）。
//
//  附加信息（契约 §5 备注里的"details 携带期望/实际"）：
//    · 错误消息 → `::grpc::Status::error_message()`
//    · `ErrorKind` 名字 → 尾随元数据 `fss-error-kind`（REST 错误体里的 `code`/`reason` 等价物）
//    · 领域错误的 `details`（键值对）→ 尾随元数据 `fss-error-details`（`k=v; k=v`）
//  ★ 尾随元数据必须写在 `ServerContext` 上（而不是塞进 `::grpc::Status`），
//    这是 gRPC 的接口约定：status 只有 code+message 两个字段。
// =============================================================================
#pragma once

#include "common/result/result.h"

#include <grpcpp/grpcpp.h>

namespace fss::adapters::grpc {

//  `ErrorKind` 的数值 gRPC 状态码（测试会用 `::grpc::StatusCode` 逐个对齐）
int GrpcStatusFor(fss::ErrorKind kind);

//  领域错误 → `::grpc::Status`（code + message）
::grpc::Status ToGrpcStatus(const fss::Error& error);

//  把 `fss-error-kind` / `fss-error-details` 写到尾随元数据（每个错误出参都要调用）
void AttachErrorMetadata(::grpc::ServerContext& context, const fss::Error& error);

}  // namespace fss::adapters::grpc
