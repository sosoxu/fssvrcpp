// =============================================================================
//  C9.30（第三半）：`/v2/info` 与指标暴露 `ioEngine` / `ioUringAvailable`
// =============================================================================
//  C9.30 原文：「`/v2/info` 与指标正确暴露 `ioEngine` / `ioUringAvailable`；
//  在**不允许 io_uring 的部署**里所有 OSDU 端点行为不变」。
//  本文件收口的是**第三半**（前两半：①不允许 io_uring 的部署里端点行为不变
//  ② `fss_io_engine{engine,requested}` 指标 —— 已实测，见 `phase9-image.md` §10）。
//
//  ---- 语义（父代理定案，勿改写）----
//    `ioEngine`          = **当前生效**的引擎名（组合根的 `io_engine_active`）。
//                          本实现下恒为 "blocking"（ADR-010 的 U1~U4 未满足）。
//    `ioUringAvailable`  = 本部署的**宿主能力探测结果**（`sys::IoEngineProbe::available()`）。
//                          ★ **可用 ≠ 已启用**：`true` 只表示"这台机器/这个 seccomp 下
//                            `io_uring_setup` 能用"，**不**代表服务正在用 uring。
//    两条协议（REST `/v2/info` 与 gRPC `GetInfo`）**同源**（`app::GetInfo` 的返回结构
//    `VersionInfo` ⇒ C7.3 的等价性按构造保证），不在适配器里各算一遍。
//
//  ---- 本机真值（实测，别假设）----
//    本工作机 = WSL2（kernel 6.18.33.2-microsoft-standard-WSL2，`kernel.io_uring_disabled=0`）
//    ⇒ 宿主机 `io_uring_setup` **成功**（`check_io_uring.sh` + `python3` 直接 syscall 双向确认）
//    ⇒ 真实进程 `ioUringAvailable == true`。
//    而 **Docker 默认 seccomp** 下同一调用是 `EPERM`（本文件之外实测，见阶段 9 证据）⇒
//    "不允许 io_uring 的部署"真值确实是 false。
//    ⚠️ 因此**不能**断言"不注入就必须 false"：那会把正确实现判失败（R4：无区分力的判据
//       不得当证据）。本文件用**探测注入接缝**把两个方向都钉住。
//
//  ---- 探测注入接缝（环境变量，**不是配置键**，见 runbook 的测试/演练小节）----
//    `FSS_IO_PROBE_INJECT=available` → 探测结果强制为可用；
//    `FSS_IO_PROBE_INJECT=blocked`   → 探测结果强制为 blocked_by_policy(EPERM)。
//    它**只**改探测结果：`UringIoEngine::enabled()` 仍 false ⇒ `ioEngine` 仍 blocking、
//    `storage.io_engine=uring` 仍**拒绝启动**（这两条下面都断言）。
//
//  ---- 每条判据都能因注入而失败（§7 的 R1 自证把三个注入点跑过）----
//    ① REST 渲染里硬编码 `ioUringAvailable=false` → 「接缝使其变 true」必失败；
//    ② gRPC 映射写死成与 REST 不同的值（或不映射）→ 「双协议同源」必失败；
//    ③ 去掉 `fss_io_uring_available` 指标 → 「指标存在且随探测变化」必失败。// =============================================================================
#include <catch2/catch.hpp>

#include "grpc_fixture.h"
#include "http_fixture.h"
#include "raw_http.h"
#include "server_process.h"

#include <grpcpp/grpcpp.h>
#include <google/protobuf/util/json_util.h>
#include <osdu/file/v1/file_service.grpc.pb.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using fss::test::DualProtocolFixture;
using fss::test::HttpDo;
using fss::test::RawClient;
using fss::test::Reply;
using fss::test::RunServerForExit;
using fss::test::ServerProcess;
using fss::test::ServerProcessOptions;
using fss::test::TempDir;

struct HttpResponse {
  bool transport_ok = false;
  int status = 0;
  std::string body;
};

//  直接用原始 socket 读（与其它真实进程用例同一套客户端；`RawClient` 不接受畸形请求
//  之外的任何"便利"）。
HttpResponse HttpGet(int port, const std::string& target) {
  HttpResponse reply;
  RawClient client(port, /*tcp_nodelay=*/true);
  if (!client.Connect()) return reply;
  if (!client.SendRequest("GET", target, {}, "")) return reply;
  const auto response = client.ReadResponse(30000);
  if (!response.has_value()) return reply;
  reply.transport_ok = true;
  reply.status = response->status;
  reply.body = response->body;
  return reply;
}

bool WaitReady(int port, int attempts = 200) {
  for (int i = 0; i < attempts; ++i) {
    const auto reply = HttpGet(port, "/api/file/v2/readiness_check");
    if (reply.transport_ok && reply.status == 200) return true;
    ::usleep(25 * 1000);
  }
  return false;
}

// -----------------------------------------------------------------------------
//  用 python3 的 `json` 模块**真解析**（不是子串匹配）：字段**存在** + **类型** + 取值。
//  判据写成"解析结果必须是 `<期望的 python 字面量>`"，解析失败（= 不是合法 JSON 或
//  缺字段）会让 python 抛异常 → 输出与期望值不相等 → 断言失败。
//  ★ 传 JSON 用临时文件而不是 argv：`/v2/info` 的 JSON 里含引号，argv 转义容易出错。
// -----------------------------------------------------------------------------
std::string RunCapture(const std::string& command) {
  std::string out;
  std::FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) return out;
  char buffer[512] = {0};
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) out += buffer;
  ::pclose(pipe);
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

struct JsonFields {
  bool parsed = false;  // python 成功解析
  std::map<std::string, std::string> types;
  std::map<std::string, std::string> values;

  //  ★ 只读取值器（不要用 `map::operator[]`：它**插入**默认值，在 const map 上
  //    无法编译，而且会让 Catch2 的表达式分解在断言里产生副作用）。
  std::string Value(const std::string& key) const {
    const auto it = values.find(key);
    return it == values.end() ? std::string("MISSING") : it->second;
  }
  std::string Type(const std::string& key) const {
    const auto it = types.find(key);
    return it == types.end() ? std::string("MISSING") : it->second;
  }
};

JsonFields ParseJsonFields(const std::string& body, const std::vector<std::string>& keys) {
  JsonFields out;
  char path[] = "/tmp/fss_info_json_XXXXXX";
  const int fd = ::mkstemp(path);
  if (fd < 0) return out;
  {
    std::ofstream file(path, std::ios::trunc | std::ios::binary);
    file << body;
  }
  ::close(fd);

  std::string script = "import json,sys\n";
  script += "try:\n";
  script += "  d = json.load(open(sys.argv[1]))\n";
  script += "except Exception:\n";
  script += "  print('PARSE_ERROR'); raise SystemExit(0)\n";
  script += "print('PARSED')\n";
  script += "ks = [";
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i != 0) script += ",";
    script += "'" + keys[i] + "'";
  }
  script += "]\n";
  script += "for k in ks:\n";
  script += "  if k not in d:\n";
  script += "    print(k + '.type=MISSING')\n";
  script += "    print(k + '.value=MISSING')\n";
  script += "    continue\n";
  script += "  print(k + '.type=' + type(d[k]).__name__)\n";
  script += "  print(k + '.value=' + repr(d[k]))\n";

  const std::string output = RunCapture("python3 -c \"" + script + "\" \"" + path + "\"");
  ::unlink(path);
  if (output.empty()) return out;

  std::size_t start = 0;
  while (start <= output.size()) {
    const auto end = output.find('\n', start);
    const std::string line =
        output.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (line == "PARSED") {
      out.parsed = true;
    } else if (line.rfind("PARSE_ERROR", 0) == 0) {
      return out;  // parsed = false
    } else {
      const auto dot = line.find('.');
      const auto eq = line.find('=', dot == std::string::npos ? 0 : dot);
      if (dot != std::string::npos && eq != std::string::npos) {
        const std::string key = line.substr(0, dot);
        const std::string what = line.substr(dot + 1, eq - dot - 1);
        const std::string value = line.substr(eq + 1);
        if (what == "type") out.types[key] = value;
        if (what == "value") out.values[key] = value;
      }
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

//  `/metrics` 是 Prometheus 文本格式（逐行），因此子串断言是**格式正确**的判据；
//  但"指标存在"必须与"值随探测变化"一起断言，否则只证明了名字出现。
bool MetricsHas(const std::string& body, const std::string& line) {
  return body.find(line) != std::string::npos;
}

//  从启动横幅里取 `io engine      : <值>（请求 <requested>）` 的 `<值>`。
//  ★ 这是"三处渲染同源"判据的**竖切解析**（横幅是纯文本，没有结构化出口）。
std::string BannerIoEngine(const std::string& log) {
  const std::string marker = "  io engine      : ";
  const auto pos = log.find(marker);
  if (pos == std::string::npos) return {};
  const auto begin = pos + marker.size();
  const auto end = log.find_first_of("（( \n", begin);
  if (end == std::string::npos) return log.substr(begin);
  return log.substr(begin, end - begin);
}

const char* const kInfoPath = "/api/file/v2/info";

}  // namespace

// =============================================================================
//  ① REST 正例：字段**存在**、类型正确、取值与宿主探测一致；既有字段未破坏
// =============================================================================
TEST_CASE("★ C9.30 REST /v2/info：ioEngine/ioUringAvailable 存在且类型正确（JSON 真解析）",
          "[phase9][c9.30][integration]") {
  ServerProcessOptions options;
  options.expect_grpc = false;
  options.default_grpc_port = false;
  options.env.push_back({"FSS_HTTP_PORT", "0"});
  ServerProcess server(options);
  REQUIRE(WaitReady(server.http_port()));

  const auto reply = HttpGet(server.http_port(), kInfoPath);
  INFO("REST /v2/info → " << reply.status << " " << reply.body);
  CAPTURE(server.DumpLog());
  REQUIRE(reply.transport_ok);
  REQUIRE(reply.status == 200);

  //  ★ 真 JSON 解析（不是 `find` 子串）：`json.load` 成功 + 字段存在 + 类型正确。
  const auto fields = ParseJsonFields(
      reply.body, {"version", "buildVersion", "connectedOuterServices", "authMode", "ioEngine",
                   "ioUringAvailable"});
  INFO("python3 json 解析：" << (fields.parsed ? "OK" : "PARSE_ERROR"));
  REQUIRE(fields.parsed);

  //  ①a 两个新字段：存在 + 类型（`str` / `bool`）
  REQUIRE(fields.Type("ioEngine") == "str");
  REQUIRE(fields.Type("ioUringAvailable") == "bool");

  //  ①b `ioEngine` = **当前生效**引擎：本实现恒为 blocking（ADR-010 U1~U4 未满足）
  REQUIRE(fields.Value("ioEngine") == "'blocking'");

  //  ①c `ioUringAvailable` = **宿主能力探测**结果 —— 必须与本机探测一致，**不写死**。
  //     本机（WSL2，io_uring_disabled=0）真值为 true；默认 seccomp 容器里为 false。
  //     用 `check_io_uring.sh` 同源的 syscall 探测做**独立参照实现**（R18 的精神：
  //     手写判定要有参照实现比对），两侧必须一致。
  const std::string probe = RunCapture(
      "python3 -c \"import ctypes,os;l=ctypes.CDLL('libc.so.6',use_errno=True);"
      "b=ctypes.create_string_buffer(1024);"
      "r=l.syscall(ctypes.c_long(425),ctypes.c_uint(8),ctypes.byref(b));"
      "print('AVAILABLE' if r>=0 else 'BLOCKED')\"");
  INFO("独立 syscall 探测（io_uring_setup）：" << probe);
  REQUIRE((probe == "AVAILABLE" || probe == "BLOCKED"));  // 前置条件显式断言（R9）
  const std::string expected = probe == "AVAILABLE" ? "True" : "False";
  REQUIRE(fields.Value("ioUringAvailable") == expected);

  //  ①d 既有字段未被破坏（**正控**：证明上面"没有某字段"的断言不是因为解析失败）
  REQUIRE(fields.Type("version") == "str");
  REQUIRE(fields.Value("version") == "'v2'");
  REQUIRE(fields.Type("buildVersion") == "str");
  REQUIRE(fields.Type("connectedOuterServices") == "list");
  REQUIRE(fields.Value("connectedOuterServices") == "['storage']");
  //  authMode 只在非空时渲染（默认 disabled 非空 ⇒ 应在）
  REQUIRE(fields.Type("authMode") == "str");
  REQUIRE(fields.Value("authMode") == "'disabled'");

  //  ①e 横幅里宿主能力也可见（R11 的另一处渲染），且与探测结果一致
  const std::string banner = server.DumpLog();
  REQUIRE(banner.find("io_uring=") != std::string::npos);
  if (probe == "AVAILABLE") {
    REQUIRE(banner.find("io_uring=available") != std::string::npos);
  } else {
    REQUIRE(banner.find("io_uring=available") == std::string::npos);
  }
}

// =============================================================================
//  ② gRPC 等价：与 REST **同源**（同一次运行、同一进程、各取一次后比对）
// =============================================================================
//  ★ 两种装配都做：
//    (a) 真实二进制进程：REST `/v2/info` 与 gRPC `GetInfo` 各一次 → 同值；
//    (b) 进程内 `DualProtocolFixture`：**把端口集合设成非默认值**（auth_mode=jwt、
//        io_uring_available=true），断言**两条协议都跟着变** —— 这条能抓住
//        "gRPC 映射写死/漏映射"（仅靠真实进程的默认值抓不住，因为默认值恰好也是
//        blocking/当前探测值）。
TEST_CASE("★ C9.30 gRPC GetInfo 与 REST /v2/info 同源（真实进程 + 进程内非默认值）",
          "[phase9][c9.30][integration]") {
  SECTION("(a) 真实二进制：两条协议各取一次，字段逐一相等") {
    ServerProcessOptions options;  // 默认开两个端口
    options.env.push_back({"FSS_GRPC_PORT", "-1"});
    ServerProcess server(options);
    REQUIRE(WaitReady(server.http_port()));
    REQUIRE(server.grpc_port() != 0);

    const auto rest = HttpGet(server.http_port(), kInfoPath);
    CAPTURE(server.DumpLog(), rest.body);
    REQUIRE(rest.transport_ok);
    REQUIRE(rest.status == 200);
    const auto fields = ParseJsonFields(rest.body, {"ioEngine", "ioUringAvailable", "authMode"});
    REQUIRE(fields.parsed);

    auto channel = ::grpc::CreateChannel("127.0.0.1:" + std::to_string(server.grpc_port()),
                                         ::grpc::InsecureChannelCredentials());
    auto stub = osdu::file::v1::FileService::NewStub(channel);
    REQUIRE(stub != nullptr);
    ::grpc::ClientContext context;
    ::google::protobuf::Empty request;
    osdu::file::v1::InfoResponse response;
    const auto status = stub->GetInfo(&context, request, &response);
    INFO("gRPC GetInfo → " << status.error_code() << " " << status.error_message());
    REQUIRE(status.ok());

    //  同源 ⇒ 逐字段相等（同一进程内，不存在"两次运行值不同"的干扰）
    REQUIRE(response.version() == "v2");
    REQUIRE(response.auth_mode() == fields.Value("authMode").substr(
                                       1, fields.Value("authMode").size() - 2));
    REQUIRE(response.io_engine() ==
            fields.Value("ioEngine").substr(1, fields.Value("ioEngine").size() - 2));
    REQUIRE(response.io_uring_available() == (fields.Value("ioUringAvailable") == "True"));
    //  gRPC 侧也必须真的是"当前生效 blocking"（不是 proto 默认空串/别的值）
    REQUIRE(response.io_engine() == "blocking");
  }

  SECTION("(b) 进程内同一份 ports：两条协议都跟着端口集合的非默认值走") {
    DualProtocolFixture fx;
    //  非默认值：若任一侧硬编码，下面必失败（探测注入抓不到"gRPC 写了别的常量"）
    fx.http.ports->auth_mode = "jwt";
    fx.http.ports->io_engine = "blocking";
    fx.http.ports->io_uring_available = true;

    const Reply rest = HttpDo(fx.http_port(), "GET", kInfoPath);
    INFO("REST： " << rest.body);
    REQUIRE(rest.status == 200);
    const auto fields = ParseJsonFields(rest.body, {"authMode", "ioEngine", "ioUringAvailable"});
    REQUIRE(fields.parsed);
    REQUIRE(fields.Value("authMode") == "'jwt'");
    REQUIRE(fields.Value("ioEngine") == "'blocking'");
    REQUIRE(fields.Value("ioUringAvailable") == "True");

    ::grpc::ClientContext context;
    ::google::protobuf::Empty request;
    osdu::file::v1::InfoResponse response;
    const auto status = fx.stub->GetInfo(&context, request, &response);
    REQUIRE(status.ok());
    REQUIRE(response.auth_mode() == "jwt");
    REQUIRE(response.io_engine() == "blocking");
    REQUIRE(response.io_uring_available());  // ← gRPC 真的读了端口集合（不是硬编码 false）

    //  ★ 反向对照（R1/R16 的精神）：把端口集合改成 false，gRPC 必须跟着变 ——
    //    证明上面那条不是"恒 true"。
    fx.http.ports->io_uring_available = false;
    ::grpc::ClientContext context2;
    osdu::file::v1::InfoResponse response2;
    REQUIRE(fx.stub->GetInfo(&context2, request, &response2).ok());
    REQUIRE_FALSE(response2.io_uring_available());
    //  而 REST 也同步变 false（同一份 ports）
    const Reply rest2 = HttpDo(fx.http_port(), "GET", kInfoPath);
    const auto fields2 = ParseJsonFields(rest2.body, {"ioUringAvailable"});
    REQUIRE(fields2.parsed);
    REQUIRE(fields2.Value("ioUringAvailable") == "False");
  }
}

// =============================================================================
//  ③ 指标：`fss_io_uring_available` 存在、是 gauge、值随探测变化；既有 `fss_io_engine` 保留
// =============================================================================
TEST_CASE("★ C9.30 /metrics：fss_io_uring_available 与 fss_io_engine 同时暴露",
          "[phase9][c9.30][integration]") {
  ServerProcessOptions options;
  options.expect_grpc = false;
  options.default_grpc_port = false;
  ServerProcess server(options);
  REQUIRE(WaitReady(server.http_port()));

  const auto metrics = HttpGet(server.http_port(), "/metrics");
  CAPTURE(server.DumpLog());
  INFO("metrics 片段：\n"
       << metrics.body.substr(0, 2000));
  REQUIRE(metrics.transport_ok);
  REQUIRE(metrics.status == 200);

  //  ③a 新指标：HELP/TYPE 齐（Prometheus 抓取器要求）+ 无标签的 0/1 gauge
  REQUIRE(metrics.body.find("# TYPE fss_io_uring_available gauge") != std::string::npos);
  REQUIRE(metrics.body.find("# HELP fss_io_uring_available ") != std::string::npos);

  //  ③b 既有指标**保留**（C9.30 的第二半不能被这一半改坏）
  REQUIRE(MetricsHas(metrics.body, "fss_io_engine{engine=\"blocking\",requested=\"blocking\"} 1"));

  //  ③c 值与宿主探测一致（不写死）：与 /v2/info 同一份事实
  const auto info = HttpGet(server.http_port(), kInfoPath);
  const auto fields = ParseJsonFields(info.body, {"ioUringAvailable"});
  REQUIRE(fields.parsed);
  const bool available = fields.Value("ioUringAvailable") == "True";
  const std::string expected_line =
      std::string("fss_io_uring_available ") + (available ? "1" : "0");
  INFO("期望指标行：" << expected_line);
  REQUIRE(MetricsHas(metrics.body, expected_line));
}

// =============================================================================
//  ④⑤ 接缝的区分力（关键）：两个方向都钉住 —— 字段**不是恒 false 也不是恒 true**
// =============================================================================
TEST_CASE("★ C9.30 接缝：FSS_IO_PROBE_INJECT 使 ioUringAvailable 双向翻转；ioEngine 恒 blocking",
          "[phase9][c9.30][integration]") {
  const auto info_of = [](const std::vector<std::pair<std::string, std::string>>& env,
                          JsonFields* out, std::string* metrics, std::string* banner) {
    ServerProcessOptions options;
    options.expect_grpc = false;
    options.default_grpc_port = false;
    for (const auto& kv : env) options.env.push_back(kv);
    ServerProcess server(options);
    REQUIRE(WaitReady(server.http_port()));
    const auto info = HttpGet(server.http_port(), kInfoPath);
    REQUIRE(info.status == 200);
    *out = ParseJsonFields(info.body, {"ioEngine", "ioUringAvailable"});
    REQUIRE(out->parsed);
    const auto m = HttpGet(server.http_port(), "/metrics");
    *metrics = m.body;
    *banner = server.DumpLog();
  };

  SECTION("available 注入 → true（证明字段来自探测，而不是硬编码 false）") {
    JsonFields fields;
    std::string metrics;
    std::string banner;
    info_of({{"FSS_IO_PROBE_INJECT", "available"}}, &fields, &metrics, &banner);

    REQUIRE(fields.Value("ioUringAvailable") == "True");
    //  ★ 生效引擎**不**因探测注入而改变（可用 ≠ 已启用，ADR-010 U1~U4）
    REQUIRE(fields.Value("ioEngine") == "'blocking'");
    //  指标同步为 1
    REQUIRE(MetricsHas(metrics, "fss_io_uring_available 1"));
    //  注入**自身可见**（否则演练结论会被误读成"这台机器真的可用"）
    REQUIRE(banner.find("available(injected)") != std::string::npos);
    REQUIRE(banner.find("FSS_IO_PROBE_INJECT=available") != std::string::npos);
  }

  SECTION("blocked 注入 → false（证明字段不是恒 true；复刻默认 seccomp 的形态）") {
    JsonFields fields;
    std::string metrics;
    std::string banner;
    info_of({{"FSS_IO_PROBE_INJECT", "blocked"}}, &fields, &metrics, &banner);

    REQUIRE(fields.Value("ioUringAvailable") == "False");
    REQUIRE(fields.Value("ioEngine") == "'blocking'");
    REQUIRE(MetricsHas(metrics, "fss_io_uring_available 0"));
    REQUIRE(banner.find("blocked_by_policy(injected)") != std::string::npos);
    REQUIRE(banner.find("EPERM") != std::string::npos);
  }

  SECTION("⑤ 不注入的负控：值 == 本机真实探测结果；且**随环境**为 false 时不出现 true") {
    //  ★ 这条把"负控"写成**对环境的显式断言**，而不是"必须为 false"：
    //    探测真值随部署变化 —— 本工作机（WSL2）为 true，Docker 默认 seccomp 下为 false。
    //    若这台机器的真值是 false，则必须**不出现** true（证明 true 不是恒有）；
    //    若真值是 true，则断言它与独立 syscall 探测一致（证明 true 也不是硬编码）。
    //    两个分支都**能因错误的硬编码实现而失败**（分别对应 inject ①/②）。
    JsonFields fields;
    std::string metrics;
    std::string banner;
    info_of({}, &fields, &metrics, &banner);

    const std::string probe = RunCapture(
        "python3 -c \"import ctypes;l=ctypes.CDLL('libc.so.6',use_errno=True);"
        "b=ctypes.create_string_buffer(1024);"
        "r=l.syscall(ctypes.c_long(425),ctypes.c_uint(8),ctypes.byref(b));"
        "print('True' if r>=0 else 'False')\"");
    INFO("本机独立 syscall 探测（io_uring_setup）：" << probe);
    REQUIRE((probe == "True" || probe == "False"));  // 前置条件显式断言（R9）
    REQUIRE(fields.Value("ioUringAvailable") == probe);
    REQUIRE(MetricsHas(metrics, std::string("fss_io_uring_available ") +
                                    (probe == "True" ? "1" : "0")));
    REQUIRE(banner.find("injected") == std::string::npos);  // 没有注入痕迹
    if (probe == "False") {
      //  负控的真正含义：**不出现** true（R16 正例的反面）
      REQUIRE(fields.Value("ioUringAvailable") != "True");
      REQUIRE(MetricsHas(metrics, "fss_io_uring_available 0"));
    }
  }

  SECTION("未知取值 → 不注入（宽容策略；值回到真实探测结果）") {
    JsonFields fields;
    std::string metrics;
    std::string banner;
    info_of({{"FSS_IO_PROBE_INJECT", "yes-please"}}, &fields, &metrics, &banner);
    REQUIRE(banner.find("injected") == std::string::npos);
    const std::string probe = RunCapture(
        "python3 -c \"import ctypes;l=ctypes.CDLL('libc.so.6',use_errno=True);"
        "b=ctypes.create_string_buffer(1024);"
        "r=l.syscall(ctypes.c_long(425),ctypes.c_uint(8),ctypes.byref(b));"
        "print('True' if r>=0 else 'False')\"");
    REQUIRE(fields.Value("ioUringAvailable") == probe);
  }

  SECTION("可用注入 + storage.io_engine=uring → **仍拒绝启动**（U1~U4 不受注入影响）") {
    //  ★ 这条是"注入不是偷偷启用 uring 的后门"的判据：探测说可用，但引擎实现
    //    未交付（UringIoEngine::enabled() 恒 false）⇒ 显式要求 uring 必须 exit 78。
    const auto outcome = RunServerForExit({"--set", "storage.io_engine=uring"},
                                          {{"FSS_IO_PROBE_INJECT", "available"}});
    CAPTURE(outcome.output);
    REQUIRE(outcome.exit_code == 78);
    REQUIRE(outcome.output.find("uring") != std::string::npos);
    //  拒绝理由必须走"探测通过但引擎实现未启用"这条分支（而不是"环境不可用"）
    REQUIRE(outcome.output.find("实现尚未启用") != std::string::npos);
    //  且**没有**进入服务状态
    REQUIRE(outcome.output.find("已启动") == std::string::npos);
  }
}

// =============================================================================
//  ⑥ 跨源一致性：`/v2/info` 的 ioEngine == 启动横幅 == `fss_io_engine` 指标
// =============================================================================
//  ★ 一句诚实的话：三处都来自组合根的**同一个变量** `io_engine_active`，因此这条
//    判据证明的是"三处渲染没有各写各的常量"，**不**证明该变量的取值是"正确选择"
//    （选择的正确性由 ADR-010 的 U1~U4 与 C10.4 的拒绝/回退用例覆盖）。
TEST_CASE("★ C9.30 三处渲染一致：/v2/info.ioEngine == 横幅 io engine 行 == fss_io_engine 指标",
          "[phase9][c9.30][integration]") {
  for (const std::string& inject : {std::string("available"), std::string("blocked")}) {
    ServerProcessOptions options;
    options.expect_grpc = false;
    options.default_grpc_port = false;
    options.env.push_back({"FSS_IO_PROBE_INJECT", inject});
    ServerProcess server(options);
    REQUIRE(WaitReady(server.http_port()));

    const auto info = HttpGet(server.http_port(), kInfoPath);
    const auto fields = ParseJsonFields(info.body, {"ioEngine"});
    REQUIRE(fields.parsed);
    const std::string json_engine =
        fields.Value("ioEngine").substr(1, fields.Value("ioEngine").size() - 2);

    const auto metrics = HttpGet(server.http_port(), "/metrics");
    const std::string banner = server.DumpLog();
    const std::string banner_engine = BannerIoEngine(banner);
    INFO("inject=" << inject << " json=" << json_engine << " banner=" << banner_engine);
    CAPTURE(banner, metrics.body);

    REQUIRE(json_engine == "blocking");
    REQUIRE(banner_engine == json_engine);
    //  指标行的标签值必须与之一致（`engine="<json_engine>"`，且 requested 来自配置）
    REQUIRE(MetricsHas(metrics.body,
                       "fss_io_engine{engine=\"" + json_engine + "\",requested=\"blocking\"} 1"));
  }
}

// =============================================================================
//  ⑦ 反向：注入接缝**不得**出现在配置键清单里（**不是**配置键 ⇒ 三态计数不变）
// =============================================================================
TEST_CASE("★ C9.30 反向：FSS_IO_PROBE_INJECT 不是配置键（不污染 157 键三态清单）",
          "[phase9][c9.30][integration]") {
  //  注入只认环境变量；把它写成配置键必须被拒（未知键 → exit 78）。
  const auto outcome =
      RunServerForExit({"--set", "storage.io_probe_inject=available"});
  CAPTURE(outcome.output);
  REQUIRE(outcome.exit_code == 78);
  REQUIRE(outcome.output.find("io_probe_inject") != std::string::npos);
}

// =============================================================================
//  ⑧ C7.5 的延伸：两个新字段的 `json_name` 必须与 REST 的 camelCase 逐字对齐
// =============================================================================
//  为什么单独钉：gRPC 客户端启用 proto3-JSON 时，序列化出来的键名就是 `json_name`；
//  写错（或漏写）会让"gRPC 那条协议的 JSON 形状"与 REST 不一致 —— 而 REST 是唯一
//  合规面（ADR-001）。这里用**真实序列化**（不是读 descriptor），因为 proto3-JSON
//  还带"默认值省略"的行为，一并钉住。
TEST_CASE("★ C9.30/C7.5 proto3-JSON：ioEngine / ioUringAvailable 键名与 REST 对齐",
          "[phase9][c9.30][integration]") {
  osdu::file::v1::InfoResponse response;
  response.set_version("v2");
  response.set_auth_mode("jwt");
  response.set_io_engine("blocking");
  response.set_io_uring_available(true);

  std::string json;
  REQUIRE(google::protobuf::util::MessageToJsonString(response, &json).ok());
  INFO("proto3-JSON：" << json);
  REQUIRE(json.find("\"ioEngine\":\"blocking\"") != std::string::npos);
  REQUIRE(json.find("\"ioUringAvailable\":true") != std::string::npos);
  REQUIRE(json.find("\"authMode\":\"jwt\"") != std::string::npos);

  //  反向对照：`false` 时 proto3-JSON **省略**该字段（proto3 语义），但 REST 会渲染
  //  `"ioUringAvailable":false` —— 这条差异是 proto3 的既有行为（其它布尔字段同），
  //  不是本切片的缺陷；记录在此以免后人误判成"gRPC 少了一个字段"。
  osdu::file::v1::InfoResponse off;
  off.set_io_uring_available(false);
  std::string json_off;
  REQUIRE(google::protobuf::util::MessageToJsonString(off, &json_off).ok());
  INFO("proto3-JSON（false）：" << json_off);
  REQUIRE(json_off.find("ioUringAvailable") == std::string::npos);
  //  ★ 但字段本身可读（二进制/线路上没有省略语义）
  REQUIRE_FALSE(off.io_uring_available());
  REQUIRE(off.io_engine().empty());  // 未设置 ⇒ proto3 标量默认值（无 presence，属已知语义）
}
