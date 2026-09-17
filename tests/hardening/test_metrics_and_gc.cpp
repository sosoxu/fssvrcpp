// =============================================================================
//  P9 硬化：指标（C9.6）+ GC 的 `.tmp_*` 清理（C9.5/C9.25）
// =============================================================================
//  C9.6 的三件事，各一条断言：
//    ① `/metrics` 暴露**全部**指标（HTTP 请求/延迟 + 存储操作/字节 + GC + 自签校验失败）
//    ② 格式合法（每行都是 `name{labels} value`；每个 family 都有 HELP/TYPE）
//    ③ **不含任何 secret**（扫描渲染结果里是否出现密钥/token 明文）
//
//  C9.25 的两件事：
//    ① GC 能识别并删除**够旧**的 `.tmp_*`（且计入 `tmp_removed`）；
//    ② "绝不把 `.tmp_*` 视为有效对象"的**反向测试**：即使某条位置记录恰好指向
//       一个 `.tmp_*` 键，GC 仍然删它（因为那种键不可能由 `ObjectKeyPolicy` 生成）。
//       另外：**太新**的 `.tmp_*` 必须被保护（在途上传）。
// =============================================================================
#include <catch2/catch.hpp>

#include "app_fixture.h"
#include "http_fixture.h"
#include "raw_http.h"

#include "app/tasks/gc_task.h"
#include "common/metrics/metrics.h"
#include "infra/blob/metered/metered_blob_store.h"
#include "infra/transfer/transfer_token.h"

#include <utime.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

using fss::test::Authed;
using fss::test::HttpDo;

//  ---- 把 POSIX 存储 + 指标注册表 + GC 装在一起（真实文件系统，便于造 .tmp_*） ----
struct MetricsFixture {
  fss::test::TempDir dir{"gc_metrics"};
  fss::ManualClock clock{1700000000};
  fss::SequentialIdGenerator ids{1};
  fss::infra::PosixBlobStore posix;
  fss::metrics::Registry registry;
  //  按声明顺序初始化：每层只依赖它前面的成员（全部按值持有 → 引用稳定）
  fss::infra::MeteredBlobStore blob{posix, registry, "posix"};
  fss::test::FakeBlobStoreFactory factory{blob};
  fss::infra::InMemoryLocationRepository locations;
  fss::infra::InMemoryMetadataRepository metadata{clock};
  //  ★ 必须用**能解码**的编解码器：数据面 PUT 会 `Decode` 自签 token，
  //    `RecordingSelfSignedCodec::Decode` 恒返回 kUnimplemented（它只用于"记录签发"的用例）→
  //    handler 立刻失败 → 见 P9-D05（真正的响应被"读体失败 400"覆盖）。
  fss::infra::HmacTransferTokenCodec codec{"gc-metrics-secret", clock};
  fss::test::AllowAllAuthorizer authorizer;
  fss::test::RecordingEventPublisher events;
  fss::test::RecordingAuditLogger audit;
  fss::test::FakePartitionRegistry partitions;
  fss::test::NoopLegalValidator legal;
  fss::test::NoopSchemaValidator schema;
  fss::infra::TransferEndpoint endpoint{codec, factory};
  //  ★ 自签 URL 的 base 必须含路由 base path（否则 PUT 会 404 —— P4-D04 的同类）
  fss::app::LocationIssuer issuer{factory, locations, codec, clock, ids,
                                  std::string("http://127.0.0.1") +
                                      std::string(fss::adapters::http::kDefaultBasePath)};
  std::unique_ptr<fss::app::UseCasePorts> ports;
  fss::logging::MemoryLogger logger;
  std::unique_ptr<fss::adapters::http::Router> router;
  std::unique_ptr<fss::http::Server> server;

  explicit MetricsFixture(fss::adapters::http::RouterOptions options = {})
      : posix(dir.child("blobs"), clock) {
    ports = std::make_unique<fss::app::UseCasePorts>(fss::app::UseCasePorts{
        factory, locations, metadata, authorizer, events, audit, partitions, legal, schema, issuer,
        clock, ids});
    //  ★ 本夹具永远把自己的注册表接进 `/metrics`（测试要断言的就是它渲染出来的文本）
    options.metrics_registry = &registry;
    fss::test::WireRouterAndServer(*this, std::move(options), {});
  }
  ~MetricsFixture() {
    if (server) server->Stop();
  }
  MetricsFixture(const MetricsFixture&) = delete;
  MetricsFixture& operator=(const MetricsFixture&) = delete;

  int port() const { return server->port(); }

  //  staging 容器目录（POSIX 驱动按 `<partition>-staging` 建目录）
  std::string staging_dir() const { return dir.child("blobs") + "/opendes-staging"; }

  //  造一个 `.tmp_*` 文件，`age_seconds` 秒前修改（GC 的 TTL 判据看 mtime）
  std::string MakeTempFile(const std::string& key, std::int64_t age_seconds) {
    std::filesystem::create_directories(staging_dir());
    const std::string path = staging_dir() + "/" + key;
    {
      std::ofstream out(path);
      out << "partial-upload-bytes";
    }
    //  ★ 用 `utime` 直接改 mtime：`std::filesystem::last_write_time` 与
    //    `system_clock` 的转换在 gcc 11 上不是隐式的（要 file_clock 转换）
    struct utimbuf times {};
    times.actime = static_cast<std::time_t>(clock.NowEpochSeconds() - age_seconds);
    times.modtime = times.actime;
    REQUIRE(::utime(path.c_str(), &times) == 0);
    return path;
  }
};

//  Prometheus 文本里的一条样本行：`name{labels} value` 或 `name value`
bool LooksLikeSampleLine(const std::string& line) {
  static const std::regex kSample(R"(^[a-zA-Z_:][a-zA-Z0-9_:]*(\{[^}]*\})? -?[0-9.eE+]+$)");
  return std::regex_match(line, kSample);
}

}  // namespace

TEST_CASE("★ C9.6 /metrics：暴露 HTTP + 存储 + GC 指标，格式合法，且不含 secret",
          "[phase9][hardening][c9.6]") {
  MetricsFixture fx;
  const int port = fx.port();

  //  ---- 造真实流量：上传 → 登记 → 下载 → 删除；再跑两轮 GC（dry-run + 真删）----
  const std::string secret = "transfer-secret-must-not-leak";
  {
    const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
    REQUIRE(upload.status == 200);
    const auto json = fss::json::ParseObject(upload.body);
    REQUIRE(json.ok());
    const std::string file_id = json.value()["FileID"].get<std::string>();
    const std::string file_source = json.value()["Location"]["FileSource"].get<std::string>();
    const std::string signed_url = json.value()["Location"]["SignedURL"].get<std::string>();

    const std::string payload = secret;  // ★ 故意把"secret"当**内容**上传：它不该出现在指标里
    fss::test::RawClient put(port, true);
    REQUIRE(put.Connect());
    REQUIRE(put.SendRequest("PUT", fss::test::TargetOf(signed_url),
                            {"Content-Length: " + std::to_string(payload.size()),
                             "data-partition-id: opendes"},
                            payload));
    const auto put_response = put.ReadResponse(10000);
    INFO("PUT " << put_response->status << " body=" << put_response->body);
    REQUIRE(put_response->status == 200);

    auto record = fss::test::AppFixture::MakeRecord(file_source, "metrics.bin");
    const auto created = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(),
                                fss::json::Dump(fss::domain::ToJson(record)));
    REQUIRE(created.status == 201);
    const auto created_json = fss::json::ParseObject(created.body);
    REQUIRE(created_json.ok());
    const std::string record_id = created_json.value()["id"].get<std::string>();

    const auto download =
        HttpDo(port, "GET", "/api/file/v2/files/" + file_id + "/downloadURL", Authed());
    REQUIRE(download.status == 200);
    const auto download_json = fss::json::ParseObject(download.body);
    REQUIRE(download_json.ok());
    fss::test::RawClient get(port, true);
    REQUIRE(get.Connect());
    REQUIRE(get.SendRequest("GET", fss::test::TargetOf(download_json.value()["SignedUrl"].get<std::string>()),
                            {"data-partition-id: opendes"}, ""));
    REQUIRE(get.ReadResponse(10000)->status == 200);

    REQUIRE(HttpDo(port, "DELETE", "/api/file/v2/files/" + record_id + "/metadata", Authed())
                .status == 204);
  }
  {
    fss::test::InMemoryLeaseRepository leases;
    fss::app::GcTask gc(*fx.ports, leases, "gc-1", &fx.registry);
    fss::app::GcOptions gc_options;
    gc_options.dry_run = true;
    REQUIRE(gc.Run("opendes", gc_options).ok());
    gc_options.dry_run = false;
    REQUIRE(gc.Run("opendes", gc_options).ok());
  }
  {
    //  自签校验失败也要有计数（走一次被篡改的 transfer URL）
    fss::test::RawClient client(port, true);
    REQUIRE(client.Connect());
    REQUIRE(client.SendRequest("GET", "/api/file/v1/transfer/tampered?exp=1&sig=x",
                               {"data-partition-id: opendes"}, ""));
    const auto response = client.ReadResponse(5000);
    REQUIRE(response.has_value());
    REQUIRE(response->status == 401);
  }

  //  ---- 抓取 /metrics ----
  const auto metrics = HttpDo(port, "GET", "/metrics");
  REQUIRE(metrics.status == 200);
  REQUIRE(metrics.Header("Content-Type").value().rfind("text/plain", 0) == 0);
  INFO(metrics.body);

  //  ① 全部指标都在（逐类断言，避免"只断言有一行"）
  const std::vector<std::string> required = {
      //  HTTP（既有）
      "fss_http_requests_total{", "fss_http_request_duration_seconds_bucket{",
      "fss_http_in_flight_requests ", "fss_http_rejected_total{",
      //  自签校验失败
      "fss_transfer_token_rejected_total ",
      //  存储操作与字节（P9 新增）
      "fss_storage_operations_total{", "fss_storage_bytes_total{",
      //  GC（P9 新增）
      "fss_gc_runs_total{", "fss_gc_last_run_epoch_seconds ",
  };
  for (const auto& needle : required) {
    CAPTURE(needle);
    REQUIRE(metrics.body.find(needle) != std::string::npos);
  }
  //  值必须真的动过（不是"指标存在但恒为 0"）
  REQUIRE(fx.registry.Value("fss_storage_bytes_total", {{"direction", "in"}}) > 0);
  REQUIRE(fx.registry.Value("fss_storage_bytes_total", {{"direction", "out"}}) > 0);
  REQUIRE(fx.registry.Value("fss_storage_operations_total",
                            {{"driver", "posix"}, {"op", "put"}, {"outcome", "ok"}}) > 0);
  REQUIRE(fx.registry.Value("fss_gc_runs_total", {{"mode", "dry_run"}, {"outcome", "ok"}}) == 1);
  REQUIRE(fx.registry.Value("fss_gc_runs_total", {{"mode", "real"}, {"outcome", "ok"}}) == 1);
  REQUIRE(fx.registry.Value("fss_gc_last_run_epoch_seconds") == fx.clock.NowEpochSeconds());

  //  ② 格式合法：每一行要么是 HELP/TYPE，要么是样本行
  std::size_t sample_lines = 0;
  std::size_t help_lines = 0;
  std::size_t type_lines = 0;
  std::istringstream stream(metrics.body);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty()) continue;
    if (line.rfind("# HELP ", 0) == 0) {
      ++help_lines;
      continue;
    }
    if (line.rfind("# TYPE ", 0) == 0) {
      ++type_lines;
      //  ★ 只有 Prometheus 的四种类型是合法的；`histogram` 是既有 HTTP 延迟指标用的
      REQUIRE((line.find(" counter") != std::string::npos ||
               line.find(" gauge") != std::string::npos ||
               line.find(" histogram") != std::string::npos));
      continue;
    }
    CAPTURE(line);
    REQUIRE(LooksLikeSampleLine(line));
    ++sample_lines;
  }
  REQUIRE(sample_lines > 20);
  REQUIRE(help_lines == type_lines);  // 每个 family 都有 HELP 与 TYPE

  //  ③ 不含 secret（把 fixture 用到的敏感串都扫一遍）
  const std::vector<std::string> sensitive_values = {
      secret, "test-token", "real-stack-secret", "dev-secret", "e2e-secret",
      //  自签 URL 的签名密钥：它会被拼进 `/v1/transfer/...` 的 URL，是最可能被
      //  日志/指标"顺手带出去"的东西（P1-D09 的同类：看起来打码了其实没有）
      "gc-metrics-secret"};
  for (const std::string& sensitive : sensitive_values) {
    CAPTURE(sensitive);
    REQUIRE(metrics.body.find(sensitive) == std::string::npos);
  }
}

TEST_CASE("★ C9.25 GC 清理 .tmp_*：够旧就删、太新保护、且**绝不视为有效对象**",
          "[phase9][hardening][c9.25]") {
  MetricsFixture fx;

  //  ---- ① 太新的临时文件：必须被保护（在途上传）----
  const std::string fresh = fx.MakeTempFile("inflight.bin.tmp.local.1.1", /*age_seconds=*/5);
  //  ---- ② 够旧的临时文件：必须被删（残留）----
  const std::string stale =
      fx.MakeTempFile("residue.bin.tmp.local.1.2",
                      /*age_seconds=*//*age_seconds=*/26 * 3600);
  //  ---- ③ "看起来被引用"的临时文件：仍然要删（那种键不可能由 ObjectKeyPolicy 生成）----
  const std::string referenced =
      fx.MakeTempFile("referenced.bin.tmp.local.1.3", /*age_seconds=*/26 * 3600);

  {
    //  给 ③ 造一条**位置记录**，其物理引用正好指向那个 `.tmp_*` 键
    fss::domain::FileLocation location;
    location.file_id = "opendes:dataset--File.Generic:deadbeefdeadbeefdeadbeefdeadbeef";
    location.file_source = "/osdu-user/1700000000000-2023-11-14-22-13-20-000/dead";
    location.zone = fss::domain::StorageZone::kStaging;
    location.user_id = "osdu-user";
    location.created_at_epoch_seconds = fx.clock.NowEpochSeconds();
    location.updated_at_epoch_seconds = location.created_at_epoch_seconds;
    location.extra = fss::json::Value::object();
    location.extra["container"] = "opendes-staging";
    location.extra["object_key"] = "referenced.bin.tmp.local.1.3";
    REQUIRE(fx.locations.Save("opendes", location).ok());
  }

  fss::test::InMemoryLeaseRepository leases;
  fss::app::GcTask gc(*fx.ports, leases, "gc-tmp", &fx.registry);
  fss::app::GcOptions options;
  options.dry_run = false;
  options.require_lease_expiry = false;  // 单实例降级路径：按 staging TTL 扫描
  options.staging_ttl_hours = 24;
  const auto report = gc.Run("opendes", options);
  REQUIRE(report.ok());
  INFO("tmp_removed=" << report.value().tmp_removed
                      << " tmp_skipped_too_young=" << report.value().tmp_skipped_too_young);
  REQUIRE(report.value().tmp_removed == 2);              // 够旧的两个
  REQUIRE(report.value().tmp_skipped_too_young == 1);    // 太新的一个
  //  ★ "看到但没删"必须**可观测**：POSIX 上临时文件对 `list()` 不可见，
  //    这个数字只能由驱动的 `remove_temp_files` 回报（P9-D06：叉乘扫描会让它翻倍）。
  REQUIRE(report.value().tmp_skipped_unknown_mtime == 0);

  REQUIRE(std::filesystem::exists(fresh));      // 在途：保护
  REQUIRE_FALSE(std::filesystem::exists(stale));    // 残留：清掉
  REQUIRE_FALSE(std::filesystem::exists(referenced));  // 反向：即使被"引用"也不放过

  //  指标同步（C9.6）：tmp 清理有独立计数，"保护"也要能排障
  REQUIRE(fx.registry.Value("fss_gc_tmp_removed_total") == 2);
  REQUIRE(fx.registry.Value("fss_gc_skipped_total", {{"reason", "tmp_too_young"}}) == 1);

  //  ---- ④ dry-run：只报候选、不动文件 ----
  const std::string another =
      fx.MakeTempFile("dryrun.bin.tmp.local.1.4", /*age_seconds=*/26 * 3600);
  fss::app::GcOptions dry;
  dry.dry_run = true;
  dry.require_lease_expiry = false;
  const auto dry_report = gc.Run("opendes", dry);
  REQUIRE(dry_report.ok());
  REQUIRE(dry_report.value().tmp_removed == 1);  // 候选被报出来
  REQUIRE(std::filesystem::exists(another));     // 但没有删
}
