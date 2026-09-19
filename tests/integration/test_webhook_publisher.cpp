// =============================================================================
//  P10 切片 6b（C10.19）：事件发布器 `events.publisher` 与 `events.webhook.*`
// =============================================================================
//  判据（`docs/04-implementation-plan.md`「阶段 10」的 C10.19 / ADR-013 §9）：
//    · `events.publisher=webhook` 真的把**两个事件**都 POST 到
//      `events.webhook.url`（**该 URL 就是完整端点**，不追加路径）；
//    · 载荷形状：`statusChanged` 的 `body` 是对象、`datasetDetails` 的 `body` 是
//      **长度为 1 的数组**；`topic` 取**配置值** `events.webhook.topic`；
//    · ★ **非致命是本切片的核心**：webhook 连不上 / 超时 / 非 2xx **绝不让请求失败** ——
//      建记录请求照常 **201**，且记录**真的建出来**（persistent 侧有文件）。
//      这是 6a「校验失败无残留」的**镜像断言**（两条语义方向相反，必须各自被测）；
//    · `events.publisher=none` → **显式关闭**：配了 url 也一个请求都不发；
//    · `events.publisher=log`（默认）→ 行为与接线前逐字一致、**不发请求**；
//    · `webhook` + 空 `url` → **exit 78** + 可读原因（发生在进入服务状态之前）；
//    · `events.webhook.timeout_ms` **真的来自配置**（同一 mock：300ms → 告警；3000ms → 无告警）。
//
//  ★ 为什么必须是**真实进程 + 可控 mock**：
//    - 进程内假对象只能证明"用例层把端口调了"，证明不了"真实二进制按配置选择了
//      webhook 发布器、并把事件体发到了配置里的地址"（组合根 R12；AGENTS §4.3 的
//      "测试夹具接上了、组合根没接 = 产品里不存在"）；
//    - 超时/连不上只有独立进程的 mock 才能确定性注入（与 C10.18 同理）。
//
//  ★ **发布失败非致命**的一手依据：`docs/03-api-contract.md` §2.6 第 10 步 +
//    `src/domain/ports/ports.h`（上游只 `log.warning("Failed to publish ...")`）。
//    因此本切片**不能**用 `FSS_TRY` 传播发布错误（R1 自证①正是把这个方向反过来）。
//
//  ⚠️ **同步发布的延迟代价**：一次 `createMetadata` 发 2~3 个事件，一个慢 webhook 最多给
//    请求路径增加 `事件数 × timeout_ms`。异步有界队列/重试退避**未交付**（ADR-013 §9.4）。
// =============================================================================
#include <catch2/catch.hpp>

#include "http_fixture.h"  // Authed / TargetOf
#include "mock_validators.h"
#include "raw_http.h"
#include "server_process.h"
#include "temp_dir.h"

#include "common/json/json.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using fss::test::Authed;
using fss::test::HttpDo;
using fss::test::MockValidators;
using fss::test::ProcessOutcome;
using fss::test::RawClient;
using fss::test::RunServerForExit;
using fss::test::ServerProcess;
using fss::test::ServerProcessOptions;
using fss::test::TargetOf;
using fss::test::TempDir;

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

bool WaitReady(int port, int attempts = 200) {
  for (int i = 0; i < attempts; ++i) {
    RawClient client(port, /*tcp_nodelay=*/true);
    if (client.Connect() && client.SendRequest("GET", "/api/file/v2/readiness_check", {}, "")) {
      const auto response = client.ReadResponse(10000);
      if (response.has_value() && response->status == 200) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

//  契约 §2.6 的一份**最小合法**记录（与 `test_remote_validators.cpp` 同源）。
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

struct RegisterResult {
  int upload_status = 0;
  int put_status = 0;
  int create_status = 0;
  std::string create_body;
  std::string file_source;
  std::string signed_url;
};

RegisterResult UploadAndRegister(int port, const std::string& body, const std::string& name) {
  RegisterResult out;
  const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  out.upload_status = upload.status;
  REQUIRE(upload.status == 200);
  const auto parsed = fss::json::ParseObject(upload.body);
  REQUIRE(parsed.ok());
  const auto& location = parsed.value()["Location"];
  const std::string file_source = location["FileSource"].get<std::string>();
  const std::string signed_url = location["SignedURL"].get<std::string>();
  out.file_source = file_source;
  out.signed_url = signed_url;

  const auto put = HttpDo(port, "PUT", TargetOf(signed_url), Authed(), body);
  out.put_status = put.status;
  REQUIRE(put.status == 200);

  const auto created = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(),
                              RecordJson(file_source, name));
  out.create_status = created.status;
  out.create_body = created.body;
  return out;
}

//  "记录**真的建出来**"的判据（本切片的核心断言，是 6a 的 `RequireNoPersistentSideEffect`
//  的**镜像**）。三条独立事实缺一不可：
//    ① `getFileList` 列出**恰好一条**记录；
//    ② 该记录的 `Location` 指向 **persistent** 容器（第 6 步的复制真的发生了）；
//    ③ 对应的 persistent 文件**在磁盘上存在**（不是只写了一条位置记录）。
//  ⚠️ `getFileList` 的 `Location` 是**容器相对路径**（`opendes-persistent/...`），
//    而 POSIX 驱动的物理根是 `<storage.posix.root>/blobs`（组合根 `storage_root + "/blobs"`），
//    因此"文件存在"必须拼出**真实物理路径**再断言 —— 直接 `exists(location)` 恒为 false
//    （判据恒真的反面：恒假；切片 6a 的 `RequireNoPersistentSideEffect` 用的就是相对的
//     `exists`，那里只因为断言的是 `REQUIRE_FALSE` 才没有暴露）。
void RequirePersistentRecordCreated(int port, const std::string& blob_root) {
  const auto list = HttpDo(port, "POST", "/api/file/v2/getFileList", Authed(),
                           R"({"PageNum":0,"Items":50})");
  CAPTURE(list.status, list.body);
  REQUIRE(list.status == 200);
  const auto parsed = fss::json::ParseObject(list.body);
  REQUIRE(parsed.ok());
  const auto& content = parsed.value()["Content"];
  REQUIRE(content.is_array());
  REQUIRE(content.size() == 1);

  const std::string location = content[0]["Location"].get<std::string>();
  CAPTURE(location);
  //  ② 位置记录已经迁到 **persistent** 容器（第 6 步的复制真的发生了）
  REQUIRE(location.find("opendes-persistent/") != std::string::npos);
  //  ③ persistent 文件**真的在磁盘上**（不是只写了一条位置记录）
  const std::filesystem::path persistent_path = std::filesystem::path(blob_root) / location;
  INFO("persistent 侧应当存在的文件：" << persistent_path.string());
  REQUIRE(std::filesystem::exists(persistent_path));
}

//  自签 URL 的 `<base>` 取自 `self_signed.public_base_url`，因此配置里必须给**具体**端口。
std::string EventsConfig(const TempDir& dir, const std::string& name, int port,
                         const TempDir& data_dir, const std::string& events_block) {
  return WriteFile(
      dir, name,
      "{\n"
      "  // 带注释的 JSON：C10.19 的真实进程用例\n"
      "  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
          std::to_string(port) +
          "}},\n"
          "  \"storage\": {\"posix\": {\"root\": \"" + data_dir.child("store") + "\"}},\n"
          "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child("loc.db") + "\"}},\n"
          "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child("meta.db") + "\"}},\n"
          "  \"self_signed\": {\"signing_key\": \"webhook-signing-secret\", "
          "\"public_base_url\": \"http://127.0.0.1:" + std::to_string(port) + "/api/file\"},\n"
          "  \"auth\": {\"mode\": \"disabled\"},\n" +
          events_block + "\n}\n");
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

std::string EventsBlock(const std::string& publisher, const std::string& url, int timeout_ms,
                        const std::string& topic) {
  return "  \"events\": {\"publisher\": \"" + publisher +
         "\", \"webhook\": {\"url\": \"" + url + "\", \"timeout_ms\": " +
         std::to_string(timeout_ms) + ", \"topic\": \"" + topic + "\"}}";
}

int ClosedPort() { return FreePort(); }

}  // namespace

// =============================================================================
//  C10.19 ①：正例（R16）+ 两个事件的载荷形状 + topic 真的来自配置
// =============================================================================
TEST_CASE("★ C10.19 ① publisher=webhook 正例：201 + statusChanged/datasetDetails 都发到配置端点",
          "[phase10][integration][c10.19]") {
  TempDir cfg_dir("fss_c10_19_ok");
  TempDir data_dir("fss_c10_19_ok_data");
  const int port = FreePort();
  REQUIRE(port > 0);
  MockValidators::Options mock_options;
  mock_options.mode = "webhook";  // 只认 2xx = 成功
  MockValidators mock(mock_options);
  const std::string config =
      EventsConfig(cfg_dir, "webhook.json", port, data_dir,
                   EventsBlock("webhook", mock.base_url(), 3000, "fss-events-test"));

  ServerProcess server(FileDriven(config));
  CAPTURE(server.DumpLog());
  REQUIRE(server.http_port() == port);
  REQUIRE(WaitReady(port));
  //  横幅必须让运维一眼看到 publisher/端点/超时/topic（**不打印任何密钥**）
  const std::string banner = server.DumpLog();
  REQUIRE(banner.find("events         : publisher=webhook") != std::string::npos);
  REQUIRE(banner.find(mock.base_url()) != std::string::npos);
  REQUIRE(banner.find("timeout=3000ms") != std::string::npos);
  REQUIRE(banner.find("topic=fss-events-test") != std::string::npos);
  REQUIRE(banner.find("webhook-signing-secret") == std::string::npos);

  const auto result = UploadAndRegister(port, "c10-19-ok", "webhook-ok.txt");
  CAPTURE(result.create_status, result.create_body);
  REQUIRE(result.create_status == 201);

  //  一次 createMetadata 会发 3 个事件：statusChanged(IN_PROGRESS) +
  //  statusChanged(SUCCESS) + datasetDetails。等 3 个都到齐。
  const auto observed = mock.WaitRequests(3);
  CAPTURE(observed.raw);
  REQUIRE(observed.requests == 3);

  //  ★ 记录 id 从 201 响应体里取（契约 §2.6），用它断言事件体**带的是真实 id**
  const auto created_json = fss::json::ParseObject(result.create_body);
  REQUIRE(created_json.ok());
  const std::string record_id = created_json.value()["id"].get<std::string>();
  REQUIRE_FALSE(record_id.empty());

  const auto statuses = observed.BodiesOfKind("statusChanged");
  const auto details = observed.BodiesOfKind("datasetDetails");
  CAPTURE(statuses.size(), details.size());
  REQUIRE(statuses.size() == 2);
  REQUIRE(details.size() == 1);

  bool saw_in_progress = false;
  bool saw_success = false;
  for (const auto& message : statuses) {
    //  ★ topic 必须等于**配置的** topic（非默认值 "fss-events-test" 才有区分力）
    REQUIRE(message["topic"].get<std::string>() == "fss-events-test");
    const auto& body = message["body"];
    REQUIRE(body.is_object());
    REQUIRE(body["partition"].get<std::string>() == "opendes");
    REQUIRE(body["datasetSync"].get<std::string>() == "DATASET_SYNC");
    REQUIRE(body.contains("recordId"));
    REQUIRE(body.contains("version"));
    const std::string status = body["status"].get<std::string>();
    if (status == "IN_PROGRESS") {
      saw_in_progress = true;
      //  第 1 步发生在建记录之前 → 此时确实还没有记录 id
      REQUIRE(body["recordId"].get<std::string>().empty());
      REQUIRE(body["version"].get<std::int64_t>() == 0);
    } else if (status == "SUCCESS") {
      saw_success = true;
      //  ★ SUCCESS 必须带上**真实**的记录 id 与非零版本
      REQUIRE(body["recordId"].get<std::string>() == record_id);
      REQUIRE(body["version"].get<std::int64_t>() >= 1);
    }
  }
  REQUIRE(saw_in_progress);
  REQUIRE(saw_success);

  //  datasetDetails：body 是**长度为 1 的数组**，元素是 {"properties":{...}}
  const auto& detail = details[0];
  REQUIRE(detail["topic"].get<std::string>() == "fss-events-test");
  REQUIRE(detail["body"].is_array());
  REQUIRE(detail["body"].size() == 1);
  const auto& properties = detail["body"][0]["properties"];
  REQUIRE(properties.is_object());
  REQUIRE(properties["datasetId"].get<std::string>() == record_id);
  REQUIRE(properties["datasetType"].get<std::string>() == "FILE");
  REQUIRE(properties["recordCount"].get<int>() == 1);
  REQUIRE(properties["datasetVersionId"].get<std::string>() == "1");
  REQUIRE(properties["timestamp"].get<std::int64_t>() > 0);
  REQUIRE(properties.contains("correlationId"));

  //  线协议的表头（ADR-013 §9）
  REQUIRE(observed.last_headers.count("Content-Type") == 1);
  REQUIRE(observed.last_headers.at("Content-Type").find("application/json") != std::string::npos);
}

// =============================================================================
//  C10.19 ②：**非致命**（本切片的核心）—— 连不上 / 500 / 超时 → 请求仍 201，
//  且记录**真的建出来**；日志里有可读告警。三条形态各自独立注入。
// =============================================================================
TEST_CASE("★ C10.19 ② 非致命：连不上 / 非 2xx / 超时 → 请求仍 201 且记录真的建出来",
          "[phase10][integration][c10.19]") {
  struct Case {
    const char* name;
    int timeout_ms;
    bool closed_port;  // 指向未监听端口（连不上）
    int force_status;  // 非 0 → mock 一律回该状态码
    int delay_ms;      // > timeout_ms → 触发客户端超时
  };
  std::vector<Case> cases;
  cases.push_back(Case{"连不上（未监听端口）", 1000, true, 0, 0});
  cases.push_back(Case{"非 2xx（500）", 3000, false, 500, 0});
  cases.push_back(Case{"超时（delay 800 > timeout 300）", 300, false, 0, 800});

  int index = 0;
  for (const auto& test_case : cases) {
    INFO("非致命形态：" << test_case.name);
    TempDir cfg_dir("fss_c10_19_nonfatal");
    TempDir data_dir("fss_c10_19_nonfatal_data");
    const int port = FreePort();
    REQUIRE(port > 0);
    std::string endpoint;
    std::unique_ptr<MockValidators> mock;
    if (test_case.closed_port) {
      endpoint = "http://127.0.0.1:" + std::to_string(ClosedPort());
    } else {
      MockValidators::Options mock_options;
      mock_options.mode = "webhook";
      mock_options.force_status = test_case.force_status;
      mock_options.delay_ms = test_case.delay_ms;
      mock = std::make_unique<MockValidators>(mock_options);
      endpoint = mock->base_url();
    }
    const std::string config = EventsConfig(
        cfg_dir, "webhook.json", port, data_dir,
        EventsBlock("webhook", endpoint, test_case.timeout_ms, "fss-events-test"));

    ServerProcess server(FileDriven(config));
    CAPTURE(server.DumpLog());
    REQUIRE(WaitReady(port));
    const auto result = UploadAndRegister(port, "c10-19-nonfatal-" + std::to_string(index++),
                                          "nonfatal.txt");
    CAPTURE(test_case.name, result.create_status, result.create_body);
    //  ★★ 判据一：发布失败**绝不能**让请求失败
    REQUIRE(result.create_status == 201);
    //  ★★ 判据二：记录**真的建出来**（6a「无残留」的镜像）
    RequirePersistentRecordCreated(port, data_dir.child("store") + "/blobs");
    //  ★★ 判据三：日志里有可读告警（不是静默吞掉）
    REQUIRE(server.DumpLog().find("Failed to publish event") != std::string::npos);
    //  依赖确实被调用过（"连不上"那条没有 mock，跳过）
    if (mock) REQUIRE(mock->WaitRequests(1).requests >= 1);
  }
}

// =============================================================================
//  C10.19 ③：`events.publisher=none` → **显式关闭**：配了 url 也一个请求都不发
// =============================================================================
TEST_CASE("★ C10.19 ③ publisher=none：显式关闭 —— 配了 url 也一个请求都不发",
          "[phase10][integration][c10.19]") {
  TempDir cfg_dir("fss_c10_19_none");
  TempDir data_dir("fss_c10_19_none_data");
  const int port = FreePort();
  REQUIRE(port > 0);
  MockValidators::Options mock_options;
  mock_options.mode = "webhook";
  MockValidators mock(mock_options);
  const std::string config =
      EventsConfig(cfg_dir, "none.json", port, data_dir,
                   EventsBlock("none", mock.base_url(), 3000, "fss-events-test"));

  ServerProcess server(FileDriven(config));
  CAPTURE(server.DumpLog());
  REQUIRE(WaitReady(port));
  const auto result = UploadAndRegister(port, "c10-19-none", "none.txt");
  CAPTURE(result.create_status, result.create_body);
  REQUIRE(result.create_status == 201);
  RequirePersistentRecordCreated(port, data_dir.child("store") + "/blobs");
  //  ★ 一次都没发（不只是"没超时"）
  REQUIRE(mock.RequestsStayZero(400));
  const std::string banner = server.DumpLog();
  REQUIRE(banner.find("events         : publisher=none") != std::string::npos);
  REQUIRE(banner.find("显式关闭") != std::string::npos);
  //  没有发布 → 不该有发布失败告警
  REQUIRE(banner.find("Failed to publish event") == std::string::npos);
  //  ★ "显式关闭" = **什么都不做**（连日志都不写）：这是把 `none` 误接成
  //    `LogEventPublisher`（而不是 `NoopEventPublisher`）时唯一会失败的断言 ——
  //    只断言"mock 收到 0 个请求"区分不出这两者（日志发布器也不发 HTTP）。
  REQUIRE(banner.find("\"msg\":\"status-changed\"") == std::string::npos);
}

// =============================================================================
//  C10.19 ④：`events.publisher=log`（默认）不受影响 —— 配了 url 也不发请求
// =============================================================================
TEST_CASE("★ C10.19 ④ publisher=log（默认）不受影响：配了 url 也不发请求、照旧写日志",
          "[phase10][integration][c10.19]") {
  TempDir cfg_dir("fss_c10_19_log");
  TempDir data_dir("fss_c10_19_log_data");
  const int port = FreePort();
  REQUIRE(port > 0);
  MockValidators::Options mock_options;
  mock_options.mode = "webhook";
  MockValidators mock(mock_options);
  const std::string config =
      EventsConfig(cfg_dir, "log.json", port, data_dir,
                   EventsBlock("log", mock.base_url(), 3000, "fss-events-test"));

  ServerProcess server(FileDriven(config));
  CAPTURE(server.DumpLog());
  REQUIRE(WaitReady(port));
  const auto result = UploadAndRegister(port, "c10-19-log", "log.txt");
  CAPTURE(result.create_status, result.create_body);
  REQUIRE(result.create_status == 201);
  RequirePersistentRecordCreated(port, data_dir.child("store") + "/blobs");
  REQUIRE(mock.RequestsStayZero(400));
  const std::string banner = server.DumpLog();
  REQUIRE(banner.find("events         : publisher=log") != std::string::npos);
  //  接线前的 LogEventPublisher 行为逐字保留：真的写了这两条事件日志
  REQUIRE(banner.find("\"msg\":\"status-changed\"") != std::string::npos);
  REQUIRE(banner.find("\"msg\":\"datasetDetails\"") != std::string::npos);
  REQUIRE(banner.find("Failed to publish event") == std::string::npos);
}

// =============================================================================
//  C10.19 ⑤：`publisher=webhook` 但 `events.webhook.url` 为空 → **exit 78** + 可读原因
//  ★ R16 正例对照：同一个二进制在 url 配好时能正常启动（① 已覆盖）。
// =============================================================================
TEST_CASE("★ C10.19 ⑤ publisher=webhook + url 为空 → exit 78 + 可读原因",
          "[phase10][integration][c10.19]") {
  TempDir cfg_dir("fss_c10_19_reject");
  TempDir data_dir("fss_c10_19_reject_data");
  const std::string config =
      EventsConfig(cfg_dir, "webhook.json", FreePort(), data_dir,
                   EventsBlock("webhook", "", 3000, "fss-events-test"));
  const ProcessOutcome outcome = RunServerForExit({"--config", config});
  CAPTURE(outcome.exit_code, outcome.output);
  REQUIRE(outcome.exit_code == 78);
  REQUIRE(outcome.output.find("拒绝启动") != std::string::npos);
  REQUIRE(outcome.output.find("events.webhook.url") != std::string::npos);
  //  拒绝启动必须**发生在进入服务状态之前**
  REQUIRE(outcome.output.find("已启动") == std::string::npos);
}

// =============================================================================
//  C10.19 ⑥：`events.webhook.timeout_ms` **真的来自配置**
//  ★ 关键是"同一个 mock、只改配置里的 timeout"：300ms → 有超时告警；3000ms → 无告警
//    且 mock 收到了事件体。否则"有告警"可能只是"这个 mock 永远超时"（判据恒真）。
// =============================================================================
TEST_CASE("★ C10.19 ⑥ timeout_ms 真的生效：同一 mock（delay 800），300 → 告警；3000 → 无告警",
          "[phase10][integration][c10.19]") {
  MockValidators::Options mock_options;
  mock_options.mode = "webhook";
  mock_options.delay_ms = 800;  // > 300ms，< 3000ms
  MockValidators mock(mock_options);

  int index = 0;
  for (const int timeout_ms : {300, 3000}) {
    TempDir cfg_dir("fss_c10_19_timeout");
    TempDir data_dir("fss_c10_19_timeout_data");
    const int port = FreePort();
    REQUIRE(port > 0);
    const std::string config =
        EventsConfig(cfg_dir, "webhook.json", port, data_dir,
                     EventsBlock("webhook", mock.base_url(), timeout_ms, "fss-events-test"));
    ServerProcess server(FileDriven(config));
    REQUIRE(WaitReady(port));
    const int before = mock.ReadObservation().requests;
    const auto result = UploadAndRegister(port, "c10-19-timeout-" + std::to_string(index++),
                                          "timeout.txt");
    CAPTURE(timeout_ms, result.create_status, result.create_body);
    //  两种取值下请求都必须成功（非致命）
    REQUIRE(result.create_status == 201);
    RequirePersistentRecordCreated(port, data_dir.child("store") + "/blobs");
    const std::string log = server.DumpLog();
    if (timeout_ms == 300) {
      //  超时 → 可读告警（而且**不影响**上面的 201）
      REQUIRE(log.find("Failed to publish event") != std::string::npos);
    } else {
      //  超时放大后**同一个 mock** 就成功了：没有告警，且事件体真的到了
      REQUIRE(log.find("Failed to publish event") == std::string::npos);
      const auto observed = mock.WaitRequests(before + 3);
      CAPTURE(observed.raw);
      REQUIRE(observed.requests >= before + 3);
      //  ★ mock 是**复用**的（上一轮 300ms 的请求体也在 `bodies` 里，且它们的
      //    到达时机受 mock 的 delay 影响），所以不能按数量断言"本轮 3 条"。
      //    改成按**本轮的记录 id** 定位：两个 kind 都要带这轮的 id。
      const auto created_json = fss::json::ParseObject(result.create_body);
      REQUIRE(created_json.ok());
      const std::string record_id = created_json.value()["id"].get<std::string>();
      REQUIRE_FALSE(record_id.empty());
      bool saw_status_for_record = false;
      bool saw_details_for_record = false;
      for (const auto& entry : observed.bodies) {
        if (!entry.is_object() || !entry.contains("kind") || !entry.contains("body")) continue;
        const std::string kind = entry["kind"].get<std::string>();
        const auto& body = entry["body"];
        if (kind == "statusChanged" && body.is_object() && body.contains("recordId") &&
            body["recordId"].get<std::string>() == record_id) {
          saw_status_for_record = true;
        }
        if (kind == "datasetDetails" && body.is_array() && body.size() == 1 &&
            body[0]["properties"]["datasetId"].get<std::string>() == record_id) {
          saw_details_for_record = true;
        }
      }
      REQUIRE(saw_status_for_record);
      REQUIRE(saw_details_for_record);
    }
  }
}
