// C1.10（部分）：common/ids —— 生产用随机 UUID 与测试用确定性 ID
#include <catch2/catch.hpp>

#include "common/ids/id_generator.h"

#include <regex>
#include <set>
#include <string>

TEST_CASE("UuidGenerator 产出合法 UUIDv4", "[phase1][ids]") {
  fss::UuidGenerator gen;
  const std::string u = gen.NewUuid();

  REQUIRE(u.size() == 36);
  // 8-4-4-4-12
  REQUIRE(u[8] == '-');
  REQUIRE(u[13] == '-');
  REQUIRE(u[18] == '-');
  REQUIRE(u[23] == '-');

  const std::regex re("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
  INFO("uuid = " << u);
  REQUIRE(std::regex_match(u, re));

  // 去横线版本：32 个十六进制字符，同一 UUID 的两半形式都合法
  const std::string nodash = gen.NewUuidNoDash();
  REQUIRE(nodash.size() == 32);
  REQUIRE(std::regex_match(nodash, std::regex("^[0-9a-f]{32}$")));
}

TEST_CASE("UuidGenerator 不重复（1000 次）", "[phase1][ids]") {
  fss::UuidGenerator gen;
  std::set<std::string> seen;
  for (int i = 0; i < 1000; ++i) seen.insert(gen.NewUuid());
  REQUIRE(seen.size() == 1000);
}

TEST_CASE("★ SequentialIdGenerator 确定性：测试断言可以写死", "[phase1][ids]") {
  fss::SequentialIdGenerator gen;
  REQUIRE(gen.NewUuid() == "00000000-0000-4000-8000-000000000001");
  REQUIRE(gen.NewUuid() == "00000000-0000-4000-8000-000000000002");

  gen.Reset(0xabc);
  REQUIRE(gen.NewUuid() == "00000000-0000-4000-8000-000000000abc");

  gen.Reset();
  REQUIRE(gen.NewUuidNoDash() == "00000000000000000000000000000001");
}

TEST_CASE("fileID 满足 OSDU 的正则约束（契约 §1.5）", "[phase1][ids][contract]") {
  // OSDU 的 FileID 必须匹配 ^[\w,\s-]+(\.\w+)?$
  // 生成器产出的 UUID 含连字符，而连字符属于 \w 之外的 '-'，但正则允许 '-'
  const std::regex osdu_file_id(R"(^[\w,\s-]+(\.\w+)?$)");
  fss::UuidGenerator gen;
  const std::string u = gen.NewUuid();
  INFO("uuid = " << u);
  REQUIRE(std::regex_match(u, osdu_file_id));
  REQUIRE(std::regex_match(gen.NewUuidNoDash(), osdu_file_id));
}

TEST_CASE("IIdGenerator 多态可用（应用层只依赖端口）", "[phase1][ids]") {
  fss::IIdGenerator* gen = new fss::SequentialIdGenerator(7);
  REQUIRE(gen->NewUuid().find("000000000007") != std::string::npos);
  delete gen;
}
