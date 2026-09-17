// =============================================================================
//  test_file_list.cpp —— C6.6：`POST /api/file/v2/getFileList` 的 Spring Page 语义
// =============================================================================
//  判据 C6.6 的四条：分页字段名精确、`PageNum=0` 与 `1` 结果不重叠、时间区间过滤正确、
//  无记录 → `400`。契约依据：`docs/03-api-contract.md` §2.5。
//
//  ★ 上游一手依据（vendored 到 `tests/conformance/fixtures/upstream/list/`）：
//    `File_GetList_EmptyPayload.json`（`{}`）、`File_GetList_InvalidPayload.json`（缺 `Items`）、
//    `File_GetList_NoRecordPayload.json`（完整请求但库中无匹配）——**三条上游期望都是 400**。
//    本文件用**逐字**的这三个 fixture 驱动，而不是自己编造请求体。
//
//  ★ 一处**刻意的放宽**（已在契约 §2.5 登记）：上游参考实现的校验器把
//    `TimeFrom`/`TimeTo`/`UserID` 定为必填（`ValidationServiceTest#fileListRequestProvider`），
//    本实现对这三者"缺省 = 不过滤"。上游**验收样例**没有覆盖这个差异（它们的"非法"样例
//    缺的是 `Items`），所以本文件对它们只断言"给了就按它过滤"，不断言"不给就 400"。
//
//  ★ 时间边界**含端点**（`>= TimeFrom` 且 `<= TimeTo`）——语义由
//    `domain::LocationQuery` 的注释与 `port_contract.h` 钉住（memory 与 SQLite 两个实现一致）。
// =============================================================================
#include <catch2/catch.hpp>

#include "http_fixture.h"

#include "common/time/time_format.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace {

using fss::test::AppFixture;
using fss::test::HttpFixture;

//  上传一份内容并登记元数据，返回记录 id（`x-user-id` 决定位置记录的 CreatedBy）
struct CreatedRecord {
  std::string file_id;
  std::string file_source;
  std::string record_id;
};

CreatedRecord CreateRecord(HttpFixture& fx, const std::string& user_id,
                           const std::string& name = "list.bin") {
  const int port = fx.port();
  auto headers = fss::test::Authed();
  headers.push_back("x-user-id: " + user_id);

  const auto upload = fss::test::HttpDo(port, "GET", "/api/file/v2/files/uploadURL", headers);
  REQUIRE(upload.status == 200);
  const auto upload_json = fss::json::ParseObject(upload.body);
  REQUIRE(upload_json.ok());
  CreatedRecord out;
  out.file_id = upload_json.value()["FileID"].get<std::string>();
  out.file_source = upload_json.value()["Location"]["FileSource"].get<std::string>();

  const auto put = fss::test::HttpDo(
      port, "PUT",
      fss::test::TargetOf(upload_json.value()["Location"]["SignedURL"].get<std::string>()),
      headers, "payload-for-" + name);
  REQUIRE(put.status == 200);

  auto record = AppFixture::MakeRecord(out.file_source, name);
  const auto created = fss::test::HttpDo(port, "POST", "/api/file/v2/files/metadata", headers,
                                       fss::json::Dump(fss::domain::ToJson(record)));
  INFO("POST metadata → " << created.status << " " << created.body);
  REQUIRE(created.status == 201);
  out.record_id = fss::json::ParseObject(created.body).value()["id"].get<std::string>();
  return out;
}

//  发一次 getFileList（`body` 是 JSON 文本）
fss::test::Reply List(HttpFixture& fx, const std::string& body,
                      const std::string& user_id = "osdu-user") {
  auto headers = fss::test::Authed();
  if (!user_id.empty()) headers.push_back("x-user-id: " + user_id);
  return fss::test::HttpDo(fx.port(), "POST", "/api/file/v2/getFileList", headers, body);
}

std::string Iso(std::int64_t epoch) { return fss::time::ToIso8601Utc(epoch, 0); }

//  逐字读取 vendored 的上游验收 payload（路径约定与 phase4 的 `ReadFixture` 相同）
std::string ReadUpstreamFixture(const char* relative) {
  const std::string path =
      std::string(FSS_REPO_ROOT) + "/tests/conformance/fixtures/upstream/" + relative;
  std::ifstream input(path);
  REQUIRE(input.good());  // 前置条件显式断言（R9）：路径写错时给出可执行的修复指令
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::set<std::string> KeysOf(const fss::json::Value& object) {
  std::set<std::string> keys;
  for (auto it = object.begin(); it != object.end(); ++it) keys.insert(it.key());
  return keys;
}

std::vector<std::string> IdsOf(const fss::json::Value& page) {
  std::vector<std::string> ids;
  for (const auto& entry : page["Content"]) ids.push_back(entry["FileID"].get<std::string>());
  return ids;
}

}  // namespace

TEST_CASE("★ C6.6 响应字段名精确 + `CreatedAt` 格式 + 单条内容", "[phase6][integration][c6.6]") {
  HttpFixture fx;
  const auto record = CreateRecord(fx, "osdu-user", "one.bin");

  const auto reply = List(fx, R"({"PageNum":0,"Items":10})");
  INFO("响应: " << reply.body);
  REQUIRE(reply.status == 200);
  const auto page = fss::json::ParseObject(reply.body);
  REQUIRE(page.ok());

  //  ★ 字段名**精确**：多一个少一个都算不兼容（`results`/`totalCount` 是最常见的错法）
  REQUIRE(KeysOf(page.value()) ==
          std::set<std::string>{"Content", "Number", "NumberOfElements", "Size"});
  REQUIRE(page.value()["Number"].get<int>() == 0);
  REQUIRE(page.value()["NumberOfElements"].get<int>() == 1);
  REQUIRE(page.value()["Size"].get<int>() == 10);
  REQUIRE(page.value()["Content"].size() == 1);

  const auto& entry = page.value()["Content"][0];
  REQUIRE(KeysOf(entry) ==
          std::set<std::string>{"FileID", "Driver", "Location", "CreatedAt", "CreatedBy"});
  REQUIRE(entry["FileID"].get<std::string>() == record.file_id);
  //  驱动名来自**存储能力上报**的真实驱动（HttpFixture 用内存适配器 → "memory"），
  //  且与 §2.2/§2.3 一样是**小写**（契约 §2.5 的样例是 POSIX 部署下的 "posix"）
  REQUIRE(entry["Driver"].get<std::string>() == "memory");
  REQUIRE(entry["Location"].get<std::string>().find(record.file_id) != std::string::npos);
  REQUIRE(entry["CreatedBy"].get<std::string>() == "osdu-user");

  //  `CreatedAt` = `yyyy-MM-dd'T'HH:mm:ss.SSS+0000`（★ 末尾是 +0000，不是 Z）
  const std::string created_at = entry["CreatedAt"].get<std::string>();
  REQUIRE(created_at.size() == 28);
  REQUIRE(created_at.substr(4, 1) == "-");
  REQUIRE(created_at.substr(10, 1) == "T");
  REQUIRE(created_at.substr(19, 1) == ".");
  REQUIRE(created_at.substr(23) == "+0000");
  //  与注入时钟一致（1700000000 = 2023-11-14T22:13:20Z）
  REQUIRE(created_at.rfind(fss::time::ToOsduTimestamp(fx.clock.NowEpochSeconds(), 0), 0) == 0);
}

TEST_CASE("★ C6.6 分页：PageNum=0 与 1 结果不重叠且并集完整", "[phase6][integration][c6.6]") {
  HttpFixture fx;
  std::set<std::string> all;
  for (int i = 0; i < 3; ++i) {
    const auto record = CreateRecord(fx, "osdu-user", "page" + std::to_string(i) + ".bin");
    all.insert(record.file_id);
    fx.clock.AdvanceSeconds(1);  // 稳定全序：created_at 升序
  }
  REQUIRE(all.size() == 3);

  const auto first = List(fx, R"({"PageNum":0,"Items":2})");
  const auto second = List(fx, R"({"PageNum":1,"Items":2})");
  REQUIRE(first.status == 200);
  REQUIRE(second.status == 200);
  const auto page0 = fss::json::ParseObject(first.body).value();
  const auto page1 = fss::json::ParseObject(second.body).value();

  //  回显的分页参数
  REQUIRE(page0["Number"].get<int>() == 0);
  REQUIRE(page0["Size"].get<int>() == 2);
  REQUIRE(page0["NumberOfElements"].get<int>() == 2);
  REQUIRE(page1["Number"].get<int>() == 1);
  REQUIRE(page1["Size"].get<int>() == 2);
  REQUIRE(page1["NumberOfElements"].get<int>() == 1);  // 第 3 条

  const auto ids0 = IdsOf(page0);
  const auto ids1 = IdsOf(page1);
  //  ★ 不重叠
  for (const auto& id : ids0) REQUIRE(std::find(ids1.begin(), ids1.end(), id) == ids1.end());
  //  并集 = 全部，且顺序稳定（升序：page0 的两条在前）
  std::set<std::string> union_all(ids0.begin(), ids0.end());
  union_all.insert(ids1.begin(), ids1.end());
  REQUIRE(union_all == all);
  REQUIRE(ids0.front() < ids0.back());

  //  翻到超出范围的页 → 无记录 → 400（上游行为）
  const auto beyond = List(fx, R"({"PageNum":9,"Items":2})");
  REQUIRE(beyond.status == 400);
}

TEST_CASE("★ C6.6 时间区间过滤（含端点）+ UserID 过滤", "[phase6][integration][c6.6]") {
  HttpFixture fx;
  const std::int64_t t0 = fx.clock.NowEpochSeconds();
  const auto a = CreateRecord(fx, "alice", "a.bin");
  fx.clock.AdvanceSeconds(3600);
  const std::int64_t t1 = fx.clock.NowEpochSeconds();
  const auto b = CreateRecord(fx, "bob", "b.bin");
  fx.clock.AdvanceSeconds(3600);
  const std::int64_t t2 = fx.clock.NowEpochSeconds();
  const auto c = CreateRecord(fx, "alice", "c.bin");

  const auto list_window = [&](std::int64_t from, std::int64_t to, const std::string& user = "") {
    fss::json::Value body = fss::json::Value::object();
    body["PageNum"] = 0;
    body["Items"] = 10;
    body["TimeFrom"] = Iso(from);
    body["TimeTo"] = Iso(to);
    if (!user.empty()) body["UserID"] = user;
    return List(fx, fss::json::Dump(body));
  };

  //  [t0, t2] → 全部 3 条
  const auto whole = list_window(t0, t2);
  REQUIRE(whole.status == 200);
  REQUIRE(IdsOf(fss::json::ParseObject(whole.body).value()).size() == 3);

  //  [t0, t1] → a、b（**含端点**：t1 时刻的 b 必须在）
  const auto early = list_window(t0, t1);
  REQUIRE(early.status == 200);
  const auto early_ids = IdsOf(fss::json::ParseObject(early.body).value());
  REQUIRE(early_ids.size() == 2);
  REQUIRE(std::find(early_ids.begin(), early_ids.end(), a.file_id) != early_ids.end());
  REQUIRE(std::find(early_ids.begin(), early_ids.end(), b.file_id) != early_ids.end());

  //  [t1, t1] → 恰好 b（证明两个端点都是**闭**的）
  const auto point = list_window(t1, t1);
  REQUIRE(point.status == 200);
  const auto point_ids = IdsOf(fss::json::ParseObject(point.body).value());
  REQUIRE(point_ids.size() == 1);
  REQUIRE(point_ids.front() == b.file_id);

  //  [t2, t2] → 恰好 c
  const auto tail = list_window(t2, t2);
  REQUIRE(tail.status == 200);
  REQUIRE(IdsOf(fss::json::ParseObject(tail.body).value()) == std::vector<std::string>{c.file_id});

  //  区间内无记录 → 400（不是 200 + 空数组）
  const auto gap = list_window(t0 - 7200, t0 - 3600);
  REQUIRE(gap.status == 400);

  //  UserID 过滤：alice 有 a、c；bob 只有 b
  const auto alice = list_window(t0, t2, "alice");
  REQUIRE(alice.status == 200);
  const auto alice_ids = IdsOf(fss::json::ParseObject(alice.body).value());
  REQUIRE(alice_ids.size() == 2);
  for (const auto& id : alice_ids) REQUIRE(id != b.file_id);

  const auto bob = list_window(t0, t2, "bob");
  REQUIRE(bob.status == 200);
  REQUIRE(IdsOf(fss::json::ParseObject(bob.body).value()) == std::vector<std::string>{b.file_id});

  //  UserID 过滤 + 时间过滤同时生效：bob 在 [t0,t1] 内有 b，在 [t2,t2] 内没有 → 400
  REQUIRE(list_window(t0, t1, "bob").status == 200);
  REQUIRE(list_window(t2, t2, "bob").status == 400);
}

TEST_CASE("★ C6.6 上游三条负向样例（逐字 fixture）→ 全部 400", "[phase6][integration][c6.6]") {
  HttpFixture fx;
  //  库里有记录，因此"400"不是因为"库是空的"，而是因为**请求本身**非法
  (void)CreateRecord(fx, "common-user", "neg.bin");

  const auto empty_payload = ReadUpstreamFixture("list/File_GetList_EmptyPayload.json");
  const auto invalid_payload = ReadUpstreamFixture("list/File_GetList_InvalidPayload.json");
  const auto no_record_payload = ReadUpstreamFixture("list/File_GetList_NoRecordPayload.json");

  SECTION("File_GetList_EmptyPayload.json（`{}`）→ 400") {
    const auto reply = List(fx, empty_payload);
    INFO("响应: " << reply.body);
    REQUIRE(reply.status == 400);
  }
  SECTION("File_GetList_InvalidPayload.json（缺 `Items`）→ 400") {
    const auto reply = List(fx, invalid_payload);
    INFO("响应: " << reply.body);
    REQUIRE(reply.status == 400);
  }
  SECTION("File_GetList_NoRecordPayload.json（时间区间内无记录）→ 400") {
    //  ★ 这条 fixture 的 TimeFrom/TimeTo 是 2020 年，注入时钟是 2023 年 → 区间内必无记录。
    //    关键：它**能解析**（否则 400 就只是因为解析失败，测试会变成空证据）——
    //    先用同样的时间区间 + 一个存在的用户断言"解析没问题"（只是没记录）。
    for (const char* field : {"TimeFrom", "TimeTo"}) {
      const auto parsed = fss::time::ParseIso8601(
          fss::json::ParseObject(no_record_payload).value()[field].get<std::string>());
      REQUIRE(parsed.ok());
    }
    const auto reply = List(fx, no_record_payload);
    INFO("响应: " << reply.body);
    REQUIRE(reply.status == 400);
    //  消息必须是"无记录"，而不是"时间格式非法"（上游 provider 的逐字消息）
    REQUIRE(reply.body.find("Nothing found for such filter and page(num: 0, size: 2).") !=
            std::string::npos);
  }
}

TEST_CASE("★ C6.6 非法分页参数与非法时间格式 → 400", "[phase6][integration][c6.6]") {
  HttpFixture fx;
  (void)CreateRecord(fx, "osdu-user", "bad.bin");

  const std::vector<std::pair<std::string, std::string>> bad_bodies = {
      {"Items=0", R"({"PageNum":0,"Items":0})"},
      {"Items=-1", R"({"PageNum":0,"Items":-1})"},
      {"PageNum=-1", R"({"PageNum":-1,"Items":10})"},
      {"TimeFrom 非法", R"({"PageNum":0,"Items":10,"TimeFrom":"2020-13-45T99:99:99Z"})"},
      {"TimeTo 非法", R"({"PageNum":0,"Items":10,"TimeTo":"not-a-time"})"},
  };
  for (const auto& [what, body] : bad_bodies) {
    INFO("非法输入：" << what);
    const auto reply = List(fx, body);
    INFO("响应: " << reply.body);
    REQUIRE(reply.status == 400);
  }

  //  ★ `TimeFrom > TimeTo` 必须报"区间非法"，**不能**靠"区间内自然没有记录"蒙混过关
  //    （否则这条用例在实现换成"无记录 → 400"之后仍然通过，等于空证据）
  const auto reversed = List(
      fx, R"({"PageNum":0,"Items":10,"TimeFrom":"2024-01-02T00:00:00Z","TimeTo":"2024-01-01T00:00:00Z"})");
  REQUIRE(reversed.status == 400);
  REQUIRE(reversed.body.find("before TimeTo") != std::string::npos);

  //  正例对照（R16）：同一批参数只要合法就必须 200 —— 否则上面的 400 可能来自"恒真拒绝"
  const auto ok = List(fx, R"({"PageNum":0,"Items":10,"TimeFrom":"2020-01-01T00:00:00Z","TimeTo":"2030-01-01T00:00:00Z"})");
  REQUIRE(ok.status == 200);
  REQUIRE(IdsOf(fss::json::ParseObject(ok.body).value()).size() == 1);
}
