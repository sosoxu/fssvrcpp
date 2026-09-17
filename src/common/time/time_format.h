// =============================================================================
//  时间格式化与解析（L1）
// =============================================================================
//  本服务需要三种格式（都在契约里出现过）：
//    ① epoch 秒 / 毫秒            —— 内部统一表示（int64）
//    ② ISO-8601 UTC（`...Z`）      —— `PreloadFileCreateDate` 等
//    ③ `yyyy-MM-dd'T'HH:mm:ss.SSSZ`（**`Z` 位置是 `+0000` 风格的偏移**）
//                                    —— `getFileList` 的 `CreatedAt`（契约 §2.5）
//    ④ HTTP 日期（RFC 1123）        —— 响应头（Last-Modified / Date）
//
//  ★ 契约 §2.5 的 `CreatedAt` 形如 `2021-03-03T15:13:33.120+0000`。
//    注意最后的 `+0000`：用 C 的 strftime 时是 `%z`，**不是** `Z`。
//    写错会让对齐上游的客户端解析失败，因此有专门的往返测试。
#pragma once

#include "common/result/result.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace fss::time {

// ② ISO-8601 UTC，毫秒精度，带 'Z'：2021-03-03T15:13:33.120Z
std::string ToIso8601Utc(std::int64_t epoch_seconds, int millis = 0);
// 解析 ISO-8601（接受 'Z' 或 ±HH:MM / ±HHMM 偏移）
Result<std::int64_t> ParseIso8601(std::string_view text);

// ③ OSDU 的 CreatedAt 格式：2021-03-03T15:13:33.120+0000
std::string ToOsduTimestamp(std::int64_t epoch_seconds, int millis = 0);
Result<std::int64_t> ParseOsduTimestamp(std::string_view text);

// ④ HTTP 日期：Wed, 03 Mar 2021 15:13:33 GMT
std::string ToRfc1123(std::int64_t epoch_seconds);

}  // namespace fss::time
