// =============================================================================
//  server_main（组合根）—— 配置 → 依赖构造 → 路由注册 → 启动监听
// =============================================================================
//  ★ R12：**所有具体实现只在这里被创建**。领域层/应用层/适配层都不 `new` 具体类型；
//     端口的实现由本文件注入。`scripts/verify_composition_root.sh`（P4 后续切片）
//     会机械检查这一点。
//
//  ---- 阶段 10 切片 1：配置面接线（C10.1~C10.8）----
//  配置来源与优先级（高 → 低）：
//      ① 命令行 `--set <json.path>=<值>`（可重复）
//      ② 环境变量**通用名**：`FSS_` + 路径大写、`.`→`_`（例：`FSS_SERVER_HTTP_PORT`）
//      ③ 环境变量**旧别名**（deprecated，但现有脚本/镜像在用）：
//         `FSS_HTTP_PORT` / `FSS_STORAGE_ROOT` / `FSS_POSIX_DURABILITY` / …（见 LegacyAliases()）
//      ④ 配置文件 `--config <path>`（等价 `FSS_CONFIG`；**带注释的 JSON**）
//      ⑤ 组合根历史默认值（与 `CoreSchema()` 默认值不同的键逐个登记在 LegacyDefaults）
//  失败语义：配置非法 → `ProblemsToString()` 全量打印 → **exit 78（EX_CONFIG）**。
//  来源可见：启动横幅逐键打印来源缩写（cli/env/file/default），`--print-config` 打印
//            脱敏后的**完整**有效配置 + 每项来源。
//
//  ⚠️ 组合根**不重写**加载器：一律走 `fss::config::Load`（类型/范围/枚举/未知键/
//     跨字段校验都是它的职责）。本文件只做"优先级决策 + 读到对象上"。
#include "adapters/grpc/file_service_adapter.h"
#include "adapters/grpc/grpc_server.h"
#include "adapters/http/router.h"
#include "app/services/location_issuer.h"
#include "app/usecases/usecases.h"
#include "common/config/config.h"
#include "common/http/http.h"
#include "common/ids/id_generator.h"
#include "common/json/json.h"
#include "common/logging/logging.h"
#include "common/metrics/metrics.h"
#include "common/result/result.h"
#include "common/sys/capability.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/auth/local/local_jwt_authorizer.h"
#include "infra/auth/remote/remote_entitlements_authorizer.h"
#include "infra/blob/metered/metered_blob_store.h"
#include "infra/blob/posix/posix_blob_store.h"
#include "infra/blob/s3/s3_blob_store.h"
#include "infra/io/uring_io_engine.h"
#include "infra/location/sqlite/sqlite_location_repository.h"
#include "infra/io/file_sync.h"
#include "infra/metadata/sqlite/sqlite_metadata_repository.h"
#include "infra/transfer/blob_byte_source.h"
#include "infra/transfer/transfer_endpoint.h"
#include "infra/transfer/transfer_token.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

//  EX_CONFIG（sysexits.h）：配置/部署形态非法 → 拒绝启动。写进 docs/operations.md。
constexpr int kExitConfigError = 78;

const char* const kUsage =
    "用法: fss_server [选项]\n"
    "  --config <path>          加载带注释的 JSON 配置文件（等价环境变量 FSS_CONFIG）\n"
    "  --set <json.path>=<值>   覆盖单个配置项，可重复；优先级最高\n"
    "  --print-config           打印脱敏后的有效配置与每项来源，随后退出（退出码 0）\n"
    "  --help, -h               打印本用法\n"
    "优先级: --set(cli) > 环境变量(通用名 FSS_<PATH>) > 旧环境变量别名 > 配置文件 > 默认值\n"
    "配置非法时的退出码: 78 (EX_CONFIG)\n";

std::string Env(const char* key, const std::string& fallback) {
  if (const char* value = std::getenv(key); value != nullptr && *value != '\0') return value;
  return fallback;
}

//  schema 默认的脱敏键清单（与 config/fss.example.json 的 `observability.redact_keys`
//  逐项一致）。组合根在没有配置来源时用它，保证"接线前日志就打码"这一行为不变。
constexpr const char* kDefaultRedactKeys =
    "secret_key,access_key,token,sig,signature,authorization,x-amz-signature,password,"
    "signing_key,dsn,static_token";

//  与加载器 `config.cpp::EnvNameFor` 同规则：`storage.posix.root` → `FSS_STORAGE_POSIX_ROOT`。
//  用于判断某个旧别名是不是恰好就是该键的通用名（那样就不算"两个来源同时存在"）。
std::string GenericEnvName(const std::string& path, std::string_view prefix = "FSS_") {
  std::string out(prefix);
  for (const char c : path) {
    if (c == '.' || c == '-') {
      out.push_back('_');
    } else {
      out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
  }
  return out;
}

// =============================================================================
//  命令行解析（C10.1）
// =============================================================================
struct CliOptions {
  std::string config_path;
  bool config_from_cli = false;
  std::vector<std::pair<std::string, std::string>> set_overrides;
  bool print_config = false;
  bool help = false;
};

bool ParseSetPair(const std::string& text, std::pair<std::string, std::string>* out,
                  std::string* error) {
  const auto eq = text.find('=');
  if (eq == std::string::npos || eq == 0) {
    *error = "--set 的形态必须是 <json.path>=<值>，实际收到：" + text;
    return false;
  }
  out->first = text.substr(0, eq);
  out->second = text.substr(eq + 1);
  return true;
}

bool ParseCli(int argc, char** argv, CliOptions* out, std::string* error) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config") {
      if (i + 1 >= argc) {
        *error = "--config 需要一个路径参数";
        return false;
      }
      out->config_path = argv[++i];
      out->config_from_cli = true;
    } else if (arg.rfind("--config=", 0) == 0) {
      out->config_path = arg.substr(std::strlen("--config="));
      out->config_from_cli = true;
    } else if (arg == "--set") {
      if (i + 1 >= argc) {
        *error = "--set 需要 <json.path>=<值>";
        return false;
      }
      std::pair<std::string, std::string> kv;
      if (!ParseSetPair(argv[++i], &kv, error)) return false;
      out->set_overrides.push_back(std::move(kv));
    } else if (arg.rfind("--set=", 0) == 0) {
      std::pair<std::string, std::string> kv;
      if (!ParseSetPair(arg.substr(std::strlen("--set=")), &kv, error)) return false;
      out->set_overrides.push_back(std::move(kv));
    } else if (arg == "--print-config") {
      out->print_config = true;
    } else if (arg == "--help" || arg == "-h") {
      out->help = true;
    } else {
      *error = "未知参数：" + arg;
      return false;
    }
  }
  if (out->config_from_cli && out->config_path.empty()) {
    *error = "--config 的路径不能为空";
    return false;
  }
  return true;
}

// =============================================================================
//  旧环境变量别名（deprecated 但必须继续可用；C10.3 的"兼容既有名字"）
// =============================================================================
//  ★ 为什么不让 `Load()` 直接吃这些名字：其中三个的名字/取值形态超出 schema 的
//    约束（`FSS_POSIX_DURABILITY=never` 被 enum 排除、`FSS_POSIX_FSYNC_THRESHOLD_BYTES`
//    的 schema 范围是 [0,0]、`FSS_GRPC_PORT=-1`/`0` 低于端口下界），强行注入会让
//    **现有脚本启动失败** —— 这正是"不破坏既有行为"要防的事。因此在 Load 之后按
//    优先级解析（env > file），并在 `--print-config` 里如实展示来源是哪个变量。
enum class LegacyType { kString, kInt, kBool };

struct LegacyAlias {
  const char* var;    // 旧环境变量名
  const char* path;   // 配置路径
  LegacyType type;
  long min_value;     // 仅 kInt
  long max_value;     // 仅 kInt
  std::vector<std::string> enum_values;  // 非空 → 必须命中其一
};

const std::vector<LegacyAlias>& LegacyAliases() {
  static const std::vector<LegacyAlias> kAliases = {
      // ---- 存储 / 元数据 ----
      {"FSS_STORAGE_ROOT", "storage.posix.root", LegacyType::kString, 0, 0, {}},
      {"FSS_SQLITE_PATH", "location.sqlite.path", LegacyType::kString, 0, 0, {}},
      {"FSS_METADATA_SQLITE_PATH", "metadata.sqlite.path", LegacyType::kString, 0, 0, {}},
      {"FSS_POSIX_DURABILITY", "storage.posix.durability", LegacyType::kString, 0, 0,
       {"per_file", "batch", "never"}},
      {"FSS_POSIX_FSYNC_THRESHOLD_BYTES", "storage.posix.fsync_threshold_bytes",
       LegacyType::kInt, 0, 1L << 40, {}},
      // ---- 服务面 ----
      {"FSS_HTTP_PORT", "server.http.port", LegacyType::kInt, 0, 65535, {}},
      {"FSS_GRPC_PORT", "server.grpc.port", LegacyType::kInt, -1, 65535, {}},
      {"FSS_BIND_ADDRESS", "server.http.bind", LegacyType::kString, 0, 0, {}},
      {"FSS_BIND_ADDRESS", "server.grpc.bind", LegacyType::kString, 0, 0, {}},
      {"FSS_SELF_BASE_URL", "self_signed.public_base_url", LegacyType::kString, 0, 0, {}},
      // ---- 密钥（`FSS_TRANSFER_SECRET` 是文档化的旧名；`FSS_TRANSFER_SIGNING_KEY`
      //      是示例文件里 `${ENV:...}` 的内嵌名，两者都继续支持，前者优先）----
      {"FSS_TRANSFER_SECRET", "self_signed.signing_key", LegacyType::kString, 0, 0, {}},
      {"FSS_TRANSFER_SIGNING_KEY", "self_signed.signing_key", LegacyType::kString, 0, 0, {}},
      {"FSS_S3_ACCESS_KEY", "storage.s3.access_key", LegacyType::kString, 0, 0, {}},
      {"FSS_S3_SECRET_KEY", "storage.s3.secret_key", LegacyType::kString, 0, 0, {}},
      // ---- 部署 / 认证 ----
      {"FSS_MAX_CLOCK_SKEW_SECONDS", "deployment.max_clock_skew_seconds", LegacyType::kInt, 0,
       300, {}},
      {"FSS_JWT_HMAC_SECRET", "auth.jwt.hmac_secret", LegacyType::kString, 0, 0, {}},
      {"FSS_JWT_ISSUER", "auth.jwt.issuer", LegacyType::kString, 0, 0, {}},
      {"FSS_JWT_AUDIENCE", "auth.jwt.audience", LegacyType::kString, 0, 0, {}},
      {"FSS_JWT_VERIFY_SIGNATURE", "auth.jwt.verify_signature", LegacyType::kBool, 0, 0, {}},
      {"FSS_JWT_USER_ID_CLAIM", "auth.jwt.user_id_claim", LegacyType::kString, 0, 0, {}},
      {"FSS_JWT_PARTITION_CLAIM", "auth.jwt.partition_claim", LegacyType::kString, 0, 0, {}},
      {"FSS_JWT_REQUIRE_PARTITION_CLAIM", "auth.jwt.require_partition_claim",
       LegacyType::kBool, 0, 0, {}},
      {"FSS_ENTITLEMENTS_URL", "auth.remote_entitlements.base_url", LegacyType::kString, 0, 0, {}},
      {"FSS_ENTITLEMENTS_AUTHORIZE_PATH", "auth.remote_entitlements.authorize_path",
       LegacyType::kString, 0, 0, {}},
      {"FSS_ENTITLEMENTS_TIMEOUT_MS", "auth.remote_entitlements.timeout_ms", LegacyType::kInt,
       1, 600000, {}},
      // ---- 可观测性 ----
      {"FSS_LOG_LEVEL", "observability.log_level", LegacyType::kString, 0, 0,
       {"debug", "info", "warn", "error"}},
      {"FSS_LOG_FORMAT", "observability.log_format", LegacyType::kString, 0, 0, {"json", "text"}},
      {"FSS_LOG_REDACT_KEYS", "observability.redact_keys", LegacyType::kString, 0, 0, {}},
  };
  return kAliases;
}

// =============================================================================
//  JSON 取值小工具（只读 `Config::effective()`）
// =============================================================================
std::string RenderScalar(const fss::json::Value& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
  if (v.is_number_float()) return v.dump();
  return {};
}

const fss::json::Value* FindIn(const fss::json::Value& root, const std::string& path) {
  const fss::json::Value* cur = &root;
  std::size_t start = 0;
  while (true) {
    if (!cur->is_object()) return nullptr;
    const auto dot = path.find('.', start);
    const std::string seg =
        path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    auto it = cur->find(seg);
    if (it == cur->end()) return nullptr;
    cur = &(*it);
    if (dot == std::string::npos) return cur;
    start = dot + 1;
  }
}

std::vector<std::string> SplitCsv(const std::string& text) {
  std::vector<std::string> out;
  std::stringstream ss(text);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) out.push_back(item);
  }
  return out;
}

std::string Join(const std::vector<std::string>& items, std::string_view sep = ",") {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i) out.append(sep);
    out.append(items[i]);
  }
  return out;
}

// =============================================================================
//  配置解析器：把"优先级 + 来源 + 旧别名 + 历史默认值"集中在一个地方
// =============================================================================
//  读法（高 → 低）：cli(--set) > env(通用名) > env(旧别名) > file > 组合根历史默认值。
//  `Load()` 已经保证 cli/env/file 三层与类型校验；本类只补"旧别名"和"历史默认值"。
class Resolver {
 public:
  struct Row {
    std::string path;
    std::string value;
    std::string source;     // cli | env | file | default
    std::string alias_var;  // 非空 = 该值来自旧环境变量（deprecated）
    bool secret = false;
  };

  Resolver(const fss::config::Config& cfg, const fss::config::Schema& schema)
      : cfg_(cfg), schema_(schema) {
    for (const auto& alias : LegacyAliases()) {
      const char* raw = std::getenv(alias.var);
      if (raw == nullptr || *raw == '\0') continue;
      ValidateLegacy(alias, raw);
      auto it = hits_.find(alias.path);
      if (it == hits_.end()) {
        hits_.emplace(alias.path, LegacyHit{alias.var, raw});
      } else if (it->second.var != alias.var && it->second.value != raw) {
        conflicts_.push_back(std::string(alias.path) + "：旧环境变量 " + it->second.var +
                             " 与 " + alias.var + " 同时存在且取值不同，采用 " + it->second.var);
      }
    }
  }

  const std::vector<fss::config::Problem>& problems() const { return problems_; }
  const std::map<std::string, Row>& rows() const { return rows_; }

  // 通用名与旧别名同时给出时，**不让两者同时生效**：按优先级取通用名，并把歧义显式告诉运维。
  void WarnConflicts(std::ostream& os) const {
    for (const auto& [path, hit] : hits_) {
      //  `metadata.sqlite.path` 这类键的**通用名**与旧名恰好同字（`FSS_METADATA_SQLITE_PATH`）：
      //  那不是歧义，同一个变量而已 —— 不要刷无意义的告警。
      if (hit.var == GenericEnvName(path)) continue;
      const std::string src = cfg_.SourceOf(path);
      if (src == "cli" || src == "env") {
        os << "警告：配置项 " << path << " 同时由更高优先级来源（" << src << "）与旧环境变量 "
           << hit.var << "（deprecated）给出；按优先级采用 " << src << "。\n";
      }
    }
    for (const auto& text : conflicts_) os << "警告：" << text << "\n";
  }

  std::string Str(const std::string& path, const std::string& legacy_default) {
    const Origin o = OriginOf(path, legacy_default);
    const std::string value = o.from_cfg ? RenderCfg(path) : o.raw;
    Record(path, value, o);
    return value;
  }

  long Int(const std::string& path, long legacy_default) {
    const Origin o = OriginOf(path, std::to_string(legacy_default));
    const std::string text = o.from_cfg ? RenderCfg(path) : o.raw;
    Record(path, text, o);
    char* end = nullptr;
    const long n = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
      problems_.push_back({path, "不是整数：" + text, o.source});
      return legacy_default;
    }
    return n;
  }

  bool Bool(const std::string& path, bool legacy_default) {
    const Origin o = OriginOf(path, legacy_default ? "true" : "false");
    const std::string text = o.from_cfg ? RenderCfg(path) : o.raw;
    Record(path, text, o);
    // 旧环境变量的布尔语义与接线前完全一致：只有字面 `false` 才为假（`0`/`no` 仍为真）。
    if (!o.alias_var.empty()) return text != "false";
    return text == "true";
  }

  std::vector<std::string> List(const std::string& path, const std::string& legacy_default_csv) {
    const Origin o = OriginOf(path, legacy_default_csv);
    std::vector<std::string> out;
    if (o.from_cfg) {
      auto value = cfg_.GetStringList(path);
      if (value.ok()) out = value.value();
    } else {
      out = SplitCsv(o.raw);
    }
    Record(path, Join(out), o);
    return out;
  }

  //  `--print-config`：按 schema 顺序打印**全部**键 + 每项来源（secret 一律 `***`）。
  void Print(std::ostream& os) const {
    os << "== 有效配置（脱敏）+ 每项来源 ==\n";
    os << "# 来源: cli(--set) > env > file > default；`别名 FSS_XXX` = 该值来自旧环境变量\n";
    os << "# secret 字段一律打印 ***\n";
    for (const auto& field : schema_.fields()) {
      Row computed;
      const Row* row = nullptr;
      auto it = rows_.find(field.path);
      if (it != rows_.end()) {
        row = &it->second;
      } else {
        const Origin o = OriginOf(field.path, field.default_value);
        computed = Row{field.path, o.from_cfg ? RenderCfg(field.path) : o.raw, o.source, o.alias_var,
                       field.secret};
        row = &computed;
      }
      os << field.path << " = " << (row->secret ? std::string("***") : row->value) << " ["
         << row->source;
      if (!row->alias_var.empty()) os << " 别名 " << row->alias_var;
      os << "]\n";
    }
  }

 private:
  struct LegacyHit {
    std::string var;
    std::string value;
  };
  struct Origin {
    bool from_cfg = false;
    std::string raw;       // from_cfg=false 时的标量值
    std::string source;    // cli | env | file | default
    std::string alias_var; // 非空 = 旧环境变量
  };

  void ValidateLegacy(const LegacyAlias& alias, const char* raw) {
    const std::string value = raw;
    const std::string var = alias.var;
    if (alias.type == LegacyType::kInt) {
      char* end = nullptr;
      const long n = std::strtol(value.c_str(), &end, 10);
      if (end == value.c_str() || *end != '\0') {
        problems_.push_back({alias.path, "旧环境变量 " + var + " 不是整数：" + value, "env"});
        return;
      }
      if (n < alias.min_value || n > alias.max_value) {
        problems_.push_back({alias.path, "旧环境变量 " + var + " 超出范围 [" +
                                             std::to_string(alias.min_value) + ", " +
                                             std::to_string(alias.max_value) + "]：" + value,
                             "env"});
        return;
      }
    }
    if (!alias.enum_values.empty() &&
        std::find(alias.enum_values.begin(), alias.enum_values.end(), value) ==
            alias.enum_values.end()) {
      problems_.push_back({alias.path,
                           "旧环境变量 " + var + " 取值非法：" + value + "，允许 " +
                               Join(alias.enum_values, " | "),
                           "env"});
    }
  }

  Origin OriginOf(const std::string& path, const std::string& legacy_default) const {
    const std::string src = cfg_.SourceOf(path);
    if (src == "cli" || src == "env") return Origin{true, {}, src, {}};
    auto it = hits_.find(path);
    if (it != hits_.end()) return Origin{false, it->second.value, "env", it->second.var};
    if (src == "file") return Origin{true, {}, "file", {}};
    return Origin{false, legacy_default, "default", {}};
  }

  std::string RenderCfg(const std::string& path) const {
    const fss::json::Value* v = FindIn(cfg_.effective(), path);
    if (v == nullptr) return {};
    if (v->is_array()) {
      std::vector<std::string> items;
      for (const auto& e : *v) items.push_back(RenderScalar(e));
      return Join(items);
    }
    return RenderScalar(*v);
  }

  void Record(const std::string& path, const std::string& value, const Origin& o) {
    if (rows_.count(path) != 0) return;
    bool secret = false;
    if (const auto* field = schema_.Find(path); field != nullptr) secret = field->secret;
    rows_.emplace(path, Row{path, value, o.source, o.alias_var, secret});
  }

  const fss::config::Config& cfg_;
  const fss::config::Schema& schema_;
  std::map<std::string, LegacyHit> hits_;
  std::map<std::string, Row> rows_;
  std::vector<std::string> conflicts_;
  std::vector<fss::config::Problem> problems_;
};

// =============================================================================
//  组合根内联实现（R12：具体实现只在这里被创建）
// =============================================================================
//  单租户演示用的 BlobStore 工厂：所有 partition/zone 共用同一个 POSIX 根。
//  P8 会换成按 partition/zone 分盘的实现（`docs/02-design.md` §4）。
class SingleStoreFactory final : public fss::domain::IBlobStoreFactory {
 public:
  explicit SingleStoreFactory(fss::domain::IBlobStore& store) : store_(&store) {}
  fss::Result<fss::domain::IBlobStore*> ForPartition(std::string_view partition,
                                                     fss::domain::StorageZone zone) override {
    (void)partition;
    (void)zone;
    return store_;
  }

 private:
  fss::domain::IBlobStore* store_;
};

//  allow-all：只做契约要求的两条"缺什么就 401"的判断；角色判定在 P8 接入。
class AllowAllAuthorizer final : public fss::domain::IAuthorizer {
 public:
  fss::Result<void> Authorize(std::string_view required_role, std::string_view partition,
                              std::string_view bearer_token) override {
    (void)required_role;
    if (bearer_token.empty()) {
      return fss::Err(fss::ErrorKind::kUnauthenticated, "Missing authorization token");
    }
    if (partition.empty()) {
      return fss::Err(fss::ErrorKind::kUnauthenticated, "Missing partitionID");
    }
    return fss::Ok();
  }

  //  "任一角色即通过"（上游 `hasPermission('a','b')`）。**鉴权尚未接入**（P8）：
  //  与 `Authorize` 一样只做"缺 token / 缺 partition"的形式检查。
  fss::Result<void> AuthorizeAny(std::span<const std::string_view> required_roles,
                                 std::string_view partition,
                                 std::string_view bearer_token) override {
    if (required_roles.empty()) {
      return fss::Err(fss::ErrorKind::kInternal, "AuthorizeAny 要求至少一个角色");
    }
    return Authorize(required_roles.front(), partition, bearer_token);
  }
};

class NoopLegalValidator final : public fss::domain::ILegalValidator {
 public:
  fss::Result<void> Validate(std::string_view, const std::vector<std::string>& tags) override {
    if (tags.empty()) {
      return fss::Err(fss::ErrorKind::kInvalidArgument, "legal tags 不能为空");
    }
    return fss::Ok();
  }
};

class NoopSchemaValidator final : public fss::domain::ISchemaValidator {
 public:
  fss::Result<void> Validate(std::string_view, const fss::json::Value&) override {
    return fss::Ok();
  }
};

class StaticPartitionRegistry final : public fss::domain::IPartitionRegistry {
 public:
  explicit StaticPartitionRegistry(fss::domain::PartitionConfig config)
      : config_(std::move(config)) {}

  fss::Result<fss::domain::PartitionConfig> Get(std::string_view partition) override {
    if (partition != config_.partition) {
      return fss::Err(fss::ErrorKind::kNotFound, "partition 未注册：" + std::string(partition));
    }
    return config_;
  }
  fss::Result<std::vector<fss::domain::PartitionConfig>> List() override {
    return std::vector<fss::domain::PartitionConfig>{config_};
  }

 private:
  fss::domain::PartitionConfig config_;
};

class LogEventPublisher final : public fss::domain::IEventPublisher {
 public:
  explicit LogEventPublisher(const fss::logging::ILogger& logger) : logger_(logger) {}
  fss::Result<void> PublishStatusChanged(
      std::string_view topic, const fss::domain::StatusChangedEvent& event) override {
    fss::logging::Info(logger_, "status-changed",
                       {{"topic", std::string(topic)},
                        {"partition", event.partition},
                        {"status", event.status},
                        {"dataset_sync", event.dataset_sync},
                        {"version", std::to_string(event.version)}});
    return fss::Ok();
  }

  //  `datasetDetails`（契约 §2.6 第 10 步）：默认实现只写日志（与 status 事件同一策略）
  fss::Result<void> PublishDatasetDetails(
      std::string_view topic, const fss::domain::DatasetDetailsEvent& event) override {
    fss::logging::Info(logger_, "datasetDetails",
                       {{"topic", std::string(topic)},
                        {"partition", event.partition},
                        {"correlationId", event.correlation_id},
                        {"datasetId", event.dataset_id},
                        {"datasetType", event.dataset_type},
                        {"datasetVersionId", event.dataset_version_id},
                        {"recordCount", std::to_string(event.record_count)}});
    return fss::Ok();
  }

 private:
  const fss::logging::ILogger& logger_;
};

class LogAuditLogger final : public fss::domain::IAuditLogger {
 public:
  explicit LogAuditLogger(const fss::logging::ILogger& logger) : logger_(logger) {}
  fss::Result<void> Record(const fss::domain::AuditEvent& event) override {
    fss::logging::Info(logger_, "audit",
                       {{"operation", event.operation},
                        {"user", event.user},
                        {"partition", event.partition},
                        {"object_id", event.object_id},
                        {"result", event.result}});
    return fss::Ok();
  }

 private:
  const fss::logging::ILogger& logger_;
};

//  `observability.audit_enabled=false` → 审计器换成 no-op（**不是**不装审计器：
//  端口必须有实现，否则用例层还要判空）。
class NoopAuditLogger final : public fss::domain::IAuditLogger {
 public:
  fss::Result<void> Record(const fss::domain::AuditEvent&) override { return fss::Ok(); }
};

}  // namespace

int main(int argc, char** argv) {
  using namespace fss;
  using namespace fss::infra;

  // ===========================================================================
  //  ① 命令行 → 配置来源（C10.1）
  // ===========================================================================
  CliOptions cli;
  std::string cli_error;
  if (!ParseCli(argc, argv, &cli, &cli_error)) {
    std::cerr << "参数错误：" << cli_error << "\n\n" << kUsage;
    return 2;
  }
  if (cli.help) {
    std::cout << kUsage;
    return 0;
  }
  //  `--config` 优先于 `FSS_CONFIG`（CLI > env）。
  const std::string config_path =
      cli.config_from_cli ? cli.config_path : Env("FSS_CONFIG", std::string());

  // ===========================================================================
  //  ② 配置加载（C10.1/C10.2）：cli(--set) > env(通用名) > file > schema 默认
  // ===========================================================================
  config::Schema schema = config::CoreSchema();
  config::LoadRequest load_request;
  load_request.schema = schema;
  load_request.file_path = config_path;
  load_request.env_enabled = true;
  load_request.env_prefix = "FSS_";
  load_request.cli_overrides = cli.set_overrides;
  //  ★ 不在这里做别名兜底：旧别名要在 Load 之后按优先级解析（见 LegacyAliases 的说明）。
  load_request.env_lookup = [](std::string_view name) -> std::optional<std::string> {
    const std::string key(name);
    if (const char* value = std::getenv(key.c_str()); value != nullptr) return std::string(value);
    return std::nullopt;
  };
  auto loaded = config::Load(load_request);
  if (!loaded.config.ok()) {
    std::cerr << loaded.config.ProblemsToString();
    std::cerr << "拒绝启动（exit " << kExitConfigError << "，EX_CONFIG）。\n";
    return kExitConfigError;
  }
  const config::Config& cfg = loaded.config;

  // ===========================================================================
  //  ③ 旧别名 + 历史默认值 → 统一解析器（C10.2：来源可见）
  // ===========================================================================
  Resolver resolver(cfg, schema);
  resolver.WarnConflicts(std::cerr);
  if (!resolver.problems().empty()) {
    for (const auto& problem : resolver.problems()) std::cerr << "  " << problem.ToString() << "\n";
    std::cerr << "拒绝启动（exit " << kExitConfigError << "，EX_CONFIG）。\n";
    return kExitConfigError;
  }

  //  ---- 服务面（server.http.* / server.grpc.*，C10.3）----
  const std::string base_path = resolver.Str("server.http.base_path", "/api/file");
  const std::string bind_address = resolver.Str("server.http.bind", "0.0.0.0");
  const long http_port = resolver.Int("server.http.port", 8080);
  const long grpc_port = resolver.Int("server.grpc.port", 0);
  const std::string grpc_bind = resolver.Str("server.grpc.bind", bind_address);
  const long worker_threads = resolver.Int("server.http.worker_threads", 0);
  const long max_connections = resolver.Int("server.http.max_connections", 0);
  const bool tcp_nodelay = resolver.Bool("server.http.tcp_nodelay", true);
  const long idle_timeout = resolver.Int("server.http.idle_timeout_seconds", 60);
  const long json_timeout = resolver.Int("server.http.json_request_timeout_seconds", 15);
  const long transfer_idle_timeout =
      resolver.Int("server.http.transfer_idle_timeout_seconds", 120);
  const long transfer_buffer_bytes = resolver.Int("server.http.transfer_buffer_bytes", 262144);
  const long transfer_memory_budget =
      resolver.Int("server.http.transfer_memory_budget_bytes", 268435456);
  const long max_header_bytes = resolver.Int("server.http.max_header_bytes", 16384);
  const long max_uri_bytes = resolver.Int("server.http.max_uri_bytes", 8192);
  const long max_body_bytes = resolver.Int("server.http.max_body_bytes", 10485760);
  const std::string error_format = resolver.Str("http.error_format", "apperror");

  //  ---- 存储（storage.*，C10.4）----
  const std::string storage_driver = resolver.Str("storage.driver", "posix");
  const std::string storage_root = resolver.Str("storage.posix.root", "/tmp/fss-data");
  const std::string durability = resolver.Str("storage.posix.durability", "per_file");
  const long fsync_threshold_bytes =
      resolver.Int("storage.posix.fsync_threshold_bytes", 1024 * 1024);
  const std::string posix_instance_id = resolver.Str("deployment.instance_id", "local");
  const std::string io_engine = resolver.Str("storage.io_engine", "blocking");
  const long io_queue_depth = resolver.Int("storage.io_uring.queue_depth", 64);
  const std::string s3_endpoint = resolver.Str("storage.s3.endpoint", "");
  const std::string s3_region = resolver.Str("storage.s3.region", "us-east-1");
  const std::string s3_access_key = resolver.Str("storage.s3.access_key", "");
  const std::string s3_secret_key = resolver.Str("storage.s3.secret_key", "");
  const bool s3_force_path_style = resolver.Bool("storage.s3.force_path_style", true);
  const bool s3_verify_tls = resolver.Bool("storage.s3.verify_tls", true);
  const long s3_connect_timeout_ms = resolver.Int("storage.s3.connect_timeout_ms", 3000);
  const long s3_total_timeout_ms = resolver.Int("storage.s3.total_timeout_ms", 30000);
  const long s3_presign_default = resolver.Int("storage.s3.presign_default_seconds", 3600);
  const long s3_presign_max = resolver.Int("storage.s3.presign_max_seconds", 604800);
  const std::string s3_driver_report = resolver.Str("storage.driver_report_override", "");
  const std::string s3_provider_key = resolver.Str("storage.provider_key_override", "");

  //  ---- 仓储（ADR-004：单实例 = 内置 SQLite）----
  const std::string metadata_repository_name = resolver.Str("metadata.repository", "sqlite");
  const std::string location_repository_name = resolver.Str("location.repository", "sqlite");
  const std::string metadata_db_path =
      resolver.Str("metadata.sqlite.path", storage_root + "/metadata.db");
  const std::string sqlite_path =
      resolver.Str("location.sqlite.path", storage_root + "/location.db");

  //  ---- 自签数据面 ----
  const bool self_signed_enabled = resolver.Bool("self_signed.enabled", true);
  const std::string transfer_secret =
      resolver.Str("self_signed.signing_key", "dev-secret-change-me");
  //  ★ 自签 URL 的 `<base>` 必须**包含路由 base path**：数据面挂在
  //    `/api/file/v1/transfer/{token}`（契约 §7 / §2 表），若这里只给 `host:port`，
  //    发出去的上传地址会指向 404（P4-D04，端到端用例抓到）。
  const std::string self_base_url =
      resolver.Str("self_signed.public_base_url",
                   "http://127.0.0.1:" + std::to_string(http_port) + base_path);

  //  ---- 部署形态（ADR-009）----
  const std::string deployment_mode = resolver.Str("deployment.mode", "single");
  const long max_clock_skew_seconds =
      resolver.Int("deployment.max_clock_skew_seconds", 5);
  const std::string deployment_environment =
      resolver.Str("deployment.environment", "development");

  //  ---- 认证（P8 / ADR-012 / C10.5）----
  const std::string auth_mode = resolver.Str("auth.mode", "disabled");
  const std::string jwt_hmac_secret = resolver.Str("auth.jwt.hmac_secret", "");
  const std::string jwt_issuer = resolver.Str("auth.jwt.issuer", "");
  const std::string jwt_audience = resolver.Str("auth.jwt.audience", "");
  const bool jwt_verify_signature = resolver.Bool("auth.jwt.verify_signature", true);
  const std::string jwt_user_id_claim = resolver.Str("auth.jwt.user_id_claim", "email");
  const std::string jwt_partition_claim =
      resolver.Str("auth.jwt.partition_claim", "data-partition-id");
  const bool jwt_require_partition_claim =
      resolver.Bool("auth.jwt.require_partition_claim", true);
  const std::string entitlements_url = resolver.Str("auth.remote_entitlements.base_url", "");
  const std::string entitlements_authorize_path =
      resolver.Str("auth.remote_entitlements.authorize_path",
                   "/api/entitlements/v2/authorizeAny");
  const long entitlements_timeout_ms = resolver.Int("auth.remote_entitlements.timeout_ms", 3000);
  const long entitlements_connect_timeout_ms =
      resolver.Int("auth.remote_entitlements.connect_timeout_ms", 1000);
  //  ★ `auth.remote_entitlements.fail_closed` 已读，但实现**恒为** fail-closed（不可关）：
  //    schema 对 remote 模式强制 true，因此这里只登记来源，不参与分支。
  (void)resolver.Bool("auth.remote_entitlements.fail_closed", true);

  //  ---- 可观测性（observability.*，C10.6）----
  const std::string log_level_text = resolver.Str("observability.log_level", "info");
  const std::string log_format = resolver.Str("observability.log_format", "json");
  //  ★ `observability.log_service` **不在 schema 内**（CoreSchema/示例文件都没有这个键）；
  //    组合根保留专用环境变量 `FSS_LOG_SERVICE`，语义与前一致（默认 file-service）。
  const std::string log_service = Env("FSS_LOG_SERVICE", "file-service");
  const std::vector<std::string> redact_keys =
      resolver.List("observability.redact_keys", kDefaultRedactKeys);
  const bool audit_enabled = resolver.Bool("observability.audit_enabled", true);
  //  ★ `audit_fail_closed` 已读并可打印来源，但当前**没有"致命审计"路径**：
  //    `RecordAudit()` 丢弃 `IAuditLogger::Record` 的结果（审计失败只影响记录本身），
  //    因此本键的行为**未实现**（如实登记在 docs/operations.md §8）。
  const bool audit_fail_closed = resolver.Bool("observability.audit_fail_closed", false);
  const bool metrics_enabled = resolver.Bool("observability.metrics_enabled", true);
  const std::string metrics_path = resolver.Str("observability.metrics_path", "/metrics");

  if (cli.print_config) {
    std::cout << "配置来源 : "
              << (config_path.empty() ? "无配置文件（仅环境变量）" : config_path) << "\n";
    resolver.Print(std::cout);
    return 0;
  }

  //  ---- 值域/依赖关系校验：任何一条不过 → 拒绝启动（exit 78）----
  if (deployment_mode == "multi") {
    //  `deployment.mode=multi` 的 5 条配置校验已经在 schema 里（C8.9），但**运行形态**还依赖
    //  PG 版仓储、PG 租约与数据库时钟（ADR-009；计划 P9/阶段 10 明确不承诺）——本版本没有交付。
    //  ★ 明确拒绝启动，而不是"以单实例状态跑在多实例里"（那会让各实例状态发散）。
    std::cerr << "拒绝启动：deployment.mode=multi 需要 PG 仓储 + PG 租约 + 数据库时钟"
                 "（ADR-009），本版本尚未交付。\n";
    return kExitConfigError;
  }
  if (metadata_repository_name != "sqlite" || location_repository_name != "sqlite") {
    //  非 sqlite 的仓储实现（postgres/remote）尚未交付：**拒绝启动**而不是静默用 SQLite
    //  （否则"配置写了 postgres、实际写 SQLite"是最危险的一类静默降级）。
    std::cerr << "拒绝启动：仅支持 metadata.repository=sqlite 且 location.repository=sqlite"
                 "（当前 metadata.repository=" << metadata_repository_name
              << "，location.repository=" << location_repository_name
              << "）；postgres/remote 实现尚未交付（ADR-004/ADR-009）。\n";
    return kExitConfigError;
  }
  if (deployment_environment == "production" && auth_mode != "jwt") {
    //  schema 的跨字段规则已经拦了这条；这里再兜一层，保证"生产不可误配成无鉴权"。
    std::cerr << "拒绝启动：deployment.environment=production 要求 auth.mode=jwt"
                 "（当前 auth.mode=" << auth_mode << "）。\n";
    return kExitConfigError;
  }
  if (metrics_enabled && (metrics_path.empty() || metrics_path.front() != '/')) {
    std::cerr << "拒绝启动：observability.metrics_path 必须以 '/' 开头（当前='" << metrics_path
              << "'）。\n";
    return kExitConfigError;
  }

  //  ---- I/O 引擎（ADR-010 / C10.4 的 `storage.io_engine`）----
  sys::IoEngineProbe uring_probe = sys::ProbeIoUring(static_cast<int>(io_queue_depth));
  const auto io_mode = sys::ParseIoEngineMode(io_engine);
  if (!io_mode.has_value()) {
    std::cerr << "拒绝启动：storage.io_engine 取值非法：" << io_engine
              << "（允许 blocking | uring | auto）。\n";
    return kExitConfigError;
  }
  //  UringIoEngine::enabled() 恒为 false（ADR-010 的 U1~U4 未满足）→ "可用"必须是
  //  "内核探测通过 **且** 引擎实现已启用"，否则就是"以为开了加速、其实一直跑阻塞路径"。
  const bool uring_usable = uring_probe.available() && infra::UringIoEngine::enabled();
  if (*io_mode == sys::IoEngineMode::kUring && !uring_usable) {
    std::cerr << "拒绝启动：storage.io_engine=uring 被显式要求，但 io_uring 不可用。\n"
              << "  探测结果：" << uring_probe.ToString() << "\n";
    if (uring_probe.available()) {
      std::cerr << "  原因：内核探测通过，但 UringIoEngine 实现尚未启用（ADR-010 U1~U4）。\n";
    }
    std::cerr << "  提示：在目标环境运行 scripts/check_io_uring.sh 确认可用性"
                 "（退出码 0=可用 / 1=不可用 / 2=无结论）；或改用 "
                 "storage.io_engine=blocking|auto。\n";
    return kExitConfigError;
  }
  const bool io_fallback =
      (*io_mode == sys::IoEngineMode::kAuto && !uring_usable);
  const std::string io_engine_active =
      (*io_mode == sys::IoEngineMode::kBlocking || io_fallback) ? "blocking" : "uring";
  const std::string io_note =
      io_fallback ? ("io_uring 不可用（" + uring_probe.errno_name +
                     (uring_probe.available() ? "；引擎实现未启用（ADR-010 U1~U4）" : "") +
                     "）→ 已回退 blocking（预期行为）")
                  : uring_probe.ToString();

  // ===========================================================================
  //  ④ 依赖构造（R12：唯一实例化具体实现的位置）
  // ===========================================================================
  SystemClock clock;
  const auto log_level = logging::ParseLevel(log_level_text).value_or(logging::Level::kInfo);
  logging::StreamLogger logger(
      logging::OptionsFromConfig(log_level, log_format, log_service, Join(redact_keys)), clock);
  UuidGenerator ids;

  std::unique_ptr<domain::IBlobStore> blob_store;
  if (storage_driver == "s3") {
    if (s3_endpoint.empty() || s3_access_key.empty() || s3_secret_key.empty()) {
      std::cerr << "拒绝启动：storage.driver=s3 需要 storage.s3.endpoint / access_key / "
                   "secret_key（或旧环境变量 FSS_STORAGE_S3_ENDPOINT / _ACCESS_KEY / "
                   "_SECRET_KEY / FSS_S3_ACCESS_KEY / FSS_S3_SECRET_KEY）。\n";
      return kExitConfigError;
    }
    infra::S3Options s3_options;
    s3_options.endpoint = s3_endpoint;
    s3_options.region = s3_region;
    s3_options.force_path_style = s3_force_path_style;
    s3_options.verify_tls = s3_verify_tls;
    s3_options.credentials = infra::AwsCredentials{s3_access_key, s3_secret_key, ""};
    s3_options.connect_timeout_ms = s3_connect_timeout_ms;
    s3_options.total_timeout_ms = s3_total_timeout_ms;
    s3_options.presign_default_seconds = s3_presign_default;
    s3_options.presign_max_seconds = s3_presign_max;
    s3_options.transfer_idle_timeout_seconds = transfer_idle_timeout;
    s3_options.driver_report_override = s3_driver_report;
    s3_options.provider_key_override = s3_provider_key;
    blob_store = std::make_unique<infra::S3BlobStore>(s3_options, clock);
  } else if (storage_driver == "posix") {
    std::error_code blob_dir_error;
    std::filesystem::create_directories(storage_root + "/blobs", blob_dir_error);
    if (blob_dir_error) {
      std::cerr << "创建存储根失败: " << blob_dir_error.message() << "\n";
      return kExitConfigError;
    }
    PosixBlobStoreOptions posix_options;
    if (durability == "per_file") {
      posix_options.fsync_policy = FsyncPolicy::kAlways;
    } else if (durability == "batch") {
      posix_options.fsync_policy = FsyncPolicy::kBySize;
      posix_options.fsync_threshold_bytes = fsync_threshold_bytes;
    } else if (durability == "never") {
      //  ★ 只能用于"数据可重建"的场景：这里显式告警，不做静默降级
      std::cerr << "警告：storage.posix.durability=never —— 进程崩溃可能丢已确认的写入\n";
      posix_options.fsync_policy = FsyncPolicy::kNever;
    } else {
      std::cerr << "拒绝启动：未知的 storage.posix.durability：" << durability
                << "（可选：per_file | batch | never）\n";
      return kExitConfigError;
    }
    //  ★ 临时文件名里的实例标识（ADR-009 M1：多实例共盘时不得互相踩）来自
    //    `deployment.instance_id`（schema 里唯一的实例标识键；默认 local）。
    posix_options.instance_id = posix_instance_id;
    blob_store = std::make_unique<PosixBlobStore>(storage_root + "/blobs", clock, posix_options);
  } else {
    std::cerr << "拒绝启动：未知的 storage.driver：" << storage_driver
              << "（可选：posix | s3）\n";
    return kExitConfigError;
  }

  //  ★ C9.6 的"暴露存储指标"必须在**真实进程**上成立：此前组合根从不创建 Registry，
  //    `RouterOptions::metrics_registry` 恒为 nullptr → `/metrics` 只有 HTTP 族，
  //    存储操作/字节只在测试夹具里被验证过（"测试里通过、产品里不存在"，正是 R15 那类陷阱）。
  //    这里把驱动包一层计量装饰器（纯转发，不改语义），并把注册表接到 `/metrics`。
  metrics::Registry metrics_registry;
  //  ★ C10.4/R11：I/O 引擎的探测与回退结果必须**可见**（横幅 + /metrics），
  //    否则"auto 回退到 blocking"只能等线上性能回归才发现。
  metrics_registry.Register("fss_io_engine", metrics::Registry::Kind::kGauge,
                            "当前生效的 I/O 引擎（1 = 生效；requested=配置请求值）");
  metrics_registry.SetGauge("fss_io_engine", 1,
                            {{"engine", io_engine_active}, {"requested", io_engine}});
  MeteredBlobStore metered_blob(*blob_store, metrics_registry,
                               storage_driver == "s3" ? "s3" : "posix");
  SingleStoreFactory blob_factory(metered_blob);

  auto location_repository = SqliteLocationRepository::Open(sqlite_path);
  if (!location_repository.ok()) {
    std::cerr << "打开位置仓储失败: " << location_repository.error().ToString() << "\n";
    return kExitConfigError;
  }
  auto metadata_repository_handle = SqliteMetadataRepository::Open(metadata_db_path, clock);
  if (!metadata_repository_handle.ok()) {
    std::cerr << "打开元数据仓储失败: " << metadata_repository_handle.error().ToString() << "\n";
    return kExitConfigError;
  }
  HmacTransferTokenCodec token_codec(transfer_secret, clock);

  //  ---- 认证（P8 / ADR-012）----
  //  `jwt` = 本地 HS256 校验（生产形态）；`disabled` = allow-all（仅开发）。
  AllowAllAuthorizer allow_all_authorizer;
  std::unique_ptr<infra::LocalJwtAuthorizer> jwt_authorizer;
  std::unique_ptr<infra::RemoteEntitlementsAuthorizer> remote_entitlements;
  domain::IAuthorizer* authorizer = &allow_all_authorizer;
  if (auth_mode == "jwt") {
    infra::LocalJwtOptions jwt_options;
    jwt_options.hmac_secret = jwt_hmac_secret;
    jwt_options.issuer = jwt_issuer;
    jwt_options.audience = jwt_audience;
    jwt_options.verify_signature = jwt_verify_signature;
    jwt_options.user_id_claim = jwt_user_id_claim;
    jwt_options.partition_claim = jwt_partition_claim;
    jwt_options.require_partition_claim = jwt_require_partition_claim;
    //  C8.10：容忍范围与 `deployment.max_clock_skew_seconds` 同源（默认 5 秒）
    jwt_options.clock_skew_seconds = max_clock_skew_seconds;
    jwt_authorizer = std::make_unique<infra::LocalJwtAuthorizer>(jwt_options, clock);
    authorizer = jwt_authorizer.get();
    //  ★ 升级为显式拒绝启动（而不是"起来但拒绝所有 token"）：空密钥 100% 是配置错误，
    //    让它以 401 的形式暴露给调用方只会制造误导性的排障路径。
    if (!jwt_authorizer->Ready()) {
      std::cerr << "拒绝启动：" << jwt_authorizer->NotReadyReason() << "\n";
      return kExitConfigError;
    }
  } else if (auth_mode == "remote-entitlements") {
    infra::RemoteEntitlementsOptions remote_options;
    remote_options.base_url = entitlements_url;
    remote_options.authorize_path = entitlements_authorize_path;
    remote_options.timeout_ms = static_cast<int>(entitlements_timeout_ms);
    remote_options.connect_timeout_ms = static_cast<int>(entitlements_connect_timeout_ms);
    remote_entitlements = std::make_unique<infra::RemoteEntitlementsAuthorizer>(remote_options);
    authorizer = remote_entitlements.get();
    //  ★ 没配地址就**拒绝启动**：起来之后"拒绝一切"只会制造误导性的排障路径
    if (!remote_entitlements->Ready()) {
      std::cerr << "拒绝启动：" << remote_entitlements->NotReadyReason() << "\n";
      return kExitConfigError;
    }
    logging::Warn(logger,
                  "auth.mode=remote-entitlements：依赖不可用时 fail-closed（503），绝不放行",
                  {{"component", "server_main"}});
  } else if (auth_mode != "disabled") {
    std::cerr << "拒绝启动：未知的 auth.mode：" << auth_mode
              << "（可选：jwt | remote-entitlements | disabled）\n";
    return kExitConfigError;
  } else {
    logging::Warn(logger,
                  "auth.mode=disabled（allow-all）：仅用于开发/测试，禁止用于生产",
                  {{"component", "server_main"}});
  }

  LogEventPublisher events(logger);
  LogAuditLogger log_audit(logger);
  NoopAuditLogger noop_audit;
  domain::IAuditLogger* audit_logger =
      audit_enabled ? static_cast<domain::IAuditLogger*>(&log_audit) : &noop_audit;

  domain::PartitionConfig partition;
  partition.partition = "opendes";
  partition.driver = storage_driver == "s3" ? domain::StorageDriver::kS3
                                            : domain::StorageDriver::kPosix;
  partition.posix_root = storage_root;
  StaticPartitionRegistry partitions(partition);

  NoopLegalValidator legal;
  NoopSchemaValidator schema_validator;

  app::LocationIssuer issuer(blob_factory, *location_repository.value(), token_codec, clock, ids,
                             self_base_url);

  app::UseCasePorts ports{blob_factory,      *location_repository.value(),
                          *metadata_repository_handle.value(),
                          *authorizer,       events,
                          *audit_logger,     partitions,
                          legal,             schema_validator,
                          issuer,            clock,
                          ids};
  ports.auth_mode = auth_mode;  // C8.5：让 `/v2/info` 与 gRPC 的 `GetInfo` 都能看到

  //  ---- 路由 ----
  const auto parsed_error_format = adapters::http::ParseErrorFormat(error_format);
  if (!parsed_error_format.has_value()) {
    std::cerr << "拒绝启动：http.error_format 取值非法：" << error_format
              << "（允许 apperror | legacy | api_error）\n";
    return kExitConfigError;
  }
  adapters::http::RouterOptions router_options;
  router_options.error_format = *parsed_error_format;
  router_options.base_path = base_path;
  router_options.metrics_registry = &metrics_registry;  // `/metrics` 渲染存储/IO 族
  router_options.metrics_enabled = metrics_enabled;
  router_options.metrics_path = metrics_path;
  router_options.json_body_limit_bytes = max_body_bytes;

  //  数据面回调：适配层不认识 L2，由这里把 TransferEndpoint 绑上去（R12）。
  //  ★ 只在**集中存储**模式下注册：S3 有原生预签名，客户端直连存储端点，
  //    服务不代理字节（C5.8）；此时注册 `/v1/transfer` 是死代码。
  infra::TransferEndpoint transfer_endpoint(token_codec, blob_factory);
  adapters::http::TransferCallbacks transfers;
  const bool with_self_signed_data_plane = storage_driver == "posix" && self_signed_enabled;
  if (with_self_signed_data_plane) {
    transfers.put = [&transfer_endpoint](std::string_view token, std::string_view expires,
                                         std::string_view signature, std::string_view partition,
                                         bytes::ByteSource& body) -> Result<void> {
      infra::TransferRequest request{std::string(token), std::string(expires),
                                     std::string(signature), "put", std::string(partition)};
      return transfer_endpoint.Put(request, body);
    };
    transfers.open_get = [&transfer_endpoint, &blob_factory](
                             std::string_view token, std::string_view expires,
                             std::string_view signature, std::string_view partition)
        -> Result<std::shared_ptr<bytes::ByteSource>> {
      infra::TransferRequest request{std::string(token), std::string(expires),
                                     std::string(signature), "get", std::string(partition)};
      FSS_TRY(resolved, transfer_endpoint.Resolve(request, "get"));
      FSS_TRY(stat, transfer_endpoint.Stat(request));
      FSS_TRY(store, blob_factory.ForPartition(resolved.partition, resolved.zone));
      domain::ObjectRef ref;
      ref.container = resolved.container;
      ref.key = resolved.object_key;
      return std::static_pointer_cast<bytes::ByteSource>(
          std::make_shared<infra::BlobByteSource>(*store, std::move(ref), stat.size));
    };
  }  // 仅 POSIX 模式（且 self_signed.enabled）注册自签数据面

  adapters::http::Router router(ports, transfers, router_options);

  //  ---- gRPC 面：与 REST 共用**同一个** `UseCasePorts`（C7.8：双协议同时运行）----
  //  ★ 与 HTTP 面一样，服务的具体实现只在这里创建（R12）；两个服务共用同一批
  //    端口实现（同一个 blob/location/metadata 实例），因此两条链路看到同一份状态。
  //  ★ 组合根**不直接**碰 grpc 类型：服务器的生命周期收在 `adapters/grpc/grpc_server.h`
  //    的包装里（C7.7 的护栏要求 `src/` 全树除 adapters/grpc/ 外不出现 `<grpcpp/`；
  //    与 `fss_http` 把 httplib 挡在适配层内是同一条纪律）。
  std::unique_ptr<adapters::grpc::FileServiceAdapter> grpc_service;
  std::unique_ptr<adapters::grpc::GrpcServerHandle> grpc_server;
  if (grpc_port != 0) {
    grpc_service = std::make_unique<adapters::grpc::FileServiceAdapter>(ports, "osdu-user");
    grpc_server =
        adapters::grpc::StartGrpcServer(*grpc_service, grpc_bind, static_cast<int>(grpc_port));
    if (!grpc_server->ok()) {
      std::cerr << grpc_server->last_error() << "\n";
      return kExitConfigError;
    }
  }

  http::ServerOptions server_options;
  server_options.bind_address = bind_address;
  server_options.port = static_cast<int>(http_port);
  server_options.worker_threads = static_cast<int>(worker_threads);
  server_options.max_connections = static_cast<int>(max_connections);
  server_options.tcp_nodelay = tcp_nodelay;
  server_options.read_timeout_sec = static_cast<int>(idle_timeout);
  server_options.json_request_timeout_sec = static_cast<int>(json_timeout);
  server_options.transfer_idle_timeout_sec = static_cast<int>(transfer_idle_timeout);
  server_options.transfer_buffer_bytes = transfer_buffer_bytes;
  server_options.transfer_memory_budget_bytes = transfer_memory_budget;
  server_options.max_header_bytes = max_header_bytes;
  server_options.max_uri_bytes = max_uri_bytes;
  server_options.service_name = log_service;
  server_options.error_format = error_format;
  //  ★ C10.3：配置层（schema 跨字段 + 单字段范围）已校验一次，这里在**构造 Server 前**
  //    再调用 `http::ValidateOptions`（第二道防线），并把原因逐条打印 ——
  //    否则 `max_connections > worker_threads` 这类组合会让"背压"静默失效。
  const auto options_valid = http::ValidateOptions(server_options);
  if (!options_valid.ok()) {
    std::cerr << "拒绝启动：server.http.* 配置非法：" << options_valid.error().message() << "\n";
    return kExitConfigError;
  }
  http::Server server(server_options, logger, clock);
  router.Register(server);

  if (!server.Bind()) {
    std::cerr << "监听失败: " << server.last_error() << "\n";
    return kExitConfigError;
  }

  //  ★ 启动横幅的**第一行**必须是配置来源（C10.1）：运维第一眼要能回答"这次到底读了哪份配置"。
  std::cout << "fss_server 配置来源 : "
            << (config_path.empty() ? "无配置文件（仅环境变量）" : config_path) << "\n"
            << "fss_server 已启动\n"
            << "  bind           : " << bind_address << ":" << server.port() << "\n"
            << "  grpc bind      : "
            << (grpc_server ? grpc_bind + ":" + std::to_string(grpc_server->port())
                            : std::string("disabled（server.grpc.port=0）"))
            << "\n"
            << "  base path      : " << base_path << "\n"
            << "  storage driver : " << storage_driver << "\n"
            << "  storage root   : " << storage_root << "\n"
            << "  sqlite path    : " << sqlite_path << "\n"
            << "  metadata path  : " << metadata_db_path << "\n"
            << "  io engine      : " << io_engine_active << "（请求 " << io_engine
            << (io_fallback ? "；已回退" : "") << "） — " << io_note << "\n"
            << "  error format   : " << adapters::http::ErrorFormatName(router_options.error_format)
            << "\n"
            << "  log            : level=" << logging::LevelName(logger.options().min_level)
            << " format=" << logger.options().format << " redact="
            << logger.options().redact_keys.size() << " 个键\n"
            << "  metrics        : "
            << (router_options.metrics_enabled
                    ? "已接入 " + router_options.metrics_path + "（含存储计量）"
                    : std::string("未接入（observability.metrics_enabled=false）"))
            << "\n"
            << "  audit          : "
            << (audit_enabled ? "开启" : "关闭（observability.audit_enabled=false）")
            << (audit_fail_closed ? "；fail_closed=true（★该行为未实现，仅登记）" : "") << "\n"
            << "  auth           : "
            << (auth_mode == "jwt"
                    ? "jwt（HS256 本地校验）"
                    : (auth_mode == "remote-entitlements"
                           ? "remote-entitlements（fail-closed，地址 " + entitlements_url + "）"
                           : "disabled（allow-all，仅开发/测试）"))
            << "\n"
            << "  environment    : " << deployment_environment << "\n";
  //  C10.2：逐键打印来源缩写（cli/env/file/default），`(别名 FSS_X)` 表示旧环境变量。
  std::cout << "  config sources :\n";
  for (const auto& [path, row] : resolver.rows()) {
    std::cout << "    " << path << " [" << row.source;
    if (!row.alias_var.empty()) std::cout << " 别名 " << row.alias_var;
    std::cout << "]\n";
  }
  std::cout.flush();

  server.Listen();

  //  ★ 退出路径必须**显式**关掉 gRPC 服务：`grpc::Server` 是 joinable 的资源，
  //    提前 return 或析构顺序不当会让进程挂在 gRPC 的线程池上（与"先 stop 再 join"
  //    同一条纪律，见 AGENTS.md §4.3）。
  if (grpc_server) {
    grpc_server->Shutdown();
    grpc_server.reset();
  }
  grpc_service.reset();
  return 0;
}
