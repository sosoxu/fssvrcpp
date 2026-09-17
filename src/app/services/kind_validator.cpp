#include "app/services/kind_validator.h"

#include <regex>

namespace fss::app {
namespace {

// 契约 §1.5 的**原样**正则（版本段的 `.` 未转义，如实保留）
const std::regex& KindRegex() {
  static const std::regex re(R"(^[\w\-\.]+:[\w\-\.]+:[\w\-\.]+:[0-9]+.[0-9]+.[0-9]+$)");
  return re;
}

std::vector<std::string> Split(std::string_view text, char sep) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (true) {
    const auto pos = text.find(sep, start);
    const auto end = pos == std::string_view::npos ? text.size() : pos;
    out.emplace_back(text.substr(start, end - start));
    if (pos == std::string_view::npos) break;
    start = pos + 1;
  }
  return out;
}

}  // namespace

bool KindValidator::MatchesSyntax(std::string_view kind) {
  return std::regex_match(std::string(kind), KindRegex());
}

Result<KindParts> KindValidator::Validate(std::string_view kind) {
  // ① 语法
  if (!MatchesSyntax(kind)) {
    return Err(ErrorKind::kInvalidArgument, std::string(kInvalidKindMessage));
  }
  // ② 语义：恰好 4 段
  const auto parts = Split(kind, ':');
  if (parts.size() != 4) {
    return Err(ErrorKind::kInvalidArgument, std::string(kInvalidKindMessage));
  }
  if (parts[1] != kWorkspaceSource) {
    return Err(ErrorKind::kInvalidArgument, std::string(kInvalidKindSourceMessage));
  }
  if (parts[2] != kFileGenericEntity) {
    return Err(ErrorKind::kInvalidArgument, std::string(kInvalidKindEntityMessage));
  }
  KindParts out;
  out.partition = parts[0];
  out.source = parts[1];
  out.entity = parts[2];
  out.version = parts[3];
  return out;
}

}  // namespace fss::app
