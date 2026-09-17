// =============================================================================
//  13 个应用层用例的单元测试（C2.2：每个用例都有测试）
// =============================================================================
//  全部用 `AppFixture` 的内存装配驱动：无真实 IO、无真实睡眠（C2.8）。
//  每个用例都覆盖成功路径**与**至少一条失败路径（失败路径的错误分类同时被
//  `test_error_kind_coverage.cpp` 汇总成覆盖率矩阵）。
// =============================================================================
#include <catch2/catch.hpp>

#include "app_fixture.h"

#include "app/services/object_key_policy.h"
#include "app/usecases/usecases.h"
#include "common/bytes/bytes.h"
#include "common/crypto/crypto.h"
#include "domain/model/types.h"

#include <string>
#include <vector>

using fss::app::CallerContext;
using fss::app::GetDownloadLocation;
using fss::app::GetFileList;
using fss::app::GetFileLocation;
using fss::app::GetFileMetadata;
using fss::app::GetFileSignedUrl;
using fss::app::GetInfo;
using fss::app::GetRetrievalInstructions;
using fss::app::GetStorageInstructions;
using fss::app::GetUploadLocation;
using fss::app::CopyFiles;
using fss::app::CopyFileSource;
using fss::app::CreateFileMetadata;
using fss::app::DeleteFileMetadata;
using fss::app::RevokeUrl;
using fss::test::AppFixture;

namespace {

//  走"uploadURL → 写字节"两步，返回 FileSource（后续 CreateFileMetadata 用它定位）
std::string UploadWithContent(AppFixture& fx, const std::string& content) {
  GetUploadLocation upload(*fx.ports);
  const auto up = upload.Execute(fx.caller, std::nullopt, "1H");
  REQUIRE(up.ok());

  const auto location = fx.locations.Find(fx.caller.partition, up.value().file_id);
  REQUIRE(location.ok());
  const auto ref = fss::app::ObjectRefFromLocation(location.value());
  REQUIRE(ref.ok());
  fss::bytes::StringSource source(content);
  REQUIRE(fx.blob.put(ref.value(), source, fss::domain::PutOptions{}).ok());
  return up.value().file_source;
}

std::string CreateMetadataFor(AppFixture& fx, const std::string& content,
                             const std::string& name = "sample.txt") {
  const std::string file_source = UploadWithContent(fx, content);
  CreateFileMetadata create(*fx.ports);
  const auto result = create.Execute(fx.caller, AppFixture::MakeRecord(file_source, name));
  REQUIRE(result.ok());
  return result.value();
}

}  // namespace

TEST_CASE("①~③ GetUploadLocation / GetFileLocation / GetDownloadLocation",
          "[phase2][usecase][c2.2]") {
  AppFixture fx;

  GetUploadLocation upload(*fx.ports);
  const auto up = upload.Execute(fx.caller, std::nullopt, "1H");
  REQUIRE(up.ok());
  REQUIRE(up.value().file_source.rfind("/osdu-user/", 0) == 0);
  REQUIRE_FALSE(up.value().signed_url.empty());

  //  审计真的被写过（"用例走了这条路"的证据）
  REQUIRE(fx.audit.events.size() == 1);
  REQUIRE(fx.audit.events[0].operation == "createLocationSuccess");
  //  ★ C8.7：审计必须带 actor / 对象 / 结果 / 时间 / correlation-id
  REQUIRE(fx.audit.events[0].user == fx.caller.user_id);
  REQUIRE(fx.audit.events[0].partition == fx.caller.partition);
  REQUIRE(fx.audit.events[0].object_id == up.value().file_id);
  REQUIRE(fx.audit.events[0].result == "success");
  REQUIRE(fx.audit.events[0].epoch_millis > 0);
  REQUIRE(fx.audit.events[0].correlation_id == fx.caller.correlation_id);

  //  上传时按契约 §2.1 在 staging 建立了空对象
  const auto location = fx.locations.Find(fx.caller.partition, up.value().file_id);
  REQUIRE(location.ok());
  const auto ref = fss::app::ObjectRefFromLocation(location.value());
  REQUIRE(ref.ok());
  const auto stat = fx.blob.stat(ref.value());
  REQUIRE(stat.ok());
  REQUIRE(stat.value().exists);

  GetFileLocation get_location(*fx.ports);
  const auto view = get_location.Execute(fx.caller, up.value().file_id);
  REQUIRE(view.ok());
  REQUIRE(view.value().file_source == up.value().file_source);
  REQUIRE_FALSE(view.value().location.empty());

  GetDownloadLocation download(*fx.ports);
  const auto dl = download.Execute(fx.caller, up.value().file_id, "2H");
  REQUIRE(dl.ok());
  REQUIRE(dl.value().file_id == up.value().file_id);
  REQUIRE_FALSE(dl.value().signed_url.empty());
  REQUIRE(fx.clock.NowEpochSeconds() + 7200 == dl.value().expires_at_epoch_seconds);

  //  失败：缺位置记录 → kNotFound
  const auto missing = download.Execute(fx.caller, "no-such-file", "1H");
  REQUIRE_FALSE(missing.ok());
  REQUIRE(missing.error().kind() == fss::ErrorKind::kNotFound);
}

TEST_CASE("④ GetFileList：分页 / 用户过滤 / 无记录 → 400", "[phase2][usecase][c2.2]") {
  AppFixture fx;
  GetFileList list(*fx.ports);

  //  空 → 400（契约 §2.5：上游行为不是 200 + 空数组）
  fss::app::FileListRequest request;
  request.items = 10;
  const auto empty = list.Execute(fx.caller, request);
  REQUIRE_FALSE(empty.ok());
  REQUIRE(empty.error().kind() == fss::ErrorKind::kInvalidArgument);

  const std::string first = UploadWithContent(fx, "one");
  fx.clock.AdvanceSeconds(1);
  const std::string second = UploadWithContent(fx, "two");

  const auto page = list.Execute(fx.caller, request);
  REQUIRE(page.ok());
  REQUIRE(page.value().number_of_elements == 2);
  REQUIRE(page.value().number == 0);
  REQUIRE(page.value().size == 10);
  REQUIRE(page.value().content[0].created_by == "osdu-user");
  REQUIRE(page.value().content[0].created_at_epoch_seconds <
          page.value().content[1].created_at_epoch_seconds);

  //  UserID 过滤
  fss::app::FileListRequest other_user = request;
  other_user.user_id = "someone-else";
  const auto none = list.Execute(fx.caller, other_user);
  REQUIRE_FALSE(none.ok());
  REQUIRE(none.error().kind() == fss::ErrorKind::kInvalidArgument);

  //  分页：limit=1 → 第一页 1 条、total 仍为 2
  fss::app::FileListRequest page1 = request;
  page1.items = 1;
  page1.page_num = 0;
  const auto p1 = list.Execute(fx.caller, page1);
  REQUIRE(p1.ok());
  REQUIRE(p1.value().number_of_elements == 1);
  REQUIRE(p1.value().total == 2);
  fss::app::FileListRequest page2 = page1;
  page2.page_num = 1;
  const auto p2 = list.Execute(fx.caller, page2);
  REQUIRE(p2.ok());
  REQUIRE(p2.value().content[0].file_id != p1.value().content[0].file_id);
  REQUIRE_FALSE(first.empty());
  REQUIRE_FALSE(second.empty());

  //  参数校验
  fss::app::FileListRequest bad = request;
  bad.items = 0;
  const auto bad_result = list.Execute(fx.caller, bad);
  REQUIRE_FALSE(bad_result.ok());
  REQUIRE(bad_result.error().kind() == fss::ErrorKind::kInvalidArgument);
}

TEST_CASE("⑤ CreateFileMetadata：复制 + 校验和覆写 + zone 迁移 + staging 清理",
          "[phase2][usecase][c2.2]") {
  AppFixture fx;
  const std::string content = "the quick brown fox";
  const std::string file_source = UploadWithContent(fx, content);

  const auto location_before = fx.locations.FindByFileSource(fx.caller.partition, file_source);
  REQUIRE(location_before.ok());
  const auto staging_ref = fss::app::ObjectRefFromLocation(location_before.value());
  REQUIRE(staging_ref.ok());

  CreateFileMetadata create(*fx.ports);
  const auto created = create.Execute(fx.caller, AppFixture::MakeRecord(file_source));
  REQUIRE(created.ok());
  REQUIRE(created.value().rfind("opendes:dataset--File.Generic:", 0) == 0);

  //  元数据：id/version/校验和被服务端覆写
  const auto record = fx.metadata.GetById(fx.caller.partition, created.value());
  REQUIRE(record.ok());
  REQUIRE(record.value().version == 1);
  REQUIRE(record.value().data.dataset_properties.file_source_info.checksum ==
          fss::crypto::Sha256Hex(content));
  REQUIRE(record.value().data.dataset_properties.file_source_info.checksum_algorithm == "SHA256");

  //  位置记录迁到 persistent，并记下上传者（getFileList 的 UserID）
  const auto location_after = fx.locations.FindByFileSource(fx.caller.partition, file_source);
  REQUIRE(location_after.ok());
  REQUIRE(location_after.value().zone == fss::domain::StorageZone::kPersistent);
  REQUIRE(location_after.value().user_id == "osdu-user");

  //  persistent 有对象、staging 已清理
  const std::string persistent_container =
      fss::app::ObjectKeyPolicy::ContainerFor(fx.caller.partition,
                                              fss::domain::StorageZone::kPersistent)
          .value();
  fss::domain::ObjectRef persistent_ref;
  persistent_ref.container = persistent_container;
  persistent_ref.key = staging_ref.value().key;
  const auto persistent_stat = fx.blob.stat(persistent_ref);
  REQUIRE(persistent_stat.ok());
  REQUIRE(persistent_stat.value().exists);
  const auto staging_stat = fx.blob.stat(staging_ref.value());
  REQUIRE(staging_stat.ok());
  REQUIRE_FALSE(staging_stat.value().exists);

  //  事件：IN_PROGRESS → SUCCESS（非致命，但这里都在）
  REQUIRE(fx.events.events.size() >= 2);
  REQUIRE(fx.events.events.front().status == "IN_PROGRESS");
  REQUIRE(fx.events.events.back().status == "SUCCESS");
}

TEST_CASE("⑤ CreateFileMetadata 失败路径：FileSource 空 / kind 非法 / 复制失败回滚",
          "[phase2][usecase][c2.2]") {
  AppFixture fx;
  CreateFileMetadata create(*fx.ports);

  //  FileSource 空 → 固定 ErrorKind（"FileSource can not be empty"）
  auto bad = AppFixture::MakeRecord("");
  const auto empty_source = create.Execute(fx.caller, bad);
  REQUIRE_FALSE(empty_source.ok());
  REQUIRE(empty_source.error().kind() == fss::ErrorKind::kFileSourceEmpty);

  //  kind 非法 → three fixed messages 之一
  auto bad_kind = AppFixture::MakeRecord("/u/1-ts/f");
  bad_kind.kind = "not-a-kind";
  const auto invalid_kind = create.Execute(fx.caller, bad_kind);
  REQUIRE_FALSE(invalid_kind.ok());
  REQUIRE(invalid_kind.error().kind() == fss::ErrorKind::kInvalidArgument);

  //  FileSource 没有位置记录 → kInvalidSourcePath + 上游固定消息
  //  （上游在存储侧 copy 失败时抛 `INVALID_SOURCE_EXCEPTION + "/" + <path>`；期望报文见
  //   fixtures/upstream/output_payloads/File_invalid_fileSource_msg.json）
  const auto no_location = create.Execute(fx.caller, AppFixture::MakeRecord("/u/9-ts/missing"));
  REQUIRE_FALSE(no_location.ok());
  REQUIRE(no_location.error().kind() == fss::ErrorKind::kInvalidSourcePath);
  REQUIRE(no_location.error().message() ==
          "Invalid source file path to copy from /u/9-ts/missing");

  //  复制失败（staging 对象被删）→ kBadGateway，且**不写**元数据
  const std::string file_source = UploadWithContent(fx, "payload");
  const auto location = fx.locations.FindByFileSource(fx.caller.partition, file_source);
  REQUIRE(location.ok());
  const auto ref = fss::app::ObjectRefFromLocation(location.value());
  REQUIRE(ref.ok());
  REQUIRE(fx.blob.remove(ref.value()).ok());

  const auto copy_failed = create.Execute(fx.caller, AppFixture::MakeRecord(file_source));
  REQUIRE_FALSE(copy_failed.ok());
  REQUIRE(copy_failed.error().kind() == fss::ErrorKind::kBadGateway);
  const auto page = fx.metadata.List(fx.caller.partition, fss::domain::MetadataQuery{});
  REQUIRE(page.ok());
  REQUIRE(page.value().total == 0);
}

TEST_CASE("⑥⑦ GetFileMetadata / DeleteFileMetadata", "[phase2][usecase][c2.2]") {
  AppFixture fx;
  const std::string record_id = CreateMetadataFor(fx, "delete-me");

  GetFileMetadata get(*fx.ports);
  const auto record = get.Execute(fx.caller, record_id);
  REQUIRE(record.ok());
  REQUIRE(record.value().id == record_id);

  //  viewers 角色（契约 §2.7：★ 官方文档误写 editors，以代码为准）
  REQUIRE(fx.authorizer.last_role == std::string(fss::domain::kRoleViewers));

  DeleteFileMetadata remove(*fx.ports);
  REQUIRE(remove.Execute(fx.caller, record_id).ok());
  REQUIRE_FALSE(get.Execute(fx.caller, record_id).ok());
  //  persistent 对象与位置记录也清理了
  const auto page = fx.locations.List(fx.caller.partition, fss::domain::LocationQuery{});
  REQUIRE(page.ok());
  REQUIRE(page.value().total == 0);

  //  删除缺失记录 → kNotFound
  const auto missing = remove.Execute(fx.caller, record_id);
  REQUIRE_FALSE(missing.ok());
  REQUIRE(missing.error().kind() == fss::ErrorKind::kNotFound);
}

TEST_CASE("⑧⑨⑩ DMS：storageInstructions / retrievalInstructions / copy",
          "[phase2][usecase][c2.2]") {
  AppFixture fx;

  GetStorageInstructions storage(*fx.ports);
  const auto instructions = storage.Execute(fx.caller, "1H");
  REQUIRE(instructions.ok());
  REQUIRE(instructions.value().provider_key == "MEMORY");  // 内存测试驱动的 capabilities().driver_name
  REQUIRE_FALSE(instructions.value().signed_url.empty());
  REQUIRE(instructions.value().created_by == "osdu-user");

  const std::string record_id = CreateMetadataFor(fx, "dms-content");
  GetRetrievalInstructions retrieval(*fx.ports);
  const auto datasets = retrieval.Execute(fx.caller, {record_id, "no-such-record"}, "1H");
  REQUIRE(datasets.ok());
  REQUIRE(datasets.value().size() == 1);            // 找不到的被跳过
  REQUIRE(datasets.value()[0].dataset_registry_id == record_id);
  REQUIRE(datasets.value()[0].provider_key == "MEMORY");
  REQUIRE_FALSE(datasets.value()[0].signed_url.empty());

  //  copy：合法 source 成功；非法 source → kInvalidSourcePath
  const std::string file_source = UploadWithContent(fx, "copy-me");
  CopyFiles copy(*fx.ports);
  const auto copied = copy.Execute(fx.caller, {CopyFileSource{file_source}});
  REQUIRE(copied.ok());
  REQUIRE(copied.value().size() == 1);
  REQUIRE(copied.value()[0].success);
  REQUIRE_FALSE(copied.value()[0].dataset_blob_storage_path.empty());

  const auto bad = copy.Execute(fx.caller, {CopyFileSource{"../etc/passwd"}});
  REQUIRE_FALSE(bad.ok());
  REQUIRE(bad.error().kind() == fss::ErrorKind::kInvalidSourcePath);
}

TEST_CASE("⑪ GetFileSignedUrl：processed / unprocessed", "[phase2][usecase][c2.2]") {
  AppFixture fx;
  GetUploadLocation upload(*fx.ports);
  const auto up = upload.Execute(fx.caller, std::nullopt, "1H");
  REQUIRE(up.ok());

  GetFileSignedUrl signed_url(*fx.ports);
  const auto result = signed_url.Execute(
      fx.caller, {"srn:file/" + up.value().file_id, "srn:file/unknown", "not-an-srn"}, "1H");
  REQUIRE(result.ok());
  REQUIRE(result.value().processed.size() == 1);
  REQUIRE(result.value().processed.count("srn:file/" + up.value().file_id) == 1);
  REQUIRE(result.value().unprocessed.size() == 2);
}

TEST_CASE("⑫⑬ RevokeUrl（不要求 partition） / GetInfo（无鉴权）", "[phase2][usecase][c2.2]") {
  AppFixture fx;
  RevokeUrl revoke(*fx.ports);
  CallerContext no_partition{"", "osdu-user", "Bearer token"};
  REQUIRE(revoke.Execute(no_partition).ok());        // 契约 §2.11：不需要 data-partition-id
  REQUIRE(fx.audit.events.back().operation == "revokeUrlSuccess");

  GetInfo info(*fx.ports);
  const auto version = info.Execute();
  REQUIRE(version.ok());
  REQUIRE(version.value().version == "v2");
  REQUIRE_FALSE(version.value().build_version.empty());
}

TEST_CASE("授权：缺 token / 缺 partition / 角色不足（契约 §1.2/§1.3/§5）",
          "[phase2][usecase][c2.2]") {
  AppFixture fx;
  GetFileList list(*fx.ports);
  fss::app::FileListRequest request;

  //  缺 token → 401 "Missing authorization token"
  CallerContext no_token{"opendes", "osdu-user", ""};
  const auto unauthenticated = list.Execute(no_token, request);
  REQUIRE_FALSE(unauthenticated.ok());
  REQUIRE(unauthenticated.error().kind() == fss::ErrorKind::kUnauthenticated);
  REQUIRE(unauthenticated.error().message() == "Missing authorization token");

  //  缺 partition → 401 "Missing partitionID"
  CallerContext no_partition{"", "osdu-user", "Bearer token"};
  const auto no_part = list.Execute(no_partition, request);
  REQUIRE_FALSE(no_part.ok());
  REQUIRE(no_part.error().kind() == fss::ErrorKind::kUnauthenticated);
  REQUIRE(no_part.error().message() == "Missing partitionID");

  //  角色不足 → 403
  fx.authorizer.deny = true;
  const auto denied = list.Execute(fx.caller, request);
  REQUIRE_FALSE(denied.ok());
  REQUIRE(denied.error().kind() == fss::ErrorKind::kPermissionDenied);
}
