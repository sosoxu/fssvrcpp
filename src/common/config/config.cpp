#include "common/config/config.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace fss::config {
namespace {

std::string EnvNameFor(std::string_view path, const std::string& prefix) {
  std::string out(prefix);
  for (char c : path) {
    if (c == '.' || c == '-') out.push_back('_');
    else out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

// 把 json 的标量/数组渲染成"统一字符串形式"，便于按 FieldSpec.type 二次解析
std::string RenderScalar(const json::Value& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
  if (v.is_number_float()) {
    std::ostringstream os; os << v.get<double>(); return os.str();
  }
  return {};  // 对象由调用方处理
}

bool ParseBool(std::string_view s, bool* out) {
  std::string v(s);
  std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (v == "true" || v == "1" || v == "yes" || v == "on") { *out = true; return true; }
  if (v == "false" || v == "0" || v == "no" || v == "off") { *out = false; return true; }
  return false;
}

void SetAt(json::Value& root, const std::string& path, const json::Value& value) {
  json::Value* cur = &root;
  std::size_t start = 0;
  while (true) {
    const auto dot = path.find('.', start);
    const std::string seg = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    if (dot == std::string::npos) { (*cur)[seg] = value; return; }
    if (!cur->contains(seg) || !(*cur)[seg].is_object()) (*cur)[seg] = json::Value::object();
    cur = &(*cur)[seg];
    start = dot + 1;
  }
}

const json::Value* GetAt(const json::Value& root, std::string_view path) {
  const json::Value* cur = &root;
  std::size_t start = 0;
  const std::string p(path);
  while (true) {
    if (!cur->is_object()) return nullptr;
    const auto dot = p.find('.', start);
    const std::string seg = p.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    auto it = cur->find(seg);
    if (it == cur->end()) return nullptr;
    cur = &(*it);
    if (dot == std::string::npos) return cur;
    start = dot + 1;
  }
}

// ${ENV:VAR} 展开。返回 false 表示引用了无法解析的环境变量。
bool ExpandEnvRefs(std::string& s, const LoadRequest& req, std::string* missing_var) {
  std::size_t pos = 0;
  bool all_ok = true;
  while ((pos = s.find("${ENV:", pos)) != std::string::npos) {
    const auto close = s.find('}', pos);
    if (close == std::string::npos) break;
    const std::string var = s.substr(pos + 6, close - pos - 6);
    std::string value;
    bool found = false;
    if (req.env_lookup) {
      if (auto v = req.env_lookup(var)) { value = *v; found = true; }
    } else if (const char* v = std::getenv(var.c_str())) {
      value = v; found = true;
    }
    if (!found) { all_ok = false; if (missing_var) *missing_var = var; pos = close + 1; continue; }
    s.replace(pos, close - pos + 1, value);
    pos += value.size();
  }
  return all_ok;
}

// 递归收集所有叶子路径
void CollectPaths(const json::Value& v, const std::string& prefix, std::vector<std::string>* out) {
  if (!v.is_object()) { if (!prefix.empty()) out->push_back(prefix); return; }
  if (v.empty() && !prefix.empty()) { out->push_back(prefix); return; }
  for (auto it = v.begin(); it != v.end(); ++it) {
    CollectPaths(it.value(), prefix.empty() ? it.key() : prefix + "." + it.key(), out);
  }
}

}  // namespace

// -----------------------------------------------------------------------------
Schema& Schema::Add(FieldSpec spec) {
  index_[spec.path] = fields_.size();
  fields_.push_back(std::move(spec));
  return *this;
}
Schema& Schema::AllowDynamicPrefix(std::string prefix) {
  dynamic_prefixes_.push_back(std::move(prefix));
  return *this;
}
Schema& Schema::AddCrossCheck(CrossCheck check, std::string name) {
  cross_checks_.emplace_back(std::move(name), std::move(check));
  return *this;
}
const FieldSpec* Schema::Find(std::string_view path) const {
  auto it = index_.find(path);
  return it == index_.end() ? nullptr : &fields_[it->second];
}
bool Schema::IsAllowedPath(std::string_view path) const {
  if (Find(path) != nullptr) return true;
  for (const auto& p : dynamic_prefixes_) {
    if (path.size() >= p.size() && path.compare(0, p.size(), p) == 0 &&
        (path.size() == p.size() || path[p.size()] == '.')) {
      return true;
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
std::string Problem::ToString() const {
  std::ostringstream os;
  os << "[" << source << "] " << path << ": " << message;
  return os.str();
}

bool Config::Has(std::string_view path) const { return GetAt(effective_, path) != nullptr; }

Result<std::string> Config::GetString(std::string_view path) const {
  const json::Value* v = GetAt(effective_, path);
  if (v == nullptr) return Err(ErrorKind::kNotFound, "配置项不存在: " + std::string(path));
  if (!v->is_string()) return Err(ErrorKind::kInvalidArgument, "配置项不是字符串: " + std::string(path));
  return v->get<std::string>();
}
Result<long> Config::GetInt(std::string_view path) const {
  const json::Value* v = GetAt(effective_, path);
  if (v == nullptr) return Err(ErrorKind::kNotFound, "配置项不存在: " + std::string(path));
  if (!v->is_number_integer()) return Err(ErrorKind::kInvalidArgument, "配置项不是整数: " + std::string(path));
  return v->get<long>();
}
Result<bool> Config::GetBool(std::string_view path) const {
  const json::Value* v = GetAt(effective_, path);
  if (v == nullptr) return Err(ErrorKind::kNotFound, "配置项不存在: " + std::string(path));
  if (!v->is_boolean()) return Err(ErrorKind::kInvalidArgument, "配置项不是布尔: " + std::string(path));
  return v->get<bool>();
}
Result<std::vector<std::string>> Config::GetStringList(std::string_view path) const {
  const json::Value* v = GetAt(effective_, path);
  if (v == nullptr) return Err(ErrorKind::kNotFound, "配置项不存在: " + std::string(path));
  if (!v->is_array()) return Err(ErrorKind::kInvalidArgument, "配置项不是数组: " + std::string(path));
  std::vector<std::string> out;
  for (const auto& e : *v) out.push_back(e.is_string() ? e.get<std::string>() : RenderScalar(e));
  return out;
}

std::string Config::SourceOf(std::string_view path) const {
  auto it = sources_.find(path);
  return it == sources_.end() ? "unknown" : it->second;
}

std::string Config::ProblemsToString() const {
  std::ostringstream os;
  os << "配置校验失败，共 " << problems_.size() << " 个问题：\n";
  for (std::size_t i = 0; i < problems_.size(); ++i) {
    os << "  " << (i + 1) << ") " << problems_[i].ToString() << "\n";
  }
  return os.str();
}

std::string Config::RedactedDump() const {
  json::Value copy = effective_;
  // 按 schema 里的 secret 标记打码
  if (schema_ != nullptr) {
    for (const auto& f : schema_->fields()) {
      if (!f.secret) continue;
      // 逐段定位，存在才替换
      json::Value* cur = &copy;
      std::size_t start = 0;
      bool ok = true;
      while (true) {
        if (!cur->is_object()) { ok = false; break; }
        const auto dot = f.path.find('.', start);
        const std::string seg = f.path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        auto it = cur->find(seg);
        if (it == cur->end()) { ok = false; break; }
        if (dot == std::string::npos) { *it = "***"; break; }
        cur = &(*it);
        start = dot + 1;
      }
      (void)ok;
    }
  }
  return json::DumpPretty(copy);
}

// -----------------------------------------------------------------------------
LoadResult Load(const LoadRequest& req) {
  Config cfg;
  cfg.schema_ = &req.schema;

  // ---------- 第 1 层：文件 ----------
  json::Value from_file = json::Value::object();
  if (!req.file_path.empty()) {
    std::ifstream in(req.file_path, std::ios::binary);
    if (!in) {
      cfg.problems_.push_back({"<file>", "无法打开配置文件: " + req.file_path, "file"});
    } else {
      std::ostringstream ss; ss << in.rdbuf();
      auto parsed = json::ParseWithComments(ss.str());
      if (!parsed.ok()) {
        cfg.problems_.push_back({"<file>", "配置解析失败: " + parsed.error().ToString(), "file"});
      } else if (!parsed.value().is_object()) {
        cfg.problems_.push_back({"<file>", "配置顶层必须是 JSON object", "file"});
      } else {
        from_file = parsed.value();
      }
    }
  }

  // 文件里的每个叶子：未知键 / 环境引用展开 / 类型与范围
  std::vector<std::string> file_paths;
  CollectPaths(from_file, "", &file_paths);
  for (const auto& p : file_paths) {
    if (!req.schema.IsAllowedPath(p)) {
      cfg.problems_.push_back({p, "未知配置项（可能是拼写错误；若为动态子树请在 Schema 里声明 AllowDynamicPrefix）", "schema"});
    }
  }
  json::Value merged = from_file;  // 后续层直接覆盖

  // ---------- 第 2 层：环境变量 ----------
  if (req.env_enabled) {
    for (const auto& f : req.schema.fields()) {
      const std::string name = EnvNameFor(f.path, req.env_prefix);
      std::optional<std::string> v;
      if (req.env_lookup) v = req.env_lookup(name);
      else if (const char* e = std::getenv(name.c_str())) v = std::string(e);
      if (v) { SetAt(merged, f.path, json::Value(*v)); cfg.sources_[f.path] = "env"; }
    }
  }

  // ---------- 第 3 层：命令行 ----------
  for (const auto& [path, value] : req.cli_overrides) {
    if (!req.schema.IsAllowedPath(path)) {
      cfg.problems_.push_back({path, "命令行覆盖了未知配置项", "cli"});
      continue;
    }
    SetAt(merged, path, json::Value(value));
    cfg.sources_[path] = "cli";
  }

  // ---------- 默认值 + 逐字段校验 ----------
  for (const auto& f : req.schema.fields()) {
    const json::Value* raw = GetAt(merged, f.path);
    const bool from_layer = raw != nullptr;

    if (!from_layer) {
      if (f.has_default) {
        // 默认值也要走类型解析，保证"默认值本身是合法的"
        merged;  // no-op，仅为可读性
        cfg.sources_[f.path] = "default";
        raw = nullptr;
      } else if (f.required) {
        cfg.problems_.push_back({f.path, "必填项缺失", "file"});
        continue;
      } else {
        continue;
      }
    }

    std::string text;
    std::vector<std::string> list_value;
    if (!from_layer) {
      text = f.default_value;
      if (f.type == ValueType::kStringList) {
        std::stringstream ss(text);
        std::string item;
        while (std::getline(ss, item, ',')) {
          if (!item.empty()) list_value.push_back(item);
        }
        text.clear();
      }
    } else if (f.type == ValueType::kStringList) {
      if (raw->is_array()) {
        for (const auto& e : *raw) list_value.push_back(RenderScalar(e));
        text.clear();
      } else if (raw->is_string()) {
        // 环境变量/命令行覆盖数组：逗号分隔
        std::string s = raw->get<std::string>();
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, ',')) {
          if (!item.empty()) list_value.push_back(item);
        }
        text.clear();
      } else {
        cfg.problems_.push_back({f.path, "类型错误：期望字符串数组", cfg.sources_.count(f.path) ? cfg.sources_[f.path] : "file"});
        continue;
      }
    } else if (raw->is_object()) {
      cfg.problems_.push_back({f.path, "类型错误：期望标量，实际是 object", cfg.sources_.count(f.path) ? cfg.sources_[f.path] : "file"});
      continue;
    } else {
      text = RenderScalar(*raw);
    }

    const std::string src = cfg.sources_.count(f.path) ? cfg.sources_[f.path] : "file";

    // ${ENV:VAR} 展开（仅字符串）
    if (f.type == ValueType::kString) {
      std::string missing;
      if (!ExpandEnvRefs(text, req, &missing)) {
        if (f.has_default) {
          text = f.default_value;
          cfg.problems_.push_back({f.path, "环境变量 " + missing + " 未设置，已回退默认值（请确认这是预期）", "file"});
        } else {
          cfg.problems_.push_back({f.path, "引用的环境变量 " + missing + " 未设置", "file"});
          continue;
        }
      }
    }

    // 类型/范围/枚举
    json::Value final_value;
    switch (f.type) {
      case ValueType::kString:
        final_value = text;
        break;
      case ValueType::kInt: {
        char* end = nullptr;
        const long n = std::strtol(text.c_str(), &end, 10);
        if (end == text.c_str() || *end != '\0') {
          cfg.problems_.push_back({f.path, "类型错误：期望整数，实际 '" + text + "'", src});
          continue;
        }
        if (f.max_value >= f.min_value && (n < f.min_value || n > f.max_value)) {
          cfg.problems_.push_back({f.path, "超出范围 [" + std::to_string(f.min_value) + ", " +
                                               std::to_string(f.max_value) + "]，实际 " + text, src});
          continue;
        }
        final_value = n;
        break;
      }
      case ValueType::kBool: {
        bool b = false;
        if (!ParseBool(text, &b)) {
          cfg.problems_.push_back({f.path, "类型错误：期望布尔值（true/false），实际 '" + text + "'", src});
          continue;
        }
        final_value = b;
        break;
      }
      case ValueType::kStringList:
        final_value = list_value;
        break;
    }

    // 枚举
    if (!f.enum_values.empty()) {
      const std::string as_str = final_value.is_string() ? final_value.get<std::string>() : RenderScalar(final_value);
      if (std::find(f.enum_values.begin(), f.enum_values.end(), as_str) == f.enum_values.end()) {
        std::string allowed;
        for (std::size_t i = 0; i < f.enum_values.size(); ++i) {
          if (i) allowed += " | ";
          allowed += f.enum_values[i];
        }
        cfg.problems_.push_back({f.path, "取值非法：'" + as_str + "'，允许 " + allowed, src});
        continue;
      }
    }

    SetAt(cfg.effective_, f.path, final_value);
    cfg.sources_[f.path] = src;
  }

  // ---------- 跨字段规则 ----------
  for (const auto& [name, check] : req.schema.cross_checks()) {
    for (auto& [path, msg] : check(cfg.effective_)) {
      cfg.problems_.push_back({path, (name.empty() ? "" : "[" + name + "] ") + msg, "cross"});
    }
  }

  LoadResult out;
  out.ok = cfg.problems_.empty();
  out.config = std::move(cfg);
  return out;
}

}  // namespace fss::config
