// C1.8：common/json —— 往返一致、未知字段保留、PascalCase 精确性、解析错误带位置
#include <catch2/catch.hpp>

#include "common/json/json.h"

using fss::json::Value;

namespace {
// 取自 OSDU 官方黄金样例（docs/03-api-contract.md §3.3）的最小化版本，
// 保留关键的大小写特征：信封 camelCase、data 内 PascalCase。
const char* kPayload = R"JSON({
  "id": "opendes:dataset--File.Generic:33a71a04",
  "kind": "opendes:wks:dataset--File.Generic:1.0.0",
  "acl": { "viewers": ["data.default.viewers@opendes.example.com"], "owners": ["data.default.owners@opendes.example.com"] },
  "legal": { "legaltags": ["opendes-storage-tag"], "otherRelevantDataCountries": ["US"], "status": "compliant" },
  "data": {
    "Name": "Dataset X221/15",
    "TotalSize": "13245217273",
    "Endian": "BIG",
    "DatasetProperties": {
      "FileSourceInfo": {
        "FileSource": "/osdu-user/1624011206350-2021-06-18-10-13-26-350/33a71a04",
        "FileSize": "95463",
        "ChecksumAlgorithm": "SHA-256"
      }
    },
    "ExtensionProperties": {}
  },
  "vendorFutureField": { "nested": [1, 2, 3] }
})JSON";
}  // namespace

TEST_CASE("解析顶层必须是 object（数组/标量视为非法输入）", "[phase1][json]") {
  REQUIRE(fss::json::ParseObject(kPayload).ok());
  REQUIRE_FALSE(fss::json::ParseObject("[1,2,3]").ok());
  REQUIRE_FALSE(fss::json::ParseObject("42").ok());
  REQUIRE_FALSE(fss::json::ParseObject("").ok());
}

TEST_CASE("往返一致：parse → dump → parse 语义相等", "[phase1][json]") {
  auto v1 = fss::json::ParseObject(kPayload);
  REQUIRE(v1.ok());
  const std::string dumped = fss::json::Dump(v1.value());
  auto v2 = fss::json::ParseObject(dumped);
  REQUIRE(v2.ok());
  REQUIRE(v1.value() == v2.value());
}

TEST_CASE("★ 未知字段原样保留（前向兼容：上游新增字段不能被静默丢弃）",
          "[phase1][json]") {
  auto v = fss::json::ParseObject(kPayload);
  REQUIRE(v.ok());
  // 样例里故意放了一个"未来才有的"字段，往返后必须仍在
  const std::string dumped = fss::json::Dump(v.value());
  REQUIRE(dumped.find("vendorFutureField") != std::string::npos);
  REQUIRE(dumped.find("nested") != std::string::npos);

  auto again = fss::json::ParseObject(dumped);
  REQUIRE(again.ok());
  REQUIRE(fss::json::Has(again.value(), "vendorFutureField"));
  auto arr = fss::json::GetObject(again.value(), "vendorFutureField");
  REQUIRE(arr.ok());
  auto nested = fss::json::GetArray(arr.value(), "nested");
  REQUIRE(nested.ok());
  REQUIRE(nested.value().size() == 3);
}

TEST_CASE("★ PascalCase 精确性：data 内必须用 PascalCase，camelCase 必须失败",
          "[phase1][json][contract]") {
  auto v = fss::json::ParseObject(kPayload);
  REQUIRE(v.ok());
  auto data = fss::json::GetObject(v.value(), "data");
  REQUIRE(data.ok());

  // 正确的 PascalCase
  auto name = fss::json::GetString(data.value(), "Name");
  REQUIRE(name.ok());
  REQUIRE(name.value() == "Dataset X221/15");

  // ★ 错误的小写：必须**失败**，不能静默返回空
  auto wrong = fss::json::GetString(data.value(), "name");
  REQUIRE_FALSE(wrong.ok());
  REQUIRE(wrong.error().kind() == fss::ErrorKind::kInvalidArgument);
  // 错误信息里必须能看出是哪个字段、期望什么
  REQUIRE(*wrong.error().Find("field") == "name");
  REQUIRE(*wrong.error().Find("expected") == "string");

  // 嵌套层同样严格：FileSource 是 PascalCase
  auto dprops = fss::json::GetObject(data.value(), "DatasetProperties");
  REQUIRE(dprops.ok());
  auto fsi = fss::json::GetObject(dprops.value(), "FileSourceInfo");
  REQUIRE(fsi.ok());
  REQUIRE(fss::json::GetString(fsi.value(), "FileSource").ok());
  REQUIRE_FALSE(fss::json::GetString(fsi.value(), "fileSource").ok());

  // RequireAbsent：把"大小写用错"显式变成失败
  REQUIRE(fss::json::RequireAbsent(data.value(), "name").ok());
  REQUIRE_FALSE(fss::json::RequireAbsent(data.value(), "Name").ok());
}

TEST_CASE("字段取值失败时错误信息包含 field/expected/actual", "[phase1][json]") {
  auto v = fss::json::ParseObject(R"({"a": 1, "b": null})");
  REQUIRE(v.ok());
  auto s = fss::json::GetString(v.value(), "a");  // 存在但是整数
  REQUIRE_FALSE(s.ok());
  REQUIRE(*s.error().Find("field") == "a");
  REQUIRE(*s.error().Find("expected") == "string");
  REQUIRE(*s.error().Find("actual") == "integer");

  auto missing = fss::json::GetString(v.value(), "zzz");
  REQUIRE_FALSE(missing.ok());
  REQUIRE(*missing.error().Find("actual") == "null");
}

TEST_CASE("★ 解析错误带行列与偏移（线上排障要靠它）", "[phase1][json]") {
  // 输入（行号见注释）：
  //   1: {
  //   2:   "a": 1
  //   3:   "b": 2      ← 第 2 行末尾少了逗号，解析器在**第 3 行**的 "b" 处发现
  //   4: }
  // 位置必须指向**真正出错的 token 所在行**（第 3 行），而不是上一行的末尾。
  // （第一版断言写成 "2" 是错的 —— 这正说明位置信息是有意义的、值得断言。）
  const char* bad = "{\n  \"a\": 1\n  \"b\": 2\n}";
  auto r = fss::json::ParseObject(bad);
  REQUIRE_FALSE(r.ok());
  REQUIRE(r.error().kind() == fss::ErrorKind::kInvalidArgument);
  REQUIRE(r.error().Find("line").has_value());
  REQUIRE(r.error().Find("column").has_value());
  REQUIRE(r.error().Find("offset").has_value());
  REQUIRE(*r.error().Find("line") == "3");

  // 第一行就出错的情形
  auto r1 = fss::json::ParseObject("{oops}");
  REQUIRE_FALSE(r1.ok());
  REQUIRE(*r1.error().Find("line") == "1");

  // 位置还要能定位到深层错误（不是只有第一层）
  auto deep = fss::json::ParseObject("{\n \"a\": {\n   \"b\": [1, 2, }\n }\n}");
  REQUIRE_FALSE(deep.ok());
  REQUIRE(*deep.error().Find("line") == "3");
}

TEST_CASE("OSDU 的数字型字段声明为 string，宽松读取是显式行为", "[phase1][json]") {
  auto v = fss::json::ParseObject(R"({"TotalSize": "13245217273", "NumericSize": 95463})");
  REQUIRE(v.ok());
  auto s = fss::json::GetString(v.value(), "TotalSize");
  REQUIRE(s.ok());
  REQUIRE(s.value() == "13245217273");

  // 严格取字符串对数字会失败……
  REQUIRE_FALSE(fss::json::GetString(v.value(), "NumericSize").ok());
  // ……而宽松取会成功并转成字符串（这是刻意的、被记录的宽松）
  auto lenient = fss::json::GetStringLenient(v.value(), "NumericSize");
  REQUIRE(lenient.ok());
  REQUIRE(lenient.value() == "95463");
}

TEST_CASE("可选字段：缺失返回 nullopt，存在但类型错误返回 Error", "[phase1][json]") {
  auto v = fss::json::ParseObject(R"({"present": "x", "wrongType": 5})");
  REQUIRE(v.ok());

  auto opt = fss::json::GetOptionalString(v.value(), "absent");
  REQUIRE(opt.ok());
  REQUIRE_FALSE(opt.value().has_value());

  auto some = fss::json::GetOptionalString(v.value(), "present");
  REQUIRE(some.ok());
  REQUIRE(some.value().has_value());
  REQUIRE(*some.value() == "x");

  auto bad = fss::json::GetOptionalString(v.value(), "wrongType");
  REQUIRE_FALSE(bad.ok());  // 存在但类型不对 → 必须报错，不能当成"缺失"
}

// ---------------------------------------------------------------------------
// 注释 JSON（P1-D07 回归）
//
// 背景：全仓库的配置格式约定是"带注释的 JSON"（见 AGENTS.md 新增约定、
// config/fss.example.json、docs/operations.md）。但 json::ParseWithComments 曾经
// 把 ignore_comments 传错位置（传给 allow_exceptions），于是**注释从来没被支持过**——
// 而因为当时没有任何测试碰这条路径，缺陷一路活到 config 模块才被暴露。
// 下面既有"必须支持"的正例，也有"Parse 必须拒绝注释"的对照（R1）。
// ---------------------------------------------------------------------------
TEST_CASE("★ ParseWithComments 真的忽略注释（P1-D07 回归）", "[phase1][json][c1.3]") {
  SECTION("行注释") {
    auto r = fss::json::ParseWithComments(R"JSON(
// 顶层说明
{
  "a": 1,  // 行尾注释
  // 独立一行
  "b": "x"
}
)JSON");
    REQUIRE(r.ok());
    REQUIRE(fss::json::GetInt(r.value(), "a").value() == 1);
    REQUIRE(fss::json::GetString(r.value(), "b").value() == "x");
  }

  SECTION("块注释") {
    auto r = fss::json::ParseWithComments(R"JSON(
/* 头部
   多行块注释 */
{"a": 1, /* 中间 */ "b": [1, /* 元素间 */ 2]}
)JSON");
    REQUIRE(r.ok());
    REQUIRE(fss::json::GetInt(r.value(), "a").value() == 1);
    REQUIRE(r.value()["b"].size() == 2);
  }

  SECTION("嵌套对象/数组内部的注释") {
    auto r = fss::json::ParseWithComments(R"JSON({
  "outer": {
    // 深层注释
    "inner": [ // 数组内
      1,
      2 // 末尾元素
    ]
  }
})JSON");
    REQUIRE(r.ok());
    REQUIRE(r.value()["outer"]["inner"].size() == 2);
  }

  SECTION("字符串里的 // 与 /* 必须原样保留（不能被当注释吃掉）") {
    auto r = fss::json::ParseWithComments(
        R"JSON({"url": "http://x/y", "glob": "a/*/b", "s": "//not a comment"})JSON");
    REQUIRE(r.ok());
    REQUIRE(fss::json::GetString(r.value(), "url").value() == "http://x/y");
    REQUIRE(fss::json::GetString(r.value(), "glob").value() == "a/*/b");
    REQUIRE(fss::json::GetString(r.value(), "s").value() == "//not a comment");
  }

  SECTION("注释不必出现在合法空白位置之外——但在没有注释时行为与 Parse 一致") {
    auto plain = fss::json::ParseWithComments(R"({"a":1})");
    REQUIRE(plain.ok());
    REQUIRE(fss::json::GetInt(plain.value(), "a").value() == 1);
  }
}

TEST_CASE("★ 自证对照：严格 Parse 必须拒绝注释（否则上面的测试证明不了什么）",
          "[phase1][json][c1.3]") {
  // 如果 Parse 也"顺便"接受注释，那么"ParseWithComments 生效"就无法被区分——
  // 上面那个用例会永远通过。这条对照让两者的差异变成可观测的。
  auto strict = fss::json::Parse("{\n// 注释\n\"a\": 1\n}");
  REQUIRE_FALSE(strict.ok());
  REQUIRE(strict.error().kind() == fss::ErrorKind::kInvalidArgument);

  auto with_comments = fss::json::ParseWithComments("{\n// 注释\n\"a\": 1\n}");
  REQUIRE(with_comments.ok());
}

TEST_CASE("注释 JSON 的语法错误仍要带行列定位", "[phase1][json]") {
  // 注释会改变行号，定位必须仍然准确（排障时运维看到的就是这个行号）
  auto r = fss::json::ParseWithComments("{\n  // 第一行注释\n  \"a\": 1\n  \"b\": 2\n}");
  REQUIRE_FALSE(r.ok());
  REQUIRE(r.error().Find("line").has_value());
  REQUIRE(*r.error().Find("line") == "4");  // "b" 那行
}
