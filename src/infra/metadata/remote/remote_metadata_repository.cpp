// RemoteStorageServiceRepository 实现。语义、线协议与理由见头文件（ADR-004）。
#include "infra/metadata/remote/remote_metadata_repository.h"

#include "common/crypto/crypto.h"
#include "common/json/json.h"

#include <curl/curl.h>

#include <cstdint>
#include <string>
#include <utility>

namespace fss::infra {

namespace {

//  `fss_metadata_remote_requests_total{op,outcome}` —— 本切片**唯一**新增的指标族。
//  R11：远端是可选依赖，"依赖被真正使用了"必须可见；且只在 **remote 形态**存在
//  （M12 断言 sqlite 形态下 /metrics 没有这一族）。
constexpr const char* kMetricName = "fss_metadata_remote_requests_total";
constexpr const char* kMetricHelp =
    "远端 Storage Service 元数据仓储的出站请求数（op=put|get|delete|probe；outcome=ok|error）";

constexpr std::string_view kTokenKey = "metadata.remote.static_token";

bool Is2xx(long status) { return status >= 200 && status < 300; }

std::size_t WriteToString(char* data, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(data, size * nmemb);
  return size * nmemb;
}

//  version 可能是整数（本项目的 mock）或字符串（OSDU Storage 的 `versions` 数组）。
bool ReadVersion(const json::Value& value, std::int64_t* out) {
  if (value.is_number_integer()) {
    *out = value.get<std::int64_t>();
    return true;
  }
  if (value.is_string()) {
    try {
      *out = std::stoll(value.get<std::string>());
      return true;
    } catch (...) {
      return false;
    }
  }
  return false;
}

}  // namespace

// =============================================================================
//  确定性记录 id（R5）
// =============================================================================
//  为什么用 `'\0'` 分隔：不加分隔符时 `("ab","c")` 与 `("a","bc")` 会派生同一个摘要
//  （SHA-256 的分块与这种"拼接歧义"无关 —— 是**输入本身**相同），从而让两个不同
//  fileSource 静默共用一个记录 id。
std::string DeriveRemoteRecordId(std::string_view partition, std::string_view file_source) {
  std::string material(partition);
  material.push_back('\0');
  material.append(file_source);
  const std::string digest = crypto::Sha256Hex(material);  // 64 个小写 hex 字符
  //  取前 128 bit（32 个 hex 字符），按 8-4-4-4-12 分组 —— 保持契约要求的 id 形状。
  const std::string hex = digest.substr(0, 32);
  std::string uuid;
  uuid.reserve(36);
  for (std::size_t i = 0; i < hex.size(); ++i) {
    if (i == 8 || i == 12 || i == 16 || i == 20) uuid.push_back('-');
    uuid.push_back(hex[i]);
  }
  return std::string(partition) + ":dataset--File.Generic:" + uuid;
}

// =============================================================================
//  构造 / 启动自检
// =============================================================================
RemoteMetadataRepository::RemoteMetadataRepository(RemoteMetadataRepositoryOptions options,
                                                   const logging::ILogger& logger)
    : options_(std::move(options)), logger_(logger) {
  //  `min(1000, timeout_ms)`：timeout 比 1s 还短时，连接超时不能反而更长
  //  （与 `WebhookEventPublisher` / 远端校验器同一套写法）。
  if (options_.connect_timeout_ms <= 0) {
    options_.connect_timeout_ms = options_.timeout_ms < 1000 ? options_.timeout_ms : 1000;
  }
  //  指标族**在这里注册**（幂等）：只有真的装配了 remote 仓储才会有它。
  //  注册与自增同处一个类，避免"忘了注册 → /metrics 出现没有 HELP/TYPE 的族"。
  if (options_.metrics != nullptr) {
    options_.metrics->Register(kMetricName, metrics::Registry::Kind::kCounter, kMetricHelp);
  }
}

std::string RemoteMetadataRepository::NotReadyReason() const {
  return "metadata.repository=remote 但没有配置 metadata.remote.base_url —— "
         "本实例无法读写任何元数据记录（fail-closed）。请配置 Storage Service 的**基址**"
         "（例如 http://host/api/storage/v2；本适配器会追加 /records）。";
}

// =============================================================================
//  HTTP 调用 + 指标 + 失败映射
// =============================================================================
void RemoteMetadataRepository::Record(const char* op, bool ok) {
  if (options_.metrics == nullptr) return;
  options_.metrics->Increment(kMetricName,
                              {{"op", op}, {"outcome", ok ? "ok" : "error"}});
}

RemoteMetadataRepository::HttpReply RemoteMetadataRepository::Call(const char* op,
                                                                   const char* method,
                                                                   const std::string& path,
                                                                   const std::string& body,
                                                                   bool send_json_body) {
  HttpReply reply;
  //  没配地址：不发起请求（组合根本会拒绝启动，这里是双保险）。
  if (options_.base_url.empty()) {
    reply.transport_error = "未配置 metadata.remote.base_url（fail-closed）";
    return reply;
  }

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    reply.transport_error = "curl_easy_init 失败";
    return reply;
  }

  const std::string url = options_.base_url + path;
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  if (!options_.static_token.empty()) {
    headers = curl_slist_append(headers, ("Authorization: Bearer " + options_.static_token).c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  //  不跟随重定向：Storage Service 的 3xx 只可能是配置错误（例如 base_url 少/多了路径），
  //  跟随会把写请求发到别处 —— 绝不能对一个元数据 upsert 做这件事。
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(options_.connect_timeout_ms));
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(options_.timeout_ms));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  if (std::string_view(method) == "PUT") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
  } else if (std::string_view(method) == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
  }
  if (send_json_body) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply.body);
  if (!options_.verify_tls) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  } else if (!options_.ca_bundle_path.empty()) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, options_.ca_bundle_path.c_str());
  }

  const CURLcode code = curl_easy_perform(curl);
  long status = 0;
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  reply.transport_ok = (code == CURLE_OK);
  if (!reply.transport_ok) {
    reply.transport_error = curl_easy_strerror(code);
  }
  reply.status = static_cast<int>(status);
  (void)op;
  return reply;
}

fss::Error RemoteMetadataRepository::Fail(const char* op, const HttpReply& reply,
                                          const char* what) const {
  fss::ErrorKind kind = fss::ErrorKind::kUnavailable;
  std::string message;
  if (!reply.transport_ok) {
    //  连不上 / 超时 / DNS / TLS → 依赖故障（fail-closed）。
    kind = fss::ErrorKind::kUnavailable;
    message = std::string("远端 Storage Service 不可达或超时（") + what + "，op=" + op +
              "，timeout=" + std::to_string(options_.timeout_ms) + "ms）：" +
              reply.transport_error;
  } else if (reply.status == 401) {
    //  ★ 令牌错**不是**"停机"：把它报成 503 会让运维去查网络，而真正的问题是凭证。
    kind = fss::ErrorKind::kUnauthenticated;
    message = std::string("远端 Storage Service 拒绝认证（HTTP 401，op=") + op +
              "，fail-closed → 401）：请检查 " + std::string(kTokenKey) +
              "（当前" + (options_.static_token.empty() ? "为空" : "已配置") +
              "）与 token_provider=static；本适配器只支持静态 Bearer token（见运维文档 §1.2.7）。";
  } else if (reply.status == 403) {
    kind = fss::ErrorKind::kPermissionDenied;
    message = std::string("远端 Storage Service 拒绝授权（HTTP 403，op=") + op +
              "）：该 token 没有 Storage 记录写入权限。";
  } else if (reply.status == 404) {
    kind = fss::ErrorKind::kNotFound;
    message = std::string("远端 Storage Service 找不到目标记录（HTTP 404，op=") + op +
              "，" + what + "）。";
  } else {
    kind = fss::ErrorKind::kUnavailable;
    message = std::string("远端 Storage Service 返回非成功状态（HTTP ") +
              std::to_string(reply.status) + "，op=" + op + "，" + what +
              "；fail-closed → 503，绝不把依赖故障当成成功）。";
  }
  //  ★ 每一次失败都留一条 warn（op + status + error kind）—— 这是"依赖故障被静默吞掉"
  //    的唯一可观测入口。
  logging::Warn(logger_, "远端元数据仓储请求失败：" + message,
                {{"component", "metadata_remote"},
                 {"op", op},
                 {"status", reply.transport_ok ? std::to_string(reply.status) : std::string("n/a")},
                 {"error_kind", std::string(fss::ErrorKindName(kind))}});
  return fss::Error(kind, std::move(message));
}

fss::Result<domain::FileMetadataRecord> RemoteMetadataRepository::ParseRecord(
    const char* op, const HttpReply& reply, const domain::FileMetadataRecord* fallback) const {
  const auto parsed = json::Parse(reply.body);
  if (!parsed.ok() || !parsed.value().is_object()) {
    return Err(fss::ErrorKind::kUnavailable,
               std::string("远端 Storage Service 的响应不是合法 JSON 对象（op=") + op +
                   "；fail-closed → 503）");
  }
  const auto& root = parsed.value();
  //  ① 整条记录 JSON（本项目与 mock 钉住的形状）
  if (root.contains("kind") && root.contains("data")) {
    auto record = domain::ParseFileMetadataRecord(root);
    if (!record.ok()) {
      return Err(fss::ErrorKind::kUnavailable,
                 std::string("远端 Storage Service 返回的记录 JSON 缺必需字段或结构非法（op=") +
                     op + "）：" + record.error().message() + "（fail-closed → 503）");
    }
    return record.value();
  }
  //  ② OSDU Storage 的 upsert 信封 `{recordCount, recordIds, versions}`（**未与真实服务联调**；
  //     只有 `PUT` 有 fallback，GET 路径我们要求它直接返回记录 JSON）。
  if (fallback != nullptr && root.contains("versions") && root["versions"].is_array() &&
      !root["versions"].empty()) {
    domain::FileMetadataRecord out = *fallback;
    if (!ReadVersion(root["versions"][0], &out.version)) {
      return Err(fss::ErrorKind::kUnavailable,
                 std::string("远端 Storage Service 的信封里 versions[0] 不是整数/数字串（op=") +
                     op + "；fail-closed → 503）");
    }
    if (root.contains("recordIds") && root["recordIds"].is_array() &&
        !root["recordIds"].empty() && root["recordIds"][0].is_string()) {
      out.id = root["recordIds"][0].get<std::string>();
    }
    return out;
  }
  return Err(fss::ErrorKind::kUnavailable,
             std::string("远端 Storage Service 的响应既不是记录 JSON 也不是 upsert 信封"
                         "（缺 kind/data 与 versions，op=") + op + "；fail-closed → 503）");
}

// =============================================================================
//  记录 CRUD
// =============================================================================
fss::Result<domain::FileMetadataRecord> RemoteMetadataRepository::Create(
    std::string_view partition, const domain::FileMetadataRecord& record) {
  //  ★ R5：id **必须**由幂等键派生 —— 覆盖调用方给的任何 id（用例给的是随机 UUID）。
  //    这样"同一 fileSource 的任何重试"都打到同一个远端记录上（upsert by id）。
  domain::FileMetadataRecord to_put = record;
  const std::string& file_source =
      to_put.data.dataset_properties.file_source_info.file_source;
  if (file_source.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "远端元数据仓储 Create 需要非空 FileSource（幂等键）：无法派生记录 id");
  }
  to_put.id = DeriveRemoteRecordId(partition, file_source);

  const std::string body = json::Dump(domain::ToJson(to_put));
  const HttpReply reply = Call("put", "PUT", "/records", body, /*send_json_body=*/true);
  if (!reply.transport_ok || !Is2xx(reply.status)) {
    Record("put", false);
    return Fail("put", reply, "PUT /records");
  }
  auto parsed = ParseRecord("put", reply, &to_put);
  if (!parsed.ok()) {
    Record("put", false);
    logging::Warn(logger_, "远端元数据仓储响应无法解析：" + parsed.error().message(),
                  {{"component", "metadata_remote"}, {"op", "put"}});
    return parsed.error();
  }
  Record("put", true);
  return parsed.value();
}

fss::Result<domain::FileMetadataRecord> RemoteMetadataRepository::Update(
    std::string_view partition, const domain::FileMetadataRecord& record) {
  (void)partition;
  if (record.id.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "远端元数据仓储 Update 需要非空记录 id");
  }
  const std::string body = json::Dump(domain::ToJson(record));
  const HttpReply reply = Call("put", "PUT", "/records", body, /*send_json_body=*/true);
  if (!reply.transport_ok || !Is2xx(reply.status)) {
    Record("put", false);
    return Fail("put", reply, "PUT /records（更新）");
  }
  auto parsed = ParseRecord("put", reply, &record);
  if (!parsed.ok()) {
    Record("put", false);
    logging::Warn(logger_, "远端元数据仓储响应无法解析：" + parsed.error().message(),
                  {{"component", "metadata_remote"}, {"op", "put"}});
    return parsed.error();
  }
  Record("put", true);
  return parsed.value();
}

fss::Result<domain::FileMetadataRecord> RemoteMetadataRepository::GetById(
    std::string_view partition, std::string_view record_id) {
  (void)partition;
  //  ★ 不做百分号编码：OSDU 记录 id 的字符集是 `[A-Za-z0-9:._-]`，全部是 RFC 3986 的
  //    unreserved 或 sub-delims（`:` `.` `-`），在路径段里**不需要**编码。若将来 id
  //    形状变化，必须在这里显式处理，而不是靠"看起来没坏"。
  const HttpReply reply = Call("get", "GET", "/records/" + std::string(record_id), "", false);
  if (!reply.transport_ok || !Is2xx(reply.status)) {
    Record("get", false);
    return Fail("get", reply, "GET /records/{id}");
  }
  auto parsed = ParseRecord("get", reply, nullptr);
  if (!parsed.ok()) {
    Record("get", false);
    return parsed.error();
  }
  Record("get", true);
  return parsed.value();
}

fss::Result<domain::FileMetadataRecord> RemoteMetadataRepository::GetLatestByFileSource(
    std::string_view partition, std::string_view file_source) {
  //  远端没有 `?fileSource=` 查询端点（那是**未文档化**的）—— 同一 fileSource 恒映射到
  //  同一派生 id，因此按 id 取就是"取最新"（Storage 的 `GET /records/{id}` 语义）。
  const std::string derived = DeriveRemoteRecordId(partition, file_source);
  const HttpReply reply = Call("get", "GET", "/records/" + derived, "", false);
  if (!reply.transport_ok || !Is2xx(reply.status)) {
    Record("get", false);
    return Fail("get", reply, "GET /records/{derived_id}（按 FileSource 取）");
  }
  auto parsed = ParseRecord("get", reply, nullptr);
  if (!parsed.ok()) {
    Record("get", false);
    return parsed.error();
  }
  Record("get", true);
  return parsed.value();
}

fss::Result<void> RemoteMetadataRepository::Delete(std::string_view partition,
                                                   std::string_view record_id) {
  (void)partition;
  const HttpReply reply =
      Call("delete", "POST", "/records/" + std::string(record_id) + ":delete", "", false);
  //  ★ ADR-004：**只有 204 算成功**。200（带一个"删除结果"体）也是失败 ——
  //    我们无法从中确认记录真的被删掉，宁可控地失败也不谎报成功。
  if (!reply.transport_ok || reply.status != 204) {
    Record("delete", false);
    return Fail("delete", reply, "POST /records/{id}:delete（要求 204）");
  }
  Record("delete", true);
  return Ok();
}

// =============================================================================
//  远端不存在的原语（原子领取）—— kUnimplemented，绝不假装
// =============================================================================
namespace {
fss::Error NoAtomicClaim(std::string_view method) {
  return Err(fss::ErrorKind::kUnimplemented,
             "远端 Storage Service 元数据仓储不支持原子领取（capabilities().atomic_claim=false）：" +
                 std::string(method) +
                 " 需要 ADR-009 §4.2 的\"条件插入\"数据库原语与一个**不属于 OSDU 记录**的 "
                 "state 列，远端两者都没有。用例必须按 capabilities() 走无领取路径"
                 "（本形态下该方法永不被调用）。");
}
}  // namespace

fss::Result<domain::MetadataClaim> RemoteMetadataRepository::ClaimForWrite(
    std::string_view, const domain::FileMetadataRecord&) {
  return NoAtomicClaim("ClaimForWrite");
}

fss::Result<domain::FileMetadataRecord> RemoteMetadataRepository::MarkReady(
    std::string_view, std::string_view, std::int64_t, const domain::FileMetadataRecord&) {
  return NoAtomicClaim("MarkReady");
}

fss::Result<void> RemoteMetadataRepository::ReleaseClaim(std::string_view, std::string_view,
                                                         std::int64_t) {
  return NoAtomicClaim("ReleaseClaim");
}

fss::Result<std::int64_t> RemoteMetadataRepository::ReclaimStaleClaiming(
    std::string_view, std::int64_t, int, const std::vector<std::string>&) {
  return NoAtomicClaim("ReclaimStaleClaiming");
}

fss::Result<domain::MetadataPage> RemoteMetadataRepository::List(std::string_view,
                                                                 const domain::MetadataQuery&) {
  return Err(fss::ErrorKind::kUnimplemented,
             "远端 Storage Service 元数据仓储不实现 List：ADR-004 的三方法草图与 "
             "docs/01-osdu-research.md 都没有\"按条件列举记录\"的端点依据，本适配器不发明"
             "一个未文档化的 query 端点。读路径用 GetById / GetLatestByFileSource。");
}

// =============================================================================
//  就绪探针
// =============================================================================
fss::Result<void> RemoteMetadataRepository::Probe() {
  //  固定、**不会存在**的 source（前缀刻意用 "fss-readiness-probe:" 且不含 `/`，
  //  它不是合法的 staging file_source）→ 正常服务必然 404。**404 = 服务活着**。
  static constexpr std::string_view kProbeSource = "fss-readiness-probe:liveness";
  const std::string derived = DeriveRemoteRecordId("__probe__", kProbeSource);
  const HttpReply reply = Call("probe", "GET", "/records/" + derived, "", false);
  //  404 与 2xx 都说明"服务在按契约回答"（2xx 是那个 source 意外存在的保守接受）。
  if (reply.transport_ok && (reply.status == 404 || Is2xx(reply.status))) {
    Record("probe", true);
    return Ok();
  }
  Record("probe", false);
  return Fail("probe", reply, "GET /records/{probe_id}（就绪探针；404 = 服务活着）");
}

}  // namespace fss::infra
