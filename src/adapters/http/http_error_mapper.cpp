// http_error_mapper 实现。映射表依据见头文件。
#include "adapters/http/http_error_mapper.h"

#include "common/json/json.h"
#include "domain/contract/error_table.h"

#include <optional>

namespace fss::adapters::http {

std::optional<ErrorFormat> ParseErrorFormat(std::string_view text) {
  if (text == "apperror") return ErrorFormat::kAppError;
  if (text == "legacy") return ErrorFormat::kLegacy;
  if (text == "api_error") return ErrorFormat::kApiError;
  return std::nullopt;
}

std::string_view ErrorFormatName(ErrorFormat format) {
  switch (format) {
    case ErrorFormat::kAppError: return "apperror";
    case ErrorFormat::kLegacy: return "legacy";
    case ErrorFormat::kApiError: return "api_error";
  }
  return "apperror";
}

int HttpStatusFor(fss::ErrorKind kind) {
  //  ★ 从**共享的契约表**派生（`domain::ErrorContractTable()`）：HTTP 与 gRPC 两条链路
  //    必须落在同一行，因此映射数据只有一份（见 domain/contract/error_table.h）。
  //    找不到 = 枚举加了新值却没登记 → 明确报错，而不是猜一个 500（否则契约会静默漂移）。
  const auto* row = fss::domain::FindErrorContract(kind);
  if (row == nullptr) return 500;
  return row->rest_status;
}

std::string_view ReasonFor(int status) {
  //  ① 领域错误覆盖的状态码 → 取自**共享契约表**（单一真相，与 gRPC 同行）
  const auto from_contract = fss::domain::ReasonForStatus(status);
  if (from_contract != "Unknown") return from_contract;
  //  ② 协议层自己产生的状态码（不在契约 §5 的错误表里）：HTTP 语义专属
  switch (status) {
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 416: return "Range Not Satisfiable";
    default: return "Error";
  }
}

std::string_view ApiErrorStatusFor(int status) {
  switch (status) {
    case 400: return "BAD_REQUEST";
    case 401: return "UNAUTHORIZED";
    case 403: return "FORBIDDEN";
    case 404: return "NOT_FOUND";
    case 413: return "PAYLOAD_TOO_LARGE";
    case 416: return "RANGE_NOT_SATISFIABLE";
    case 500: return "INTERNAL_SERVER_ERROR";
    case 501: return "NOT_IMPLEMENTED";
    case 502: return "BAD_GATEWAY";
    case 503: return "SERVICE_UNAVAILABLE";
    default: return "ERROR";
  }
}

std::string RenderErrorBody(fss::ErrorKind kind, std::string_view message, ErrorFormat format) {
  const int status = HttpStatusFor(kind);
  const std::string reason(ReasonFor(status));
  const std::string text(message);

  //  ⚠️ 三种形态只有外层包装不同：`code`（或 `status`）与 `message` 必须一致（C4.5）
  switch (format) {
    case ErrorFormat::kAppError: {
      json::Value body = json::Value::object();
      body["code"] = status;
      body["reason"] = reason;
      body["message"] = text;
      return json::Dump(body);
    }
    case ErrorFormat::kLegacy: {
      json::Value inner = json::Value::object();
      inner["code"] = status;
      inner["message"] = text;
      json::Value entry = json::Value::object();
      entry["reason"] = reason;
      entry["message"] = text;
      inner["errors"] = json::Value::array({entry});
      json::Value body = json::Value::object();
      body["error"] = inner;
      return json::Dump(body);
    }
    case ErrorFormat::kApiError: {
      json::Value body = json::Value::object();
      body["status"] = std::string(ApiErrorStatusFor(status));
      body["message"] = text;
      body["errors"] = json::Value::array({text});
      return json::Dump(body);
    }
  }
  return "{}";
}

fss::http::Response ErrorToResponse(const fss::Error& error, ErrorFormat format) {
  const int status = HttpStatusFor(error.kind());
  fss::http::Response response = fss::http::Response::Json(status, RenderErrorBody(error.kind(),
                                                                        error.message(), format));
  //  便于客户端与排障：把机器可读的分类放进头（不在错误体里，避免破坏契约字段集）
  response.headers.Set("X-FSS-Error-Kind", std::string(fss::ErrorKindName(error.kind())));
  return response;
}

}  // namespace fss::adapters::http
