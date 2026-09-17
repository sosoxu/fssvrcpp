// =============================================================================
//  fss::json —— JSON 解析/序列化与 OSDU 字段名访问（L1）
// =============================================================================
//
//  为什么需要一个薄封装而不是到处直接写 nlohmann
//  ---------------------------------------------------------------------------
//  1. **解析错误必须带位置**：OSDU 客户端传错 JSON 时要能定位到行列，
//     否则线上排障只能靠猜（`Result` 的 details 里带 line/column/offset）。
//  2. **OSDU 的字段名大小写是契约的一部分**：`data` 内部是 **PascalCase**
//     （`Name`/`TotalSize`/`FileSource`），信封是 camelCase（`id`/`kind`/`acl`）。
//     这里提供显式的大小写敏感访问器，并给出**可读的缺失字段错误**；
//     不允许"大小写不匹配就静默返回空"——那会把契约错误变成数据错误。
//  3. **可替换**：`Value` 是一个别名。将来换 JSON 库只需改本文件；
//     领域层只写 `fss::json::Value`，不写具体库名。
//
//  ★ 为什么允许领域层使用它
//    领域模型对**开放字段**（`ExtensionProperties` 等）必须原样承载，
//    否则上游新增字段会被静默丢弃。这些字段用 `json::Value` 表示是刻意的选择
//    （docs/02-design.md §5.1），不是分层泄漏。
#pragma once

#include "common/result/result.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fss::json {

using Value = nlohmann::json;

// -----------------------------------------------------------------------------
//  解析 / 序列化
// -----------------------------------------------------------------------------
//  失败时 ErrorKind = kInvalidArgument，details 里带 "line" / "column" / "offset"。
//  成功时保证顶层是 **object**（OSDU 的请求体都是对象；数组/标量视为非法输入）。
Result<Value> ParseObject(std::string_view text);

// 任意顶层类型（用于开放字段、数组等）
Result<Value> Parse(std::string_view text);

// 允许 `//` 与 `/* */` 注释的解析 —— **配置文件的格式就是它**。
// 为什么配置用"带注释的 JSON"而不是 YAML：
//   * 零新依赖（复用本模块，已测）；YAML 需要引入 yaml-cpp
//     （镜像 pool 只有 0.5.x 的古老 API，源码构建不划算）
//   * nlohmann 原生支持注释，人写的配置仍然可读、可解释
//   * 与 REST 的 JSON 契约共用同一套解析错误定位（行/列/偏移）
// 注意：仍**不**允许尾随逗号（那不是 JSON）；测试里有专门的负向用例。
Result<Value> ParseWithComments(std::string_view text);

// 紧凑序列化（OSDU 不要求字段顺序，也不要求 pretty）
std::string Dump(const Value& v);
// 便于日志与人工排查
std::string DumpPretty(const Value& v, int indent = 2);

// -----------------------------------------------------------------------------
//  大小写敏感的字段访问（OSDU 契约要求）
// -----------------------------------------------------------------------------
bool Has(const Value& obj, std::string_view key);

// 取值失败时：
//   * kind = kInvalidArgument
//   * message 形如 `字段 "Name" 缺失或类型不符（期望 string，实际 null）`
//   * details 带 "field" 与 "expected"
Result<std::string> GetString(const Value& obj, std::string_view key);
Result<std::int64_t> GetInt(const Value& obj, std::string_view key);
Result<bool> GetBool(const Value& obj, std::string_view key);
Result<Value> GetObject(const Value& obj, std::string_view key);
Result<Value> GetArray(const Value& obj, std::string_view key);

// 可选字段：缺失时返回 std::nullopt；**存在但类型错误**时返回 Error
Result<std::optional<std::string>> GetOptionalString(const Value& obj, std::string_view key);

// 宽松取字符串：OSDU 里有些"数字"字段声明为 string（如 TotalSize/FileSize），
// 但客户端有时传数字。这里做**显式**的宽松转换，并记录它是宽松的。
Result<std::string> GetStringLenient(const Value& obj, std::string_view key);

// -----------------------------------------------------------------------------
//  契约护栏：大小写误用必须失败（不是"返回空值"）
// -----------------------------------------------------------------------------
//  用法（测试与 DTO 层）：断言某个 camelCase 名字**不应该**存在于 OSDU data 里。
//  这防止"用 j.at("name") 兜底"这种把契约错误变成静默数据错误的写法。
Result<void> RequireAbsent(const Value& obj, std::string_view key);

}  // namespace fss::json
