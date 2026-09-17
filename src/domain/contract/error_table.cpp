// error_table 实现。逐行对应 docs/03-api-contract.md §5 的权威表。
#include "domain/contract/error_table.h"

#include <array>

namespace fss::domain {

namespace {

//  gRPC `StatusCode` 的数值（与 grpc/status.h 一致；测试会在适配层逐个对齐）
constexpr int kOk = 0;
constexpr int kInvalidArgument = 3;
constexpr int kNotFound = 5;
constexpr int kAlreadyExists = 6;
constexpr int kPermissionDenied = 7;
constexpr int kUnimplemented = 12;
constexpr int kInternal = 13;
constexpr int kUnavailable = 14;
constexpr int kUnauthenticated = 16;

constexpr std::array<ErrorContractRow, 14> kTable = {{
    //  ★ kOk 不是"错误"，但它是 `ErrorKind` 的合法取值：把它误当错误使用时，
    //    REST 给 500、gRPC 给 INTERNAL（都表示"实现有 bug"，不是客户端的问题）。
    {fss::ErrorKind::kOk, 500, "Internal Server Error", kInternal, "INTERNAL"},
    {fss::ErrorKind::kInvalidArgument, 400, "Bad Request", kInvalidArgument, "INVALID_ARGUMENT"},
    {fss::ErrorKind::kFileSourceEmpty, 400, "Bad Request", kInvalidArgument, "INVALID_ARGUMENT"},
    {fss::ErrorKind::kInvalidSourcePath, 400, "Bad Request", kInvalidArgument, "INVALID_ARGUMENT"},
    //  ★ 上游把"fileID 已存在"映射成 **400**（不是 409）；gRPC 侧仍是 ALREADY_EXISTS
    {fss::ErrorKind::kLocationAlreadyExists, 400, "Bad Request", kAlreadyExists, "ALREADY_EXISTS"},
    {fss::ErrorKind::kChecksumMismatch, 400, "Bad Request", kInvalidArgument, "INVALID_ARGUMENT"},
    {fss::ErrorKind::kUnauthenticated, 401, "Unauthorized", kUnauthenticated, "UNAUTHENTICATED"},
    {fss::ErrorKind::kPermissionDenied, 403, "Forbidden", kPermissionDenied, "PERMISSION_DENIED"},
    //  存储侧拒绝本服务：对客户端同样是 403（与"你没权限"区分在错误体/元数据里）
    {fss::ErrorKind::kStorageAccessDenied, 403, "Forbidden", kPermissionDenied,
     "PERMISSION_DENIED"},
    {fss::ErrorKind::kNotFound, 404, "Not Found", kNotFound, "NOT_FOUND"},
    {fss::ErrorKind::kUnimplemented, 501, "Not Implemented", kUnimplemented, "UNIMPLEMENTED"},
    {fss::ErrorKind::kInternal, 500, "Internal Server Error", kInternal, "INTERNAL"},
    {fss::ErrorKind::kBadGateway, 502, "Bad Gateway", kUnavailable, "UNAVAILABLE"},
    {fss::ErrorKind::kUnavailable, 503, "Service Unavailable", kUnavailable, "UNAVAILABLE"},
}};

}  // namespace

std::span<const ErrorContractRow> ErrorContractTable() { return kTable; }

const ErrorContractRow* FindErrorContract(fss::ErrorKind kind) {
  for (const auto& row : kTable) {
    if (row.kind == kind) return &row;
  }
  return nullptr;
}

std::string_view ReasonForStatus(int status) {
  for (const auto& row : kTable) {
    if (row.rest_status == status) return row.rest_reason;
  }
  return "Unknown";
}

}  // namespace fss::domain
