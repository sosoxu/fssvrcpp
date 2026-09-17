// =============================================================================
//  P8 切片 3：审计覆盖（C8.7）—— **每个受保护端点在成功与失败两侧都有审计记录**
// =============================================================================
//  判据要求审计记录含：actor（user）、对象（object_id）、结果（result）、时间
//  （epoch_millis）、correlation-id。
//
//  为什么必须表驱动地"逐个用例"过一遍：
//    "审计写了吗"很容易只在一个端点上验证，然后假定其它端点也一样 —— 实际上
//    `RecordAudit` 是手写在每个 `Execute` 里的调用，**漏一条不会报错、不会失败**。
//    本轮把它们统一成 RAII 守卫（`AuditGuard`，任何提前 return 都会记账），
//    再在这里逐个钉住两件事：①成功侧有记录；②失败侧也有记录。
//
//  失败侧怎么构造：给所有用例换一个**拒绝一切**的授权器（`AllowAllAuthorizer::deny`）——
//  这是唯一对所有 15 个受保护用例都成立的失败路径，因此它能机械地覆盖"每个端点"。
//  少数端点另加一条领域失败（如 `kNotFound`）作为非授权类失败的样本。
// =============================================================================
#include <catch2/catch.hpp>

#include "app_fixture.h"

#include <functional>
#include <string>
#include <vector>

namespace {

using fss::test::AppFixture;

constexpr char kCorrelationId[] = "corr-c8.7";

//  ★ 不要写"返回 AppFixture 的工厂"：`ManualClock` 含 `std::atomic`，AppFixture 不可移动
void Prepare(AppFixture& fx) { fx.caller.correlation_id = kCorrelationId; }

//  ---- 准备数据：一条已上传字节、已登记元数据的记录 ----
struct Seeded {
  std::string file_id;
  std::string file_source;
  std::string record_id;
};

Seeded Seed(AppFixture& fx) {
  Seeded seeded;
  {
    fss::app::GetUploadLocation upload(*fx.ports);
    const auto result = upload.Execute(fx.caller, std::nullopt, "1H");
    REQUIRE(result.ok());
    seeded.file_id = result.value().file_id;
    seeded.file_source = result.value().file_source;
  }
  {
    fss::app::UploadFile upload(*fx.ports);
    fss::app::UploadStreamRequest request;
    request.file_source = seeded.file_source;
    fss::bytes::StringSource body("audit-coverage-payload");
    const auto result = upload.Execute(fx.caller, request, body);
    REQUIRE(result.ok());
  }
  {
    fss::app::CreateFileMetadata create(*fx.ports);
    const auto record = AppFixture::MakeRecord(seeded.file_source, "audit.bin");
    const auto result = create.Execute(fx.caller, record);
    REQUIRE(result.ok());
    seeded.record_id = result.value();
  }
  return seeded;
}

//  ---- 审计断言：字段必须齐全（C8.7 的"含 actor、对象、结果、时间、correlation-id"）----
void CheckAuditSide(AppFixture& fx, const std::string& expected_operation,
                    const std::string& expected_result, bool require_object_id) {
  const auto& events = fx.audit.events;
  REQUIRE_FALSE(events.empty());
  const auto& event = events.back();
  CAPTURE(expected_operation, expected_result, events.size());
  REQUIRE(event.operation == expected_operation);
  REQUIRE(event.result == expected_result);
  REQUIRE(event.user == fx.caller.user_id);
  REQUIRE(event.partition == fx.caller.partition);
  REQUIRE(event.epoch_millis > 0);
  REQUIRE(event.correlation_id == kCorrelationId);
  if (require_object_id) REQUIRE_FALSE(event.object_id.empty());
}

//  一次"成功调用 + 一次失败调用"的样本
struct Case {
  std::string name;
  std::string operation;      // 不带 Success/Failure 后缀
  bool object_id_known;       // 成功侧是否应带对象 id
  std::function<void(AppFixture&, const Seeded&)> run_success;
};

std::vector<Case> Cases() {
  return {
      {"GetUploadLocation", "createLocation", true,
       [](AppFixture& fx, const Seeded&) {
         fss::app::GetUploadLocation usecase(*fx.ports);
         REQUIRE(usecase.Execute(fx.caller, std::nullopt, "1H").ok());
       }},
      {"GetFileLocation", "readFileLocation", true,
       [](AppFixture& fx, const Seeded& seeded) {
         fss::app::GetFileLocation usecase(*fx.ports);
         REQUIRE(usecase.Execute(fx.caller, seeded.file_id).ok());
       }},
      {"GetDownloadLocation", "createDownloadLocation", true,
       [](AppFixture& fx, const Seeded& seeded) {
         fss::app::GetDownloadLocation usecase(*fx.ports);
         REQUIRE(usecase.Execute(fx.caller, seeded.file_id, "1H").ok());
       }},
      {"GetFileList", "getFileList", false,
       [](AppFixture& fx, const Seeded&) {
         fss::app::GetFileList usecase(*fx.ports);
         fss::app::FileListRequest request;
         request.items = 10;
         REQUIRE(usecase.Execute(fx.caller, request).ok());
       }},
      {"CreateFileMetadata", "createMetadata", true,
       [](AppFixture& fx, const Seeded&) {
         fss::app::GetUploadLocation upload(*fx.ports);
         const auto uploaded = upload.Execute(fx.caller, std::nullopt, "1H");
         REQUIRE(uploaded.ok());
         fss::app::CreateFileMetadata usecase(*fx.ports);
         const auto record = AppFixture::MakeRecord(uploaded.value().file_source, "second.bin");
         REQUIRE(usecase.Execute(fx.caller, record).ok());
       }},
      {"GetFileMetadata", "readMetadata", true,
       [](AppFixture& fx, const Seeded& seeded) {
         fss::app::GetFileMetadata usecase(*fx.ports);
         REQUIRE(usecase.Execute(fx.caller, seeded.record_id).ok());
       }},
      {"DeleteFileMetadata", "deleteMetadata", true,
       [](AppFixture& fx, const Seeded& seeded) {
         fss::app::DeleteFileMetadata usecase(*fx.ports);
         REQUIRE(usecase.Execute(fx.caller, seeded.record_id).ok());
       }},
      {"GetStorageInstructions", "getStorageInstructions", true,
       [](AppFixture& fx, const Seeded&) {
         fss::app::GetStorageInstructions usecase(*fx.ports);
         REQUIRE(usecase.Execute(fx.caller, "1H").ok());
       }},
      {"GetRetrievalInstructions", "getRetrievalInstructions", false,
       [](AppFixture& fx, const Seeded& seeded) {
         fss::app::GetRetrievalInstructions usecase(*fx.ports);
         REQUIRE(usecase.Execute(fx.caller, {seeded.record_id}, "1H").ok());
       }},
      {"CopyFiles", "copyFiles", false,
       [](AppFixture& fx, const Seeded& seeded) {
         fss::app::CopyFiles usecase(*fx.ports);
         const auto outcomes = usecase.Execute(fx.caller, {fss::app::CopyFileSource{seeded.file_source}});
         REQUIRE(outcomes.ok());
       }},
      {"GetFileSignedUrl", "getFileSignedUrl", false,
       [](AppFixture& fx, const Seeded& seeded) {
         fss::app::GetFileSignedUrl usecase(*fx.ports);
         const auto result =
             usecase.Execute(fx.caller, {"srn:file/" + seeded.record_id}, "1H");
         REQUIRE(result.ok());
       }},
      {"RevokeUrl", "revokeUrl", false,
       [](AppFixture& fx, const Seeded&) {
         fss::app::RevokeUrl usecase(*fx.ports);
         REQUIRE(usecase.Execute(fx.caller).ok());
       }},
      {"UploadFile", "uploadFile", true,
       [](AppFixture& fx, const Seeded& seeded) {
         fss::app::UploadFile usecase(*fx.ports);
         fss::app::UploadStreamRequest request;
         request.file_source = seeded.file_source;
         fss::bytes::StringSource body("again");
         REQUIRE(usecase.Execute(fx.caller, request, body).ok());
       }},
      {"DownloadFile", "downloadFile", true,
       [](AppFixture& fx, const Seeded& seeded) {
         fss::app::DownloadFile usecase(*fx.ports);
         fss::bytes::StringSink sink;
         REQUIRE(usecase.Execute(fx.caller, seeded.file_id, "", 0, 0, sink).ok());
       }},
      {"ServerSideCopy", "serverSideCopy", true,
       [](AppFixture& fx, const Seeded& seeded) {
         fss::app::ServerSideCopy usecase(*fx.ports);
         const auto result = usecase.Execute(
             fx.caller, seeded.file_source,
             "/osdu-user/1700000000000-2023-11-14-22-13-20-000/copied", fss::domain::StorageZone::kPersistent);
         REQUIRE(result.ok());
       }},
  };
}

}  // namespace

TEST_CASE("★ C8.7 每个受保护用例在成功侧都留下完整审计（actor/对象/结果/时间/correlation-id）",
          "[phase8][unit][c8.7]") {
  for (const auto& test_case : Cases()) {
    INFO("用例：" << test_case.name);
    AppFixture fx;
    Prepare(fx);
    const auto seeded = Seed(fx);
    const std::size_t before = fx.audit.events.size();

    test_case.run_success(fx, seeded);

    REQUIRE(fx.audit.events.size() > before);  // 成功侧必须新增审计
    CheckAuditSide(fx, test_case.operation + "Success", "success", test_case.object_id_known);
  }
}

TEST_CASE("★ C8.7 失败侧的审计：授权失败 / 领域失败（kNotFound）各有记录",
          "[phase8][unit][c8.7]") {
  //  ① 授权失败：每个受保护用例的第一个分支
  {
    AppFixture fx;
    Prepare(fx);
    Seed(fx);
    fss::test::AllowAllAuthorizer denying;
    denying.deny = true;
    fx.UseAuthorizer(denying);

    //  逐个用例用"拒绝一切"的授权器调用（全部返回 !ok），断言失败侧审计
    const auto denied_calls = [&](const std::string& operation, auto&& call) {
      const std::size_t before = fx.audit.events.size();
      const auto result = call();
      INFO("被拒用例：" << operation);
      REQUIRE_FALSE(result.ok());
      REQUIRE(result.error().kind() == fss::ErrorKind::kPermissionDenied);
      REQUIRE(fx.audit.events.size() > before);
      CheckAuditSide(fx, operation + "Failure", "failure", /*require_object_id=*/false);
    };

    denied_calls("createLocation", [&] {
      fss::app::GetUploadLocation usecase(*fx.ports);
      return usecase.Execute(fx.caller, std::nullopt, "1H");
    });
    denied_calls("readFileLocation", [&] {
      fss::app::GetFileLocation usecase(*fx.ports);
      return usecase.Execute(fx.caller, "some-id");
    });
    denied_calls("createDownloadLocation", [&] {
      fss::app::GetDownloadLocation usecase(*fx.ports);
      return usecase.Execute(fx.caller, "some-id", "1H");
    });
    denied_calls("getFileList", [&] {
      fss::app::GetFileList usecase(*fx.ports);
      fss::app::FileListRequest request;
      request.items = 10;
      return usecase.Execute(fx.caller, request);
    });
    denied_calls("createMetadata", [&] {
      fss::app::CreateFileMetadata usecase(*fx.ports);
      return usecase.Execute(fx.caller, AppFixture::MakeRecord("/x/1/2", "x.bin"));
    });
    denied_calls("readMetadata", [&] {
      fss::app::GetFileMetadata usecase(*fx.ports);
      return usecase.Execute(fx.caller, "some-id");
    });
    denied_calls("deleteMetadata", [&] {
      fss::app::DeleteFileMetadata usecase(*fx.ports);
      return usecase.Execute(fx.caller, "some-id");
    });
    denied_calls("getStorageInstructions", [&] {
      fss::app::GetStorageInstructions usecase(*fx.ports);
      return usecase.Execute(fx.caller, "1H");
    });
    denied_calls("getRetrievalInstructions", [&] {
      fss::app::GetRetrievalInstructions usecase(*fx.ports);
      return usecase.Execute(fx.caller, {"some-id"}, "1H");
    });
    denied_calls("copyFiles", [&] {
      fss::app::CopyFiles usecase(*fx.ports);
      return usecase.Execute(fx.caller, {fss::app::CopyFileSource{"/x/1/2"}});
    });
    denied_calls("getFileSignedUrl", [&] {
      fss::app::GetFileSignedUrl usecase(*fx.ports);
      return usecase.Execute(fx.caller, {"srn:file/some-id"}, "1H");
    });
    denied_calls("revokeUrl", [&] {
      fss::app::RevokeUrl usecase(*fx.ports);
      return usecase.Execute(fx.caller);
    });
    denied_calls("uploadFile", [&] {
      fss::app::UploadFile usecase(*fx.ports);
      fss::app::UploadStreamRequest request;
      request.file_source = "/x/1/2";
      fss::bytes::StringSource body("x");
      return usecase.Execute(fx.caller, request, body);
    });
    denied_calls("downloadFile", [&] {
      fss::app::DownloadFile usecase(*fx.ports);
      fss::bytes::StringSink sink;
      return usecase.Execute(fx.caller, "some-id", "", 0, 0, sink);
    });
    denied_calls("serverSideCopy", [&] {
      fss::app::ServerSideCopy usecase(*fx.ports);
      return usecase.Execute(fx.caller, "/x/1/2", "/y/3/4", fss::domain::StorageZone::kPersistent);
    });
  }

  //  ② 领域失败（非授权类）：`kNotFound` 也要留下 failure 审计，且带对象 id
  {
    AppFixture fx;
    Prepare(fx);
    const std::size_t before = fx.audit.events.size();
    fss::app::GetFileMetadata usecase(*fx.ports);
    const auto result = usecase.Execute(fx.caller, "opendes:dataset--File.Generic:missing");
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().kind() == fss::ErrorKind::kNotFound);
    REQUIRE(fx.audit.events.size() == before + 1);
    CheckAuditSide(fx, "readMetadataFailure", "failure", /*require_object_id=*/true);
  }
}
