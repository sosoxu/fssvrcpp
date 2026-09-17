// =============================================================================
//  C5.1 / C5.5（单元部分）：SigV4 签名器与百分号编码的边界
// =============================================================================
//  ★ 测试分三层，各自回答不同的问题：
//    ① **算法形状**：canonical request / string-to-sign 的逐字节文本（可人工核对）；
//    ② **覆盖性**（R1 的自证对照）：改动 method/path/query/头/日期/密钥/区域/服务
//       中的**任何一个**都必须改变签名 —— 只断言"签名非空"无法区分"签了"与"没签"；
//    ③ **边界编码**（C5.5）：`~` 不编码、空格 `%20`、`+` `%2B`、`%` `%25`、
//       非 ASCII 逐字节、`/` 在路径里保留而在 query 里编码。
//
//  ⚠️ 独立实现的等价比对（libcurl `--aws-sigv4` 与 Python 参照实现）在
//     `tests/integration/test_sigv4_crosscheck.cpp`：**本文件里的期望值都由我们自己的
//     规则推出，单靠它无法发现"两边一起错"**（P1-D07 的教训）。
// =============================================================================
#include <catch2/catch.hpp>

#include "infra/blob/s3/sigv4_signer.h"

#include <string>
#include <vector>

using fss::infra::AwsCredentials;
using fss::infra::MakeSigV4Request;
using fss::infra::SigV4EncodePath;
using fss::infra::SigV4EncodeQuery;
using fss::infra::SigV4Options;
using fss::infra::SigV4Request;
using fss::infra::SigV4Signer;

namespace {

constexpr std::string_view kAmzDate = "20130524T000000Z";
constexpr std::string_view kEmptySha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

AwsCredentials DemoCredentials() {
  return AwsCredentials{"AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", ""};
}

SigV4Options DemoOptions() {
  SigV4Options options;
  options.region = "us-east-1";
  options.service = "s3";
  options.force_path_style = true;
  options.endpoint = "s3.amazonaws.com";
  options.scheme = "https";
  return options;
}

}  // namespace

TEST_CASE("★ C5.5 百分号编码：unreserved 不编码、`/` 分场景、非 ASCII 逐字节",
          "[phase5][sigv4][c5.5]") {
  //  unreserved 集合原样保留（★ `~` 若被编成 %7E，签名与真实 S3 不一致）
  const std::string unreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";
  REQUIRE(SigV4EncodeQuery(unreserved) == unreserved);
  REQUIRE(SigV4EncodePath(unreserved) == unreserved);

  //  必须被编码的常见字节
  REQUIRE(SigV4EncodeQuery("a b") == "a%20b");   // ★ 空格是 %20，不是 `+`
  REQUIRE(SigV4EncodeQuery("a+b") == "a%2Bb");   // ★ `+` 必须编码（否则会被解成空格）
  REQUIRE(SigV4EncodeQuery("100%") == "100%25");
  REQUIRE(SigV4EncodeQuery("=") == "%3D");
  REQUIRE(SigV4EncodeQuery("&") == "%26");
  REQUIRE(SigV4EncodeQuery("?") == "%3F");
  REQUIRE(SigV4EncodeQuery("#") == "%23");

  //  `/` 的两种用法：canonical URI 保留、query 里编码
  REQUIRE(SigV4EncodePath("/a/b/c") == "/a/b/c");
  REQUIRE(SigV4EncodeQuery("/a/b/c") == "%2Fa%2Fb%2Fc");

  //  非 ASCII：先 UTF-8，再逐字节 %XX（大写十六进制）
  REQUIRE(SigV4EncodeQuery("中") == "%E4%B8%AD");        // U+4E2D = E4 B8 AD
  REQUIRE(SigV4EncodePath("/目录/文件.txt") == "/%E7%9B%AE%E5%BD%95/%E6%96%87%E4%BB%B6.txt");
  REQUIRE(SigV4EncodeQuery("é") == "%C3%A9");            // U+00E9 = C3 A9

  //  十六进制必须**大写**（AWS 的规范；小写会导致签名不匹配）
  REQUIRE(SigV4EncodeQuery("\x1f") == "%1F");
}

TEST_CASE("★ C5.1 canonical request / string-to-sign 的逐字节形状",
          "[phase5][sigv4][c5.1]") {
  SigV4Signer signer(DemoCredentials(), DemoOptions());
  auto request = MakeSigV4Request(DemoOptions(), "GET", "examplebucket", "test.txt",
                                  {{"max-keys", "2"}}, {}, kEmptySha256, kAmzDate);
  const auto signed_request = signer.SignRequest(request, kAmzDate);
  REQUIRE(signed_request.ok());

  const auto& result = signed_request.value();
  //  ① canonical request：METHOD\nURI\nQUERY\nCANONICAL_HEADERS\n\nSIGNED_HEADERS\nPAYLOAD
  const std::string expected_canonical =
      "GET\n"
      "/examplebucket/test.txt\n"
      "max-keys=2\n"
      "host:s3.amazonaws.com\n"
      "x-amz-content-sha256:" + std::string(kEmptySha256) + "\n"
      "x-amz-date:" + std::string(kAmzDate) + "\n"
      "\n"
      "host;x-amz-content-sha256;x-amz-date\n" + std::string(kEmptySha256);
  INFO("actual:\n" << result.canonical_request);
  REQUIRE(result.canonical_request == expected_canonical);

  //  ② 头名小写、按字典序、以 `;` 连接
  REQUIRE(result.signed_headers == "host;x-amz-content-sha256;x-amz-date");

  //  ③ string to sign：ALGORITHM\ndate\nscope\nhex(sha256(creq))
  REQUIRE(result.string_to_sign.rfind("AWS4-HMAC-SHA256\n", 0) == 0);
  REQUIRE(result.string_to_sign.find("\n20130524T000000Z\n20130524/us-east-1/s3/aws4_request\n") !=
          std::string::npos);

  //  ④ Authorization 头的三段结构（客户端与服务端都按这个格式解析）
  REQUIRE(result.authorization.rfind("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/"
                                     "20130524/us-east-1/s3/aws4_request, SignedHeaders=",
                                     0) == 0);
  REQUIRE(result.authorization.find(", Signature=" + result.signature) != std::string::npos);
  REQUIRE(result.signature.size() == 64);  // hex(sha256) = 32 字节
}

TEST_CASE("★ C5.1 签名覆盖性：改动任一签名输入都必须改变签名（R1 的自证对照）",
          "[phase5][sigv4][c5.1]") {
  SigV4Signer signer(DemoCredentials(), DemoOptions());
  const auto base_request = MakeSigV4Request(DemoOptions(), "GET", "examplebucket", "test.txt",
                                             {{"max-keys", "2"}}, {}, kEmptySha256, kAmzDate);
  const auto baseline = signer.SignRequest(base_request, kAmzDate);
  REQUIRE(baseline.ok());
  const std::string base_signature = baseline.value().signature;

  SECTION("method / path / query 变化") {
    SigV4Request request = base_request;
    request.method = "PUT";
    REQUIRE(signer.SignRequest(request, kAmzDate).value().signature != base_signature);

    request = base_request;
    request.path = "/examplebucket/other.txt";
    REQUIRE(signer.SignRequest(request, kAmzDate).value().signature != base_signature);

    request = base_request;
    request.query = {{"max-keys", "3"}};
    REQUIRE(signer.SignRequest(request, kAmzDate).value().signature != base_signature);

    //  ★ query 顺序不影响签名（canonical query 会排序）——这是**正例**断言（R16）
    request = base_request;
    request.query = {{"b", "2"}, {"a", "1"}};
    auto sorted = request;
    sorted.query = {{"a", "1"}, {"b", "2"}};
    REQUIRE(signer.SignRequest(request, kAmzDate).value().signature ==
            signer.SignRequest(sorted, kAmzDate).value().signature);
  }

  SECTION("头的变化（含只改一个字节）") {
    SigV4Request request = base_request;
    request.headers.push_back({"x-amz-meta-note", "a"});
    REQUIRE(signer.SignRequest(request, kAmzDate).value().signature != base_signature);

    request = base_request;
    for (auto& [name, value] : request.headers) {
      if (name == "x-amz-content-sha256") value = std::string(64, '0');
    }
    REQUIRE(signer.SignRequest(request, kAmzDate).value().signature != base_signature);
  }

  SECTION("日期 / payload 哈希") {
    REQUIRE(signer.SignRequest(base_request, "20130525T000000Z").value().signature !=
            base_signature);
    SigV4Request request = base_request;
    request.payload_sha256_hex = "UNSIGNED-PAYLOAD";
    REQUIRE(signer.SignRequest(request, kAmzDate).value().signature != base_signature);
  }

  SECTION("密钥 / 区域 / 服务 / 会话令牌") {
    SigV4Signer other_key(AwsCredentials{"AKIDEXAMPLE", "another-secret", ""}, DemoOptions());
    REQUIRE(other_key.SignRequest(base_request, kAmzDate).value().signature != base_signature);

    auto region_options = DemoOptions();
    region_options.region = "eu-west-1";
    SigV4Signer other_region(DemoCredentials(), region_options);
    REQUIRE(other_region.SignRequest(base_request, kAmzDate).value().signature != base_signature);

    auto service_options = DemoOptions();
    service_options.service = "s3-object-lambda";
    SigV4Signer other_service(DemoCredentials(), service_options);
    REQUIRE(other_service.SignRequest(base_request, kAmzDate).value().signature != base_signature);

    //  会话令牌必须进 SignedHeaders（否则服务端重算时看不到它）
    SigV4Signer with_token(AwsCredentials{"AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY",
                                          "SESSIONTOKEN"},
                           DemoOptions());
    SigV4Request token_request = base_request;
    token_request.headers.push_back({"x-amz-security-token", "SESSIONTOKEN"});
    const auto token_signature = with_token.SignRequest(token_request, kAmzDate);
    REQUIRE(token_signature.ok());
    REQUIRE(token_signature.value().signed_headers.find("x-amz-security-token") !=
            std::string::npos);
    REQUIRE(token_signature.value().signature != base_signature);
  }

  SECTION("确定性：同一输入两次签名必须完全相同") {
    const auto again = signer.SignRequest(base_request, kAmzDate);
    REQUIRE(again.ok());
    REQUIRE(again.value().signature == base_signature);
  }
}

TEST_CASE("★ C5.4/C5.8 预签名 URL：结构、有效期上限、两种寻址形态",
          "[phase5][sigv4][c5.4]") {
  SigV4Signer signer(DemoCredentials(), DemoOptions());
  auto request = MakeSigV4Request(DemoOptions(), "PUT", "examplebucket", "dir/obj.bin",
                                  {{"x-id", "PutObject"}}, {}, "UNSIGNED-PAYLOAD", kAmzDate);
  request.headers = {{"host", request.host}};  // 预签名只需 host 参与签名

  const auto presigned = signer.PresignUrl(request, kAmzDate, 3600);
  REQUIRE(presigned.ok());
  const std::string url = presigned.value();
  INFO(url);
  REQUIRE(url.rfind("https://s3.amazonaws.com/examplebucket/dir/obj.bin?", 0) == 0);
  for (const char* param : {"X-Amz-Algorithm=AWS4-HMAC-SHA256",
                            "X-Amz-Credential=AKIDEXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request",
                            "X-Amz-Date=20130524T000000Z", "X-Amz-Expires=3600",
                            "X-Amz-SignedHeaders=host", "X-Amz-Signature="}) {
    INFO(param);
    REQUIRE(url.find(param) != std::string::npos);
  }
  //  ★ Credential 里的 `/` 必须编码成 %2F，且算法名里的 `/` 不能（它是固定字面量）
  REQUIRE(url.find("X-Amz-Credential=") != std::string::npos);
  REQUIRE(url.find("AWS4-HMAC-SHA256&") != std::string::npos);

  SECTION("有效期边界：0 与 >7 天被拒，604800 通过（S3 上限）") {
    REQUIRE_FALSE(signer.PresignUrl(request, kAmzDate, 0).ok());
    REQUIRE_FALSE(signer.PresignUrl(request, kAmzDate, 604801).ok());
    REQUIRE(signer.PresignUrl(request, kAmzDate, 604800).ok());
    //  有效期变了签名必须变（否则"过期时间"会被服务端算成同一个签名）
    auto other = signer.PresignUrl(request, kAmzDate, 7200);
    REQUIRE(other.ok());
    REQUIRE(other.value() != url);
  }

  SECTION("virtual-host 形态：host 变、路径变、签名随之不同") {
    auto options = DemoOptions();
    options.force_path_style = false;
    SigV4Signer virtual_host(DemoCredentials(), options);
    auto vh_request =
        MakeSigV4Request(options, "PUT", "examplebucket", "dir/obj.bin", {}, {},
                         "UNSIGNED-PAYLOAD", kAmzDate);
    REQUIRE(vh_request.host == "examplebucket.s3.amazonaws.com");
    REQUIRE(vh_request.path == "/dir/obj.bin");
    REQUIRE(signer.HostFor("examplebucket") == "s3.amazonaws.com");
    REQUIRE(virtual_host.HostFor("examplebucket") == "examplebucket.s3.amazonaws.com");

    const auto vh_signed = virtual_host.SignRequest(vh_request, kAmzDate);
    REQUIRE(vh_signed.ok());
    REQUIRE(vh_signed.value().canonical_request.find(
                "host:examplebucket.s3.amazonaws.com") != std::string::npos);

    //  ★ 注意用**未改动**的请求：上面的 `request.headers` 为了预签名只留了 host，
    //    直接拿它做头部签名会因缺 `x-amz-date` 而失败（这正是"两种签名的输入不同"）
    auto path_request = MakeSigV4Request(DemoOptions(), "PUT", "examplebucket", "dir/obj.bin",
                                        {}, {}, "UNSIGNED-PAYLOAD", kAmzDate);
    const auto path_signed = signer.SignRequest(path_request, kAmzDate);
    REQUIRE(path_signed.ok());
    REQUIRE(path_signed.value().signature != vh_signed.value().signature);
  }

  SECTION("缺少 host / x-amz-date 时必须报错（不是默默签一个错的东西）") {
    SigV4Request broken = request;
    broken.headers.clear();
    const auto missing_host = signer.SignRequest(broken, kAmzDate);
    REQUIRE_FALSE(missing_host.ok());
    REQUIRE(missing_host.error().message().find("host") != std::string::npos);

    SigV4Request no_date = request;
    no_date.headers = {{"host", no_date.host}};
    REQUIRE_FALSE(signer.SignRequest(no_date, kAmzDate).ok());

    //  amz_date 形状不对必须被拒（否则会拼出一个荒谬的 scope）
    REQUIRE_FALSE(signer.SignRequest(request, "20130524").ok());
  }
}
