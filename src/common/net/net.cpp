#include "common/net/net.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>

namespace fss::net {
namespace {

constexpr char kHexUpper[] = "0123456789ABCDEF";

bool IsUnreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '.' || c == '_' || c == '~';
}

// RFC 3986 §3.3 的 sub-delims（路径段里允许保留）
bool IsSubDelim(unsigned char c) {
  switch (c) {
    case '!': case '$': case '&': case '\'': case '(': case ')':
    case '*': case '+': case ',': case ';': case '=':
      return true;
    default:
      return false;
  }
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string ToLowerAscii(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

void AppendEncoded(std::string& out, std::string_view text, bool keep_sub_delims) {
  out.reserve(out.size() + text.size());
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    const bool keep = IsUnreserved(c) || (keep_sub_delims && (IsSubDelim(c) || c == ':' || c == '@'));
    if (keep) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHexUpper[c >> 4];
      out += kHexUpper[c & 0x0F];
    }
  }
}

bool ParsePort(std::string_view text, std::uint16_t* out) {
  if (text.empty()) return false;
  std::uint32_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<std::uint32_t>(c - '0');
    if (value > 65535) return false;
  }
  if (value == 0) return false;  // 端口 0 没有意义，视为非法
  *out = static_cast<std::uint16_t>(value);
  return true;
}

}  // namespace

std::string PercentEncode(std::string_view text) {
  std::string out;
  AppendEncoded(out, text, /*keep_sub_delims=*/false);
  return out;
}

std::string PercentEncodePathSegment(std::string_view text) {
  std::string out;
  AppendEncoded(out, text, /*keep_sub_delims=*/true);
  return out;
}

Result<std::string> PercentDecode(std::string_view text, bool plus_as_space) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '%') {
      if (i + 2 >= text.size()) {
        return Err(ErrorKind::kInvalidArgument,
                   "百分号编码不完整：'" + std::string(text.substr(i)) + "'");
      }
      const int hi = HexValue(text[i + 1]);
      const int lo = HexValue(text[i + 2]);
      if (hi < 0 || lo < 0) {
        return Err(ErrorKind::kInvalidArgument,
                   "百分号编码非法：'%" + std::string(text.substr(i + 1, 2)) + "'");
      }
      out += static_cast<char>((hi << 4) | lo);
      i += 2;
      continue;
    }
    if (c == '+' && plus_as_space) {
      out += ' ';
      continue;
    }
    out += c;
  }
  return out;
}

Result<QueryParams> ParseQuery(std::string_view query, bool plus_as_space) {
  QueryParams params;
  // 允许调用方把前导 '?' 一起传进来
  if (!query.empty() && query.front() == '?') query.remove_prefix(1);
  if (query.empty()) return params;

  std::size_t pos = 0;
  while (pos <= query.size()) {
    const auto amp = query.find('&', pos);
    const auto end = amp == std::string_view::npos ? query.size() : amp;
    const std::string_view pair = query.substr(pos, end - pos);
    pos = amp == std::string_view::npos ? query.size() + 1 : amp + 1;
    if (pair.empty()) continue;  // `a=1&&b=2` 里的空项忽略

    const auto eq = pair.find('=');
    const std::string_view raw_key = eq == std::string_view::npos ? pair : pair.substr(0, eq);
    const std::string_view raw_value = eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1);

    FSS_TRY(key, PercentDecode(raw_key, plus_as_space));
    FSS_TRY(value, PercentDecode(raw_value, plus_as_space));
    params.emplace_back(std::move(key), std::move(value));
  }
  return params;
}

std::string BuildQuery(const QueryParams& params) {
  std::string out;
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i) out += '&';
    out += PercentEncode(params[i].first);
    out += '=';
    out += PercentEncode(params[i].second);
  }
  return out;
}

std::optional<std::string> GetQueryParam(const QueryParams& params, std::string_view key) {
  for (const auto& [k, v] : params) {
    if (k == key) return v;
  }
  return std::nullopt;
}

std::vector<std::string> GetAllQueryParams(const QueryParams& params, std::string_view key) {
  std::vector<std::string> out;
  for (const auto& [k, v] : params) {
    if (k == key) out.push_back(v);
  }
  return out;
}

Result<std::int64_t> GetIntQueryParam(const QueryParams& params, std::string_view key) {
  const auto text = GetQueryParam(params, key);
  if (!text.has_value()) {
    return Err(ErrorKind::kInvalidArgument, "缺少查询参数 '" + std::string(key) + "'");
  }
  const std::string& s = *text;
  if (s.empty()) {
    return Err(ErrorKind::kInvalidArgument, "查询参数 '" + std::string(key) + "' 为空");
  }
  std::int64_t value = 0;
  // 整串必须是数字：`from_chars` 会告诉我们首个未解析位置，借此拒绝 "12abc" / " 12" / "+12"
  const auto* first = s.data();
  const auto* last = s.data() + s.size();
  const auto res = std::from_chars(first, last, value);
  if (res.ec != std::errc{} || res.ptr != last) {
    return Err(ErrorKind::kInvalidArgument,
               "查询参数 '" + std::string(key) + "' 不是合法整数：'" + s + "'");
  }
  return value;
}

// -----------------------------------------------------------------------------
//  Url
// -----------------------------------------------------------------------------
std::uint16_t Url::EffectivePort() const {
  if (has_port) return port;
  if (scheme == "http") return 80;
  if (scheme == "https") return 443;
  return 0;
}

std::string BracketIfIpv6(std::string_view host) {
  if (host.find(':') == std::string_view::npos) return std::string(host);
  if (!host.empty() && host.front() == '[') return std::string(host);
  return "[" + std::string(host) + "]";
}

std::string Url::Authority() const {
  std::string out;
  if (!userinfo.empty()) {
    out += userinfo;
    out += '@';
  }
  out += BracketIfIpv6(host);
  // 只在"显式端口，或显式端口与默认端口不同"时写出端口：
  // 签名（SigV4 的 canonical host）对 `h:443` 与 `h` 的期望不同，必须由调用方决定，
  // 所以这里严格"有 has_port 才写"。
  if (has_port) {
    out += ':';
    out += std::to_string(port);
  }
  return out;
}

std::string Url::PathAndQuery() const {
  std::string out = path.empty() ? "/" : path;
  if (!query.empty()) {
    out += '?';
    out += query;
  }
  return out;
}

std::string Url::ToString(bool with_fragment) const {
  std::string out;
  out += scheme;
  out += "://";
  out += Authority();
  out += path.empty() ? "/" : path;
  if (!query.empty()) {
    out += '?';
    out += query;
  }
  if (with_fragment && has_fragment) {
    out += '#';
    out += fragment;
  }
  return out;
}

Result<Url> ParseUrl(std::string_view absolute_url) {
  Url url;
  const std::string_view text = absolute_url;

  const auto scheme_end = text.find("://");
  if (scheme_end == std::string_view::npos) {
    return Err(ErrorKind::kInvalidArgument, "URL 缺少 scheme：'" + std::string(text) + "'");
  }
  url.scheme = ToLowerAscii(text.substr(0, scheme_end));
  if (url.scheme != "http" && url.scheme != "https") {
    return Err(ErrorKind::kInvalidArgument, "不支持的 scheme：'" + url.scheme + "'");
  }

  std::string_view rest = text.substr(scheme_end + 3);
  // fragment 先切掉（它不影响 host/path/query 的解析）
  if (const auto hash = rest.find('#'); hash != std::string_view::npos) {
    url.fragment = std::string(rest.substr(hash + 1));
    url.has_fragment = true;
    rest = rest.substr(0, hash);
  }
  // query
  std::string_view authority_and_path = rest;
  if (const auto q = rest.find('?'); q != std::string_view::npos) {
    url.query = std::string(rest.substr(q + 1));
    authority_and_path = rest.substr(0, q);
  }
  // path
  std::string_view authority = authority_and_path;
  if (const auto slash = authority_and_path.find('/'); slash != std::string_view::npos) {
    url.path = std::string(authority_and_path.substr(slash));
    authority = authority_and_path.substr(0, slash);
  } else {
    url.path = "/";
  }
  if (authority.empty()) {
    return Err(ErrorKind::kInvalidArgument, "URL 缺少 host：'" + std::string(text) + "'");
  }

  // userinfo
  if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
    url.userinfo = std::string(authority.substr(0, at));
    authority = authority.substr(at + 1);
  }

  // host[:port]；IPv6 形如 [::1]:8080
  if (!authority.empty() && authority.front() == '[') {
    const auto close = authority.find(']');
    if (close == std::string_view::npos) {
      return Err(ErrorKind::kInvalidArgument, "IPv6 地址缺少 ']'：'" + std::string(authority) + "'");
    }
    url.host = ToLowerAscii(authority.substr(1, close - 1));
    const auto tail = authority.substr(close + 1);
    if (!tail.empty()) {
      if (tail.front() != ':') {
        return Err(ErrorKind::kInvalidArgument, "IPv6 地址后的字符非法：'" + std::string(tail) + "'");
      }
      if (!ParsePort(tail.substr(1), &url.port)) {
        return Err(ErrorKind::kInvalidArgument, "端口非法：'" + std::string(tail.substr(1)) + "'");
      }
      url.has_port = true;
    }
  } else {
    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
      url.host = ToLowerAscii(authority);
    } else {
      url.host = ToLowerAscii(authority.substr(0, colon));
      if (!ParsePort(authority.substr(colon + 1), &url.port)) {
        return Err(ErrorKind::kInvalidArgument,
                   "端口非法：'" + std::string(authority.substr(colon + 1)) + "'");
      }
      url.has_port = true;
    }
  }
  if (url.host.empty()) {
    return Err(ErrorKind::kInvalidArgument, "URL 缺少 host：'" + std::string(text) + "'");
  }
  // host 里不允许出现空白或控制字符（曾见过把整个 header 拼进 URL 的调用方）
  for (const char c : url.host) {
    if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) {
      return Err(ErrorKind::kInvalidArgument, "host 含空白/控制字符：'" + url.host + "'");
    }
  }
  return url;
}

std::string AppendPathSegment(std::string_view base_url, std::string_view segment) {
  std::string out(base_url);
  // 去掉 base 末尾多余的 '/'，避免出现 '//'
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  out += '/';
  out += PercentEncodePathSegment(segment);
  return out;
}

}  // namespace fss::net
