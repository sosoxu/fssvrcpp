// =============================================================================
//  C2.6：LocationIssuer 在两个能力组合下分别产出原生预签名与自签 URL
// =============================================================================
//  这一条门槛的实质是 **ADR-003 的核心承诺**：上层"按能力编程"，而不是"按驱动类型分支"。
//  因此测试不只断言"两条路都能出 URL"，还断言：
//    · 同样的逻辑请求在两种能力下产出**结构完全一致**的领域结果
//      （file_id / file_source / zone / 有效期相同），差别只在 url / native / driver 字符串；
//    · `driver` 字段取自 `capabilities().driver_name`，不是 `StorageDriver` 枚举；
//    · 走原生分支时**不会**触碰自签 codec，反之亦然（调用计数为 0）。
//  驱动类型分支的"源码级"检查在 `test_capability_guard.cpp`（C2.7）。
// =============================================================================
#include <catch2/catch.hpp>

#include "fake_ports.h"

#include "app/services/expiry_policy.h"
#include "app/services/location_issuer.h"
#include "app/services/object_key_policy.h"
#include "common/ids/id_generator.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"
#include "infra/blob/memory/memory_blob_store.h"
#include "infra/location/memory/memory_location_repository.h"

#include <memory>
#include <optional>
#include <string>

using fss::ManualClock;
using fss::SequentialIdGenerator;
using fss::app::LocationIssuer;
using fss::app::LocationResult;
using fss::domain::BlobCapabilities;
using fss::domain::StorageZone;

namespace {

//  一个"可换能力"的完整装配：内存数据面 + 能力装饰器 + 自签 codec 替身
struct Harness {
  ManualClock clock{1700000000};
  SequentialIdGenerator ids{1};
  fss::infra::InMemoryBlobStore inner{clock};
  fss::infra::InMemoryLocationRepository locations;
  fss::test::RecordingSelfSignedCodec codec;
  std::unique_ptr<fss::test::CapabilityOverrideBlobStore> store;
  std::unique_ptr<fss::test::FakeBlobStoreFactory> factory;
  std::unique_ptr<LocationIssuer> issuer;

  Harness(bool native_presign, std::string driver_name,
          std::string presign_prefix = "https://storage.invalid")
      : clock(1700000000) {
    BlobCapabilities caps;
    caps.native_presign = native_presign;
    caps.server_side_copy = true;
    caps.range_read = true;
    caps.streaming_put = true;
    caps.driver_name = std::move(driver_name);
    store = std::make_unique<fss::test::CapabilityOverrideBlobStore>(inner, caps,
                                                                    std::move(presign_prefix));
    factory = std::make_unique<fss::test::FakeBlobStoreFactory>(*store);
    issuer = std::make_unique<LocationIssuer>(*factory, locations, codec, clock, ids,
                                             "https://self.invalid");
  }
};

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

}  // namespace

TEST_CASE("★ C2.6 原生预签名能力：上传/下载都走 store->presign_*，不触碰自签 codec",
          "[phase2][location][c2.6]") {
  Harness h(/*native_presign=*/true, "s3");
  const std::int64_t now = h.clock.NowEpochSeconds();

  const auto upload = h.issuer->IssueUploadLocation("opendes", "osdu-user", std::nullopt, "1H");
  REQUIRE(upload.ok());
  const LocationResult& up = upload.value();
  REQUIRE_FALSE(up.file_id.empty());
  REQUIRE(fss::app::ObjectKeyPolicy::IsValidFileId(up.file_id));
  REQUIRE(StartsWith(up.file_source, "/osdu-user/"));
  REQUIRE(up.file_source.size() > up.file_id.size());
  REQUIRE(up.file_source.compare(up.file_source.size() - up.file_id.size(), up.file_id.size(),
                                 up.file_id) == 0);
  REQUIRE(up.zone == StorageZone::kStaging);
  REQUIRE(up.native_presign);
  REQUIRE(up.driver == "s3");                       // 来自 capabilities().driver_name
  REQUIRE(up.expires_at_epoch_seconds == now + 3600);
  REQUIRE(StartsWith(up.signed_url, "https://storage.invalid/"));

  // 走原生分支 ⇒ 自签 codec 一次都没被调用（调用计数是"真的走了这条路"的证据）
  REQUIRE(h.codec.encode_calls == 0);
  REQUIRE(h.store->presign_put_calls() == 1);
  REQUIRE(h.store->presign_get_calls() == 0);
  REQUIRE(h.store->last_presign_options().method == "PUT");
  REQUIRE(h.store->last_presign_options().expires_in_seconds == 3600);

  // 位置记录 + 物理引用（download 靠它取 ObjectRef）
  const auto location = h.locations.Find("opendes", up.file_id);
  REQUIRE(location.ok());
  REQUIRE_FALSE(location.value().extra["container"].get<std::string>().empty());
  REQUIRE_FALSE(location.value().extra["object_key"].get<std::string>().empty());
  REQUIRE(location.value().extra["driver_name"].get<std::string>() == "s3");

  const auto download = h.issuer->IssueDownloadLocation("opendes", up.file_id, "2H");
  REQUIRE(download.ok());
  REQUIRE(download.value().native_presign);
  REQUIRE(download.value().driver == "s3");
  REQUIRE(download.value().expires_at_epoch_seconds == now + 7200);
  REQUIRE(h.store->presign_get_calls() == 1);
  REQUIRE(h.store->last_presign_options().method == "GET");
  REQUIRE(h.codec.encode_calls == 0);
}

TEST_CASE("★ C2.6 无原生能力：上传/下载都走自签 codec，token 字段完整",
          "[phase2][location][c2.6]") {
  Harness h(/*native_presign=*/false, "posix");
  const std::int64_t now = h.clock.NowEpochSeconds();

  const auto upload = h.issuer->IssueUploadLocation("opendes", "osdu-user", std::nullopt, "1H");
  REQUIRE(upload.ok());
  const LocationResult& up = upload.value();
  REQUIRE_FALSE(up.native_presign);
  REQUIRE(up.driver == "posix");
  REQUIRE(StartsWith(up.signed_url, "https://self.invalid/"));
  REQUIRE(h.codec.encode_calls == 1);
  REQUIRE(h.store->presign_put_calls() == 0);   // 原生分支一次都没走

  const auto& token = h.codec.last_token;
  REQUIRE(token.partition == "opendes");
  REQUIRE(token.file_id == up.file_id);
  REQUIRE(token.zone == StorageZone::kStaging);
  REQUIRE(token.op == "put");
  REQUIRE(token.expires_at_epoch_seconds == now + 3600);
  REQUIRE_FALSE(token.object_key.empty());
  //  ★ 必须**拷贝**：last_token 会被下一次 Encode 覆盖（引用会让后面的断言恒真）
  const auto put_token = token;

  const auto download = h.issuer->IssueDownloadLocation("opendes", up.file_id, std::nullopt);
  REQUIRE(download.ok());
  REQUIRE_FALSE(download.value().native_presign);
  REQUIRE(h.codec.encode_calls == 2);
  REQUIRE(h.codec.last_token.op == "get");
  REQUIRE(h.codec.last_token.object_key == put_token.object_key);
  REQUIRE(h.store->presign_get_calls() == 0);
}

TEST_CASE("★ C2.6 两种能力下的领域结果结构一致 —— 没有驱动类型分支的痕迹",
          "[phase2][location][c2.6]") {
  //  同样的逻辑请求、同样的时钟与 ID 生成器，只有 capabilities 不同
  Harness native_h(/*native_presign=*/true, "s3");
  Harness plain_h(/*native_presign=*/false, "posix");

  const auto a = native_h.issuer->IssueUploadLocation("opendes", "osdu-user", std::nullopt, "1H");
  const auto b = plain_h.issuer->IssueUploadLocation("opendes", "osdu-user", std::nullopt, "1H");
  REQUIRE(a.ok());
  REQUIRE(b.ok());

  //  领域结果中与"驱动类型"无关的部分必须逐字段相等
  REQUIRE(a.value().file_id == b.value().file_id);
  REQUIRE(a.value().file_source == b.value().file_source);
  REQUIRE(a.value().zone == b.value().zone);
  REQUIRE(a.value().expires_at_epoch_seconds == b.value().expires_at_epoch_seconds);
  //  与"能力"有关的部分只体现在 url / native / driver 字符串
  REQUIRE(a.value().native_presign != b.value().native_presign);
  REQUIRE(a.value().driver == "s3");
  REQUIRE(b.value().driver == "posix");
  REQUIRE(a.value().signed_url != b.value().signed_url);
}

TEST_CASE("C2.6 有效期由 ExpiryPolicy 统一决定（缺省 1H / 超限静默截断 7D / 非法固定消息）",
          "[phase2][location][c2.6]") {
  Harness h(false, "posix");
  const std::int64_t now = h.clock.NowEpochSeconds();

  const auto defaulted = h.issuer->IssueUploadLocation("opendes", "u", std::nullopt, std::nullopt);
  REQUIRE(defaulted.ok());
  REQUIRE(defaulted.value().expires_at_epoch_seconds == now + 3600);

  const auto capped = h.issuer->IssueUploadLocation("opendes", "u", std::nullopt, "30D");
  REQUIRE(capped.ok());
  REQUIRE(capped.value().expires_at_epoch_seconds == now + 7 * 86400);  // 静默截断

  const auto bad = h.issuer->IssueUploadLocation("opendes", "u", std::nullopt, "5X");
  REQUIRE_FALSE(bad.ok());
  REQUIRE(bad.error().kind() == fss::ErrorKind::kInvalidArgument);
  REQUIRE(bad.error().message() == std::string(fss::app::kExpiryInvalidMessage));
}

TEST_CASE("LocationIssuer：输入校验、fileID 冲突与缺失位置记录",
          "[phase2][location][c2.6]") {
  Harness h(false, "posix");

  //  客户端指定的 fileID 必须符合契约 §1.5 的正则
  const auto bad_id = h.issuer->IssueUploadLocation("opendes", "u", std::string("a/b"), "1H");
  REQUIRE_FALSE(bad_id.ok());
  REQUIRE(bad_id.error().kind() == fss::ErrorKind::kInvalidArgument);

  //  已存在的 fileID → kLocationAlreadyExists（上游 400 ALREADY_EXISTS，不是 409）
  const auto first = h.issuer->IssueUploadLocation("opendes", "u", std::string("fixed-id-1"), "1H");
  REQUIRE(first.ok());
  const auto dup = h.issuer->IssueUploadLocation("opendes", "u", std::string("fixed-id-1"), "1H");
  REQUIRE_FALSE(dup.ok());
  REQUIRE(dup.error().kind() == fss::ErrorKind::kLocationAlreadyExists);
  REQUIRE(dup.error().message().find("already exists") != std::string::npos);

  //  缺位置记录 → kNotFound（固定消息前缀）
  const auto missing = h.issuer->IssueDownloadLocation("opendes", "no-such-file", "1H");
  REQUIRE_FALSE(missing.ok());
  REQUIRE(missing.error().kind() == fss::ErrorKind::kNotFound);
  REQUIRE(missing.error().message().rfind("Not found location for fileID : ", 0) == 0);

  //  参数校验
  const auto empty_partition = h.issuer->IssueUploadLocation("", "u", std::nullopt, "1H");
  REQUIRE_FALSE(empty_partition.ok());
  REQUIRE(empty_partition.error().kind() == fss::ErrorKind::kInvalidArgument);
  const auto empty_user = h.issuer->IssueUploadLocation("opendes", "", std::nullopt, "1H");
  REQUIRE_FALSE(empty_user.ok());
  REQUIRE(empty_user.error().kind() == fss::ErrorKind::kInvalidArgument);
}

TEST_CASE("LocationIssuer：位置记录缺少物理引用 → kInternal（不静默用坏数据）",
          "[phase2][location][c2.6]") {
  Harness h(false, "posix");
  //  手工塞一条"没有 extra.container / object_key"的记录（模拟旧数据或损坏数据）
  fss::domain::FileLocation legacy;
  legacy.file_id = "legacy-1";
  legacy.file_source = "/u/1-2021-01-01-00-00-00-000/legacy-1";
  legacy.zone = StorageZone::kStaging;
  REQUIRE(h.locations.Save("opendes", legacy).ok());

  const auto r = h.issuer->IssueDownloadLocation("opendes", "legacy-1", "1H");
  REQUIRE_FALSE(r.ok());
  REQUIRE(r.error().kind() == fss::ErrorKind::kInternal);
}

TEST_CASE("LocationIssuer：自签 codec 失败时传播错误（不返回半成品 URL）",
          "[phase2][location][c2.6]") {
  Harness h(false, "posix");
  h.codec.fail = true;
  const auto r = h.issuer->IssueUploadLocation("opendes", "u", std::nullopt, "1H");
  REQUIRE_FALSE(r.ok());
  REQUIRE(r.error().kind() == fss::ErrorKind::kInternal);
}
