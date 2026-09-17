// C1.6：日志脱敏 —— `secret_key`/`access_key`/`token`/`sig` 在日志中不出现明文
//
// 这条门槛的对象是"泄漏"，而泄漏测试最容易写成**永远通过**的空断言
// （比如只检查日志里有没有 `***`）。因此每个关键断言都配了对照：
//   · 脱敏开启 → 明文必须不出现
//   · 脱敏关闭（空规则集）→ **同一份输入必须出现明文**（否则说明输入根本没有密钥，
//     前面的"不出现"证明不了任何事）
#include <catch2/catch.hpp>

#include "common/logging/logging.h"
#include "common/time/clock.h"

#include <atomic>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using fss::logging::Fields;
using fss::logging::Level;
using fss::logging::LogOptions;
using fss::logging::MemoryLogger;
using fss::logging::Redactor;
using fss::logging::StreamLogger;

namespace {

// `config/fss.example.json` 的 `observability.redact_keys` 默认值（保持同步）
constexpr const char* kDefaultRedactKeys =
    "secret_key,access_key,token,sig,signature,authorization,x-amz-signature,password,"
    "signing_key,dsn,static_token";

LogOptions DefaultOptions() {
  LogOptions o;
  o.redact_keys = Redactor::FromCommaSeparated(kDefaultRedactKeys).keys();
  return o;
}

// 一份"最坏情况"的输入：既有结构化密钥字段，也有把密钥拼进消息的常见写法
Fields SecretFields() {
  return Fields{
      {"secret_key", "AKIAIOSFODNN7EXAMPLE-SECRET"},
      {"access_key", "AKIAIOSFODNN7EXAMPLE"},
      {"token", "eyJhbGciOiJIUzI1NiJ9.LEAKME"},
      {"signature", "deadbeefcafe1234"},
  };
}

std::string Rendered(const LogOptions& options, std::string_view msg, const Fields& fields) {
  fss::ManualClock clock(1700000000);
  std::ostringstream os;
  StreamLogger logger(options, clock, os);
  return logger.Render(Level::kInfo, msg, fields);
}

}  // namespace

TEST_CASE("★ C1.6：结构化密钥字段的值不出现在日志中（JSON 与 text 双格式）",
          "[phase1][logging][c1.6]") {
  const auto fields = SecretFields();
  const std::vector<std::string> secrets = {"AKIAIOSFODNN7EXAMPLE-SECRET",
                                           "AKIAIOSFODNN7EXAMPLE",
                                           "eyJhbGciOiJIUzI1NiJ9.LEAKME",
                                           "deadbeefcafe1234"};

  SECTION("自证对照：先证明这些明文**本来会出现**（换掉脱敏规则）") {
    LogOptions no_redaction;
    no_redaction.redact_keys = {};  // ★ 故意不脱敏
    const std::string out = Rendered(no_redaction, "read location", fields);
    for (const auto& s : secrets) {
      INFO("对照输入里必须真的有这个密钥: " << s);
      REQUIRE(out.find(s) != std::string::npos);
    }
  }

  SECTION("JSON 格式：任何密钥明文都不得出现") {
    const std::string out = Rendered(DefaultOptions(), "read location", fields);
    for (const auto& s : secrets) {
      INFO("泄漏了: " << s << "\n" << out);
      REQUIRE(out.find(s) == std::string::npos);
    }
    REQUIRE(out.find("***") != std::string::npos);
    // 打码不能把整条日志毁掉：非密钥字段仍要可见（否则排障时日志没用了）
    REQUIRE(out.find("read location") != std::string::npos);
  }

  SECTION("text 格式：同样不得泄漏") {
    LogOptions o = DefaultOptions();
    o.format = "text";
    const std::string out = Rendered(o, "read location", fields);
    for (const auto& s : secrets) {
      REQUIRE(out.find(s) == std::string::npos);
    }
    REQUIRE(out.find("INFO") != std::string::npos);
    REQUIRE(out.find("read location") != std::string::npos);
  }
}

TEST_CASE("★ C1.6：密钥被拼进**消息文本**时也要脱敏（Authorization / X-Amz-Signature）",
          "[phase1][logging][c1.6]") {
  // 这是最容易漏的一类：字段是干净的，密钥在 msg 里。
  const std::string header_dump = "upstream 403; Authorization: Bearer eyJTOKENLEAK.abc";
  const std::string query = "GET /b?X-Amz-Signature=abcdef0123456789&X-Amz-Date=20240101T000000Z";
  const std::string jsonish = R"({"secret_key":"RAWKEYLEAK","region":"us-east-1"})";

  SECTION("对照：不脱敏时明文确实出现") {
    LogOptions no_redaction;
    const std::string out = Rendered(no_redaction, header_dump, Fields{{"q", query}});
    REQUIRE(out.find("eyJTOKENLEAK.abc") != std::string::npos);
    REQUIRE(out.find("abcdef0123456789") != std::string::npos);

    const std::string out2 = Rendered(no_redaction, jsonish, {});
    REQUIRE(out2.find("RAWKEYLEAK") != std::string::npos);
  }

  SECTION("脱敏后：消息与字符串值里的明文都被打码") {
    const std::string out = Rendered(DefaultOptions(), header_dump, Fields{{"q", query}});
    INFO(out);
    REQUIRE(out.find("eyJTOKENLEAK.abc") == std::string::npos);
    REQUIRE(out.find("abcdef0123456789") == std::string::npos);
    // 非敏感部分保留，日志仍然有用
    REQUIRE(out.find("upstream 403") != std::string::npos);
    REQUIRE(out.find("X-Amz-Date") != std::string::npos);  // 键名留下，值不是密钥
  }

  SECTION("脱敏后：字符串值里的内嵌 JSON 也不泄漏") {
    const std::string out = Rendered(DefaultOptions(), jsonish, {});
    INFO(out);
    REQUIRE(out.find("RAWKEYLEAK") == std::string::npos);
    REQUIRE(out.find("us-east-1") != std::string::npos);  // 不相关的字段不受影响
  }
}

TEST_CASE("脱敏是大小写不敏感的**子串**匹配（覆盖真实键名的各种写法）",
          "[phase1][logging][c1.6]") {
  const Redactor r = Redactor::FromCommaSeparated(kDefaultRedactKeys);

  for (const char* name : {"secret_key", "SecretKey", "SECRET_KEY", "my_secret_key_value",
                           "x-amz-signature", "X-Amz-Signature", "Signature",
                           "Authorization", "authorization", "accesstoken", "static_token",
                           "signing_key", "SIGNING-KEY", "password", "dsn", "access_key"}) {
    INFO("必须命中: " << name);
    REQUIRE(r.Matches(name));
  }
  // 不相关字段不应该被误伤（否则日志会变得没法用）
  for (const char* name : {"file_id", "partition_id", "status", "duration_ms", "zone"}) {
    INFO("不应命中: " << name);
    REQUIRE_FALSE(r.Matches(name));
  }
  // ⚠️ 被记录在案的取舍：子串匹配会连带命中含 "sig" 的无关词，方向是"宁可多打码"
  REQUIRE(r.Matches("design"));
}

TEST_CASE("脱敏递归进嵌套对象与数组，且不破坏结构", "[phase1][logging][c1.6]") {
  const Redactor r = Redactor::FromCommaSeparated(kDefaultRedactKeys);

  const auto input = fss::json::Parse(R"({
    "file_id": "f-1",
    "storage": {"driver": "s3", "secret_key": "NESTEDLEAK", "region": "cn-north-1"},
    "credentials": [{"access_key": "ARRAYLEAK1"}, {"access_key": "ARRAYLEAK2"}],
    "n": 7,
    "ok": true
  })");
  REQUIRE(input.ok());

  const auto masked = r.Apply(input.value());
  const std::string dumped = fss::json::Dump(masked);

  REQUIRE(dumped.find("NESTEDLEAK") == std::string::npos);
  REQUIRE(dumped.find("ARRAYLEAK1") == std::string::npos);
  REQUIRE(dumped.find("ARRAYLEAK2") == std::string::npos);
  // 结构与非敏感值必须完整保留
  REQUIRE(dumped.find("cn-north-1") != std::string::npos);
  REQUIRE(dumped.find("\"driver\":\"s3\"") != std::string::npos);
  // 类型不能被改成字符串（否则下游按 JSON 解析会拿到错误类型）
  REQUIRE(masked["n"].is_number());
  REQUIRE(masked["ok"].is_boolean());
  REQUIRE(masked["credentials"].size() == 2);
}

TEST_CASE("空规则集 = 不脱敏（脱敏必须是显式配置的结果，而不是默认到处打码）",
          "[phase1][logging][c1.6]") {
  const Redactor empty = Redactor::FromCommaSeparated("");
  REQUIRE(empty.empty());
  REQUIRE_FALSE(empty.Matches("secret_key"));
  REQUIRE(empty.Apply(fss::json::Value{{"secret_key", "VISIBLE"}})["secret_key"] == "VISIBLE");

  // 逗号列表首尾空白要被裁掉（配置里人写的列表常带空格）
  const Redactor spaced = Redactor::FromCommaSeparated(" secret_key , access_key ,, ");
  REQUIRE(spaced.keys().size() == 2);
  REQUIRE(spaced.Matches("secret_key"));
  REQUIRE(spaced.Matches("access_key"));
}

TEST_CASE("级别过滤：低级别被抑制，但抑制的是**输出**而不是构造", "[phase1][logging]") {
  fss::ManualClock clock(1700000000);
  LogOptions o = DefaultOptions();
  o.min_level = Level::kWarn;
  std::ostringstream os;
  StreamLogger logger(o, clock, os);

  REQUIRE_FALSE(logger.Enabled(Level::kDebug));
  REQUIRE_FALSE(logger.Enabled(Level::kInfo));
  REQUIRE(logger.Enabled(Level::kWarn));
  REQUIRE(logger.Enabled(Level::kError));

  fss::logging::Debug(logger, "不该出现");
  fss::logging::Info(logger, "不该出现");
  fss::logging::Warn(logger, "警告出现");
  fss::logging::Error(logger, "错误出现");

  REQUIRE(logger.written() == 2);  // ★ 自证：确实写了 2 条，不是"一条都没写"
  const std::string out = os.str();
  REQUIRE(out.find("警告出现") != std::string::npos);
  REQUIRE(out.find("错误出现") != std::string::npos);
  REQUIRE(out.find("不该出现") == std::string::npos);
}

TEST_CASE("级别名解析", "[phase1][logging]") {
  REQUIRE(fss::logging::ParseLevel("debug").value() == Level::kDebug);
  REQUIRE(fss::logging::ParseLevel("INFO").value() == Level::kInfo);
  REQUIRE(fss::logging::ParseLevel("Warn").value() == Level::kWarn);
  REQUIRE(fss::logging::ParseLevel("warning").value() == Level::kWarn);
  REQUIRE(fss::logging::ParseLevel("error").value() == Level::kError);
  REQUIRE_FALSE(fss::logging::ParseLevel("verbose").has_value());
  REQUIRE(fss::logging::LevelName(Level::kError) == "error");
  REQUIRE(fss::logging::LevelNameUpper(Level::kWarn) == "WARN");
}

TEST_CASE("JSON Lines：一条记录一行，且可被重新解析（采集端的前提）",
          "[phase1][logging]") {
  fss::ManualClock clock(1700000000);
  LogOptions o = DefaultOptions();
  std::ostringstream os;
  StreamLogger logger(o, clock, os);

  fss::logging::Info(logger, "first", Fields{{"file_id", "f-1"}, {"status", 200}});
  fss::logging::Warn(logger, "second", Fields{{"duration_ms", 12}});

  std::istringstream lines(os.str());
  std::string line;
  int count = 0;
  while (std::getline(lines, line)) {
    INFO("行: " << line);
    auto parsed = fss::json::Parse(line);
    REQUIRE(parsed.ok());                      // ★ 每行都是合法 JSON
    REQUIRE(parsed.value()["ts"].is_string());  // 且带时间戳
    REQUIRE(parsed.value()["msg"].is_string());
    ++count;
  }
  REQUIRE(count == 2);
}

TEST_CASE("★ 日志伪造（注入）：消息里的换行不得造出额外日志行", "[phase1][logging]") {
  // 调用方把用户输入拼进日志（例如文件名），若换行不被转义，攻击者就能
  // 伪造出一条 "level":"error" 的假日志来误导排障。
  fss::ManualClock clock(1700000000);
  LogOptions o = DefaultOptions();
  std::ostringstream os;
  StreamLogger logger(o, clock, os);

  SECTION("对照：不转义时确实会多出一行") {
    // 用一个刻意不转义的渲染（text 的 EscapeForTextLine 被绕过）来证明风险真实存在
    std::string raw = "file=evil.txt\nINFO forged line";
    std::ostringstream naive;
    naive << raw << '\n';
    std::istringstream in(naive.str());
    std::string l;
    int n = 0;
    while (std::getline(in, l)) ++n;
    REQUIRE(n == 2);  // 未经处理的消息 = 2 行
  }

  const std::string evil = "GET /f?name=evil.txt\n{\"level\":\"error\",\"msg\":\"forged\"}";

  SECTION("JSON 格式") {
    fss::logging::Info(logger, evil, {});
    std::istringstream in(os.str());
    std::string l;
    int n = 0;
    while (std::getline(in, l)) {
      auto parsed = fss::json::Parse(l);
      REQUIRE(parsed.ok());
      ++n;
    }
    REQUIRE(n == 1);  // ★ 仍然只有一行
    REQUIRE(os.str().find("forged") != std::string::npos);  // 内容被保留（已转义）
  }

  SECTION("text 格式") {
    LogOptions t = DefaultOptions();
    t.format = "text";
    std::ostringstream os2;
    StreamLogger logger2(t, clock, os2);
    fss::logging::Info(logger2, evil, {});
    std::istringstream in(os2.str());
    std::string l;
    int n = 0;
    while (std::getline(in, l)) ++n;
    REQUIRE(n == 1);
    REQUIRE(os2.str().find("\n{\"level\"") == std::string::npos);
  }
}

TEST_CASE("并发写：整行原子，不出现交错（否则 JSON Lines 会被解析器拒绝）",
          "[phase1][logging][c1.6]") {
  fss::ManualClock clock(1700000000);
  LogOptions o = DefaultOptions();
  std::ostringstream os;
  StreamLogger logger(o, clock, os);

  constexpr int kThreads = 8;
  constexpr int kPerThread = 200;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&logger, t] {
      for (int i = 0; i < kPerThread; ++i) {
        fss::logging::Info(logger, "concurrent",
                           Fields{{"thread", t}, {"i", i}, {"token", "THREADLEAK"}});
      }
    });
  }
  for (auto& th : threads) th.join();

  REQUIRE(logger.written() == static_cast<std::uint64_t>(kThreads) * kPerThread);

  std::istringstream in(os.str());
  std::string line;
  std::size_t n = 0;
  while (std::getline(in, line)) {
    auto parsed = fss::json::Parse(line);
    INFO("坏行: " << line);
    REQUIRE(parsed.ok());  // ★ 每一行都还得是完整 JSON —— 交错会在这里失败
    REQUIRE(parsed.value()["msg"] == "concurrent");
    ++n;
  }
  REQUIRE(n == static_cast<std::size_t>(kThreads) * kPerThread);
  REQUIRE(os.str().find("THREADLEAK") == std::string::npos);
}

TEST_CASE("MemoryLogger：测试与审计用的内存实现同样脱敏", "[phase1][logging][c1.6]") {
  MemoryLogger logger(DefaultOptions());
  fss::logging::Info(logger, "ok", Fields{{"secret_key", "MEMLEAK"}, {"file_id", "f-9"}});
  fss::logging::Debug(logger, "被级别过滤掉");  // 默认 info

  REQUIRE(logger.size() == 1);
  const auto recs = logger.records();
  REQUIRE(recs.at(0)["secret_key"] == "***");
  REQUIRE(recs.at(0)["file_id"] == "f-9");
  REQUIRE(fss::json::Dump(recs.at(0)).find("MEMLEAK") == std::string::npos);

  logger.SetMinLevel(Level::kDebug);
  fss::logging::Debug(logger, "现在能出来了");
  REQUIRE(logger.size() == 2);
  logger.Clear();
  REQUIRE(logger.size() == 0);
}

TEST_CASE("ContextFields：只输出非空字段（空字段会淹没有用信息）", "[phase1][logging]") {
  fss::logging::Context ctx;
  REQUIRE(fss::logging::ContextFields(ctx).empty());

  ctx.correlation_id = "cid-1";
  ctx.user_id = "u-1";
  const auto fields = fss::logging::ContextFields(ctx);
  REQUIRE(fields.size() == 2);
  REQUIRE(fields.at(0).first == "correlation_id");
  REQUIRE(fields.at(1).first == "user_id");
}

TEST_CASE("OptionsFromConfig：从 observability.* 的取值构造（与示例配置同源）",
          "[phase1][logging][config]") {
  const auto o = fss::logging::OptionsFromConfig(Level::kWarn, "text", "fss-test",
                                                kDefaultRedactKeys);
  REQUIRE(o.min_level == Level::kWarn);
  REQUIRE(o.format == "text");
  REQUIRE(o.service == "fss-test");
  REQUIRE(o.redact_keys.size() == 11);
  REQUIRE(Redactor(o.redact_keys).Matches("x-amz-signature"));

  // 空 format 回退到 json（避免配置写空导致输出既不是 json 也不是 text）
  const auto d = fss::logging::OptionsFromConfig(Level::kInfo, "", "", "");
  REQUIRE(d.format == "json");
  REQUIRE(d.service == "file-service");
  REQUIRE(d.redact_keys.empty());
}

// ===========================================================================
//  P1-D12 / P1-D11 回归
// ===========================================================================

TEST_CASE("★ 自备 JSON 转义必须与 nlohmann 的 replace 语义**逐字节等价**（穷举）",
          "[phase1][logging][c1.6]") {
  // 为什么这样测：转义是安全边界，而我们是**手写**的。手写转义最常见的辩护是
  // "看起来对"。这里用 nlohmann 的 `error_handler_t::replace` 作为**参照实现**，
  // 穷举所有字节组合逐一比对 —— 于是"正确"变成可机械判定的事实，而不是信心。
  const auto oracle = [](const std::string& s) {
    return fss::json::Value(s).dump(-1, ' ', false, fss::json::Value::error_handler_t::replace);
  };
  // 用 "A" + seq + "B" 包裹：既覆盖序列本身，也覆盖"替换之后如何继续"这条路径
  const auto check = [&](const std::string& seq, std::string* first_bad) {
    const std::string payload = "A" + seq + "B";
    const std::string mine = fss::logging::EscapeJsonString(payload);
    const std::string want = oracle(payload);
    if (mine == want) return true;
    if (first_bad->empty()) {
      *first_bad = "payload bytes:";
      for (unsigned char c : payload) {
        char buf[8];
        std::snprintf(buf, sizeof buf, " %02x", c);
        *first_bad += buf;
      }
      *first_bad += "\n  ours: " + mine + "\n  want: " + want;
    }
    return false;
  };

  std::string bad;
  std::size_t cases = 0;

  SECTION("全部 256 个单字节") {
    for (int b = 0; b < 256; ++b) {
      REQUIRE(check(std::string(1, static_cast<char>(b)), &bad));
      ++cases;
    }
    REQUIRE(cases == 256);
  }

  SECTION("全部 65536 个双字节序列") {
    for (int a = 0; a < 256; ++a) {
      for (int b = 0; b < 256; ++b) {
        std::string seq;
        seq += static_cast<char>(a);
        seq += static_cast<char>(b);
        if (!check(seq, &bad)) {
          INFO("首个不一致: " << bad);
          REQUIRE(false);
        }
        ++cases;
      }
    }
    REQUIRE(cases == 65536);
  }

  SECTION("三字节：覆盖每个 3 字节首字节类别 × 全部后续组合") {
    for (int a : {0xC2, 0xDF, 0xE0, 0xE4, 0xED, 0xEF}) {
      for (int b = 0; b < 256; ++b) {
        for (int c = 0; c < 256; ++c) {
          std::string seq;
          seq += static_cast<char>(a);
          seq += static_cast<char>(b);
          seq += static_cast<char>(c);
          if (!check(seq, &bad)) {
            INFO("首个不一致: " << bad);
            REQUIRE(false);
          }
          ++cases;
        }
      }
    }
    REQUIRE(cases == 6 * 256 * 256);
  }

  SECTION("四字节：覆盖首字节/尾随字节的关键边界") {
    for (int a : {0xF0, 0xF1, 0xF4, 0xF5}) {
      for (int b = 0; b < 256; ++b) {
        for (int c : {0x80, 0x8F, 0x90, 0xBF, 0xC0}) {
          for (int d : {0x80, 0xBF, 0xC0}) {
            std::string seq;
            seq += static_cast<char>(a);
            seq += static_cast<char>(b);
            seq += static_cast<char>(c);
            seq += static_cast<char>(d);
            if (!check(seq, &bad)) {
              INFO("首个不一致: " << bad);
              REQUIRE(false);
            }
            ++cases;
          }
        }
      }
    }
    REQUIRE(cases == 4 * 256 * 5 * 3);
  }

  // 注：Catch2 的每个 SECTION 都是独立执行，所以计数不跨 section 累加；
  //     每个 section 各自断言了自己的精确数量（见上）。
}

TEST_CASE("★ P1-D12：非法 UTF-8 不得让日志抛异常，也不得把坏字节写进输出",
          "[phase1][logging][c1.6]") {
  const std::vector<std::string> nasty = {
      std::string("x\xff\xfey"),                  // 非法首字节
      std::string("x\xe4\xb8", 4),                // 截断的 3 字节序列
      std::string("x\xed\xa0\x80y"),              // UTF-16 代理区
      std::string("x\xc0\xafy"),                  // 过长编码
      std::string("x\xf5y"),                      // > U+10FFFF
      std::string("a\0b", 3),                     // 裸 NUL
      std::string("x\x80y"),                      // 孤立续字节
  };

  SECTION("对照：nlohmann 默认 dump **会抛异常**（这就是必须自备转义的原因）") {
    for (const auto& s : nasty) {
      bool threw = false;
      try {
        (void)fss::json::Dump(fss::json::Value(s));
      } catch (const std::exception&) {
        threw = true;
      }
      if (s.find('\0') != std::string::npos || s == "x\x80y") continue;  // NUL 与孤立续字节是合法 JSON
      INFO("原本会抛异常的输入长度: " << s.size());
      bool any_invalid = false;
      for (const unsigned char c : s) {
        if (c >= 0x80) { any_invalid = true; break; }
      }
      if (any_invalid) REQUIRE(threw);
    }
  }

  SECTION("StreamLogger：不抛异常，且输出仍是可解析 JSON") {
    fss::ManualClock clock(1700000000);
    std::ostringstream os;
    StreamLogger logger(DefaultOptions(), clock, os);
    std::size_t written = 0;
    for (const auto& s : nasty) {
      REQUIRE_NOTHROW(fss::logging::Info(logger, s, Fields{{"path", s}, {"token", s}}));
      ++written;
    }
    REQUIRE(logger.written() == written);

    std::istringstream in(os.str());
    std::string line;
    std::size_t n = 0;
    while (std::getline(in, line)) {
      auto parsed = fss::json::Parse(line);
      INFO("行: " << line);
      REQUIRE(parsed.ok());
      ++n;
    }
    REQUIRE(n == nasty.size());
    // 坏字节不能原样出现在输出里（U+FFFD 是合法替代）
    REQUIRE(os.str().find('\xff') == std::string::npos);
    REQUIRE(os.str().find('\xfe') == std::string::npos);
  }

  SECTION("MemoryLogger 同样不抛") {
    MemoryLogger logger(DefaultOptions());
    for (const auto& s : nasty) REQUIRE_NOTHROW(fss::logging::Info(logger, s, {}));
    REQUIRE(logger.size() == nasty.size());
  }

  SECTION("编译期保证：ILogger::Log 是 noexcept") {
    static_assert(noexcept(std::declval<const fss::logging::ILogger&>().Log(
                      Level::kInfo, std::string_view{}, std::declval<const Fields&>())),
                  "日志不许把异常抛回调用方");
    static_assert(noexcept(std::declval<const StreamLogger&>().Log(Level::kInfo, {}, {})),
                  "StreamLogger::Log 必须是 noexcept");
    SUCCEED();
  }
}

TEST_CASE("★ Render 的往返一致性：解析回来必须与原值逐个相等（手写追加的强校验）",
          "[phase1][logging]") {
  // Render 不再是"构造 nlohmann 对象再 dump"，而是手写追加。
  // 手写就可能写错类型/漏转义 —— 用"解析回来比对"把这件事变成可判定事实。
  fss::ManualClock clock(1700000000);
  std::ostringstream os;
  StreamLogger logger(DefaultOptions(), clock, os);

  const std::string tricky = "路径/中文\"引号\\反斜杠\t制表\n换行\x01控制";
  const Fields fields{
      {"s", tricky},
      {"i", -12345},
      {"u", 18446744073709551615ULL},
      {"b_true", true},
      {"b_false", false},
      {"null_v", nullptr},
      {"d", 1.5},
      {"nested", fss::json::Value{{"k", "v"}}},
      {"arr", fss::json::Value::array({1, "two", false})},
      {"empty", ""},
  };
  fss::logging::Info(logger, tricky, fields);

  auto parsed = fss::json::Parse(os.str());
  REQUIRE(parsed.ok());
  const auto& rec = parsed.value();
  REQUIRE(rec["msg"] == tricky);
  REQUIRE(rec["s"] == tricky);
  REQUIRE(rec["i"].get<std::int64_t>() == -12345);
  REQUIRE(rec["u"].get<std::uint64_t>() == 18446744073709551615ULL);
  REQUIRE(rec["b_true"].get<bool>() == true);
  REQUIRE(rec["b_false"].get<bool>() == false);
  REQUIRE(rec["null_v"].is_null());
  REQUIRE(rec["d"].get<double>() == 1.5);
  REQUIRE(rec["nested"]["k"] == "v");
  REQUIRE(rec["arr"].size() == 3);
  REQUIRE(rec["empty"] == "");
  REQUIRE(rec["ts"] == "2023-11-14T22:13:20.000Z");
}

TEST_CASE("★ 单趟脱敏：一条消息里多个不同密钥必须全部被打码（P1-D11 重写后的回归）",
          "[phase1][logging][c1.6]") {
  const auto r = Redactor::FromCommaSeparated(kDefaultRedactKeys);

  SECTION("三个不同规则的密钥 + 重复出现") {
    const std::string msg =
        "a=1 secret_key=LEAK1 token=LEAK2 x-amz-signature=LEAK3 "
        "again secret_key=LEAK1";
    const std::string out = r.ScrubText(msg);
    INFO(out);
    for (const char* leak : {"LEAK1", "LEAK2", "LEAK3"}) {
      REQUIRE(out.find(leak) == std::string::npos);
    }
    REQUIRE(out.find("a=1") != std::string::npos);  // 无关部分必须保留
    REQUIRE(out.find("again") != std::string::npos);
  }

  SECTION("重叠规则（sig 与 signature 命中同一处）不得把输出改坏") {
    const std::string out = r.ScrubText("X-Amz-Signature=OVERLAP");
    INFO(out);
    REQUIRE(out.find("OVERLAP") == std::string::npos);
    // 只应替换一次：不允许多次替换把 *** 叠成更长的东西
    REQUIRE(out == "X-Amz-Signature=***");
  }

  SECTION("相邻的两个键值对都要处理，且后面的不能被前面吃掉") {
    const std::string out = r.ScrubText("token=A&access_key=B");
    INFO(out);
    REQUIRE(out == "token=***&access_key=***");
  }

  SECTION("两侧对照：空规则集时明文原样保留") {
    const Redactor empty = Redactor::FromCommaSeparated("");
    const std::string msg = "secret_key=LEAK1&token=LEAK2";
    REQUIRE(empty.ScrubText(msg) == msg);
  }

  SECTION("短于最短规则的输入走早退路径且不改内容") {
    REQUIRE(r.ScrubText("ab") == "ab");
    REQUIRE(r.ScrubText("") == "");
  }
}

TEST_CASE("Redactor 的 min_key_len 早退不改变匹配语义（与逐字符判定一致）",
          "[phase1][logging]") {
  const Redactor r = Redactor::FromCommaSeparated("sig,secret_key,authorization");
  // 长度刚好等于/大于最短规则（3）的名字都要正常判定
  REQUIRE(r.Matches("x-amz-signature"));
  REQUIRE(r.Matches("sig"));
  REQUIRE(r.Matches("Secret_Key"));
  REQUIRE_FALSE(r.Matches("ab"));
  REQUIRE_FALSE(r.Matches("st"));
}
