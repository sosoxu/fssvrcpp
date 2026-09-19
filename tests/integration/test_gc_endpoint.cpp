// =============================================================================
//  C9.31（P9 补交，P10 期间完成）：按需 GC 的 HTTP 端点 `POST {base}/v2/gc:run`
// =============================================================================
//  判据原文（docs/04-implementation-plan.md 的 C9.31；契约 §7.3 / ADR-013 §10）：
//    · `POST {base_path}/v2/gc:run`，授权 `service.file.admin`，**不需要** `data-partition-id`；
//      报告 = `GcReport` 的字段（snake_case）+ `partition` / `scheduled`；
//    · `?dryRun=true` **只能更保守**（有效值 = `配置 gc.dry_run || 请求 dryRun`）；
//    · 与周期调度共享单飞护栏：已在跑 → `kUnavailable` → **503**；
//    · `gc.enabled=false` 时端点**仍可用**（按需 GC 与周期调度是两件事），报告 `scheduled=false`；
//    · 会删数据的运维动作**必须审计**（`operation=gcRun`，成功/失败两侧）。
//
//  ★ 全部断言都在**真实 `build/bin/fss_server`** 上做（组合根 R12：测试自己装配的对象
//    替代不了"进程真的把 `GcTask` 绑到了路由上"）。
//  ★ "文件真的消失/仍在"的断言用**真实物理路径** `<storage.posix.root>/blobs/<container>/`
//    （phase10 §11.4.2 的教训：相对容器路径在测试进程 CWD 下恒假，`REQUIRE_FALSE` 会静默恒真）。
//    每条否定式判据旁边都有**正控**（同一条路径解析上断言"该存在的确实存在"）。
// =============================================================================
#include <catch2/catch.hpp>

#include "http_fixture.h"  // RawClient / Reply / HttpDo
#include "server_process.h"
#include "temp_dir.h"

#include "common/crypto/crypto.h"
#include "common/json/json.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using fss::test::HttpDo;
using fss::test::Reply;
using fss::test::ServerProcess;
using fss::test::ServerProcessOptions;
using fss::test::TempDir;

constexpr char kGcJwtSecret[] = "c9.31-gc-endpoint-secret";
constexpr char kAdminRole[] = "service.file.admin";
constexpr char kEditorRole[] = "service.file.editors";

//  让内核分配一个空闲端口（配置文件里必须写**具体**端口，否则并行 ctest 下互相踩）
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
    const auto reply = HttpDo(port, "GET", "/api/file/v2/readiness_check");
    if (reply.status == 200) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

//  ★ 关掉夹具默认注入的 env（`FSS_HTTP_PORT` / `FSS_STORAGE_ROOT` / `FSS_SQLITE_PATH`）：
//    env 的优先级高于配置文件，不关掉就测不到"配置文件里的 gc.* 真的生效"。
ServerProcessOptions SelfContainedOptions(const std::vector<std::string>& args) {
  ServerProcessOptions options;
  options.default_http_port = false;
  options.default_grpc_port = false;
  options.expect_grpc = false;
  options.default_storage_env = false;
  options.args = args;
  return options;
}

//  POSIX 驱动的 staging 容器物理目录 = `<storage.posix.root>/blobs/<container>`。
std::string StagingDir(const std::string& storage_root) {
  return storage_root + "/blobs/opendes-staging";
}
std::string PersistentDir(const std::string& storage_root) {
  return storage_root + "/blobs/opendes-persistent";
}

//  造一个"够旧的残留 .tmp.*"：用 `utime` 直接改 mtime（与 test_config_wiring.cpp 同源）。
std::string MakeOldTempFile(const std::string& storage_root, const std::string& key,
                            std::int64_t age_seconds) {
  const std::string dir = StagingDir(storage_root);
  std::filesystem::create_directories(dir);
  const std::string path = dir + "/" + key;
  {
    std::ofstream out(path);
    out << "partial-upload-bytes";
  }
  struct utimbuf times {};
  times.actime = ::time(nullptr) - static_cast<std::time_t>(age_seconds);
  times.modtime = times.actime;
  REQUIRE(::utime(path.c_str(), &times) == 0);
  return path;
}

//  HS256 签名一个 JWT（真实进程按 `auth.jwt.hmac_secret` 验签）。
//  `exp` 用**真实时钟**（进程用 SystemClock 判过期）。
std::string MintToken(const std::vector<std::string>& roles, const std::string& email) {
  const auto now = static_cast<std::int64_t>(::time(nullptr));
  fss::json::Value header;
  header["alg"] = "HS256";
  fss::json::Value payload;
  payload["sub"] = "user-1";
  payload["email"] = email;
  payload["data-partition-id"] = "opendes";
  payload["roles"] = roles;
  payload["nbf"] = now - 10;
  payload["exp"] = now + 3600;
  const std::string header_b64 = fss::crypto::Base64UrlEncode(fss::json::Dump(header));
  const std::string payload_b64 = fss::crypto::Base64UrlEncode(fss::json::Dump(payload));
  const std::string signing_input = header_b64 + "." + payload_b64;
  const auto digest = fss::crypto::HmacSha256(kGcJwtSecret, signing_input);
  return signing_input + "." + fss::crypto::Base64UrlEncode(digest);
}

//  每用例一份独立的数据根 + 配置文件（jwt 模式：`service.file.admin` 才有权调端点）
struct GcServerConfig {
  std::string root;
  std::string config_path;
};

GcServerConfig WriteGcConfig(const TempDir& cfg_dir, const TempDir& data_dir, const std::string& name,
                             int port, bool gc_enabled, bool dry_run) {
  GcServerConfig out;
  out.root = data_dir.child(name + "-store");
  out.config_path = WriteFile(
      cfg_dir, name + ".json",
      std::string("{\n  \"server\": {\"http\": {\"bind\": \"127.0.0.1\", \"port\": ") +
          std::to_string(port) +
          "}, \"grpc\": {\"enabled\": false}},\n"
          "  \"storage\": {\"posix\": {\"root\": \"" + out.root + "\"}},\n"
          "  \"location\": {\"sqlite\": {\"path\": \"" + data_dir.child(name + "-loc.db") + "\"}},\n"
          "  \"metadata\": {\"sqlite\": {\"path\": \"" + data_dir.child(name + "-meta.db") + "\"}},\n"
          "  \"self_signed\": {\"signing_key\": \"gc-ep\"},\n"
          "  \"auth\": {\"mode\": \"jwt\", \"jwt\": {\"hmac_secret\": \"" + kGcJwtSecret +
          "\", \"verify_signature\": true}},\n"
          "  \"gc\": {\"enabled\": " + (gc_enabled ? "true" : "false") +
          ", \"dry_run\": " + (dry_run ? "true" : "false") +
          ", \"require_lease_expiry\": true, \"staging_ttl_hours\": 24}\n}\n");
  return out;
}

//  POST `gc:run`（带 admin token，**不带** `data-partition-id` —— 契约 §7.3）
Reply PostGcRun(int port, const std::string& token, const std::string& query = "") {
  std::vector<std::string> headers;
  if (!token.empty()) headers.push_back("authorization: Bearer " + token);
  return HttpDo(port, "POST", "/api/file/v2/gc:run" + query, headers, "");
}

std::int64_t IntField(const fss::json::Value& body, const char* key) {
  const auto it = body.find(key);
  REQUIRE(it != body.end());
  REQUIRE(it->is_number_integer());
  return it->get<std::int64_t>();
}

}  // namespace

// =============================================================================
//  ① 正例（R16）：配置 gc.dry_run=false → 端点在**真实进程**上真的删掉够旧的 .tmp.*
// =============================================================================
TEST_CASE("★ C9.31：按需 GC 端点真的删数据（gc.dry_run=false；够旧 .tmp.* 消失、太新的仍在）",
          "[phase9][integration][c9.31]") {
  TempDir cfg_dir("c931_cfg_pos");
  TempDir data_dir("c931_data_pos");
  const int port = FreePort();
  REQUIRE(port > 0);
  //  ★ `gc.enabled=false`：证明按需 GC 与周期调度是**两件事**（磁盘满时运维要能立刻扫一轮）
  const auto cfg = WriteGcConfig(cfg_dir, data_dir, "pos", port, /*gc_enabled=*/false,
                                 /*dry_run=*/false);

  const std::string residue = MakeOldTempFile(cfg.root, "residue.bin.tmp.local.9.1", 3 * 24 * 3600);
  const std::string fresh = MakeOldTempFile(cfg.root, "inflight.bin.tmp.local.9.2", 5);
  //  ★ 正控（phase10 §11.4.2）：同一条路径解析上，断言"该存在的确实存在" ——
  //    否则 `REQUIRE_FALSE(exists(residue))` 可能是"路径写错"造成的恒真。
  REQUIRE(std::filesystem::exists(residue));
  REQUIRE(std::filesystem::exists(fresh));

  ServerProcess server(SelfContainedOptions({"--config", cfg.config_path}));
  REQUIRE(server.http_port() == port);
  REQUIRE(WaitReady(port));
  //  横幅必须说明"调度未启动"（但不影响端点）
  REQUIRE(server.DumpLog().find("gc             : 未启动（gc.enabled=false") != std::string::npos);
  REQUIRE(server.DumpLog().find("/v2/gc:run") != std::string::npos);

  const auto reply = PostGcRun(port, MintToken({kAdminRole}, "gc-admin@example.com"));
  CAPTURE(reply.status, reply.body);
  REQUIRE(reply.status == 200);
  const auto parsed = fss::json::ParseObject(reply.body);
  REQUIRE(parsed.ok());
  const auto& body = parsed.value();
  REQUIRE(body["dry_run"] == false);                 // 有效值：这一轮**真的删**
  REQUIRE(body["partition"] == "opendes");           // 运行态字段
  REQUIRE(body["scheduled"] == false);               // gc.enabled=false → 调度没跑
  REQUIRE(IntField(body, "tmp_removed") >= 1);       // 报告里候选/删除数真的动了
  REQUIRE(IntField(body, "tmp_skipped_too_young") >= 1);  // 太新的被保护（正控之一）
  REQUIRE(IntField(body, "errors") == 0);

  //  ★ 真正要证明的东西：**物理路径**上的文件消失（不是"响应里数字好看"）
  REQUIRE_FALSE(std::filesystem::exists(residue));
  //  ★ 正向对照：太新的 `.tmp.*` 仍在（TTL 判据，绝不是"见到 tmp 就删"）
  REQUIRE(std::filesystem::exists(fresh));
}

// =============================================================================
//  ② 干跑安全：`?dryRun=true` 只能**更保守**，绝不能把配置的 dry-run 翻成真删
// =============================================================================
TEST_CASE("★ C9.31：dry-run 只能更保守（配置 true → 文件仍在；配置 false + ?dryRun=true → 仍在）",
          "[phase9][integration][c9.31]") {
  SECTION("配置 gc.dry_run=true：端点仍 200，报告 dry_run=true，文件**仍在**") {
    TempDir cfg_dir("c931_cfg_dry");
    TempDir data_dir("c931_data_dry");
    const int port = FreePort();
    REQUIRE(port > 0);
    const auto cfg = WriteGcConfig(cfg_dir, data_dir, "dry", port, /*gc_enabled=*/false,
                                   /*dry_run=*/true);
    const std::string residue = MakeOldTempFile(cfg.root, "residue.bin.tmp.local.9.3", 3 * 24 * 3600);
    REQUIRE(std::filesystem::exists(residue));  // 正控

    ServerProcess server(SelfContainedOptions({"--config", cfg.config_path}));
    REQUIRE(WaitReady(port));

    const auto reply = PostGcRun(port, MintToken({kAdminRole}, "gc-admin@example.com"));
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 200);
    const auto parsed = fss::json::ParseObject(reply.body);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value()["dry_run"] == true);
    REQUIRE(IntField(parsed.value(), "tmp_removed") >= 1);  // 报候选（但不删）
    //  ★ 目标判据（有正控在同一条路径上）
    REQUIRE(std::filesystem::exists(residue));
  }

  SECTION("配置 gc.dry_run=false + `?dryRun=true`：有效 dry_run=true，文件**仍在**") {
    TempDir cfg_dir("c931_cfg_force");
    TempDir data_dir("c931_data_force");
    const int port = FreePort();
    REQUIRE(port > 0);
    const auto cfg = WriteGcConfig(cfg_dir, data_dir, "force", port, /*gc_enabled=*/false,
                                   /*dry_run=*/false);
    const std::string residue = MakeOldTempFile(cfg.root, "residue.bin.tmp.local.9.4", 3 * 24 * 3600);
    REQUIRE(std::filesystem::exists(residue));  // 正控

    ServerProcess server(SelfContainedOptions({"--config", cfg.config_path}));
    REQUIRE(WaitReady(port));

    //  ① 不带查询参数：配置要真删 → 文件消失（证明这个实例**确实**是 dry_run=false）
    const auto real = PostGcRun(port, MintToken({kAdminRole}, "gc-admin@example.com"));
    CAPTURE(real.status, real.body);
    REQUIRE(real.status == 200);
    const auto real_json = fss::json::ParseObject(real.body);
    REQUIRE(real_json.ok());
    REQUIRE(real_json.value()["dry_run"] == false);
    REQUIRE_FALSE(std::filesystem::exists(residue));

    //  ② 重新播种，带 `?dryRun=true`：有效值必须变成 true，文件**仍在**
    const std::string residue2 =
        MakeOldTempFile(cfg.root, "residue2.bin.tmp.local.9.5", 3 * 24 * 3600);
    REQUIRE(std::filesystem::exists(residue2));  // 正控
    const auto forced = PostGcRun(port, MintToken({kAdminRole}, "gc-admin@example.com"),
                                  "?dryRun=true");
    CAPTURE(forced.status, forced.body);
    REQUIRE(forced.status == 200);
    const auto forced_json = fss::json::ParseObject(forced.body);
    REQUIRE(forced_json.ok());
    REQUIRE(forced_json.value()["dry_run"] == true);       // 有效值 = 配置 || 请求
    REQUIRE(IntField(forced_json.value(), "tmp_removed") >= 1);  // 候选被报告
    REQUIRE(std::filesystem::exists(residue2));            // ★ 真正没删
  }
}

// =============================================================================
//  ③ 授权矩阵：无 token 401 / 非 admin 403 / admin 200（且**不需要** data-partition-id）
// =============================================================================
TEST_CASE("★ C9.31：授权矩阵（401 / 403 / 200；admin 不带 data-partition-id 也 200）",
          "[phase9][integration][c9.31]") {
  TempDir cfg_dir("c931_cfg_auth");
  TempDir data_dir("c931_data_auth");
  const int port = FreePort();
  REQUIRE(port > 0);
  const auto cfg = WriteGcConfig(cfg_dir, data_dir, "auth", port, /*gc_enabled=*/false,
                                 /*dry_run=*/true);
  ServerProcess server(SelfContainedOptions({"--config", cfg.config_path}));
  REQUIRE(WaitReady(port));

  //  ① 无 token → 401（缺凭证）
  {
    const auto reply = PostGcRun(port, "");
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 401);
  }
  //  ② 有 token 但只有 editors 角色 → 403（不是 400：未授权者不得靠差异探测）
  {
    const auto reply = PostGcRun(port, MintToken({kEditorRole}, "editor@example.com"));
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 403);
  }
  //  ③ admin → 200；★ 而且**不带** `data-partition-id` 也必须 200（契约 §7.3）
  {
    const auto reply = PostGcRun(port, MintToken({kAdminRole}, "admin@example.com"));
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 200);
  }
  //  ④ 反向对照（R16）：**没有 token** 时不能靠"带上 partition"绕过 → 仍然 401
  {
    const auto reply = HttpDo(port, "POST", "/api/file/v2/gc:run",
                              {"data-partition-id: opendes"}, "");
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 401);
  }
}

// =============================================================================
//  ④ 审计：会删数据的运维动作，成功与失败两侧都要留痕（operation=gcRun）
// =============================================================================
TEST_CASE("★ C9.31：审计两侧都留痕（operation=gcRun + actor + result=success/failure）",
          "[phase9][integration][c9.31]") {
  TempDir cfg_dir("c931_cfg_audit");
  TempDir data_dir("c931_data_audit");
  const int port = FreePort();
  REQUIRE(port > 0);
  const auto cfg = WriteGcConfig(cfg_dir, data_dir, "audit", port, /*gc_enabled=*/false,
                                 /*dry_run=*/true);
  ServerProcess server(SelfContainedOptions({"--config", cfg.config_path}));
  REQUIRE(WaitReady(port));

  const auto token = MintToken({kAdminRole}, "gc-admin@example.com");
  //  ① 成功侧：`x-user-id` 是 actor 的来源（`app::CallerFromHeaders`）
  std::vector<std::string> headers = {"authorization: Bearer " + token,
                                      "x-user-id: gc-operator@example.com"};
  const auto ok_reply = HttpDo(port, "POST", "/api/file/v2/gc:run", headers, "");
  CAPTURE(ok_reply.status, ok_reply.body);
  REQUIRE(ok_reply.status == 200);
  {
    const std::string log = server.DumpLog();
    CAPTURE(log);
    REQUIRE(log.find("\"operation\":\"gcRun\"") != std::string::npos);
    REQUIRE(log.find("\"user\":\"gc-operator@example.com\"") != std::string::npos);
    REQUIRE(log.find("\"result\":\"success\"") != std::string::npos);
  }

  //  ② 失败侧：把 persistent 容器目录删掉 → `GcTask` 的孤儿扫描拿不到容器（errors>0），
  //     审计必须记 result=failure（"这一轮没能正常跑完"）。
  REQUIRE(std::filesystem::exists(PersistentDir(cfg.root)));  // 正控：启动时确实建了容器
  std::error_code ec;
  std::filesystem::remove_all(PersistentDir(cfg.root), ec);
  REQUIRE_FALSE(std::filesystem::exists(PersistentDir(cfg.root)));  // 前置条件显式断言（R9）

  const auto fail_reply = HttpDo(port, "POST", "/api/file/v2/gc:run", headers, "");
  CAPTURE(fail_reply.status, fail_reply.body);
  REQUIRE(fail_reply.status == 200);  // 报告本身带着 errors（见契约 §7.3）
  const auto fail_json = fss::json::ParseObject(fail_reply.body);
  REQUIRE(fail_json.ok());
  REQUIRE(IntField(fail_json.value(), "errors") >= 1);
  {
    const std::string log = server.DumpLog();
    CAPTURE(log);
    REQUIRE(log.find("\"result\":\"failure\"") != std::string::npos);
    REQUIRE(log.find("\"user\":\"gc-operator@example.com\"") != std::string::npos);
  }
}

// =============================================================================
//  ⑤ 与周期调度无关：`gc.enabled` 只影响 `scheduled` 字段，端点两种情形都可用
// =============================================================================
TEST_CASE("★ C9.31：gc.enabled=false → scheduled=false；gc.enabled=true → scheduled=true",
          "[phase9][integration][c9.31]") {
  SECTION("gc.enabled=false：端点 200 + scheduled=false") {
    TempDir cfg_dir("c931_cfg_off");
    TempDir data_dir("c931_data_off");
    const int port = FreePort();
    REQUIRE(port > 0);
    const auto cfg = WriteGcConfig(cfg_dir, data_dir, "off", port, /*gc_enabled=*/false,
                                   /*dry_run=*/true);
    ServerProcess server(SelfContainedOptions({"--config", cfg.config_path}));
    REQUIRE(WaitReady(port));
    const auto reply = PostGcRun(port, MintToken({kAdminRole}, "admin@example.com"));
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 200);
    const auto parsed = fss::json::ParseObject(reply.body);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value()["scheduled"] == false);
  }

  SECTION("gc.enabled=true：端点 200 + scheduled=true") {
    TempDir cfg_dir("c931_cfg_on");
    TempDir data_dir("c931_data_on");
    const int port = FreePort();
    REQUIRE(port > 0);
    const auto cfg = WriteGcConfig(cfg_dir, data_dir, "on", port, /*gc_enabled=*/true,
                                   /*dry_run=*/true);
    ServerProcess server(SelfContainedOptions({"--config", cfg.config_path}));
    REQUIRE(WaitReady(port));
    REQUIRE(server.DumpLog().find("gc             : 已启动（间隔") != std::string::npos);
    const auto reply = PostGcRun(port, MintToken({kAdminRole}, "admin@example.com"));
    CAPTURE(reply.status, reply.body);
    REQUIRE(reply.status == 200);
    const auto parsed = fss::json::ParseObject(reply.body);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value()["scheduled"] == true);
  }
}

// =============================================================================
//  ⑥ 指标：端点跑完一轮后 `/metrics` 的 `fss_gc_runs_total` 真的涨了
// =============================================================================
TEST_CASE("★ C9.31：端点调用后 /metrics 的 fss_gc_runs_total 真的涨了",
          "[phase9][integration][c9.31]") {
  TempDir cfg_dir("c931_cfg_metrics");
  TempDir data_dir("c931_data_metrics");
  const int port = FreePort();
  REQUIRE(port > 0);
  //  ★ `gc.enabled=false` → 周期调度不跑 → 计数只能来自**端点**（判据有区分力：
  //    "指标涨了"不可能被调度器顺手跑出来）
  const auto cfg = WriteGcConfig(cfg_dir, data_dir, "metrics", port, /*gc_enabled=*/false,
                                 /*dry_run=*/true);
  ServerProcess server(SelfContainedOptions({"--config", cfg.config_path}));
  REQUIRE(WaitReady(port));

  const auto before = HttpDo(port, "GET", "/metrics");
  CAPTURE(before.body);
  REQUIRE(before.status == 200);
  REQUIRE(before.body.find("fss_gc_runs_total") == std::string::npos);  // 还没人跑过

  const auto reply = PostGcRun(port, MintToken({kAdminRole}, "admin@example.com"));
  CAPTURE(reply.status, reply.body);
  REQUIRE(reply.status == 200);

  //  轮询实际条件（不用固定 sleep）：`fss_gc_runs_total{...}` 以非 0 值出现
  bool counted = false;
  std::string last_metrics;
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto metrics = HttpDo(port, "GET", "/metrics");
    if (metrics.status == 200) {
      last_metrics = metrics.body;
      const auto pos = last_metrics.find("fss_gc_runs_total{");
      if (pos != std::string::npos) {
        //  形如 `fss_gc_runs_total{mode="dry_run",outcome="ok"} 1`
        const auto value_pos = last_metrics.find(' ', pos);
        if (value_pos != std::string::npos && last_metrics[value_pos + 1] != '0') counted = true;
      }
    }
    if (counted) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  CAPTURE(last_metrics, counted);
  REQUIRE(counted);
}
