// =============================================================================
//  net（L1）：URL 解析/构造、百分号编码、查询串编解码
// =============================================================================
//
//  为什么需要它
//    · 对外：OSDU 的 `downloadURL` 是**我们生成的绝对 URL**，签名字符串（SigV4）要求
//      canonical URI/query 与最终发出的 URL **逐字节一致** —— 编码规则错了，签名就对不上，
//      而且症状是"远端 403"，排查代价极高。
//    · 对内：`/v2/getFileList` 的 `expiryTime`、`pageNum`、`pageSize` 等 query 参数需要
//      严格解析（多一个空格、多一个 `+` 都可能让客户端与我们的理解不一致）。
//
//  ⚠️ 编码规则只认 RFC 3986（unreserved = ALPHA / DIGIT / `-` `.` `_` `~`）。
//     **不要把 `+` 当成空格去编码**：`+` 是 `application/x-www-form-urlencoded` 的约定，
//     不是 URL 的约定。本模块编码时一律用 `%20`；只在**解析查询串**时按
//     form 约定把 `+` 视作空格（可关闭），因为客户端确实会这么发。
#pragma once

#include "common/result/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fss::net {

// -----------------------------------------------------------------------------
//  百分号编码 / 解码
// -----------------------------------------------------------------------------
//  严格模式：只保留 RFC 3986 的 unreserved 字符，其余全部 `%XX`（大写十六进制）。
std::string PercentEncode(std::string_view text);

//  路径段模式：额外保留 pchar 中允许出现在路径段里的子分隔符（`: @ ! $ & ' ( ) * + , ; =`）。
//  `/` **不在**其中 —— 它是分隔符，出现在段里必须被编码（否则段会被劈成两段）。
std::string PercentEncodePathSegment(std::string_view text);

//  解码：非法的 `%` 序列（`%zz`、`%2`、末尾 `%`）返回 kInvalidArgument，**不做**"尽力而为"。
//  理由：静默替换坏序列会让"我们以为解出来的值"与"客户端实际发的值"不一致 ——
//  在签名/租户隔离这类场景下，这种不一致就是安全缺陷。
Result<std::string> PercentDecode(std::string_view text, bool plus_as_space = false);

// -----------------------------------------------------------------------------
//  查询串
// -----------------------------------------------------------------------------
//  保序 + 允许重复键（`?a=1&a=2` 是合法的；用 map 会丢信息）
using QueryParams = std::vector<std::pair<std::string, std::string>>;

//  解析 `a=1&b=2`（**不带**前导 `?`）。宽容点：
//    · `a`（无 `=`）→ {"a", ""}
//    · `a=`        → {"a", ""}
//    · `=1`        → {"", "1"}（保留，交给调用方判非法键）
//    · 空串        → 空结果
//  默认把 `+` 当空格（form 约定，客户端常这么发）
Result<QueryParams> ParseQuery(std::string_view query, bool plus_as_space = true);

//  构造：键与值都做严格百分号编码，用 `&` 连接（不做 `?` 前缀）
std::string BuildQuery(const QueryParams& params);

//  取值：找到第一个匹配的键；找不到返回 nullopt。**不做**大小写折叠
//  （OSDU 的参数名是驼峰的，折叠会掩盖拼写错误）
std::optional<std::string> GetQueryParam(const QueryParams& params, std::string_view key);
std::vector<std::string> GetAllQueryParams(const QueryParams& params, std::string_view key);
//  整型参数（`expiryTime` / `pageNum` / `pageSize`）：必须整串是数字，否则报错
Result<std::int64_t> GetIntQueryParam(const QueryParams& params, std::string_view key);

// -----------------------------------------------------------------------------
//  URL
// -----------------------------------------------------------------------------
struct Url {
  std::string scheme;      // 小写，无 `:`
  std::string userinfo;    // 可空（不含 `@`）
  std::string host;        // 小写；IPv6 不含方括号
  std::uint16_t port = 0;  // 有显式端口时为其值，否则 0
  bool has_port = false;
  std::string path;      // 至少为 "/"（解析成功的绝对 URL 一定带路径）
  std::string query;     // 不含 `?`
  std::string fragment;  // 不含 `#`
  bool has_fragment = false;

  // 显式端口，或 scheme 的默认端口（http=80 / https=443）；未知 scheme 返回 0
  std::uint16_t EffectivePort() const;
  // `host:port`（IPv6 带方括号；默认端口且未显式给出时不带端口）
  std::string Authority() const;
  std::string PathAndQuery() const;
  std::string ToString(bool with_fragment = true) const;
  bool IsSecure() const { return scheme == "https"; }
};

//  只支持 http / https（本服务不会去别的 scheme）。
//  失败原因分级：缺 scheme / 非法端口 / 缺 host / 未知 scheme / 非法百分号编码。
Result<Url> ParseUrl(std::string_view absolute_url);

//  在 base 之后追加一个**段**（做路径段编码，避免 `/` 注入）：
//    AppendPathSegment("https://h/api/file", "a/b") → "https://h/api/file/a%2Fb"
std::string AppendPathSegment(std::string_view base_url, std::string_view segment);

//  IPv6 字面量加方括号（`::1` → `[::1]`）；已是 `[..]` 则原样返回
std::string BracketIfIpv6(std::string_view host);

}  // namespace fss::net
