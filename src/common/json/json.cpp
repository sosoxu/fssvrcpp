#include "common/json/json.h"

#include <sstream>

namespace fss::json {
namespace {

// nlohmann 的异常类型 → 结构化 Error（带位置）
Error ParseError(const nlohmann::detail::parse_error& e) {
  Error err(ErrorKind::kInvalidArgument, e.what());
  const auto& pos = e.byte;
  err.With("offset", std::to_string(pos));
  // nlohmann 的 what() 里含 "[json.exception.parse_error.101] parse error at line 2, column 5: ..."
  // 这里把行列解析出来单独放进 details，便于映射层直接使用。
  const std::string what = e.what();
  const auto lp = what.find("line ");
  const auto cp = what.find("column ");
  if (lp != std::string::npos) {
    const auto comma = what.find(',', lp);
    err.With("line", what.substr(lp + 5, (comma == std::string::npos ? what.size() : comma) - lp - 5));
  }
  if (cp != std::string::npos) {
    const auto end = what.find_first_of(",:", cp);
    err.With("column", what.substr(cp + 7, (end == std::string::npos ? what.size() : end) - cp - 7));
  }
  return err;
}

std::string TypeName(const Value& v) {
  if (v.is_null()) return "null";
  if (v.is_string()) return "string";
  if (v.is_number_integer()) return "integer";
  if (v.is_number_unsigned()) return "unsigned";
  if (v.is_number_float()) return "number";
  if (v.is_boolean()) return "boolean";
  if (v.is_object()) return "object";
  if (v.is_array()) return "array";
  return "unknown";
}

Error FieldError(std::string_view key, const char* expected, const Value& actual) {
  Error err(ErrorKind::kInvalidArgument,
            "字段 \"" + std::string(key) + "\" 缺失或类型不符（期望 " + expected +
                "，实际 " + TypeName(actual) + "）");
  err.With("field", std::string(key));
  err.With("expected", expected);
  err.With("actual", TypeName(actual));
  return err;
}

const Value* Lookup(const Value& obj, std::string_view key) {
  if (!obj.is_object()) return nullptr;
  auto it = obj.find(std::string(key));  // nlohmann 的 find 是**大小写敏感**的
  return it == obj.end() ? nullptr : &(*it);
}

}  // namespace

Result<Value> Parse(std::string_view text) {
  try {
    return Value::parse(text.begin(), text.end());
  } catch (const nlohmann::detail::parse_error& e) {
    return ParseError(e);
  } catch (const nlohmann::detail::exception& e) {
    return Err(ErrorKind::kInvalidArgument, std::string("JSON 解析失败: ") + e.what());
  }
}

Result<Value> ParseWithComments(std::string_view text) {
  try {
    // ⚠️ P1-D07：迭代器重载的形参顺序是
    //     parse(first, last, cb, allow_exceptions, ignore_comments)
    //   第 4 个是 allow_exceptions，**不是** ignore_comments。曾把 true 传给第 4 个，
    //   结果"支持注释"从未生效（第 5 个保持默认 false），而全部文档都写着用注释 JSON。
    return Value::parse(text.begin(), text.end(), nullptr,
                        /*allow_exceptions=*/true, /*ignore_comments=*/true);
  } catch (const nlohmann::detail::parse_error& e) {
    return ParseError(e);
  } catch (const nlohmann::detail::exception& e) {
    return Err(ErrorKind::kInvalidArgument, std::string("JSON 解析失败: ") + e.what());
  }
}

Result<Value> ParseObject(std::string_view text) {
  FSS_TRY(v, Parse(text));
  if (!v.is_object()) {
    Error err(ErrorKind::kInvalidArgument,
              std::string("JSON 顶层必须是 object，实际是 ") + TypeName(v));
    err.With("expected", "object").With("actual", TypeName(v));
    return err;
  }
  return v;
}

std::string Dump(const Value& v) { return v.dump(); }
std::string DumpPretty(const Value& v, int indent) { return v.dump(indent); }

bool Has(const Value& obj, std::string_view key) { return Lookup(obj, key) != nullptr; }

Result<std::string> GetString(const Value& obj, std::string_view key) {
  const Value* v = Lookup(obj, key);
  if (v == nullptr || !v->is_string()) return FieldError(key, "string", v ? *v : Value());
  return v->get<std::string>();
}

Result<std::int64_t> GetInt(const Value& obj, std::string_view key) {
  const Value* v = Lookup(obj, key);
  if (v == nullptr || !v->is_number_integer()) return FieldError(key, "integer", v ? *v : Value());
  return v->get<std::int64_t>();
}

Result<bool> GetBool(const Value& obj, std::string_view key) {
  const Value* v = Lookup(obj, key);
  if (v == nullptr || !v->is_boolean()) return FieldError(key, "boolean", v ? *v : Value());
  return v->get<bool>();
}

Result<Value> GetObject(const Value& obj, std::string_view key) {
  const Value* v = Lookup(obj, key);
  if (v == nullptr || !v->is_object()) return FieldError(key, "object", v ? *v : Value());
  return *v;
}

Result<Value> GetArray(const Value& obj, std::string_view key) {
  const Value* v = Lookup(obj, key);
  if (v == nullptr || !v->is_array()) return FieldError(key, "array", v ? *v : Value());
  return *v;
}

Result<std::optional<std::string>> GetOptionalString(const Value& obj, std::string_view key) {
  const Value* v = Lookup(obj, key);
  if (v == nullptr) return std::optional<std::string>{};
  if (!v->is_string()) return FieldError(key, "string", *v);
  return std::optional<std::string>{v->get<std::string>()};
}

Result<std::string> GetStringLenient(const Value& obj, std::string_view key) {
  const Value* v = Lookup(obj, key);
  if (v == nullptr) return FieldError(key, "string 或 number", Value());
  if (v->is_string()) return v->get<std::string>();
  // 显式宽松：OSDU 的 TotalSize/FileSize 声明为 string，但客户端可能传数字
  if (v->is_number_integer()) return std::to_string(v->get<std::int64_t>());
  if (v->is_number_unsigned()) return std::to_string(v->get<std::uint64_t>());
  return FieldError(key, "string 或 number", *v);
}

Result<void> RequireAbsent(const Value& obj, std::string_view key) {
  if (Has(obj, key)) {
    Error err(ErrorKind::kInvalidArgument,
              "字段 \"" + std::string(key) +
                  "\" 不应存在（OSDU 的 data 内字段是 PascalCase；出现即说明大小写用错）");
    err.With("field", std::string(key));
    return err;
  }
  return Ok();
}

}  // namespace fss::json
