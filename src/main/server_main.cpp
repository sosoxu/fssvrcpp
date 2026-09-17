// =============================================================================
//  server_main（组合根）—— 配置 → 依赖构造 → 路由注册 → 启动监听
// =============================================================================
//  ★ R12：**所有具体实现只在这里被创建**。领域层/应用层/适配层都不 `new` 具体类型；
//     端口的实现由本文件注入。`scripts/verify_composition_root.sh`（P4 后续切片）
//     会机械检查这一点。
//
//  本切片的配置来源是**环境变量**（够用且可测）：
//     FSS_STORAGE_ROOT / FSS_SQLITE_PATH / FSS_TRANSFER_SECRET / FSS_HTTP_PORT
//     FSS_SELF_BASE_URL / FSS_BIND_ADDRESS
//  `config/fss.example.json` 的完整配置接入与校验在后续切片完成（配置项较多，
//  且要与 `docs/operations.md` 做自动比对）。
//
//  ⚠️ 鉴权在本阶段是 **allow-all**（只要求 token 非空），启动时必须打印显著告警
//     （契约 §8 的 C8.5）；P8 会替换成真实的 JWT/Entitlements 校验。
#include "adapters/grpc/file_service_adapter.h"
#include "adapters/grpc/grpc_server.h"
#include "adapters/http/router.h"
#include "app/services/location_issuer.h"
#include "app/usecases/usecases.h"
#include "common/http/http.h"
#include "common/ids/id_generator.h"
#include "common/logging/logging.h"
#include "common/result/result.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/blob/posix/posix_blob_store.h"
#include "infra/blob/s3/s3_blob_store.h"
#include "infra/location/sqlite/sqlite_location_repository.h"
#include "infra/metadata/memory/memory_metadata_repository.h"
#include "infra/transfer/blob_byte_source.h"
#include "infra/transfer/transfer_endpoint.h"
#include "infra/transfer/transfer_token.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <memory>
#include <string>

namespace {

std::string Env(const char* key, const std::string& fallback) {
  if (const char* value = std::getenv(key); value != nullptr && *value != '\0') return value;
  return fallback;
}

int EnvInt(const char* key, int fallback) {
  if (const char* value = std::getenv(key); value != nullptr && *value != '\0') {
    return std::atoi(value);
  }
  return fallback;
}

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

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  using namespace fss;
  using namespace fss::infra;

  //  ---- 驱动选择（C5.9：切换驱动**只改配置**，同一二进制）----
  const std::string storage_driver = Env("FSS_STORAGE_DRIVER", "posix");
  const std::string storage_root = Env("FSS_STORAGE_ROOT", "/tmp/fss-data");
  const std::string sqlite_path = Env("FSS_SQLITE_PATH", storage_root + "/location.db");
  const std::string transfer_secret = Env("FSS_TRANSFER_SECRET", "dev-secret-change-me");
  const int port = EnvInt("FSS_HTTP_PORT", 8080);
  //  ---- gRPC 面（P7）：与 REST **同一个进程、同一批端口实现**同时服务（C7.8）----
  //    0  = 关闭（默认关闭，保持"RPC 是平台外扩展"的默认姿态，ADR-001）
  //    -1 = 由系统分配端口（启动横幅里给出实际端口，供测试/脚本使用）
  const int grpc_port_config = EnvInt("FSS_GRPC_PORT", 0);
  const std::string bind_address = Env("FSS_BIND_ADDRESS", "0.0.0.0");
  //  ★ 自签 URL 的 `<base>` 必须**包含路由 base path**：数据面挂在
  //    `/api/file/v1/transfer/{token}`（契约 §7 / §2 表），若这里只给 `host:port`，
  //    发出去的上传地址会指向 404（P4-D04，端到端用例抓到）。
  const std::string base_path(adapters::http::kDefaultBasePath);
  const std::string self_base_url = Env(
      "FSS_SELF_BASE_URL", "http://127.0.0.1:" + std::to_string(port) + base_path);
  //  ---- S3 驱动配置（`storage.driver=s3` 时才读取）----
  const std::string s3_endpoint = Env("FSS_STORAGE_S3_ENDPOINT", "");
  const std::string s3_region = Env("FSS_STORAGE_S3_REGION", "us-east-1");
  const std::string s3_access_key = Env("FSS_STORAGE_S3_ACCESS_KEY", "");
  const std::string s3_secret_key = Env("FSS_STORAGE_S3_SECRET_KEY", "");
  const bool s3_force_path_style = Env("FSS_STORAGE_S3_FORCE_PATH_STYLE", "true") != "false";
  const bool s3_verify_tls = Env("FSS_STORAGE_S3_VERIFY_TLS", "true") != "false";

  //  ---- 组合根：唯一实例化具体实现的位置（R12）----
  SystemClock clock;
  logging::StreamLogger logger(logging::LogOptions{}, clock);
  logging::Warn(logger, "auth.mode=disabled（allow-all）：仅用于开发/测试，禁止用于生产",
                {{"component", "server_main"}});

  UuidGenerator ids;
  //  ★ 两种驱动都只在**组合根**创建（R12）。上层只看到 `IBlobStore` 与能力声明，
  //    因此"换驱动"对用例/适配层完全透明（C5.9 要证明的正是这一点）。
  std::unique_ptr<domain::IBlobStore> blob_store;
  if (storage_driver == "s3") {
    if (s3_endpoint.empty() || s3_access_key.empty() || s3_secret_key.empty()) {
      std::cerr << "storage.driver=s3 需要 FSS_STORAGE_S3_ENDPOINT / _ACCESS_KEY / _SECRET_KEY\n";
      return 1;
    }
    infra::S3Options s3_options;
    s3_options.endpoint = s3_endpoint;
    s3_options.region = s3_region;
    s3_options.force_path_style = s3_force_path_style;
    s3_options.verify_tls = s3_verify_tls;
    s3_options.credentials = infra::AwsCredentials{s3_access_key, s3_secret_key, ""};
    blob_store = std::make_unique<infra::S3BlobStore>(s3_options, clock);
  } else if (storage_driver == "posix") {
    std::error_code blob_dir_error;
    std::filesystem::create_directories(storage_root + "/blobs", blob_dir_error);
    if (blob_dir_error) {
      std::cerr << "创建存储根失败: " << blob_dir_error.message() << "\n";
      return 1;
    }
    blob_store = std::make_unique<PosixBlobStore>(storage_root + "/blobs", clock);
  } else {
    std::cerr << "未知的 storage.driver: " << storage_driver << "（可选：posix | s3）\n";
    return 1;
  }
  SingleStoreFactory blob_factory(*blob_store);

  auto location_repository = SqliteLocationRepository::Open(sqlite_path);
  if (!location_repository.ok()) {
    std::cerr << "打开位置仓储失败: " << location_repository.error().ToString() << "\n";
    return 1;
  }
  InMemoryMetadataRepository metadata_repository(clock);  // P6 换成 SQLite
  HmacTransferTokenCodec token_codec(transfer_secret, clock);

  AllowAllAuthorizer authorizer;
  LogEventPublisher events(logger);
  LogAuditLogger audit(logger);

  domain::PartitionConfig partition;
  partition.partition = "opendes";
  partition.driver = storage_driver == "s3" ? domain::StorageDriver::kS3
                                            : domain::StorageDriver::kPosix;
  partition.posix_root = storage_root;
  StaticPartitionRegistry partitions(partition);

  NoopLegalValidator legal;
  NoopSchemaValidator schema;

  app::LocationIssuer issuer(blob_factory, *location_repository.value(), token_codec, clock, ids,
                             self_base_url);

  app::UseCasePorts ports{blob_factory,      *location_repository.value(), metadata_repository,
                          authorizer,        events,                         audit,
                          partitions,        legal,                          schema,
                          issuer,            clock,                          ids};

  //  ---- 路由 ----
  adapters::http::RouterOptions router_options;
  router_options.error_format = adapters::http::ErrorFormat::kAppError;
  router_options.base_path = base_path;

  //  数据面回调：适配层不认识 L2，由这里把 TransferEndpoint 绑上去（R12）。
  //  ★ 只在**集中存储**模式下注册：S3 有原生预签名，客户端直连存储端点，
  //    服务不代理字节（C5.8）；此时注册 `/v1/transfer` 是死代码。
  infra::TransferEndpoint transfer_endpoint(token_codec, blob_factory);
  adapters::http::TransferCallbacks transfers;
  const bool with_self_signed_data_plane = storage_driver == "posix";
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
  }  // namespace 结束：仅 POSIX 模式注册自签数据面

  adapters::http::Router router(ports, transfers, router_options);

  //  ---- gRPC 面：与 REST 共用**同一个** `UseCasePorts`（C7.8：双协议同时运行）----
  //  ★ 与 HTTP 面一样，服务的具体实现只在这里创建（R12）；两个服务共用同一批
  //    端口实现（同一个 blob/location/metadata 实例），因此两条链路看到同一份状态。
  //  ★ 组合根**不直接**碰 grpc 类型：服务器的生命周期收在 `adapters/grpc/grpc_server.h`
  //    的包装里（C7.7 的护栏要求 `src/` 全树除 adapters/grpc/ 外不出现 `<grpcpp/`；
  //    与 `fss_http` 把 httplib 挡在适配层内是同一条纪律）。
  std::unique_ptr<adapters::grpc::FileServiceAdapter> grpc_service;
  std::unique_ptr<adapters::grpc::GrpcServerHandle> grpc_server;
  if (grpc_port_config != 0) {
    grpc_service = std::make_unique<adapters::grpc::FileServiceAdapter>(ports, "osdu-user");
    grpc_server =
        adapters::grpc::StartGrpcServer(*grpc_service, bind_address, grpc_port_config);
    if (!grpc_server->ok()) {
      std::cerr << grpc_server->last_error() << "\n";
      return 1;
    }
  }

  http::ServerOptions server_options;
  server_options.bind_address = bind_address;
  server_options.port = port;
  server_options.service_name = "file-service";
  server_options.error_format = "apperror";
  http::Server server(server_options, logger, clock);
  router.Register(server);

  if (!server.Bind()) {
    std::cerr << "监听失败: " << server.last_error() << "\n";
    return 1;
  }

  std::cout << "fss_server 已启动\n"
            << "  bind           : " << bind_address << ":" << server.port() << "\n"
            << "  grpc bind      : "
            << (grpc_server ? bind_address + ":" + std::to_string(grpc_server->port())
                            : std::string("disabled（FSS_GRPC_PORT=0）"))
            << "\n"
            << "  base path      : " << router_options.base_path << "\n"
            << "  storage driver : " << storage_driver << "\n"
            << "  storage root   : " << storage_root << "\n"
            << "  sqlite path    : " << sqlite_path << "\n"
            << "  error format   : " << adapters::http::ErrorFormatName(router_options.error_format)
            << "\n"
            << "  auth           : disabled（allow-all，仅开发/测试）\n";
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
