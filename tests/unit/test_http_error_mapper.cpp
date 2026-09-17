// =============================================================================
//  C4.5：`ErrorKind` → (HTTP 状态码, reason) 的唯一权威表 + 三种错误体形态
// =============================================================================
//  契约 §5 是"唯一权威表"；契约 §1.6 规定三种形态**只有外层包装不同**：
//  同一请求的 `code`（或 `status`）与 `message` 必须一致。
//
//  本测试刻意把"映射表"写成**显式清单**并断言清单长度（13），这样新增一个
//  `ErrorKind` 而忘了更新映射时，两处会同时提醒（与 `test_error_kind_coverage` 呼应）。
// =============================================================================
#include <catch2/catch.hpp>

#include "adapters/http/http_error_mapper.h"
#include "common/json/json.h"

#include <string>
#include <utility>
#include <vector>

using fss::ErrorKind;
using fss::adapters::http::ErrorFormat;
using fss::adapters::http::ErrorFormatName;
using fss::adapters::http::HttpStatusFor;
using fss::adapters::http::ParseErrorFormat;
using fss::adapters::http::ReasonFor;
using fss::adapters::http::RenderErrorBody;

namespace {

//  契约 §5 的权威表（除 kOk 外的全部错误取值）
const std::vector<std::pair<ErrorKind, int>>& ExpectedTable() {
  static const std::vector<std::pair<ErrorKind, int>> table = {
      {ErrorKind::kInvalidArgument, 400},      {ErrorKind::kFileSourceEmpty, 400},
      {ErrorKind::kInvalidSourcePath, 400},    {ErrorKind::kLocationAlreadyExists, 400},
      {ErrorKind::kChecksumMismatch, 400},     {ErrorKind::kUnauthenticated, 401},
      {ErrorKind::kPermissionDenied, 403},     {ErrorKind::kStorageAccessDenied, 403},
      {ErrorKind::kNotFound, 404},
      {ErrorKind::kUnimplemented, 501},        {ErrorKind::kInternal, 500},
      {ErrorKind::kBadGateway, 502},           {ErrorKind::kUnavailable, 503},
  };
  return table;
}

}  // namespace

TEST_CASE("★ C4.5 ErrorKind → 状态码 + reason（覆盖全部 13 个错误取值）",
          "[phase4][error][c4.5]") {
  //  清单长度固定：新增 ErrorKind 必须同时更新这里与映射实现
  REQUIRE(ExpectedTable().size() == 13);

  for (const auto& [kind, status] : ExpectedTable()) {
    INFO("ErrorKind=" << fss::ErrorKindName(kind));
    REQUIRE(HttpStatusFor(kind) == status);
    REQUIRE_FALSE(ReasonFor(status).empty());
  }

  //  reason 逐条比对（客户端可能按文案断言）
  REQUIRE(ReasonFor(400) == "Bad Request");
  REQUIRE(ReasonFor(401) == "Unauthorized");
  REQUIRE(ReasonFor(403) == "Forbidden");
  REQUIRE(ReasonFor(404) == "Not Found");
  REQUIRE(ReasonFor(500) == "Internal Server Error");
  REQUIRE(ReasonFor(501) == "Not Implemented");
  REQUIRE(ReasonFor(502) == "Bad Gateway");
  REQUIRE(ReasonFor(503) == "Service Unavailable");
}

TEST_CASE("★ C4.5 三种错误体：code/message 一致，只有外层包装不同", "[phase4][error][c4.5]") {
  const std::string message = "FileSource can not be empty";

  SECTION("apperror（默认，OSDU 标准）") {
    const auto body = fss::json::ParseObject(
        RenderErrorBody(ErrorKind::kFileSourceEmpty, message, ErrorFormat::kAppError));
    REQUIRE(body.ok());
    REQUIRE(body.value()["code"] == 400);
    REQUIRE(body.value()["reason"] == "Bad Request");
    REQUIRE(body.value()["message"] == message);
  }

  SECTION("legacy（ErrorResponse）") {
    const auto body = fss::json::ParseObject(
        RenderErrorBody(ErrorKind::kFileSourceEmpty, message, ErrorFormat::kLegacy));
    REQUIRE(body.ok());
    REQUIRE(body.value()["error"]["code"] == 400);
    REQUIRE(body.value()["error"]["message"] == message);
    REQUIRE(body.value()["error"]["errors"][0]["reason"] == "Bad Request");
    REQUIRE(body.value()["error"]["errors"][0]["message"] == message);
  }

  SECTION("api_error（ApiError）") {
    const auto body = fss::json::ParseObject(
        RenderErrorBody(ErrorKind::kFileSourceEmpty, message, ErrorFormat::kApiError));
    REQUIRE(body.ok());
    REQUIRE(body.value()["status"] == "BAD_REQUEST");
    REQUIRE(body.value()["message"] == message);
    REQUIRE(body.value()["errors"][0] == message);
  }
}

TEST_CASE("★ C4.5 契约 §1.6 的上游固定消息必须原样透出（逐字节）",
          "[phase4][error][c4.5]") {
  const std::vector<std::string> fixed_messages = {
      "FileSource can not be empty",
      "Invalid source file path to copy from /x/y",
      "Record Not Found",
      "Not found location for fileID : f-1",
      "Location for fileID = f-1 already exists",
      "Invalid kind",
      "Invalid source in kind",
      "Invalid entity in kind",
      "Missing authorization token",
      "Missing partitionID",
      "ConstraintViolationException: Invalid FileLocationRequest",
      "expiryTime pattern isn't supported. Value should be one of these regex patterns "
      "^[0-9]+M$ , ^[0-9]+H$ , ^[0-9]+D$",
  };
  REQUIRE(fixed_messages.size() == 12);

  for (const auto& message : fixed_messages) {
    for (const auto format :
         {ErrorFormat::kAppError, ErrorFormat::kLegacy, ErrorFormat::kApiError}) {
      const auto body =
          fss::json::ParseObject(RenderErrorBody(ErrorKind::kInvalidArgument, message, format));
      REQUIRE(body.ok());
      //  三种形态里 message 都必须是**原样**（没有任何转义/包装改动）
      const std::string rendered = [&] {
        switch (format) {
          case ErrorFormat::kAppError: return body.value()["message"].get<std::string>();
          case ErrorFormat::kLegacy:
            return body.value()["error"]["message"].get<std::string>();
          case ErrorFormat::kApiError: return body.value()["message"].get<std::string>();
        }
        return std::string();
      }();
      INFO("format=" << ErrorFormatName(format));
      REQUIRE(rendered == message);
    }
  }
}

TEST_CASE("C4.5 错误体形态配置解析（非法值必须被拒）", "[phase4][error][c4.5]") {
  REQUIRE(ParseErrorFormat("apperror").value() == ErrorFormat::kAppError);
  REQUIRE(ParseErrorFormat("legacy").value() == ErrorFormat::kLegacy);
  REQUIRE(ParseErrorFormat("api_error").value() == ErrorFormat::kApiError);
  //  非法值不静默折叠成默认（否则配置写错会"看起来正常"）
  REQUIRE_FALSE(ParseErrorFormat("").has_value());
  REQUIRE_FALSE(ParseErrorFormat("APPERROR").has_value());
  REQUIRE_FALSE(ParseErrorFormat("app_error").has_value());

  REQUIRE(ErrorFormatName(ErrorFormat::kAppError) == "apperror");
  REQUIRE(ErrorFormatName(ErrorFormat::kLegacy) == "legacy");
  REQUIRE(ErrorFormatName(ErrorFormat::kApiError) == "api_error");
}

TEST_CASE("C4.5 ErrorToResponse：状态码 / JSON Content-Type / 分类头", "[phase4][error][c4.5]") {
  const fss::Error error = fss::Err(ErrorKind::kNotFound, "Record Not Found");
  const auto response = fss::adapters::http::ErrorToResponse(error, ErrorFormat::kAppError);
  REQUIRE(response.status == 404);
  const auto content_type = response.headers.Get("Content-Type");
  REQUIRE(content_type.has_value());
  REQUIRE(content_type->rfind("application/json", 0) == 0);
  const auto kind = response.headers.Get("X-FSS-Error-Kind");
  REQUIRE(kind.has_value());
  REQUIRE(*kind == "kNotFound");
}
