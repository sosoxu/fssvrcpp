// =============================================================================
//  test_error_equivalence.cpp —— C7.2：契约 §5 的**错误码双向映射无空缺**
// =============================================================================
//  判据：每个 `ErrorKind` 的 REST `(status, reason)` 与 gRPC `StatusCode` 落在**同一行**。
//
//  ★ 这个测试的"权威"来自**契约**，不是实现里的表：
//      · 下面 `kContractRows` 是**逐个手写**的契约 §5 期望值（与实现里的
//        `domain::ErrorContractTable()` 相互独立）；
//      · 测试同时检查实现表与两个适配器（HTTP / gRPC）—— 任一处写错都会失败。
//    否则"表与测试一起错"是查不出来的（R18 的同类教训）。
// =============================================================================
#include <catch2/catch.hpp>

#include "adapters/grpc/grpc_error_mapper.h"
#include "adapters/http/http_error_mapper.h"
#include "common/result/result.h"
#include "domain/contract/error_table.h"

#include <grpcpp/grpcpp.h>

#include <set>
#include <string>
#include <vector>

namespace {

using fss::ErrorKind;

struct ExpectedRow {
  ErrorKind kind;
  int rest_status;
  const char* rest_reason;
  ::grpc::StatusCode grpc_status;
};

//  ★ 手抄自 docs/03-api-contract.md §5（改动契约必须先改这里）
const std::vector<ExpectedRow> kContractRows = {
    {ErrorKind::kInvalidArgument, 400, "Bad Request", ::grpc::StatusCode::INVALID_ARGUMENT},
    {ErrorKind::kFileSourceEmpty, 400, "Bad Request", ::grpc::StatusCode::INVALID_ARGUMENT},
    {ErrorKind::kInvalidSourcePath, 400, "Bad Request", ::grpc::StatusCode::INVALID_ARGUMENT},
    {ErrorKind::kLocationAlreadyExists, 400, "Bad Request", ::grpc::StatusCode::ALREADY_EXISTS},
    {ErrorKind::kChecksumMismatch, 400, "Bad Request", ::grpc::StatusCode::INVALID_ARGUMENT},
    {ErrorKind::kUnauthenticated, 401, "Unauthorized", ::grpc::StatusCode::UNAUTHENTICATED},
    {ErrorKind::kPermissionDenied, 403, "Forbidden", ::grpc::StatusCode::PERMISSION_DENIED},
    {ErrorKind::kStorageAccessDenied, 403, "Forbidden", ::grpc::StatusCode::PERMISSION_DENIED},
    {ErrorKind::kNotFound, 404, "Not Found", ::grpc::StatusCode::NOT_FOUND},
    {ErrorKind::kUnimplemented, 501, "Not Implemented", ::grpc::StatusCode::UNIMPLEMENTED},
    {ErrorKind::kInternal, 500, "Internal Server Error", ::grpc::StatusCode::INTERNAL},
    {ErrorKind::kBadGateway, 502, "Bad Gateway", ::grpc::StatusCode::UNAVAILABLE},
    {ErrorKind::kUnavailable, 503, "Service Unavailable", ::grpc::StatusCode::UNAVAILABLE},
    //  ⚠️ `kOk` 不在契约 §5 的"错误"表里，但它是 `ErrorKind` 的合法取值：
    //    被误当错误使用时，REST 给 500、gRPC 给 INTERNAL（表示"实现有 bug"）。
    {ErrorKind::kOk, 500, "Internal Server Error", ::grpc::StatusCode::INTERNAL},
};

}  // namespace

TEST_CASE("★ C7.2 契约 §5 的每一行：REST 状态/reason 与 gRPC 状态码必须同行",
          "[phase7][conformance][c7.2]") {
  for (const auto& expected : kContractRows) {
    const std::string name(fss::ErrorKindName(expected.kind));
    INFO("ErrorKind = " << name);

    //  ① 实现里的共享表必须与契约逐字段一致
    const auto* row = fss::domain::FindErrorContract(expected.kind);
    REQUIRE(row != nullptr);
    REQUIRE(row->rest_status == expected.rest_status);
    REQUIRE(row->rest_reason == expected.rest_reason);
    REQUIRE(row->grpc_status == static_cast<int>(expected.grpc_status));

    //  ② REST 适配层的投影（状态码 + reason）
    REQUIRE(fss::adapters::http::HttpStatusFor(expected.kind) == expected.rest_status);
    REQUIRE(fss::adapters::http::ReasonFor(expected.rest_status) == expected.rest_reason);

    //  ③ gRPC 适配层的投影：数值必须等于 `grpc::StatusCode` 的枚举值
    //     （表里存的是数值以保持 L3 无 grpc 依赖 —— 这里把它钉死）
    REQUIRE(fss::adapters::grpc::GrpcStatusFor(expected.kind) ==
            static_cast<int>(expected.grpc_status));

    //  ④ 由 `Error` 走一遍：消息与 details 必须带到 gRPC status / 尾随元数据里
    fss::Error error(expected.kind, "契约测试消息 " + name);
    error.With("expected", "aa").With("actual", "bb");
    const auto status = fss::adapters::grpc::ToGrpcStatus(error);
    REQUIRE(status.error_code() == expected.grpc_status);
    REQUIRE(std::string(status.error_message()) == error.message());
  }
}

TEST_CASE("★ C7.2 无空缺：`ErrorKind` 的每个取值都在表里（枚举新增必须同步登记）",
          "[phase7][conformance][c7.2]") {
  //  ★ 这里是"防漏"的关键：如果新增了一个 `ErrorKind` 却忘了登记契约表，
  //    `FindErrorContract` 会返回 nullptr，本用例立刻失败。
  for (int raw = static_cast<int>(ErrorKind::kOk);
       raw <= static_cast<int>(ErrorKind::kUnavailable); ++raw) {
    const auto kind = static_cast<ErrorKind>(raw);
    INFO("ErrorKind = " << fss::ErrorKindName(kind));
    REQUIRE(fss::domain::FindErrorContract(kind) != nullptr);
    //  表里的行数必须等于枚举取值数（多一行 = 有非法枚举值）
    REQUIRE(fss::adapters::grpc::GrpcStatusFor(kind) !=
            static_cast<int>(::grpc::StatusCode::OK));  // 错误一律不是 OK
  }
  REQUIRE(fss::domain::ErrorContractTable().size() == kContractRows.size());

  //  自洽：同一 REST 状态码的所有行必须给出同一个 reason（否则 reason 会漂移）
  for (const auto& row : fss::domain::ErrorContractTable()) {
    INFO("ErrorKind = " << fss::ErrorKindName(row.kind));
    REQUIRE(fss::domain::ReasonForStatus(row.rest_status) == row.rest_reason);
    REQUIRE(fss::adapters::http::ReasonFor(row.rest_status) == row.rest_reason);
  }

  //  两个协议的状态码集合必须与契约一致（没有"多出来的"状态码）
  std::set<int> rest_statuses;
  std::set<int> grpc_statuses;
  for (const auto& row : fss::domain::ErrorContractTable()) {
    rest_statuses.insert(row.rest_status);
    grpc_statuses.insert(row.grpc_status);
  }
  REQUIRE(rest_statuses == std::set<int>{400, 401, 403, 404, 500, 501, 502, 503});
  REQUIRE(grpc_statuses ==
          std::set<int>{static_cast<int>(::grpc::StatusCode::INVALID_ARGUMENT),
                        static_cast<int>(::grpc::StatusCode::ALREADY_EXISTS),
                        static_cast<int>(::grpc::StatusCode::UNAUTHENTICATED),
                        static_cast<int>(::grpc::StatusCode::PERMISSION_DENIED),
                        static_cast<int>(::grpc::StatusCode::NOT_FOUND),
                        static_cast<int>(::grpc::StatusCode::UNIMPLEMENTED),
                        static_cast<int>(::grpc::StatusCode::INTERNAL),
                        static_cast<int>(::grpc::StatusCode::UNAVAILABLE)});
}

TEST_CASE("★ C7.2 契约表里没有的 `ErrorKind`（越界值）→ gRPC 明确 INTERNAL，不静默 OK",
          "[phase7][conformance][c7.2]") {
  //  非法枚举值（例如从网络反序列化出来的垃圾）：必须报 INTERNAL，
  //  绝不能返回 OK —— 那会让客户端把失败当成功（P1-D08 的同类教训）。
  const auto bogus = static_cast<ErrorKind>(999);
  REQUIRE(fss::domain::FindErrorContract(bogus) == nullptr);
  REQUIRE(fss::adapters::grpc::GrpcStatusFor(bogus) ==
          static_cast<int>(::grpc::StatusCode::INTERNAL));
  REQUIRE(fss::adapters::http::HttpStatusFor(bogus) == 500);
}
