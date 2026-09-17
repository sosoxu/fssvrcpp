// C1.x：common/net —— 百分号编码、查询串、URL 解析
//
// 编码/解码是**签名正确性**的前置（SigV4 的 canonical URI 与最终 URL 必须逐字节一致），
// 所以每条"非法输入被拒"的断言都配了"合法输入被接受"的对照（R16）。
#include <catch2/catch.hpp>

#include "common/net/net.h"

#include <string>
#include <vector>

using fss::net::ParseQuery;
using fss::net::ParseUrl;
using fss::net::PercentDecode;
using fss::net::PercentEncode;
using fss::net::PercentEncodePathSegment;

TEST_CASE("★ 百分号编码：unreserved 原样保留，其余 %XX（大写）", "[phase1][net]") {
  // RFC 3986 §2.3：unreserved = ALPHA / DIGIT / "-" / "." / "_" / "~"
  REQUIRE(PercentEncode("abcXYZ019-._~") == "abcXYZ019-._~");

  SECTION("保留字符与空格被编码") {
    REQUIRE(PercentEncode(" ") == "%20");
    REQUIRE(PercentEncode("a b") == "a%20b");
    REQUIRE(PercentEncode("a/b") == "a%2Fb");
    REQUIRE(PercentEncode("a?b") == "a%3Fb");
    REQUIRE(PercentEncode("a&b=c") == "a%26b%3Dc");
    REQUIRE(PercentEncode("a#b") == "a%23b");
    REQUIRE(PercentEncode("100%") == "100%25");
  }

  SECTION("★ `+` 必须编码成 %2B（不能当成空格，也不能原样留）") {
    // 这是与 form 编码最容易混淆的地方：URL 里 `+` 是**字面加号**
    REQUIRE(PercentEncode("+") == "%2B");
    REQUIRE(PercentEncode("a+b") == "a%2Bb");
  }

  SECTION("UTF-8 按字节编码（不是按码点）") {
    REQUIRE(PercentEncode("中") == "%E4%B8%AD");
    REQUIRE(PercentEncode("é") == "%C3%A9");
  }

  SECTION("十六进制用大写（RFC 3986 §6.2.2.1 的规范化建议）") {
    REQUIRE(PercentEncode("\xe4\xb8\xad") == "%E4%B8%AD");
    REQUIRE(PercentEncode("~") == "~");  // 不写成 %7E
  }

  SECTION("控制字符与 NUL") {
    REQUIRE(PercentEncode(std::string("a\0b", 3)) == "a%00b");
    REQUIRE(PercentEncode("\n") == "%0A");
    REQUIRE(PercentEncode("\x7f") == "%7F");
  }

  SECTION("路径段模式额外保留 sub-delims，但 `/` 必须编码") {
    REQUIRE(PercentEncodePathSegment("a-b_c.d~e") == "a-b_c.d~e");
    REQUIRE(PercentEncodePathSegment("a:b@c") == "a:b@c");
    REQUIRE(PercentEncodePathSegment("a!$&'()*+,;=b") == "a!$&'()*+,;=b");
    // ★ 段里出现 '/' 必须编码，否则一个段会被劈成两段（路径注入）
    REQUIRE(PercentEncodePathSegment("a/b") == "a%2Fb");
    REQUIRE(PercentEncodePathSegment("..") == "..");  // 字符本身合法，语义由上层校验
    REQUIRE(PercentEncodePathSegment(" ") == "%20");
  }
}

TEST_CASE("★ 百分号解码：非法序列必须报错，不能'尽力而为'", "[phase1][net]") {
  REQUIRE(PercentDecode("a%20b").value() == "a b");
  REQUIRE(PercentDecode("%E4%B8%AD").value() == "中");
  REQUIRE(PercentDecode("").value() == "");
  REQUIRE(PercentDecode("plain").value() == "plain");
  // 小写十六进制也要能解（客户端会这么写）
  REQUIRE(PercentDecode("%e4%b8%ad").value() == "中");

  const std::vector<std::string> bad = {"%", "%2", "%zz", "a%", "a%2", "50%off", "%GG"};
  for (const auto& s : bad) {
    INFO("input: " << s);
    const auto r = PercentDecode(s);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().kind() == fss::ErrorKind::kInvalidArgument);
  }

  SECTION("`+` 默认**不**当空格（严格 RFC 3986）") {
    REQUIRE(PercentDecode("a+b").value() == "a+b");
    REQUIRE(PercentDecode("a+b", /*plus_as_space=*/true).value() == "a b");
  }

  SECTION("往返：编码后再解码必须还原（含任意字节）") {
    const std::string raw = "a b/c?d=e&f+g#h%i\x01\x7f中";
    REQUIRE(PercentDecode(PercentEncode(raw)).value() == raw);
  }
}

TEST_CASE("查询串：解析保序、保留重复键、宽容缺值", "[phase1][net]") {
  SECTION("基本解析") {
    const auto q = ParseQuery("expiryTime=3600&pageNum=1&pageSize=20");
    REQUIRE(q.ok());
    REQUIRE(q.value().size() == 3);
    REQUIRE(fss::net::GetQueryParam(q.value(), "expiryTime").value() == "3600");
    REQUIRE(fss::net::GetQueryParam(q.value(), "pageNum").value() == "1");
    REQUIRE(fss::net::GetQueryParam(q.value(), "pageSize").value() == "20");
    REQUIRE_FALSE(fss::net::GetQueryParam(q.value(), "PageNum").has_value());  // 大小写敏感
  }

  SECTION("重复键全部保留且保持顺序") {
    const auto q = ParseQuery("a=1&a=2&b=3&a=4");
    REQUIRE(q.ok());
    REQUIRE(q.value().size() == 4);
    REQUIRE(fss::net::GetQueryParam(q.value(), "a").value() == "1");  // 取第一个
    const auto all = fss::net::GetAllQueryParams(q.value(), "a");
    REQUIRE(all == std::vector<std::string>{"1", "2", "4"});
  }

  SECTION("宽容：无 `=`、空值、空项、前导 `?`、前导 `&`") {
    const auto q = ParseQuery("?a&b=&=c&&d=1");
    REQUIRE(q.ok());
    REQUIRE(q.value().size() == 4);
    REQUIRE(fss::net::GetQueryParam(q.value(), "a").value() == "");
    REQUIRE(fss::net::GetQueryParam(q.value(), "b").value() == "");
    REQUIRE(fss::net::GetQueryParam(q.value(), "").value() == "c");
    REQUIRE(fss::net::GetQueryParam(q.value(), "d").value() == "1");
  }

  SECTION("解码发生在解析过程中（%XX 与 `+`）") {
    const auto q = ParseQuery("name=%E4%B8%AD%E6%96%87&q=a+b");
    REQUIRE(q.ok());
    REQUIRE(fss::net::GetQueryParam(q.value(), "name").value() == "中文");
    REQUIRE(fss::net::GetQueryParam(q.value(), "q").value() == "a b");  // form 约定
  }

  SECTION("关闭 plus_as_space 时 `+` 保持字面") {
    const auto q = ParseQuery("q=a+b", /*plus_as_space=*/false);
    REQUIRE(fss::net::GetQueryParam(q.value(), "q").value() == "a+b");
  }

  SECTION("非法编码 → 报错（不是静默替换）") {
    const auto q = ParseQuery("name=%zz");
    REQUIRE_FALSE(q.ok());
    REQUIRE(q.error().kind() == fss::ErrorKind::kInvalidArgument);
  }

  SECTION("空查询串 → 空结果") {
    REQUIRE(ParseQuery("").value().empty());
    REQUIRE(ParseQuery("?").value().empty());
  }
}

TEST_CASE("查询串：构造与整型取值", "[phase1][net]") {
  SECTION("构造时严格编码，空格用 %20（不是 +）") {
    const fss::net::QueryParams params{{"a b", "c d"}, {"k", "1+2"}, {"中", "文"}};
    const auto built = fss::net::BuildQuery(params);
    REQUIRE(built == "a%20b=c%20d&k=1%2B2&%E4%B8%AD=%E6%96%87");
  }

  SECTION("构造 → 解析 往返一致") {
    const fss::net::QueryParams params{{"expiryTime", "3600"}, {"x", "a b&c=d"}, {"x", "2"}};
    const auto parsed = ParseQuery(fss::net::BuildQuery(params));
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value() == params);
  }

  SECTION("整型参数：整串必须是数字") {
    const auto q = ParseQuery("n=42&neg=-7&big=9223372036854775807&bad=12abc&empty=&sp=%2012");
    REQUIRE(q.ok());
    REQUIRE(fss::net::GetIntQueryParam(q.value(), "n").value() == 42);
    REQUIRE(fss::net::GetIntQueryParam(q.value(), "neg").value() == -7);
    REQUIRE(fss::net::GetIntQueryParam(q.value(), "big").value() ==
            std::numeric_limits<std::int64_t>::max());
    // 这些都必须被拒（`from_chars` + 全串校验）
    REQUIRE_FALSE(fss::net::GetIntQueryParam(q.value(), "bad").ok());
    REQUIRE_FALSE(fss::net::GetIntQueryParam(q.value(), "empty").ok());
    REQUIRE_FALSE(fss::net::GetIntQueryParam(q.value(), "sp").ok());
    // 缺参数也要报错（不能默认成 0 —— 那会把"没传"变成"传了 0"）
    REQUIRE_FALSE(fss::net::GetIntQueryParam(q.value(), "absent").ok());
  }
}

TEST_CASE("URL 解析：scheme/host 归一化、端口、路径、查询、片段", "[phase1][net]") {
  SECTION("最简形式") {
    const auto u = ParseUrl("https://storage.example.com");
    REQUIRE(u.ok());
    REQUIRE(u.value().scheme == "https");
    REQUIRE(u.value().host == "storage.example.com");
    REQUIRE_FALSE(u.value().has_port);
    REQUIRE(u.value().EffectivePort() == 443);
    REQUIRE(u.value().path == "/");
    REQUIRE(u.value().ToString() == "https://storage.example.com/");
    REQUIRE(u.value().IsSecure());
  }

  SECTION("显式端口 + 路径 + 查询 + 片段") {
    const auto u = ParseUrl("http://localhost:8080/api/file/v2/files/abc?x=1#frag");
    REQUIRE(u.ok());
    REQUIRE(u.value().host == "localhost");
    REQUIRE(u.value().port == 8080);
    REQUIRE(u.value().has_port);
    REQUIRE(u.value().EffectivePort() == 8080);
    REQUIRE(u.value().path == "/api/file/v2/files/abc");
    REQUIRE(u.value().query == "x=1");
    REQUIRE(u.value().PathAndQuery() == "/api/file/v2/files/abc?x=1");
    REQUIRE(u.value().fragment == "frag");
    REQUIRE(u.value().ToString(false) == "http://localhost:8080/api/file/v2/files/abc?x=1");
  }

  SECTION("scheme 与 host 大小写归一为小写（签名与 DNS 都按小写处理）") {
    const auto u = ParseUrl("HTTPS://Storage.EXAMPLE.Com/Path");
    REQUIRE(u.ok());
    REQUIRE(u.value().scheme == "https");
    REQUIRE(u.value().host == "storage.example.com");
    REQUIRE(u.value().path == "/Path");  // ★ 路径大小写敏感，不能动
  }

  SECTION("userinfo") {
    const auto u = ParseUrl("https://user:pass@host:9000/p");
    REQUIRE(u.ok());
    REQUIRE(u.value().userinfo == "user:pass");
    REQUIRE(u.value().host == "host");
    REQUIRE(u.value().port == 9000);
    REQUIRE(u.value().Authority() == "user:pass@host:9000");
  }

  SECTION("IPv6 字面量（带方括号与端口）") {
    const auto a = ParseUrl("http://[::1]:8080/x");
    REQUIRE(a.ok());
    REQUIRE(a.value().host == "::1");
    REQUIRE(a.value().port == 8080);
    REQUIRE(a.value().Authority() == "[::1]:8080");
    REQUIRE(a.value().ToString() == "http://[::1]:8080/x");

    const auto b = ParseUrl("http://[2001:db8::1]/y");
    REQUIRE(b.ok());
    REQUIRE(b.value().host == "2001:db8::1");
    REQUIRE_FALSE(b.value().has_port);
    REQUIRE(b.value().ToString() == "http://[2001:db8::1]/y");
  }

  SECTION("★ 默认端口：不显式写就不出现在 Authority/ToString 里") {
    // 这一点很关键：SigV4 的 canonical host 对 `h:443` 与 `h` 的期望不同，
    // 所以"是否写了端口"必须被保留，而不是被规范化掉。
    const auto u = ParseUrl("https://h/p");
    REQUIRE(u.value().Authority() == "h");
    REQUIRE(u.value().ToString() == "https://h/p");
    const auto u2 = ParseUrl("https://h:443/p");
    REQUIRE(u2.value().Authority() == "h:443");   // 显式写了就保留
    REQUIRE(u2.value().EffectivePort() == 443);
  }
}

TEST_CASE("URL 解析：非法输入被拒（含对照）", "[phase1][net]") {
  const std::vector<std::string> bad = {
      "",                              // 空
      "example.com/path",              // 缺 scheme
      "ftp://example.com/",            // 不支持的 scheme
      "https://",                      // 缺 host
      "https:///path",                 // 空 authority
      "https://host:0/",               // 端口 0
      "https://host:99999/",           // 端口越界
      "https://host:abc/",             // 端口非数字
      "https://ho st/",                // host 含空格
      "https://[::1/",                 // IPv6 缺 ']'
      "https://[::1]x/",               // ']' 后既不是 ':' 也不是结束
  };
  for (const auto& s : bad) {
    INFO("url: " << s);
    const auto r = ParseUrl(s);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().kind() == fss::ErrorKind::kInvalidArgument);
  }

  SECTION("★ 对照：同一批形状的合法输入必须被接受") {
    for (const char* s : {"http://example.com/", "https://example.com:8443/", "http://[::1]/",
                          "https://user@host/p?a=1"}) {
      INFO("url: " << s);
      REQUIRE(ParseUrl(s).ok());
    }
  }

  SECTION("控制字符注入：host 里出现 CR/LF 必须被拒（响应拆分/头注入的入口）") {
    REQUIRE_FALSE(ParseUrl("https://ho\r\nst/").ok());
    REQUIRE_FALSE(ParseUrl("https://ho\tst/").ok());
  }
}

TEST_CASE("AppendPathSegment：段编码防路径注入", "[phase1][net]") {
  REQUIRE(fss::net::AppendPathSegment("https://h/api/file/v2/files", "abc") ==
          "https://h/api/file/v2/files/abc");
  // 末尾多余 '/' 被归一
  REQUIRE(fss::net::AppendPathSegment("https://h/api/", "abc") == "https://h/api/abc");

  SECTION("★ 段里的 '/' 与 '..' 必须被编码，不能改变路径结构") {
    // 若不编码，`a/b` 会变成两层路径；`../x` 会越权到上一级
    REQUIRE(fss::net::AppendPathSegment("https://h/api", "a/b") == "https://h/api/a%2Fb");
    REQUIRE(fss::net::AppendPathSegment("https://h/api", "../x") == "https://h/api/..%2Fx");
    REQUIRE(fss::net::AppendPathSegment("https://h/api", "a b") == "https://h/api/a%20b");
    // `?` 与 `#` 必须编码（否则会改变 URL 结构）；而 `=` `:` `@` 等属于 RFC 3986 的
    // sub-delims/pchar，在**路径段**里本来就合法，因此**刻意保留**（不重复编码）。
    // 这条取舍要留着：将来 P5 做 SigV4 canonical URI 时，编码规则必须与这里一致。
    REQUIRE(fss::net::AppendPathSegment("https://h/api", "a?b=1") == "https://h/api/a%3Fb=1");
    REQUIRE(fss::net::AppendPathSegment("https://h/api", "a#f") == "https://h/api/a%23f");
    REQUIRE(fss::net::AppendPathSegment("https://h/api", "k:e=q@r") == "https://h/api/k:e=q@r");
  }

  SECTION("对照：合法段不被过度编码（否则 URL 会难看且可能影响签名）") {
    REQUIRE(fss::net::AppendPathSegment("https://h/api", "a-b_c.d~e") == "https://h/api/a-b_c.d~e");
  }
}
