// =============================================================================
//  error_table（L3）—— 契约 §5 的**唯一权威表**：领域错误的两条协议投影
// =============================================================================
//  为什么放在 L3（领域层）而不是某个适配层
//    同一个 `ErrorKind` 必须在 REST 与 gRPC 两条链路上落到**同一行**（ADR-001 的
//    双协议等价性）。如果两个适配器各自维护一张 switch，就必然出现"改了一处忘了另一处"
//    —— 那正是 P4 之前的隐患（HTTP 的 `HttpStatusFor` 与 gRPC 尚不存在时无从对照）。
//    表本身是**契约数据**（不含 HTTP/gRPC 类型），放 L3 才能被两个 L5 适配器共享。
//
//  ★ 只放"投影"，不放"错误体格式"：错误体/状态消息的渲染仍留在各适配层
//    （REST 有三种形态，gRPC 有 `details` 尾随元数据）。
//
//  ★ gRPC 状态码用**数值**存放：`grpc::StatusCode` 的数值是 gRPC 的稳定 ABI
//    （OK=0 … UNAUTHENTICATED=16）。L3 不允许 include grpc 头（护栏强制），
//    因此这里存数值 + 名字，由 `tests/conformance/test_error_equivalence.cpp`
//    在**适配层**把它们与 `grpc::StatusCode` 逐个对齐（防止"表里的数字写错"）。
// =============================================================================
#pragma once

#include "common/result/result.h"

#include <span>
#include <string_view>

namespace fss::domain {

struct ErrorContractRow {
  fss::ErrorKind kind;
  int rest_status;                 // HTTP 状态码
  std::string_view rest_reason;    // `reason` 字段（由状态码单点派生）
  int grpc_status;                 // grpc::StatusCode 的数值
  std::string_view grpc_name;      // 对应的 gRPC 枚举名（排障/测试用）
};

//  契约 §5 的全部行（顺序与契约表一致）
std::span<const ErrorContractRow> ErrorContractTable();

//  找不到时返回 nullptr（调用方必须显式处理，而不是猜一个默认值）
const ErrorContractRow* FindErrorContract(fss::ErrorKind kind);

//  REST 状态码 → `reason`（同一状态码的多行必须给出同一个 reason；测试会断言自洽）
std::string_view ReasonForStatus(int status);

}  // namespace fss::domain
