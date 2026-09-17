// =============================================================================
//  fss::config —— 分层配置加载与"一次性全量校验"（L1）
// =============================================================================
//
//  优先级（高 → 低）：命令行 > 环境变量 > 配置文件 > 内置默认值
//
//  ★ C1.5 的核心判据：**非法配置 → 拒绝启动，并一次性列出全部问题**
//    这条不是"好看的错误信息"，而是运维现实：如果只报第一个问题，
//    运维就要"改一个、重启、再发现一个"地迭代十几次。因此：
//      * `Load()` 收集**所有**问题后才返回
//      * 问题带 path / message / source（file|env|cli），便于直接定位
//
//  配置格式：**带注释的 JSON**（`json::ParseWithComments`），由 `config/fss.example.json` 示范。
//  （本仓库不引入 YAML 解析器 —— 见 AGENTS.md 的硬性约定）
//
//  环境变量映射：`server.http.port` → `FSS_SERVER_HTTP_PORT`
//  `${ENV:VAR}` 引用：字符串值里的具名环境变量引用；无法解析 → 计为问题
//                     （除非该字段有默认值，此时使用默认值）
#pragma once

#include "common/json/json.h"
#include "common/result/result.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fss::config {

enum class ValueType { kString, kInt, kBool, kStringList };

struct FieldSpec {
  std::string path;              // "server.http.port"
  ValueType type = ValueType::kString;
  bool required = false;
  bool has_default = false;
  std::string default_value;     // 统一以字符串存储，按 type 解析
  long min_value = 0;            // 仅 kInt；max_value < min_value 表示不校验范围
  long max_value = -1;
  bool secret = false;           // 日志/诊断输出中打码
  std::vector<std::string> enum_values;  // 非空则必须命中其一
  std::string description;

  // 链式构造，便于可读地声明字段
  FieldSpec& Str(std::string d = "") { type = ValueType::kString; description = std::move(d); return *this; }
  FieldSpec& Int(long lo, long hi, std::string d = "") {
    type = ValueType::kInt; min_value = lo; max_value = hi; description = std::move(d); return *this;
  }
  FieldSpec& Bool(std::string d = "") { type = ValueType::kBool; description = std::move(d); return *this; }
  FieldSpec& List(std::string d = "") { type = ValueType::kStringList; description = std::move(d); return *this; }
  FieldSpec& Required() { required = true; return *this; }
  FieldSpec& Default(std::string v) { has_default = true; default_value = std::move(v); return *this; }
  FieldSpec& Secret() { secret = true; return *this; }
  FieldSpec& Enum(std::vector<std::string> vs) { enum_values = std::move(vs); return *this; }
};

// 跨字段规则（例如 ADR-009 的 "multi 模式下仓储必须是 postgres"）。
// 输入是**已通过单字段校验**的原始 JSON，输出若干问题。
using CrossCheck = std::function<std::vector<std::pair<std::string, std::string>>(const json::Value&)>;

class Schema {
 public:
  Schema& Add(FieldSpec spec);
  // 动态子树：其下的键不参与"未知键"检查（例如 partition.file.<租户名>.*）
  Schema& AllowDynamicPrefix(std::string prefix);
  Schema& AddCrossCheck(CrossCheck check, std::string name = "");

  const FieldSpec* Find(std::string_view path) const;
  bool IsAllowedPath(std::string_view path) const;
  const std::vector<FieldSpec>& fields() const { return fields_; }
  const std::vector<std::string>& dynamic_prefixes() const { return dynamic_prefixes_; }
  const std::vector<std::pair<std::string, CrossCheck>>& cross_checks() const { return cross_checks_; }

 private:
  std::vector<FieldSpec> fields_;
  std::map<std::string, std::size_t, std::less<>> index_;
  std::vector<std::string> dynamic_prefixes_;
  std::vector<std::pair<std::string, CrossCheck>> cross_checks_;
};

// P1 阶段的核心字段集（后续阶段按需扩展；扩展时同步 config/fss.example.json）
Schema CoreSchema();

struct Problem {
  std::string path;     // 配置路径；文件级错误用 "<file>"
  std::string message;  // 人类可读
  std::string source;   // file | env | cli | cross | schema
  std::string ToString() const;
};

struct LoadRequest {
  Schema schema;
  std::string file_path;                       // 空 = 不读文件
  bool env_enabled = true;
  std::string env_prefix = "FSS_";
  // 命令行覆盖：{"server.http.port", "9090"}
  std::vector<std::pair<std::string, std::string>> cli_overrides;
  // 由调用方注入的环境变量读取（便于测试；默认读真实环境）
  std::function<std::optional<std::string>(std::string_view)> env_lookup;
};

// 前置声明：让 Load() 成为 Config 的友元（它需要写内部状态）
struct LoadResult;
LoadResult Load(const LoadRequest& req);

class Config {
 public:
  friend LoadResult Load(const LoadRequest&);
  bool ok() const { return problems_.empty(); }
  const std::vector<Problem>& problems() const { return problems_; }

  bool Has(std::string_view path) const;
  Result<std::string> GetString(std::string_view path) const;
  Result<long> GetInt(std::string_view path) const;
  Result<bool> GetBool(std::string_view path) const;
  Result<std::vector<std::string>> GetStringList(std::string_view path) const;

  // 供日志与 /v2/info：secret 字段打码；**绝不**输出明文密钥
  const json::Value& effective() const { return effective_; }
  std::string RedactedDump() const;
  // 哪些字段来自哪一层（便于排障："这个值到底是哪来的"）
  std::string SourceOf(std::string_view path) const;

  // 一行式问题汇总（启动失败时打印这个）
  std::string ProblemsToString() const;

 private:
  json::Value effective_ = json::Value::object();
  std::map<std::string, std::string, std::less<>> sources_;  // path -> file|env|cli|default
  std::vector<Problem> problems_;
  const Schema* schema_ = nullptr;
};

struct LoadResult {
  bool ok = false;
  Config config;
};

}  // namespace fss::config
