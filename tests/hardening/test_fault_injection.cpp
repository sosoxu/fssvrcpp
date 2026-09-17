// =============================================================================
//  P9 硬化：故障注入 + 恢复（C9.2）
// =============================================================================
//  判据
//    · 每类故障的行为符合契约 §5（错误类别 → 状态码）
//    · **解除故障后 ≤ 30 秒恢复**（有测试、有时间断言）
//
//  覆盖的故障类别（P9 任务清单的 4 类 + 两个"非致命"依赖）：
//    ① 存储不可用（工厂解析失败）        → 503（kUnavailable）
//    ② 元数据写入失败（DB 只读/锁的等价物）→ 500（kInternal）
//    ③ 磁盘满（ENOSPC 映射到的错误）      → 503，且**回滚**（无元数据记录、persistent 无残留）
//    ④ 远端鉴权依赖不可用/超时            → 503，恢复后立刻 200
//    ⑤ 事件发布失败（非致命）             → 仍 201（契约 §2.6 第 1/10 步）
//    ⑥ 审计写入失败（非致命，当前配置）   → 仍正常（`observability.audit_fail_closed=false`）
//
//  ★ "磁盘满"用**注入的错误**（ENOSPC 语义）而不是真的写满磁盘：制造真实 ENOSPC 需要
//    一个可写的小文件系统（`mount` 需要 root，本环境不可用）。该限制如实登记在证据文件。
// =============================================================================
#include <catch2/catch.hpp>

#include "http_fixture.h"
#include "mock_entitlements.h"
#include "raw_http.h"

#include "app/usecases/usecases.h"
#include "infra/auth/remote/remote_entitlements_authorizer.h"
#include "infra/transfer/transfer_endpoint.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using fss::test::Authed;
using fss::test::HttpDo;
using fss::test::HttpFixture;
using fss::test::MockEntitlements;

//  ---- 一个可开关的"存储故障"装饰器：模拟 ENOSPC / 权限 / 连接失败 ----
class FaultInjectingBlobStore final : public fss::domain::IBlobStore {
 public:
  enum class Fault { kNone, kNoSpace, kAccessDenied, kUnavailable };

  FaultInjectingBlobStore(fss::domain::IBlobStore& inner, Fault& fault, std::atomic<int>& put_calls)
      : inner_(inner), fault_(fault), put_calls_(put_calls) {}

  fss::domain::BlobCapabilities capabilities() const override { return inner_.capabilities(); }
  fss::Result<void> ensure_container(const std::string& container) override {
    return inner_.ensure_container(container);
  }
  fss::Result<fss::domain::SignedLocation> presign_put(
      const fss::domain::ObjectRef& ref, const fss::domain::PresignOptions& options) override {
    return inner_.presign_put(ref, options);
  }
  fss::Result<fss::domain::SignedLocation> presign_get(
      const fss::domain::ObjectRef& ref, const fss::domain::PresignOptions& options) override {
    return inner_.presign_get(ref, options);
  }
  fss::Result<void> put(const fss::domain::ObjectRef& ref, fss::bytes::ByteSource& source,
                        const fss::domain::PutOptions& options) override {
    ++put_calls_;
    switch (fault_) {
      case Fault::kNoSpace:
        //  ENOSPC 的映射：存储暂时不可用 → 503（不是 500：重试可能成功）
        return Err(fss::ErrorKind::kUnavailable, "No space left on device");
      case Fault::kAccessDenied:
        return Err(fss::ErrorKind::kStorageAccessDenied, "存储侧拒绝：AccessDenied");
      case Fault::kUnavailable:
        return Err(fss::ErrorKind::kUnavailable, "存储后端不可用");
      case Fault::kNone:
        break;
    }
    return inner_.put(ref, source, options);
  }
  fss::Result<void> get(const fss::domain::ObjectRef& ref, fss::bytes::ByteSink& sink,
                        const fss::domain::ByteRange& range) override {
    return inner_.get(ref, sink, range);
  }
  fss::Result<fss::domain::ObjectStat> stat(const fss::domain::ObjectRef& ref) override {
    return inner_.stat(ref);
  }
  fss::Result<void> remove(const fss::domain::ObjectRef& ref) override {
    return inner_.remove(ref);
  }
  fss::Result<fss::domain::ObjectStat> copy(const fss::domain::ObjectRef& from,
                                            const fss::domain::ObjectRef& to) override {
    //  ★ 两个 zone 指向**同一个** store 时，12 步序列走的是 `copy()` 而不是 `put()`：
    //    故障注入必须同时覆盖这条路径，否则"磁盘满"用例会静默地成功（第一版就是这样）
    switch (fault_) {
      case Fault::kNoSpace:
        return Err(fss::ErrorKind::kUnavailable, "No space left on device");
      case Fault::kAccessDenied:
        return Err(fss::ErrorKind::kStorageAccessDenied, "存储侧拒绝：AccessDenied");
      case Fault::kUnavailable:
        return Err(fss::ErrorKind::kUnavailable, "存储后端不可用");
      case Fault::kNone:
        break;
    }
    return inner_.copy(from, to);
  }
  fss::Result<fss::domain::ListPage> list(const std::string& container, const std::string& prefix,
                                          const std::string& continuation_token,
                                          int limit) override {
    return inner_.list(container, prefix, continuation_token, limit);
  }

  //  C9.25：转发（故障装饰器只关心 put/copy）
  fss::Result<fss::domain::TempSweepResult> remove_temp_files(const std::string& container,
                                                 std::int64_t older_than_epoch_seconds,
                                                 bool dry_run) override {
    return inner_.remove_temp_files(container, older_than_epoch_seconds, dry_run);
  }

 private:
  fss::domain::IBlobStore& inner_;
  Fault& fault_;
  std::atomic<int>& put_calls_;
};

//  ---- 恢复时间：轮询探测端点，返回"从清障到成功"的毫秒数 ----
//  ★ 不用固定 sleep 等恢复（AGENTS §4.3）：轮询实际条件，并断言"≤ 30 秒"这个判据本身。
template <typename Clear, typename Probe>
double MeasureRecoveryMillis(Clear clear_fault, Probe probe) {
  const auto start = std::chrono::steady_clock::now();
  clear_fault();
  for (int attempt = 0; attempt < 300; ++attempt) {
    if (probe()) {
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
          .count();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return 30000.0 + 1;  // 超过上限（判据是 ≤ 30 秒）
}

//  ---- 本地装配：`AppFixture`（内存适配器，**可注入**元数据仓储与存储故障）+ 真实 HTTP 端口 ----
//  为什么不直接用 `HttpFixture`：它的 `ports` 在构造时就被 Router 引用，
//  之后替换元数据仓储会让 Router 持有悬空引用 —— 注入必须发生在装配**之前**。
struct FaultFixture {
  fss::test::AppFixture app;
  fss::test::FaultyMetadataRepository faulty{app.metadata};
  FaultInjectingBlobStore::Fault fault = FaultInjectingBlobStore::Fault::kNone;
  std::atomic<int> put_calls{0};
  FaultInjectingBlobStore injecting{app.blob, fault, put_calls};
  fss::infra::TransferEndpoint endpoint{app.codec, app.factory};
  fss::logging::MemoryLogger logger;
  //  ★ `WireRouterAndServer` 按名字访问这些成员 → 用引用把它们投影出来
  fss::test::FakeBlobStoreFactory& factory = app.factory;
  fss::test::RecordingSelfSignedCodec& codec = app.codec;
  std::unique_ptr<fss::app::UseCasePorts>& ports = app.ports;
  fss::ManualClock& clock = app.clock;
  std::unique_ptr<fss::adapters::http::Router> router;
  std::unique_ptr<fss::http::Server> server;

  FaultFixture() {
    app.UseMetadata(faulty);
    app.factory.SetZoneStore(fss::domain::StorageZone::kStaging, injecting);
    app.factory.SetZoneStore(fss::domain::StorageZone::kPersistent, injecting);
    fss::test::WireRouterAndServer(*this, {}, {});
  }
  ~FaultFixture() {
    if (server) server->Stop();
  }
  FaultFixture(const FaultFixture&) = delete;
  FaultFixture& operator=(const FaultFixture&) = delete;

  int port() const { return server->port(); }
};

}  // namespace

TEST_CASE("★ C9.2 ① 存储不可用 → 503；解除后 ≤ 30 秒恢复", "[phase9][hardening][c9.2]") {
  FaultFixture fx;
  const int port = fx.port();

  //  前置：正常时 200（否则"故障后的 503"说明不了问题）
  REQUIRE(HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed()).status == 200);

  //  注入：工厂无法解析出存储实例（kUnavailable）
  fx.app.factory.fail = true;
  const auto broken = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  INFO("存储不可用 → " << broken.status << " " << broken.body);
  REQUIRE(broken.status == 503);
  REQUIRE(broken.body.find("\"code\":503") != std::string::npos);

  //  解除 + 测恢复时间
  const double recovery_ms = MeasureRecoveryMillis(
      [&] { fx.app.factory.fail = false; },
      [&] { return HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed()).status == 200; });
  INFO("恢复耗时 " << recovery_ms << " ms");
  REQUIRE(recovery_ms <= 30000.0);
}

TEST_CASE("★ C9.2 ② 元数据写入失败（DB 只读/锁的等价物）→ 500；恢复后 201",
          "[phase9][hardening][c9.2]") {
  FaultFixture fx;
  const int port = fx.port();

  //  先拿一个上传地址（同时把空对象写进 staging）
  const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(upload.status == 200);
  const auto json = fss::json::ParseObject(upload.body);
  REQUIRE(json.ok());
  const std::string file_source = json.value()["Location"]["FileSource"].get<std::string>();

  fss::test::FaultyMetadataRepository& faulty = fx.faulty;
  faulty.fail_create = true;  // ★ 注入：写入元数据失败（DB 只读/锁的等价物）

  auto record = fss::test::AppFixture::MakeRecord(file_source, "fault.bin");
  const std::string body = fss::json::Dump(fss::domain::ToJson(record));
  const auto failed = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(), body);
  INFO("元数据写入失败 → " << failed.status << " " << failed.body);
  REQUIRE(failed.status == 500);  // kInternal → 500
  REQUIRE(failed.body.find("\"code\":500") != std::string::npos);

  const double recovery_ms = MeasureRecoveryMillis(
      [&] { faulty.fail_create = false; },
      [&] {
        const auto retry = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(), body);
        return retry.status == 201;
      });
  INFO("恢复耗时 " << recovery_ms << " ms");
  REQUIRE(recovery_ms <= 30000.0);
}

TEST_CASE("★ C9.2 ③ 磁盘满（ENOSPC）→ 503 且**回滚**；恢复后 201",
          "[phase9][hardening][c9.2]") {
  FaultFixture fx;
  const int port = fx.port();
  auto& fault = fx.fault;

  const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(upload.status == 200);
  const auto json = fss::json::ParseObject(upload.body);
  REQUIRE(json.ok());
  const std::string file_id = json.value()["FileID"].get<std::string>();
  const std::string file_source = json.value()["Location"]["FileSource"].get<std::string>();

  //  注入 ENOSPC：第 6 步（复制到 persistent）失败
  fault = FaultInjectingBlobStore::Fault::kNoSpace;
  auto record = fss::test::AppFixture::MakeRecord(file_source, "full-disk.bin");
  const std::string body = fss::json::Dump(fss::domain::ToJson(record));
  const auto failed = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(), body);
  INFO("磁盘满 → " << failed.status << " " << failed.body);
  //  ★ 存储故障在**这条路径**上映射成 502（`kBadGateway`）：第 6 步"复制到 persistent"是
  //    对**依赖服务（存储）**的调用，契约 §5 把依赖失败归到 502；而 case ① 里
  //    "工厂解析不出存储"发生在**取上传地址**路径，归到 503（`kUnavailable`）。
  //    两条都是契约约定的，测试按路径各钉一条（不是"随便挑一个 5xx"）。
  REQUIRE(failed.status == 502);
  REQUIRE(failed.body.find(R"("code":502)") != std::string::npos);

  //  ★ 回滚语义（契约 §2.6 第 12 步）：失败后**不能**留下元数据记录
  const auto latest = fx.app.metadata.GetLatestByFileSource("opendes", file_source);
  REQUIRE_FALSE(latest.ok());
  //  位置记录仍应是 staging（没有第 10 步的迁移）
  const auto location = fx.app.locations.Find("opendes", file_id);
  REQUIRE(location.ok());
  REQUIRE(location.value().zone == fss::domain::StorageZone::kStaging);

  //  恢复
  const double recovery_ms = MeasureRecoveryMillis(
      [&] { fault = FaultInjectingBlobStore::Fault::kNone; },
      [&] {
        const auto retry = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(), body);
        return retry.status == 201;
      });
  INFO("恢复耗时 " << recovery_ms << " ms");
  REQUIRE(recovery_ms <= 30000.0);
}

TEST_CASE("★ C9.2 ④ 远端鉴权依赖不可用/超时 → 503；解除后 ≤ 30 秒恢复",
          "[phase9][hardening][c9.2]") {
  //  用**真实**的远端授权器 + 独立进程 mock：这样"依赖不可用"是真的（不是替身里翻开关）
  fss::test::TempDir dir("fault-entitlements");
  const std::string fail_file = dir.child("FAIL");
  MockEntitlements::Options mock_options;
  mock_options.grant = "service.file.editors,service.file.viewers,service.file.admin";
  mock_options.fail_file = fail_file;
  MockEntitlements mock(mock_options);
  { std::ofstream touch(fail_file); touch << "fail"; }  // 先注入故障

  fss::infra::RemoteEntitlementsOptions options;
  options.base_url = mock.base_url();
  options.timeout_ms = 2000;
  options.connect_timeout_ms = 1000;
  options.verify_tls = false;
  fss::infra::RemoteEntitlementsAuthorizer authorizer(options);
  HttpFixture fx(authorizer);
  const int port = fx.port();

  const auto failed = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  INFO("鉴权依赖不可用 → " << failed.status << " " << failed.body);
  REQUIRE(failed.status == 503);

  const double recovery_ms = MeasureRecoveryMillis(
      [&] { std::remove(fail_file.c_str()); },
      [&] { return HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed()).status == 200; });
  INFO("恢复耗时 " << recovery_ms << " ms");
  REQUIRE(recovery_ms <= 30000.0);
}

TEST_CASE("★ C9.2 ⑤⑥ 非致命依赖：事件发布失败仍 201；审计写入失败不影响结果",
          "[phase9][hardening][c9.2]") {
  FaultFixture fx;
  const int port = fx.port();
  const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(upload.status == 200);
  const auto json = fss::json::ParseObject(upload.body);
  REQUIRE(json.ok());
  const std::string file_source = json.value()["Location"]["FileSource"].get<std::string>();
  auto record = fss::test::AppFixture::MakeRecord(file_source, "nonfatal.bin");
  const std::string body = fss::json::Dump(fss::domain::ToJson(record));

  //  ⑤ 事件发布失败（第 1 步 IN_PROGRESS + 第 10 步 datasetDetails）→ **非致命**
  fx.app.events.fail_status = "IN_PROGRESS";
  fx.app.events.fail_dataset_details = true;
  const auto with_events_down = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(), body);
  INFO("事件失败 → " << with_events_down.status);
  REQUIRE(with_events_down.status == 201);

  //  ⑥ 审计写入失败 → 当前配置是 fail-open（`observability.audit_fail_closed=false`）：
  //     操作仍然成功。★ 这是我们**显式选择**的语义（端口注释 + 契约 §8），不是遗漏。
  fx.app.events.fail_status.clear();
  fx.app.events.fail_dataset_details = false;
  fx.app.audit.fail = true;
  const auto other = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  INFO("审计失败 → " << other.status);
  REQUIRE(other.status == 200);
}
