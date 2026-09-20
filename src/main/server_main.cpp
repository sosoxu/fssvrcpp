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
#include "app/services/expiry_policy.h"
#include "app/services/location_issuer.h"
#include "app/services/object_key_policy.h"
#include "app/tasks/gc_task.h"
#include "app/usecases/usecases.h"
#include "common/config/config.h"
#include "common/crypto/crypto.h"
#include "common/http/http.h"
#include "common/ids/id_generator.h"
#include "common/json/json.h"
#include "common/logging/logging.h"
#include "common/metrics/metrics.h"
#include "common/result/result.h"
#include "common/sys/capability.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "app/version.h"
#include "infra/auth/local/local_jwt_authorizer.h"
#include "infra/auth/remote/remote_entitlements_authorizer.h"
#include "infra/blob/metered/metered_blob_store.h"
#include "infra/blob/posix/posix_blob_store.h"
#include "infra/blob/posix/shared_mount_probe.h"
#include "infra/blob/s3/s3_blob_store.h"
#include "infra/event/webhook_event_publisher.h"
#include "infra/io/uring_io_engine.h"
#include "infra/location/memory/memory_lease_repository.h"
#include "infra/location/postgres/postgres_lease_repository.h"
#include "infra/location/postgres/postgres_location_repository.h"
#include "infra/location/sqlite/sqlite_location_repository.h"
#include "infra/io/file_sync.h"
#include "infra/legal/remote_legal_validator.h"
#include "infra/metadata/postgres/postgres_metadata_repository.h"
#include "infra/metadata/sqlite/sqlite_metadata_repository.h"
#include "infra/postgres/pg_leader_election.h"
#include "infra/postgres/pg_instance_registry.h"
#include "infra/postgres/pg_schema.h"
#include "infra/schema/remote_schema_validator.h"
#include "infra/transfer/blob_byte_source.h"
#include "infra/transfer/transfer_endpoint.h"
#include "infra/transfer/transfer_token.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

//  EX_CONFIG（sysexits.h）：配置/部署形态非法 → 拒绝启动。写进 docs/operations.md。
constexpr int kExitConfigError = 78;

//  EX_SOFTWARE（sysexits.h）：**未预期异常**逃到顶层 → 干净退出（C9.32）。
//  ★ 为什么不复用 78（EX_CONFIG）：78 的语义是"这份配置/部署形态不合法，改配置再来"。
//    线程创建 EAGAIN、bad_alloc 这类**运行期资源耗尽**与配置文件无关 —— 归到 78 会把
//    运维引向错误的排查方向（反复改配置文件），而真实原因在容器 `--pids-limit` / 内存。
//  ★ 为什么不复用 2：2 在本程序里是"未知命令行参数 / 用法错误"（`ParseCli` 失败），
//    而且 2 是 shell 内建命令误用的通用保留码；把异常折进 2 就无法区分这两类。
//  70 = EX_SOFTWARE 是唯一"内部软件错误"的标准退出码，且不与 78/2 冲突。
constexpr int kExitInternalError = 70;

const char* const kUsage =
    "用法: fss_server [选项]\n"
    "  --config <path>          加载带注释的 JSON 配置文件（等价环境变量 FSS_CONFIG）\n"
    "  --set <json.path>=<值>   覆盖单个配置项，可重复；优先级最高\n"
    "  --once                   只跑一轮 GC 后退出（退出码 0；便于 cron/一次性清理）\n"
    "  --print-config           打印脱敏后的有效配置与每项来源，随后退出（退出码 0）\n"
    "  --help, -h               打印本用法\n"
    "优先级: --set(cli) > 环境变量(通用名 FSS_<PATH>) > 旧环境变量别名 > 配置文件 > 默认值\n"
    "配置非法时的退出码: 78 (EX_CONFIG)\n";

std::string Env(const char* key, const std::string& fallback) {
  if (const char* value = std::getenv(key); value != nullptr && *value != '\0') return value;
  return fallback;
}

// =============================================================================
//  启动/运行期**故障注入接缝**（测试/演练用；**不是配置键**）
// =============================================================================
//  环境变量 `FSS_STARTUP_FAULT_INJECT` 的取值：
//    ""                     → 不注入（默认；**生产绝不设置**）
//    "throw_system_error"   → 精确复刻容器 `--pids-limit` 过小时的失败形态：
//                             `pthread_create` 返回 `EAGAIN` → `std::thread` 抛
//                             `std::system_error(resource_unavailable_try_again,
//                             "Resource temporarily unavailable")`
//    "throw_bad_alloc"      → `std::bad_alloc{}`（内存耗尽）
//    "throw_unknown"        → 一个**非 std** 类型的自定义结构（唯一目的是驱动顶层
//                             `catch (...)`）
//    "throw_after_start"    → HTTP 服务器已 `Start()`、GC 调度线程已 `Start()`
//                             **之后**再抛（证明异常路径上 RAII 清理真的发生：
//                             `GcScheduler::~GcScheduler → Stop()+join`、
//                             `http::Server::~Server → Stop()`）
//  未知取值 → **不注入**（与 `FSS_AUDIT_FAULT_INJECT` 只认 `"1"` 同一宽容策略；这是
//  测试接缝，不做配置级 fail-fast，避免误伤）。
//
//  ★ 为什么不做成配置键（与 `FSS_AUDIT_FAULT_INJECT` / P9 的 `mock_entitlements
//    --fail-file` 同一理由）：
//    ① 157 个叶子键的三态清单（生效 128 / 拒绝启动 15 / 已读但无效果 14）是
//       `test_operations_doc` **机械比对** `config/fss.example.json` 与
//       `docs/operations.md` 的；凭空加一个键会让计数与逐键语义双双失真；
//    ② 它也不是运维语义 —— 没有"生产上要不要让启动抛异常"这种配置项；
//       `--pids-limit` 才是真实的那条路径（容器层，不在应用配置面）。
//  ⚠️ **不要在生产设置** `FSS_STARTUP_FAULT_INJECT`（见 docs/runbook.md 的测试/演练小节）。
// =============================================================================
struct StartupFaultUnknown {};  // 非 std 类型：唯一的用途是驱动顶层 `catch (...)`

std::string StartupFaultInjection() {
  const char* value = std::getenv("FSS_STARTUP_FAULT_INJECT");
  return (value == nullptr) ? std::string() : std::string(value);
}

//  按取值抛出对应异常（`which` 必须是上面登记的取值之一；调用点已判定）。
[[noreturn]] void ThrowStartupFault(const std::string& which) {
  if (which == "throw_system_error") {
    throw std::system_error(
        std::make_error_code(std::errc::resource_unavailable_try_again),
        "Resource temporarily unavailable");
  }
  if (which == "throw_bad_alloc") {
    throw std::bad_alloc{};
  }
  //  `throw_unknown` 与 `throw_after_start` 都抛**非 std** 类型：前者覆盖
  //  `catch (...)`，后者覆盖"晚注入点上 RAII 清理完成、随后仍被顶层接住"。
  throw StartupFaultUnknown{};
}

//  早注入点：**CLI 解析之后、任何资源装配之前**。与 pids 那次故障的发生时机同段
//  （都发生在装配期线程创建），因此注入形态与真实故障同族。
void MaybeInjectStartupFaultEarly() {
  const std::string which = StartupFaultInjection();
  if (which == "throw_system_error" || which == "throw_bad_alloc" || which == "throw_unknown") {
    ThrowStartupFault(which);
  }
}

//  晚注入点：HTTP 服务器已 `Start()`、GC 调度线程已 `Start()` 之后。
//  ★ **只在主线程抛**：跨线程逃出的异常会直接 `terminate`（那是另一个话题，不在本切片）。
void MaybeInjectStartupFaultAfterStart() {
  if (StartupFaultInjection() == "throw_after_start") ThrowStartupFault("throw_after_start");
}

//  ★ C9.26：**测试专用**的"持 claim 窗口"接缝（环境变量 `FSS_CLAIM_HOLD_MS`，
//    **不是配置键**；见 docs/runbook.md §10.2、证据 docs/test-evidence/phase10.md §20）。
//    `CreateFileMetadata` 在原子领取成功 + 在途租约就绪之后、复制之前阻塞该毫秒数。
//    为什么不做成配置键（与 `FSS_STARTUP_FAULT_INJECT` / `FSS_AUDIT_FAULT_INJECT` 同一理由）：
//      ① 它是测试接缝，没有运维语义 —— 三态清单（生效 128 / 拒绝启动 15 / 已读但无效果 14）
//         由 `test_operations_doc` 与 `config/fss.example.json` **机械比对**，凭空加键会让
//         计数与逐键语义双双失真；
//      ② 生产上"让每个 createMetadata 故意卡住 N 毫秒"只会制造事故。
//    取值解析：非数字 / <= 0 / 空 → **不注入**（与其它接缝一致的宽容策略：测试接缝不做
//    fail-fast，避免误伤启动）。默认（未设置）= 0 = 生产路径逐字不变。
std::int64_t ClaimHoldMillisFromEnv() {
  const char* const value = std::getenv("FSS_CLAIM_HOLD_MS");
  if (value == nullptr || *value == '\0') return 0;
  char* end = nullptr;
  const long long parsed = std::strtoll(value, &end, 10);
  if (end == value || *end != '\0' || parsed <= 0) return 0;
  return static_cast<std::int64_t>(parsed);
}

//  ★ B2a：**测试专用**的"时钟偏移注入"接缝（环境变量 `FSS_CLOCK_SKEW_INJECT_MS`，
//    **不是配置键**；见 docs/runbook.md §10.3）。
//    > 0 时把**本实例的本地钟**向前拨该毫秒数（负值 = 向后拨）；未设置 / 空 / 非数字 → 0
//    （= 不注入，生产路径逐字节不变）。
//    为什么必须有它：ADR-009 §6.4/§8.1 的"实例钟必须在数据库钟的容忍范围内"这条判据，
//    在真实机器上**无法**用配置制造出可控偏差（改系统时间会波及整机，且测试没有 root）。
//    注入点选在**本地钟**（而不是只改比较那一行）：这样进程内的本地钟真的偏了，与
//    "一台钟走偏的实例"形态一致；横幅会显式打印注入值，避免演练结论被误读。
//    为什么不做成配置键（同 `FSS_STARTUP_FAULT_INJECT` / `FSS_CLAIM_HOLD_MS`）：
//      ① 三态清单（生效/拒绝启动/已读但无效果）由 `test_operations_doc` 与
//         `config/fss.example.json` **机械比对**，凭空加键会让计数失真；
//      ② "让实例钟故意走偏 N 毫秒"不是运维语义，生产上只会制造事故。
std::int64_t ClockSkewInjectMillisFromEnv() {
  const char* const value = std::getenv("FSS_CLOCK_SKEW_INJECT_MS");
  if (value == nullptr || *value == '\0') return 0;
  char* end = nullptr;
  const long long parsed = std::strtoll(value, &end, 10);
  if (end == value || *end != '\0') return 0;
  return static_cast<std::int64_t>(parsed);
}

//  本地钟装饰器：`NowEpochMillis/Seconds` 加上注入偏移；单调钟（用于耗时）**不偏移**
//  （偏移只该影响"墙上时间"，不该影响延迟测量）。
class OffsetClock final : public fss::IClock {
 public:
  OffsetClock(const fss::IClock& base, std::int64_t offset_millis)
      : base_(base), offset_millis_(offset_millis) {}

  std::int64_t NowEpochSeconds() const override {
    return base_.NowEpochSeconds() + offset_millis_ / 1000;
  }
  std::int64_t NowEpochMillis() const override {
    return base_.NowEpochMillis() + offset_millis_;
  }
  std::chrono::steady_clock::time_point NowSteady() const override {
    return base_.NowSteady();
  }

 private:
  const fss::IClock& base_;
  std::int64_t offset_millis_ = 0;
};

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
  bool once = false;
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
    } else if (arg == "--once") {
      out->once = true;
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

  //  动态子树（`auth.local_roles.<user>` / `partition.file.<partition>.*`）：schema 里
  //  只声明了 `AllowDynamicPrefix`，`Load` 不为这些键登记来源 → `SourceOf` 返回
  //  "unknown"。值确实来自 `effective()`（file/env/cli 已合并）；无法区分具体层时
  //  如实标为 `file`（比标成 `default` 更接近事实：默认值里没有这些键）。
  void NoteDynamic(const std::string& path, const std::string& value) {
    std::string source = cfg_.SourceOf(path);
    if (source.empty() || source == "unknown") source = "file";
    Record(path, value, Origin{false, value, source, {}});
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
//  动态子树：`auth.local_roles.<user>`（C10.15）
// =============================================================================
//  为什么不能用 `Resolver::Str/List`：schema 用 `AllowDynamicPrefix("auth.local_roles")`
//  放行该子树，但**没有逐键声明**，因此 `Load` 不为这些键登记来源 —— 只能从
//  `effective()` 里自己枚举。键名是邮箱，**不能**进 CoreSchema（否则 157 键的计数
//  与 `test_operations_doc` 的机械比对都会失真，见 operations.md §1.2）。
std::map<std::string, std::vector<std::string>, std::less<>> CollectLocalRoles(
    const fss::config::Config& cfg, Resolver& resolver) {
  std::map<std::string, std::vector<std::string>, std::less<>> out;
  const fss::json::Value* node = FindIn(cfg.effective(), "auth.local_roles");
  if (node == nullptr || !node->is_object()) return out;
  for (auto it = node->begin(); it != node->end(); ++it) {
    const std::string user = it.key();
    std::vector<std::string> roles;
    if (it.value().is_array()) {
      for (const auto& item : it.value()) {
        if (item.is_string()) roles.push_back(item.get<std::string>());
      }
    } else if (it.value().is_string()) {
      roles.push_back(it.value().get<std::string>());  // 单值写法也接受（`"a@b": "role"`）
    }
    if (user.empty() || roles.empty()) continue;
    out.emplace(user, roles);
    resolver.NoteDynamic("auth.local_roles." + user, Join(roles));
  }
  return out;
}

// =============================================================================
//  动态子树：`partition.file.<partition>.*`（C10.16）
// =============================================================================
//  与 `auth.local_roles.*` 同源：schema 用 `AllowDynamicPrefix("partition.file")` 放行，
//  但**没有逐键声明**，`Load` 不登记来源 → 只能从 `effective()` 自己枚举
//  （键名含 partition，不能进 CoreSchema，否则 157 键的计数会失真）。
//
//  ★ 只接**真实存在**的字段：`PartitionConfig` 的成员只有 `max_object_bytes`
//    （-1 = 不限）。容器名（`staging_container`/`persistent_container`）与
//    分区级 `storage_driver` 在 `PartitionConfig`/`ObjectKeyPolicy` 里**没有**对应字段
//    → 不发明，继续登记为"已读但无效果"（见 operations.md §1.3.3）。
struct PartitionFileOptions {
  std::string partition = "opendes";
  //  schema 语义：0 = 不限；`PartitionConfig::max_object_bytes` 用 -1 表示不限。
  std::int64_t max_file_bytes = 0;
  std::vector<std::string> allowed_checksum_algorithms = {"SHA-256", "SHA-1", "MD5"};
  std::string default_checksum_algorithm = "SHA-256";
  //  ★ 阶段 10（C10.16 续）：容器名与分区级驱动。空串 = 与接线前逐字一致的默认
  //    （`<partition>-staging` / `<partition>-persistent` / 跟随顶层 `storage.driver`）。
  std::string staging_container;
  std::string persistent_container;
  std::string storage_driver;
};

PartitionFileOptions ReadPartitionFileOptions(const fss::config::Config& cfg, Resolver& resolver) {
  PartitionFileOptions out;
  const std::string prefix = "partition.file." + out.partition + ".";
  if (const auto* v = FindIn(cfg.effective(), prefix + "max_file_bytes");
      v != nullptr && v->is_number_integer()) {
    const long n = v->get<long>();
    if (n >= 0) out.max_file_bytes = n;
    resolver.NoteDynamic(prefix + "max_file_bytes", std::to_string(out.max_file_bytes));
  }
  if (const auto* v = FindIn(cfg.effective(), prefix + "allowed_checksum_algorithms");
      v != nullptr && v->is_array()) {
    std::vector<std::string> items;
    for (const auto& e : *v) items.push_back(RenderScalar(e));
    if (!items.empty()) out.allowed_checksum_algorithms = items;
    resolver.NoteDynamic(prefix + "allowed_checksum_algorithms",
                         Join(out.allowed_checksum_algorithms));
  }
  if (const auto* v = FindIn(cfg.effective(), prefix + "default_checksum_algorithm");
      v != nullptr && v->is_string()) {
    out.default_checksum_algorithm = v->get<std::string>();
    resolver.NoteDynamic(prefix + "default_checksum_algorithm", out.default_checksum_algorithm);
  }
  //  ★ 阶段 10（C10.16 续）：容器名与分区级驱动 —— 这三条是**真正被使用**的（容器名）
  //    或**做启动期冲突判定**的（driver），不是只登记来源。
  if (const auto* v = FindIn(cfg.effective(), prefix + "staging_container");
      v != nullptr && v->is_string()) {
    out.staging_container = v->get<std::string>();
    resolver.NoteDynamic(prefix + "staging_container", out.staging_container);
  }
  if (const auto* v = FindIn(cfg.effective(), prefix + "persistent_container");
      v != nullptr && v->is_string()) {
    out.persistent_container = v->get<std::string>();
    resolver.NoteDynamic(prefix + "persistent_container", out.persistent_container);
  }
  if (const auto* v = FindIn(cfg.effective(), prefix + "storage_driver");
      v != nullptr && v->is_string()) {
    out.storage_driver = v->get<std::string>();
    resolver.NoteDynamic(prefix + "storage_driver", out.storage_driver);
  }
  return out;
}

//  ★ 阶段 10（切片 4）：`*.sqlite.synchronous` 的枚举 → `PRAGMA synchronous` 数值。
//  SQLite 的整数值是稳定的（OFF=0 / NORMAL=1 / FULL=2），schema 的枚举名才是给人看的。
//  非法值返回 -1，由调用方 → 拒绝启动（exit 78），绝不静默折成默认值。
int ParseSynchronousLevel(const std::string& text) {
  if (text == "OFF") return 0;
  if (text == "NORMAL") return 1;
  if (text == "FULL") return 2;
  return -1;
}

//  校验算法集合与默认算法：**按真实语义**只做启动期校验（R16：正例必须能通过）。
//    ① 集合里每个名字必须是我们真的支持的算法（SHA-256 / SHA-1 / MD5）；
//    ② 默认算法必须在这个集合里。
//  为什么不是"请求里声明非法算法 → 400"：C6.4 有上游一手证据 —— 客户端传入的
//  `Checksum`/`ChecksumAlgorithm` 是**被服务端覆写**的输入，不是待校验的断言
//  （`File_CorrectPayload.json` 声明 SHA-256 却给 MD5 值，期望响应是 201）。
//  返回空串 = 通过；否则是"拒绝启动"的原因。
std::string ValidatePartitionChecksums(const PartitionFileOptions& options) {
  std::vector<fss::crypto::ChecksumAlgorithm> allowed;
  for (const auto& name : options.allowed_checksum_algorithms) {
    const auto algorithm = fss::crypto::ParseChecksumAlgorithm(name);
    if (!algorithm.has_value()) {
      return "partition.file." + options.partition +
             ".allowed_checksum_algorithms 含未知算法：" + name +
             "（允许 SHA-256 | SHA-1 | MD5）";
    }
    allowed.push_back(*algorithm);
  }
  if (allowed.empty()) {
    return "partition.file." + options.partition +
           ".allowed_checksum_algorithms 不能为空（否则没有任何算法可用于校验和）";
  }
  const auto default_algorithm =
      fss::crypto::ParseChecksumAlgorithm(options.default_checksum_algorithm);
  if (!default_algorithm.has_value()) {
    return "partition.file." + options.partition +
           ".default_checksum_algorithm 是未知算法：" + options.default_checksum_algorithm +
           "（允许 SHA-256 | SHA-1 | MD5）";
  }
  if (std::find(allowed.begin(), allowed.end(), *default_algorithm) == allowed.end()) {
    return "partition.file." + options.partition + ".default_checksum_algorithm（" +
           options.default_checksum_algorithm + "）不在 allowed_checksum_algorithms 集合内";
  }
  return {};
}

//  ★ 阶段 10（C10.16 续）：容器名与分区级驱动的启动期校验。
//    · 容器名是**安全边界**（`ObjectKeyPolicy::ContainerFor` 会把它拼进对象路径）：
//      非法字符必须在启动期被拒，而不是等到请求期在 `fs::SafeJoin` 里失败 —— 那时
//      运维看到的是"上传 500"，与配置的因果关系太远。
//    · `storage_driver` 只能"跟随顶层"：组合根只装配**一个** BlobStore
//      （`SingleStoreFactory` 的所有 partition/zone 共用它），分区级覆盖做不到。
//      冲突时**拒绝启动**并说明，而不是静默忽略（R15：「写了但没生效」等于不存在）。
//    返回空串 = 通过（R16：默认/合法值必须能启动）。
std::string ValidatePartitionContainers(const PartitionFileOptions& options,
                                        const std::string& top_level_driver) {
  const auto check_name = [&](const std::string& name, const char* key) -> std::string {
    if (name.empty()) return {};  // 空 = 默认命名
    if (name == "." || name == "..") {
      return "partition.file." + options.partition + "." + key + " 不能是 \".\" / \"..\"";
    }
    for (const char c : name) {
      const auto u = static_cast<unsigned char>(c);
      if (u < 0x20 || u == 0x7F || c == '/' || c == '\\' || c == '%') {
        return "partition.file." + options.partition + "." + key +
               " 含非法字符（不允许控制字符 / '/' / '\\' / '%'）：" + name;
      }
    }
    return {};
  };
  if (const std::string problem = check_name(options.staging_container, "staging_container");
      !problem.empty()) {
    return problem;
  }
  if (const std::string problem = check_name(options.persistent_container, "persistent_container");
      !problem.empty()) {
    return problem;
  }
  if (!options.storage_driver.empty() && options.storage_driver != top_level_driver) {
    return "partition.file." + options.partition + ".storage_driver（" + options.storage_driver +
           "）与顶层 storage.driver（" + top_level_driver +
           "）冲突 —— 分区级驱动覆盖未交付（组合根只装配一个 BlobStore，"
           "SingleStoreFactory 的所有 partition/zone 共用它）。"
           "下一步：删除该键（跟随顶层 storage.driver），或等按 partition/zone 分盘的"
           "工厂交付后再接通。";
  }
  return {};
}

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

//  `events.publisher=none` → **显式关闭**事件发布（不是"没实现"）：
//  端口必须有实现（否则用例层要判空），但两个方法什么都不做、**不发任何请求**。
//  ★ 与 `NoopAuditLogger` 同风格；`publisher=webhook` 的空 URL 是**拒绝启动**，
//    而不是静默退化成 noop（运维必须显式写出 none 才能关掉事件）。
class NoopEventPublisher final : public fss::domain::IEventPublisher {
 public:
  fss::Result<void> PublishStatusChanged(
      std::string_view, const fss::domain::StatusChangedEvent&) override {
    return fss::Ok();
  }
  fss::Result<void> PublishDatasetDetails(
      std::string_view, const fss::domain::DatasetDetailsEvent&) override {
    return fss::Ok();
  }
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

//  ★ C10.13 的**故障注入接缝**（仅用于验证 fail-closed 路径；`FSS_AUDIT_FAULT_INJECT=1`
//    时装配）：一个"必然失败"的审计后端，让"审计写入失败 → 请求失败"这条路径在**真实
//    进程**上可被驱动。为什么不做成配置键：157 个叶子键的三态清单是机械比对的
//    （`test_operations_doc`），加键会让清单失真；而它本身也不是运维语义的一部分，
//    只是测试/演练用的故障开关（与 P9 的 mock_entitlements --fail-file 同族）。
class FailingAuditLogger final : public fss::domain::IAuditLogger {
 public:
  fss::Result<void> Record(const fss::domain::AuditEvent&) override {
    return fss::Err(fss::ErrorKind::kInternal,
                    "审计后端不可用（FSS_AUDIT_FAULT_INJECT=1 注入的必然失败）");
  }
};

// =============================================================================
//  GC 周期调度（阶段 10 切片 2 / C10.9）
// =============================================================================
//  为什么要有这个类：`GcTask` 从 P6 起就实现了语义，但**组合根从不装配它** ——
//  "测试里通过、产品里不存在"（R15 的同族陷阱）。这里把它接到真实进程的**后台线程**上。
//
//  ★ 优雅停止：`Stop()` 置位 + `notify_all` + `join`，且析构函数也调用 `Stop()`，
//    因此**任何提前 return 都不会留下 joinable 线程**（AGENTS §4.3 第一条陷阱）。
//    等待用 `condition_variable::wait_for`（不是固定 sleep）：SIGTERM 后立刻退出。
//  ★ 第一轮**立即执行**（不等一个 interval）：运维最需要的是"启动后马上有一轮结果"，
//    而不是等一小时。之后按 `gc.interval_seconds` 周期运行。
//  ★ B1：leader 门控。`interval_seconds <= 0` 时**不启动**：宁可"不跑并在横幅说明"，
//    也不要空转把机器压满。
//  ★ B1：`is_leader`（可空）= 领导者判定（组合根注入 `PgLeaderElection::IsLeader`）。
//    为空 = 未启用 leader election ⇒ 每个实例都跑（单实例语义，逐字不变）。
//    非 leader 时**在调用 `GcTask::Run` 之前**返回 —— 因此 `fss_gc_runs_total` 不增加
//    （这是"选举真的门控了 GC"的判据）。
class GcScheduler {
 public:
  GcScheduler(fss::app::GcTask& task, std::string partition, fss::app::GcOptions options,
              std::int64_t interval_seconds, const fss::logging::ILogger& logger,
              std::function<bool()> is_leader = {})
      : task_(task),
        partition_(std::move(partition)),
        options_(options),
        interval_seconds_(interval_seconds),
        logger_(logger),
        is_leader_(std::move(is_leader)) {}

  ~GcScheduler() { Stop(); }
  GcScheduler(const GcScheduler&) = delete;
  GcScheduler& operator=(const GcScheduler&) = delete;

  void Start() {
    thread_ = std::thread([this] { Loop(); });
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  std::int64_t runs() const { return runs_.load(); }

 private:
  void Loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
      lock.unlock();
      RunOnce();
      lock.lock();
      if (stop_) break;
      cv_.wait_for(lock, std::chrono::seconds(interval_seconds_), [this] { return stop_; });
    }
  }

  void RunOnce() {
    //  ★ B1：先做 leader 判定，再决定是否调用 `GcTask::Run`。
    //    非 leader **不调用** Run ⇒ `fss_gc_runs_total` 不增加（可观测判据）。
    if (is_leader_ && !is_leader_()) {
      fss::logging::Warn(logger_, "gc_skipped_not_leader",
                         {{"partition", partition_},
                          {"reason", "本实例不是 leader（leader_election.enabled=true）→ "
                                     "跳过本轮；fss_gc_runs_total 不增加"}});
      return;
    }
    const auto report = task_.Run(partition_, options_);
    runs_.fetch_add(1);
    if (!report.ok()) {
      fss::logging::Warn(logger_, "gc_run_failed",
                         {{"partition", partition_},
                          {"error", report.error().message()}});
      return;
    }
    const auto& value = report.value();
    fss::logging::Info(logger_, "gc_run",
                       {{"partition", partition_},
                        {"dry_run", value.dry_run ? "true" : "false"},
                        {"expired_leases_claimed", std::to_string(value.expired_leases_claimed)},
                        {"deleted_objects", std::to_string(value.deleted_objects)},
                        {"deleted_locations", std::to_string(value.deleted_locations)},
                        {"tmp_removed", std::to_string(value.tmp_removed)},
                        {"tmp_skipped_too_young", std::to_string(value.tmp_skipped_too_young)},
                        {"skipped_has_record", std::to_string(value.skipped_has_record)},
                        {"skipped_too_young", std::to_string(value.skipped_too_young)},
                        {"errors", std::to_string(value.errors)}});
  }

  fss::app::GcTask& task_;
  std::string partition_;
  fss::app::GcOptions options_;
  std::int64_t interval_seconds_ = 3600;
  const fss::logging::ILogger& logger_;
  //  ★ B1：空 = 不做 leader 门控（leader election 未启用）。
  std::function<bool()> is_leader_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::atomic<std::int64_t> runs_{0};
};

// =============================================================================
//  ★ B2b（ADR-009 §5.3 / §8.1 item 4）：实例心跳 + 配置/版本一致性 + 共享挂载探针
// =============================================================================
//  三条**固定参数**（钉住的设计：不新增配置键）：
//    · 心跳周期 10s；
//    · live 窗口 = 3 × 心跳周期 = 30s（`instance_registry.heartbeat_at` 在窗口内才算 live）；
//    · 过老行清理阈值 300s（= 10 × 窗口；远大于窗口，不会误删"只是晚了几拍"的实例）。
constexpr int kInstanceHeartbeatSeconds = 10;
constexpr int kInstanceLivenessSeconds = 3 * kInstanceHeartbeatSeconds;
constexpr int kInstanceStaleCleanupSeconds = 10 * kInstanceLivenessSeconds;
//  探针文件的过老清理阈值：只用于清掉崩溃实例留下的文件（不参与 liveness 判定）。
constexpr int kProbeStaleCleanupSeconds = 3600;

//  `config_hash` 的材料 = **脱敏后的有效配置**去掉"每实例本来就不同"的键，再 SHA-256。
//  为什么安全（不许出现明文密钥，也不许让哈希可反推密钥）：
//    ① 先走 `Config::RedactedDump()` —— 所有 `FieldSpec::Secret()` 字段（HMAC 密钥、
//       S3 secret、自签 signing_key、两个 PG DSN、remote static_token）都被替换成 `***`，
//       明文根本不会进入哈希函数；
//    ② 存的只是 SHA-256 摘要（64 位十六进制），不可逆，无法从中恢复任何配置值；
//    ③ 移除的键是**实例身份/监听地址/对外 URL**（同机多实例必须不同），它们不代表
//       "配置一致性"，否则同一部署的两个实例永远互相判为不一致。
//  产出的串会写进 `instance_registry.config_hash` 并打印在启动横幅（只截断展示）。
fss::Result<std::string> ComputeInstanceConfigHash(const fss::config::Config& cfg) {
  auto parsed = fss::json::Parse(cfg.RedactedDump());
  if (!parsed.ok()) {
    return fss::Err(fss::ErrorKind::kInternal,
                    "config_hash 计算失败：脱敏配置不是合法 JSON：" +
                        parsed.error().message());
  }
  fss::json::Value material = parsed.value();
  //  nlohmann 的对象键是有序的（std::map）⇒ `Dump` 对同一份配置产出确定串。
  static const char* const kInstanceLocalKeys[] = {
      "deployment.instance_id",       // 实例身份（multi 下还可能被自动生成）
      "server.http.bind",             // 监听地址（每实例可不同）
      "server.http.port",             // 监听端口（同机多实例必须不同）
      "server.grpc.bind",             // 同上
      "server.grpc.port",             // 同上
      "self_signed.public_base_url",  // 对外可达地址（内嵌每实例端口）
  };
  for (const char* key : kInstanceLocalKeys) {
    const std::string path(key);
    fss::json::Value* cursor = &material;
    std::size_t start = 0;
    while (true) {
      if (!cursor->is_object()) break;
      const auto dot = path.find('.', start);
      const std::string segment =
          path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
      if (dot == std::string::npos) {
        cursor->erase(segment);
        break;
      }
      auto it = cursor->find(segment);
      if (it == cursor->end()) break;
      cursor = &(*it);
      start = dot + 1;
    }
  }
  return fss::crypto::Sha256Hex(fss::json::Dump(material));
}

//  共享挂载失败的**可读原因**（启动期检查与运行时心跳共用同一份措辞）。
std::string MountVisibilityFailureReason(const fss::infra::SharedMountProbe& probe,
                                         const std::string& peer_id, int liveness_seconds) {
  return "共享挂载校验失败：实例 " + peer_id + " 是 live peer（心跳在 " +
         std::to_string(liveness_seconds) + "s 内），但其探针文件 " + probe.PathFor(peer_id) +
         " 在本实例的 storage.posix.root（" + probe.root() +
         "）下不可见 —— storage.posix.root 不是所有实例共享的挂载"
         "（能写自己的探针、却看不到别的实例的探针 = 各实例看到的是各自的本地目录）";
}

//  启动期交叉可见性检查：对**当前 live peer** 逐个验证探针可见性。
//  返回 live peer 数（0 = 尚无 live peer，无法证明共享性；由运行时心跳复查兜住）。
fss::Result<int> StartupMountVisibilityCheck(fss::infra::PgInstanceRegistry& registry,
                                             fss::infra::SharedMountProbe& probe) {
  FSS_TRY(peers, registry.ListLivePeers(kInstanceLivenessSeconds));
  for (const auto& peer : peers) {
    if (!probe.PeerProbeVisible(peer.instance_id)) {
      return fss::Err(fss::ErrorKind::kUnavailable,
                      MountVisibilityFailureReason(probe, peer.instance_id,
                                                   kInstanceLivenessSeconds));
    }
  }
  return static_cast<int>(peers.size());
}

//  B2b 的**运行时**判定：每 10s 刷新心跳、复查与每个 live peer 的一致性
//  （共享挂载可见性 / 服务版本兼容性 / config_hash），把"不 ready 的可读原因"
//  缓存在内存里。readiness 探针（`ports.shared_state_probe`）只读缓存 ——
//  与 B2a 的 PG 探针合成**同一个**判据（REST 与 gRPC 按构造一致）。
class InstanceConsistencyMonitor {
 public:
  InstanceConsistencyMonitor(fss::infra::PgInstanceRegistry& registry,
                             fss::infra::SharedMountProbe* probe,
                             std::string self_service_version, std::string self_config_hash)
      : registry_(registry),
        probe_(probe),
        self_service_version_(std::move(self_service_version)),
        self_config_hash_(std::move(self_config_hash)) {}
  ~InstanceConsistencyMonitor() { Stop(); }
  InstanceConsistencyMonitor(const InstanceConsistencyMonitor&) = delete;
  InstanceConsistencyMonitor& operator=(const InstanceConsistencyMonitor&) = delete;

  void Start() {
    if (thread_.joinable()) return;
    stop_.store(false);
    thread_ = std::thread([this] { Run(); });
  }
  //  幂等；**必须**在每条退出路径上跑到（析构也调用）—— 绝不留 joinable 线程。
  void Stop() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    wait_cv_.notify_all();
    thread_.join();
  }
  std::string NotReadyReason() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return reason_;
  }

 private:
  void Run() {
    Tick();  // 立即做一次：readiness 从**第一次**请求起就反映真实一致性
    std::unique_lock<std::mutex> lock(wait_mutex_);
    while (!stop_.load()) {
      if (wait_cv_.wait_for(lock, std::chrono::seconds(kInstanceHeartbeatSeconds),
                            [this] { return stop_.load(); })) {
        return;
      }
      lock.unlock();
      Tick();
      lock.lock();
    }
  }

  void Tick() {
    std::string reason = Evaluate();
    std::lock_guard<std::mutex> lock(state_mutex_);
    reason_ = std::move(reason);
  }

  std::string Evaluate() {
    //  ① 每次心跳刷新自己的探针文件：内容里的时间戳是**信息性**的（用系统墙钟即可，
    //     它不参与任何判定；mtime 才是清理依据），但刷新保证"活实例的探针不会因过老被清"。
    if (probe_ != nullptr) {
      const auto now_millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
      if (const auto written = probe_->WriteOwn(now_millis); !written.ok()) {
        return "共享挂载探针写入失败（readiness fail-closed）：" + written.error().message();
      }
    }
    //  ② 心跳：失败即 not ready（心跳写不进去 ⇒ 别的实例会误判本实例已死）。
    if (const auto touched = registry_.TouchHeartbeat(); !touched.ok()) {
      return "instance_registry 心跳失败（readiness fail-closed）：" +
             touched.error().message();
    }
    //  ③ live peer 一致性（顺序固定：可见性 → 版本 → config_hash）。
    const auto peers = registry_.ListLivePeers(kInstanceLivenessSeconds);
    if (!peers.ok()) {
      return "无法读取共享的 instance_registry（readiness fail-closed）：" +
             peers.error().message();
    }
    for (const auto& peer : peers.value()) {
      if (probe_ != nullptr && !probe_->PeerProbeVisible(peer.instance_id)) {
        return MountVisibilityFailureReason(*probe_, peer.instance_id, kInstanceLivenessSeconds);
      }
      std::string mismatch =
          fss::infra::PeerConsistencyReason(peer, self_service_version_, self_config_hash_);
      if (!mismatch.empty()) return mismatch;
    }
    return {};
  }

  fss::infra::PgInstanceRegistry& registry_;
  fss::infra::SharedMountProbe* probe_;  // 可空（shared_mount_required=false）
  std::string self_service_version_;
  std::string self_config_hash_;
  mutable std::mutex state_mutex_;
  std::string reason_;
  std::mutex wait_mutex_;
  std::condition_variable wait_cv_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

//  SIGINT / SIGTERM → 置位（**信号处理器里只做这一件事**：async-signal-safe）。
//  ★ 主循环轮询它（AGENTS §4.3："不要用固定 sleep 等状态"），随后走统一退出路径：
//    Stop GC 调度 → Stop HTTP → Shutdown gRPC。绝不留下 joinable 线程。
volatile std::sig_atomic_t g_stop_requested = 0;

extern "C" void HandleStopSignal(int) { g_stop_requested = 1; }

}  // namespace

// =============================================================================
//  RunServer —— 原来的 `main()` 全文（C9.32：整段搬进来，函数体逐字不变）
// =============================================================================
//  为什么要把函数体搬出来：`main()` 必须是**薄包装**，才能在它外面套一层
//  `try/catch` 把"未捕获异常 → terminate/139"变成"可读原因 + 确定退出码 70"。
//  这一层**不做任何清理**：`RunServer` 的局部对象（`GcScheduler`/`http::Server`/
//  gRPC 句柄/仓储/驱动）在栈展开时按 RAII 完成 stop/join/close（见测试
//  `throw_after_start` 的墙钟断言）。
static int RunServer(int argc, char** argv) {
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

  //  ---- 故障注入接缝的**早注入点**（测试/演练用；**不是配置键**）----
  //  ★ 位置：CLI 解析之后、任何资源装配之前（见 MaybeInjectStartupFaultEarly 的说明）。
  MaybeInjectStartupFaultEarly();

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
  //  ★ C10.15：`server.grpc.enabled` 真的参与判定（此前"读了但无效果"：只有
  //    `server.grpc.port=0` 能表达关闭）。两者是**与**关系：
  //      · `enabled=false` → 无论端口配成什么，都不开 gRPC 面；
  //      · `enabled=true` 且 `port=0` → 仍然关闭（`FSS_GRPC_PORT=0` 的既有语义不变）。
  const bool grpc_enabled = resolver.Bool("server.grpc.enabled", true);
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
  //  ★ 阶段 10（切片 4）：数据面 PUT 的**全局**请求体上限（0 = 不限）。真正的落点值还要与
  //    `partition.file.<p>.max_file_bytes` 取较小者（见下方 router_options 赋值处）。
  const long transfer_max_body_bytes =
      resolver.Int("server.http.transfer_max_body_bytes", 0);
  const std::string error_format = resolver.Str("http.error_format", "apperror");

  //  ---- 存储（storage.*，C10.4）----
  const std::string storage_driver = resolver.Str("storage.driver", "posix");
  const std::string storage_root = resolver.Str("storage.posix.root", "/tmp/fss-data");
  //  ★ B2b（ADR-009 §8.1 item 4）：共享挂载探针开关。`true` 时组合根会写
  //    `<storage.posix.root>/.fss_probe.<instance_id>` 并与 `instance_registry` 的
  //    live peer 交叉比对可见性（`multi` 由 schema 强制为 true）。
  const bool shared_mount_required = resolver.Bool("storage.posix.shared_mount_required", false);
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

  //  ---- 仓储（ADR-004：单实例 = 内置 SQLite；B1 起 multi = PG）----
  const std::string metadata_repository_name = resolver.Str("metadata.repository", "sqlite");
  const std::string location_repository_name = resolver.Str("location.repository", "sqlite");
  //  ---- B1：PG 仓储的连接参数（`*.postgres.*`）----
  //  ★ 逐键来源（默认值 = `core_schema.cpp` / `config/fss.example.json`）：
  //    `metadata.postgres.{dsn,max_connections,statement_timeout_ms}`（3 个键 → 生效）
  //    `location.postgres.{dsn,max_connections}`（2 个键 → 生效）
  //  ★ `location.postgres.*` 组**没有** `statement_timeout_ms` 键 —— 该组用
  //    `PgOptions` 的默认值（5000ms），不在这里发明一个配置键。
  //  ★ dsn 是 secret：绝不打印（横幅只打印"用的是 postgres"与连接数）。
  const std::string metadata_postgres_dsn = resolver.Str("metadata.postgres.dsn", "");
  const long metadata_postgres_max_connections =
      resolver.Int("metadata.postgres.max_connections", 16);
  const long metadata_postgres_statement_timeout_ms =
      resolver.Int("metadata.postgres.statement_timeout_ms", 5000);
  const std::string location_postgres_dsn = resolver.Str("location.postgres.dsn", "");
  const long location_postgres_max_connections =
      resolver.Int("location.postgres.max_connections", 8);
  //  ★ B2a：readiness 的迁移版本校验开关（`metadata.postgres.schema_version_check`）。
  //    `true`（默认）→ readiness 除 `SELECT 1` 存活探针外，还比对
  //    `schema_migrations.max(version)` 与 `infra::kExpectedSchemaVersion`；
  //    `false` → **只**做存活探针（跳过版本比对）。
  //    落点：`ports.shared_state_probe`（组合根注入的 PG 连接池探针）→ REST
  //    `/v2/readiness_check` 与 gRPC `Check(PROBE_READINESS)` 同源。
  const bool metadata_postgres_schema_version_check =
      resolver.Bool("metadata.postgres.schema_version_check", true);
  const std::string metadata_db_path =
      resolver.Str("metadata.sqlite.path", storage_root + "/metadata.db");
  const std::string sqlite_path =
      resolver.Str("location.sqlite.path", storage_root + "/location.db");
  //  ---- C10.14 / 切片 4：SQLite 调优键（只接**真实存在**的 Options 字段）----
  //  ★ 按 `SqliteMetadataRepositoryOptions` / `SqliteLocationRepositoryOptions` 的字段接线；
  //    切片 4 给两个结构体补齐了 `journal_mode`（`wal` 布尔）与 `synchronous`
  //    （`synchronous_level`），因此这四个键从「已读但无效果」移入「生效」。
  //    **本切片（C10.20）**把 `group_commit*`（6 个键）也接通（真组提交实现）——
  //    仍未接的只剩 `max_write_concurrency`（单连接 + 互斥，实际并发恒为 1）—— 见 §1.3.3。
  const long metadata_sqlite_busy_timeout_ms =
      resolver.Int("metadata.sqlite.busy_timeout_ms", 5000);
  const long location_sqlite_busy_timeout_ms =
      resolver.Int("location.sqlite.busy_timeout_ms", 5000);
  const std::string metadata_sqlite_journal_mode =
      resolver.Str("metadata.sqlite.journal_mode", "WAL");
  const std::string location_sqlite_journal_mode =
      resolver.Str("location.sqlite.journal_mode", "WAL");
  //  enum NORMAL / FULL / OFF → PRAGMA synchronous 的 1 / 2 / 0（非法 → -1 → exit 78）
  const std::string metadata_sqlite_synchronous_text =
      resolver.Str("metadata.sqlite.synchronous", "NORMAL");
  const std::string location_sqlite_synchronous_text =
      resolver.Str("location.sqlite.synchronous", "NORMAL");
  const int metadata_sqlite_synchronous = ParseSynchronousLevel(metadata_sqlite_synchronous_text);
  const int location_sqlite_synchronous = ParseSynchronousLevel(location_sqlite_synchronous_text);
  //  `max_write_concurrency`：字段存在，但当前实现是"单连接 + 互斥"（实际并发 1），
  //  取值不改变行为 → 仍如实登记为"已读但无效果"（见 §1.3.3），这里只把值传进 Options。
  const long location_sqlite_max_write_concurrency =
      resolver.Int("location.sqlite.max_write_concurrency", 8);
  //  ---- C10.20（本切片）：数据库层组提交（6 个键）----
  //  ★ 逐键来源（默认值 = `core_schema.cpp` / `config/fss.example.json`）：
  //    `metadata.sqlite.{group_commit,group_commit_max_wait_ms,group_commit_max_batch}`
  //      → `SqliteMetadataRepositoryOptions`（下游 = 共用协调器 `SqliteGroupCommitOptions`）
  //    `location.sqlite.{group_commit,group_commit_max_wait_ms,group_commit_max_batch}`
  //      → `SqliteLocationRepositoryOptions`（同一个共用协调器）
  //  ★ 语义（详见 `src/infra/sqlite/sqlite_group_commit.h`）：`group_commit=true` 时并发
  //    写操作凑成一批、一个事务一次提交（每操作 SAVEPOINT 保原子性）；`false` 时逐操作
  //    提交（与接线前逐字一致）。
  //  ⚠️ **代价**：批事务期间领队持有连接互斥 ⇒ **读可能多等 ≤ max_wait_ms**
  //    （写进 `docs/operations.md` 与 `docs/02-design.md`，不让运维以为是免费收益）。
  const bool metadata_sqlite_group_commit = resolver.Bool("metadata.sqlite.group_commit", true);
  const long metadata_sqlite_group_commit_max_wait_ms =
      resolver.Int("metadata.sqlite.group_commit_max_wait_ms", 5);
  const long metadata_sqlite_group_commit_max_batch =
      resolver.Int("metadata.sqlite.group_commit_max_batch", 64);
  const bool location_sqlite_group_commit = resolver.Bool("location.sqlite.group_commit", true);
  const long location_sqlite_group_commit_max_wait_ms =
      resolver.Int("location.sqlite.group_commit_max_wait_ms", 5);
  const long location_sqlite_group_commit_max_batch =
      resolver.Int("location.sqlite.group_commit_max_batch", 64);

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
  //  ★ 阶段 10 切片 5：`self_signed.key_id` 与自签分支的 TTL 上界。
  //    · `key_id` → `HmacTransferTokenCodec`：非空时进**被签名的载荷**，解码侧要求一致
  //      （fail-closed）。**不打印密钥**；多密钥轮换未交付（只做标识绑定）。
  //    · `{default,max}_ttl_seconds` → `LocationIssuer` 的**自签分支上界**（`expiry.*` 仍是
  //      `expiryTime` 参数的解析规则与缺省；见 `SelfSignedTtlOptions` 的理由）。
  const std::string transfer_key_id = resolver.Str("self_signed.key_id", "k1");
  const long self_signed_default_ttl_seconds =
      resolver.Int("self_signed.default_ttl_seconds", 3600);
  const long self_signed_max_ttl_seconds = resolver.Int("self_signed.max_ttl_seconds", 604800);

  //  ---- 有效期（expiry.*，C10.12）----
  //  ★ 判定规则不变（缺省 / 静默夹紧 / 非法固定消息），配置只改**缺省值**与**上限基数**；
  //    上限本身用 `ParseExact`（**不夹紧**），否则"把上限配成 30D"会被静默改成 7D。
  const std::string expiry_default_text = resolver.Str("expiry.default", "1H");
  const std::string expiry_max_text = resolver.Str("expiry.max", "7D");

  //  ---- GC（gc.*，C10.9）----
  const bool gc_enabled = resolver.Bool("gc.enabled", false);
  const bool gc_dry_run = resolver.Bool("gc.dry_run", true);
  const bool gc_require_lease_expiry = resolver.Bool("gc.require_lease_expiry", true);
  const long gc_staging_ttl_hours = resolver.Int("gc.staging_ttl_hours", 24);
  const long gc_orphan_grace_hours = resolver.Int("gc.orphan_grace_hours", 72);
  const long gc_interval_seconds = resolver.Int("gc.interval_seconds", 3600);

  //  ---- 未实现能力的守卫键（C10.11：非默认值必须**拒绝启动**，不许静默无效）----
  const bool large_file_plane_enabled =
      resolver.Bool("server.http.large_file_plane.enabled", false);
  const long max_connections_per_partition =
      resolver.Int("server.http.max_connections_per_partition", 0);
  const long grpc_max_message_bytes = resolver.Int("server.grpc.max_message_bytes", 4194304);
  const long grpc_streaming_chunk_bytes =
      resolver.Int("server.grpc.streaming_chunk_bytes", 262144);
  const std::string proxy_mode = resolver.Str("storage.proxy_mode", "auto");
  const bool io_uring_register_files = resolver.Bool("storage.io_uring.register_files", false);
  const bool leases_enabled = resolver.Bool("leases.enabled", false);
  const long leases_ttl_seconds = resolver.Int("leases.ttl_seconds", 60);
  const long leases_renew_interval_seconds =
      resolver.Int("leases.renew_interval_seconds", 20);
  const std::string leases_time_source = resolver.Str("leases.time_source", "database");
  //  ---- B1：领导者选举（`leader_election.*`，3 个键 → 生效）----
  //  ★ `enabled` 不再是"未实现 → exit 78"的守卫：它真的决定是否创建
  //    `PgLeaderElection` 并用它门控 GC（见本文件 §④ 与 GcScheduler/GcCallbacks）。
  //  ★ `backend` 只有一个枚举值 `postgres_advisory_lock`：组合根**校验**它并把它
  //    写进横幅（schema 已拒绝其它取值；这里的 else 是防御性的第二道）。
  //  ★ `lock_key` 真的交给 `pg_try_advisory_lock($1::bigint)`。
  const bool leader_election_enabled = resolver.Bool("leader_election.enabled", false);
  const std::string leader_election_backend =
      resolver.Str("leader_election.backend", "postgres_advisory_lock");
  const long leader_election_lock_key = resolver.Int("leader_election.lock_key", 1179865927);
  //  ---- 阶段 10 切片 6a：远端 legal / schema 校验器（ADR-013）----
  //  ★ `*.remote.base_url` 就是**完整端点 URL**（POST 到它，不追加路径）—— 与
  //    `auth.remote_entitlements` 不同（后者另有 `authorize_path` 键）。理由：
  //    legal/schema 没有 path 键，"与其发明一个路径，不如把控制权交给运维"（ADR-013 §2）。
  //  ★ 非法取值由 schema 的 enum 拒绝（`core_schema.cpp` 已声明 `Enum({"noop","remote"})`）。
  const std::string legal_validator_kind = resolver.Str("legal.validator", "noop");
  const std::string legal_remote_base_url = resolver.Str("legal.remote.base_url", "");
  const long legal_remote_timeout_ms = resolver.Int("legal.remote.timeout_ms", 3000);
  const std::string schema_validator_kind = resolver.Str("schema.validator", "noop");
  const std::string schema_remote_base_url = resolver.Str("schema.remote.base_url", "");
  const long schema_remote_timeout_ms = resolver.Int("schema.remote.timeout_ms", 3000);
  const std::string events_publisher = resolver.Str("events.publisher", "log");
  //  ---- 阶段 10 切片 6b：事件发布器（ADR-013 §9）----
  //  ★ `events.webhook.url` 就是**完整端点 URL**（POST 到它，不追加路径）——与
  //    切片 6a 的 `*.remote.base_url` **同一约定**（ADR-013 §2）：没有上游路径依据时，
  //    把完整 URL 的控制权交给运维。
  //  ★ 事件发布是**非致命**的（上游只 `log.warning`）：webhook 失败绝不让请求失败。
  const std::string events_webhook_url = resolver.Str("events.webhook.url", "");
  const long events_webhook_timeout_ms = resolver.Int("events.webhook.timeout_ms", 3000);
  const std::string events_webhook_topic =
      resolver.Str("events.webhook.topic", "status-changed");
  const bool single_use_nonce = resolver.Bool("self_signed.single_use_nonce", false);
  const std::string nonce_store = resolver.Str("self_signed.nonce_store", "memory");
  const std::string partition_registry = resolver.Str("partition.registry", "file");
  const long clock_skew_tolerance_seconds =
      resolver.Int("deployment.clock_skew_tolerance_seconds", 60);
  //  ★ B2a（C9.28）：部署在该 PG 上的实例数（含本实例）。启动期连接预算的因子。
  const long expected_instances = resolver.Int("deployment.expected_instances", 1);

  //  ---- 阶段 10（C10.16 续）：`storage.posix.*` 细节键 ----
  //  ★ 5 个**真接通**（驱动层有对应字段），2 个只做拒绝启动守卫（批提交协议未实现）。
  const long posix_group_commit_max_batch =
      resolver.Int("storage.posix.group_commit_max_batch", 500);
  const bool posix_sync_dir_after_batch =
      resolver.Bool("storage.posix.sync_dir_after_batch", true);
  const bool posix_atomic_write = resolver.Bool("storage.posix.atomic_write", true);
  const std::string posix_dir_mode_text = resolver.Str("storage.posix.dir_mode", "0750");
  const std::string posix_file_mode_text = resolver.Str("storage.posix.file_mode", "0640");
  const bool posix_fadvise_random = resolver.Bool("storage.posix.fadvise_random", true);
  const bool posix_fadvise_dontneed =
      resolver.Bool("storage.posix.fadvise_dontneed_after_large_read", false);
  //  权限位是**八进制**字符串（schema 类型是 string，如 "0750"）。非法形态必须拒绝启动，
  //  不能静默退回默认值（否则运维会以为生效了）。
  const auto parse_octal_mode = [](const std::string& text) -> long {
    if (text.empty()) return -1;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 8);
    if (end == text.c_str() || *end != '\0' || value < 0 || value > 07777) return -1;
    return value;
  };
  const long posix_dir_mode = parse_octal_mode(posix_dir_mode_text);
  const long posix_file_mode = parse_octal_mode(posix_file_mode_text);
  if (posix_dir_mode < 0 || posix_file_mode < 0) {
    std::cerr << "拒绝启动：storage.posix.dir_mode / file_mode 必须是八进制权限位"
                 "（如 \"0750\" / \"0640\"）；实际 dir_mode=\"" << posix_dir_mode_text
              << "\"，file_mode=\"" << posix_file_mode_text << "\"。\n";
    return kExitConfigError;
  }

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
  //  ★ C10.15：`auth.jwt.roles_claim`（默认 `roles`）与 `auth.local_roles.*`（静态角色表）
  //    真的接到 `LocalJwtOptions` —— 此前两者都是"已读但无效果"（固定 `roles`、
  //    静态表未装配），于是"配置里的用户 → 角色"完全不参与 200/403 判定。
  const std::string jwt_roles_claim = resolver.Str("auth.jwt.roles_claim", "roles");
  const auto local_roles = CollectLocalRoles(cfg, resolver);
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
  //  ★ C10.13：`audit_fail_closed` **行为已实现** —— `true` 时审计写入失败会让请求
  //    以 500 结束（契约 §5 的 `kInternal`），`false`（默认）保持接线前的非致命语义。
  //    判定在用例层（`UseCasePorts::audit_fail_closed`），见 usecases.cpp 的 RecordAudit。
  const bool audit_fail_closed = resolver.Bool("observability.audit_fail_closed", false);
  //  故障注入接缝（测试/演练用；不是配置键，见 FailingAuditLogger 的说明）
  const bool audit_fault_inject = Env("FSS_AUDIT_FAULT_INJECT", "") == "1";
  const bool metrics_enabled = resolver.Bool("observability.metrics_enabled", true);
  const std::string metrics_path = resolver.Str("observability.metrics_path", "/metrics");

  if (cli.print_config) {
    std::cout << "配置来源 : "
              << (config_path.empty() ? "无配置文件（仅环境变量）" : config_path) << "\n";
    resolver.Print(std::cout);
    return 0;
  }

  //  ---- C10.11：未实现能力的非默认值 → 拒绝启动（"未实现 + 下一步"）----
  //  ★ 这一节的存在理由是"不许读了但静默无效"：如果一个键的实现不存在，把非默认值
  //    接受下来就是让运维**以为配置生效了**。宁可拒绝启动，并给出下一步。
  //    每条都配一个正例对照（默认值必须能启动）—— 见 tests/integration/test_gc_scheduling.cpp。
  const auto reject_startup = [](const std::string& message) {
    std::cerr << "拒绝启动：" << message << "\n";
    return kExitConfigError;
  };

  if (large_file_plane_enabled) {
    return reject_startup(
        "server.http.large_file_plane.enabled=true —— 独立大文件数据面（sendfile）尚未交付"
        "（ADR-006 只定稿方向，实现未交付）。下一步：保持 false（默认，走 httplib 内容提供者），"
        "或在 ADR-006 §6 落地后开启。");
  }
  if (max_connections_per_partition != 0) {
    return reject_startup(
        "server.http.max_connections_per_partition 非 0 —— 每租户并发上限尚未实现"
        "（当前只有全局 max_connections）。下一步：保持 0，或用 "
        "server.http.max_connections 表达全局上限。");
  }
  if (grpc_max_message_bytes != 4194304 || grpc_streaming_chunk_bytes != 262144) {
    return reject_startup(
        "server.grpc.max_message_bytes / server.grpc.streaming_chunk_bytes 非默认 —— "
        "gRPC 侧消息上限尚未接通（实现内固定 4 MiB / 256 KiB）。下一步：保持默认值，"
        "或先接通 gRPC 适配层的 options。");
  }
  if (proxy_mode == "always") {
    return reject_startup(
        "storage.proxy_mode=always —— 「强制服务代理所有字节」尚未实现"
        "（地址形态当前只按驱动能力决定，见 LocationIssuer）。下一步：保持 auto。");
  }
  if (io_uring_register_files) {
    return reject_startup(
        "storage.io_uring.register_files=true —— io_uring 引擎本身未启用（ADR-010 U1~U4），"
        "注册文件表更不可能生效。下一步：保持 false，并先跑 scripts/check_io_uring.sh。");
  }
  //  ★ B1：`leases.enabled` / `leader_election.enabled` **不再是**这里的"未实现 → 拒绝启动"
  //    守卫 —— 它们已经是**生效**键：分别决定"用 PG 租约还是内存租约"与"是否做 leader
  //    选举并门控 GC"。真正打开它们需要可用 PG（见 §④ 的创建失败 → exit 78）。
  if (single_use_nonce) {
    return reject_startup(
        "self_signed.single_use_nonce=true —— nonce 存储未交付（ADR-009 M5：本地表无法跨实例）。"
        "下一步：保持 false；多实例防重放需要共享 nonce 存储。");
  }
  if (nonce_store != "memory") {
    return reject_startup(
        "self_signed.nonce_store=" + nonce_store +
        " —— 只有内存 nonce 存储存在，且它在 single_use_nonce=false 时并不被使用。"
        "下一步：保持 memory，并在实现 PG nonce 表后再启用 single_use_nonce。");
  }
  if (partition_registry == "remote") {
    return reject_startup(
        "partition.registry=remote —— 远端租户注册表未交付（组合根只装配内置租户 opendes）。"
        "下一步：保持 file，或先实现 partition.registry=remote 的拉取与校验。");
  }
  //  ★ B2a：`deployment.clock_skew_tolerance_seconds` **不再**是"未实现 → 拒绝启动"的守卫
  //    —— 它现在是**生效**键：组合根在启动期用它比对「本地钟 vs PG now()」，超限 exit 78
  //    （见本文件 §④ 之后的"B2a 生产就绪检查"）。非默认值因此被真正使用，而不是被拒。
  //  ★ 本轮（ADR-008 的 P4 已交付）：`group_commit_max_batch` **生效**（不再拒绝启动）；
  //    `sync_dir_after_batch=false` 仍然**拒绝启动** —— ADR-008 §5 的 R2（"rename 之后
  //    必须 fsync 目录"）是**不变量**，schema 描述也写明"必须 true"。为了多一个"生效"
  //    键而放宽这条护栏，等于允许"已确认的对象在崩溃后消失"（AGENTS §4.3：
  //    机械护栏挡住架构上必须存在的代码时，不要放宽护栏）。
  if (!posix_sync_dir_after_batch) {
    return reject_startup(
        "storage.posix.sync_dir_after_batch=false —— ADR-008 §5 的 R2 是**不变量**："
        "rename 之后必须 fsync(目录)，否则「已确认」的对象在崩溃后可能消失"
        "（schema 描述也写明该键必须为 true）。下一步：保持 true。");
  }
  //  ★ 切片 4：两个仓储的 journal_mode 只接通 WAL / DELETE 两档（Options 里是 `wal` 布尔）。
  //    TRUNCATE 若被静默当成 DELETE，运维会得到"配置写了 TRUNCATE、实际是 DELETE"的假象。
  if (location_sqlite_journal_mode != "WAL" && location_sqlite_journal_mode != "DELETE") {
    return reject_startup(
        "location.sqlite.journal_mode=" + location_sqlite_journal_mode +
        " —— 组合根只接通 WAL | DELETE 两档（SQLite Options 里是 `wal` 布尔；TRUNCATE 未接通）。"
        "下一步：保持 WAL（默认）或 DELETE；需要 TRUNCATE 时先在仓储 Options 上显式加字段。");
  }
  if (metadata_sqlite_journal_mode != "WAL" && metadata_sqlite_journal_mode != "DELETE") {
    return reject_startup(
        "metadata.sqlite.journal_mode=" + metadata_sqlite_journal_mode +
        " —— 组合根只接通 WAL | DELETE 两档（SQLite Options 里是 `wal` 布尔；TRUNCATE 未接通）。"
        "下一步：保持 WAL（默认）或 DELETE；需要 TRUNCATE 时先在仓储 Options 上显式加字段。");
  }
  //  ★ 切片 4：`PRAGMA synchronous` 只接通 OFF / NORMAL / FULL（映射到 0 / 1 / 2）。
  //    非法值绝不静默折成默认值（否则运维以为配了 FULL，实际是 NORMAL）。
  if (metadata_sqlite_synchronous < 0) {
    return reject_startup(
        "metadata.sqlite.synchronous=" + metadata_sqlite_synchronous_text +
        " —— 只接通 OFF | NORMAL | FULL（映射到 PRAGMA synchronous 的 0 | 1 | 2）。"
        "下一步：改成三者之一（默认 NORMAL）。");
  }
  if (location_sqlite_synchronous < 0) {
    return reject_startup(
        "location.sqlite.synchronous=" + location_sqlite_synchronous_text +
        " —— 只接通 OFF | NORMAL | FULL（映射到 PRAGMA synchronous 的 0 | 1 | 2）。"
        "下一步：改成三者之一（默认 NORMAL）。");
  }

  //  ---- 有效期（expiry.*，C10.12）：把两个字符串解析成基数 ----
  const auto expiry_default = app::ExpiryPolicy::ParseExact(expiry_default_text);
  const auto expiry_max = app::ExpiryPolicy::ParseExact(expiry_max_text);
  if (!expiry_default.ok() || !expiry_max.ok()) {
    std::cerr << "拒绝启动：expiry.default / expiry.max 必须是 <数字><M|H|D> 形态"
                 "（实际 expiry.default=\"" << expiry_default_text << "\"，expiry.max=\""
              << expiry_max_text << "\"）。\n";
    return kExitConfigError;
  }
  if (expiry_default.value() > expiry_max.value()) {
    std::cerr << "拒绝启动：expiry.default（" << expiry_default_text
              << " = " << expiry_default.value() << "s）不得大于 expiry.max（"
              << expiry_max_text << " = " << expiry_max.value() << "s）。\n";
    return kExitConfigError;
  }
  const app::ExpiryOptions expiry_options{expiry_default.value(), expiry_max.value()};
  //  ★ 阶段 10 切片 5：自签分支的 TTL 上界（默认值与 `expiry.*` 的 schema 默认相同 ⇒
  //    默认配置下行为与接线前逐字一致；`expiry.*` 的语义**不变**）。
  const app::SelfSignedTtlOptions self_signed_ttl_options{
      self_signed_default_ttl_seconds, self_signed_max_ttl_seconds};

  //  ---- 值域/依赖关系校验：任何一条不过 → 拒绝启动（exit 78）----
  //  ★ B1：`deployment.mode=multi` **不再**在这里被一刀切拒绝。multi 的 7 条跨字段校验
  //    （PG 仓储 / PG 租约 / leader election / shared_mount_required / require_lease_expiry /
  //    clock skew）已经在 `core_schema.cpp` 里先行执行；本文件负责**真的把运行形态装配起来**：
  //      · 本构建没有 libpq → 下面的 `#ifndef FSS_HAVE_LIBPQ` 给出可执行的修复指令；
  //      · 有 libpq → §④ 创建 PG 仓储 + PG 租约 + leader election，任一失败 → exit 78。
  if (metadata_repository_name != "sqlite" && metadata_repository_name != "postgres") {
    std::cerr << "拒绝启动：metadata.repository 只支持 sqlite | postgres（当前="
              << metadata_repository_name
              << "）；remote 元数据仓储尚未交付（ADR-004/ADR-009）。\n";
    return kExitConfigError;
  }
  if (location_repository_name != "sqlite" && location_repository_name != "postgres") {
    std::cerr << "拒绝启动：location.repository 只支持 sqlite | postgres（当前="
              << location_repository_name << "）。\n";
    return kExitConfigError;
  }
  //  ★ `leader_election.backend` 只有一个合法值；schema 的 enum 已拒绝其它取值，
  //    这里是防御性的第二道（枚举被改宽时不会静默降级成"不做选举"）。
  if (leader_election_enabled && leader_election_backend != "postgres_advisory_lock") {
    std::cerr << "拒绝启动：leader_election.backend=" << leader_election_backend
              << " 未实现（当前只有 postgres_advisory_lock）。\n";
    return kExitConfigError;
  }
#ifndef FSS_HAVE_LIBPQ
  //  ★ fail-closed 的**可执行**路径：本二进制在构建时没找到 libpq。绝不静默回退到 SQLite /
  //    内存（那正是 ADR-009 §4.1 禁止的"以为共享、其实各存一份"）。
  {
    const bool wants_pg = deployment_mode == "multi" ||
                          metadata_repository_name == "postgres" ||
                          location_repository_name == "postgres" || leases_enabled ||
                          leader_election_enabled;
    if (wants_pg) {
      std::cerr
          << "拒绝启动：本配置需要 PostgreSQL（deployment.mode=" << deployment_mode
          << "，metadata.repository=" << metadata_repository_name
          << "，location.repository=" << location_repository_name
          << "，leases.enabled=" << (leases_enabled ? "true" : "false")
          << "，leader_election.enabled=" << (leader_election_enabled ? "true" : "false")
          << "），但本二进制在构建时**未找到 libpq**。\n"
             "  下一步：安装 libpq 开发文件（Debian/Ubuntu: `apt-get install libpq-dev`，"
             "或从镜像 `apt-get download libpq-dev` + `dpkg-deb -x`），然后重新运行 "
             "`cmake -S . -B build` 并重编 —— libpq 由 `find_package(PostgreSQL)` 自动发现，"
             "**不需要** `-DFSS_WITH_PG=ON`（后者只用于注册 PG 测试）。\n"
             "  绝不静默回退到 SQLite/内存：那会让每个实例各存一份状态。\n";
      return kExitConfigError;
    }
  }
#endif
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
  //  ★ B2a：本地钟 = `SystemClock` + **测试专用**的偏移注入（`FSS_CLOCK_SKEW_INJECT_MS`）。
  //    未设置（默认）时偏移 = 0 ⇒ 行为与接线前**逐字一致**（每次读多一次虚调用与加法）。
  //    注入生效时横幅显式打印，避免演练结论被误读成"这台机器钟真的偏了"。
  SystemClock system_clock;
  const std::int64_t clock_skew_inject_millis = ClockSkewInjectMillisFromEnv();
  OffsetClock clock(system_clock, clock_skew_inject_millis);
  const auto log_level = logging::ParseLevel(log_level_text).value_or(logging::Level::kInfo);
  logging::StreamLogger logger(
      logging::OptionsFromConfig(log_level, log_format, log_service, Join(redact_keys)), clock);
  UuidGenerator ids;

  //  ★ B1 / ADR-009 §4.5（M1）：`deployment.instance_id` 的 multi 语义。
  //    schema 默认是 `""`，组合根历史默认是 `"local"` —— **两者都不是唯一标识**。
  //    多实例共享同一标识 + 共享挂载 ⇒ 临时文件名确定性撞名 ⇒ 静默串数据
  //    （ADR-009 §3 M1 实测 40 次里 21 次）。因此：
  //      · multi 且**未显式配置**（有效值 = `local`）或显式为空 → 用组合根已有的
  //        UUID 生成器自动生成唯一 id，并在横幅 + 日志里打印实际生效值与警告；
  //      · multi 且显式配置了非默认值 → 原样使用（运维注入 POD_NAME 的场景）；
  //      · single → **逐字不变**（空仍是空、`local` 仍是 `local`），绝不凭空造 id。
  std::string effective_instance_id = posix_instance_id;
  bool instance_id_generated = false;
  if (deployment_mode == "multi" &&
      (effective_instance_id.empty() || effective_instance_id == "local")) {
    effective_instance_id = ids.NewUuid();
    instance_id_generated = true;
    logging::Warn(logger,
                  "deployment.instance_id 未配置（有效值 local）或为空 → 在 multi 模式下"
                  "已自动生成唯一实例 id；默认 local 会让多实例共享标识，导致共享挂载上的"
                  "临时文件确定性撞名（ADR-009 M1：静默串数据）",
                  {{"component", "server_main"},
                   {"deployment.mode", deployment_mode},
                   {"instance_id", effective_instance_id}});
  }

  //  ★ 注册表要**早于** blob store 构造：POSIX 驱动的批提交（ADR-008 的 P4）要把
  //    `syncfs`/批次数记进 `fss_*` 指标族（否则运维无法观察摊销是否发生）。
  metrics::Registry metrics_registry;

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
      //  ★ ADR-008 的 P4：**真批提交**（并发驱动的组提交）。
      //    此前的映射是近似 —— `kBySize` 的语义是"小文件不 fsync，靠批提交摊销"，
      //    而批提交根本不存在 ⇒ 默认配置下小文件**从不落盘**（耐久性谎言）。
      //    现在 `batch_commit=true` 打开「写整批 tmp → syncfs → 统一 rename →
      //    fsync(dir)」，`fsync_policy` 只用来表达"≥ 阈值的对象强制单独 fdatasync"。
      posix_options.batch_commit = true;
      posix_options.fsync_policy = FsyncPolicy::kBySize;
      posix_options.fsync_threshold_bytes = fsync_threshold_bytes;
      posix_options.group_commit_max_batch =
          static_cast<std::size_t>(posix_group_commit_max_batch);
      posix_options.sync_dir_after_batch = posix_sync_dir_after_batch;
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
    //    `deployment.instance_id`；multi 下未配置/为空时用的是上面**自动生成的**
    //    `effective_instance_id`（默认 `local` 正是 M1 的暴露面，见其说明）。
    posix_options.instance_id = effective_instance_id;
    //  ★ 阶段 10（C10.16 续）：`storage.posix.*` 细节键 → 驱动层 Options。
    //    `atomic_write=false` 走"直接写目标文件"分支（失败时删除目标，不留半成品）；
    //    `dir_mode`/`file_mode` 是显式权限位；两个 fadvise 在读写路径上真的下发提示。
    posix_options.atomic_write = posix_atomic_write;
    posix_options.dir_mode = static_cast<decltype(posix_options.dir_mode)>(posix_dir_mode);
    posix_options.file_mode = static_cast<decltype(posix_options.file_mode)>(posix_file_mode);
    posix_options.fadvise_random = posix_fadvise_random;
    posix_options.fadvise_dontneed_after_large_read = posix_fadvise_dontneed;
    //  ★ 批提交的 syncfs/批次数进 `fss_*` 指标族（C9.6 的"真实进程可观察"）
    posix_options.metrics = &metrics_registry;
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
  //    （`metrics_registry` 在 blob store **之前**已声明：POSIX 驱动的批提交要用它记
  //     `syncfs`/批次数，见 ADR-008 的 P4。）
  //  ★ C10.4/R11：I/O 引擎的探测与回退结果必须**可见**（横幅 + /metrics），
  //    否则"auto 回退到 blocking"只能等线上性能回归才发现。
  metrics_registry.Register("fss_io_engine", metrics::Registry::Kind::kGauge,
                            "当前生效的 I/O 引擎（1 = 生效；requested=配置请求值）");
  metrics_registry.SetGauge("fss_io_engine", 1,
                            {{"engine", io_engine_active}, {"requested", io_engine}});
  //  ★ C9.30（ADR-010 的 R11）：**宿主能力探测结果**必须与"生效引擎"分开暴露。
  //    `fss_io_engine{engine="blocking"}` 只说明"现在跑的是阻塞路径"，回答不了
  //    "这台机器到底能不能用 io_uring"（运维决定要不要改 seccomp profile 需要后者）。
  //    ⚠️ `1` = 宿主允许 io_uring，**不**代表服务正在使用它（引擎实现未交付）。
  metrics_registry.Register("fss_io_uring_available", metrics::Registry::Kind::kGauge,
                            "宿主能力探测：io_uring_setup 是否被允许（1=可用；仅探测，"
                            "不代表引擎已启用）");
  metrics_registry.SetGauge("fss_io_uring_available",
                            uring_probe.available() ? 1 : 0);
  //  ★ ADR-008 的 P4：批提交的摊销是否发生，必须能被运维观察（否则"批提交"只是文档）。
  //    判据：`fss_posix_batch_objects_total / fss_posix_group_commits_total` = 平均批大小。
  metrics_registry.Register("fss_posix_syncfs_total", metrics::Registry::Kind::kCounter,
                            "POSIX 批提交中 syncfs(2) 的调用次数（ADR-008 的 P4）");
  metrics_registry.Register("fss_posix_group_commits_total", metrics::Registry::Kind::kCounter,
                            "POSIX 批提交的批次数（每批 1 次 syncfs + 1 次 fsync(dir)）");
  metrics_registry.Register("fss_posix_batch_objects_total", metrics::Registry::Kind::kCounter,
                            "通过两阶段批提交提交的对象数（用于计算平均批大小）");
  //  ★ C10.20：SQLite 组提交的可观测性（否则"组提交生效"在真实进程上无法验证）。
  //    判据：`fss_sqlite_ops_total{repo} / fss_sqlite_group_commits_total{repo}` = 平均批大小；
  //    `group_commit=false` 时两者恒等（每操作一批）。
  metrics_registry.Register("fss_sqlite_group_commits_total", metrics::Registry::Kind::kCounter,
                            "SQLite 组提交的批次数（每批 1 次事务 COMMIT；逐操作档下每操作一批）");
  metrics_registry.Register("fss_sqlite_ops_total", metrics::Registry::Kind::kCounter,
                            "交给 SQLite 组提交协调器的写操作数（用于计算平均批大小）");
  MeteredBlobStore metered_blob(*blob_store, metrics_registry,
                               storage_driver == "s3" ? "s3" : "posix");
  SingleStoreFactory blob_factory(metered_blob);

  //  ★ 切片 4：改用**具名赋值**（而不是聚合初始化）—— 结构体新增字段时，按位置的
  //    聚合初始化会把后面的 int 静默错位到新字段上（`-Wmissing-field-initializers`
  //    也拦不住），具名赋值让"哪个键落到哪个字段"一眼可见。
  //
  //  ★ B1：仓储的**后端选择**（sqlite | postgres）。具体实现只在这里创建（R12）；
  //    上层（LocationIssuer / UseCasePorts / GcTask）只见 `domain::I*Repository` 端口。
  //    创建失败一律 exit 78 + libpq 原文，**绝不**静默回退到 SQLite（ADR-009 §4.1）。
  //
  //  ★ B2a：组合根另外持有 PG 仓储/租约的**具体类型非拥有指针**。readiness 的
  //    共享状态探针、C9.28 的连接预算、ADR-009 §6.4 的时钟偏移都需要直接访问
  //    连接池（`PgPool&`），而这些访问**只能在组合根**发生（R12：适配层不依赖 L2）。
#ifdef FSS_HAVE_LIBPQ
  PostgresLocationRepository* pg_location_repo = nullptr;
  PostgresMetadataRepository* pg_metadata_repo = nullptr;
  PostgresLeaseRepository* pg_lease_repo = nullptr;
#endif
  std::unique_ptr<domain::IFileLocationRepository> location_repository;
  std::string location_repository_backend;
  if (location_repository_name == "postgres") {
#ifdef FSS_HAVE_LIBPQ
    PostgresLocationRepositoryOptions pg_location_options;
    pg_location_options.pg.dsn = location_postgres_dsn;
    pg_location_options.pg.max_connections = static_cast<int>(location_postgres_max_connections);
    //  `location.postgres.*` 组**没有** statement_timeout_ms 键 → 用 PgOptions 默认（5000ms）。
    auto opened = PostgresLocationRepository::Open(std::move(pg_location_options));
    if (!opened.ok()) {
      std::cerr << "拒绝启动：location.repository=postgres 但打开 PG 位置仓储失败："
                << opened.error().ToString()
                << "\n  下一步：检查 location.postgres.dsn（当前"
                << (location_postgres_dsn.empty() ? "为空" : "已配置")
                << "）与 location.postgres.max_connections=" << location_postgres_max_connections
                << "，确认 PG 可达且已执行 db/migrations/001_init.sql。\n";
      return kExitConfigError;
    }
    pg_location_repo = opened.value().get();
    location_repository = std::move(opened).value();
    location_repository_backend = "postgres";
#else
    std::cerr << "拒绝启动：location.repository=postgres 需要 libpq（本二进制未编译 PG 支持）。"
                 "下一步：安装 libpq-dev 后重新 cmake + 重编。\n";
    return kExitConfigError;
#endif
  } else {
    SqliteLocationRepositoryOptions location_sqlite_options;
    location_sqlite_options.busy_timeout_millis =
        static_cast<int>(location_sqlite_busy_timeout_ms);
    location_sqlite_options.wal = location_sqlite_journal_mode != "DELETE";
    location_sqlite_options.max_write_concurrency =
        static_cast<int>(location_sqlite_max_write_concurrency);
    location_sqlite_options.synchronous_level = location_sqlite_synchronous;
    //  C10.20：`location.sqlite.{group_commit,group_commit_max_wait_ms,group_commit_max_batch}`
    location_sqlite_options.group_commit = location_sqlite_group_commit;
    location_sqlite_options.group_commit_max_wait_ms =
        static_cast<int>(location_sqlite_group_commit_max_wait_ms);
    location_sqlite_options.group_commit_max_batch =
        static_cast<int>(location_sqlite_group_commit_max_batch);
    location_sqlite_options.metrics = &metrics_registry;
    auto opened = SqliteLocationRepository::Open(sqlite_path, location_sqlite_options);
    if (!opened.ok()) {
      std::cerr << "打开位置仓储失败: " << opened.error().ToString() << "\n";
      return kExitConfigError;
    }
    location_repository = std::move(opened).value();
    location_repository_backend = "sqlite";
  }

  std::unique_ptr<domain::IMetadataRepository> metadata_repository;
  std::string metadata_repository_backend;
  if (metadata_repository_name == "postgres") {
#ifdef FSS_HAVE_LIBPQ
    PostgresMetadataRepositoryOptions pg_metadata_options;
    pg_metadata_options.pg.dsn = metadata_postgres_dsn;
    pg_metadata_options.pg.max_connections =
        static_cast<int>(metadata_postgres_max_connections);
    pg_metadata_options.pg.statement_timeout_millis =
        static_cast<int>(metadata_postgres_statement_timeout_ms);
    auto opened = PostgresMetadataRepository::Open(std::move(pg_metadata_options), clock);
    if (!opened.ok()) {
      std::cerr << "拒绝启动：metadata.repository=postgres 但打开 PG 元数据仓储失败："
                << opened.error().ToString()
                << "\n  下一步：检查 metadata.postgres.dsn（当前"
                << (metadata_postgres_dsn.empty() ? "为空" : "已配置")
                << "）与 metadata.postgres.max_connections=" << metadata_postgres_max_connections
                << "，确认 PG 可达且已执行 db/migrations/001_init.sql。\n";
      return kExitConfigError;
    }
    pg_metadata_repo = opened.value().get();
    metadata_repository = std::move(opened).value();
    metadata_repository_backend = "postgres";
#else
    std::cerr << "拒绝启动：metadata.repository=postgres 需要 libpq（本二进制未编译 PG 支持）。"
                 "下一步：安装 libpq-dev 后重新 cmake + 重编。\n";
    return kExitConfigError;
#endif
  } else {
    SqliteMetadataRepositoryOptions metadata_sqlite_options;
    metadata_sqlite_options.busy_timeout_millis = static_cast<int>(metadata_sqlite_busy_timeout_ms);
    metadata_sqlite_options.wal = metadata_sqlite_journal_mode != "DELETE";
    metadata_sqlite_options.synchronous_level = metadata_sqlite_synchronous;
    //  C10.20：`metadata.sqlite.{group_commit,group_commit_max_wait_ms,group_commit_max_batch}`
    metadata_sqlite_options.group_commit = metadata_sqlite_group_commit;
    metadata_sqlite_options.group_commit_max_wait_ms =
        static_cast<int>(metadata_sqlite_group_commit_max_wait_ms);
    metadata_sqlite_options.group_commit_max_batch =
        static_cast<int>(metadata_sqlite_group_commit_max_batch);
    metadata_sqlite_options.metrics = &metrics_registry;
    auto opened = SqliteMetadataRepository::Open(metadata_db_path, clock, metadata_sqlite_options);
    if (!opened.ok()) {
      std::cerr << "打开元数据仓储失败: " << opened.error().ToString() << "\n";
      return kExitConfigError;
    }
    metadata_repository = std::move(opened).value();
    metadata_repository_backend = "sqlite";
  }

  //  ---- 在途租约（C10.9 / B1 / C2）：`leases.enabled` 现在**真的**选择后端 ----
  //  · false（默认，单实例）→ 内存租约（逐字保持接线前的语义；上传路径**不**做租约动作）；
  //  · true → PG 租约（`staging_leases`；跨实例共享）。★ C2 起上传路径**真的**有调用方：
  //    `GetUploadLocation` 发地址时 `Acquire`，`CreateFileMetadata` 在复制/校验和期间
  //    `Renew`（`LeaseRenewer`）、`MarkReady`/回滚后 `Release`；GC 用 `ClaimExpired` 的结果
  //    驱动 `metadata.ReclaimStaleClaiming` 回收崩溃者的 claiming 行。
  //    `leases.time_source` 决定 PG 租约的时间基准（database / local），见下。
  infra::InMemoryLeaseRepository memory_lease_repository(clock);
#ifdef FSS_HAVE_LIBPQ
  std::unique_ptr<infra::PostgresLeaseRepository> pg_lease_repository;
#endif
  domain::ILeaseRepository* lease_port = &memory_lease_repository;
  std::string lease_backend = "内存（单实例；跨进程不共享；leases.enabled=false）";
  if (leases_enabled) {
#ifdef FSS_HAVE_LIBPQ
    PostgresLeaseRepositoryOptions pg_lease_options;
    pg_lease_options.pg.dsn = location_postgres_dsn;
    pg_lease_options.pg.max_connections = static_cast<int>(location_postgres_max_connections);
    //  ★ C2：`leases.time_source`。`database`（默认）= 三个原语一致地用 PG `now()`；
    //    `local` = 三个原语一致地用注入的 `IClock`（组合根这里就是它自己的 `clock`）。
    //    两种取值下 Acquire/Renew/ClaimExpired 必须**同一时间基准**（PG 实现的注释里钉住了）。
    pg_lease_options.time_source = leases_time_source == "local"
                                       ? infra::LeaseTimeSource::kLocal
                                       : infra::LeaseTimeSource::kDatabase;
    pg_lease_options.clock = &clock;
    auto opened = PostgresLeaseRepository::Open(std::move(pg_lease_options));
    if (!opened.ok()) {
      std::cerr << "拒绝启动：leases.enabled=true 但打开 PG 租约仓储失败："
                << opened.error().ToString()
                << "\n  下一步：配置 location.postgres.dsn（租约表 staging_leases 与位置记录"
                   "在同一个库；当前 "
                << (location_postgres_dsn.empty() ? "为空" : "已配置")
                << "），并确认 PG 可达且已执行 db/migrations/001_init.sql。\n";
      return kExitConfigError;
    }
    pg_lease_repo = opened.value().get();
    pg_lease_repository = std::move(opened).value();
    lease_port = pg_lease_repository.get();
    lease_backend = "postgres（staging_leases；时间源 = " +
                    std::string(leases_time_source == "local" ? "本地 IClock（leases.time_source=local）"
                                                              : "数据库 now()（leases.time_source=database）") +
                    "）";
#else
    std::cerr << "拒绝启动：leases.enabled=true 需要 libpq（本二进制未编译 PG 支持）。"
                 "下一步：安装 libpq-dev 后重新 cmake + 重编。\n";
    return kExitConfigError;
#endif
  }

  //  ---- B1：领导者选举（`leader_election.enabled` / `backend` / `lock_key`）----
  //  · 启用 → 建立**专用锁连接**（`metadata.postgres.*`：该组有 statement_timeout_ms，
  //    锁连接只跑短语句，滞留窗口因此被夹在 statement_timeout 以内）+ 启动时 `TryAcquire()`；
  //  · 未启用 → `gc_leader_check` 为空，GC 在每个实例都跑（单实例语义，逐字不变）。
#ifdef FSS_HAVE_LIBPQ
  std::unique_ptr<infra::PgLeaderElection> pg_leader_election;
#endif
  std::function<bool()> gc_leader_check;  // 返回 true = 本实例可以跑单例任务（GC）
  std::string leader_state_banner =
      "未启用（leader_election.enabled=false；GC 在每个实例都会跑）";
  if (leader_election_enabled) {
#ifdef FSS_HAVE_LIBPQ
    PgLeaderElectionOptions leader_options;
    leader_options.pg.dsn = metadata_postgres_dsn;
    leader_options.pg.max_connections = 1;  // 专用单连接（本类不过池）
    leader_options.pg.statement_timeout_millis =
        static_cast<int>(metadata_postgres_statement_timeout_ms);
    leader_options.lock_key = static_cast<std::int64_t>(leader_election_lock_key);
    auto opened = PgLeaderElection::Open(std::move(leader_options));
    if (!opened.ok()) {
      std::cerr << "拒绝启动：leader_election.enabled=true 但创建 leader election 失败："
                << opened.error().ToString()
                << "\n  下一步：配置 metadata.postgres.dsn（会话级 advisory lock 必须连到所有"
                   "实例共享的数据库；当前 "
                << (metadata_postgres_dsn.empty() ? "为空" : "已配置")
                << "），并确认 PG 可达。\n";
      return kExitConfigError;
    }
    pg_leader_election = std::move(opened).value();
    const auto acquired = pg_leader_election->TryAcquire();
    if (!acquired.ok()) {
      std::cerr << "拒绝启动：leader_election.enabled=true 但获取 advisory lock 失败："
                << acquired.error().ToString()
                << "\n  提示：锁连接只跑短语句；若 PG 上 lock_key=" << leader_election_lock_key
                << " 被别的会话长期持有，本实例不会成为 leader。\n";
      return kExitConfigError;
    }
    const bool startup_leader = acquired.value();
    gc_leader_check = [&pg_leader_election]() { return pg_leader_election->IsLeader(); };
    leader_state_banner =
        "启用（backend=" + leader_election_backend +
        " lock_key=" + std::to_string(leader_election_lock_key) + "）；启动时本实例=" +
        (startup_leader ? "leader（跑 GC）"
                        : "非 leader（跳过 GC，由持锁实例负责；锁空闲时会自动接管）");
#else
    std::cerr << "拒绝启动：leader_election.enabled=true 需要 libpq（本二进制未编译 PG 支持）。"
                 "下一步：安装 libpq-dev 后重新 cmake + 重编。\n";
    return kExitConfigError;
#endif
  }

  // ===========================================================================
  //  ⑤ B2a：生产就绪检查（真实进程、启动期、fail-closed → exit 78）
  // ===========================================================================
  //  ADR-009 的三条"共享状态可用"判据在本节落地：
  //    ① readiness 的 PG 探活（`SELECT 1`）——探针在下面装配给 `ports`（REST/gRPC 同源）；
  //    ② C9.28 PG 连接预算：`expected_instances × 每实例池上限 ≤ PG max_connections`；
  //    ③ ADR-009 §6.4/§8.1 时钟偏移：`|本地钟 − PG now()| ≤ clock_skew_tolerance_seconds`。
  //  ★ 三条都只在**真的用了 PG**（元数据或位置仓储 = postgres）时才有意义；
  //    single + SQLite/内存 → 全部跳过，行为与接线前逐字一致。
  // ===========================================================================
  std::string pg_budget_banner = "未校验（本进程未使用 PG 仓储）";
  std::string clock_skew_banner = "未校验（本进程未使用 PG 仓储）";
  //  ★ B2b：注册表/探针/监视器的启动横幅（三条判据的实际结论必须可见）。
  std::string instance_registry_banner = "未启用（本进程未使用 PG 仓储）";
  std::string consistency_banner = "未启用（本进程未使用 PG 仓储）";
  std::string mount_probe_banner = "未启用（本进程未使用 PG 仓储）";
#ifdef FSS_HAVE_LIBPQ
  //  ★ B2b：生命周期 = RunServer 的局部对象（RAII）。声明顺序保证析构顺序为
  //    monitor（stop+join）→ probe（删自己的探针）→ registry（删自己的行）。
  std::unique_ptr<infra::PgInstanceRegistry> instance_registry;
  std::unique_ptr<infra::SharedMountProbe> shared_mount_probe;
  std::unique_ptr<InstanceConsistencyMonitor> consistency_monitor;
  //  共享状态连接池：优先元数据池（`metadata.postgres.*` 是版本检查键的命名空间），
  //  否则用位置池（single + `location.repository=postgres` 的合法形态，DSN 同库）。
  infra::PgPool* shared_state_pool = nullptr;
  if (pg_metadata_repo != nullptr) {
    shared_state_pool = &pg_metadata_repo->pool();
  } else if (pg_location_repo != nullptr) {
    shared_state_pool = &pg_location_repo->pool();
  }
  if (shared_state_pool != nullptr) {
    //  ---- ① C9.28：PG 连接预算 ----
    //  ★ 每实例"最坏连接数"的构成（全部来自**组合根实际创建的池**，不是配置里写了什么）：
    //      · `metadata.postgres.max_connections`（元数据池）；
    //      · `location.postgres.max_connections`（位置池）；
    //      · 启用租约时**再加一份** `location.postgres.max_connections` ——
    //        `PostgresLeaseRepository` 是**独立池**（server_main 用它自己的 PgOptions），
    //        任务给出的公式 `metadata + location + 1` 会漏掉这一份；这里按真实池计数，
    //        宁可保守拒绝，也不让"实例数 × 上限"静默超过 PG 的 `max_connections`；
    //      · **+1**：`PgLeaderElection` 的专用会话级 advisory lock 连接（不过池）。
    //        即使 `leader_election.enabled=false`（当前不建那条连接）也按 +1 计 ——
    //        最坏情况上界，避免"开了选举就超预算"的静默漂移。
    const auto pg_max_connections_result = infra::ReadPgMaxConnections(*shared_state_pool);
    if (!pg_max_connections_result.ok()) {
      return reject_startup(
          "PG 连接预算无法校验（fail-closed）：" + pg_max_connections_result.error().ToString() +
          "\n  下一步：确认运行账号可读 pg_settings（`SELECT setting::int FROM pg_settings "
          "WHERE name='max_connections'`）。");
    }
    const long pg_max_connections = pg_max_connections_result.value();
    const long metadata_pool_cap =
        pg_metadata_repo != nullptr ? metadata_postgres_max_connections : 0;
    const long location_pool_cap =
        pg_location_repo != nullptr ? location_postgres_max_connections : 0;
    const long lease_pool_cap =
        pg_lease_repo != nullptr ? location_postgres_max_connections : 0;
    const long leader_lock_cap = 1;
    const long per_instance_cap =
        metadata_pool_cap + location_pool_cap + lease_pool_cap + leader_lock_cap;
    const long required = expected_instances * per_instance_cap;
    if (required > pg_max_connections) {
      std::ostringstream message;
      message << "PG 连接预算不足（C9.28：实例数 × 每实例池上限 ≤ PG max_connections）。\n"
              << "  PG max_connections            = " << pg_max_connections << "\n"
              << "  每实例最坏连接数               = " << per_instance_cap << " = metadata("
              << metadata_pool_cap << ") + location(" << location_pool_cap << ") + lease("
              << lease_pool_cap << "，独立池) + leader_lock(" << leader_lock_cap << ")\n"
              << "  deployment.expected_instances  = " << expected_instances << "\n"
              << "  需要 " << expected_instances << " × " << per_instance_cap << " = " << required
              << " > " << pg_max_connections << "\n"
              << "  下一步：提高 PG max_connections（当前 " << pg_max_connections
              << "），或调低 metadata/location.postgres.max_connections（每实例当前 "
              << per_instance_cap << "），或调低 deployment.expected_instances（当前 "
              << expected_instances << "）。";
      return reject_startup(message.str());
    }
    pg_budget_banner = "OK（每实例 " + std::to_string(per_instance_cap) + " = metadata(" +
                       std::to_string(metadata_pool_cap) + ") + location(" +
                       std::to_string(location_pool_cap) + ") + lease(" +
                       std::to_string(lease_pool_cap) + ") + leader_lock(" +
                       std::to_string(leader_lock_cap) + ")；expected_instances=" +
                       std::to_string(expected_instances) + " → 需要 " +
                       std::to_string(required) + " ≤ PG max_connections=" +
                       std::to_string(pg_max_connections) + "）";

    //  ---- ② ADR-009 §6.4/§8.1：本地钟 vs 数据库 now() ----
    //  容忍值来自 `deployment.clock_skew_tolerance_seconds`（语义 = "与数据库 now() 的
    //  偏移容忍"）。★ 另一个相关键 `deployment.max_clock_skew_seconds` **不是**本比较的
    //  容忍值：它继续用于 multi 跨字段校验与 JWT `exp`/`nbf` 的容忍（C8.10），见 §1.3。
    const auto pg_now = shared_state_pool->NowEpochMillis();
    if (!pg_now.ok()) {
      return reject_startup("时钟偏移校验失败：无法读取数据库 now()：" +
                            pg_now.error().ToString() +
                            "\n  下一步：确认 PG 可达且运行账号可执行 `SELECT now()`。");
    }
    const std::int64_t local_now_millis = clock.NowEpochMillis();
    const std::int64_t skew_millis = local_now_millis - pg_now.value();
    const std::int64_t tolerance_millis =
        static_cast<std::int64_t>(clock_skew_tolerance_seconds) * 1000;
    if (std::llabs(skew_millis) > tolerance_millis) {
      std::ostringstream message;
      message << "实例时钟与数据库 now() 偏差超限（ADR-009 §6.4/§8.1）。\n"
              << "  本地钟（本实例）  = " << local_now_millis << " ms (epoch)\n"
              << "  数据库 now()      = " << pg_now.value() << " ms (epoch)\n"
              << "  偏差              = " << skew_millis << " ms（正 = 本地快）\n"
              << "  容忍范围          = deployment.clock_skew_tolerance_seconds="
              << clock_skew_tolerance_seconds << "s（" << tolerance_millis << " ms）\n"
              << "  下一步：校准宿主机 NTP/chrony（本实例与数据库主机都要），"
                 "或按部署现实的偏差上界调大 deployment.clock_skew_tolerance_seconds。";
      return reject_startup(message.str());
    }
    clock_skew_banner =
        "OK（本地 " + std::to_string(local_now_millis) + " ms vs 数据库 " +
        std::to_string(pg_now.value()) + " ms，偏差 " + std::to_string(skew_millis) +
        " ms，容忍 " + std::to_string(tolerance_millis) + " ms）";

    // =========================================================================
    //  ③ B2b：instance_registry 心跳 + 配置/版本一致性 + 共享挂载探针
    // =========================================================================
    //  ADR-009 §5.3（滚动升级：版本/配置不一致 → 不 ready）与 §8.1 item 4
    //  （`storage.posix.root` 是否真的共享 → 交叉探针可见性）。
    //  ★ 顺序（不可交换）：**先写自己的探针文件 → 再注册注册表行 → 再查对端可见性**。
    //    这样"对端注册后立刻查我"时我的探针一定已在共享目录里（见 shared_mount_probe.h）。
    //  ★ 连接预算：本模块复用 `shared_state_pool`，不新建池 ⇒ C9.28 的公式不变。
    {
      //  service_version = 二进制已有的版本串（`FSS_BUILD_VERSION`，由 CMake 注入）。
      //  测试接缝 `FSS_SERVICE_VERSION_OVERRIDE`（**环境变量，不是配置键**）用于验证
      //  "滚动升级时版本不一致"这条判据；横幅会显式标注接缝生效（避免误读）。
      const char* version_override = std::getenv("FSS_SERVICE_VERSION_OVERRIDE");
      const bool version_overridden = version_override != nullptr && *version_override != '\0';
      const std::string self_service_version = version_overridden
                                                   ? std::string(version_override)
                                                   : std::string(fss::app::BuildVersion());

      //  ① 探针文件（仅 `shared_mount_required=true`）：**先于**注册表注册。
      int stale_probes_removed = 0;
      if (shared_mount_required) {
        shared_mount_probe =
            std::make_unique<infra::SharedMountProbe>(storage_root, effective_instance_id);
        if (const auto written = shared_mount_probe->WriteOwn(clock.NowEpochMillis());
            !written.ok()) {
          return reject_startup(
              "storage.posix.shared_mount_required=true 但探针文件写入失败（fail-closed）：" +
              written.error().ToString() + "\n  下一步：确认 storage.posix.root（" + storage_root +
              "）存在、可写，且在所有实例上指向**同一个共享挂载**。");
        }
        //  清理崩溃实例留下的过老探针（不碰自己的、不碰 `.tmp.` 中间态）。
        if (const auto cleaned =
                shared_mount_probe->CleanupStale(std::chrono::seconds(kProbeStaleCleanupSeconds));
            cleaned.ok()) {
          stale_probes_removed = cleaned.value();
        }
      }

      //  ② 注册表注册（含 config_hash 计算）。
      const auto config_hash_result = ComputeInstanceConfigHash(cfg);
      if (!config_hash_result.ok()) {
        return reject_startup("config_hash 计算失败（fail-closed）：" +
                              config_hash_result.error().ToString());
      }
      const std::string self_config_hash = config_hash_result.value();
      instance_registry = std::make_unique<infra::PgInstanceRegistry>(
          *shared_state_pool, effective_instance_id, self_service_version, self_config_hash);
      if (const auto registered = instance_registry->UpsertSelf(); !registered.ok()) {
        return reject_startup(
            "instance_registry 注册失败（fail-closed）：" + registered.error().ToString() +
            "\n  下一步：确认已执行 db/migrations/001_init.sql（instance_registry 表），"
            "且运行账号对该表有 INSERT/UPDATE/DELETE 权限。");
      }
      int stale_rows_removed = 0;
      if (const auto cleaned = instance_registry->CleanupStale(kInstanceStaleCleanupSeconds);
          cleaned.ok()) {
        stale_rows_removed = cleaned.value();
      }
      instance_registry_banner =
          "已注册（instance_id=" + effective_instance_id +
          " service_version=" + self_service_version +
          (version_overridden
               ? "（★测试接缝 FSS_SERVICE_VERSION_OVERRIDE 已生效；生产禁止设置）"
               : "") +
          " config_hash=" + self_config_hash.substr(0, 12) +
          "；心跳 " + std::to_string(kInstanceHeartbeatSeconds) + "s，live 窗口 " +
          std::to_string(kInstanceLivenessSeconds) + "s，过老清理阈值 " +
          std::to_string(kInstanceStaleCleanupSeconds) + "s，启动清理 " +
          std::to_string(stale_rows_removed) + " 行）";

      //  ③ 启动期交叉可见性检查（仅 `shared_mount_required=true`）。
      int live_peers_at_startup = 0;
      if (shared_mount_required) {
        const auto visibility =
            StartupMountVisibilityCheck(*instance_registry, *shared_mount_probe);
        if (!visibility.ok()) {
          return reject_startup(
              visibility.error().message() +
              "\n  下一步：让所有实例的 storage.posix.root 指向**同一个共享挂载**"
              "（NFS / 共享卷），或把 deployment.mode 改回 single"
              "（multi 强制要求 shared_mount_required=true，见 core_schema.cpp 跨字段校验）。");
        }
        live_peers_at_startup = visibility.value();
        if (live_peers_at_startup == 0) {
          mount_probe_banner =
              "已写入 " + shared_mount_probe->OwnPath() + "（写回自证通过）；" +
              "★ 启动时没有 live peer ⇒ **跨实例可见性尚未验证**" +
              "（无法证明共享、也无法证伪；由运行时每 " +
              std::to_string(kInstanceHeartbeatSeconds) + "s 的心跳复查兜住）";
          logging::Warn(logger,
                        "shared mount cross-instance visibility NOT yet verified at startup",
                        {{"component", "server_main"},
                         {"probe_path", shared_mount_probe->OwnPath()},
                         {"reason", "启动时 instance_registry 里没有 live peer；"
                                    "已写入自己的探针，运行时心跳会复查对端可见性"}});
        } else {
          mount_probe_banner =
              "已验证（" + std::to_string(live_peers_at_startup) +
              " 个 live peer 的探针在本实例 root 下均可见；自己的探针 " +
              shared_mount_probe->OwnPath() + "）";
        }
        mount_probe_banner += "；清理过老探针 " + std::to_string(stale_probes_removed) + " 个";
      } else {
        mount_probe_banner = "未启用（storage.posix.shared_mount_required=false）";
      }
      consistency_banner =
          "已启用（每 " + std::to_string(kInstanceHeartbeatSeconds) +
          "s 心跳；对每个 live peer 检查探针可见性" +
          std::string(shared_mount_required ? "" : "（探针未启用）") +
          " + 服务版本兼容性 + config_hash；不一致 → readiness not ready）";

      //  ④ 启动运行时监视器（readiness 从第一次请求起就读它的结论）。
      consistency_monitor = std::make_unique<InstanceConsistencyMonitor>(
          *instance_registry, shared_mount_probe.get(), self_service_version, self_config_hash);
      consistency_monitor->Start();
    }
  }
#endif

  HmacTransferTokenCodec token_codec(transfer_secret, clock, transfer_key_id);

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
    jwt_options.roles_claim = jwt_roles_claim;
    jwt_options.local_roles = local_roles;
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

  //  ---- 事件发布器（P10 切片 6b / ADR-013 §9）----
  //  ★ R12：具体实现只能在**组合根**创建 —— 用例层只见 `IEventPublisher` 端口。
  //  ★ 三分支：`log`（默认，行为逐字不变）/ `webhook`（出站 POST，失败**非致命**）/
  //    `none`（**显式关闭**，不发任何请求）。非法取值由 schema 的 enum 拒绝，
  //    这里的 `else` 只是防御性的第二道（枚举被改宽时不会静默降级）。
  LogEventPublisher log_events(logger);
  NoopEventPublisher noop_events;
  std::unique_ptr<infra::WebhookEventPublisher> webhook_events;
  domain::IEventPublisher* events = &log_events;
  if (events_publisher == "webhook") {
    infra::WebhookEventPublisherOptions options;
    //  ★ `url` 就是**完整端点 URL**（POST 到它，不追加路径）—— 与切片 6a 同一约定
    options.url = events_webhook_url;
    options.timeout_ms = static_cast<int>(events_webhook_timeout_ms);
    options.topic = events_webhook_topic;
    webhook_events = std::make_unique<infra::WebhookEventPublisher>(options, logger);
    events = webhook_events.get();
    //  ★ 没配地址就**拒绝启动**（与远端校验器一致）：起来之后"每个事件都发不出去"
    //    只会制造误导性的排障路径；要关掉事件必须显式写 `publisher=none`。
    if (!webhook_events->Ready()) {
      std::cerr << "拒绝启动：" << webhook_events->NotReadyReason() << "\n";
      return kExitConfigError;
    }
    logging::Warn(logger,
                  "events.publisher=webhook：发布失败**非致命**（连不上/超时/非 2xx 只告警，"
                  "请求照常成功；同步发布 → 慢 webhook 会增加请求延迟）",
                  {{"component", "server_main"}, {"endpoint", events_webhook_url}});
  } else if (events_publisher == "none") {
    events = &noop_events;
    logging::Warn(logger, "events.publisher=none：已**显式关闭**事件发布（不发任何请求）",
                  {{"component", "server_main"}});
  } else if (events_publisher != "log") {
    std::cerr << "拒绝启动：未知的 events.publisher：" << events_publisher
              << "（可选：log | webhook | none）\n";
    return kExitConfigError;
  }

  LogAuditLogger log_audit(logger);
  NoopAuditLogger noop_audit;
  FailingAuditLogger failing_audit;
  //  ★ 优先级：`audit_enabled=false` → no-op（记录被关掉，谈不上"失败"）；
  //    否则注入开关打开时用"必然失败"的后端（C10.13 的可驱动接缝）；否则真实日志审计器。
  domain::IAuditLogger* audit_logger = &log_audit;
  if (!audit_enabled) {
    audit_logger = &noop_audit;
  } else if (audit_fault_inject) {
    audit_logger = &failing_audit;
  }

  //  ★ C10.16：`partition.file.<partition>.*`（动态子树）。只接**真实存在**的字段：
  //    `PartitionConfig::max_object_bytes`（-1 = 不限）；校验算法集合/默认算法按真实语义
  //    只做**启动期校验**（非法算法名 → exit 78），请求期客户端声明的算法按 C6.4 被覆写。
  const PartitionFileOptions partition_file = ReadPartitionFileOptions(cfg, resolver);
  if (const std::string problem = ValidatePartitionChecksums(partition_file); !problem.empty()) {
    std::cerr << "拒绝启动（exit " << kExitConfigError << "）：" << problem
              << "\n下一步：把 allowed/default 校验算法改成 SHA-256 | SHA-1 | MD5 之一"
                 "（大小写与 `-`/`_` 不计）。\n";
    return kExitConfigError;
  }
  //  ★ 阶段 10（C10.16 续）：容器名 / 分区级驱动（非法容器名或驱动冲突 → exit 78）
  if (const std::string problem = ValidatePartitionContainers(partition_file, storage_driver);
      !problem.empty()) {
    std::cerr << "拒绝启动（exit " << kExitConfigError << "）：" << problem << "\n";
    return kExitConfigError;
  }

  domain::PartitionConfig partition_cfg;
  partition_cfg.partition = partition_file.partition;
  partition_cfg.driver = storage_driver == "s3" ? domain::StorageDriver::kS3
                                                : domain::StorageDriver::kPosix;
  partition_cfg.posix_root = storage_root;
  //  ★ 阶段 10：容器名覆盖（空串 → `ObjectKeyPolicy` 的默认命名，逐字一致）
  partition_cfg.staging_container = partition_file.staging_container;
  partition_cfg.persistent_container = partition_file.persistent_container;
  partition_cfg.storage_driver = partition_file.storage_driver;
  //  0（schema 语义：不限）→ -1（端口语义：不限）
  partition_cfg.max_object_bytes = partition_file.max_file_bytes > 0 ? partition_file.max_file_bytes
                                                                     : -1;
  StaticPartitionRegistry partitions(partition_cfg);

  //  ---- 可选校验器（legal / schema；P10 切片 6a / ADR-013）----
  //  ★ R12：具体实现只能在**组合根**创建 —— 用例层只见 `ILegalValidator` /
  //    `ISchemaValidator` 端口。`noop`（默认）逐字保持接线前的行为。
  //  ★ 非法取值由 schema 的 enum 拒绝（`core_schema.cpp` 已声明 `Enum({"noop","remote"})`
  //    → exit 78）；这里的 `else` 分支只是防御性的第二道（枚举被改宽时不会静默降级）。
  NoopLegalValidator noop_legal;
  NoopSchemaValidator noop_schema;
  std::unique_ptr<infra::RemoteLegalValidator> remote_legal;
  std::unique_ptr<infra::RemoteSchemaValidator> remote_schema;
  domain::ILegalValidator* legal = &noop_legal;
  domain::ISchemaValidator* schema_validator_port = &noop_schema;
  if (legal_validator_kind == "remote") {
    infra::RemoteLegalValidatorOptions options;
    options.base_url = legal_remote_base_url;
    options.timeout_ms = static_cast<int>(legal_remote_timeout_ms);
    remote_legal = std::make_unique<infra::RemoteLegalValidator>(options);
    legal = remote_legal.get();
    //  ★ 没配地址就**拒绝启动**（与 RemoteEntitlementsAuthorizer 一致）：起来之后
    //    "每次校验都 503"只会制造误导性的排障路径。
    if (!remote_legal->Ready()) {
      std::cerr << "拒绝启动：" << remote_legal->NotReadyReason() << "\n";
      return kExitConfigError;
    }
    logging::Warn(logger,
                  "legal.validator=remote：依赖不可用时 fail-closed（503），绝不放过",
                  {{"component", "server_main"}, {"endpoint", legal_remote_base_url}});
  } else if (legal_validator_kind != "noop") {
    std::cerr << "拒绝启动：未知的 legal.validator：" << legal_validator_kind
              << "（可选：noop | remote）\n";
    return kExitConfigError;
  }
  if (schema_validator_kind == "remote") {
    infra::RemoteSchemaValidatorOptions options;
    options.base_url = schema_remote_base_url;
    options.timeout_ms = static_cast<int>(schema_remote_timeout_ms);
    remote_schema = std::make_unique<infra::RemoteSchemaValidator>(options);
    schema_validator_port = remote_schema.get();
    if (!remote_schema->Ready()) {
      std::cerr << "拒绝启动：" << remote_schema->NotReadyReason() << "\n";
      return kExitConfigError;
    }
    logging::Warn(logger,
                  "schema.validator=remote：依赖不可用时 fail-closed（503），绝不放过",
                  {{"component", "server_main"}, {"endpoint", schema_remote_base_url}});
  } else if (schema_validator_kind != "noop") {
    std::cerr << "拒绝启动：未知的 schema.validator：" << schema_validator_kind
              << "（可选：noop | remote）\n";
    return kExitConfigError;
  }

  //  ★ 阶段 10：把租户注册表交给 LocationIssuer —— 否则 uploadURL 签发的容器名与
  //    用例/GC 解析出的容器名会不一致（`partition.file.*.staging_container` 只对
  //    一半路径生效，等于没生效）。
  app::LocationIssuer issuer(blob_factory, *location_repository, token_codec, clock, ids,
                             self_base_url, expiry_options, &partitions, self_signed_ttl_options);

  app::UseCasePorts ports{blob_factory,      *location_repository,
                          *metadata_repository,
                          *authorizer,       *events,
                          *audit_logger,     partitions,
                          *legal,            *schema_validator_port,
                          issuer,            clock,
                          ids};
  ports.auth_mode = auth_mode;  // C8.5：让 `/v2/info` 与 gRPC 的 `GetInfo` 都能看到
  //  ★ C9.30（ADR-010 的 R11）：把"当前生效引擎"与"宿主能力探测结果"交给用例 ——
  //    REST 的 `/v2/info` 与 gRPC 的 `InfoResponse` 都从 `GetInfo` 取值，两条协议
  //    的一致性因此按构造保证（不在适配器里各算一遍）。
  //    ⚠️ 语义区分（契约与头文件里同样写明）：`io_engine` = **当前生效**（本实现
  //       恒为 blocking，ADR-010 U1~U4 未满足）；`io_uring_available` = **宿主能力**
  //       （`io_uring_setup` 是否被允许），**可用 ≠ 已启用**。
  ports.io_engine = io_engine_active;
  ports.io_uring_available = uring_probe.available();
  //  C10.13：审计失败是否让请求失败（用例层判定；见 usecases.cpp 的 RecordAudit）
  ports.audit_fail_closed = audit_fail_closed;
  //  ★ C2（ADR-009 §4.2/§4.3）：上传路径的在途租约。
  //    `leases_enabled=false`（默认）→ `ports.leases_enabled=false` 且不装配端口指针，
  //    上传/登记路径一个租约调用都不做（单实例行为逐字不变）。
  //    `true` → `GetUploadLocation` 发地址时 Acquire、`CreateFileMetadata` 复制期间续租。
  ports.leases = lease_port;
  ports.leases_enabled = leases_enabled;
  ports.lease_ttl_seconds = leases_ttl_seconds;
  ports.lease_renew_interval_seconds = leases_renew_interval_seconds;
  ports.instance_id = effective_instance_id;
  //  ★ B2a（ADR-009 §5.3）：readiness 的**共享状态**探针 —— PG 存活（`SELECT 1`）+
  //    迁移版本（`schema_migrations.max(version) == kExpectedSchemaVersion`）。
  //    组合根在这里把连接池包成回调注入（R12：适配层不依赖 L2）；REST 与 gRPC 从
  //    **同一份** `ports` 取值 ⇒ 两条协议的 readiness 判据按构造一致。
  //    未使用 PG（`shared_state_pool == nullptr`）时不装配 ⇒ 适配层退回原有最小探针。
#ifdef FSS_HAVE_LIBPQ
  if (shared_state_pool != nullptr) {
    const bool check_schema_version = metadata_postgres_schema_version_check;
    infra::PgPool* probe_pool = shared_state_pool;
    //  ★ B2b：把"实例一致性"（心跳/共享挂载/版本/config_hash）与 B2a 的 PG 探针
    //    合成**同一个** readiness 判据。监视器可能为空（理论上不会：注册失败会 exit 78），
    //    因此这里判空。
    InstanceConsistencyMonitor* monitor = consistency_monitor.get();
    ports.shared_state_probe = [probe_pool, check_schema_version,
                                monitor]() -> fss::Result<void> {
      FSS_TRY(infra::ProbePgSharedState(*probe_pool, check_schema_version));
      if (monitor != nullptr) {
        const std::string reason = monitor->NotReadyReason();
        if (!reason.empty()) return Err(fss::ErrorKind::kUnavailable, reason);
      }
      return Ok();
    };
  }
#endif
  //  ★ C9.26：测试专用的崩溃窗口接缝（`FSS_CLAIM_HOLD_MS`，**不是配置键**）。
  //    默认未设置 → 0 → `CreateFileMetadata` 与接线前逐字一致（不 sleep）。
  const std::int64_t claim_hold_millis = ClaimHoldMillisFromEnv();
  ports.claim_hold_millis = claim_hold_millis;
  if (claim_hold_millis > 0) {
    logging::Warn(logger, "claim_hold_seam_active",
                  {{"component", "server_main"},
                   {"claim_hold_ms", std::to_string(claim_hold_millis)},
                   {"reason", "★ 测试接缝 FSS_CLAIM_HOLD_MS 已生效：createMetadata 将在"
                              "原子领取后、复制前阻塞该毫秒数；生产禁止设置"}});
  }

  // ===========================================================================
  //  GC（C10.9）：GcTask + 调度参数。`--once` 与周期调度共用同一份 options。
  // ===========================================================================
  app::GcOptions gc_options;
  gc_options.dry_run = gc_dry_run;
  gc_options.require_lease_expiry = gc_require_lease_expiry;
  gc_options.staging_ttl_hours = gc_staging_ttl_hours;
  gc_options.orphan_grace_hours = gc_orphan_grace_hours;
  //  ★ C2：`leases.ttl_seconds` 是回收"崩溃领取者 claiming 行"的二级年龄护栏。
  gc_options.lease_ttl_seconds = leases_ttl_seconds;
  const std::string gc_partition = "opendes";  // 组合根内置的单租户（与 StaticPartitionRegistry 同源）
  app::GcTask gc_task(ports, *lease_port, effective_instance_id, &metrics_registry);

  //  ★ GC 的两条 `list()` 路径要求容器真实存在（POSIX 驱动对不存在的容器返回
  //    `kNotFound`）。启动时按 `ObjectKeyPolicy` 生成的两个容器名确保目录存在 ——
  //    否则**全新实例**每轮 GC 都会把"没有残留"记成 errors（`outcome="error"`），
  //    把正常状态误报成扫描失败。
  for (const auto zone : {domain::StorageZone::kStaging, domain::StorageZone::kPersistent}) {
    //  ★ 阶段 10：用**注册表**解析容器名（分区级覆盖），否则自定义容器名在启动时
    //    不会被创建，GC 每轮把"没有残留"记成 errors。
    const auto container = app::ObjectKeyPolicy::ContainerFor(partitions, gc_partition, zone);
    if (container.ok()) (void)metered_blob.ensure_container(container.value());
  }

  //  `--once`（便于 cron）：跑**一轮** GC 就退出（退出码 0），与正常启动共用配置加载。
  if (cli.once) {
    //  ★ B1：`--once` 也做 leader 门控 —— 否则 multi 下每个实例的 cron 都会真删，
    //    与"GC 单例运行"矛盾。非 leader 不是错误（退出码 0），但要明确说明跳过了。
    if (gc_leader_check && !gc_leader_check()) {
      std::cout << "gc once : 跳过（本实例不是 leader；leader_election.enabled=true，"
                   "单例 GC 由持锁实例负责）\n";
      return 0;
    }
    const auto once_report = gc_task.Run(gc_partition, gc_options);
    if (!once_report.ok()) {
      std::cerr << "GC 单次运行失败：" << once_report.error().ToString() << "\n";
      return kExitConfigError;
    }
    const auto& report = once_report.value();
    std::cout << "gc once : partition=" << gc_partition
              << " dry_run=" << (report.dry_run ? "true" : "false")
              << " expired_leases_claimed=" << report.expired_leases_claimed
              << " deleted_objects=" << report.deleted_objects
              << " deleted_locations=" << report.deleted_locations
              << " tmp_removed=" << report.tmp_removed
              << " tmp_skipped_too_young=" << report.tmp_skipped_too_young
              << " tmp_skipped_unknown_mtime=" << report.tmp_skipped_unknown_mtime
              << " skipped_has_record=" << report.skipped_has_record
              << " skipped_no_location=" << report.skipped_no_location
              << " skipped_too_young=" << report.skipped_too_young
              << " errors=" << report.errors << "\n";
    return 0;
  }

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
  //  ★ C10.16 / 切片 4：数据面 PUT 的请求体上限 = 全局键
  //    `server.http.transfer_max_body_bytes` 与分区键
  //    `partition.file.<partition>.max_file_bytes` 的**较小者**（0 = 该侧"不限"，
  //    因此只在两侧都 > 0 时取 min；否则取非 0 的那一侧）。两个都为 0/不限 → 结果 0，
  //    行为与接线前**逐字一致**。超限由 HTTP 包装层拒绝：有 `Content-Length` → 413，
  //    chunked → 400（`common/http/http.h` 的既定语义，不改包装层）。
  const std::int64_t transfer_put_max_body_bytes =
      (transfer_max_body_bytes > 0 && partition_file.max_file_bytes > 0)
          ? std::min<std::int64_t>(transfer_max_body_bytes, partition_file.max_file_bytes)
          : std::max<std::int64_t>(transfer_max_body_bytes, partition_file.max_file_bytes);
  router_options.transfer_put_max_body_bytes = transfer_put_max_body_bytes;

  //  ---- 按需 GC 回调（C9.31 / ADR-013 §10）----
  //  ★ R12：`GcTask` 的实例只在这里创建；适配层只拿到"跑一轮、返回 GcReport"的形状。
  //  ★ **同一个** `gc_task` 既给周期调度、又给端点 —— 单飞护栏因此是共享的（不排队/不并行）。
  //  ★ `gc_schedule` 在这里就判定（而不是等横幅前），因为响应里的 `scheduled` 字段
  //    必须与"调度到底跑不跑"一致：`gc.enabled=false` 时端点仍然可用，但 `scheduled=false`。
  const bool gc_schedule = gc_enabled && gc_interval_seconds > 0;
  adapters::http::GcCallbacks gc_callbacks;
  gc_callbacks.partition = gc_partition;
  gc_callbacks.scheduled = gc_schedule;
  gc_callbacks.run = [&gc_task, &gc_options, &gc_partition, &ports, &logger, &gc_leader_check](
                         const app::CallerContext& caller,
                         bool force_dry_run) -> Result<app::GcReport> {
    //  ★ B1：leader 门控（与周期调度**同一判据**）。非 leader **不调用** `GcTask::Run`
    //    ⇒ `fss_gc_runs_total` 不增加；HTTP 侧以 503 + 可读原因回应（与单飞护栏同族的语义）。
    if (gc_leader_check && !gc_leader_check()) {
      logging::Warn(logger, "gc_skipped_not_leader",
                    {{"partition", gc_partition},
                     {"reason", "按需 GC 被跳过：本实例不是 leader"
                                "（leader_election.enabled=true）→ fss_gc_runs_total 不增加"}});
      return Err(fss::ErrorKind::kUnavailable,
                 "GC 未运行：本实例不是 leader（leader_election.enabled=true）。"
                 "单例 GC 由持锁实例负责；本实例跳过本轮。");
    }
    //  ★ dry-run 只能**更保守**：配置为真删时，请求可以要求本轮干跑；反之**不行**。
    app::GcOptions options = gc_options;
    options.dry_run = gc_options.dry_run || force_dry_run;
    auto result = gc_task.Run(gc_partition, options);

    //  ★ 审计（C9.31）：这是会**删数据**的运维动作，成功/失败两侧都要留痕。
    //    operation = `gcRun`（不套 `Success`/`Failure` 后缀：结果在 `result` 字段里）。
    //    "失败"包含两种：`Run` 返回错误，或报告里 `errors > 0`（扫描没跑完）。
    const bool round_ok = result.ok() && result.value().errors == 0;
    domain::AuditEvent event;
    event.operation = "gcRun";
    event.user = caller.user_id;
    event.partition = gc_partition;
    event.result = round_ok ? "success" : "failure";
    event.epoch_millis = ports.clock.NowEpochMillis();
    event.correlation_id = caller.correlation_id;
    //  审计写入失败**不改结论**（与 `observability.audit_fail_closed=false` 的默认一致：
    //  GC 已经真的跑了，谎报失败会让运维以为没删；`Record` 的返回码在这里只用于告警）。
    if (const auto recorded = ports.audit.Record(event); !recorded.ok()) {
      logging::Warn(logger, "gc_run_audit_failed", {{"error", recorded.error().message()}});
    }
    return result;
  };

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

  adapters::http::Router router(ports, transfers, router_options, std::move(gc_callbacks));

  //  ---- gRPC 面：与 REST 共用**同一个** `UseCasePorts`（C7.8：双协议同时运行）----
  //  ★ 与 HTTP 面一样，服务的具体实现只在这里创建（R12）；两个服务共用同一批
  //    端口实现（同一个 blob/location/metadata 实例），因此两条链路看到同一份状态。
  //  ★ 组合根**不直接**碰 grpc 类型：服务器的生命周期收在 `adapters/grpc/grpc_server.h`
  //    的包装里（C7.7 的护栏要求 `src/` 全树除 adapters/grpc/ 外不出现 `<grpcpp/`；
  //    与 `fss_http` 把 httplib 挡在适配层内是同一条纪律）。
  std::unique_ptr<adapters::grpc::FileServiceAdapter> grpc_service;
  //  ★ 变量名不用 `grpc_server`：那会遮蔽 libpq/grpc 头里的全局 typedef `grpc_server`
  //    （`-Wshadow`；既有告警，本切片顺手清掉，让"改动的文件零告警"成立）。
  std::unique_ptr<adapters::grpc::GrpcServerHandle> grpc_handle;
  //  C10.15：`server.grpc.enabled=false` 或 `server.grpc.port=0` 都表示"不开 gRPC 面"。
  if (grpc_enabled && grpc_port != 0) {
    grpc_service = std::make_unique<adapters::grpc::FileServiceAdapter>(ports, "osdu-user");
    grpc_handle =
        adapters::grpc::StartGrpcServer(*grpc_service, grpc_bind, static_cast<int>(grpc_port));
    if (!grpc_handle->ok()) {
      std::cerr << grpc_handle->last_error() << "\n";
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

  //  ---- GC 周期调度（C10.9）：先判定，再在横幅里如实说明"跑/不跑 + 原因" ----
  //  ★ `gc_schedule` 已在构造按需端点回调时判定（响应里的 `scheduled` 必须与它一致）。
  std::unique_ptr<GcScheduler> gc_scheduler;
  std::string gc_banner;
  if (!gc_enabled) {
    gc_banner =
        "未启动（gc.enabled=false；schema 与组合根默认都是 false —— 不提供配置时不跑 GC，"
        "与接线前的行为一致）";
  } else if (gc_interval_seconds <= 0) {
    gc_banner = "未启动（gc.interval_seconds=" + std::to_string(gc_interval_seconds) +
                " <= 0：调度周期必须为正，否则等于空转压满机器）";
  } else {
    gc_banner = "已启动（间隔 " + std::to_string(gc_interval_seconds) + "s，dry_run=" +
                std::string(gc_dry_run ? "true" : "false") +
                "，require_lease_expiry=" +
                std::string(gc_require_lease_expiry ? "true" : "false") + "，staging_ttl=" +
                std::to_string(gc_staging_ttl_hours) + "h，orphan_grace=" +
                std::to_string(gc_orphan_grace_hours) + "h；每轮立即跑一次再按间隔重复）";
  }
  //  ★ C9.31：按需端点是**独立于周期调度**的能力（`gc.enabled=false` 也可用）——
  //    横幅必须把它说清楚，否则运维只会看到"GC 未启动"而不知道可以手动清一轮。
  gc_banner += "；按需端点 POST " + base_path +
               "/v2/gc:run（需 service.file.admin，不需要 data-partition-id；"
               "本轮结果见响应；?dryRun=true 只能把本轮降级为预览）";

  if (!server.Start()) {
    std::cerr << "监听失败: " << server.last_error() << "\n";
    return kExitConfigError;
  }

  //  ★ 启动横幅的**第一行**必须是配置来源（C10.1）：运维第一眼要能回答"这次到底读了哪份配置"。
  std::cout << "fss_server 配置来源 : "
            << (config_path.empty() ? "无配置文件（仅环境变量）" : config_path) << "\n"
            << "fss_server 已启动\n"
            << "  bind           : " << bind_address << ":" << server.port() << "\n"
            << "  grpc bind      : "
            << (grpc_handle ? grpc_bind + ":" + std::to_string(grpc_handle->port())
                            : std::string("disabled（") +
                                  (grpc_enabled ? "server.grpc.port=0"
                                                : "server.grpc.enabled=false") +
                                  "）")
            << "\n"
            << "  base path      : " << base_path << "\n"
            << "  storage driver : " << storage_driver << "\n"
            << "  storage root   : " << storage_root << "\n"
            //  ★ ADR-008 §5.4 / C9.20：**必须在真实进程里显式声明当前耐久性粒度**
            //    （批级 / 单文件级），不允许"默认值不说清"。
            << "  durability     : " << durability
            << (durability == "batch"
                    ? "（两阶段批提交：write all tmp → syncfs → rename all → fsync(dir)；"
                      "粒度=批，批上限 " +
                          std::to_string(posix_group_commit_max_batch) + "；并发才有摊销）"
                    : std::string(durability == "per_file"
                                      ? "（每对象 fdatasync + fsync(dir)；粒度=单文件）"
                                      : "（不落盘；仅限可重建数据）"))
            << "\n"
            << "  sqlite path    : " << sqlite_path << "\n"
            << "  sqlite tuning  : location busy_timeout=" << location_sqlite_busy_timeout_ms
            << "ms journal_mode=" << location_sqlite_journal_mode << "（wal="
            << (location_sqlite_journal_mode != "DELETE" ? "true" : "false")
            << "，max_write_concurrency=" << location_sqlite_max_write_concurrency
            << "，synchronous=" << location_sqlite_synchronous_text
            << "）| metadata busy_timeout=" << metadata_sqlite_busy_timeout_ms
            << "ms journal_mode=" << metadata_sqlite_journal_mode << "（wal="
            << (metadata_sqlite_journal_mode != "DELETE" ? "true" : "false")
            << "，synchronous=" << metadata_sqlite_synchronous_text << "）\n"
            //  ★ C10.20：6 个组提交键的实际取值必须与 journal_mode / synchronous 同处可见
            //    （否则运维无法确认"默认的组提交"到底开没开、窗口多大）。
            << "  sqlite commit  : location group_commit="
            << (location_sqlite_group_commit ? "true" : "false")
            << " max_wait_ms=" << location_sqlite_group_commit_max_wait_ms
            << " max_batch=" << location_sqlite_group_commit_max_batch
            << "（组提交：并发写一批一次 COMMIT；读可能多等 ≤ max_wait_ms）"
            << " | metadata group_commit=" << (metadata_sqlite_group_commit ? "true" : "false")
            << " max_wait_ms=" << metadata_sqlite_group_commit_max_wait_ms
            << " max_batch=" << metadata_sqlite_group_commit_max_batch << "\n"
            << "  transfer limit : 数据面 PUT max_body_bytes=" << transfer_put_max_body_bytes
            << "（0=不限；全局 server.http.transfer_max_body_bytes=" << transfer_max_body_bytes
            << "，partition.file." << partition_file.partition
            << ".max_file_bytes=" << partition_file.max_file_bytes << "，取较小者）\n"
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
                    ? "已接入 " + router_options.metrics_path + "（含存储计量 + fss_gc_*）"
                    : std::string("未接入（observability.metrics_enabled=false）"))
            << "\n"
            << "  audit          : "
            << (audit_enabled ? "开启" : "关闭（observability.audit_enabled=false）")
            << (audit_enabled && audit_fault_inject ? "（★故障注入：审计后端必然失败）" : "")
            << (audit_enabled
                    ? (audit_fail_closed ? "；fail_closed=true（审计失败 → 请求 500）"
                                         : "；fail_closed=false（审计失败非致命）")
                    : "")
            << "\n"
            << "  auth           : "
            << (auth_mode == "jwt"
                    ? "jwt（HS256 本地校验）"
                    : (auth_mode == "remote-entitlements"
                           ? "remote-entitlements（fail-closed，地址 " + entitlements_url + "）"
                           : "disabled（allow-all，仅开发/测试）"))
            << "\n"
            << "  auth claims    : roles_claim=" << jwt_roles_claim << "，local_roles="
            << local_roles.size() << " 个用户（auth.mode=jwt 时参与角色判定）\n"
            << "  validators     : legal=" << legal_validator_kind
            << (legal_validator_kind == "remote"
                    ? "（端点 " + legal_remote_base_url + "，timeout=" +
                          std::to_string(legal_remote_timeout_ms) +
                          "ms；fail-closed → 503；端点须允许无 per-request 认证）"
                    : "（空值防线：legaltags 为空 → 400；不发请求）")
            << " | schema=" << schema_validator_kind
            << (schema_validator_kind == "remote"
                    ? "（端点 " + schema_remote_base_url + "，timeout=" +
                          std::to_string(schema_remote_timeout_ms) +
                          "ms；fail-closed → 503；端点须允许无 per-request 认证）"
                    : "（不校验 schema；不发请求）")
            << "\n"
            << "  events         : publisher=" << events_publisher
            << (events_publisher == "webhook"
                    ? "（端点 " + events_webhook_url + "，timeout=" +
                          std::to_string(events_webhook_timeout_ms) + "ms，topic=" +
                          events_webhook_topic +
                          "；**发布失败非致命**：连不上/超时/非 2xx 只告警，请求照常成功；"
                          "同步发布 → 慢端点按「事件数 × timeout」增加请求延迟）"
                    : (events_publisher == "none"
                           ? std::string("（**显式关闭**：不发任何请求）")
                           : std::string("（写日志：status-changed / datasetDetails；不发请求）")))
            << "\n"
            << "  environment    : " << deployment_environment << "\n"
            << "  gc             : " << gc_banner << "\n"
            << "  expiry         : default=" << expiry_default_text << "（"
            << expiry_options.default_seconds << "s）max=" << expiry_max_text << "（"
            << expiry_options.max_seconds << "s）\n"
            << "  self signed    : key_id=" << transfer_key_id
            << "（进签名载荷；多密钥轮换未交付）TTL 自签上界 default="
            << self_signed_default_ttl_seconds << "s max=" << self_signed_max_ttl_seconds
            << "s（仅 !native_presign 分支；expiry.* 仍是 expiryTime 的解析规则与缺省）\n"
            << "  leases         : " << lease_backend
            << "（enabled=" << (leases_enabled ? "true" : "false")
            << " ttl=" << leases_ttl_seconds << "s renew=" << leases_renew_interval_seconds
            << "s time_source=" << leases_time_source
            << "；★ 上传路径尚未 Acquire/Renew，这三个键当前无效果）\n"
            //  ★ 上面这一行原本**硬编码**「内存租约」，在 `leases.enabled=true` 时与本行打印的
            //    `enabled=true` 自相矛盾（B1 复核发现）。现在打印的是**实际后端** `lease_backend`。
            //  ---- B1 新增行 ----
            << "  repositories   : metadata=" << metadata_repository_backend
            << (metadata_repository_backend == "postgres"
                    ? "（metadata.postgres.dsn=*** max_connections=" +
                          std::to_string(metadata_postgres_max_connections) +
                          " statement_timeout_ms=" +
                          std::to_string(metadata_postgres_statement_timeout_ms) + "）"
                    : "（" + metadata_db_path + "）")
            << " | location=" << location_repository_backend
            << (location_repository_backend == "postgres"
                    ? "（location.postgres.dsn=*** max_connections=" +
                          std::to_string(location_postgres_max_connections) +
                          "；该组无 statement_timeout_ms 键 → PgOptions 默认 5000ms）"
                    : "（" + sqlite_path + "）")
            << "\n"
            << "  instance id    : " << effective_instance_id
            << (instance_id_generated
                    ? "（★ multi 未配置/为空 → 自动生成唯一 id；默认 `local` 会让多实例共享"
                      "标识 → 共享挂载上临时文件名确定性撞名，ADR-009 M1 静默串数据）"
                    : std::string("（来自 deployment.instance_id，未改动）"))
            << "\n"
            << "  lease backend  : " << lease_backend << "\n"
            << "  leader         : " << leader_state_banner << "\n"
            //  ★ B2a：三条生产就绪判据的实际结论必须可见（否则"检查过没过"只能靠猜）。
            << "  pg budget      : " << pg_budget_banner << "\n"
            << "  clock skew     : " << clock_skew_banner << "\n"
            << "  pg schema check: "
            << (metadata_postgres_schema_version_check
                    ? "true（readiness 比对 schema_migrations.max(version) 与期望 " +
                          std::to_string(infra::kExpectedSchemaVersion) + "）"
                    : std::string("false（readiness 只做 SELECT 1 探活，不比对迁移版本）"))
            << "\n"
            //  ★ B2b：注册表 / 一致性 / 共享挂载探针的实际结论（同样不许"检查过没过靠猜"）。
            << "  instance reg   : " << instance_registry_banner << "\n"
            << "  consistency    : " << consistency_banner << "\n"
            << "  shared mount   : " << mount_probe_banner << "\n";
  //  ★ C9.26：测试接缝生效时**必须可见**（可运维：横幅回答"它会不会故意卡住"）。
  //    未设置时不打印该行 → 既有横幅逐字不变（既有测试只做子串断言，不受影响）。
  if (claim_hold_millis > 0) {
    std::cout << "  claim hold ms  : " << claim_hold_millis
              << "（★ C9.26 测试接缝 FSS_CLAIM_HOLD_MS：createMetadata 在原子领取后、"
                 "复制前阻塞该毫秒数；**生产禁止设置**）\n";
  }
  //  ★ B2a：时钟偏移注入接缝生效时同样必须可见（避免演练结论被误读成"这台机器钟真的偏了"）。
  if (clock_skew_inject_millis != 0) {
    std::cout << "  clock inject   : " << clock_skew_inject_millis
              << " ms（★ B2a 测试接缝 FSS_CLOCK_SKEW_INJECT_MS：本地钟被拨动该毫秒数；"
                 "**生产禁止设置**）\n";
  }
  //  C10.2：逐键打印来源缩写（cli/env/file/default），`(别名 FSS_X)` 表示旧环境变量。
  std::cout << "  config sources :\n";
  for (const auto& [path, row] : resolver.rows()) {
    std::cout << "    " << path << " [" << row.source;
    if (!row.alias_var.empty()) std::cout << " 别名 " << row.alias_var;
    std::cout << "]\n";
  }
  std::cout.flush();

  //  ---- 启动周期调度（在横幅之后：横幅要先回答"它会不会跑"）----
  if (gc_schedule) {
    gc_scheduler = std::make_unique<GcScheduler>(gc_task, gc_partition, gc_options,
                                                 gc_interval_seconds, logger, gc_leader_check);
    gc_scheduler->Start();
  }

  //  ---- 故障注入接缝的**晚注入点**（测试/演练用；**不是配置键**）----
  //  ★ 此时 HTTP 服务器已 `Start()`、GC 调度线程已在跑 —— 用于证明"异常路径上 RAII
  //    清理真的发生且不挂死"（`GcScheduler::~GcScheduler → Stop()+join`、
  //    `http::Server::~Server → Stop()`）。★ 只在主线程抛。
  MaybeInjectStartupFaultAfterStart();

  //  ★ 优雅停止（C10.9）：SIGINT/SIGTERM 只置位；主循环**轮询**该标志
  //    （不用固定 sleep 等状态 —— AGENTS §4.3），随后走**唯一的**退出路径。
  std::signal(SIGINT, HandleStopSignal);
  std::signal(SIGTERM, HandleStopSignal);
  while (g_stop_requested == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  //  统一退出路径：先停 GC 调度（signal + join；绝不留 joinable thread），
  //  再停 HTTP（Server::Stop 会 join 自己的 runner），最后 shutdown gRPC。
  if (gc_scheduler) gc_scheduler->Stop();
  server.Stop();

  //  ★ 退出路径必须**显式**关掉 gRPC 服务：`grpc::Server` 是 joinable 的资源，
  //    提前 return 或析构顺序不当会让进程挂在 gRPC 的线程池上（与"先 stop 再 join"
  //    同一条纪律，见 AGENTS.md §4.3）。
  if (grpc_handle) {
    grpc_handle->Shutdown();
    grpc_handle.reset();
  }
  grpc_service.reset();
  return 0;
}

// =============================================================================
//  main —— **薄包装**：只负责"未预期异常 → 可读原因 + 确定退出码 70"（C9.32）
// =============================================================================
//  ★ 这一层**不做清理**：所有资源都在 `RunServer` 的局部对象里，栈展开时 RAII 已完成。
//  ★ 消息里**不得**出现任何配置值/密钥：只允许 `what()` 与这段**静态**提示
//    （`what()` 可能含路径，但绝不含 `storage.s3.secret_key` 之类的配置值 ——
//    组合根从不把密钥放进异常消息；本文件里唯一的异常构造是故障注入接缝）。
int main(int argc, char** argv) {
  try {
    return RunServer(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "未预期异常（exit " << kExitInternalError << "）：" << e.what() << "\n"
              << "常见原因：容器 --pids-limit 过小导致线程创建 EAGAIN（见 docs/runbook.md）；"
                 "或内存不足（bad_alloc）。\n";
    return kExitInternalError;
  } catch (...) {
    std::cerr << "未预期异常（未知类型，exit " << kExitInternalError << "）\n";
    return kExitInternalError;
  }
}
