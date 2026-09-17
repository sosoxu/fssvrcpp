// 领域模型：`dataset--File.Generic` 的序列化/反序列化 + 必需字段校验
//
// 判据来自契约 §3.1（必需字段）、§3.2（大小写）、§3.3（权威黄金样例）、§3.4（负向矩阵）。
// 这里最重要的两条断言：
//   ① **未知字段必须原样保留**（黄金样例里有 `ResourceHomeRegionID` 等模型未显式列出的字段）
//   ② 必需字段缺失时的消息要能对上上游验收测试的文案（`FileSource can not be empty` 等）
#include <catch2/catch.hpp>

#include "common/json/json.h"
#include "domain/model/file_metadata.h"

#include <fstream>
#include <sstream>
#include <string>

using fss::domain::FileMetadataRecord;
using fss::domain::ParseFileMetadataRecord;
using fss::domain::ToJson;
using fss::domain::ValidateMetadataRecord;

namespace {

std::string ReadFixture(const char* name) {
  const std::string path = std::string(FSS_REPO_ROOT) + "/tests/conformance/fixtures/" + name;
  std::ifstream in(path);
  REQUIRE(in.good());
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// 语义等价比较（nlohmann 的 == 就是递归的语义比较：键序无关）
bool SemanticallyEqual(const fss::json::Value& a, const fss::json::Value& b) { return a == b; }

}  // namespace

TEST_CASE("★ 黄金样例：解析 → 序列化必须语义等价（含未知字段）",
          "[phase2][model][contract]") {
  const auto text = ReadFixture("File_CorrectPayload.json");
  auto parsed = fss::json::Parse(text);
  REQUIRE(parsed.ok());
  const auto& original = parsed.value();

  const auto record = ParseFileMetadataRecord(original);
  if (!record.ok()) INFO("解析失败: " << record.error().ToString());
  REQUIRE(record.ok());

  // 关键字段映射（契约 §3.2：data 层是 PascalCase）
  const auto& r = record.value();
  REQUIRE(r.id == "opendes:dataset--File.Generic:33a71a04d20f4240904b4b3fca4657b7");
  REQUIRE(r.kind == "opendes:wks:dataset--File.Generic:1.0.0");
  REQUIRE(r.Partition() == "opendes");
  REQUIRE(r.acl.viewers.size() == 1);
  REQUIRE(r.acl.owners.size() == 1);
  REQUIRE(r.legal.legaltags == std::vector<std::string>{"opendes-storage-tag"});
  REQUIRE(r.legal.other_relevant_data_countries == std::vector<std::string>{"US"});
  REQUIRE(r.data.name.value() == "Dataset X221/15");
  REQUIRE(r.data.total_size.value() == "13245217273");
  REQUIRE(r.data.endian.value() == "BIG");
  REQUIRE(r.data.dataset_properties.file_source_info.file_source ==
          "/osdu-user/1624011206350-2021-06-18-10-13-26-350/33a71a04d20f4240904b4b3fca4657b7");
  REQUIRE(r.data.dataset_properties.file_source_info.file_size.value() == "95463");
  REQUIRE(r.data.dataset_properties.file_source_info.checksum_algorithm.value() == "SHA-256");
  // 未知字段（模型未显式列出）必须进 extra 而不是被丢掉
  REQUIRE(r.data.resource_home_region_id.has_value());
  REQUIRE(r.data.resource_host_region_ids.size() == 1);

  // ★ 往返：语义等价（键序无关，但字段一个都不能少、不能变）
  const auto round_tripped = ToJson(r);
  INFO("往返后的 JSON: " << fss::json::Dump(round_tripped));
  INFO("原始 JSON: " << fss::json::Dump(original));
  REQUIRE(SemanticallyEqual(round_tripped, original));
  // 显式点名几个容易漏的开放字段
  REQUIRE(round_tripped["data"].contains("ResourceHomeRegionID"));
  REQUIRE(round_tripped["data"].contains("ExtensionProperties"));
  REQUIRE(round_tripped.contains("tags"));
  REQUIRE(round_tripped.contains("meta"));
}

TEST_CASE("★ 黄金样例通过必需字段校验（含 Endian 枚举）", "[phase2][model][contract]") {
  const auto text = ReadFixture("File_CorrectPayload.json");
  const auto record = ParseFileMetadataRecord(fss::json::Parse(text).value());
  REQUIRE(record.ok());
  const auto vr = ValidateMetadataRecord(record.value());
  INFO("问题列表:");
  for (const auto& issue : vr.issues) INFO("  " << issue.path << " → " << issue.message);
  REQUIRE(vr.ok());
}

TEST_CASE("★ 负向矩阵（契约 §3.4）：必需字段缺失 → 明确的问题路径与固定消息",
          "[phase2][model][contract]") {
  const auto base_text = ReadFixture("File_CorrectPayload.json");

  SECTION("FileSource 为空 → `FileSource can not be empty`") {
    auto v = fss::json::Parse(base_text).value();
    v["data"]["DatasetProperties"]["FileSourceInfo"]["FileSource"] = "";
    const auto r = ParseFileMetadataRecord(v);
    REQUIRE(r.ok());
    const auto vr = ValidateMetadataRecord(r.value());
    REQUIRE_FALSE(vr.ok());
    bool found = false;
    for (const auto& i : vr.issues) {
      if (i.path == "data.DatasetProperties.FileSourceInfo.FileSource") {
        REQUIRE(i.message == "FileSource can not be empty");
        found = true;
      }
    }
    REQUIRE(found);
  }

  SECTION("FileSource 非法（无前导斜杠 / 含 .. ）→ `Invalid source file path to copy from <path>`") {
    for (const char* bad : {"relative/path", "/a/../../etc/passwd", "/a\nb"}) {
      auto v = fss::json::Parse(base_text).value();
      v["data"]["DatasetProperties"]["FileSourceInfo"]["FileSource"] = bad;
      const auto r = ParseFileMetadataRecord(v);
      REQUIRE(r.ok());
      const auto vr = ValidateMetadataRecord(r.value());
      INFO("FileSource=" << bad);
      REQUIRE_FALSE(vr.ok());
      bool found = false;
      for (const auto& i : vr.issues) {
        if (i.path == "data.DatasetProperties.FileSourceInfo.FileSource") {
          REQUIRE(i.message == std::string("Invalid source file path to copy from ") + bad);
          found = true;
        }
      }
      REQUIRE(found);
    }
  }

  SECTION("acl 为空 / owners 为空 / viewers 为空") {
    for (const char* field : {"viewers", "owners"}) {
      auto v = fss::json::Parse(base_text).value();
      v["acl"][field] = fss::json::Value::array();
      const auto r = ParseFileMetadataRecord(v);
      REQUIRE(r.ok());
      const auto vr = ValidateMetadataRecord(r.value());
      INFO("field=" << field);
      REQUIRE_FALSE(vr.ok());
      bool found = false;
      for (const auto& i : vr.issues) {
        if (i.path == std::string("acl.") + field) found = true;
      }
      REQUIRE(found);
    }
  }

  SECTION("legal 为空 / legaltags 为空 / 国家代码非法") {
    {
      auto v = fss::json::Parse(base_text).value();
      v["legal"]["legaltags"] = fss::json::Value::array();
      const auto vr = ValidateMetadataRecord(ParseFileMetadataRecord(v).value());
      REQUIRE_FALSE(vr.ok());
    }
    {
      auto v = fss::json::Parse(base_text).value();
      v["legal"]["otherRelevantDataCountries"] = fss::json::Value::array();
      const auto vr = ValidateMetadataRecord(ParseFileMetadataRecord(v).value());
      REQUIRE_FALSE(vr.ok());
    }
    {
      auto v = fss::json::Parse(base_text).value();
      v["legal"]["otherRelevantDataCountries"] = fss::json::Value::array({"USA"});
      const auto vr = ValidateMetadataRecord(ParseFileMetadataRecord(v).value());
      REQUIRE_FALSE(vr.ok());
    }
    {
      // 重复的 legaltag（契约 §3.1：元素唯一）
      auto v = fss::json::Parse(base_text).value();
      v["legal"]["legaltags"] = fss::json::Value::array({"t1", "t1"});
      const auto vr = ValidateMetadataRecord(ParseFileMetadataRecord(v).value());
      REQUIRE_FALSE(vr.ok());
    }
  }

  SECTION("Endian 非法 → **解析阶段**报 `Invalid value of <值> for Endian`（对齐上游）") {
    //  时机与消息都对齐上游：上游把 Endian 建模成带 `@JsonCreator` 的枚举，非法值在
    //  **反序列化**时就抛 `EnumValidationException`，早于 Bean Validation。顺序是可观测的：
    //  `File_invalid_Endian.json` 的 FileSource 同时非法，上游仍然报 Endian（P4-D11）。
    auto v = fss::json::Parse(base_text).value();
    v["data"]["Endian"] = "MIDDLE";
    const auto parsed = ParseFileMetadataRecord(v);
    REQUIRE_FALSE(parsed.ok());
    REQUIRE(parsed.error().message() == "Invalid value of MIDDLE for Endian");
    //  非 JSON 入口（手写记录）的安全网仍在校验器里
    auto hand_built = ParseFileMetadataRecord(fss::json::Parse(base_text).value()).value();
    hand_built.data.endian = "MIDDLE";
    const auto vr = ValidateMetadataRecord(hand_built);
    REQUIRE_FALSE(vr.ok());
    bool found = false;
    for (const auto& i : vr.issues) {
      if (i.path == "data.Endian") {
        REQUIRE(i.message == "Invalid value of MIDDLE for Endian");
        found = true;
      }
    }
    REQUIRE(found);
  }

  SECTION("kind 段数不对 → Invalid kind") {
    auto v = fss::json::Parse(base_text).value();
    v["kind"] = "opendes:wks:dataset--File.Generic";
    const auto vr = ValidateMetadataRecord(ParseFileMetadataRecord(v).value());
    REQUIRE_FALSE(vr.ok());
  }

  SECTION("★ 对照：完整样例必须通过（否则上面的拒绝说明不了什么）") {
    const auto vr =
        ValidateMetadataRecord(ParseFileMetadataRecord(fss::json::Parse(base_text).value()).value());
    REQUIRE(vr.ok());
  }
}

TEST_CASE("解析的结构性错误：顶层或子对象类型不对 → kInvalidArgument", "[phase2][model]") {
  SECTION("顶层不是对象") {
    const auto r = ParseFileMetadataRecord(fss::json::Parse("[]").value());
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().kind() == fss::ErrorKind::kInvalidArgument);
  }
  SECTION("acl / legal / data 类型不对") {
    for (const char* field : {"acl", "legal", "data"}) {
      auto v = fss::json::Parse("{}").value();
      v[field] = "not-an-object";
      INFO("field=" << field);
      REQUIRE_FALSE(ParseFileMetadataRecord(v).ok());
    }
  }
  SECTION("空对象：**解析阶段**就报缺必需段（不是退化成'FileSource 为空'）") {
    //  ★ 行为在 P4 切片 4 收紧：必需**段**（kind/acl/legal/data）缺失时在解析阶段报错。
    //    原因：模型无法表达"段不存在"，缺失与为空在解析后长得一样 ——
    //    若只靠 ValidateMetadataRecord，`File_missing_data.json` 会退化成
    //    "FileSource can not be empty"，客户端拿到的消息就不是契约 §3.4 要求的
    //    "data 为空"（P4-D07）。这里同时钉住"解析必须失败"和"消息要提到段名"。
    const auto r = ParseFileMetadataRecord(fss::json::Parse("{}").value());
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().kind() == fss::ErrorKind::kInvalidArgument);
    INFO(r.error().message());
    REQUIRE(r.error().message().find("kind") != std::string::npos);
  }
  SECTION("补齐必需段后：解析成功、校验拦下其余缺失项") {
    //  正例（R16）：只补齐"段"，内容仍为空 → 解析必须成功，由校验给出全部问题
    auto v = fss::json::Parse(
                 R"({"kind": "", "acl": {}, "legal": {}, "data": {"Name": "x"}})")
                 .value();
    const auto r = ParseFileMetadataRecord(v);
    REQUIRE(r.ok());
    const auto vr = ValidateMetadataRecord(r.value());
    REQUIRE_FALSE(vr.ok());
    REQUIRE(vr.issues.size() >= 4);  // kind / viewers / owners / legaltags / 国家 / FileSource
  }
  SECTION("逐个缺段：消息必须点出缺的是哪一段") {
    for (const char* section : {"kind", "acl", "legal", "data"}) {
      auto v = fss::json::Parse(
                   R"({"kind": "k", "acl": {}, "legal": {}, "data": {"Name": "x"}})")
                   .value();
      v.erase(section);
      INFO("section=" << section);
      const auto r = ParseFileMetadataRecord(v);
      REQUIRE_FALSE(r.ok());
      REQUIRE(r.error().message().find(section) != std::string::npos);
    }
  }
}

TEST_CASE("ACL 主体与 legal tag 的校验规则（契约 §1.5）", "[phase2][model][contract]") {
  using fss::domain::IsValidAclPrincipal;
  REQUIRE(IsValidAclPrincipal("data.default.viewers@opendes.example.com"));
  REQUIRE(IsValidAclPrincipal("data.default.owners@opendes.example.com"));
  REQUIRE(IsValidAclPrincipal("data.group_1+test@a.b.co"));
  REQUIRE_FALSE(IsValidAclPrincipal(""));
  REQUIRE_FALSE(IsValidAclPrincipal("not-an-email"));
  REQUIRE_FALSE(IsValidAclPrincipal("data.default.viewers@"));
  REQUIRE_FALSE(IsValidAclPrincipal("viewers@example.com"));  // 必须以 data. 开头

  using fss::domain::IsValidLegalTag;
  REQUIRE(IsValidLegalTag("opendes-storage-tag"));
  REQUIRE_FALSE(IsValidLegalTag(""));
  REQUIRE_FALSE(IsValidLegalTag(std::string("bad\ntag")));
}
