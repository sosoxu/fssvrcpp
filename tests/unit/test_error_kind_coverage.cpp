// =============================================================================
//  C2.2：ErrorKind 覆盖率矩阵（每个取值至少一个触发用例，无空缺）
// =============================================================================
//  "每个 ErrorKind 至少一个触发用例"这条判据的目的不是凑数，而是防止
//  "错误分类只写了枚举、没有任何代码真的产生它" —— 那样契约 §5 的映射表就是
//  一纸空文（映射到不存在的分支）。
//
//  本测试按**取值范围**（而不是按端点）逐个触发，最后打印矩阵并断言无空缺。
//  刻意**不**把 `kOk` 算作错误。
// =============================================================================
#include <catch2/catch.hpp>

#include "app_fixture.h"

#include "app/services/object_key_policy.h"
#include "app/usecases/usecases.h"
#include "common/bytes/bytes.h"
#include "common/crypto/crypto.h"
#include "domain/model/types.h"

#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using fss::ErrorKind;

//  契约 §5 映射表的**全部**错误取值（顺序与 result.h 一致；kOk 不是错误）
const std::vector<std::pair<ErrorKind, std::string>>& AllErrorKinds() {
  static const std::vector<std::pair<ErrorKind, std::string>> kinds = {
      {ErrorKind::kInvalidArgument, "kInvalidArgument"},
      {ErrorKind::kFileSourceEmpty, "kFileSourceEmpty"},
      {ErrorKind::kInvalidSourcePath, "kInvalidSourcePath"},
      {ErrorKind::kLocationAlreadyExists, "kLocationAlreadyExists"},
      {ErrorKind::kChecksumMismatch, "kChecksumMismatch"},
      {ErrorKind::kUnauthenticated, "kUnauthenticated"},
      {ErrorKind::kPermissionDenied, "kPermissionDenied"},
      {ErrorKind::kStorageAccessDenied, "kStorageAccessDenied"},
      {ErrorKind::kNotFound, "kNotFound"},
      {ErrorKind::kUnimplemented, "kUnimplemented"},
      {ErrorKind::kInternal, "kInternal"},
      {ErrorKind::kBadGateway, "kBadGateway"},
      {ErrorKind::kUnavailable, "kUnavailable"},
  };
  return kinds;
}

//  记录"这条错误是被哪个用例触发的"（矩阵的可读性来自这里）
class Matrix {
 public:
  void Observe(const fss::Result<void>& result, const std::string& trigger) {
    if (!result.ok()) seen_[result.error().kind()] = trigger;
  }
  template <typename T>
  void Observe(const fss::Result<T>& result, const std::string& trigger) {
    if (!result.ok()) seen_[result.error().kind()] = trigger;
  }

  bool Has(ErrorKind kind) const { return seen_.count(kind) == 1; }
  std::string TriggerOf(ErrorKind kind) const {
    const auto it = seen_.find(kind);
    return it == seen_.end() ? std::string("（空缺）") : it->second;
  }

 private:
  std::map<ErrorKind, std::string> seen_;
};

std::string UploadWithContent(fss::test::AppFixture& fx, const std::string& content) {
  fss::app::GetUploadLocation upload(*fx.ports);
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

}  // namespace

TEST_CASE("★ C2.2 每个 ErrorKind 都有触发用例（覆盖率矩阵无空缺）",
          "[phase2][usecase][c2.2]") {
  fss::test::AppFixture fx;
  Matrix matrix;

  //  ---- kUnauthenticated（缺 token）与 kPermissionDenied ----
  fss::app::GetFileList list(*fx.ports);
  fss::app::FileListRequest list_request;
  fss::app::CallerContext no_token{"opendes", "osdu-user", ""};
  matrix.Observe(list.Execute(no_token, list_request), "GetFileList: 缺 token");
  fx.authorizer.deny = true;
  matrix.Observe(list.Execute(fx.caller, list_request), "GetFileList: 角色不足");
  fx.authorizer.deny = false;

  //  ---- kInvalidArgument ----
  fss::app::FileListRequest bad_items;
  bad_items.items = 0;
  matrix.Observe(list.Execute(fx.caller, bad_items), "GetFileList: Items<=0");

  //  ---- kNotFound ----
  fss::app::GetFileMetadata get_metadata(*fx.ports);
  matrix.Observe(get_metadata.Execute(fx.caller, "no-such-record"), "GetFileMetadata: 缺失");

  //  ---- kFileSourceEmpty ----
  fss::app::CreateFileMetadata create(*fx.ports);
  matrix.Observe(create.Execute(fx.caller, fss::test::AppFixture::MakeRecord("")),
                 "CreateFileMetadata: FileSource 空");

  //  ---- kInvalidSourcePath ----
  fss::app::CopyFiles copy(*fx.ports);
  matrix.Observe(copy.Execute(fx.caller, {fss::app::CopyFileSource{"relative/path"}}),
                 "CopyFiles: 非法源路径");

  //  ---- kLocationAlreadyExists ----
  fss::app::GetUploadLocation upload(*fx.ports);
  REQUIRE(upload.Execute(fx.caller, std::string("fixed-id-cov"), "1H").ok());
  matrix.Observe(upload.Execute(fx.caller, std::string("fixed-id-cov"), "1H"),
                 "GetUploadLocation: fileID 已存在");

  //  ---- kUnimplemented（内存存储没有原生预签名） ----
  {
    fss::domain::ObjectRef ref;
    ref.container = "opendes-staging";
    ref.key = "x";
    fss::domain::PresignOptions options;
    matrix.Observe(fx.blob.presign_get(ref, options), "InMemoryBlobStore: native_presign=false");
  }

  //  ---- kUnavailable（存储工厂不可用） ----
  fx.factory.fail = true;
  matrix.Observe(upload.Execute(fx.caller, std::nullopt, "1H"), "GetUploadLocation: 存储不可用");
  fx.factory.fail = false;

  //  ---- kInternal（位置记录缺少物理引用） ----
  {
    fss::domain::FileLocation legacy;
    legacy.file_id = "legacy-cov";
    legacy.file_source = "/osdu-user/1-ts/legacy-cov";
    legacy.zone = fss::domain::StorageZone::kStaging;
    REQUIRE(fx.locations.Save(fx.caller.partition, legacy).ok());
    fss::app::GetDownloadLocation download(*fx.ports);
    matrix.Observe(download.Execute(fx.caller, "legacy-cov", "1H"),
                   "GetDownloadLocation: 记录缺少物理引用");
  }

  //  ---- kBadGateway（复制到 persistent 失败） ----
  {
    const std::string file_source = UploadWithContent(fx, "will-fail");
    const auto location = fx.locations.FindByFileSource(fx.caller.partition, file_source);
    REQUIRE(location.ok());
    const auto ref = fss::app::ObjectRefFromLocation(location.value());
    REQUIRE(ref.ok());
    REQUIRE(fx.blob.remove(ref.value()).ok());
    matrix.Observe(create.Execute(fx.caller, fss::test::AppFixture::MakeRecord(file_source)),
                   "CreateFileMetadata: staging→persistent 复制失败");
  }

  //  ---- kChecksumMismatch（put 的期望校验和不符） ----
  {
    fss::domain::ObjectRef ref;
    ref.container = "opendes-staging";
    ref.key = "checksum-cov";
    REQUIRE(fx.blob.ensure_container(ref.container).ok());
    fss::bytes::StringSource source("payload");
    fss::domain::PutOptions options;
    options.expected_checksum = fss::crypto::Sha256Hex("something else");
    options.checksum_algorithm = "SHA256";
    matrix.Observe(fx.blob.put(ref, source, options), "InMemoryBlobStore: expected_checksum 不符");
  }

  //  ---- kStorageAccessDenied（存储侧拒绝：AccessDenied / SignatureDoesNotMatch） ----
  //  ★ 这条**不能**由内存适配器自然产生：它是"依赖（存储）拒绝了本服务"，
  //    典型来源是 S3（P5）。这里用注入了错误的端口替身走**真实用例路径**
  //    （GetDownloadLocation → LocationIssuer → store.presign_get），证明应用层会把它
  //    **原样**透出，而不是折叠成 kInternal —— 否则错误分类在适配层就丢了。
  {
    fss::test::CapabilityOverrideBlobStore denying(
        fx.blob, [] {
          fss::domain::BlobCapabilities caps;
          caps.native_presign = true;
          caps.driver_name = "s3";
          return caps;
        }(),
        "http://denied.invalid");
    denying.presign_error = fss::Error(fss::ErrorKind::kStorageAccessDenied,
                                       "S3 AccessDenied（凭证/桶策略拒绝）");
    fss::test::FakeBlobStoreFactory denying_factory(denying);
    const std::string file_source = UploadWithContent(fx, "denied-presign");
    const auto location = fx.locations.FindByFileSource(fx.caller.partition, file_source);
    REQUIRE(location.ok());

    fss::app::LocationIssuer issuer(denying_factory, fx.locations, fx.codec, fx.clock, fx.ids,
                                    "http://self.invalid");
    const auto denied = issuer.IssueDownloadLocation(fx.caller.partition, location.value().file_id,
                                                     "1H");
    REQUIRE_FALSE(denied.ok());
    matrix.Observe(denied, "LocationIssuer: 存储侧拒绝预签名（S3 AccessDenied）");
  }

  //  ---- 打印矩阵 + 断言无空缺 ----
  std::cout << "\n  ErrorKind 覆盖率矩阵（C2.2）\n";
  std::cout << "  ------------------------------------------------\n";
  std::size_t covered = 0;
  for (const auto& [kind, name] : AllErrorKinds()) {
    const bool has = matrix.Has(kind);
    if (has) ++covered;
    std::cout << "  " << (has ? "✓" : "✗") << " " << name;
    for (std::size_t pad = name.size(); pad < 24; ++pad) std::cout << ' ';
    std::cout << matrix.TriggerOf(kind) << "\n";
  }
  std::cout << "  ------------------------------------------------\n";
  std::cout << "  " << covered << "/" << AllErrorKinds().size() << " 已覆盖\n";

  for (const auto& [kind, name] : AllErrorKinds()) {
    INFO("ErrorKind 无触发用例：" << name);
    REQUIRE(matrix.Has(kind));
  }
  REQUIRE(covered == AllErrorKinds().size());
}
