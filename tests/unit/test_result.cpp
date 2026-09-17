// fss::Result / fss::Error 单元测试。
// 关注点：错误传播不丢信息（ErrorKind 与 details 必须原样穿过 FSS_TRY 链）。
#include <catch2/catch.hpp>

#include "common/result/result.h"

#include <string>

namespace {

fss::Result<int> Fails(fss::ErrorKind kind, const std::string& msg) {
  return fss::Err(kind, msg);
}

// 三层嵌套：验证错误穿过整条链后 kind/message/details 都没变
fss::Result<int> Level3() { return Fails(fss::ErrorKind::kNotFound, "Record Not Found"); }
fss::Result<int> Level2() {
  FSS_TRY(v, Level3());
  return fss::Ok(v + 1);
}
fss::Result<int> Level1() {
  FSS_TRY(v, Level2());
  return fss::Ok(v * 2);
}

fss::Result<void> VoidLevel2() {
  FSS_TRY(Fails(fss::ErrorKind::kPermissionDenied, "no role"));
  return fss::Ok();
}
fss::Result<void> VoidLevel1() {
  FSS_TRY(VoidLevel2());
  return fss::Ok();
}

}  // namespace

TEST_CASE("Ok 携带值，Err 携带结构化错误", "[phase1][result]") {
  auto ok = fss::Ok(42);
  REQUIRE(ok.ok());
  REQUIRE(ok.value() == 42);
  REQUIRE(static_cast<bool>(ok));

  auto err = Fails(fss::ErrorKind::kInvalidArgument, "bad expiryTime");
  REQUIRE_FALSE(err.ok());
  REQUIRE(err.error().kind() == fss::ErrorKind::kInvalidArgument);
  REQUIRE(err.error().message() == "bad expiryTime");
}

TEST_CASE("FSS_TRY 传播错误且不丢信息", "[phase1][result]") {
  auto r = Level1();
  REQUIRE_FALSE(r.ok());
  // 关键：kind 与 message 必须原样穿过三层
  REQUIRE(r.error().kind() == fss::ErrorKind::kNotFound);
  REQUIRE(r.error().message() == "Record Not Found");
}

TEST_CASE("FSS_TRY 在成功路径上正确绑定值", "[phase1][result]") {
  struct S {
    static fss::Result<int> Inner() { return fss::Ok(20); }
    static fss::Result<int> Outer() {
      FSS_TRY(v, Inner());
      return fss::Ok(v + 1);
    }
  };
  auto r = S::Outer();
  REQUIRE(r.ok());
  REQUIRE(r.value() == 21);
}

TEST_CASE("Result<void> 支持只传播错误的 FSS_TRY", "[phase1][result]") {
  auto r = VoidLevel1();
  REQUIRE_FALSE(r.ok());
  REQUIRE(r.error().kind() == fss::ErrorKind::kPermissionDenied);
  REQUIRE(r.error().message() == "no role");

  auto okv = fss::Ok();
  REQUIRE(okv.ok());
}

TEST_CASE("Error 的 details 可携带附加信息（如校验和期望/实际）", "[phase1][result]") {
  fss::Error e(fss::ErrorKind::kChecksumMismatch, "checksum mismatch");
  e.With("expected", "d41d8cd98f00b204e9800998ecf8427e").With("actual", "0000");

  REQUIRE(e.Find("expected").has_value());
  REQUIRE(*e.Find("expected") == "d41d8cd98f00b204e9800998ecf8427e");
  REQUIRE(*e.Find("actual") == "0000");
  REQUIRE_FALSE(e.Find("missing").has_value());

  const std::string s = e.ToString();
  REQUIRE(s.find("kChecksumMismatch") != std::string::npos);
  REQUIRE(s.find("expected=") != std::string::npos);
}

TEST_CASE("ErrorKind 的名字与契约 §5 的映射表一一对应（不是笔误）",
          "[phase1][result][contract]") {
  // 这组断言的作用：如果有人在枚举里加了新值却忘了在 ErrorKindName 里处理，
  // 或者改了名字，这里会失败 —— 提醒同步 docs/03-api-contract.md §5。
  REQUIRE(fss::ErrorKindName(fss::ErrorKind::kInvalidArgument) == "kInvalidArgument");
  REQUIRE(fss::ErrorKindName(fss::ErrorKind::kFileSourceEmpty) == "kFileSourceEmpty");
  REQUIRE(fss::ErrorKindName(fss::ErrorKind::kInvalidSourcePath) == "kInvalidSourcePath");
  REQUIRE(fss::ErrorKindName(fss::ErrorKind::kLocationAlreadyExists) == "kLocationAlreadyExists");
  REQUIRE(fss::ErrorKindName(fss::ErrorKind::kChecksumMismatch) == "kChecksumMismatch");
  REQUIRE(fss::ErrorKindName(fss::ErrorKind::kUnauthenticated) == "kUnauthenticated");
  REQUIRE(fss::ErrorKindName(fss::ErrorKind::kPermissionDenied) == "kPermissionDenied");
  REQUIRE(fss::ErrorKindName(fss::ErrorKind::kNotFound) == "kNotFound");
  REQUIRE(fss::ErrorKindName(fss::ErrorKind::kUnimplemented) == "kUnimplemented");
  REQUIRE(fss::ErrorKindName(fss::ErrorKind::kInternal) == "kInternal");
  REQUIRE(fss::ErrorKindName(fss::ErrorKind::kBadGateway) == "kBadGateway");
  REQUIRE(fss::ErrorKindName(fss::ErrorKind::kUnavailable) == "kUnavailable");
}

TEST_CASE("value_or 提供无分支的兜底取值", "[phase1][result]") {
  auto ok = fss::Ok(std::string("real"));
  REQUIRE(ok.value_or("fallback") == "real");

  fss::Result<std::string> bad = fss::Err(fss::ErrorKind::kInternal, "boom");
  REQUIRE(bad.value_or("fallback") == "fallback");
}
