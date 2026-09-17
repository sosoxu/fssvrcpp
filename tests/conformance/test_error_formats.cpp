// =============================================================================
//  C4.5（HTTP 侧）：三种错误体形态 + 契约 §1.6 的固定消息，**在真实端口上**
// =============================================================================
//  为什么适配层单测（`tests/unit/test_http_error_mapper.cpp`）不够：
//    · 单测只证明"映射函数对"，证明不了"`Wrap()` 的三个出口（成功/契约错误/异常）
//      都走了映射"——漏掉任何一个出口，客户端拿到的就是另一副样子；
//    · `X-FSS-Error-Kind` 这类响应头只有在线缆上才看得见；
//    · `Content-Type` 是不是 `application/json`、body 是不是被包装层二次编码，
//      都只有真实端口能证明（H-1/H-2 的同类教训）。
// =============================================================================
#include <catch2/catch.hpp>

#include "http_fixture.h"

#include "adapters/http/http_error_mapper.h"
#include "common/json/json.h"

#include <string>
#include <vector>

namespace {

using fss::test::Authed;
using fss::test::HttpDo;
using fss::test::HttpFixture;
using fss::test::Reply;

//  契约 §1.6 的三种形态里，`code`/`message`/`reason` 必须一致，只有外层包装不同
std::string FixedMessageOf(const std::string& body) {
  const auto value = fss::json::ParseObject(body);
  REQUIRE(value.ok());
  for (const char* key : {"message", "Message"}) {
    if (const auto it = value.value().find(key); it != value.value().end() && it->is_string()) {
      return it->get<std::string>();
    }
  }
  //  旧形态可能把消息放在 `error.message` 里
  if (const auto error = value.value().find("error"); error != value.value().end()) {
    if (const auto message = error->find("message");
        message != error->end() && message->is_string()) {
      return message->get<std::string>();
    }
  }
  return {};
}

}  // namespace

TEST_CASE("★ C4.5 三种错误体形态：同一错误的 code/message 一致，只有外层不同",
          "[phase4][conformance][c4.5]") {
  struct FormatCase {
    fss::adapters::http::ErrorFormat format;
    const char* name;
  };
  const std::vector<FormatCase> formats = {
      {fss::adapters::http::ErrorFormat::kAppError, "apperror"},
      {fss::adapters::http::ErrorFormat::kLegacy, "legacy"},
      {fss::adapters::http::ErrorFormat::kApiError, "apierror"},
  };

  for (const auto& entry : formats) {
    fss::adapters::http::RouterOptions options;
    options.error_format = entry.format;
    HttpFixture fixture(options);
    const int port = fixture.port();
    INFO("error_format = " << entry.name);

    //  401 + `Missing partitionID`：一个头都不给（租户判定先于凭证判定）
    const auto no_headers = HttpDo(port, "GET", "/api/file/v2/files/uploadURL");
    REQUIRE(no_headers.status == 401);
    REQUIRE(no_headers.Header("Content-Type").has_value());
    REQUIRE(no_headers.Header("Content-Type")->find("application/json") != std::string::npos);
    //  ★ 响应头带上 ErrorKind：客户端/运维据此判断分支，不必解析 body
    REQUIRE(no_headers.Header("X-FSS-Error-Kind").value() == "kUnauthenticated");
    REQUIRE(FixedMessageOf(no_headers.body) == "Missing partitionID");
    REQUIRE(fss::json::ParseObject(no_headers.body).ok());

    //  只给 partition、不给 authorization → 401 + `Missing authorization token`
    const auto unauthorized = HttpDo(port, "GET", "/api/file/v2/files/uploadURL",
                                     {"data-partition-id: opendes"});
    REQUIRE(unauthorized.status == 401);
    REQUIRE(unauthorized.Header("X-FSS-Error-Kind").value() == "kUnauthenticated");
    REQUIRE(FixedMessageOf(unauthorized.body) == "Missing authorization token");

    //  只给 authorization、不给 partition → 401 + `Missing partitionID`
    const auto no_partition = HttpDo(port, "GET", "/api/file/v2/files/uploadURL",
                                     {"authorization: Bearer test-token"});
    REQUIRE(no_partition.status == 401);
    REQUIRE(FixedMessageOf(no_partition.body) == "Missing partitionID");

    //  404：不存在的记录（固定消息 `Record Not Found`）
    const auto missing = HttpDo(
        port, "GET",
        "/api/file/v2/files/opendes:dataset--File.Generic:000000000000000000000000000000ff/metadata",
        Authed());
    REQUIRE(missing.status == 404);
    REQUIRE(FixedMessageOf(missing.body) == "Record Not Found");

    //  §2.2 的固定消息：fileID 已存在 → 400（**不是** 409）
    const auto first = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
    REQUIRE(first.status == 200);
    const auto first_json = fss::json::ParseObject(first.body);
    REQUIRE(first_json.ok());
    const std::string file_id = first_json.value()["FileID"].get<std::string>();
    const auto again = HttpDo(port, "POST", "/api/file/v2/getLocation", Authed(),
                              std::string("{\"FileID\":\"") + file_id + "\"}");
    REQUIRE(again.status == 400);
    REQUIRE(FixedMessageOf(again.body) == "Location for fileID = " + file_id + " already exists");

    //  §1.4：非法 expiryTime → 400 + 固定消息
    const auto bad_expiry =
        HttpDo(port, "GET", "/api/file/v2/files/uploadURL?expiryTime=5X", Authed());
    REQUIRE(bad_expiry.status == 400);
    REQUIRE(FixedMessageOf(bad_expiry.body) ==
            "expiryTime pattern isn't supported. Value should be one of these regex patterns "
            "^[0-9]+M$ , ^[0-9]+H$ , ^[0-9]+D$");
  }
}

TEST_CASE("★ C4.5 三种形态的**外层结构**差异（实现与测试都不许互相抄袭）",
          "[phase4][conformance][c4.5]") {
  const auto body_for = [](fss::adapters::http::ErrorFormat format) {
    fss::adapters::http::RouterOptions options;
    options.error_format = format;
    HttpFixture fixture(options);
    //  固定触发 401：给租户、不给凭证
    return HttpDo(fixture.port(), "GET", "/api/file/v2/files/uploadURL",
                  {"data-partition-id: opendes"})
        .body;
  };

  const std::string app_error = body_for(fss::adapters::http::ErrorFormat::kAppError);
  const std::string legacy = body_for(fss::adapters::http::ErrorFormat::kLegacy);
  const std::string api_error = body_for(fss::adapters::http::ErrorFormat::kApiError);
  INFO("apperror : " << app_error);
  INFO("legacy   : " << legacy);
  INFO("apierror : " << api_error);

  //  三种形态必须**互不相同**：如果实现把开关忽略了，这三条会全相等，
  //  而"客户端兼容开关"这个能力就等于不存在（R15）
  REQUIRE(app_error != legacy);
  REQUIRE(app_error != api_error);
  REQUIRE(legacy != api_error);

  //  `code` 与 `message` 是三种形态的公共部分（契约 §1.6）
  for (const auto& body : {app_error, legacy, api_error}) {
    const auto value = fss::json::ParseObject(body);
    REQUIRE(value.ok());
    REQUIRE(FixedMessageOf(body) == "Missing authorization token");
  }
  //  `apperror` 是规范形态：顶层 `code` 直接是数字
  const auto app_value = fss::json::ParseObject(app_error);
  REQUIRE(app_value.value()["code"] == 401);
}
