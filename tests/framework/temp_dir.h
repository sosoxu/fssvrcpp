// 测试用临时目录（RAII 清理）。多个测试文件共用，避免各自复制一份。
#pragma once

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

namespace fss::test {

class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    auto base = std::filesystem::temp_directory_path() /
                ("fss_" + tag + "_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter()++));
    std::filesystem::create_directories(base);
    path_ = base;
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);  // 清理失败不应导致测试失败
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::string str() const { return path_.string(); }
  std::string child(const std::string& name) const { return (path_ / name).string(); }

 private:
  static std::atomic<int>& counter() {
    static std::atomic<int> c{0};
    return c;
  }
  std::filesystem::path path_;
};

}  // namespace fss::test
