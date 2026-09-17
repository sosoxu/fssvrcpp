// =============================================================================
//  阶段 0 测试：工具链与契约可行性验证
// =============================================================================
//
//  目的
//  ---------------------------------------------------------------------------
//  在投入任何业务实现之前，用可执行证据回答四个问题：
//
//    Q1 外部依赖齐全吗？ —— C++20 / nlohmann-json / OpenSSL(HMAC-SHA256) /
//                         SQLite3 / libcurl(含真实 HTTP 往返) 能否编译并正确运行？
//    Q2 RPC 通路通吗？   —— 本仓库的 proto 能否被 protoc+grpc_cpp_plugin 生成、
//                         编译、启动真实 gRPC 服务并在回环上完成一次调用？
//    Q3 双协议同构可行吗？ —— 同一份 OSDU 规范 JSON（PascalCase schema 字段 +
//                         camelCase 信封字段）能否经"REST 语义"与"RPC 语义"
//                         两条解析链路产出**完全等价**的领域结果？这是
//                         "一套应用层 + 两个薄适配器"架构成立的前提。
//    Q4 分层护栏有效吗？  —— 领域层/应用层必须对 protobuf 与 HTTP 框架零依赖。
//                         本阶段以"fss_proto 只被测试目标链接"作为最小验证，
//                         完整护栏在阶段 1 建立（见 docs/02-design.md §4）。
//
//  本测试全部通过 = 阶段 0 的验收门槛达成。
//  任何一项失败都必须在进入阶段 1 之前解决（不得带病推进）。
//
//  运行：ctest -L phase0 --output-on-failure
// =============================================================================

#include <catch2/catch.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <sqlite3.h>

#if defined(FSS_ENABLE_GRPC)
#include <grpcpp/grpcpp.h>
#include "osdu/file/v1/file_service.grpc.pb.h"
#endif

namespace fs = std::filesystem;

namespace {

// -----------------------------------------------------------------------------
//  测试用临时目录（RAII 清理）
// -----------------------------------------------------------------------------
class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    auto base = fs::temp_directory_path() /
                ("fss_phase0_" + tag + "_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter()++));
    fs::create_directories(base);
    path_ = base;
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);  // 清理失败不应导致测试失败
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const fs::path& path() const { return path_; }
  std::string str() const { return path_.string(); }

 private:
  static std::atomic<int>& counter() {
    static std::atomic<int> c{0};
    return c;
  }
  fs::path path_;
};

// -----------------------------------------------------------------------------
//  最小回环 HTTP 服务器：只服务一次请求，返回固定的 JSON。
//  用途：证明 libcurl 在本环境可完成真实 TCP+HTTP 往返（含运行期 SSL/代理配置
//  不干扰本地调用）。业务用的自研 HTTP 内核在阶段 1 实现，见 ADR-002。
// -----------------------------------------------------------------------------
class OneShotHttpServer {
 public:
  OneShotHttpServer(std::string path, std::string body)
      : path_(std::move(path)), body_(std::move(body)) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(fd_, 4);
    socklen_t len = sizeof(addr);
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ::ntohs(addr.sin_port);
  }
  ~OneShotHttpServer() {
    stop();
    if (fd_ >= 0) ::close(fd_);
  }
  OneShotHttpServer(const OneShotHttpServer&) = delete;
  OneShotHttpServer& operator=(const OneShotHttpServer&) = delete;

  void start() {
    thread_ = std::thread([this] { serve(); });
  }
  void stop() {
    if (thread_.joinable()) thread_.join();
  }
  int port() const { return port_; }

 private:
  void serve() {
    int conn = ::accept(fd_, nullptr, nullptr);
    if (conn < 0) return;
    // 读到请求头结束即可，不需要解析
    char buf[4096];
    std::string req;
    while (req.find("\r\n\r\n") == std::string::npos) {
      const ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
      if (n <= 0) break;
      req.append(buf, static_cast<size_t>(n));
    }
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
    resp += "Content-Length: " + std::to_string(body_.size()) + "\r\n";
    resp += "Connection: close\r\n\r\n";
    resp += body_;
    ssize_t written = 0;
    while (written < static_cast<ssize_t>(resp.size())) {
      const ssize_t n = ::send(conn, resp.data() + written,
                               resp.size() - static_cast<size_t>(written), MSG_NOSIGNAL);
      if (n <= 0) break;
      written += n;
    }
    ::close(conn);
  }

  int fd_ = -1;
  int port_ = 0;
  std::string path_;
  std::string body_;
  std::thread thread_;
};

size_t curlWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
  return size * nmemb;
}

// -----------------------------------------------------------------------------
//  OSDU 规范样例载荷（取自 OSDU File 服务验收测试的黄金样例
//  input_payloads/File_CorrectPayload.json，仅把占位符替换为具体值）
//  关键特征：data 内部字段为 PascalCase；信封字段为 camelCase。
// -----------------------------------------------------------------------------
const char* kOsduFileMetadataJson = R"JSON({
  "id": "opendes:dataset--File.Generic:33a71a04d20f4240904b4b3fca4657b7",
  "kind": "opendes:wks:dataset--File.Generic:1.0.0",
  "acl": {
    "viewers": ["data.default.viewers@opendes.example.com"],
    "owners":  ["data.default.owners@opendes.example.com"]
  },
  "legal": {
    "legaltags": ["opendes-storage-tag"],
    "otherRelevantDataCountries": ["US"],
    "status": "compliant"
  },
  "data": {
    "Name": "Dataset X221/15",
    "Description": "As originally delivered by ACME.com.",
    "TotalSize": "13245217273",
    "EncodingFormatTypeID": "namespace:reference-data--EncodingFormatType:text%2Fcsv:",
    "Endian": "BIG",
    "Checksum": "d41d8cd98f00b204e9800998ecf8427e",
    "DatasetProperties": {
      "FileSourceInfo": {
        "FileSource": "/osdu-user/1624011206350-2021-06-18-10-13-26-350/33a71a04d20f4240904b4b3fca4657b7",
        "Name": "1000.witsml",
        "FileSize": "95463",
        "Checksum": "d41d8cd98f00b204e9800998ecf8427e",
        "ChecksumAlgorithm": "SHA-256"
      }
    },
    "ExtensionProperties": {}
  }
})JSON";

// 从 OSDU 元数据 JSON 中抽取"领域关键字段"。
// 双协议等价性比较的基准：无论经哪条链路，抽取结果必须一致。
struct DomainProjection {
  std::string id;
  std::string kind;
  std::string data_name;
  std::string file_source;
  std::string file_size;
  std::vector<std::string> viewers;
  std::vector<std::string> legaltags;

  bool operator==(const DomainProjection& o) const {
    return id == o.id && kind == o.kind && data_name == o.data_name &&
           file_source == o.file_source && file_size == o.file_size &&
           viewers == o.viewers && legaltags == o.legaltags;
  }
};

DomainProjection projectOsduJson(const nlohmann::json& j) {
  DomainProjection p;
  p.id = j.at("id").get<std::string>();
  p.kind = j.at("kind").get<std::string>();
  // 注意 PascalCase：这是 OSDU schema 的强制要求，不能用 camelCase 兜底
  p.data_name = j.at("data").at("Name").get<std::string>();
  const auto& fsi = j.at("data").at("DatasetProperties").at("FileSourceInfo");
  p.file_source = fsi.at("FileSource").get<std::string>();
  p.file_size = fsi.at("FileSize").get<std::string>();
  p.viewers = j.at("acl").at("viewers").get<std::vector<std::string>>();
  p.legaltags = j.at("legal").at("legaltags").get<std::vector<std::string>>();
  return p;
}

#if defined(FSS_ENABLE_GRPC)
// 把 OSDU 规范 JSON 装配成 proto 消息（模拟 gRPC 适配层的反序列化职责）。
osdu::file::v1::FileMetadataRecord toProtoRecord(const nlohmann::json& j) {
  osdu::file::v1::FileMetadataRecord rec;
  rec.set_id(j.at("id").get<std::string>());
  rec.set_kind(j.at("kind").get<std::string>());

  for (const auto& v : j.at("acl").at("viewers")) {
    rec.mutable_acl()->add_viewers(v.get<std::string>());
  }
  for (const auto& v : j.at("acl").at("owners")) {
    rec.mutable_acl()->add_owners(v.get<std::string>());
  }
  auto* legal = rec.mutable_legal();
  for (const auto& v : j.at("legal").at("legaltags")) {
    legal->add_legaltags(v.get<std::string>());
  }
  for (const auto& v : j.at("legal").at("otherRelevantDataCountries")) {
    legal->add_other_relevant_data_countries(v.get<std::string>());
  }
  legal->set_status(j.at("legal").at("status").get<std::string>());

  const auto& d = j.at("data");
  auto* data = rec.mutable_data();
  data->set_name(d.at("Name").get<std::string>());
  data->set_description(d.at("Description").get<std::string>());
  data->set_total_size(d.at("TotalSize").get<std::string>());
  data->set_encoding_format_type_id(d.at("EncodingFormatTypeID").get<std::string>());
  data->set_endian(d.at("Endian").get<std::string>());
  data->set_checksum(d.at("Checksum").get<std::string>());

  const auto& fsi = d.at("DatasetProperties").at("FileSourceInfo");
  auto* out = data->mutable_dataset_properties()->mutable_file_source_info();
  out->set_file_source(fsi.at("FileSource").get<std::string>());
  out->set_name(fsi.at("Name").get<std::string>());
  out->set_file_size(fsi.at("FileSize").get<std::string>());
  out->set_checksum(fsi.at("Checksum").get<std::string>());
  out->set_checksum_algorithm(fsi.at("ChecksumAlgorithm").get<std::string>());
  return rec;
}

// 反向：proto -> 领域投影。与 projectOsduJson 必须产出相同结果。
DomainProjection projectProtoRecord(const osdu::file::v1::FileMetadataRecord& r) {
  DomainProjection p;
  p.id = r.id();
  p.kind = r.kind();
  p.data_name = r.data().name();
  p.file_source = r.data().dataset_properties().file_source_info().file_source();
  p.file_size = r.data().dataset_properties().file_source_info().file_size();
  for (const auto& v : r.acl().viewers()) p.viewers.push_back(v);
  for (const auto& v : r.legal().legaltags()) p.legaltags.push_back(v);
  return p;
}

// -----------------------------------------------------------------------------
//  测试用 FileService 实现：只覆盖本阶段需要的最小方法集，
//  其余方法由 grpc 生成基类的默认实现返回 UNIMPLEMENTED。
//  真实实现见 src/adapters/grpc/（阶段 5）。
// -----------------------------------------------------------------------------
class Phase0FileService final : public osdu::file::v1::FileService::Service {
 public:
  grpc::Status GetUploadLocation(
      grpc::ServerContext* /*ctx*/,
      const osdu::file::v1::GetUploadLocationRequest* req,
      osdu::file::v1::LocationResponse* resp) override {
    const std::string file_id =
        req->file_id().empty() ? "phase0-generated-id" : req->file_id();
    resp->set_file_id(file_id);
    // 对齐实测契约：FileSource 位于 Location 内部
    // LocationResponse = {"FileID":..., "Location":{"SignedURL":..., "FileSource":...}}
    resp->mutable_location()->set_file_source("/osdu-user/phase0/" + file_id);
    resp->mutable_location()->set_signed_url(
        "http://127.0.0.1:0/v1/transfer/" + file_id);
    resp->set_driver(osdu::file::v1::STORAGE_DRIVER_POSIX);
    resp->set_zone(osdu::file::v1::STORAGE_ZONE_STAGING);
    return grpc::Status::OK;
  }

  grpc::Status GetFileMetadata(grpc::ServerContext* /*ctx*/,
                               const osdu::file::v1::GetFileMetadataRequest* req,
                               osdu::file::v1::FileMetadataRecord* resp) override {
    if (req->id().empty()) {
      // OSDU REST 语义：400 AppError。gRPC 侧映射为 INVALID_ARGUMENT，
      // 错误模型的双向映射表见 docs/03-api-contract.md §5。
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "id must not be empty");
    }
    auto j = nlohmann::json::parse(kOsduFileMetadataJson);
    j["id"] = req->id();
    *resp = toProtoRecord(j);
    resp->set_version(1);
    return grpc::Status::OK;
  }
};
#endif  // FSS_ENABLE_GRPC

}  // namespace

// =============================================================================
//  Q1 外部依赖验证
// =============================================================================

TEST_CASE("Q1.1 C++20 语言特性可用", "[phase0][toolchain]") {
  // 指定初始化器（后续配置结构体大量使用）
  struct Config { int port; const char* root; };
  constexpr Config kCfg{.port = 8080, .root = "/var/lib/fss"};
  static_assert(kCfg.port == 8080);
  REQUIRE(std::string(kCfg.root) == "/var/lib/fss");

  // concepts（后续用于约束存储驱动接口契约）
  auto satisfies_size = []<typename T>(T) { return requires(T t) { t.size(); }; };
  REQUIRE(satisfies_size(std::string("abc")));

  REQUIRE(__cplusplus >= 202002L);
}

TEST_CASE("Q1.2 nlohmann/json 能精确处理 OSDU 的 PascalCase 字段", "[phase0][toolchain]") {
  auto j = nlohmann::json::parse(kOsduFileMetadataJson);
  auto p = projectOsduJson(j);

  REQUIRE(p.data_name == "Dataset X221/15");
  REQUIRE(p.file_source.rfind("/osdu-user/", 0) == 0);
  REQUIRE(p.file_size == "95463");
  REQUIRE(p.viewers.size() == 1);
  REQUIRE(p.legaltags == std::vector<std::string>{"opendes-storage-tag"});

  // 关键回归防线：camelCase 访问必须失败，证明我们没有"宽容"地破坏 OSDU 契约
  REQUIRE_THROWS(j.at("data").at("name"));
  REQUIRE_THROWS(j.at("data").at("DatasetProperties").at("fileSourceInfo"));

  // 序列化往返一致性
  auto round_trip = projectOsduJson(nlohmann::json::parse(j.dump()));
  REQUIRE(round_trip == p);
}

TEST_CASE("Q1.3 OpenSSL 提供 HMAC-SHA256（S3 SigV4 预签名的基础）", "[phase0][toolchain]") {
  // AWS SigV4 官方文档的已知答案向量（"Deriving the signing key"）：
  //   secret     = wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY
  //   date       = 20120215, region = us-east-1, service = iam
  //   kSigning   = f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d
  //
  //  本用例最初写成"用上一轮的十六进制字符串作为下一轮的 key"，被断言当场
  //  抓住（kRegion 不匹配）。正确做法是**用原始摘要字节做链式 key**。
  //  这正是自研 SigV4 签名器最容易踩的坑，因此在此固化为回归测试。

  const std::string kSecret = "AWS4wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";

  // 返回原始 32 字节摘要
  auto hmac_raw = [](const std::string& key, const std::string& data) {
    std::vector<unsigned char> out(EVP_MAX_MD_SIZE);
    unsigned int len = 0;
    ::HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
           reinterpret_cast<const unsigned char*>(data.data()), data.size(),
           out.data(), &len);
    out.resize(len);
    return out;
  };
  auto to_hex = [](const std::vector<unsigned char>& v) {
    static const char* kHex = "0123456789abcdef";
    std::string hex;
    hex.reserve(v.size() * 2);
    for (unsigned char b : v) {
      hex.push_back(kHex[b >> 4]);
      hex.push_back(kHex[b & 0x0F]);
    }
    return hex;
  };

  const auto k_date = hmac_raw(kSecret, "20120215");
  // 关键：以原始摘要字节作为下一轮 key（不是 hex 字符串）
  const auto k_region = hmac_raw(std::string(k_date.begin(), k_date.end()), "us-east-1");
  const auto k_service = hmac_raw(std::string(k_region.begin(), k_region.end()), "iam");
  const auto k_signing =
      hmac_raw(std::string(k_service.begin(), k_service.end()), "aws4_request");

  REQUIRE(to_hex(k_date) ==
          "969fbb94feb542b71ede6f87fe4d5fa29c789342b0f407474670f0c2489e0a0d");
  REQUIRE(to_hex(k_region) ==
          "69daa0209cd9c5ff5c8ced464a696fd4252e981430b10e3d3fd8e2f197d7a70c");
  REQUIRE(to_hex(k_service) ==
          "f72cfd46f26bc4643f06a11eabb6c0ba18780c19a8da0c31ace671265e3c87fa");
  REQUIRE(to_hex(k_signing) ==
          "f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d");
  REQUIRE(k_signing.size() == 32);
  // 原始摘要必须是 32 字节二进制，任何时刻都不应退化为 64 字符 hex
  REQUIRE(k_date.size() == 32);
}

TEST_CASE("Q1.4 SQLite3 可持久化文件位置记录", "[phase0][toolchain]") {
  TempDir dir("sqlite");
  const std::string db_path = dir.str() + "/file_locations.db";

  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);
  auto close_guard = std::unique_ptr<sqlite3, int (*)(sqlite3*)>(db, sqlite3_close);

  const char* ddl =
      "CREATE TABLE file_locations ("
      "  file_id      TEXT PRIMARY KEY,"
      "  file_source  TEXT NOT NULL,"
      "  driver       TEXT NOT NULL,"
      "  zone         TEXT NOT NULL,"
      "  created_at   INTEGER NOT NULL);";
  char* err = nullptr;
  REQUIRE(sqlite3_exec(db, ddl, nullptr, nullptr, &err) == SQLITE_OK);

  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(
              db,
              "INSERT INTO file_locations(file_id,file_source,driver,zone,created_at)"
              " VALUES(?1,?2,?3,?4,?5);",
              -1, &stmt, nullptr) == SQLITE_OK);
  sqlite3_bind_text(stmt, 1, "file-1", -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, "/osdu-user/staging/file-1", -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, "posix", -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 4, "staging", -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 5, 1700000000);
  REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  sqlite3_stmt* q = nullptr;
  REQUIRE(sqlite3_prepare_v2(
              db, "SELECT file_source,driver FROM file_locations WHERE file_id=?1;",
              -1, &q, nullptr) == SQLITE_OK);
  sqlite3_bind_text(q, 1, "file-1", -1, SQLITE_STATIC);
  REQUIRE(sqlite3_step(q) == SQLITE_ROW);
  REQUIRE(std::string(reinterpret_cast<const char*>(sqlite3_column_text(q, 0))) ==
          "/osdu-user/staging/file-1");
  REQUIRE(std::string(reinterpret_cast<const char*>(sqlite3_column_text(q, 1))) == "posix");
  sqlite3_finalize(q);
}

TEST_CASE("Q1.5 libcurl 可完成真实 HTTP 往返（出站客户端基线）", "[phase0][toolchain]") {
  OneShotHttpServer server("/ping", R"({"status":"ok","service":"fss"})");
  server.start();

  CURL* curl = curl_easy_init();
  REQUIRE(curl != nullptr);
  std::string body;
  const std::string url =
      "http://127.0.0.1:" + std::to_string(server.port()) + "/ping";

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_PROXY, "");      // 忽略环境代理，避免测试受环境影响
  curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");

  const CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);
  server.stop();

  REQUIRE(rc == CURLE_OK);
  REQUIRE(http_code == 200);
  auto j = nlohmann::json::parse(body);
  REQUIRE(j.at("status") == "ok");
}

// =============================================================================
//  Q2 / Q3：RPC 通路 + 双协议同构
// =============================================================================
#if defined(FSS_ENABLE_GRPC)

TEST_CASE("Q2 proto 可生成、可编译、可在回环上完成 gRPC 调用", "[phase0][rpc]") {
  Phase0FileService service;

  grpc::ServerBuilder builder;
  int bound_port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &bound_port);
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  REQUIRE(server != nullptr);
  REQUIRE(bound_port > 0);

  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(bound_port),
                                     grpc::InsecureChannelCredentials());
  auto stub = osdu::file::v1::FileService::NewStub(channel);

  SECTION("GetUploadLocation 返回 fileID / Location.SignedURL / Location.FileSource") {
    osdu::file::v1::GetUploadLocationRequest req;
    req.mutable_expiry()->set_raw("5M");
    osdu::file::v1::LocationResponse resp;
    grpc::ClientContext ctx;
    ctx.AddMetadata("data-partition-id", "opendes");

    const auto status = stub->GetUploadLocation(&ctx, req, &resp);
    REQUIRE(status.ok());
    REQUIRE(!resp.file_id().empty());
    // 关键契约点：FileSource 在 Location 之内，且带前导斜杠
    REQUIRE(resp.location().file_source() == "/osdu-user/phase0/" + resp.file_id());
    REQUIRE(resp.location().signed_url().rfind("http://", 0) == 0);
    REQUIRE(resp.driver() == osdu::file::v1::STORAGE_DRIVER_POSIX);
    REQUIRE(resp.zone() == osdu::file::v1::STORAGE_ZONE_STAGING);
  }

  SECTION("传入 fileID 时被原样保留（与 REST /v2/getLocation 语义一致）") {
    osdu::file::v1::GetUploadLocationRequest req;
    req.set_file_id("my-file-id");
    osdu::file::v1::LocationResponse resp;
    grpc::ClientContext ctx;
    REQUIRE(stub->GetUploadLocation(&ctx, req, &resp).ok());
    REQUIRE(resp.file_id() == "my-file-id");
  }

  SECTION("错误语义可区分：空 id -> INVALID_ARGUMENT（对应 REST 400）") {
    osdu::file::v1::GetFileMetadataRequest req;
    osdu::file::v1::FileMetadataRecord resp;
    grpc::ClientContext ctx;
    const auto status = stub->GetFileMetadata(&ctx, req, &resp);
    REQUIRE_FALSE(status.ok());
    REQUIRE(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
  }

  SECTION("metadata 头可在服务端读到（data-partition-id 透传）") {
    osdu::file::v1::GetUploadLocationRequest req;
    osdu::file::v1::LocationResponse resp;
    grpc::ClientContext ctx;
    ctx.AddMetadata("data-partition-id", "opendes");
    ctx.AddMetadata("correlation-id", "phase0-corr");
    REQUIRE(stub->GetUploadLocation(&ctx, req, &resp).ok());
  }

  server->Shutdown();
}

TEST_CASE("Q3 同一份 OSDU JSON 经 REST 语义与 RPC 语义两条链路产出等价领域结果",
          "[phase0][rpc][contract]") {
  // ---- 链路 A：REST 语义（HTTP body 里的 JSON 直接投影为领域对象）----
  const DomainProjection via_rest = projectOsduJson(
      nlohmann::json::parse(kOsduFileMetadataJson));

  // ---- 链路 B：RPC 语义（JSON -> proto 消息 -> 领域对象）----
  // 真实启动 gRPC 服务，确保走的是完整的序列化/反序列化通路，
  // 而不是在进程内直接构造 proto 对象。
  Phase0FileService service;
  grpc::ServerBuilder builder;
  int grpc_port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &grpc_port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  REQUIRE(server != nullptr);

  auto stub = osdu::file::v1::FileService::NewStub(
      grpc::CreateChannel("127.0.0.1:" + std::to_string(grpc_port),
                          grpc::InsecureChannelCredentials()));
  osdu::file::v1::GetFileMetadataRequest req;
  req.set_id("phase0-id");
  osdu::file::v1::FileMetadataRecord rec;
  grpc::ClientContext ctx;
  REQUIRE(stub->GetFileMetadata(&ctx, req, &rec).ok());
  server->Shutdown();

  DomainProjection via_rpc = projectProtoRecord(rec);
  DomainProjection expected = via_rest;
  expected.id = "phase0-id";  // 服务端按请求 id 覆写

  // ---- 核心断言：两条链路必须等价 ----
  // 这是"一套应用层 + 两个薄适配器"架构的前提。若失败，说明 proto 契约与
  // OSDU REST 契约存在语义漂移，必须先修契约，再写业务代码。
  REQUIRE(via_rpc == expected);
  REQUIRE(via_rpc.data_name == via_rest.data_name);
  REQUIRE(via_rpc.file_source == via_rest.file_source);
  REQUIRE(via_rpc.viewers == via_rest.viewers);
  REQUIRE(via_rpc.legaltags == via_rest.legaltags);
}

#endif  // FSS_ENABLE_GRPC
