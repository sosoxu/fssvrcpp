// C1.9：common/fs —— 安全路径（≥12 个恶意输入）与原子写（绝不产生半截文件）
#include <catch2/catch.hpp>

#include "common/fs/fs.h"
#include "framework/temp_dir.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace stdfs = std::filesystem;

TEST_CASE("★ 词法路径安全：13 种恶意键必须全部被拒", "[phase1][fs][security]") {
  const std::string root = "/var/lib/fss/data";
  const std::vector<std::pair<std::string, const char*>> bad = {
      {"", "空键"},
      {"/etc/passwd", "绝对路径"},
      {"../outside", "以 .. 开头"},
      {"a/../../b", "中间的路径穿越"},
      {"..", "纯 .."},
      {".", "纯 ."},
      {"a//b", "空路径段"},
      {"a/./b", ". 路径段"},
      {std::string("a\0b", 3), "含 NUL 字节"},
      {"a/", "以 / 结尾（目录语义）"},
      {"a/..", "以 .. 结尾"},
      {"a/../..", "多级回退"},
      {std::string(2000, 'x'), "超长键"},
  };
  for (const auto& [key, why] : bad) {
    const auto r = fss::fs::LexicalJoin(root, key);
    INFO("应被拒：" << why << "  key=" << key.substr(0, 40));
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().kind() == fss::ErrorKind::kInvalidArgument);
    REQUIRE(r.error().Find("reason").has_value());
  }
  REQUIRE(bad.size() >= 12);  // 判据要求 ≥12 个恶意输入
}

TEST_CASE("合法键被正确规范化并拼接", "[phase1][fs]") {
  auto r = fss::fs::LexicalJoin("/root/", "2025/09/16/abc.txt");
  REQUIRE(r.ok());
  REQUIRE(r.value() == "/root/2025/09/16/abc.txt");

  // 合法但"看起来可疑"的键：这些在 POSIX 上是普通文件名，**不应**被拒
  REQUIRE(fss::fs::LexicalJoin("/root", "..../x").ok());       // "...." 是合法目录名
  REQUIRE(fss::fs::LexicalJoin("/root", "a..b").ok());
  REQUIRE(fss::fs::LexicalJoin("/root", "a\\..\\b").ok());     // 反斜杠在 POSIX 不是分隔符
  REQUIRE(fss::fs::LexicalJoin("/root", "文件.bin").ok());     // 非 ASCII
}

TEST_CASE("★ 符号链接逃逸必须被拒（词法合法但解析后出界）",
          "[phase1][fs][security]") {
  fss::test::TempDir tmp("fs_symlink");
  const std::string root = tmp.child("root");
  const std::string outside = tmp.child("outside");
  fss::fs::EnsureDir(root);
  fss::fs::EnsureDir(outside);

  // 在 root 里放一个指向外部的符号链接
  const std::string link = root + "/escape";
  std::error_code ec;
  stdfs::create_directory_symlink(outside, link, ec);
  REQUIRE_FALSE(ec);

  // 词法上完全合法 —— 只有真实路径解析才能发现它出界
  REQUIRE(fss::fs::LexicalJoin(root, "escape/pwned.txt").ok());
  const auto r = fss::fs::SafeJoin(root, "escape/pwned.txt");
  REQUIRE_FALSE(r.ok());
  REQUIRE(r.error().Find("resolved").has_value());

  // 正常键仍然通过
  REQUIRE(fss::fs::SafeJoin(root, "normal/ok.txt").ok());
}

TEST_CASE("原子写：内容精确、不残留临时文件、可覆盖", "[phase1][fs]") {
  fss::test::TempDir tmp("fs_atomic");
  const std::string dir = tmp.child("data");
  fss::fs::EnsureDir(dir);
  const std::string target = dir + "/obj.bin";

  const std::string payload = "hello-atomic-write";
  REQUIRE(fss::fs::AtomicWriteFile(target, payload).ok());

  auto read_back = fss::fs::ReadFile(target);
  REQUIRE(read_back.ok());
  REQUIRE(read_back.value() == payload);

  // 不残留 tmp 文件（临时名形如 <target>.tmp.<pid>.<suffix>.<rand>）
  int leftovers = 0;
  for (const auto& e : stdfs::directory_iterator(dir)) {
    if (e.path().filename().string().find(".tmp.") != std::string::npos) ++leftovers;
  }
  REQUIRE(leftovers == 0);

  // 覆盖写：内容整段替换，不出现"新旧混合"
  const std::string shorter = "short";
  REQUIRE(fss::fs::AtomicWriteFile(target, shorter).ok());
  auto r2 = fss::fs::ReadFile(target);
  REQUIRE(r2.ok());
  REQUIRE(r2.value() == shorter);
  auto sz = fss::fs::FileSize(target);
  REQUIRE(sz.ok());
  REQUIRE(sz.value() == shorter.size());
}

TEST_CASE("原子写失败时目标路径不出现半截文件", "[phase1][fs]") {
  fss::test::TempDir tmp("fs_atomic_fail");
  // 目标目录不存在 → 创建临时文件就失败
  const std::string target = tmp.child("nonexistent-dir/x.bin");
  const auto r = fss::fs::AtomicWriteFile(target, "data");
  REQUIRE_FALSE(r.ok());
  REQUIRE_FALSE(fss::fs::Exists(target));

  // 也不应在不存在的目录里留下任何东西
  REQUIRE_FALSE(stdfs::exists(tmp.child("nonexistent-dir")));
}

TEST_CASE("原子写可关闭 fsync（性能/耐久性开关，内容不受影响）", "[phase1][fs]") {
  fss::test::TempDir tmp("fs_atomic_nosync");
  const std::string dir = tmp.child("d");
  fss::fs::EnsureDir(dir);
  const std::string target = dir + "/x.bin";

  fss::fs::WriteOptions opts;
  opts.fsync_data = false;
  opts.fsync_dir = false;
  opts.tmp_suffix = "inst-1";
  REQUIRE(fss::fs::AtomicWriteFile(target, "abc", opts).ok());
  auto r = fss::fs::ReadFile(target);
  REQUIRE(r.ok());
  REQUIRE(r.value() == "abc");
}

TEST_CASE("ReadAt 使用 pread：区间读、越界、并发安全语义", "[phase1][fs]") {
  fss::test::TempDir tmp("fs_pread");
  const std::string dir = tmp.child("d");
  fss::fs::EnsureDir(dir);
  const std::string target = dir + "/data.bin";
  const std::string payload = "0123456789abcdefghij";
  REQUIRE(fss::fs::AtomicWriteFile(target, payload).ok());

  auto mid = fss::fs::ReadAt(target, 5, 4);
  REQUIRE(mid.ok());
  REQUIRE(mid.value() == "5678");

  // 跨越尾部：只返回实际可读的部分
  auto tail = fss::fs::ReadAt(target, 18, 10);
  REQUIRE(tail.ok());
  REQUIRE(tail.value() == "ij");

  // 完全越界：返回空（不是错误）—— 与 pread 的 EOF 语义一致
  auto beyond = fss::fs::ReadAt(target, 100, 4);
  REQUIRE(beyond.ok());
  REQUIRE(beyond.value().empty());

  // pread 不改变文件偏移：连续两次同区间读结果一致
  auto a = fss::fs::ReadAt(target, 0, 3);
  auto b = fss::fs::ReadAt(target, 0, 3);
  REQUIRE(a.value() == b.value());
  REQUIRE(a.value() == "012");
}

TEST_CASE("RemoveFile 幂等：不存在的文件视为成功", "[phase1][fs]") {
  fss::test::TempDir tmp("fs_remove");
  const std::string dir = tmp.child("d");
  fss::fs::EnsureDir(dir);
  const std::string target = dir + "/x";

  REQUIRE(fss::fs::RemoveFile(target).ok());  // 不存在也算成功
  REQUIRE(fss::fs::AtomicWriteFile(target, "1").ok());
  REQUIRE(fss::fs::Exists(target));
  REQUIRE(fss::fs::RemoveFile(target).ok());
  REQUIRE_FALSE(fss::fs::Exists(target));
}
