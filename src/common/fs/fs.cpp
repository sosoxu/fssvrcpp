#include "common/fs/fs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "common/crypto/crypto.h"

namespace fss::fs {
namespace {

Error PathError(std::string_view key, std::string reason) {
  Error err(ErrorKind::kInvalidArgument, "非法路径键 \"" + std::string(key) + "\": " + reason);
  err.With("reason", std::move(reason));
  return err;
}

Error SysError(ErrorKind kind, std::string msg, std::string_view path) {
  Error err(kind, std::move(msg));
  err.With("path", std::string(path)).With("errno", std::to_string(errno));
  return err;
}

// 校验单个 key 的**词法**合法性；成功时返回规范化后的相对路径
Result<std::string> NormalizeKey(std::string_view key) {
  if (key.empty()) return PathError(key, "键为空");
  if (key.size() > kMaxKeyBytes) return PathError(key, "键过长（>" + std::to_string(kMaxKeyBytes) + " 字节）");
  if (key.find('\0') != std::string_view::npos) return PathError(key, "含 NUL 字节");
  if (key.front() == '/') return PathError(key, "不允许绝对路径");
  if (key.back() == '/') return PathError(key, "不允许以 '/' 结尾（只接受文件键）");

  std::string out;
  out.reserve(key.size());
  std::size_t start = 0;
  while (start <= key.size()) {
    std::size_t slash = key.find('/', start);
    const std::size_t end = (slash == std::string_view::npos) ? key.size() : slash;
    const std::string_view seg = key.substr(start, end - start);
    if (seg.empty()) return PathError(key, "存在空路径段（如 'a//b'）");
    if (seg == ".") return PathError(key, "存在 '.' 路径段");
    if (seg == "..") return PathError(key, "存在 '..' 路径段（路径穿越）");
    if (!out.empty()) out.push_back('/');
    out.append(seg);
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return out;
}

bool IsWithin(const std::filesystem::path& root, const std::filesystem::path& p) {
  // 逐段比较，避免 "/data" 与 "/database" 的前缀误判
  auto r = root.begin();
  auto q = p.begin();
  for (; r != root.end(); ++r, ++q) {
    if (q == p.end() || *r != *q) return false;
  }
  return true;
}

}  // namespace

Result<std::string> LexicalJoin(std::string_view root, std::string_view key) {
  FSS_TRY(norm, NormalizeKey(key));
  if (root.empty()) return Err(ErrorKind::kInvalidArgument, "root 为空");
  std::string out(root);
  if (out.back() == '/') out.pop_back();
  out.push_back('/');
  out.append(norm);
  return out;
}

Result<void> VerifyResolvedWithinRoot(std::string_view root, std::string_view path) {
  namespace stdfs = std::filesystem;
  std::error_code ec;
  const stdfs::path root_p{std::string(root)};
  const stdfs::path path_p{std::string(path)};

  const stdfs::path root_canon = stdfs::weakly_canonical(root_p, ec);
  if (ec) return SysError(ErrorKind::kInvalidArgument, "无法解析 root 真实路径", root);

  // 找到 path 的最长已存在前缀，并对它做真实路径解析
  stdfs::path probe = path_p;
  while (!probe.empty() && !stdfs::exists(probe, ec)) {
    probe = probe.parent_path();
  }
  if (probe.empty()) return Err(ErrorKind::kInvalidArgument, "路径没有已存在的祖先目录");
  const stdfs::path probe_canon = stdfs::weakly_canonical(probe, ec);
  if (ec) return SysError(ErrorKind::kInvalidArgument, "无法解析路径真实路径", path);

  if (!IsWithin(root_canon, probe_canon)) {
    Error err(ErrorKind::kInvalidArgument,
              "路径逃逸出 root（可能是符号链接指向外部）");
    err.With("root", root_canon.string()).With("resolved", probe_canon.string());
    return err;
  }
  return Ok();
}

Result<std::string> SafeJoin(std::string_view root, std::string_view key) {
  FSS_TRY(joined, LexicalJoin(root, key));
  FSS_TRY(VerifyResolvedWithinRoot(root, joined));
  return joined;
}

// -----------------------------------------------------------------------------
Result<void> AtomicWriteFile(std::string_view path, std::string_view data,
                             const WriteOptions& opts) {
  // 临时文件名必须**全局唯一**：含 pid + 随机后缀，避免多实例/多线程互踩（ADR-009 M1）
  const std::string tmp =
      std::string(path) + ".tmp." + std::to_string(::getpid()) + "." + opts.tmp_suffix +
      (opts.tmp_suffix.empty() ? "" : ".") + crypto::RandomHex(8);

  // ① 写临时文件
  int fd = ::open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC, opts.mode);
  if (fd < 0) return SysError(ErrorKind::kInternal, "创建临时文件失败", tmp);
  bool wrote_ok = true;
  std::size_t written = 0;
  while (written < data.size()) {
    const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
    if (n <= 0) { wrote_ok = false; break; }
    written += static_cast<std::size_t>(n);
  }
  // ② 数据先落盘（ADR-008 的 R1：必须先于改名 durable）
  if (wrote_ok && opts.fsync_data) {
    if (::fdatasync(fd) != 0) wrote_ok = false;
  }
  if (::close(fd) != 0) wrote_ok = false;

  if (!wrote_ok) {
    const int saved = errno;
    ::unlink(tmp.c_str());  // 尽力清理；失败也不掩盖原错误
    errno = saved;
    return SysError(ErrorKind::kInternal, "写入临时文件失败", tmp);
  }

  // ③ 原子改名（可见性原子性：目标路径只会完整出现或完全不出现）
  if (::rename(tmp.c_str(), std::string(path).c_str()) != 0) {
    const int saved = errno;
    ::unlink(tmp.c_str());
    errno = saved;
    return SysError(ErrorKind::kInternal, "rename 失败", path);
  }

  // ④ 目录落盘（ADR-008 的 R2：承诺前改名必须 durable）
  if (opts.fsync_dir) {
    const auto dir = std::filesystem::path(std::string(path)).parent_path();
    if (!dir.empty()) {
      int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
      if (dfd < 0) return SysError(ErrorKind::kInternal, "打开目录失败", dir.string());
      const int rc = ::fsync(dfd);
      ::close(dfd);
      if (rc != 0) return SysError(ErrorKind::kInternal, "fsync 目录失败", dir.string());
    }
  }
  return Ok();
}

// -----------------------------------------------------------------------------
Result<void> EnsureDir(std::string_view path, unsigned mode) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path{std::string(path)}, ec);
  if (ec) {
    Error err(ErrorKind::kInternal, "创建目录失败: " + ec.message());
    err.With("path", std::string(path));
    return err;
  }
  ::chmod(std::string(path).c_str(), mode);  // 尽力；失败不致命（umask 可能不同）
  return Ok();
}

bool Exists(std::string_view path) {
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path{std::string(path)}, ec);
}

Result<std::uint64_t> FileSize(std::string_view path) {
  std::error_code ec;
  const auto sz = std::filesystem::file_size(std::filesystem::path{std::string(path)}, ec);
  if (ec) return SysError(ErrorKind::kNotFound, "取文件大小失败", path);
  return static_cast<std::uint64_t>(sz);
}

Result<std::string> ReadFile(std::string_view path) {
  std::ifstream in(std::string(path), std::ios::binary);
  if (!in) return SysError(ErrorKind::kNotFound, "打开文件失败", path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

Result<void> RemoveFile(std::string_view path) {
  if (::unlink(std::string(path).c_str()) != 0) {
    if (errno == ENOENT) return Ok();  // 幂等：不存在视为成功
    return SysError(ErrorKind::kInternal, "删除文件失败", path);
  }
  return Ok();
}

Result<std::string> ReadAt(std::string_view path, std::uint64_t offset, std::size_t length) {
  int fd = ::open(std::string(path).c_str(), O_RDONLY);
  if (fd < 0) return SysError(ErrorKind::kNotFound, "打开文件失败", path);
  std::string buf(length, '\0');
  std::size_t total = 0;
  while (total < length) {
    // pread：不改变文件偏移 → 并发读同一文件天然安全
    const ssize_t n = ::pread(fd, buf.data() + total, length - total,
                              static_cast<off_t>(offset + total));
    if (n < 0) { const int e = errno; ::close(fd); errno = e; return SysError(ErrorKind::kInternal, "pread 失败", path); }
    if (n == 0) break;  // EOF
    total += static_cast<std::size_t>(n);
  }
  ::close(fd);
  buf.resize(total);
  return buf;
}

}  // namespace fss::fs
