// =============================================================================
//  test_roles.cpp —— C6.8：鉴权角色常量与"端点 ↔ 角色"映射
// =============================================================================
//  为什么需要这个文件
//    角色是**字符串字面量**：写错一个字母（`viewers` vs `viewer`）不会编译失败，
//    只会让线上 403 —— 而我们的测试替身默认"全部放行"，所以这类错误在本文件之前
//    **没有任何机械检查**。这里做两件事：
//      ① 9 个常量逐字节断言（含 `service.storage.viewer` / `service.storage.admin`
//         这两个此前**根本没定义**的）；
//      ② 每个用例在入口处请求的角色集合必须等于上游 `@PreAuthorize` 的值 —— 用
//         "一律拒绝"的替身驱动，**不需要构造合法输入**（授权是每个 Execute 的第一步）。
//
//  上游一手依据（vendored 到 `/home/ll/osdu-file-upstream`，commit d7c25c2d）：
//    · `file-core/.../api/FileMetadataApi.java`       POST metadata→EDITORS；GET→VIEWERS；
//                                                     DELETE→`hasPermission(EDITORS, ADMIN)`
//    · `file-core/.../api/FileListApi.java`           getFileList→EDITORS
//    · `file-core/.../api/FileLocationApi.java`       getLocation/getFileLocation/uploadURL→EDITORS
//    · `file-core/.../api/FileDeliveryApi.java`       downloadURL→**VIEWERS**
//    · `file-core/.../api/FileDmsApi.java`            storageInstructions→DATASET_EDITOR；
//                                                     retrievalInstructions→DATASET_VIEWER；
//                                                     copy→`hasPermission(STORAGE_CREATOR, STORAGE_ADMIN)`
//    · `file-core/.../api/FileCollectionDmsApi.java`  同上（三个端点同规则）
//    · `file-core/.../api/DeliveryApi.java`           /v2/delivery/GetFileSignedUrl→DELIVERY_VIEWER
//    · `file-core/.../api/FileAdminApi.java`          /v2/files/revokeURL→ADMIN
//  角色字面值来源：`docs/03-api-contract.md` §1.3（实测自上游 `FileServiceRole` /
//  `DatasetConstants` / `StorageRole` / `DeliveryRole`，其中 `@PreAuthorize` 用到的
//  常量名见上表）。
//
//  ★ 本文件同时断言一条**安全属性**：授权发生在**输入校验之前**（403 先于 400）。
//    否则未授权的调用者能靠错误信息的差异探测数据是否存在（上游 Spring Security
//    的过滤器也在 controller 之前跑）。
// =============================================================================
#include <catch2/catch.hpp>

#include "app_fixture.h"

#include "app/usecases/usecases.h"
#include "domain/ports/ports.h"

#include <functional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace {

using fss::domain::IAuthorizer;
using fss::test::AppFixture;

//  记录每次授权请求（角色集合），并且**一律拒绝** —— 用例会在入口处立即返回，
//  因此不需要喂合法输入就能观测到"这个用例要求什么角色"。
class DenyingRecordingAuthorizer final : public IAuthorizer {
 public:
  fss::Result<void> Authorize(std::string_view required_role, std::string_view partition,
                              std::string_view bearer_token) override {
    (void)partition;
    (void)bearer_token;
    calls.push_back({std::string(required_role)});
    return fss::Err(fss::ErrorKind::kPermissionDenied, "测试替身：一律拒绝");
  }

  fss::Result<void> AuthorizeAny(std::span<const std::string_view> required_roles,
                                 std::string_view partition,
                                 std::string_view bearer_token) override {
    (void)partition;
    (void)bearer_token;
    std::vector<std::string> roles;
    for (const auto role : required_roles) roles.emplace_back(role);
    calls.push_back(std::move(roles));
    return fss::Err(fss::ErrorKind::kPermissionDenied, "测试替身：一律拒绝");
  }

  std::vector<std::vector<std::string>> calls;
  std::size_t call_count() const { return calls.size(); }
};

//  "任一角色"的集合比较：忽略顺序（上游 `hasPermission(a, b)` 的语义与顺序无关）
bool SameRoles(const std::vector<std::string>& actual,
               const std::vector<std::string>& expected) {
  return std::set<std::string>(actual.begin(), actual.end()) ==
         std::set<std::string>(expected.begin(), expected.end());
}

}  // namespace

TEST_CASE("★ C6.8 角色常量逐字节断言（9 个）", "[phase6][unit][c6.8]") {
  //  ★ 期望值直接写死字面量：任何"重命名/拼写调整/大小写调整"都必须先改这里，
  //    也就必须先解释为什么（契约 §1.3 是上游实测值）。
  struct Row {
    const char* name;
    std::string_view actual;
    const char* expected;
  };
  const std::vector<Row> rows = {
      {"FileServiceRole.VIEWERS", fss::domain::kRoleFileViewers, "service.file.viewers"},
      {"FileServiceRole.EDITORS", fss::domain::kRoleFileEditors, "service.file.editors"},
      {"FileServiceRole.ADMIN", fss::domain::kRoleFileAdmin, "service.file.admin"},
      {"DeliveryRole.VIEWER", fss::domain::kRoleDeliveryViewer, "service.delivery.viewer"},
      {"DatasetConstants.DATASET_VIEWER_ROLE", fss::domain::kRoleDatasetViewers,
       "service.dataset.viewers"},
      {"DatasetConstants.DATASET_EDITOR_ROLE", fss::domain::kRoleDatasetEditors,
       "service.dataset.editors"},
      {"StorageRole.VIEWER", fss::domain::kRoleStorageViewer, "service.storage.viewer"},
      {"StorageRole.CREATOR", fss::domain::kRoleStorageCreator, "service.storage.creator"},
      {"StorageRole.ADMIN", fss::domain::kRoleStorageAdmin, "service.storage.admin"},
  };
  REQUIRE(rows.size() == 9);
  std::set<std::string> seen;
  for (const auto& row : rows) {
    INFO(row.name << " = " << row.actual);
    REQUIRE(row.actual == row.expected);
    //  自洽：不得有重复值（重复意味着"两个角色实际是同一个"，必然有一个是错的）
    REQUIRE(seen.insert(std::string(row.actual)).second);
    //  形态：必须是 `service.<组>.<角色>`，且组名只能是契约里那四个
    REQUIRE(row.actual.rfind("service.", 0) == 0);
  }

  //  单数/复数不能互换（`viewers` vs `viewer`）—— 这是最容易写错且最难发现的一处
  REQUIRE(fss::domain::kRoleFileViewers != fss::domain::kRoleStorageViewer);
  REQUIRE(fss::domain::kRoleFileViewers != fss::domain::kRoleDatasetViewers);
  REQUIRE(fss::domain::kRoleDatasetViewers != fss::domain::kRoleDeliveryViewer);

  //  app 层的短名必须是**同一份常量**的别名（单一真相），不得各写一份字面量
  REQUIRE(fss::app::kRoleAdmin == fss::domain::kRoleFileAdmin);
  REQUIRE(fss::app::kRoleDatasetEditors == fss::domain::kRoleDatasetEditors);
  REQUIRE(fss::app::kRoleDatasetViewers == fss::domain::kRoleDatasetViewers);
  REQUIRE(fss::app::kRoleStorageCreator == fss::domain::kRoleStorageCreator);
  REQUIRE(fss::app::kRoleDeliveryViewer == fss::domain::kRoleDeliveryViewer);
  REQUIRE(fss::domain::kRoleViewers == fss::domain::kRoleFileViewers);
  REQUIRE(fss::domain::kRoleEditors == fss::domain::kRoleFileEditors);
}

TEST_CASE("★ C6.8 端点 ↔ 角色映射：每个用例入口请求的角色集合 == 上游 @PreAuthorize",
          "[phase6][unit][c6.8]") {
  //  用"一律拒绝"的作者器：每个 Execute 都在**第一步**授权后立即返回，
  //  因此下面这些参数根本不会被用到（这本身就是一条断言：授权在输入校验之前）。
  struct Row {
    const char* usecase;
    std::vector<std::string> expected_roles;
    std::function<void(AppFixture&, DenyingRecordingAuthorizer&)> drive;
  };

  const std::vector<Row> rows = {
      {"GetUploadLocation", {"service.file.editors"},
       [](AppFixture& fx, DenyingRecordingAuthorizer& a) {
         fx.UseAuthorizer(a);
         fss::app::GetUploadLocation uc(*fx.ports);
         (void)uc.Execute(fx.caller, std::nullopt, std::nullopt);
       }},
      {"GetFileLocation", {"service.file.editors"},
       [](AppFixture& fx, DenyingRecordingAuthorizer& a) {
         fx.UseAuthorizer(a);
         fss::app::GetFileLocation uc(*fx.ports);
         (void)uc.Execute(fx.caller, "unknown-file-id");
       }},
      {"GetDownloadLocation", {"service.file.viewers"},
       [](AppFixture& fx, DenyingRecordingAuthorizer& a) {
         fx.UseAuthorizer(a);
         fss::app::GetDownloadLocation uc(*fx.ports);
         (void)uc.Execute(fx.caller, "unknown-file-id", std::nullopt);
       }},
      {"GetFileList", {"service.file.editors"},
       [](AppFixture& fx, DenyingRecordingAuthorizer& a) {
         fx.UseAuthorizer(a);
         fss::app::GetFileList uc(*fx.ports);
         (void)uc.Execute(fx.caller, fss::app::FileListRequest{});
       }},
      {"CreateFileMetadata", {"service.file.editors"},
       [](AppFixture& fx, DenyingRecordingAuthorizer& a) {
         fx.UseAuthorizer(a);
         fss::app::CreateFileMetadata uc(*fx.ports);
         (void)uc.Execute(fx.caller, fss::domain::FileMetadataRecord{});
       }},
      {"GetFileMetadata", {"service.file.viewers"},
       [](AppFixture& fx, DenyingRecordingAuthorizer& a) {
         fx.UseAuthorizer(a);
         fss::app::GetFileMetadata uc(*fx.ports);
         (void)uc.Execute(fx.caller, "unknown-file-id");
       }},
      //  ★ DELETE：上游 `hasPermission(EDITORS, ADMIN)` —— **任一**即可
      {"DeleteFileMetadata", {"service.file.editors", "service.file.admin"},
       [](AppFixture& fx, DenyingRecordingAuthorizer& a) {
         fx.UseAuthorizer(a);
         fss::app::DeleteFileMetadata uc(*fx.ports);
         (void)uc.Execute(fx.caller, "unknown-file-id");
       }},
      {"GetStorageInstructions", {"service.dataset.editors"},
       [](AppFixture& fx, DenyingRecordingAuthorizer& a) {
         fx.UseAuthorizer(a);
         fss::app::GetStorageInstructions uc(*fx.ports);
         (void)uc.Execute(fx.caller, std::nullopt);
       }},
      {"GetRetrievalInstructions", {"service.dataset.viewers"},
       [](AppFixture& fx, DenyingRecordingAuthorizer& a) {
         fx.UseAuthorizer(a);
         fss::app::GetRetrievalInstructions uc(*fx.ports);
         (void)uc.Execute(fx.caller, std::vector<std::string>{"unknown"}, std::nullopt);
       }},
      //  ★ copy：上游 `hasPermission(STORAGE_CREATOR, STORAGE_ADMIN)` —— **任一**即可
      {"CopyFiles", {"service.storage.creator", "service.storage.admin"},
       [](AppFixture& fx, DenyingRecordingAuthorizer& a) {
         fx.UseAuthorizer(a);
         fss::app::CopyFiles uc(*fx.ports);
         (void)uc.Execute(fx.caller, std::vector<fss::app::CopyFileSource>{});
       }},
      {"GetFileSignedUrl", {"service.delivery.viewer"},
       [](AppFixture& fx, DenyingRecordingAuthorizer& a) {
         fx.UseAuthorizer(a);
         fss::app::GetFileSignedUrl uc(*fx.ports);
         (void)uc.Execute(fx.caller, std::vector<std::string>{"unknown"}, std::nullopt);
       }},
      {"RevokeUrl", {"service.file.admin"},
       [](AppFixture& fx, DenyingRecordingAuthorizer& a) {
         fx.UseAuthorizer(a);
         fss::app::RevokeUrl uc(*fx.ports);
         (void)uc.Execute(fx.caller);
       }},
  };

  for (const auto& row : rows) {
    INFO("用例：" << row.usecase);
    AppFixture fx;
    DenyingRecordingAuthorizer authorizer;
    row.drive(fx, authorizer);

    //  ① 恰好请求了一次授权，且角色集合与上游一致
    REQUIRE(authorizer.call_count() == 1);
    REQUIRE(SameRoles(authorizer.calls.front(), row.expected_roles));
  }
}

TEST_CASE("★ C6.8 授权在输入校验之前（403 先于 400）", "[phase6][unit][c6.8]") {
  //  未授权调用者必须拿到 403，**不能**靠 400 的差异探测数据是否存在。
  //  下面全部传"必然非法"的输入；若某个用例先做校验再授权，这里会拿到别的东西。
  AppFixture fx;
  DenyingRecordingAuthorizer authorizer;
  fx.UseAuthorizer(authorizer);
  std::size_t checked = 0;

  {
    fss::app::CreateFileMetadata uc(*fx.ports);
    const auto r = uc.Execute(fx.caller, fss::domain::FileMetadataRecord{});  // kind 为空
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().kind() == fss::ErrorKind::kPermissionDenied);
    ++checked;
  }
  {
    fss::app::GetFileList uc(*fx.ports);
    fss::app::FileListRequest bad;  // items = 0 → 本来会是 400
    const auto r = uc.Execute(fx.caller, bad);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().kind() == fss::ErrorKind::kPermissionDenied);
    ++checked;
  }
  {
    fss::app::GetFileLocation uc(*fx.ports);
    const auto r = uc.Execute(fx.caller, "");  // 空 id → 本来会是 400
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().kind() == fss::ErrorKind::kPermissionDenied);
    ++checked;
  }
  {
    fss::app::GetFileMetadata uc(*fx.ports);
    const auto r = uc.Execute(fx.caller, "does-not-exist");  // 本来会是 404
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().kind() == fss::ErrorKind::kPermissionDenied);
    ++checked;
  }
  {
    fss::app::DeleteFileMetadata uc(*fx.ports);
    const auto r = uc.Execute(fx.caller, "does-not-exist");  // 本来会是 404
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().kind() == fss::ErrorKind::kPermissionDenied);
    ++checked;
  }

  REQUIRE(checked == 5);
  REQUIRE(authorizer.call_count() == checked);
}
