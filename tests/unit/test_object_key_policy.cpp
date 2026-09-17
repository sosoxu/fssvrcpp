// C2.4：ObjectKeyPolicy —— 生成/解析往返 + ≥10 个恶意输入全部被拒
#include <catch2/catch.hpp>

#include "app/services/object_key_policy.h"

#include <string>
#include <vector>

using fss::app::ObjectKeyPolicy;
using Parts = fss::app::ObjectKeyPolicy::SourcePath;

namespace {
Parts Sample() {
  Parts p;
  p.user_id = "osdu-user";
  p.epoch_millis = 1614784413120;
  p.timestamp = "2021-03-03-15-13-33-120";
  p.file_id = "da92f52401dc4d1cb93515f159c110d4";
  return p;
}
}  // namespace

TEST_CASE("★ C2.4 生成 FileSource：格式与契约 §1.5 逐字符一致", "[phase2][key][contract]") {
  const auto r = ObjectKeyPolicy::MakeFileSource(Sample());
  REQUIRE(r.ok());
  // 与上游示例逐字节一致（含前导斜杠与两段结构）
  REQUIRE(r.value() ==
          "/osdu-user/1614784413120-2021-03-03-15-13-33-120/da92f52401dc4d1cb93515f159c110d4");
  REQUIRE(r.value().front() == '/');  // ★ 前导斜杠是契约点
}

TEST_CASE("★ C2.4 解析往返：生成 → 解析 → 完全相等", "[phase2][key][contract]") {
  const auto src = ObjectKeyPolicy::MakeFileSource(Sample()).value();
  const auto parsed = ObjectKeyPolicy::ParseFileSource(src);
  REQUIRE(parsed.ok());
  REQUIRE(parsed.value() == Sample());

  SECTION("对象键布局：POSIX 同形、S3 日期分层") {
    const auto& p = parsed.value();
    REQUIRE(ObjectKeyPolicy::MakePosixKey(p) ==
            "osdu-user/1614784413120-2021-03-03-15-13-33-120/da92f52401dc4d1cb93515f159c110d4");
    // 契约 §2.1 的 URL 样例里是 `<bucket>/2025/09/16/<fileID>`
    REQUIRE(ObjectKeyPolicy::MakeS3Key(p) == "2021/03/03/da92f52401dc4d1cb93515f159c110d4");
  }

  SECTION("容器名：`<partition>-<zone>`（小写）") {
    REQUIRE(ObjectKeyPolicy::ContainerFor("opendes", fss::domain::StorageZone::kStaging).value() ==
            "opendes-staging");
    REQUIRE(ObjectKeyPolicy::ContainerFor("OPENDES", fss::domain::StorageZone::kPersistent).value() ==
            "opendes-persistent");
    REQUIRE(ObjectKeyPolicy::ContainerFor("", fss::domain::StorageZone::kStaging).ok() == false);
    REQUIRE(ObjectKeyPolicy::ContainerFor("a/b", fss::domain::StorageZone::kStaging).ok() == false);
  }

  SECTION("时间戳格式化：与上游 `yyyy-MM-dd-HH-mm-ss-SSS` 一致") {
    REQUIRE(ObjectKeyPolicy::FormatTimestamp(1614784413, 120) == "2021-03-03-15-13-33-120");
    REQUIRE(ObjectKeyPolicy::FormatTimestamp(1614784413, 0) == "2021-03-03-15-13-33-000");
  }
}

TEST_CASE("★★ C2.4 恶意 FileSource：逐条必须被拒（≥10 例）", "[phase2][key][contract]") {
  struct Case { const char* name; std::string value; };
  const std::vector<Case> cases = {
      {"空串", ""},
      {"父目录穿越", "/osdu-user/1614784413120-2021-03-03-15-13-33-120/../secret"},
      {"深层穿越", "/osdu-user/1614784413120-2021-03-03-15-13-33-120/a/../../etc/passwd"},
      {"绝对路径注入", "/etc/passwd"},
      {"绝对路径（第 3 段）", "/osdu-user/1614784413120-2021-03-03-15-13-33-120//etc/passwd"},
      {"缺少前导斜杠", "osdu-user/1614784413120-2021-03-03-15-13-33-120/f"},
      {"段数不足", "/osdu-user/f"},
      {"段数过多", "/osdu-user/1614784413120-2021-03-03-15-13-33-120/a/b"},
      // ★ 不要手写 `std::string("...\0...", N)` 的字节数：写错长度会在**字面量之外**多读
      //   几个字节（ASan: global-buffer-overflow）。用拼接表达"含内嵌 NUL"的意图，
      //   长度由标准库算，不靠人肉数数。
      {"NUL 字节", std::string("/osdu-user/1614784413120-2021-03-03-15-13-33-120/a") + '\0' + 'b'},
      {"控制字符", "/osdu-user/1614784413120-2021-03-03-15-13-33-120/a\nb"},
      {"反斜杠", "/osdu-user/1614784413120-2021-03-03-15-13-33-120/a\\b"},
      {"百分号编码残留", "/osdu-user/1614784413120-2021-03-03-15-13-33-120/a%2Fb"},
      {"只有点段", "/osdu-user/1614784413120-2021-03-03-15-13-33-120/."},
      {"父目录段", "/osdu-user/1614784413120-2021-03-03-15-13-33-120/.."},
      {"时间戳格式错", "/osdu-user/1614784413120-2021-03-03/abcdef"},
      {"epoch 非数字", "/osdu-user/abcdef-2021-03-03-15-13-33-120/f"},
      {"epoch 缺失", "/osdu-user/-2021-03-03-15-13-33-120/f"},
      {"超长（>1024）", "/osdu-user/1614784413120-2021-03-03-15-13-33-120/" + std::string(1200, 'a')},
      {"尾部斜杠", "/osdu-user/1614784413120-2021-03-03-15-13-33-120/"},
      {"双斜杠", "//osdu-user/1614784413120-2021-03-03-15-13-33-120/f"},
      {"空格注入", "/osdu user/1614784413120-2021-03-03-15-13-33-120/f"},
      {"通配符", "/osdu-user/1614784413120-2021-03-03-15-13-33-120/*"},
  };
  REQUIRE(cases.size() >= 10);
  for (const auto& c : cases) {
    INFO("case=" << c.name << " value=" << c.value);
    const auto r = ObjectKeyPolicy::ParseFileSource(c.value);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().kind() == fss::ErrorKind::kInvalidArgument);
  }
}

TEST_CASE("★ C2.4 对照：合法输入必须被接受（否则上面的拒绝说明不了什么）",
          "[phase2][key][contract]") {
  const std::vector<std::string> good = {
      ObjectKeyPolicy::MakeFileSource(Sample()).value(),
      "/a/1-2021-01-01-00-00-00-000/b",
      "/user-1/1614784413120-2021-03-03-15-13-33-120/file.txt",   // fileID 带扩展名
      "/u/1614784413120-2021-03-03-15-13-33-120/UPPER_lower-123",
  };
  for (const auto& s : good) {
    INFO("value=" << s);
    REQUIRE(ObjectKeyPolicy::ParseFileSource(s).ok());
  }
  // 生成时的参数校验（不生成可疑路径）
  Parts bad = Sample();
  bad.user_id = "../etc";
  REQUIRE_FALSE(ObjectKeyPolicy::MakeFileSource(bad).ok());
  bad = Sample();
  bad.file_id = "a/b";
  REQUIRE_FALSE(ObjectKeyPolicy::MakeFileSource(bad).ok());
  bad = Sample();
  bad.timestamp = "2021-03-03";
  REQUIRE_FALSE(ObjectKeyPolicy::MakeFileSource(bad).ok());
  bad = Sample();
  bad.epoch_millis = 0;
  REQUIRE_FALSE(ObjectKeyPolicy::MakeFileSource(bad).ok());
}

TEST_CASE("C2.4 旧接口 FileID 正则与长度上限（契约 §1.5）", "[phase2][key][contract]") {
  // `^[\w,\s-]+(\.\w+)?$`
  REQUIRE(ObjectKeyPolicy::IsValidFileId("da92f52401dc4d1cb93515f159c110d4"));
  REQUIRE(ObjectKeyPolicy::IsValidFileId("file.txt"));
  REQUIRE(ObjectKeyPolicy::IsValidFileId("a,b-c_d"));
  REQUIRE(ObjectKeyPolicy::IsValidFileId("with space"));  // `\s` 允许空格（上游正则如此）
  REQUIRE_FALSE(ObjectKeyPolicy::IsValidFileId(""));
  REQUIRE_FALSE(ObjectKeyPolicy::IsValidFileId("../etc/passwd"));
  REQUIRE_FALSE(ObjectKeyPolicy::IsValidFileId("a/b"));
  REQUIRE_FALSE(ObjectKeyPolicy::IsValidFileId(".hidden"));   // 首段不能以 '.' 开头
  REQUIRE_FALSE(ObjectKeyPolicy::IsValidFileId("trailing."));
  REQUIRE_FALSE(ObjectKeyPolicy::IsValidFileId("a.b.c"));     // 只允许一段扩展名
  REQUIRE_FALSE(ObjectKeyPolicy::IsValidFileId(std::string(600, 'a')));  // 超长度上限
}
