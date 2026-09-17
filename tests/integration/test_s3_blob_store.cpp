// =============================================================================
//  C5.3 / C5.6 / C5.7：S3BlobStore 的**同一套契约**、分页与错误映射
// =============================================================================
//  为什么这里必须复用 `port_contract.h` 的那一份断言（C2.10/C5.3）：
//    三个实现（memory / POSIX / S3）如果各写一套测试，"语义一致"就只是口号；
//    上层迟早写出"只在某个后端成立"的分支。同一份断言跑第三遍，才是可执行的等价性。
//
//  对拍对象：`tests/tools/mock_s3.py`（Python 独立验签的假 S3）。因此这组用例同时覆盖
//  "数据面能不能用"与"签名在真实 HTTP 上是否被独立实现接受"。
// =============================================================================
#include <catch2/catch.hpp>

#include "mock_s3.h"
#include "port_contract.h"

#include "common/crypto/crypto.h"
#include "common/time/clock.h"
#include "infra/blob/s3/s3_blob_store.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using fss::domain::ByteRange;
using fss::domain::ObjectRef;
using fss::domain::PutOptions;
using fss::infra::AwsCredentials;
using fss::infra::S3BlobStore;
using fss::infra::S3Options;

constexpr const char* kAccessKey = "AKIDEXAMPLE";
constexpr const char* kSecretKey = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";

S3BlobStore MakeStore(const std::string& endpoint,
                      const std::string& access_key = kAccessKey,
                      const std::string& secret_key = kSecretKey) {
  S3Options options;
  options.endpoint = endpoint;
  options.region = "us-east-1";
  options.force_path_style = true;
  options.verify_tls = false;
  options.scheme = "http";
  options.credentials = AwsCredentials{access_key, secret_key, ""};
  static fss::SystemClock clock;
  return S3BlobStore(options, clock);
}

}  // namespace

TEST_CASE("★ C5.3 S3BlobStore 通过与 memory/POSIX **同一套**端口契约（C2.10 第三实现）",
          "[phase5][integration][s3][c5.3]") {
  fss::test::MockS3 mock;
  S3BlobStore store = MakeStore(mock.endpoint());
  fss::test::CheckBlobStoreContract(store);
}

TEST_CASE("★ C5.7 S3 错误码 → ErrorKind 映射（AccessDenied / SlowDown / NoSuchBucket）",
          "[phase5][integration][s3][c5.7]") {
  fss::test::MockS3 mock;
  S3BlobStore store = MakeStore(mock.endpoint());
  REQUIRE(store.ensure_container("testbucket").ok());

  SECTION("AccessDenied（403）→ kStorageAccessDenied（不是 kPermissionDenied）") {
    //  ★ 这条映射的意义：存储侧拒绝本服务 ≠ 调用方没权限。若映射成 kPermissionDenied，
    //    客户端会看到"你没权限"（403 且语义错误），而真正该做的是告警存储凭证/桶策略。
    fss::bytes::StringSource source("x");
    const auto result = store.put(ObjectRef{"testbucket", "denied/x.denied"}, source,
                                  PutOptions{});
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kStorageAccessDenied);
    REQUIRE(result.error().message().find("AccessDenied") != std::string::npos);
  }

  SECTION("SlowDown（503）→ kUnavailable") {
    fss::bytes::StringSource source("x");
    const auto result = store.put(ObjectRef{"testbucket", "slow/x.slowdown"}, source,
                                  PutOptions{});
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kUnavailable);
    REQUIRE(result.error().message().find("SlowDown") != std::string::npos);
  }

  SECTION("NoSuchBucket（404）→ kNotFound；stat 缺失对象 → Ok + exists=false") {
    const auto missing = store.stat(ObjectRef{"no-such-bucket-xyz", "a"});
    REQUIRE(missing.ok());
    REQUIRE_FALSE(missing.value().exists);

    fss::bytes::StringSource source("x");
    const auto result = store.put(ObjectRef{"no-such-bucket-xyz", "a"}, source, PutOptions{});
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kNotFound);
  }

  SECTION("get 缺失对象 → kNotFound，且**不会**把错误 XML 写进 sink") {
    fss::bytes::StringSink sink;
    const auto result = store.get(ObjectRef{"testbucket", "nope/missing.bin"}, sink,
                                  ByteRange{});
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kNotFound);
    REQUIRE(sink.str().empty());  // ★ 关键：错误体不能污染调用方的 sink
  }

  SECTION("remove 缺失对象 → Ok（删除天然幂等）") {
    REQUIRE(store.remove(ObjectRef{"testbucket", "nope/missing.bin"}).ok());
  }
}

TEST_CASE("★ C5.6 ListObjectsV2 分页：>1000 个键必须不重不漏地翻完",
          "[phase5][integration][s3][c5.6]") {
  constexpr int kTotal = 1005;  // > 1000：必须走至少两页（S3 的单页上限是 1000）
  fss::test::MockS3 mock([] {
    fss::test::MockS3::Options options;
    options.seed_count = kTotal;
    options.seed_prefix = "page/";
    return options;
  }());
  S3BlobStore store = MakeStore(mock.endpoint());

  std::vector<std::string> collected;
  std::string token;
  int pages = 0;
  while (true) {
    const auto page = store.list("testbucket", "page/", token, 1000);
    REQUIRE(page.ok());
    INFO("第 " << pages + 1 << " 页：" << page.value().entries.size() << " 条，truncated="
               << page.value().truncated);
    for (const auto& entry : page.value().entries) collected.push_back(entry.key);
    ++pages;
    if (!page.value().truncated) break;
    token = page.value().continuation_token;
    REQUIRE_FALSE(token.empty());  // 声明还有下一页却不给 token = 客户端必然死循环
    REQUIRE(pages < 10);           // 兜底：避免"永远 truncated"把测试挂死
  }

  REQUIRE(collected.size() == static_cast<std::size_t>(kTotal));  // 不重不漏
  REQUIRE(pages >= 2);
  //  字典序且严格递增（S3 的 ListObjectsV2 按 UTF-8 字节序返回）
  for (std::size_t i = 1; i < collected.size(); ++i) {
    INFO("i=" << i << " " << collected[i - 1] << " -> " << collected[i]);
    REQUIRE(collected[i - 1] < collected[i]);
  }
  REQUIRE(collected.front() == "page/00000");
  REQUIRE(collected.back() == "page/01004");

  SECTION("前缀过滤：非该前缀的键不得出现") {
    const auto page = store.list("testbucket", "page/000", "", 1000);
    REQUIRE(page.ok());
    //  前缀 "page/000" 命中 page/00000..page/00099（100 个）——不要凭直觉写 10
    REQUIRE(page.value().entries.size() == 100);
    for (const auto& entry : page.value().entries) {
      REQUIRE(entry.key.rfind("page/000", 0) == 0);
    }
  }
}
