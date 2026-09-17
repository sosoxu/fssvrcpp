// S3BlobStore 实现：预签名（切片 2）+ 数据面（切片 3，libcurl 流式）。
//
// 数据面的三条口径（都由实测/契约决定，不是"看起来合理"）：
//   ① **上传流式**：`x-amz-content-sha256: UNSIGNED-PAYLOAD`（HTTPS 下 S3 接受；流式上传
//      无法在发第一字节前知道整包哈希）。若调用方给了 `expected_checksum`，我们**同时**
//      带上 `x-amz-checksum-sha256`（base64）让存储侧自己拒收错包，并在本地边传边算，
//      以便报出 `kChecksumMismatch` 的 `details{expected,actual}`（契约 §5）。
//   ② **下载流式**：`get` 直接把响应体写进 `ByteSink`，RSS 与对象大小无关；
//      错误响应体（4xx/5xx 的 XML）**不写进 sink**，而是留在内存里做错误映射。
//   ③ **超时**：数据面**没有整体超时**（C4.11 的同一原则），只有"无进展"的空闲断开
//      （`transfer_idle_timeout_seconds`）；控制类操作（list/stat/copy/remove）用整体超时。
#include "infra/blob/s3/s3_blob_store.h"

#include "common/crypto/crypto.h"
#include "common/time/time_format.h"

#include <curl/curl.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fss::infra {

namespace {

// -----------------------------------------------------------------------------
//  小工具
// -----------------------------------------------------------------------------
void EnsureCurlInitialized() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string TrimQuotes(std::string text) {
  while (!text.empty() && (text.front() == '"' || text.front() == ' ')) text.erase(text.begin());
  while (!text.empty() && (text.back() == '"' || text.back() == ' ' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

std::optional<std::vector<std::uint8_t>> HexToBytes(std::string_view hex) {
  if (hex.size() % 2 != 0) return std::nullopt;
  const auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::vector<std::uint8_t> out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int high = digit(hex[i]);
    const int low = digit(hex[i + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    out.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return out;
}

// ---- 极简 XML 取值（够 S3 的响应；R18：转义/反转义配了等价比对用例）----
std::string XmlUnescape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '&') {
      out.push_back(text[i]);
      continue;
    }
    const auto end = text.find(';', i);
    if (end == std::string_view::npos) {
      out.push_back(text[i]);
      continue;
    }
    const std::string_view entity = text.substr(i + 1, end - i - 1);
    if (entity == "amp") {
      out.push_back('&');
    } else if (entity == "lt") {
      out.push_back('<');
    } else if (entity == "gt") {
      out.push_back('>');
    } else if (entity == "quot") {
      out.push_back('"');
    } else if (entity == "apos") {
      out.push_back('\'');
    } else if (!entity.empty() && entity.front() == '#') {
      std::size_t pos = 1;
      int base = 10;
      if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')) {
        base = 16;
        pos = 2;
      }
      const long code = std::strtol(std::string(entity.substr(pos)).c_str(), nullptr, base);
      if (code > 0 && code < 256) out.push_back(static_cast<char>(code));
    } else {
      out.append(text.substr(i, end - i + 1));  // 不认识的实体原样保留
      i = end;
      continue;
    }
    i = end;
  }
  return out;
}

//  取第一个 `<tag>...</tag>` 的内容（已反转义）。用整段 `<tag>` 匹配，避免
//  `<Key` 命中 `<KeyCount>`（S3 的 ListBucketResult 里两者相邻）
std::optional<std::string> FindTag(std::string_view xml, std::string_view tag) {
  const std::string open = "<" + std::string(tag) + ">";
  const auto pos = xml.find(open);
  if (pos == std::string_view::npos) return std::nullopt;
  const std::string close = "</" + std::string(tag) + ">";
  const auto end = xml.find(close, pos + open.size());
  if (end == std::string_view::npos) return std::nullopt;
  return XmlUnescape(xml.substr(pos + open.size(), end - pos - open.size()));
}

std::vector<std::string> FindAllBlocks(std::string_view xml, std::string_view tag) {
  std::vector<std::string> blocks;
  const std::string open = "<" + std::string(tag) + ">";
  const std::string close = "</" + std::string(tag) + ">";
  std::size_t pos = 0;
  while ((pos = xml.find(open, pos)) != std::string_view::npos) {
    const auto end = xml.find(close, pos + open.size());
    if (end == std::string_view::npos) break;
    blocks.emplace_back(xml.substr(pos + open.size(), end - pos - open.size()));
    pos = end + close.size();
  }
  return blocks;
}

//  `Sun, 06 Nov 1994 08:49:37 GMT` → epoch 秒（解析失败返回 0）
std::int64_t ParseHttpDate(const std::string& text) {
  std::tm tm{};
  if (::strptime(text.c_str(), "%a, %d %b %Y %H:%M:%S", &tm) == nullptr) return 0;
  return static_cast<std::int64_t>(::timegm(&tm));
}

//  S3 的 `<Error><Code>` → 我们的 ErrorKind（契约 §5 的映射，C5.7）
fss::Error MapS3Error(long status, const std::string& body, std::string_view what) {
  const std::string code = FindTag(body, "Code").value_or("");
  const std::string message = FindTag(body, "Message").value_or(body.substr(0, 200));
  const std::string context = std::string(what) + " 失败（HTTP " + std::to_string(status) +
                              (code.empty() ? "" : " " + code) + "）：" + message;
  if (code == "NoSuchKey" || code == "NoSuchBucket" || status == 404) {
    return Err(fss::ErrorKind::kNotFound, context);
  }
  if (code == "AccessDenied" || code == "SignatureDoesNotMatch" ||
      code == "InvalidAccessKeyId" || status == 403) {
    //  ★ 存储侧拒绝与"用户无权限"是两回事：映射成 kStorageAccessDenied（依赖故障一侧），
    //    不能映射成 401/403 让客户端以为是自己没权限
    return Err(fss::ErrorKind::kStorageAccessDenied, context);
  }
  if (code == "SlowDown" || code == "ServiceUnavailable" || status == 503) {
    return Err(fss::ErrorKind::kUnavailable, context);
  }
  if (code == "BadDigest" || code == "InvalidDigest" || code == "XAmzContentSHA256Mismatch") {
    return Err(fss::ErrorKind::kChecksumMismatch, context);
  }
  if (code == "InvalidRange" || status == 416) {
    //  契约：`offset >= size` 属于"不可满足区间" → kInvalidArgument（对齐 S3 的 416 语义），
    //  不能落进 kInternal —— 那是让调用方以为"服务端出错了"
    return Err(fss::ErrorKind::kInvalidArgument, context);
  }
  if (status >= 500) return Err(fss::ErrorKind::kBadGateway, context);
  return Err(fss::ErrorKind::kInternal, context);
}

// -----------------------------------------------------------------------------
//  一次 HTTP 调用
// -----------------------------------------------------------------------------
struct DownloadState {
  long status = 0;
  bytes::ByteSink* sink = nullptr;
  std::string* error_body = nullptr;
  std::map<std::string, std::string>* headers = nullptr;
  bool sink_failed = false;
  std::string error;
  std::int64_t received = 0;
  //  响应体的采集上限：**错误体**只要够做错误映射（64 KiB）；但**成功的 XML 响应**
  //  （ListObjectsV2 一页 1000 个键 ≈ 200 KiB）必须完整收下 —— 曾用同一个 64 KiB 上限，
  //  结果 list 只解析出前 ~425 个键（P5-D05：分页"不重不漏"用例立刻抓到）。
  std::size_t capture_limit = 64 * 1024;
};

struct UploadState {
  bytes::ByteSource* source = nullptr;
  std::int64_t sent = 0;
  bool failed = false;
  std::string error;
  crypto::Sha256Hasher hasher;  // 边传边算（checksum 校验用）
};

struct HttpCall {
  std::string method;
  std::string url;
  std::vector<std::string> headers;     // "Name: value"（含 Authorization）
  bool head_only = false;
  bytes::ByteSource* upload = nullptr;
  std::int64_t upload_size = -1;
  bytes::ByteSink* download = nullptr;
  bool no_total_timeout = false;        // 数据面：只用"无进展"空闲超时
  std::string* error_body = nullptr;
  std::map<std::string, std::string>* response_headers = nullptr;
  std::size_t capture_limit = 64 * 1024;  // 见 DownloadState 的说明
};

std::size_t ReadCallback(char* buffer, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* state = static_cast<UploadState*>(userdata);
  const std::size_t capacity = size * nmemb;
  if (state->failed) return 0;
  auto read = state->source->Read(buffer, capacity);
  if (!read.ok()) {
    state->failed = true;
    state->error = read.error().ToString();
    return CURL_READFUNC_ABORT;
  }
  if (read.value() > 0) {
    state->hasher.Update(std::string_view(buffer, read.value()));
    state->sent += static_cast<std::int64_t>(read.value());
  }
  return read.value();
}

std::size_t WriteCallback(char* data, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* state = static_cast<DownloadState*>(userdata);
  const std::size_t length = size * nmemb;
  state->received += static_cast<std::int64_t>(length);
  if (state->status >= 400) {
    //  ★ 错误体**不写进调用方的 sink**：否则调用方会把一坨 XML 当成对象内容
    if (state->error_body != nullptr && state->error_body->size() < state->capture_limit) {
      state->error_body->append(data, length);
    }
    return length;
  }
  if (state->sink == nullptr) {
    if (state->error_body != nullptr && state->error_body->size() < state->capture_limit) {
      state->error_body->append(data, length);  // 无 sink 时把体收回（如 XML 响应）
    } else if (state->error_body != nullptr) {
      //  采满了还继续来：**必须暴露**，不能静默丢掉后半段（那正是 P5-D05 的表现）
      state->sink_failed = true;
      state->error = "响应体超过采集上限 " + std::to_string(state->capture_limit) + " 字节";
      return 0;
    }
    return length;
  }
  const auto written = state->sink->Write(std::string_view(data, length));
  if (!written.ok()) {
    state->sink_failed = true;
    state->error = written.error().ToString();
    return 0;  // 中止传输
  }
  return length;
}

std::size_t HeaderCallback(char* data, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* state = static_cast<DownloadState*>(userdata);
  const std::size_t length = size * nmemb;
  const std::string line(data, length);
  if (StartsWith(line, "HTTP/")) {
    const auto first_space = line.find(' ');
    if (first_space != std::string::npos) {
      state->status = std::atol(line.c_str() + first_space + 1);
    }
    return length;
  }
  if (state->headers != nullptr) {
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
      std::string name = Lower(line.substr(0, colon));
      std::string value = line.substr(colon + 1);
      while (!value.empty() && value.front() == ' ') value.erase(value.begin());
      while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
      (*state->headers)[name] = value;
    }
  }
  return length;
}

//  执行一次请求。返回 HTTP 状态码（<400）；传输层失败 → Error
fss::Result<long> CurlPerform(const S3Options& options, HttpCall& call, UploadState* upload_state) {
  EnsureCurlInitialized();
  CURL* curl = curl_easy_init();
  if (curl == nullptr) return Err(fss::ErrorKind::kInternal, "curl_easy_init 失败");
  struct Guard {
    CURL* handle;
    curl_slist* list = nullptr;
    ~Guard() {
      if (list != nullptr) curl_slist_free_all(list);
      curl_easy_cleanup(handle);
    }
  } guard{curl};

  DownloadState download_state;
  download_state.sink = call.download;
  download_state.error_body = call.error_body;
  download_state.headers = call.response_headers;
  download_state.capture_limit = call.capture_limit;

  curl_easy_setopt(curl, CURLOPT_URL, call.url.c_str());
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, options.connect_timeout_ms);
  if (call.no_total_timeout) {
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                     static_cast<long>(options.transfer_idle_timeout_seconds));
  } else {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, options.total_timeout_ms);
  }
  if (!options.verify_tls) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  } else if (!options.ca_bundle_path.empty()) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, options.ca_bundle_path.c_str());
  }

  if (call.head_only) {
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  } else if (call.method == "PUT") {
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, ReadCallback);
    curl_easy_setopt(curl, CURLOPT_READDATA, upload_state);
    if (call.upload_size >= 0) {
      curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                       static_cast<curl_off_t>(call.upload_size));
    }
  } else if (call.method == "GET") {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  } else {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, call.method.c_str());
  }

  for (const auto& header : call.headers) {
    guard.list = curl_slist_append(guard.list, header.c_str());
  }
  if (guard.list != nullptr) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, guard.list);

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download_state);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &download_state);

  const CURLcode code = curl_easy_perform(curl);
  long status = download_state.status;
  if (status == 0) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  if (code != CURLE_OK) {
    if (upload_state != nullptr && upload_state->failed) {
      return Err(fss::ErrorKind::kUnavailable, "读取上传源失败：" + upload_state->error);
    }
    if (download_state.sink_failed) {
      return Err(fss::ErrorKind::kUnavailable, "写入下载目标失败：" + download_state.error);
    }
    return Err(fss::ErrorKind::kUnavailable,
               std::string("curl 传输失败：") + curl_easy_strerror(code));
  }
  return status;
}

}  // namespace

// =============================================================================
//  构造 / 能力 / 预签名
// =============================================================================
SigV4Options MakeSignOptions(const S3Options& options) {
  SigV4Options sign_options;
  sign_options.region = options.region;
  sign_options.service = "s3";
  sign_options.force_path_style = options.force_path_style;
  sign_options.endpoint = options.endpoint;
  sign_options.scheme = options.effective_scheme();
  return sign_options;
}

//  `x-amz-date` 形如 `20130524T000000Z`（UTC、无分隔符、**无毫秒**）
//  ⚠️ `time::ToIso8601Utc` 带毫秒，必须先截到秒（P5-D02：错误的时间格式会伪装成签名错误）
std::string AmzDate(std::int64_t epoch_seconds) {
  const std::string iso = time::ToIso8601Utc(epoch_seconds);
  const std::string seconds = iso.substr(0, 19);
  std::string out;
  out.reserve(17);
  for (const char c : seconds) {
    if (c == '-' || c == ':') continue;
    out.push_back(c);
  }
  out.push_back('Z');
  return out;
}

S3BlobStore::S3BlobStore(S3Options options, const fss::IClock& clock)
    : options_(std::move(options)),
      sign_options_(MakeSignOptions(options_)),
      signer_(options_.credentials, sign_options_),
      clock_(clock) {}

domain::BlobCapabilities S3BlobStore::capabilities() const {
  domain::BlobCapabilities caps;
  caps.native_presign = true;    // ★ 客户端直连存储端点（服务不代理字节，C5.8）
  caps.server_side_copy = true;  // x-amz-copy-source
  caps.range_read = true;        // S3 原生支持 Range
  caps.streaming_put = true;     // libcurl 流式
  caps.recommended_part_size = 8 * 1024 * 1024;
  caps.driver_name =
      options_.driver_report_override.empty() ? "s3" : options_.driver_report_override;
  return caps;
}

fss::Result<std::string> S3BlobStore::BuildUrl(const domain::ObjectRef& ref) const {
  return signer_.BuildUrl(ref.container, ref.key, {}, /*include_query=*/false);
}

S3Options S3BlobStore::EffectiveOptions() const { return options_; }

std::string S3BlobStore::ObjectPath(const domain::ObjectRef& ref) const {
  if (options_.force_path_style) return "/" + ref.container + "/" + ref.key;
  return "/" + ref.key;
}

std::string S3BlobStore::BucketPath(const std::string& container) const {
  if (options_.force_path_style) return "/" + container;
  return "/";
}

fss::Result<domain::SignedLocation> S3BlobStore::Presign(const domain::ObjectRef& ref,
                                                         const domain::PresignOptions& options,
                                                         std::string_view method) {
  if (options_.endpoint.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "S3：endpoint 未配置");
  }
  if (options_.credentials.access_key.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "S3：缺少 access_key，无法预签名");
  }
  //  有效期：缺省 → presign_default_seconds；超过上限 → **拒绝**（不是静默截断：
  //  静默截断会让调用方以为拿到的是 7 天。契约 §1.4 的 expiryTime 是另一回事，
  //  那里 OSDU 明确要求静默截断）
  const std::int64_t ttl = options.expires_in_seconds > 0 ? options.expires_in_seconds
                                                          : options_.presign_default_seconds;
  if (ttl > options_.presign_max_seconds) {
    return Err(fss::ErrorKind::kInvalidArgument, "S3：预签名有效期超过上限 " +
                                                     std::to_string(options_.presign_max_seconds) +
                                                     " 秒");
  }

  const std::int64_t now = clock_.NowEpochSeconds();
  const std::string amz_date = AmzDate(now);

  SigV4Request request;
  request.method = std::string(method);
  request.host = signer_.HostFor(ref.container);
  request.path = options_.force_path_style
                     ? "/" + ref.container + (ref.key.empty() ? "" : "/" + ref.key)
                     : "/" + ref.key;
  request.headers.emplace_back("host", request.host);
  if (!options.content_type.empty()) {
    request.headers.emplace_back("content-type", options.content_type);
  }
  request.payload_sha256_hex = "UNSIGNED-PAYLOAD";  // 预签名无法预知请求体

  FSS_TRY(url, signer_.PresignUrl(request, amz_date, ttl));

  domain::SignedLocation out;
  out.url = std::move(url);
  out.method = std::string(method);
  out.expires_at_epoch_seconds = now + ttl;
  out.native = true;
  if (!options.content_type.empty()) {
    out.required_headers.emplace_back("Content-Type", options.content_type);
  }
  return out;
}

fss::Result<domain::SignedLocation> S3BlobStore::presign_put(const domain::ObjectRef& ref,
                                                             const domain::PresignOptions& options) {
  return Presign(ref, options, "PUT");
}

fss::Result<domain::SignedLocation> S3BlobStore::presign_get(const domain::ObjectRef& ref,
                                                             const domain::PresignOptions& options) {
  return Presign(ref, options, "GET");
}

// =============================================================================
//  数据面（切片 3）
// =============================================================================
namespace {

fss::Result<void> SignAndCollect(const SigV4Signer& signer, SigV4Request& request,
                                 std::string_view amz_date,
                                 std::vector<std::string>* out_headers) {
  FSS_TRY(signed_request, signer.SignRequest(request, amz_date));
  out_headers->push_back("Authorization: " + signed_request.authorization);
  for (const auto& [name, value] : request.headers) {
    //  `host` 由 libcurl 自己发；其余签过的头必须**原样**发出去，否则服务端重算不一致
    if (name == "host") continue;
    out_headers->push_back(name + ": " + value);
  }
  return Ok();
}

}  // namespace

fss::Result<void> S3BlobStore::ensure_container(const std::string& container) {
  if (container.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "S3：容器名不能为空");
  }
  //  先 HEAD（幂等：已存在就直接成功）
  {
    SigV4Request request;
    request.method = "HEAD";
    request.host = signer_.HostFor(container);
    request.path = BucketPath(container);
    request.headers.emplace_back("host", request.host);
    const std::string amz_date = AmzDate(clock_.NowEpochSeconds());
    request.headers.emplace_back("x-amz-date", amz_date);
    request.headers.emplace_back("x-amz-content-sha256", crypto::Sha256Hex(""));
    request.payload_sha256_hex = crypto::Sha256Hex("");

    HttpCall call;
    call.method = "HEAD";
    call.url = options_.effective_scheme() + "://" + request.host + request.path;
    call.head_only = true;
    FSS_TRY(SignAndCollect(signer_, request, amz_date, &call.headers));
    FSS_TRY(status, CurlPerform(options_, call, nullptr));
    if (status >= 200 && status < 300) return Ok();
    if (status != 404) {
      return MapS3Error(status, "", "ensure_container(HEAD)");
    }
  }
  //  不存在 → 创建
  SigV4Request request;
  request.method = "PUT";
  request.host = signer_.HostFor(container);
  request.path = BucketPath(container);
  request.headers.emplace_back("host", request.host);
  const std::string amz_date = AmzDate(clock_.NowEpochSeconds());
  request.headers.emplace_back("x-amz-date", amz_date);
  request.headers.emplace_back("x-amz-content-sha256", crypto::Sha256Hex(""));
  request.payload_sha256_hex = crypto::Sha256Hex("");

  HttpCall call;
  call.method = "PUT";
  call.url = options_.effective_scheme() + "://" + request.host + request.path;
  bytes::StringSource empty("");
  call.upload = &empty;
  call.upload_size = 0;
  std::string error_body;
  call.error_body = &error_body;
  FSS_TRY(SignAndCollect(signer_, request, amz_date, &call.headers));
  FSS_TRY(status, CurlPerform(options_, call, nullptr));
  if (status >= 400) return MapS3Error(status, error_body, "ensure_container(PUT)");
  return Ok();
}

fss::Result<void> S3BlobStore::put(const domain::ObjectRef& ref, bytes::ByteSource& source,
                                   const domain::PutOptions& options) {
  if (ref.container.empty() || ref.key.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "S3：container 与 key 都不能为空");
  }
  if (!options.expected_checksum.empty() && options.checksum_algorithm.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "S3：给了 expected_checksum 就必须给 checksum_algorithm（否则无法校验）");
  }
  const auto source_size = source.Size();
  if (options.expected_size >= 0 && source_size.has_value() &&
      source_size.value() != options.expected_size) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "S3：expected_size=" + std::to_string(options.expected_size) + " 与实际长度 " +
                   std::to_string(source_size.value()) + " 不符");
  }
  if (options.expected_size >= 0 && !source_size.has_value()) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "S3：expected_size 已声明但源长度未知，无法保证不静默截断");
  }

  SigV4Request request;
  request.method = "PUT";
  request.host = signer_.HostFor(ref.container);
  request.path = ObjectPath(ref);
  request.headers.emplace_back("host", request.host);
  const std::string amz_date = AmzDate(clock_.NowEpochSeconds());
  request.headers.emplace_back("x-amz-date", amz_date);
  //  流式上传：用 UNSIGNED-PAYLOAD（无法预先知道整包哈希）
  request.headers.emplace_back("x-amz-content-sha256", "UNSIGNED-PAYLOAD");
  request.payload_sha256_hex = "UNSIGNED-PAYLOAD";
  if (!options.content_type.empty()) {
    request.headers.emplace_back("content-type", options.content_type);
  }
  if (!options.expected_checksum.empty()) {
    //  让存储侧也校验（S3：base64 的 SHA-256；错包直接被拒）
    const auto digest = HexToBytes(options.expected_checksum);
    if (!digest.has_value() || options.checksum_algorithm != "SHA256") {
      return Err(fss::ErrorKind::kInvalidArgument,
                 "S3：只支持 SHA256 的 expected_checksum（十六进制）");
    }
    request.headers.emplace_back("x-amz-checksum-sha256", crypto::Base64Encode(digest.value()));
  }

  UploadState upload_state;
  upload_state.source = &source;

  HttpCall call;
  call.method = "PUT";
  call.url = options_.effective_scheme() + "://" + request.host + request.path;
  call.upload = &source;
  call.upload_size = source_size.value_or(-1);
  call.no_total_timeout = true;  // 数据面没有整体超时（C4.11）
  std::string error_body;
  call.error_body = &error_body;
  FSS_TRY(SignAndCollect(signer_, request, amz_date, &call.headers));

  FSS_TRY(status, CurlPerform(options_, call, &upload_state));
  if (status >= 400) {
    auto error = MapS3Error(status, error_body, "put");
    if (error.kind() == fss::ErrorKind::kChecksumMismatch &&
        !options.expected_checksum.empty()) {
      //  契约 §5：details 里要带 expected/actual 便于排障
      fss::Error enriched(error.kind(), error.message());
      enriched.With("expected", options.expected_checksum)
          .With("actual", upload_state.hasher.HexDigest());
      return enriched;
    }
    return error;
  }
  //  本地也校验一遍：存储侧可能没实现 `x-amz-checksum-sha256`（如某些 S3 兼容实现）
  if (!options.expected_checksum.empty()) {
    const std::string actual = upload_state.hasher.HexDigest();
    if (actual != options.expected_checksum) {
      fss::Error error(fss::ErrorKind::kChecksumMismatch, "put：校验和不符");
      error.With("expected", options.expected_checksum).With("actual", actual);
      return error;
    }
  }
  return Ok();
}

fss::Result<void> S3BlobStore::get(const domain::ObjectRef& ref, bytes::ByteSink& sink,
                                   const domain::ByteRange& range) {
  if (ref.container.empty() || ref.key.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "S3：container 与 key 都不能为空");
  }
  SigV4Request request;
  request.method = "GET";
  request.host = signer_.HostFor(ref.container);
  request.path = ObjectPath(ref);
  request.headers.emplace_back("host", request.host);
  const std::string amz_date = AmzDate(clock_.NowEpochSeconds());
  request.headers.emplace_back("x-amz-date", amz_date);
  request.headers.emplace_back("x-amz-content-sha256", crypto::Sha256Hex(""));
  request.payload_sha256_hex = crypto::Sha256Hex("");

  HttpCall call;
  call.method = "GET";
  call.url = options_.effective_scheme() + "://" + request.host + request.path;
  call.download = &sink;
  call.no_total_timeout = true;
  std::string error_body;
  call.error_body = &error_body;
  if (!range.IsWholeObject()) {
    //  `Range` 头不参与签名（与 S3 的实测一致：它不在 SignedHeaders 里）
    const std::string last = range.length == 0
                                 ? ""
                                 : std::to_string(range.offset + range.length - 1);
    call.headers.push_back("Range: bytes=" + std::to_string(range.offset) + "-" + last);
  }
  FSS_TRY(SignAndCollect(signer_, request, amz_date, &call.headers));

  FSS_TRY(status, CurlPerform(options_, call, nullptr));
  if (status >= 400) return MapS3Error(status, error_body, "get");
  return sink.Close();
}

fss::Result<domain::ObjectStat> S3BlobStore::stat(const domain::ObjectRef& ref) {
  if (ref.container.empty() || ref.key.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "S3：container 与 key 都不能为空");
  }
  SigV4Request request;
  request.method = "HEAD";
  request.host = signer_.HostFor(ref.container);
  request.path = ObjectPath(ref);
  request.headers.emplace_back("host", request.host);
  const std::string amz_date = AmzDate(clock_.NowEpochSeconds());
  request.headers.emplace_back("x-amz-date", amz_date);
  request.headers.emplace_back("x-amz-content-sha256", crypto::Sha256Hex(""));
  request.payload_sha256_hex = crypto::Sha256Hex("");

  std::map<std::string, std::string> response_headers;
  HttpCall call;
  call.method = "HEAD";
  call.url = options_.effective_scheme() + "://" + request.host + request.path;
  call.head_only = true;
  call.response_headers = &response_headers;
  FSS_TRY(SignAndCollect(signer_, request, amz_date, &call.headers));

  FSS_TRY(status, CurlPerform(options_, call, nullptr));
  domain::ObjectStat out;
  if (status == 404) {
    out.exists = false;  // 契约：stat(缺失) → Ok + exists == false
    return out;
  }
  if (status >= 400) return MapS3Error(status, "", "stat");

  out.exists = true;
  if (const auto it = response_headers.find("content-length"); it != response_headers.end()) {
    out.size = std::atoll(it->second.c_str());
  }
  if (const auto it = response_headers.find("content-type"); it != response_headers.end()) {
    out.content_type = it->second;
  }
  if (const auto it = response_headers.find("etag"); it != response_headers.end()) {
    out.checksum = TrimQuotes(it->second);
    out.checksum_algorithm = "ETAG";
  }
  if (const auto it = response_headers.find("last-modified"); it != response_headers.end()) {
    out.last_modified_epoch_seconds = ParseHttpDate(it->second);
  }
  return out;
}

fss::Result<void> S3BlobStore::remove(const domain::ObjectRef& ref) {
  if (ref.container.empty() || ref.key.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "S3：container 与 key 都不能为空");
  }
  SigV4Request request;
  request.method = "DELETE";
  request.host = signer_.HostFor(ref.container);
  request.path = ObjectPath(ref);
  request.headers.emplace_back("host", request.host);
  const std::string amz_date = AmzDate(clock_.NowEpochSeconds());
  request.headers.emplace_back("x-amz-date", amz_date);
  request.headers.emplace_back("x-amz-content-sha256", crypto::Sha256Hex(""));
  request.payload_sha256_hex = crypto::Sha256Hex("");

  HttpCall call;
  call.method = "DELETE";
  call.url = options_.effective_scheme() + "://" + request.host + request.path;
  std::string error_body;
  call.error_body = &error_body;
  FSS_TRY(SignAndCollect(signer_, request, amz_date, &call.headers));

  FSS_TRY(status, CurlPerform(options_, call, nullptr));
  //  契约：删除天然幂等 —— 404 也算成功
  if (status == 404) return Ok();
  if (status >= 400) return MapS3Error(status, error_body, "remove");
  return Ok();
}

fss::Result<domain::ObjectStat> S3BlobStore::copy(const domain::ObjectRef& from,
                                                 const domain::ObjectRef& to) {
  if (from.container.empty() || from.key.empty() || to.container.empty() || to.key.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "S3：copy 的 container 与 key 都不能为空");
  }
  SigV4Request request;
  request.method = "PUT";
  request.host = signer_.HostFor(to.container);
  request.path = ObjectPath(to);
  request.headers.emplace_back("host", request.host);
  const std::string amz_date = AmzDate(clock_.NowEpochSeconds());
  request.headers.emplace_back("x-amz-date", amz_date);
  request.headers.emplace_back("x-amz-content-sha256", crypto::Sha256Hex(""));
  request.payload_sha256_hex = crypto::Sha256Hex("");
  //  ★ `x-amz-copy-source` 是 x-amz-* 头，必须**参与签名**（否则服务端重算不一致）
  const std::string copy_source = "/" + from.container + "/" + from.key;
  request.headers.emplace_back("x-amz-copy-source", copy_source);

  HttpCall call;
  call.method = "PUT";
  call.url = options_.effective_scheme() + "://" + request.host + request.path;
  bytes::StringSource empty("");
  call.upload = &empty;
  call.upload_size = 0;
  std::string body;
  call.error_body = &body;
  FSS_TRY(SignAndCollect(signer_, request, amz_date, &call.headers));

  FSS_TRY(status, CurlPerform(options_, call, nullptr));
  if (status >= 400) return MapS3Error(status, body, "copy");
  //  S3 的复制是**同步**的：200 即完成。但 `<CopyObjectResult>` 只给 ETag，
  //  而端口契约要求 `copy` 返回完整 `ObjectStat`（含 size）—— 因此补一次 HEAD。
  //  （另一个选择是让调用方自己 stat，但那会把"复制成功"与"读元数据"耦合到每一处调用点）
  domain::ObjectStat out;
  FSS_TRY(stat_result, stat(to));
  out = stat_result;
  if (!out.checksum_algorithm.empty()) return out;
  if (const auto etag = FindTag(body, "ETag"); etag.has_value()) {
    out.checksum = TrimQuotes(*etag);
    out.checksum_algorithm = "ETAG";
  }
  return out;
}

fss::Result<domain::ListPage> S3BlobStore::list(const std::string& container,
                                                const std::string& prefix,
                                                const std::string& continuation_token,
                                                int limit) {
  if (container.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "S3：容器名不能为空");
  }
  if (limit <= 0) limit = 1000;

  //  查询串参与签名（SigV4 要求按字典序；`SigV4Signer` 已排序）
  std::vector<std::pair<std::string, std::string>> query = {{"list-type", "2"},
                                                            {"max-keys", std::to_string(limit)}};
  if (!prefix.empty()) query.emplace_back("prefix", prefix);
  if (!continuation_token.empty()) {
    query.emplace_back("continuation-token", continuation_token);
  }

  SigV4Request request;
  request.method = "GET";
  request.host = signer_.HostFor(container);
  request.path = BucketPath(container);
  request.query = query;
  request.headers.emplace_back("host", request.host);
  const std::string amz_date = AmzDate(clock_.NowEpochSeconds());
  request.headers.emplace_back("x-amz-date", amz_date);
  request.headers.emplace_back("x-amz-content-sha256", crypto::Sha256Hex(""));
  request.payload_sha256_hex = crypto::Sha256Hex("");

  HttpCall call;
  call.method = "GET";
  call.url = options_.effective_scheme() + "://" + request.host + request.path + "?" +
             [&query] {
               std::string out;
               for (const auto& [key, value] : query) {
                 if (!out.empty()) out.push_back('&');
                 out += key + "=" + value;
               }
               return out;
             }();
  std::string body;
  call.error_body = &body;  // 无 sink → 响应体会收进 error_body（这里就是正常响应体）
  call.capture_limit = 8 * 1024 * 1024;  // 一页 1000 个键 ≈ 200 KiB，64 KiB 不够（P5-D05）
  FSS_TRY(SignAndCollect(signer_, request, amz_date, &call.headers));

  FSS_TRY(status, CurlPerform(options_, call, nullptr));
  if (status >= 400) return MapS3Error(status, body, "list");

  domain::ListPage page;
  const auto truncated = FindTag(body, "IsTruncated");
  page.truncated = truncated.has_value() && *truncated == "true";
  if (page.truncated) {
    page.continuation_token = FindTag(body, "NextContinuationToken").value_or("");
  }
  for (const auto& block : FindAllBlocks(body, "Contents")) {
    domain::ListEntry entry;
    entry.key = FindTag(block, "Key").value_or("");
    if (const auto size = FindTag(block, "Size"); size.has_value()) {
      entry.size = std::atoll(size->c_str());
    }
    if (const auto modified = FindTag(block, "LastModified"); modified.has_value()) {
      entry.last_modified_epoch_seconds = ParseHttpDate(*modified);
    }
    page.entries.push_back(std::move(entry));
  }
  return page;
}

}  // namespace fss::infra
