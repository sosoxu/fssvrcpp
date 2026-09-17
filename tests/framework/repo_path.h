// 测试脚手架：把仓库相对路径转成绝对路径（`FSS_REPO_ROOT` 由 fss_test_support 注入）
#pragma once

#include <string>

namespace fss::test {

inline std::string RepoRelative(const std::string& relative) {
  return std::string(FSS_REPO_ROOT) + "/" + relative;
}

}  // namespace fss::test
