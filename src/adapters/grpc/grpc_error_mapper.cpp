// grpc_error_mapper 实现。映射数据来自 L3 的共享契约表。
#include "adapters/grpc/grpc_error_mapper.h"

#include "domain/contract/error_table.h"

#include <string>

namespace fss::adapters::grpc {

namespace {

constexpr char kKindMetadataKey[] = "fss-error-kind";
constexpr char kDetailsMetadataKey[] = "fss-error-details";

//  ★ 尾随元数据以 `string_ref` 引用外部存储 → 字符串必须活得比 RPC 久。
//    这里用 `ServerContext::AddTrailingMetadata` 的拷贝语义：gRPC 会把值复制进
//    自己的 metadata 容器，因此局部 `std::string` 是安全的（与 `set_trailing_metadata`
//    的裸指针版本不同，后者需要调用方保证生命周期）。
std::string RenderDetails(const fss::Error& error) {
  std::string out;
  for (const auto& [key, value] : error.details()) {
    if (!out.empty()) out += "; ";
    out += key;
    out += "=";
    out += value;
  }
  return out;
}

}  // namespace

int GrpcStatusFor(fss::ErrorKind kind) {
  const auto* row = fss::domain::FindErrorContract(kind);
  //  ★ 找不到 = 新增了 `ErrorKind` 却没登记契约表 → 明确报 INTERNAL，
  //    并且 `test_error_equivalence` 会直接失败（不能静默漂移）。
  if (row == nullptr) return static_cast<int>(::grpc::StatusCode::INTERNAL);
  return row->grpc_status;
}

::grpc::Status ToGrpcStatus(const fss::Error& error) {
  return ::grpc::Status(static_cast<::grpc::StatusCode>(GrpcStatusFor(error.kind())),
                      error.message());
}

void AttachErrorMetadata(::grpc::ServerContext& context, const fss::Error& error) {
  context.AddTrailingMetadata(kKindMetadataKey, std::string(fss::ErrorKindName(error.kind())));
  const std::string details = RenderDetails(error);
  if (!details.empty()) context.AddTrailingMetadata(kDetailsMetadataKey, details);
}

}  // namespace fss::adapters::grpc
