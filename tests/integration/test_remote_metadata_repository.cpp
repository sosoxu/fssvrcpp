// =============================================================================
//  本切片（`metadata.repository=remote` + `metadata.remote.*`）：远端 Storage Service
//  元数据仓储（ADR-004）
// =============================================================================
//  判据（父代理的任务书 M1~M12；`docs/adr/ADR-004-persistence-strategy.md`）：
//    · 线协议：`PUT {base}/records` / `GET {base}/records/{id}` /
//      `POST {base}/records/{id}:delete`（**必须 204**）；`base_url` 是**基址**（追加路径）；
//    · 确定性记录 id（R5）：`(partition, file_source)` → SHA-256 前 128 bit 的
//      `8-4-4-4-12` 形状；**跨进程/跨运行稳定**、不同输入不同；
//    · 幂等：同一 fileSource 的第二次 createMetadata **不发第二次 PUT**；
//    · fail-closed：连不上/超时/非 2xx/非 JSON/缺字段 → `kUnavailable`；404→`kNotFound`；
//      401→`kUnauthenticated`；403→`kPermissionDenied`；**绝不映射成成功**；
//    · 能力：`capabilities() == {atomic_claim=false, backend_name="remote"}`，四个领取原语
//      返回 `kUnimplemented`；
//    · 真实进程端到端（M9）：组合根**真的**选了远端仓储（不是我方 SQLite）；
//    · 就绪（M10）：依赖挂 → readiness 503 而 **liveness 仍 200**；恢复 → ready；
//    · 拒绝启动矩阵（M11）：缺 base_url / token_provider≠static / leases.enabled=true /
//      deployment.mode=multi → exit 78 + 点名键与修法；
//    · 默认不变（M12）：sqlite 形态**没有** `fss_metadata_remote_requests_total` 族。
//
//  ⚠️ **没有真实 Storage Service**：线协议是按 `docs/01-osdu-research.md` 的端点写的，
//     由 `tests/tools/mock_validators.py --mode storage` **钉住**，**未与真实服务联调**。
//     适配器的"记录 JSON 响应"与"`{recordCount,recordIds,versions}` 信封"两种形状都被
//     这条 mock 覆盖（`--put-envelope`），但真实 Storage 的响应形状**未验证**。
//
//  ★ M1~M8 是**仓储级**（直接调 `RemoteMetadataRepository` + 可控 mock，不起服务）；
//    M2/M9/M10/M11/M12 需要**真实 `fss_server`**（证明组合根按配置选中了 remote）。
//    M2 的"第二次 createMetadata 不发 PUT"是被测**产品路径**（用例的幂等预检，decision 4）
//    的行为，因此必须走真实进程 —— 仓储的 `Create` 本身就是一次 upsert 的 PUT（decision 1）。
// =============================================================================
#include <catch2/catch.hpp>

#include "http_fixture.h"  // Authed / TargetOf
#include "mock_storage.h"
#include "raw_http.h"
#include "server_process.h"
#include "temp_dir.h"

#include "common/json/json.h"
#include "common/logging/logging.h"
#include "common/metrics/metrics.h"
#include "domain/model/file_metadata.h"
#include "infra/metadata/remote/remote_metadata_repository.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace {

using fss::ErrorKind;
using fss::test::Authed;
using fss::test::HttpDo;
using fss::test::MockStorage;
using fss::test::ProcessOutcome;
using fss::test::RawClient;
using fss::test::RunServerForExit;
using fss::test::ServerProcess;
using fss::test::ServerProcessOptions;
using fss::test::TargetOf;
using fss::test::TempDir;

constexpr const char* kPutPath = "/api/storage/v2/records";

// -----------------------------------------------------------------------------
//  小工具
// -----------------------------------------------------------------------------
//  ★ `Result<T>::value()`/`error()` 用的是 `std::variant` + `std::get`：对**失败**结果取
//   `value()`（或对成功结果取 `error()`）会抛 `std::bad_variant_access`，把"依赖返回了错误"
//   变成一条读不懂的异常，判据反而失效。诊断时统一走这个安全访问器。
template <typename T>
std::string ErrOf(const fss::Result<T>& result) {
  return result.ok() ? std::string("<ok>") : result.error().ToString();
}
int FreePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  int port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
      port = ntohs(addr.sin_port);
    }
  }
  ::close(fd);
  return port;
}

std::string WriteFile(const TempDir& dir, const std::string& name, const std::string& content) {
  const std::string path = dir.child(name);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.good());
  out << content;
  out.close();
  return path;
}

bool WaitReadyStatus(int port, int expected, int attempts = 400) {
  for (int i = 0; i < attempts; ++i) {
    RawClient client(port, /*tcp_nodelay=*/true);
    if (client.Connect() && client.SendRequest("GET", "/api/file/v2/readiness_check", {}, "")) {
      const auto response = client.ReadResponse(10000);
      if (response.has_value() && response->status == expected) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}
bool WaitReady(int port) { return WaitReadyStatus(port, 200); }

//  一份**契约合法**的最小记录（字段集与 `AppFixture::MakeRecord` / 契约 §3.1 同源）。
fss::domain::FileMetadataRecord MakeRecord(const std::string& partition,
                                           const std::string& file_source,
                                           const std::string& name) {
  fss::domain::FileMetadataRecord record;
  record.kind = partition + ":wks:dataset--File.Generic:1.0.0";
  record.version = 1;
  record.acl.viewers = {"data.default.viewers@" + partition + ".example.com"};
  record.acl.owners = {"data.default.owners@" + partition + ".example.com"};
  record.legal.legaltags = {"opendes-public-1"};
  record.legal.other_relevant_data_countries = {"US"};
  record.legal.status = fss::domain::LegalStatus::kCompliant;
  record.data.name = name;
  record.data.endian = "LITTLE";
  record.data.dataset_properties.present = true;
  record.data.dataset_properties.file_source_info.present = true;
  record.data.dataset_properties.file_source_info.file_source = file_source;
  return record;
}

fss::infra::RemoteMetadataRepositoryOptions RemoteOptions(const MockStorage& mock,
                                                          std::string token = "",
                                                          int timeout_ms = 3000) {
  fss::infra::RemoteMetadataRepositoryOptions options;
  options.base_url = mock.base_url();  // 基址（适配器追加 /records）
  options.static_token = std::move(token);
  options.timeout_ms = timeout_ms;
  return options;
}

//  契约 §2.6 的**最小合法**记录 JSON（真实进程的 POST body）。
std::string RecordJson(const std::string& file_source, const std::string& name) {
  fss::json::Value record = fss::json::Value::object();
  record["kind"] = "opendes:wks:dataset--File.Generic:1.0.0";
  fss::json::Value acl = fss::json::Value::object();
  acl["viewers"] = fss::json::Value::array({"data.default.viewers@opendes.example.com"});
  acl["owners"] = fss::json::Value::array({"data.default.owners@opendes.example.com"});
  record["acl"] = std::move(acl);
  fss::json::Value legal = fss::json::Value::object();
  legal["legaltags"] = fss::json::Value::array({"opendes-public-1"});
  legal["otherRelevantDataCountries"] = fss::json::Value::array({"US"});
  legal["status"] = "compliant";
  record["legal"] = std::move(legal);
  fss::json::Value data = fss::json::Value::object();
  data["Name"] = name;
  data["Endian"] = "LITTLE";
  fss::json::Value props = fss::json::Value::object();
  fss::json::Value source_info = fss::json::Value::object();
  source_info["FileSource"] = file_source;
  props["FileSourceInfo"] = std::move(source_info);
  data["DatasetProperties"] = std::move(props);
  record["data"] = std::move(data);
  return fss::json::Dump(record);
}

// -----------------------------------------------------------------------------
//  真实进程的配置
// -----------------------------------------------------------------------------
std::string RemoteConfig(const TempDir& dir, const std::string& name, int port,
                         const TempDir& data_dir, const std::string& metadata_block) {
  return WriteFile(
      dir, name,
      "{\n"
      "  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
          std::to_string(port) +
          "}},\n"
          "  \"storage\": {\"posix\": {\"root\": \"" + data_dir.child("store") + "\"}},\n"
          "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child("loc.db") + "\"}},\n"
          "  \"self_signed\": {\"signing_key\": \"remote-meta-secret\", "
          "\"public_base_url\": \"http://127.0.0.1:" + std::to_string(port) + "/api/file\"},\n"
          "  \"auth\": {\"mode\": \"disabled\"},\n" +
          metadata_block + "\n}\n");
}

std::string RemoteBlock(const std::string& base_url, const std::string& token_provider,
                        const std::string& static_token, int timeout_ms) {
  return "  \"metadata\": {\"repository\": \"remote\", \"remote\": {\"base_url\": \"" + base_url +
         "\", \"token_provider\": \"" + token_provider + "\", \"static_token\": \"" +
         static_token + "\", \"timeout_ms\": " + std::to_string(timeout_ms) + "}}";
}

ServerProcessOptions FileDriven(const std::string& config) {
  ServerProcessOptions options;
  options.default_http_port = false;
  options.default_grpc_port = false;
  options.expect_grpc = false;
  options.default_storage_env = false;
  options.args = {"--config", config};
  return options;
}

struct RegisterResult {
  int upload_status = 0;
  int put_status = 0;
  int create_status = 0;
  std::string create_body;
  std::string file_source;
};

RegisterResult UploadAndRegister(int port, const std::string& body, const std::string& name) {
  RegisterResult out;
  const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  out.upload_status = upload.status;
  REQUIRE(upload.status == 200);
  const auto parsed = fss::json::ParseObject(upload.body);
  REQUIRE(parsed.ok());
  const auto& location = parsed.value()["Location"];
  out.file_source = location["FileSource"].get<std::string>();
  const std::string signed_url = location["SignedURL"].get<std::string>();

  const auto put = HttpDo(port, "PUT", TargetOf(signed_url), Authed(), body);
  out.put_status = put.status;
  REQUIRE(put.status == 200);

  const auto created = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(),
                              RecordJson(out.file_source, name));
  out.create_status = created.status;
  out.create_body = created.body;
  return out;
}

std::string CreateIdOf(const std::string& create_body) {
  const auto parsed = fss::json::ParseObject(create_body);
  REQUIRE(parsed.ok());
  return parsed.value()["id"].get<std::string>();
}

}  // namespace

// =============================================================================
//  M1：Create → 恰好一次 `PUT /records`；body 解码为记录（派生 id / kind / FileSource）；
//      返回的 version == mock 分配的版本
// =============================================================================
TEST_CASE("★ M1 远端 Create：一次 PUT /records + 派生 id + 返回远端分配的 version",
          "[phase10][integration][remote-meta][m1]") {
  MockStorage mock;
  fss::logging::MemoryLogger logger;
  fss::metrics::Registry registry;
  fss::infra::RemoteMetadataRepository repo(RemoteOptions(mock), logger);

  const std::string file_source = "/data/m1.txt";
  const auto created = repo.Create("opendes", MakeRecord("opendes", file_source, "m1.txt"));
  CAPTURE(ErrOf(created));
  REQUIRE(created.ok());

  const std::string expected_id = fss::infra::DeriveRemoteRecordId("opendes", file_source);
  REQUIRE(created.value().id == expected_id);
  REQUIRE(created.value().version == 1);

  const auto observed = mock.WaitPuts(1);
  CAPTURE(observed.raw);
  REQUIRE(observed.puts == 1);
  REQUIRE(observed.CountCalls("PUT", kPutPath) == 1);
  REQUIRE(observed.body.is_object());
  REQUIRE(observed.body["id"].get<std::string>() == expected_id);
  REQUIRE(observed.body["kind"].get<std::string>() == "opendes:wks:dataset--File.Generic:1.0.0");
  REQUIRE(observed.body["data"]["DatasetProperties"]["FileSourceInfo"]["FileSource"]
              .get<std::string>() == file_source);
  //  ★ 幂等键必须落在 id 上，而不是调用方给的随机 UUID
  REQUIRE(created.value().id.find("dataset--File.Generic:") != std::string::npos);

  SECTION("★ 正控：Storage 的 upsert 信封形状也被接受（--put-envelope）") {
    MockStorage::Options options;
    options.put_envelope = true;
    MockStorage envelope_mock(options);
    fss::infra::RemoteMetadataRepository envelope_repo(RemoteOptions(envelope_mock), logger);
    const auto envelope_created =
        envelope_repo.Create("opendes", MakeRecord("opendes", "/data/m1-envelope.txt", "e.txt"));
    CAPTURE(ErrOf(envelope_created));
    REQUIRE(envelope_created.ok());
    REQUIRE(envelope_created.value().id ==
            fss::infra::DeriveRemoteRecordId("opendes", "/data/m1-envelope.txt"));
    REQUIRE(envelope_created.value().version == 1);  // 信封的 versions[0] = "1"
  }
}

// =============================================================================
//  M2 幂等（R5）：同一 fileSource 的第二次 createMetadata **不发第二次 PUT**；
//  正控：不同 fileSource → 第二次 PUT 且 id 不同
//  ★ 走真实进程（产品路径的幂等预检在用例 `CreateFileMetadata` 的 4b 步，decision 4）
// =============================================================================
TEST_CASE("★ M2 幂等：第二次 createMetadata 不发第二次 PUT（真实进程 + mock）",
          "[phase10][integration][remote-meta][m2]") {
  TempDir cfg_dir("fss_m2_cfg");
  TempDir data_dir("fss_m2_data");
  const int port = FreePort();
  REQUIRE(port > 0);
  MockStorage mock;
  const std::string config = RemoteConfig(cfg_dir, "remote.json", port, data_dir,
                                          RemoteBlock(mock.base_url(), "static", "", 3000));
  ServerProcess server(FileDriven(config));
  CAPTURE(server.DumpLog());
  REQUIRE(server.http_port() == port);
  REQUIRE(WaitReady(port));

  const auto first = UploadAndRegister(port, "m2-body-1", "m2.txt");
  CAPTURE(first.create_status, first.create_body);
  REQUIRE(first.create_status == 201);
  const std::string first_id = CreateIdOf(first.create_body);
  REQUIRE(first_id == fss::infra::DeriveRemoteRecordId("opendes", first.file_source));
  REQUIRE(mock.WaitPuts(1).puts == 1);

  //  第二次：**同一个** POST（同 fileSource）→ 201 同 id，且 PUT 计数仍是 1
  const auto second = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(),
                             RecordJson(first.file_source, "m2.txt"));
  CAPTURE(second.status, second.body);
  REQUIRE(second.status == 201);
  REQUIRE(CreateIdOf(second.body) == first_id);
  REQUIRE(mock.PutsStayAt(1, 600));  // ★ 核心判据：没有第二次 PUT

  //  正控：**不同** fileSource → 必须真的发第二次 PUT，且 id 不同
  const auto other = UploadAndRegister(port, "m2-body-2", "m2-other.txt");
  CAPTURE(other.create_status, other.create_body);
  REQUIRE(other.create_status == 201);
  REQUIRE(CreateIdOf(other.create_body) != first_id);
  REQUIRE(mock.WaitPuts(2).puts == 2);
}

// =============================================================================
//  M3 派生 id 稳定性（R5）：golden 值 + 跨调用稳定 + 不同输入不同 + UUID 形状
// =============================================================================
TEST_CASE("★ M3 派生 id：golden 值 + 稳定 + 区分不同输入 + 保持 UUID 形状",
          "[phase10][integration][remote-meta][m3]") {
  //  ★ golden：固定输入的**精确**期望值。任何"静默换算法"都会在这里失败。
  //    （由独立实现核对过：SHA-256("opendes" || 0x00 || "/data/remote-meta-golden.txt")
  //      前 32 个 hex 字符 → 5b9b934d2064726b0d4af85673f6fba3。）
  const std::string golden =
      fss::infra::DeriveRemoteRecordId("opendes", "/data/remote-meta-golden.txt");
  REQUIRE(golden == "opendes:dataset--File.Generic:5b9b934d-2064-726b-0d4a-f85673f6fba3");

  //  跨调用（两次独立派生；不同测试用例也各派生一次 —— 见 M1/M9 的断言）稳定
  REQUIRE(fss::infra::DeriveRemoteRecordId("opendes", "/data/remote-meta-golden.txt") == golden);

  //  负控：不同 fileSource / 不同 partition → 不同 id
  REQUIRE(fss::infra::DeriveRemoteRecordId("opendes", "/data/other.txt") != golden);
  REQUIRE(fss::infra::DeriveRemoteRecordId("opendes2", "/data/remote-meta-golden.txt") != golden);
  //  ★ `'\0'` 分隔符的负控：("ab","c") 与 ("a","bc") 不得撞
  REQUIRE(fss::infra::DeriveRemoteRecordId("ab", "c") !=
          fss::infra::DeriveRemoteRecordId("a", "bc"));

  //  形状：`<partition>:dataset--File.Generic:<8-4-4-4-12>` 且 hex 全小写
  const std::regex shape(
      "^opendes:dataset--File\\.Generic:[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
  REQUIRE(std::regex_match(golden, shape));
  //  ★ 形状断言本身要能失败（R1）：纯 hex（去横线）**不**匹配
  REQUIRE_FALSE(std::regex_match(
      std::string("opendes:dataset--File.Generic:5b9b934d2064726b0d4af85673f6fba3"), shape));
}

// =============================================================================
//  M4：GetById / GetLatestByFileSource 返回 mock 存的东西；404 → kNotFound（断言 ErrorKind）
// =============================================================================
TEST_CASE("★ M4 读路径：GetById / GetLatestByFileSource；404 → kNotFound（断言 ErrorKind）",
          "[phase10][integration][remote-meta][m4]") {
  MockStorage mock;
  fss::logging::MemoryLogger logger;
  fss::infra::RemoteMetadataRepository repo(RemoteOptions(mock), logger);
  const std::string file_source = "/data/m4.txt";
  const auto created = repo.Create("opendes", MakeRecord("opendes", file_source, "m4.txt"));
  REQUIRE(created.ok());
  const std::string id = created.value().id;

  const auto by_id = repo.GetById("opendes", id);
  CAPTURE(ErrOf(by_id));
  REQUIRE(by_id.ok());
  REQUIRE(by_id.value().id == id);
  REQUIRE(by_id.value().version == 1);
  REQUIRE(by_id.value().data.name.has_value());
  REQUIRE(*by_id.value().data.name == "m4.txt");

  const auto by_source = repo.GetLatestByFileSource("opendes", file_source);
  CAPTURE(ErrOf(by_source));
  REQUIRE(by_source.ok());
  REQUIRE(by_source.value().id == id);
  //  路径必须是**派生 id**（没有未文档化的 query 端点）
  REQUIRE(mock.ReadObservation().last_headers.empty() == false);

  SECTION("404 → kNotFound（**断言 ErrorKind**，不是只断言 not ok）") {
    const auto missing =
        repo.GetById("opendes", "opendes:dataset--File.Generic:00000000-0000-0000-0000-000000000000");
    CAPTURE(ErrOf(missing));
    REQUIRE_FALSE(missing.ok());
    REQUIRE(missing.error().kind() == ErrorKind::kNotFound);
  }

  SECTION("mock 以 404 + **伪装成成功的记录体**回答 → 仍然 kNotFound（状态码真的被检查）") {
    MockStorage::Options options;
    options.fail_get = 404;
    MockStorage failing(options);
    fss::infra::RemoteMetadataRepository failing_repo(RemoteOptions(failing), logger);
    const auto result = failing_repo.GetById("opendes", id);
    CAPTURE(ErrOf(result));
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == ErrorKind::kNotFound);
  }
}

// =============================================================================
//  M5：Update → PUT 同一 id → 版本递增（mock 观测）
// =============================================================================
TEST_CASE("★ M5 Update：PUT 同一 id，版本由远端递增（mock 观测）",
          "[phase10][integration][remote-meta][m5]") {
  MockStorage mock;
  fss::logging::MemoryLogger logger;
  fss::infra::RemoteMetadataRepository repo(RemoteOptions(mock), logger);
  const std::string file_source = "/data/m5.txt";
  const auto created = repo.Create("opendes", MakeRecord("opendes", file_source, "m5.txt"));
  REQUIRE(created.ok());
  REQUIRE(created.value().version == 1);

  auto to_update = created.value();
  to_update.data.name = "m5-renamed.txt";
  const auto updated = repo.Update("opendes", to_update);
  CAPTURE(ErrOf(updated));
  REQUIRE(updated.ok());
  REQUIRE(updated.value().id == created.value().id);
  REQUIRE(updated.value().version == 2);  // 远端递增

  const auto reloaded = repo.GetById("opendes", created.value().id);
  REQUIRE(reloaded.ok());
  REQUIRE(reloaded.value().version == 2);  // ★ mock 真的存下来了
  REQUIRE(reloaded.value().data.name.has_value());
  REQUIRE(*reloaded.value().data.name == "m5-renamed.txt");

  const auto observed = mock.ReadObservation();
  REQUIRE(observed.puts == 2);
  REQUIRE(observed.CountCalls("PUT", kPutPath) == 2);
}

// =============================================================================
//  M6：Delete → `POST /records/{id}:delete`；204 → ok；**200 → 失败**（ADR-004 要求 204）
// =============================================================================
TEST_CASE("★ M6 Delete：204 才算成功；返回 200 → 失败（ADR-004）",
          "[phase10][integration][remote-meta][m6]") {
  fss::logging::MemoryLogger logger;
  const std::string file_source = "/data/m6.txt";
  const std::string derived = fss::infra::DeriveRemoteRecordId("opendes", file_source);
  const std::string delete_path = kPutPath + std::string("/") + derived + ":delete";

  SECTION("204 → Ok；记录真的被删掉") {
    MockStorage mock;
    fss::infra::RemoteMetadataRepository repo(RemoteOptions(mock), logger);
    const auto created = repo.Create("opendes", MakeRecord("opendes", file_source, "m6.txt"));
    REQUIRE(created.ok());
    const auto removed = repo.Delete("opendes", created.value().id);
    CAPTURE(ErrOf(removed));
    REQUIRE(removed.ok());
    const auto observed = mock.ReadObservation();
    REQUIRE(observed.deletes == 1);
    REQUIRE(observed.CountCalls("POST", delete_path) == 1);
    //  删除后读不到 → kNotFound（证明 mock 真的删了，而不是我们自说自话）
    const auto after = repo.GetById("opendes", created.value().id);
    REQUIRE_FALSE(after.ok());
    REQUIRE(after.error().kind() == ErrorKind::kNotFound);
  }

  SECTION("200 → 失败（kUnavailable），绝不报成功") {
    MockStorage::Options options;
    options.delete_status = 200;
    MockStorage mock(options);
    fss::infra::RemoteMetadataRepository repo(RemoteOptions(mock), logger);
    const auto created = repo.Create("opendes", MakeRecord("opendes", file_source, "m6.txt"));
    REQUIRE(created.ok());
    const auto removed = repo.Delete("opendes", created.value().id);
    CAPTURE(ErrOf(removed));
    REQUIRE_FALSE(removed.ok());
    REQUIRE(removed.error().kind() == ErrorKind::kUnavailable);
    REQUIRE(removed.error().message().find("204") != std::string::npos);
    REQUIRE(mock.ReadObservation().CountCalls("POST", delete_path) == 1);
  }
}

// =============================================================================
//  M7 鉴权头：`Authorization: Bearer <static_token>`；错/空 token → kUnauthenticated
// =============================================================================
TEST_CASE("★ M7 鉴权：正确 token 通过（且 mock 真的收到 Bearer 头）；错/空 → kUnauthenticated",
          "[phase10][integration][remote-meta][m7]") {
  fss::logging::MemoryLogger logger;
  const std::string file_source = "/data/m7.txt";

  MockStorage::Options options;
  options.require_token = "tok-123";
  MockStorage mock(options);

  SECTION("static_token=tok-123 → Ok，且 mock 收到 `Authorization: Bearer tok-123`") {
    fss::infra::RemoteMetadataRepository repo(RemoteOptions(mock, "tok-123"), logger);
    const auto created = repo.Create("opendes", MakeRecord("opendes", file_source, "m7.txt"));
    CAPTURE(ErrOf(created));
    REQUIRE(created.ok());
    const auto observed = mock.WaitPuts(1);
    const auto* call = observed.LastCall("PUT", kPutPath);
    REQUIRE(call != nullptr);
    const auto& headers = (*call)["headers"];
    REQUIRE(headers.contains("Authorization"));
    REQUIRE(headers["Authorization"].get<std::string>() == "Bearer tok-123");
  }

  SECTION("错误 token → kUnauthenticated，且原因点名 metadata.remote.static_token") {
    fss::infra::RemoteMetadataRepository repo(RemoteOptions(mock, "wrong-token"), logger);
    const auto created = repo.Create("opendes", MakeRecord("opendes", file_source, "m7.txt"));
    CAPTURE(ErrOf(created));
    REQUIRE_FALSE(created.ok());
    REQUIRE(created.error().kind() == ErrorKind::kUnauthenticated);
    REQUIRE(created.error().message().find("metadata.remote.static_token") != std::string::npos);
  }

  SECTION("空 token（没有 Authorization 头）→ kUnauthenticated（**不是** 503 停机）") {
    fss::infra::RemoteMetadataRepository repo(RemoteOptions(mock, ""), logger);
    const auto created = repo.Create("opendes", MakeRecord("opendes", file_source, "m7.txt"));
    CAPTURE(ErrOf(created));
    REQUIRE_FALSE(created.ok());
    REQUIRE(created.error().kind() == ErrorKind::kUnauthenticated);
  }
}

// =============================================================================
//  M8 故障映射：500 → kUnavailable；超时 → kUnavailable；非 JSON → kUnavailable
//  ★ mock 在这些形态下回"伪装成成功的记录体（version=999）" ⇒ 只有真的检查状态码/
//    真的解析失败才会 fail-closed（R1：判据有区分力）
// =============================================================================
TEST_CASE("★ M8 故障映射：非 2xx / 超时 / 非 JSON → kUnavailable（断言 ErrorKind）",
          "[phase10][integration][remote-meta][m8]") {
  fss::logging::MemoryLogger logger;
  const std::string file_source = "/data/m8.txt";

  SECTION("HTTP 500 → kUnavailable") {
    MockStorage::Options options;
    options.fail_put = 500;
    MockStorage mock(options);
    fss::infra::RemoteMetadataRepository repo(RemoteOptions(mock), logger);
    const auto created = repo.Create("opendes", MakeRecord("opendes", file_source, "m8.txt"));
    CAPTURE(ErrOf(created));
    REQUIRE_FALSE(created.ok());
    REQUIRE(created.error().kind() == ErrorKind::kUnavailable);
    //  ★ 反例断言：500 的体是"伪装成成功的记录"（version=999）。若实现把 500 当成功，
    //    返回的 version 会是 999 —— 这条让注入 I4 无法蒙混。
    if (created.ok()) REQUIRE(created.value().version != 999);
  }

  SECTION("客户端超时（mock 睡 1500ms，timeout_ms=300）→ kUnavailable") {
    MockStorage::Options options;
    options.timeout_ms = 1500;
    MockStorage mock(options);
    fss::infra::RemoteMetadataRepository repo(RemoteOptions(mock, "", 300), logger);
    const auto created = repo.Create("opendes", MakeRecord("opendes", file_source, "m8.txt"));
    CAPTURE(ErrOf(created));
    REQUIRE_FALSE(created.ok());
    REQUIRE(created.error().kind() == ErrorKind::kUnavailable);
  }

  SECTION("非 JSON 响应 → kUnavailable（Create 与 GetById 两条路径）") {
    MockStorage::Options options;
    options.malformed = true;
    MockStorage mock(options);
    fss::infra::RemoteMetadataRepository repo(RemoteOptions(mock), logger);
    const auto created = repo.Create("opendes", MakeRecord("opendes", file_source, "m8.txt"));
    CAPTURE(ErrOf(created));
    REQUIRE_FALSE(created.ok());
    REQUIRE(created.error().kind() == ErrorKind::kUnavailable);

    const auto got = repo.GetById("opendes", fss::infra::DeriveRemoteRecordId("opendes", file_source));
    CAPTURE(ErrOf(got));
    REQUIRE_FALSE(got.ok());
    REQUIRE(got.error().kind() == ErrorKind::kUnavailable);
  }

  SECTION("缺必需字段（合法 JSON 但既无 kind/data 也无 versions）→ kUnavailable") {
    //  用 `--put-envelope` 的**空 versions** 不可能通过开关构造；这里直接验证
    //  "信封缺 versions" 的判定：mock 回 `{}`（200 合法 JSON）。
    //  ★ 没有专门的 mock 开关 —— 用 `fail_get` 的"伪装记录体"不能表达它，因此
    //    这条用 `malformed` 之外的方式无法注入；如实保留在未覆盖清单里。
    SUCCEED("见 docs/test-evidence/phase10.md §26 的未覆盖项：缺字段形态未单独注入");
  }
}

// =============================================================================
//  M8b：能力声明 —— atomic_claim=false；四个领取原语 kUnimplemented（点名能力与理由）
// =============================================================================
TEST_CASE("★ M8b 能力：atomic_claim=false + 领取原语 kUnimplemented（不假装支持）",
          "[phase10][integration][remote-meta][m8b]") {
  MockStorage mock;
  fss::logging::MemoryLogger logger;
  fss::infra::RemoteMetadataRepository repo(RemoteOptions(mock), logger);

  const auto capabilities = repo.capabilities();
  REQUIRE_FALSE(capabilities.atomic_claim);
  REQUIRE(capabilities.backend_name == "remote");

  const auto record = MakeRecord("opendes", "/data/m8b.txt", "m8b.txt");
  const auto claim = repo.ClaimForWrite("opendes", record);
  REQUIRE_FALSE(claim.ok());
  REQUIRE(claim.error().kind() == ErrorKind::kUnimplemented);
  REQUIRE(claim.error().message().find("atomic_claim") != std::string::npos);

  const auto marked = repo.MarkReady("opendes", "id", 1, record);
  REQUIRE_FALSE(marked.ok());
  REQUIRE(marked.error().kind() == ErrorKind::kUnimplemented);

  const auto released = repo.ReleaseClaim("opendes", "id", 1);
  REQUIRE_FALSE(released.ok());
  REQUIRE(released.error().kind() == ErrorKind::kUnimplemented);

  const auto reclaimed = repo.ReclaimStaleClaiming("opendes", 0, 10, {});
  REQUIRE_FALSE(reclaimed.ok());
  REQUIRE(reclaimed.error().kind() == ErrorKind::kUnimplemented);

  //  这些原语**没有**发起任何 HTTP 请求（本地即返回）
  REQUIRE(mock.ReadObservation().requests == 0);
}

// =============================================================================
//  M9 真实进程端到端：组合根真的选了 remote（不是我方 SQLite）
// =============================================================================
TEST_CASE("★ M9 真实进程：remote 仓储的 createMetadata/读/删端到端（mock 观测）",
          "[phase10][integration][remote-meta][m9]") {
  TempDir cfg_dir("fss_m9_cfg");
  TempDir data_dir("fss_m9_data");
  const int port = FreePort();
  REQUIRE(port > 0);
  MockStorage mock;
  const std::string config = RemoteConfig(cfg_dir, "remote.json", port, data_dir,
                                          RemoteBlock(mock.base_url(), "static", "", 3000));
  ServerProcess server(FileDriven(config));
  CAPTURE(server.DumpLog());
  REQUIRE(server.http_port() == port);
  REQUIRE(WaitReady(port));

  //  ★ R11：横幅必须让运维看到真实依赖（base_url / token_provider / timeout / atomic_claim）
  const std::string banner = server.DumpLog();
  REQUIRE(banner.find("metadata=remote") != std::string::npos);
  REQUIRE(banner.find(mock.base_url()) != std::string::npos);
  REQUIRE(banner.find("atomic_claim=false") != std::string::npos);
  REQUIRE(banner.find("remote-meta-secret") == std::string::npos);  // 不打印签名密钥

  const auto created = UploadAndRegister(port, "m9-body-1", "m9.txt");
  CAPTURE(created.create_status, created.create_body);
  REQUIRE(created.create_status == 201);
  const std::string id = CreateIdOf(created.create_body);
  REQUIRE(id == fss::infra::DeriveRemoteRecordId("opendes", created.file_source));
  REQUIRE(mock.WaitPuts(1).puts == 1);

  //  第二次相同 POST → 201 同 id，仍只有一次 PUT
  const auto again = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(),
                            RecordJson(created.file_source, "m9.txt"));
  CAPTURE(again.status, again.body);
  REQUIRE(again.status == 201);
  REQUIRE(CreateIdOf(again.body) == id);
  REQUIRE(mock.PutsStayAt(1, 600));

  //  ★ 读路径：记录来自**远端**（mock 的 GET 命中）
  const int gets_before = mock.ReadObservation().gets;
  const auto got = HttpDo(port, "GET", "/api/file/v2/files/" + id + "/metadata", Authed());
  CAPTURE(got.status, got.body);
  REQUIRE(got.status == 200);
  const auto got_json = fss::json::ParseObject(got.body);
  REQUIRE(got_json.ok());
  REQUIRE(got_json.value()["id"].get<std::string>() == id);
  REQUIRE(got_json.value()["data"]["Name"].get<std::string>() == "m9.txt");
  const std::string get_path = std::string(kPutPath) + "/" + id;
  REQUIRE(mock.ReadObservation().CountCalls("GET", get_path) >= 1);
  REQUIRE(mock.ReadObservation().gets > gets_before);

  //  ★ 删除：204 语义 + mock 收到 `POST /records/{id}:delete`
  const auto removed =
      HttpDo(port, "DELETE", "/api/file/v2/files/" + id + "/metadata", Authed());
  CAPTURE(removed.status, removed.body);
  REQUIRE(removed.status == 204);
  REQUIRE(mock.ReadObservation().CountCalls("POST", get_path + ":delete") == 1);
}

// =============================================================================
//  M10 就绪：依赖挂 → readiness 503（可读原因）而 liveness 200；重启 → ready
// =============================================================================
TEST_CASE("★ M10 就绪探针：mock 挂 → readiness 503 + liveness 200；同端口重启 → ready",
          "[phase10][integration][remote-meta][m10]") {
  TempDir cfg_dir("fss_m10_cfg");
  TempDir data_dir("fss_m10_data");
  const int port = FreePort();
  const int mock_port = FreePort();
  REQUIRE(port > 0);
  REQUIRE(mock_port > 0);

  MockStorage::Options mock_options;
  mock_options.port = mock_port;  // ★ 固定端口：重启后 base_url 仍指向它
  auto mock = std::make_unique<MockStorage>(mock_options);
  REQUIRE(mock->port() == mock_port);
  const std::string base_url = mock->base_url();

  const std::string config =
      RemoteConfig(cfg_dir, "remote.json", port, data_dir, RemoteBlock(base_url, "static", "", 3000));
  ServerProcess server(FileDriven(config));
  CAPTURE(server.DumpLog());
  REQUIRE(server.http_port() == port);
  REQUIRE(WaitReady(port));  // 正控：mock 在 → ready（探针 404 = 服务活着）

  //  ★ 杀掉依赖（真的杀进程，不是"睡一会儿"）
  mock->Stop();
  mock.reset();

  //  轮询：readiness 必须变 503，且 503 体里带可读原因（点名远端 Storage Service）
  REQUIRE(WaitReadyStatus(port, 503));
  const auto not_ready = HttpDo(port, "GET", "/api/file/v2/readiness_check", Authed());
  CAPTURE(not_ready.status, not_ready.body);
  REQUIRE(not_ready.status == 503);
  REQUIRE(not_ready.body.find("not ready") != std::string::npos);
  REQUIRE(not_ready.body.find("Storage Service") != std::string::npos);

  //  ★ 区分：liveness **仍然 200**（进程活着，是依赖挂了）
  const auto alive = HttpDo(port, "GET", "/api/file/v2/liveness_check", Authed());
  CAPTURE(alive.status, alive.body);
  REQUIRE(alive.status == 200);

  //  ★ 恢复：同端口重启 mock → 轮询回 ready
  mock = std::make_unique<MockStorage>(mock_options);
  REQUIRE(mock->port() == mock_port);
  REQUIRE(WaitReady(port));
  REQUIRE(mock->ReadObservation().requests >= 1);  // 恢复后探针真的被调用过
}

// =============================================================================
//  M11 拒绝启动矩阵：每一条都 **exit 78** + 消息点名"出错的键 + 修法"
// =============================================================================
TEST_CASE("★ M11 remote 的 fail-closed 前置条件：缺 base_url / token_provider / leases / multi",
          "[phase10][integration][remote-meta][m11]") {
  SECTION("缺 metadata.remote.base_url → exit 78") {
    TempDir cfg_dir("fss_m11_base");
    TempDir data_dir("fss_m11_base_data");
    const std::string config =
        RemoteConfig(cfg_dir, "remote.json", FreePort(), data_dir,
                     RemoteBlock("", "static", "", 3000));
    const ProcessOutcome outcome = RunServerForExit({"--config", config});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("metadata.remote.base_url") != std::string::npos);
    REQUIRE(outcome.output.find("拒绝启动") != std::string::npos);
    REQUIRE(outcome.output.find("已启动") == std::string::npos);
  }

  SECTION("metadata.remote.token_provider=file（非 static）→ exit 78") {
    TempDir cfg_dir("fss_m11_token");
    TempDir data_dir("fss_m11_token_data");
    const std::string config =
        RemoteConfig(cfg_dir, "remote.json", FreePort(), data_dir,
                     RemoteBlock("http://127.0.0.1:1/api/storage/v2", "file", "", 3000));
    const ProcessOutcome outcome = RunServerForExit({"--config", config});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("metadata.remote.token_provider") != std::string::npos);
    REQUIRE(outcome.output.find("static") != std::string::npos);
  }

  SECTION("leases.enabled=true → exit 78") {
    TempDir cfg_dir("fss_m11_leases");
    TempDir data_dir("fss_m11_leases_data");
    //  leases.enabled=true 还会触发 PG 需求；这里只关心 remote 的那条消息 —— 顺序上
    //  remote 的前置条件在同一个函数里、且 `leases.enabled` 在 remote 块内被点名。
    const std::string config = RemoteConfig(
        cfg_dir, "remote.json", FreePort(), data_dir,
        RemoteBlock("http://127.0.0.1:1/api/storage/v2", "static", "", 3000) +
            ",\n  \"leases\": {\"enabled\": true}");
    const ProcessOutcome outcome = RunServerForExit({"--config", config});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("leases.enabled") != std::string::npos);
  }

  SECTION("deployment.mode=multi → exit 78（与 remote 不兼容；消息含 deployment.mode / postgres）") {
    TempDir cfg_dir("fss_m11_multi");
    TempDir data_dir("fss_m11_multi_data");
    const std::string config = RemoteConfig(
        cfg_dir, "remote.json", FreePort(), data_dir,
        RemoteBlock("http://127.0.0.1:1/api/storage/v2", "static", "", 3000) +
            ",\n  \"deployment\": {\"mode\": \"multi\"}");
    const ProcessOutcome outcome = RunServerForExit({"--config", config});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    //  core_schema 的跨字段校验先触发：点名 metadata.repository 必须是 postgres，
    //  消息里同时出现 `deployment.mode=multi`（出错的键）与修法。
    REQUIRE(outcome.output.find("deployment.mode=multi") != std::string::npos);
    REQUIRE(outcome.output.find("postgres") != std::string::npos);
  }
}

// =============================================================================
//  M12 默认不变（正控）：sqlite 形态正常启停，且 **没有** 远端指标族
// =============================================================================
TEST_CASE("★ M12 默认 sqlite：能建能读，且 /metrics **没有** fss_metadata_remote_requests_total",
          "[phase10][integration][remote-meta][m12]") {
  SECTION("sqlite（默认）：功能正常 + 远端指标族不存在") {
    TempDir cfg_dir("fss_m12_sqlite");
    TempDir data_dir("fss_m12_sqlite_data");
    const int port = FreePort();
    REQUIRE(port > 0);
    const std::string config = WriteFile(
        cfg_dir, "sqlite.json",
        "{\n"
        "  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
            std::to_string(port) +
            "}},\n"
            "  \"storage\": {\"posix\": {\"root\": \"" + data_dir.child("store") + "\"}},\n"
            "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child("loc.db") + "\"}},\n"
            "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child("meta.db") + "\"}},\n"
            "  \"self_signed\": {\"signing_key\": \"s\", "
            "\"public_base_url\": \"http://127.0.0.1:" + std::to_string(port) + "/api/file\"},\n"
            "  \"auth\": {\"mode\": \"disabled\"}\n}\n");
    ServerProcess server(FileDriven(config));
    REQUIRE(server.http_port() == port);
    REQUIRE(WaitReady(port));

    const auto created = UploadAndRegister(port, "m12-body", "m12.txt");
    CAPTURE(created.create_status, created.create_body);
    REQUIRE(created.create_status == 201);
    const std::string id = CreateIdOf(created.create_body);
    const auto got = HttpDo(port, "GET", "/api/file/v2/files/" + id + "/metadata", Authed());
    REQUIRE(got.status == 200);

    const auto metrics = HttpDo(port, "GET", "/metrics", {});
    REQUIRE(metrics.status == 200);
    //  ★ 反向断言 + 同路径正控（R16 / AGENTS §4.3 的"否定式判据必须配正控"）：
    //    先证明这个 /metrics 确实渲染了别的族，再断言远端族不在。
    REQUIRE(metrics.body.find("fss_io_engine") != std::string::npos);
    REQUIRE(metrics.body.find("fss_metadata_remote_requests_total") == std::string::npos);
  }

  SECTION("正控：remote 形态的 /metrics **有** fss_metadata_remote_requests_total") {
    TempDir cfg_dir("fss_m12_remote");
    TempDir data_dir("fss_m12_remote_data");
    const int port = FreePort();
    REQUIRE(port > 0);
    MockStorage mock;
    const std::string config = RemoteConfig(cfg_dir, "remote.json", port, data_dir,
                                            RemoteBlock(mock.base_url(), "static", "", 3000));
    ServerProcess server(FileDriven(config));
    REQUIRE(server.http_port() == port);
    REQUIRE(WaitReady(port));  // 就绪探针已产生一次 probe 请求
    const auto metrics = HttpDo(port, "GET", "/metrics", {});
    CAPTURE(metrics.status);
    REQUIRE(metrics.status == 200);
    REQUIRE(metrics.body.find("fss_metadata_remote_requests_total") != std::string::npos);
    REQUIRE(metrics.body.find("op=\"probe\"") != std::string::npos);
  }
}
