// http_error_mapper 实现。映射表依据见头文件。
#include "adapters/http/http_error_mapper.h"

#include "common/json/json.h"

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
  switch (kind) {
    // 400：校验、非法 expiryTime、FileSource 缺失/不存在、kind 非法、fileID 已存在、校验和不符
    case fss::ErrorKind::kInvalidArgument:
    case fss::ErrorKind::kFileSourceEmpty:
    case fss::ErrorKind::kInvalidSourcePath:
    case fss::ErrorKind::kLocationAlreadyExists:
    case fss::ErrorKind::kChecksumMismatch:
      return 400;
    // 401：缺 token / 缺 partition / token 无效
    case fss::ErrorKind::kUnauthenticated:
      return 401;
    // 403：调用方角色不足 / 存储侧拒绝（两者对客户端都是 403，但错误体里的 kind 不同）
    case fss::ErrorKind::kPermissionDenied:
    case fss::ErrorKind::kStorageAccessDenied:
      return 403;
    // 404：记录或位置不存在
    case fss::ErrorKind::kNotFound:
      return 404;
    // 501：扩展端点未启用
    case fss::ErrorKind::kUnimplemented:
      return 501;
    // 502：依赖服务异常
    case fss::ErrorKind::kBadGateway:
      return 502;
    // 503：存储后端不可用/过载
    case fss::ErrorKind::kUnavailable:
      return 503;
    // 500：未预期异常（含 kOk 被误当错误使用的情况）
    case fss::ErrorKind::kInternal:
    case fss::ErrorKind::kOk:
      return 500;
  }
  return 500;
}

std::string_view ReasonFor(int status) {
  switch (status) {
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
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
