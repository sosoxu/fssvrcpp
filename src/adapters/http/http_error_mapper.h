// =============================================================================
//  http_error_mapper（L5 适配层）—— `ErrorKind` ⇄ HTTP 状态码 / reason / 错误体
// =============================================================================
//  契约依据
//    · §1.6：**三种错误体形态** + 兼容开关 `http.error_format`
//    · §5：`ErrorKind` → REST 状态码 + `reason` 的**唯一权威表**
//    · §1.6 的"上游实测固定消息"必须**原样透出**（客户端按文案断言）
//
//  设计约束
//    · 领域层不携带 HTTP 细节（`Error` 只有 kind/message/details），
//      协议细节**只在这里**翻译 —— 这是"领域不知道 HTTP"的落点。
//    · `reason` 由状态码单点派生，不散落在各 route 里。
//    · 三种形态只有外层包装不同：`code`（或 `status`）与 `message` 必须一致（C4.5）。
#pragma once

#include "common/http/http.h"
#include "common/result/result.h"

#include <string>
#include <string_view>

namespace fss::adapters::http {

enum class ErrorFormat {
  kAppError,   // {"code":400,"reason":"Bad Request","message":"..."}   ← 默认（OSDU 标准）
  kLegacy,     // {"error":{"code":400,"message":"...","errors":[{"reason":"...","message":"..."}]}}
  kApiError,   // {"status":"BAD_REQUEST","message":"...","errors":["..."]}
};

//  解析配置项 `http.error_format`；非法值返回 nullopt（由配置校验报错）
std::optional<ErrorFormat> ParseErrorFormat(std::string_view text);
std::string_view ErrorFormatName(ErrorFormat format);

//  状态码与 reason（唯一权威表，契约 §5）
int HttpStatusFor(fss::ErrorKind kind);
std::string_view ReasonFor(int status);
//  `ApiError` 的 `status` 字段（大写下划线）
std::string_view ApiErrorStatusFor(int status);

//  渲染错误体（不含 HTTP 状态码，由调用方放进 `Response::status`）
std::string RenderErrorBody(fss::ErrorKind kind, std::string_view message, ErrorFormat format);

//  直接产出 `http::Response`（Content-Type: application/json）
fss::http::Response ErrorToResponse(const fss::Error& error, ErrorFormat format);

}  // namespace fss::adapters::http
