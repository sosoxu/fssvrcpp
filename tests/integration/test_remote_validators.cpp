// =============================================================================
//  P10 切片 6a（C10.18）：远端 legal / schema 校验器（真实二进制 + 可控 mock）
// =============================================================================
//  判据（`docs/04-implementation-plan.md`「阶段 10」的 C10.18 / ADR-013）：
//    · `legal.validator=remote` / `schema.validator=remote` 真的把校验请求发给
//      `*.remote.base_url`（**该 URL 就是完整端点**，不追加路径）；
//    · `200 + {"valid":true}` → 记录被建出来（R16 正例）；
//    · `200 + {"valid":false,"message":M}` → **400**，且响应体里带上远端的 M；
//    · **fail-closed 五态**（超时 / 非 200 / 非 JSON / 缺 valid / 连不上）→ **503**，
//      且**没有记录被建出来**（校验发生在第 3c 步、持久化之前）；
//    · `*.remote.base_url` 为空且选择器为 `remote` → **exit 78** + 可读原因；
//    · `noop`（默认）**不发起任何请求**（mock 的 `requests == 0`）；
//    · `*.remote.timeout_ms` **真的来自配置**（同一 mock 在超时放大后 → 201）。
//
//  ★ 为什么必须是**真实进程 + 可控 mock**：
//    - 进程内假对象只能证明"用例层把端口调了"，证明不了"真实二进制按配置选择了
//      远端校验器、并且把请求发到了配置里的地址"（组合根 R12；AGENTS §4.3 的
//      "测试夹具接上了、组合根没接 = 产品里不存在"）；
//    - 超时/连不上只有独立进程的 mock 才能确定性注入（与 C8.4 用 mock_entitlements
//      的理由相同）。
//
//  ★ 远端端点**没有上游路径依据**：`docs/01-osdu-research.md:110` 指出 legal tag 的
//    合规性由 Storage Service 的 PUT /records 内部校验，上游 File Service 不直接调用
//    Legal/Schema 服务。因此这是本服务的**扩展**（ADR-013），线协议是本项目约定，
//    **未与真实 Legal/Schema 服务联调**（已登记在 ADR-013 §5 与 phase10 证据 §11）。
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

struct HttpReply {
  bool transport_ok = false;
  int status = 0;
  std::string body;
};

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

//  契约 §2.6 的一份**最小合法**记录（字段集与 `AppFixture::MakeRecord` 同源）。
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

//  一次完整的"上传字节 → 登记元数据"（与 C4.8 的端到端切片同一顺序）。
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

//  "校验失败 **不留残留**"的判据（不能只看状态码）。
//
//  ★ 这里有一条本轮实测抓到的**假判据**（记在 phase10 证据 §11 的"未做/修正"里）：
//    最初写的是"`getFileList` 必须 400（无记录）"，但 `getFileList` 列的是
//    **位置仓储**（`location`）而不是元数据 —— `uploadURL` 一旦签发就已经写了一条
//    staging 位置记录，所以即使元数据请求失败，`getFileList` 仍返回 **200**。
//    那条断言若"通过"只能说明前置上传没做，测不到"元数据没建成"。
//
//  正确判据（元数据 12 步序列的**可观测副作用**）：`uploadURL` 返回的 `Location`
//  指向 **staging** 对象；只有第 6 步（`CreateFileMetadata` 的复制）会把它带到
//  **persistent**，而校验发生在第 3c 步 —— 因此"校验失败后 staging 对象仍在原位、
//  persistent 侧没有对应文件"才是"没有记录被建出来"的可证事实。
void RequireNoPersistentSideEffect(int port) {
  const auto list = HttpDo(port, "POST", "/api/file/v2/getFileList", Authed(),
                           R"({"PageNum":0,"Items":50})");
  CAPTURE(list.status, list.body);
  REQUIRE(list.status == 200);  // staging 位置记录仍在（我们确实上传过）
  const auto parsed = fss::json::ParseObject(list.body);
  REQUIRE(parsed.ok());
  const auto& content = parsed.value()["Content"];
  REQUIRE(content.is_array());
  REQUIRE(content.size() == 1);

  //  ② `Location` 必须仍然指向 **staging** 容器（第 6 步没有发生）
  const std::string location = content[0]["Location"].get<std::string>();
  CAPTURE(location);
  REQUIRE(location.find("opendes-staging/") != std::string::npos);
  //  ③ 对应的 persistent 文件**不存在**（复制/落库都没发生）
  std::string persistent = location;
  const auto pos = persistent.find("opendes-staging/");
  persistent.replace(pos, std::string("opendes-staging/").size(), "opendes-persistent/");
  const std::filesystem::path persistent_path(persistent);
  INFO("persistent 侧不应存在的文件：" << persistent_path.string());
  REQUIRE_FALSE(std::filesystem::exists(persistent_path));
}

//  自签 URL 的 `<base>` 取自 `self_signed.public_base_url`（默认用的是配置端口），
//  因此配置里必须给一个**具体**端口（不能用 0，否则 URL 指向 0 号端口）。
struct ServerOptionsWithConfig {
  ServerProcessOptions options;
  std::string config_path;
};

std::string ValidatorConfig(const TempDir& dir, const std::string& name, int port,
                            const TempDir& data_dir, const std::string& legal_block,
                            const std::string& schema_block) {
  return WriteFile(
      dir, name,
      "{\n"
      "  // 带注释的 JSON：C10.18 的真实进程用例\n"
      "  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": " +
          std::to_string(port) +
          "}},\n"
          "  \"storage\": {\"posix\": {\"root\": \"" + data_dir.child("store") + "\"}},\n"
          "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child("loc.db") + "\"}},\n"
          "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child("meta.db") + "\"}},\n"
          "  \"self_signed\": {\"signing_key\": \"remote-validator-secret\", "
          "\"public_base_url\": \"http://127.0.0.1:" + std::to_string(port) + "/api/file\"},\n"
          "  \"auth\": {\"mode\": \"disabled\"},\n" +
          legal_block + ",\n" + schema_block + "\n}\n");
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

std::string LegalBlock(const std::string& validator, const std::string& base_url,
                       int timeout_ms) {
  return "  \"legal\": {\"validator\": \"" + validator + "\", \"remote\": {\"base_url\": \"" +
         base_url + "\", \"timeout_ms\": " + std::to_string(timeout_ms) + "}}";
}

std::string SchemaBlock(const std::string& validator, const std::string& base_url,
                        int timeout_ms) {
  return "  \"schema\": {\"validator\": \"" + validator + "\", \"remote\": {\"base_url\": \"" +
         base_url + "\", \"timeout_ms\": " + std::to_string(timeout_ms) + "}}";
}

//  未监听的端口：`bind` 成功后立刻关闭 → 该端口上**一定**没有监听者（"连不上"用例）。
int ClosedPort() { return FreePort(); }

}  // namespace

// =============================================================================
//  C10.18 ①：正例（R16）+ 请求体形状 + "不通过 → 400 带远端 message"
// =============================================================================
TEST_CASE("★ C10.18 ① legal.validator=remote：请求真的发到 base_url（完整端点）+ 正例 201",
          "[phase10][integration][c10.18]") {
  TempDir cfg_dir("fss_c10_18_legal_ok");
  TempDir data_dir("fss_c10_18_legal_ok_data");
  const int port = FreePort();
  REQUIRE(port > 0);
  MockValidators::Options mock_options;
  mock_options.mode = "legal";
  mock_options.valid = true;
  MockValidators mock(mock_options);
  const std::string config =
      ValidatorConfig(cfg_dir, "legal.json", port, data_dir,
                      LegalBlock("remote", mock.base_url(), 3000), SchemaBlock("noop", "", 3000));

  ServerProcess server(FileDriven(config));
  CAPTURE(server.DumpLog());
  REQUIRE(server.http_port() == port);
  REQUIRE(WaitReady(port));
  //  横幅必须让运维一眼看到选择了远端校验器、端点与超时（**不打印任何密钥**）
  const std::string banner = server.DumpLog();
  REQUIRE(banner.find("validators     : legal=remote") != std::string::npos);
  REQUIRE(banner.find(mock.base_url()) != std::string::npos);
  REQUIRE(banner.find("timeout=3000ms") != std::string::npos);
  REQUIRE(banner.find("remote-validator-secret") == std::string::npos);

  const auto result = UploadAndRegister(port, "c10-18-legal-ok", "legal-ok.txt");
  CAPTURE(result.create_status, result.create_body);
  REQUIRE(result.create_status == 201);

  //  ★ 请求体形状（legal：partition + legaltags）—— 否则"客户端按约定发了请求"
  //    只是文档里的一句话（R15）
  const auto observed = mock.WaitRequests(1);
  CAPTURE(observed.raw);
  REQUIRE(observed.requests == 1);
  REQUIRE(observed.body.is_object());
  REQUIRE(observed.body["partition"].get<std::string>() == "opendes");
  REQUIRE(observed.body["legaltags"].is_array());
  REQUIRE(observed.body["legaltags"].size() == 1);
  REQUIRE(observed.body["legaltags"][0].get<std::string>() == "opendes-public-1");
  //  端点就是配置里的 URL，**不追加任何路径**（ADR-013 §2）
  REQUIRE(observed.body.contains("record") == false);
}

TEST_CASE("★ C10.18 ② legal.validator=remote：远端不通过 → 400 且带上远端 message",
          "[phase10][integration][c10.18]") {
  TempDir cfg_dir("fss_c10_18_legal_invalid");
  TempDir data_dir("fss_c10_18_legal_invalid_data");
  const int port = FreePort();
  REQUIRE(port > 0);
  MockValidators::Options mock_options;
  mock_options.mode = "legal";
  mock_options.invalid_message = "bad tag";
  MockValidators mock(mock_options);
  const std::string config =
      ValidatorConfig(cfg_dir, "legal.json", port, data_dir,
                      LegalBlock("remote", mock.base_url(), 3000), SchemaBlock("noop", "", 3000));

  ServerProcess server(FileDriven(config));
  REQUIRE(WaitReady(port));
  const auto result = UploadAndRegister(port, "c10-18-legal-bad", "legal-bad.txt");
  CAPTURE(result.create_status, result.create_body);
  REQUIRE(result.create_status == 400);
  //  远端的 message 必须被透传（而不是被替换成通用文案）
  REQUIRE(result.create_body.find("bad tag") != std::string::npos);
  //  "不通过"**不是**依赖故障：mock 收到了请求，记录**没有**被建出来
  REQUIRE(mock.WaitRequests(1).requests == 1);
  RequireNoPersistentSideEffect(port);
}

// =============================================================================
//  C10.18 ③：fail-closed 五态（超时 / 500 / 坏 JSON / 缺 valid / 连不上）
//  每一条都断言：**503** + **没有记录被建出来**（校验在第 3c 步、持久化之前）
// =============================================================================
TEST_CASE("★ C10.18 ③ legal fail-closed：超时 / 非 200 / 非 JSON / 缺 valid / 连不上 → 一律 503",
          "[phase10][integration][c10.18]") {
  struct Case {
    const char* name;
    MockValidators::Options mock;  // mode 由循环设置
    int timeout_ms;
    bool use_closed_port;
  };
  std::vector<Case> cases;
  {
    Case c;
    c.name = "超时";
    c.mock.delay_ms = 2000;  // > timeout_ms
    c.timeout_ms = 300;
    c.use_closed_port = false;
    cases.push_back(c);
  }
  {
    Case c;
    c.name = "非 200（500）";
    c.mock.force_status = 500;
    c.timeout_ms = 3000;
    c.use_closed_port = false;
    cases.push_back(c);
  }
  {
    Case c;
    c.name = "非 JSON";
    c.mock.malformed = true;
    c.timeout_ms = 3000;
    c.use_closed_port = false;
    cases.push_back(c);
  }
  {
    Case c;
    c.name = "缺 valid 字段";
    c.mock.no_valid_field = true;
    c.timeout_ms = 3000;
    c.use_closed_port = false;
    cases.push_back(c);
  }
  {
    Case c;
    c.name = "连不上（未监听端口）";
    c.timeout_ms = 1000;
    c.use_closed_port = true;
    cases.push_back(c);
  }

  int index = 0;
  for (auto& test_case : cases) {
    test_case.mock.mode = "legal";
    INFO("fail-closed 形态：" << test_case.name);
    TempDir cfg_dir("fss_c10_18_fc");
    TempDir data_dir("fss_c10_18_fc_data");
    const int port = FreePort();
    REQUIRE(port > 0);
    std::string endpoint;
    std::unique_ptr<MockValidators> mock;
    if (test_case.use_closed_port) {
      endpoint = "http://127.0.0.1:" + std::to_string(ClosedPort());
    } else {
      mock = std::make_unique<MockValidators>(test_case.mock);
      endpoint = mock->base_url();
    }
    const std::string config = ValidatorConfig(
        cfg_dir, "legal.json", port, data_dir,
        LegalBlock("remote", endpoint, test_case.timeout_ms), SchemaBlock("noop", "", 3000));

    ServerProcess server(FileDriven(config));
    CAPTURE(server.DumpLog());
    REQUIRE(WaitReady(port));
    const std::string body = "c10-18-fc-" + std::to_string(index++);
    const auto result = UploadAndRegister(port, body, "fc.txt");
    CAPTURE(test_case.name, result.upload_status, result.put_status, result.create_status,
            result.create_body);
    REQUIRE(result.upload_status == 200);
    REQUIRE(result.put_status == 200);
    //  ★ fail-closed：503（**不是** 400，也**不是** 201）
    REQUIRE(result.create_status == 503);
    //  ★ 不留残留：没有位置记录（校验失败发生在持久化之前）
    RequireNoPersistentSideEffect(port);
    //  依赖确实被调用过（"连不上"那条没有 mock，跳过）
    if (mock) REQUIRE(mock->WaitRequests(1).requests >= 1);
  }
}

// =============================================================================
//  C10.18 ④：`legal.remote.timeout_ms` **真的来自配置**
//  ★ 关键是"同一个 mock、只改配置里的 timeout"：300ms → 503，3000ms → 201。
//    否则 503 可能只是"这个 mock 永远超时"（判据恒真）。
// =============================================================================
TEST_CASE("★ C10.18 ④ timeout_ms 真的生效：同一个 mock，300ms → 503；3000ms → 201",
          "[phase10][integration][c10.18]") {
  MockValidators::Options mock_options;
  mock_options.mode = "legal";
  mock_options.valid = true;
  mock_options.delay_ms = 800;  // > 300ms，< 3000ms
  MockValidators mock(mock_options);

  int index = 0;
  for (const int timeout_ms : {300, 3000}) {
    TempDir cfg_dir("fss_c10_18_timeout");
    TempDir data_dir("fss_c10_18_timeout_data");
    const int port = FreePort();
    REQUIRE(port > 0);
    const std::string config =
        ValidatorConfig(cfg_dir, "legal.json", port, data_dir,
                        LegalBlock("remote", mock.base_url(), timeout_ms),
                        SchemaBlock("noop", "", 3000));
    ServerProcess server(FileDriven(config));
    REQUIRE(WaitReady(port));
    //  ★ 先记录这次请求之前的计数，避免把上一次尝试的请求算进来
    const int before = index == 0 ? 0 : mock.ReadObservation().requests;
    const auto result =
        UploadAndRegister(port, "c10-18-timeout-" + std::to_string(index++), "timeout.txt");
    CAPTURE(timeout_ms, result.create_status, result.create_body);
    if (timeout_ms == 300) {
      REQUIRE(result.create_status == 503);  // 超时 → fail-closed
      RequireNoPersistentSideEffect(port);
    } else {
      REQUIRE(result.create_status == 201);  // 超时放大后**同一个 mock** 就通过了
      REQUIRE(mock.ReadObservation().requests > before);
    }
  }
}

// =============================================================================
//  C10.18 ⑤：noop（默认）**不发起任何请求**，且行为与接线前逐字一致
//  ★ 反向证据用两件事同时证明（避免"指向未监听端口也能成功"是别的原因）：
//    ① mock 在窗口内 `requests == 0`；② 指向**未监听端口**仍然建记录成功。
// =============================================================================
TEST_CASE("★ C10.18 ⑤ noop 不受影响：不发任何请求；*.remote.* 配了地址也不生效",
          "[phase10][integration][c10.18]") {
  TempDir cfg_dir("fss_c10_18_noop");
  TempDir data_dir("fss_c10_18_noop_data");
  const int port = FreePort();
  REQUIRE(port > 0);
  MockValidators::Options mock_options;
  mock_options.mode = "legal";
  mock_options.valid = true;  // 即使远端说"通过"，noop 也不该问它
  MockValidators mock(mock_options);
  //  schema 侧指向一个**未监听**端口：noop 选择器下它必须完全不被使用
  const std::string config =
      ValidatorConfig(cfg_dir, "noop.json", port, data_dir,
                      LegalBlock("noop", mock.base_url(), 300),
                      SchemaBlock("noop", "http://127.0.0.1:" + std::to_string(ClosedPort()), 1));

  ServerProcess server(FileDriven(config));
  REQUIRE(WaitReady(port));
  const auto result = UploadAndRegister(port, "c10-18-noop", "noop.txt");
  CAPTURE(result.create_status, result.create_body);
  REQUIRE(result.create_status == 201);
  //  ① mock 一个请求都没收到（不只是"没超时"）
  REQUIRE(mock.RequestsStayZero(400));
  //  ② 横幅说明 noop 的语义（运维可读）
  const std::string banner = server.DumpLog();
  REQUIRE(banner.find("validators     : legal=noop") != std::string::npos);
  REQUIRE(banner.find("| schema=noop") != std::string::npos);
}

// =============================================================================
//  C10.18 ⑥：schema.validator=remote 的对称一组（正例 / 不通过 / 一种依赖故障）
// =============================================================================
TEST_CASE("★ C10.18 ⑥ schema.validator=remote：正例 201 + 请求体形状（kind/record）",
          "[phase10][integration][c10.18]") {
  TempDir cfg_dir("fss_c10_18_schema_ok");
  TempDir data_dir("fss_c10_18_schema_ok_data");
  const int port = FreePort();
  REQUIRE(port > 0);
  MockValidators::Options mock_options;
  mock_options.mode = "schema";
  mock_options.valid = true;
  MockValidators mock(mock_options);
  const std::string config =
      ValidatorConfig(cfg_dir, "schema.json", port, data_dir, LegalBlock("noop", "", 3000),
                      SchemaBlock("remote", mock.base_url(), 3000));

  ServerProcess server(FileDriven(config));
  CAPTURE(server.DumpLog());
  REQUIRE(WaitReady(port));
  const auto result = UploadAndRegister(port, "c10-18-schema-ok", "schema-ok.txt");
  CAPTURE(result.create_status, result.create_body);
  REQUIRE(result.create_status == 201);

  const auto observed = mock.WaitRequests(1);
  CAPTURE(observed.raw);
  REQUIRE(observed.requests == 1);
  REQUIRE(observed.body.is_object());
  REQUIRE(observed.body["kind"].get<std::string>() == "opendes:wks:dataset--File.Generic:1.0.0");
  REQUIRE(observed.body["record"].is_object());
  //  完整记录 JSON：必须带上待落库记录的字段（否则远端校验的不是我们要存的东西）
  REQUIRE(observed.body["record"]["kind"].get<std::string>() ==
          "opendes:wks:dataset--File.Generic:1.0.0");
  REQUIRE(observed.body["record"]["legal"]["legaltags"][0].get<std::string>() ==
          "opendes-public-1");
  REQUIRE(observed.body["record"]["data"]["Name"].get<std::string>() == "schema-ok.txt");
  //  legal 的字段**不得**出现在 schema 的请求体里（两种线协议不混用）
  REQUIRE(observed.body.contains("partition") == false);
  REQUIRE(observed.body.contains("legaltags") == false);
}

TEST_CASE("★ C10.18 ⑥ schema.validator=remote：不通过 → 400 带 message；连不上 → 503",
          "[phase10][integration][c10.18]") {
  SECTION("远端不通过 → 400 + 远端 message + 无残留") {
    TempDir cfg_dir("fss_c10_18_schema_invalid");
    TempDir data_dir("fss_c10_18_schema_invalid_data");
    const int port = FreePort();
    REQUIRE(port > 0);
    MockValidators::Options mock_options;
    mock_options.mode = "schema";
    mock_options.invalid_message = "missing required field: data.Name";
    MockValidators mock(mock_options);
    const std::string config =
        ValidatorConfig(cfg_dir, "schema.json", port, data_dir, LegalBlock("noop", "", 3000),
                        SchemaBlock("remote", mock.base_url(), 3000));
    ServerProcess server(FileDriven(config));
    REQUIRE(WaitReady(port));
    const auto result = UploadAndRegister(port, "c10-18-schema-bad", "schema-bad.txt");
    CAPTURE(result.create_status, result.create_body);
    REQUIRE(result.create_status == 400);
    REQUIRE(result.create_body.find("missing required field: data.Name") != std::string::npos);
    RequireNoPersistentSideEffect(port);
  }
  SECTION("依赖故障（非 JSON）→ 503 + 无残留") {
    TempDir cfg_dir("fss_c10_18_schema_fc");
    TempDir data_dir("fss_c10_18_schema_fc_data");
    const int port = FreePort();
    REQUIRE(port > 0);
    MockValidators::Options mock_options;
    mock_options.mode = "schema";
    mock_options.malformed = true;
    MockValidators mock(mock_options);
    const std::string config =
        ValidatorConfig(cfg_dir, "schema.json", port, data_dir, LegalBlock("noop", "", 3000),
                        SchemaBlock("remote", mock.base_url(), 3000));
    ServerProcess server(FileDriven(config));
    REQUIRE(WaitReady(port));
    const auto result = UploadAndRegister(port, "c10-18-schema-fc", "schema-fc.txt");
    CAPTURE(result.create_status, result.create_body);
    REQUIRE(result.create_status == 503);
    REQUIRE(mock.WaitRequests(1).requests == 1);
    RequireNoPersistentSideEffect(port);
  }
}

// =============================================================================
//  C10.18 ⑦：`base_url` 为空 + 选择器为 remote → **拒绝启动（exit 78）** + 可读原因
//  ★ R16 正例对照：同一个二进制在 `base_url` 配好时能正常启动（①⑥ 已覆盖）。
// =============================================================================
TEST_CASE("★ C10.18 ⑦ *.validator=remote 但 base_url 为空 → exit 78 + 可读原因",
          "[phase10][integration][c10.18]") {
  SECTION("legal.validator=remote + legal.remote.base_url 为空") {
    TempDir cfg_dir("fss_c10_18_reject_legal");
    TempDir data_dir("fss_c10_18_reject_legal_data");
    const std::string config =
        ValidatorConfig(cfg_dir, "legal.json", FreePort(), data_dir, LegalBlock("remote", "", 3000),
                        SchemaBlock("noop", "", 3000));
    const ProcessOutcome outcome = RunServerForExit({"--config", config});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("拒绝启动") != std::string::npos);
    REQUIRE(outcome.output.find("legal.remote.base_url") != std::string::npos);
    //  拒绝启动必须**发生在进入服务状态之前**
    REQUIRE(outcome.output.find("已启动") == std::string::npos);
  }
  SECTION("schema.validator=remote + schema.remote.base_url 为空") {
    TempDir cfg_dir("fss_c10_18_reject_schema");
    TempDir data_dir("fss_c10_18_reject_schema_data");
    const std::string config =
        ValidatorConfig(cfg_dir, "schema.json", FreePort(), data_dir, LegalBlock("noop", "", 3000),
                        SchemaBlock("remote", "", 3000));
    const ProcessOutcome outcome = RunServerForExit({"--config", config});
    CAPTURE(outcome.exit_code, outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("拒绝启动") != std::string::npos);
    REQUIRE(outcome.output.find("schema.remote.base_url") != std::string::npos);
    REQUIRE(outcome.output.find("已启动") == std::string::npos);
  }
}

// =============================================================================
//  C10.18 ⑧：**依赖恢复**（mock 的 `--fail-file`）—— 不可用 → 503，删掉文件 →
//  同一个服务进程**立刻** 201。
//  ★ 这条同时证明两件事（都是真实性质，不是"跑通一次"）：
//    ① 依赖故障是 fail-closed 的（与 ③ 的 500 形态同族，但走的是"故障控制文件"通路）；
//    ② 校验结论**不缓存**：同一个进程、不重启、只删掉控制文件 → 下一次请求就通过。
//      若实现里加了缓存（"第一次失败就记住"或"成功一次就不再问"），这条会失败。
//  ★ 为什么必须用 `--fail-file` 而不是重启换配置：重启会掩盖"缓存"这类缺陷。
// =============================================================================
TEST_CASE("★ C10.18 ⑧ 依赖恢复：--fail-file → 503；删掉后同一进程立刻 201（不缓存结论）",
          "[phase10][integration][c10.18]") {
  TempDir cfg_dir("fss_c10_18_recover");
  TempDir data_dir("fss_c10_18_recover_data");
  //  ★ 控制文件一开始就存在 = 依赖不可用（mock 每次请求都查它的存在性）
  const std::string fail_file = WriteFile(data_dir, "dep_down", "down\n");

  MockValidators::Options mock_options;
  mock_options.mode = "legal";
  mock_options.valid = true;
  mock_options.fail_file = fail_file;
  MockValidators mock(mock_options);

  const int port = FreePort();
  REQUIRE(port > 0);
  const std::string config =
      ValidatorConfig(cfg_dir, "legal.json", port, data_dir,
                      LegalBlock("remote", mock.base_url(), 3000),
                      SchemaBlock("noop", "", 3000));
  ServerProcess server(FileDriven(config));
  CAPTURE(server.DumpLog());
  REQUIRE(WaitReady(port));

  //  ① 依赖不可用 → fail-closed（503），且没有记录被建出来
  const auto down = UploadAndRegister(port, "c10-18-recover-1", "recover1.txt");
  CAPTURE(down.create_status, down.create_body);
  REQUIRE(down.create_status == 503);
  RequireNoPersistentSideEffect(port);
  REQUIRE(mock.WaitRequests(1).requests >= 1);

  //  ② 删掉控制文件 = 依赖恢复；**同一个进程**、不重启、下一次请求必须成功
  std::error_code remove_ec;
  std::filesystem::remove(fail_file, remove_ec);
  REQUIRE_FALSE(remove_ec);
  REQUIRE_FALSE(std::filesystem::exists(fail_file));

  const auto up = UploadAndRegister(port, "c10-18-recover-2", "recover2.txt");
  CAPTURE(up.create_status, up.create_body);
  REQUIRE(up.create_status == 201);
  REQUIRE(mock.WaitRequests(2).requests >= 2);
}
