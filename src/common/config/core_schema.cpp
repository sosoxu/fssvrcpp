// P1 阶段的核心字段集。
//
// 说明：
//   * 字段声明是**声明式**的 —— 加字段只需在下面加一行，校验（类型/范围/枚举/
//     必填/未知键/环境变量展开/打码）自动生效。
//   * 这里的范围刻意覆盖 config/fss.example.json 的键；示例文件由
//     tests/unit/test_config.cpp 的一部分用例做"键集一致性"检查，
//     防止示例与 schema 漂移。
//   * 后续阶段（P2~P9）按需扩展；扩展时同步 config/fss.example.json。
#include "common/config/config.h"

#include <string>
#include <vector>

namespace fss::config {

namespace {
json::Value GetOr(const json::Value& root, const std::string& path, const char* fallback) {
  const json::Value* cur = &root;
  std::size_t start = 0;
  while (true) {
    if (!cur->is_object()) return json::Value(fallback);
    const auto dot = path.find('.', start);
    const std::string seg = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    auto it = cur->find(seg);
    if (it == cur->end()) return json::Value(fallback);
    if (dot == std::string::npos) return *it;
    cur = &(*it);
    start = dot + 1;
  }
}
// 把标量渲染成字符串。⚠️ P1-D08：必须处理布尔/数字——曾直接 "非 string 就返回 fallback"，
// 于是 `"leases.enabled": true` 被读成 fallback "false"，multi 模式的跨字段校验
// **永远无法通过**（配置写对了也报错）。
std::string RenderScalar(const json::Value& v, const char* fallback) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
  if (v.is_number_float()) return v.dump();
  return std::string(fallback);
}
std::string StrOr(const json::Value& root, const std::string& path, const char* fallback) {
  return RenderScalar(GetOr(root, path, fallback), fallback);
}
}  // namespace

Schema CoreSchema() {
  Schema s;

  // ---------------- deployment ----------------
  s.Add(FieldSpec{"deployment.environment"}
            .Str("development=开发/测试；production=生产（触发更严格的强制校验）")
            .Enum({"development", "production"}).Default("development"));
  //  时钟偏差容忍范围（秒）：本地钟与参考钟（多实例下 = 数据库 now()）的允许偏差。
  //  ★ TTL/租约/过期判定一律用参考钟；本值只决定"偏差多大时拒绝服务"（C8.10）
  s.Add(FieldSpec{"deployment.max_clock_skew_seconds"}
            .Int(0, 300, "多实例下不得超过 60（容忍范围过大等于放任误判）").Default("5"));
  s.Add(FieldSpec{"deployment.mode"}.Str("single=单实例；multi=多实例")
          .Enum({"single", "multi"}).Default("single"));
  s.Add(FieldSpec{"deployment.instance_id"}.Str("空则自动生成；K8s 下建议注入 POD_NAME").Default(""));
  s.Add(FieldSpec{"deployment.clock_skew_tolerance_seconds"}
          .Int(0, 86400, "与数据库 now() 的偏移容忍；超限拒绝启动").Default("60"));

  // ---------------- server.http ----------------
  s.Add(FieldSpec{"server.http.base_path"}.Str("必须与上游 context path 一致").Required().Default("/api/file"));
  s.Add(FieldSpec{"server.http.bind"}.Str().Default("0.0.0.0"));
  s.Add(FieldSpec{"server.http.port"}.Int(1, 65535, "监听端口").Default("8080"));
  s.Add(FieldSpec{"server.http.max_header_bytes"}.Int(1024, 1048576).Default("16384"));
  s.Add(FieldSpec{"server.http.max_uri_bytes"}.Int(128, 1048576).Default("8192"));
  s.Add(FieldSpec{"server.http.max_body_bytes"}.Int(1, 1073741824).Default("10485760"));
  s.Add(FieldSpec{"server.http.tcp_nodelay"}
          .Bool("★必须为 true：关闭时小请求有 ~40ms delayed-ACK 停顿（实测 950x）").Default("true"));
  s.Add(FieldSpec{"server.http.worker_threads"}
          .Int(0, 65536, "语义是并发连接上限；0=按公式推导").Default("0"));
  s.Add(FieldSpec{"server.http.max_connections"}.Int(0, 65536).Default("0"));
  s.Add(FieldSpec{"server.http.max_connections_per_partition"}.Int(0, 65536).Default("0"));
  s.Add(FieldSpec{"server.http.idle_timeout_seconds"}.Int(1, 86400).Default("60"));
  s.Add(FieldSpec{"server.http.json_request_timeout_seconds"}.Int(1, 86400).Default("15"));
  s.Add(FieldSpec{"server.http.transfer_idle_timeout_seconds"}
          .Int(1, 86400, "数据面只有“空闲无进展”超时，无整体超时").Default("120"));
  s.Add(FieldSpec{"server.http.transfer_max_body_bytes"}.Int(0, 0, "0=不限").Default("0"));
  s.Add(FieldSpec{"server.http.transfer_buffer_bytes"}.Int(4096, 67108864).Default("262144"));
  s.Add(FieldSpec{"server.http.transfer_memory_budget_bytes"}
          .Int(1048576, 0, "并发×缓冲 ≤ 该值，违反拒绝启动").Default("268435456"));
  s.Add(FieldSpec{"server.http.large_file_plane.enabled"}.Bool().Default("false"));
  s.Add(FieldSpec{"server.http.large_file_plane.bind"}.Str().Default("0.0.0.0"));
  s.Add(FieldSpec{"server.http.large_file_plane.port"}.Int(1, 65535).Default("8081"));
  s.Add(FieldSpec{"server.http.large_file_plane.use_sendfile"}.Bool().Default("true"));
  s.Add(FieldSpec{"server.http.large_file_plane.sendfile_chunk_bytes"}.Int(65536, 0).Default("1073741824"));
  s.Add(FieldSpec{"server.http.large_file_plane.workers"}.Int(0, 65536).Default("0"));
  s.Add(FieldSpec{"server.http.large_file_plane.max_connections"}.Int(0, 65536).Default("0"));

  // ---------------- server.grpc ----------------
  s.Add(FieldSpec{"server.grpc.enabled"}.Bool().Default("true"));
  s.Add(FieldSpec{"server.grpc.bind"}.Str().Default("0.0.0.0"));
  s.Add(FieldSpec{"server.grpc.port"}.Int(1, 65535).Default("50051"));
  s.Add(FieldSpec{"server.grpc.max_message_bytes"}.Int(1024, 0).Default("4194304"));
  s.Add(FieldSpec{"server.grpc.streaming_chunk_bytes"}.Int(4096, 0).Default("262144"));

  // ---------------- storage ----------------
  s.Add(FieldSpec{"storage.driver"}.Str("posix=集中存储；s3=对象存储")
          .Enum({"posix", "s3"}).Default("posix"));
  s.Add(FieldSpec{"storage.proxy_mode"}.Enum({"auto", "always"}).Default("auto"));
  s.Add(FieldSpec{"storage.driver_report_override"}
          .Str("上游把 Driver 硬编码为 GCS；留空则上报真实驱动").Default(""));
  s.Add(FieldSpec{"storage.provider_key_override"}.Str().Default(""));
  s.Add(FieldSpec{"storage.io_engine"}
          .Str("blocking=默认（到处能跑）；uring=需环境允许（容器 seccomp 常阻断）；auto=探测后择优")
          .Enum({"blocking", "uring", "auto"}).Default("blocking"));
  s.Add(FieldSpec{"storage.io_uring.queue_depth"}.Int(1, 4096).Default("64"));
  s.Add(FieldSpec{"storage.io_uring.register_files"}.Bool().Default("false"));
  s.Add(FieldSpec{"storage.posix.root"}.Str("集中存储根目录").Required().Default("/var/lib/fss/data"));
  s.Add(FieldSpec{"storage.posix.durability"}
          .Str("batch=两阶段批提交（ADR-008，推荐）；per_file=每文件 fdatasync（精确但慢 82x）")
          .Enum({"batch", "per_file"}).Default("batch"));
  s.Add(FieldSpec{"storage.posix.group_commit_max_batch"}.Int(1, 100000).Default("500"));
  s.Add(FieldSpec{"storage.posix.sync_dir_after_batch"}.Bool("R2 不变量：必须 true").Default("true"));
  s.Add(FieldSpec{"storage.posix.fsync_threshold_bytes"}.Int(0, 0, ">0 时大于该值的文件强制 per_file").Default("0"));
  s.Add(FieldSpec{"storage.posix.atomic_write"}.Bool().Default("true"));
  s.Add(FieldSpec{"storage.posix.dir_mode"}.Str().Default("0750"));
  s.Add(FieldSpec{"storage.posix.file_mode"}.Str().Default("0640"));
  s.Add(FieldSpec{"storage.posix.fadvise_random"}.Bool().Default("true"));
  s.Add(FieldSpec{"storage.posix.fadvise_dontneed_after_large_read"}.Bool().Default("false"));
  s.Add(FieldSpec{"storage.posix.shared_mount_required"}.Bool("multi 下必须 true").Default("false"));
  s.Add(FieldSpec{"storage.posix.one_filesystem_per_partition"}.Bool().Default("false"));
  s.Add(FieldSpec{"storage.s3.endpoint"}.Str().Default("http://127.0.0.1:9000"));
  s.Add(FieldSpec{"storage.s3.region"}.Str().Default("us-east-1"));
  s.Add(FieldSpec{"storage.s3.access_key"}.Str().Secret().Default(""));
  s.Add(FieldSpec{"storage.s3.secret_key"}.Str().Secret().Default(""));
  s.Add(FieldSpec{"storage.s3.force_path_style"}.Bool().Default("true"));
  s.Add(FieldSpec{"storage.s3.verify_tls"}.Bool().Default("true"));
  s.Add(FieldSpec{"storage.s3.connect_timeout_ms"}.Int(1, 600000).Default("3000"));
  s.Add(FieldSpec{"storage.s3.total_timeout_ms"}.Int(1, 3600000).Default("30000"));
  s.Add(FieldSpec{"storage.s3.presign_default_seconds"}.Int(1, 604800).Default("3600"));
  s.Add(FieldSpec{"storage.s3.presign_max_seconds"}.Int(1, 604800, "对齐 OSDU 上限 7 天").Default("604800"));

  // ---------------- self_signed ----------------
  s.Add(FieldSpec{"self_signed.enabled"}.Bool().Default("true"));
  s.Add(FieldSpec{"self_signed.public_base_url"}.Str("对外可达地址，用于生成自签 URL").Default("http://127.0.0.1:8080"));
  s.Add(FieldSpec{"self_signed.signing_key"}.Str("★所有实例必须共享").Secret().Default(""));
  s.Add(FieldSpec{"self_signed.key_id"}.Str().Default("k1"));
  s.Add(FieldSpec{"self_signed.default_ttl_seconds"}.Int(1, 604800).Default("3600"));
  s.Add(FieldSpec{"self_signed.max_ttl_seconds"}.Int(1, 604800).Default("604800"));
  s.Add(FieldSpec{"self_signed.single_use_nonce"}
          .Bool("多实例下必须 false：本地 nonce 表无法跨实例拒绝重放").Default("false"));
  s.Add(FieldSpec{"self_signed.nonce_store"}.Enum({"memory", "postgres"}).Default("memory"));

  // ---------------- expiry / http ----------------
  s.Add(FieldSpec{"expiry.default"}.Str("对齐上游实测：缺省 1 小时").Default("1H"));
  s.Add(FieldSpec{"expiry.max"}.Str("超限静默截断为 7 天").Default("7D"));
  s.Add(FieldSpec{"http.error_format"}.Str("apperror=OSDU 标准").Enum({"apperror", "legacy", "api_error"}).Default("apperror"));

  // ---------------- metadata / location ----------------
  s.Add(FieldSpec{"metadata.repository"}.Str("multi 下必须是 postgres")
          .Enum({"sqlite", "postgres", "remote"}).Default("sqlite"));
  s.Add(FieldSpec{"metadata.sqlite.path"}.Str().Default("/var/lib/fss/meta.db"));
  s.Add(FieldSpec{"metadata.sqlite.busy_timeout_ms"}.Int(0, 600000).Default("5000"));
  s.Add(FieldSpec{"metadata.sqlite.journal_mode"}.Enum({"WAL", "DELETE", "TRUNCATE"}).Default("WAL"));
  s.Add(FieldSpec{"metadata.sqlite.synchronous"}.Str("FULL 比 NORMAL 慢 21x").Enum({"NORMAL", "FULL", "OFF"}).Default("NORMAL"));
  s.Add(FieldSpec{"metadata.sqlite.max_write_concurrency"}.Int(1, 256, "实测 8 为峰值").Default("8"));
  s.Add(FieldSpec{"metadata.sqlite.group_commit"}.Bool().Default("true"));
  s.Add(FieldSpec{"metadata.sqlite.group_commit_max_wait_ms"}.Int(0, 1000).Default("5"));
  s.Add(FieldSpec{"metadata.sqlite.group_commit_max_batch"}.Int(1, 100000).Default("64"));
  s.Add(FieldSpec{"metadata.postgres.dsn"}.Str().Secret().Default(""));
  s.Add(FieldSpec{"metadata.postgres.max_connections"}.Int(1, 10000).Default("16"));
  s.Add(FieldSpec{"metadata.postgres.statement_timeout_ms"}.Int(1, 600000).Default("5000"));
  s.Add(FieldSpec{"metadata.postgres.schema_version_check"}.Bool().Default("true"));
  s.Add(FieldSpec{"metadata.remote.base_url"}.Str().Default(""));
  s.Add(FieldSpec{"metadata.remote.token_provider"}.Str().Default("static"));
  s.Add(FieldSpec{"metadata.remote.static_token"}.Str().Secret().Default(""));
  s.Add(FieldSpec{"metadata.remote.timeout_ms"}.Int(1, 600000).Default("5000"));
  s.Add(FieldSpec{"location.repository"}.Enum({"sqlite", "postgres"}).Default("sqlite"));
  s.Add(FieldSpec{"location.sqlite.path"}.Str().Default("/var/lib/fss/location.db"));
  s.Add(FieldSpec{"location.sqlite.busy_timeout_ms"}.Int(0, 600000).Default("10000"));
  s.Add(FieldSpec{"location.sqlite.journal_mode"}.Enum({"WAL", "DELETE", "TRUNCATE"}).Default("WAL"));
  s.Add(FieldSpec{"location.sqlite.synchronous"}.Enum({"NORMAL", "FULL", "OFF"}).Default("NORMAL"));
  s.Add(FieldSpec{"location.sqlite.max_write_concurrency"}.Int(1, 256).Default("8"));
  s.Add(FieldSpec{"location.sqlite.group_commit"}.Bool().Default("true"));
  s.Add(FieldSpec{"location.sqlite.group_commit_max_wait_ms"}.Int(0, 1000).Default("5"));
  s.Add(FieldSpec{"location.sqlite.group_commit_max_batch"}.Int(1, 100000).Default("64"));
  s.Add(FieldSpec{"location.postgres.dsn"}.Str().Secret().Default(""));
  s.Add(FieldSpec{"location.postgres.max_connections"}.Int(1, 10000).Default("8"));

  // ---------------- leases / leader ----------------
  s.Add(FieldSpec{"leases.enabled"}.Bool("multi 下必须 true").Default("false"));
  s.Add(FieldSpec{"leases.ttl_seconds"}.Int(1, 86400).Default("60"));
  s.Add(FieldSpec{"leases.renew_interval_seconds"}.Int(1, 86400, "建议 ttl/3").Default("20"));
  s.Add(FieldSpec{"leases.time_source"}.Enum({"database", "local"}).Default("database"));
  s.Add(FieldSpec{"leader_election.enabled"}.Bool("multi 下必须 true").Default("false"));
  s.Add(FieldSpec{"leader_election.backend"}.Enum({"postgres_advisory_lock"}).Default("postgres_advisory_lock"));
  s.Add(FieldSpec{"leader_election.lock_key"}.Int(0, 2147483647).Default("1179865927"));

  // ---------------- auth / legal / schema / events ----------------
  s.Add(FieldSpec{"auth.mode"}.Str("jwt=本地校验；remote-entitlements=远端；disabled=仅开发")
          .Enum({"jwt", "remote-entitlements", "disabled"}).Default("jwt"));
  s.Add(FieldSpec{"auth.jwt.jwks_url"}.Str().Default(""));
  s.Add(FieldSpec{"auth.jwt.issuer"}.Str().Default(""));
  s.Add(FieldSpec{"auth.jwt.audience"}.Str().Default(""));
  s.Add(FieldSpec{"auth.jwt.verify_signature"}.Bool().Default("true"));
  s.Add(FieldSpec{"auth.jwt.roles_claim"}.Str().Default("roles"));
  s.Add(FieldSpec{"auth.jwt.user_id_claim"}.Str().Default("email"));
  //  HS256 共享密钥（ADR-012 §5.1）。verify_signature=true 时为空 → 实例**拒绝所有 token**
  s.Add(FieldSpec{"auth.jwt.hmac_secret"}
            .Str("HS256 共享密钥；为空且开启验签时本实例拒绝一切 token（fail-closed）")
            .Default(""));
  //  租户绑定：token 里必须带这个 claim，且与请求头 `data-partition-id` 一致（ADR-012 §3）
  s.Add(FieldSpec{"auth.jwt.partition_claim"}.Str("租户 claim 名").Default("data-partition-id"));
  s.Add(FieldSpec{"auth.jwt.require_partition_claim"}
            .Bool("必须 true：否则 A 租户的 token 可以配 B 的请求头读 B 的数据")
            .Default("true"));
  s.Add(FieldSpec{"auth.remote_entitlements.base_url"}.Str().Default(""));
  //  与 Entitlements 约定的路径（契约 §4.5）；可配置以便适配真实服务
  s.Add(FieldSpec{"auth.remote_entitlements.authorize_path"}
            .Str("authorizeAny 的路径").Default("/api/entitlements/v2/authorizeAny"));
  s.Add(FieldSpec{"auth.remote_entitlements.connect_timeout_ms"}.Int(1, 600000).Default("1000"));
  s.Add(FieldSpec{"auth.remote_entitlements.timeout_ms"}.Int(1, 600000).Default("3000"));
  s.Add(FieldSpec{"auth.remote_entitlements.fail_closed"}
          .Bool("必须 true：依赖不可用不可降级为放行").Default("true"));
  s.AllowDynamicPrefix("auth.local_roles");
  s.Add(FieldSpec{"legal.validator"}.Enum({"noop", "remote"}).Default("noop"));
  s.Add(FieldSpec{"legal.remote.base_url"}.Str().Default(""));
  s.Add(FieldSpec{"legal.remote.timeout_ms"}.Int(1, 600000).Default("3000"));
  s.Add(FieldSpec{"schema.validator"}.Enum({"noop", "remote"}).Default("noop"));
  s.Add(FieldSpec{"schema.remote.base_url"}.Str().Default(""));
  s.Add(FieldSpec{"schema.remote.timeout_ms"}.Int(1, 600000).Default("3000"));
  s.Add(FieldSpec{"events.publisher"}.Enum({"log", "webhook", "none"}).Default("log"));
  s.Add(FieldSpec{"events.webhook.url"}.Str().Default(""));
  s.Add(FieldSpec{"events.webhook.timeout_ms"}.Int(1, 600000).Default("3000"));
  s.Add(FieldSpec{"events.webhook.topic"}.Str().Default("status-changed"));

  // ---------------- partition（动态子树） / gc / observability ----------------
  s.AllowDynamicPrefix("partition.file");
  s.Add(FieldSpec{"partition.registry"}.Enum({"file", "remote"}).Default("file"));
  s.Add(FieldSpec{"gc.enabled"}.Bool().Default("false"));
  s.Add(FieldSpec{"gc.dry_run"}.Bool("默认只记录不删除").Default("true"));
  s.Add(FieldSpec{"gc.require_lease_expiry"}
          .Bool("★必须 true：禁止“无元数据记录即删”（实测 20/20 误删在途上传）").Default("true"));
  s.Add(FieldSpec{"gc.staging_ttl_hours"}.Int(1, 87600).Default("24"));
  s.Add(FieldSpec{"gc.orphan_grace_hours"}.Int(1, 87600).Default("72"));
  s.Add(FieldSpec{"gc.interval_seconds"}.Int(1, 86400).Default("3600"));
  s.Add(FieldSpec{"observability.log_level"}.Enum({"debug", "info", "warn", "error"}).Default("info"));
  s.Add(FieldSpec{"observability.log_format"}.Enum({"json", "text"}).Default("json"));
  s.Add(FieldSpec{"observability.audit_enabled"}.Bool().Default("true"));
  s.Add(FieldSpec{"observability.audit_fail_closed"}.Bool().Default("false"));
  s.Add(FieldSpec{"observability.metrics_enabled"}.Bool().Default("true"));
  s.Add(FieldSpec{"observability.metrics_path"}.Str().Default("/metrics"));
  // 数组型字段：默认值用逗号分隔表达（loader 对 kStringList 也解析 default_value）
  s.Add(FieldSpec{"observability.redact_keys"}
            .List("日志/诊断输出中要打码的键名（子串匹配；大小写不敏感，且忽略 _ 与 -，故 secret_key/secretKey/SECRET-KEY 等效）")
            .Default("secret_key,access_key,token,sig,signature,authorization,x-amz-signature,password,signing_key,dsn,static_token"));
  s.AddCrossCheck(
      [](const json::Value& c) {
        std::vector<std::pair<std::string, std::string>> problems;
        // multi 模式的强制校验（ADR-009 §8.1）—— C8.9 会在此之上补更多
        const std::string mode = StrOr(c, "deployment.mode", "single");
        if (mode == "multi") {
          const std::string repo = StrOr(c, "metadata.repository", "sqlite");
          if (repo != "postgres")
            problems.emplace_back("metadata.repository", "deployment.mode=multi 时必须是 postgres（否则各实例状态发散）");
          const std::string locrepo = StrOr(c, "location.repository", "sqlite");
          if (locrepo != "postgres")
            problems.emplace_back("location.repository", "deployment.mode=multi 时必须是 postgres");
          if (StrOr(c, "leases.enabled", "false") != "true")
            problems.emplace_back("leases.enabled", "multi 时必须开启（否则 GC 会误删在途上传）");
          if (StrOr(c, "leader_election.enabled", "false") != "true")
            problems.emplace_back("leader_election.enabled", "multi 时必须开启（GC 必须单例运行）");
          if (StrOr(c, "storage.posix.shared_mount_required", "false") != "true")
            problems.emplace_back("storage.posix.shared_mount_required", "multi 时必须为 true");
          //  ★ GC 必须要求"租约到期"（ADR-009：无元数据记录即在途，不能当孤儿删）
          if (StrOr(c, "gc.require_lease_expiry", "true") != "true")
            problems.emplace_back("gc.require_lease_expiry",
                                  "multi 时必须为 true（否则 GC 会误删在途上传）");
          //  ★ 时钟偏差容忍范围必须存在且足够小（C8.10）：过大等于放任误判
          const auto skew = GetOr(c, "deployment.max_clock_skew_seconds", "5");
          if (skew.is_number_integer() && (skew.get<long>() <= 0 || skew.get<long>() > 60)) {
            problems.emplace_back("deployment.max_clock_skew_seconds",
                                  "multi 时必须 > 0 且 <= 60 秒（时钟偏差过大会让租约/过期误判）");
          }
        }
        // ---- 认证（ADR-012 / C8.5）----
        const std::string environment = StrOr(c, "deployment.environment", "development");
        const std::string auth_mode = StrOr(c, "auth.mode", "jwt");
        const std::string verify_signature = StrOr(c, "auth.jwt.verify_signature", "true");
        const std::string hmac_secret = StrOr(c, "auth.jwt.hmac_secret", "");
        const std::string jwks_url = StrOr(c, "auth.jwt.jwks_url", "");
        if (environment == "production" && auth_mode == "disabled") {
          problems.emplace_back("auth.mode",
                                "production 环境不允许 disabled（那等于没有鉴权）");
        }
        if (environment == "production" && verify_signature != "true") {
          problems.emplace_back("auth.jwt.verify_signature",
                                "production 环境必须为 true（不接受不验签的 token）");
        }
        //  ★ 只在 production 强制"必须有密钥"：空密钥在开发环境是**允许**的
        //    （语义是"本实例拒绝所有 token" + 启动显著告警），否则 schema 的默认值
        //    会与自己的校验冲突 —— Load({}) 直接失败，默认配置根本起不来。
        if (environment == "production" && auth_mode == "jwt" && verify_signature == "true" &&
            hmac_secret.empty()) {
          problems.emplace_back("auth.jwt.hmac_secret",
                                "production 环境必须配置共享密钥"
                                "（否则本实例会拒绝所有 token）");
        }
        const std::string remote_base = StrOr(c, "auth.remote_entitlements.base_url", "");
        const std::string remote_fail_closed =
            StrOr(c, "auth.remote_entitlements.fail_closed", "true");
        if (auth_mode == "remote-entitlements") {
          //  ★ 本地实现与配置必须一致：远端模式要求①有地址②fail_closed=true ——
          //    "依赖坏了就放行"不是可配置项，是本项目明确拒绝的语义（ADR-012 §5.1）
          if (remote_base.empty()) {
            problems.emplace_back("auth.remote_entitlements.base_url",
                                  "auth.mode=remote-entitlements 时必须配置 Entitlements 地址");
          }
          if (remote_fail_closed != "true") {
            problems.emplace_back("auth.remote_entitlements.fail_closed",
                                  "必须为 true：依赖不可用不可降级为放行");
          }
        }
        if (!jwks_url.empty()) {
          problems.emplace_back("auth.jwt.jwks_url",
                                "RS256/JWKS 尚未实现（ADR-012 §5.3）：配置它会让服务"
                                "「看起来配了鉴权」，因此这里直接拒绝启动");
        }

        // 传输内存预算：并发 × 缓冲 ≤ 预算
        const auto buf = GetOr(c, "server.http.transfer_buffer_bytes", "262144");
        const auto budget = GetOr(c, "server.http.transfer_memory_budget_bytes", "268435456");
        const auto workers = GetOr(c, "server.http.worker_threads", "0");
        if (buf.is_number_integer() && budget.is_number_integer() && workers.is_number_integer() &&
            workers.get<long>() > 0 && buf.get<long>() * workers.get<long>() > budget.get<long>()) {
          problems.emplace_back("server.http.transfer_memory_budget_bytes",
                                "并发上限(" + std::to_string(workers.get<long>()) + ") × 缓冲(" +
                                    std::to_string(buf.get<long>()) + ") 超过预算(" +
                                    std::to_string(budget.get<long>()) + ")");
        }
        // 续租间隔必须小于 TTL（否则租约会在续租前过期）
        const auto ttl = GetOr(c, "leases.ttl_seconds", "60");
        const auto renew = GetOr(c, "leases.renew_interval_seconds", "20");
        if (ttl.is_number_integer() && renew.is_number_integer() && renew.get<long>() >= ttl.get<long>()) {
          problems.emplace_back("leases.renew_interval_seconds",
                                "必须小于 leases.ttl_seconds（否则租约会在续租前过期）");
        }
        return problems;
      },
      "consistency");

  return s;
}

}  // namespace fss::config
