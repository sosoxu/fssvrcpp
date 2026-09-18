#include "app/services/object_key_policy.h"

#include "common/result/result.h"
#include "common/time/time_format.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace fss::app {
namespace {

bool AllDigits(std::string_view s) {
  if (s.empty()) return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// `yyyy-MM-dd-HH-mm-ss-SSS`：17 位数字 + 4 个 '-'，共 21 字符（日期部分 10 + 时间 8 + 毫秒 3）
bool IsTimestampShape(std::string_view ts) {
  if (ts.size() != 23) return false;
  const std::size_t dashes[] = {4, 7, 10, 13, 16, 19};
  for (const auto pos : dashes) {
    if (ts[pos] != '-') return false;
  }
  for (std::size_t i = 0; i < ts.size(); ++i) {
    if (std::find(std::begin(dashes), std::end(dashes), i) != std::end(dashes)) continue;
    if (!std::isdigit(static_cast<unsigned char>(ts[i]))) return false;
  }
  return true;
}

std::string UpperToLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

}  // namespace

bool ObjectKeyPolicy::IsSafeSegment(std::string_view segment) {
  if (segment.empty() || segment == "." || segment == "..") return false;
  for (const char c : segment) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) return false;                       // 控制字符（含 NUL）
    if (c == '/' || c == '\\' || c == '%') return false;           // 分隔符与编码残留
    const bool ok = std::isalnum(u) != 0 || c == '.' || c == '_' || c == '-';
    if (!ok) return false;                                         // 白名单之外一律拒绝
  }
  return true;
}

std::string ObjectKeyPolicy::FormatTimestamp(std::int64_t epoch_seconds, int millis) {
  //  复用 L1 的格式化（ISO-8601 → 把 'T' 与 ':' 换成 '-'，去掉 'Z'）
  std::string iso = time::ToIso8601Utc(epoch_seconds, millis);  // 2021-03-03T15:13:33.120Z
  if (iso.size() < 20) return {};
  std::string out = iso.substr(0, 19);  // 2021-03-03T15:13:33
  std::replace(out.begin(), out.end(), 'T', '-');
  std::replace(out.begin(), out.end(), ':', '-');
  out += "-";
  out += iso.substr(20, 3);  // 毫秒
  return out;
}

Result<std::string> ObjectKeyPolicy::MakeFileSource(const SourcePath& parts) {
  if (!IsSafeSegment(parts.user_id)) {
    return Err(ErrorKind::kInvalidArgument, "userId 段非法：" + parts.user_id);
  }
  if (!IsSafeSegment(parts.file_id) || parts.file_id.size() > kMaxFileIdLength) {
    return Err(ErrorKind::kInvalidArgument, "fileID 段非法：" + parts.file_id);
  }
  if (parts.epoch_millis <= 0) {
    return Err(ErrorKind::kInvalidArgument, "epochMillis 必须为正");
  }
  if (!IsTimestampShape(parts.timestamp)) {
    return Err(ErrorKind::kInvalidArgument, "时间戳格式必须是 yyyy-MM-dd-HH-mm-ss-SSS：" +
                                                parts.timestamp);
  }
  std::string out = "/";
  out += parts.user_id;
  out += '/';
  out += std::to_string(parts.epoch_millis);
  out += '-';
  out += parts.timestamp;
  out += '/';
  out += parts.file_id;
  if (out.size() > kMaxFileSourceLength) {
    return Err(ErrorKind::kInvalidArgument,
               "FileSource 超过长度上限 " + std::to_string(kMaxFileSourceLength));
  }
  return out;
}

Result<ObjectKeyPolicy::SourcePath> ObjectKeyPolicy::ParseFileSource(std::string_view file_source) {
  if (file_source.empty()) {
    return Err(ErrorKind::kInvalidArgument, "FileSource can not be empty");
  }
  if (file_source.size() > kMaxFileSourceLength) {
    return Err(ErrorKind::kInvalidArgument,
               "FileSource 超过长度上限 " + std::to_string(kMaxFileSourceLength));
  }
  if (file_source.front() != '/') {
    return Err(ErrorKind::kInvalidArgument,
               "Invalid source file path to copy from " + std::string(file_source));
  }
  //  ★ 逐字符白名单先过一遍：把 NUL、控制字符、`%`、`\` 这类直接挡在外面
  for (const char c : file_source) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) {
      return Err(ErrorKind::kInvalidArgument,
                 "Invalid source file path to copy from " + std::string(file_source));
    }
    if (c == '\\' || c == '%') {
      return Err(ErrorKind::kInvalidArgument,
                 "Invalid source file path to copy from " + std::string(file_source));
    }
  }

  //  切成段：期望恰好 3 段（user / epoch-timestamp / fileID）
  std::vector<std::string_view> segments;
  std::size_t i = 1;  // 跳过前导 '/'
  while (i <= file_source.size()) {
    const auto slash = file_source.find('/', i);
    const auto end = slash == std::string_view::npos ? file_source.size() : slash;
    segments.push_back(file_source.substr(i, end - i));
    if (slash == std::string_view::npos) break;
    i = slash + 1;
    if (i > file_source.size()) break;
  }
  if (segments.size() != 3) {
    return Err(ErrorKind::kInvalidArgument,
               "Invalid source file path to copy from " + std::string(file_source));
  }

  SourcePath parts;
  parts.user_id = std::string(segments[0]);
  if (!IsSafeSegment(parts.user_id)) {
    return Err(ErrorKind::kInvalidArgument,
               "Invalid source file path to copy from " + std::string(file_source));
  }

  // 第 2 段：`<epochMillis>-<timestamp>`
  const std::string_view epoch_and_ts = segments[1];
  const auto dash = epoch_and_ts.find('-');
  if (dash == std::string_view::npos) {
    return Err(ErrorKind::kInvalidArgument,
               "Invalid source file path to copy from " + std::string(file_source));
  }
  const std::string_view epoch_text = epoch_and_ts.substr(0, dash);
  const std::string_view ts = epoch_and_ts.substr(dash + 1);
  if (!AllDigits(epoch_text) || epoch_text.size() > 19 || !IsTimestampShape(ts)) {
    return Err(ErrorKind::kInvalidArgument,
               "Invalid source file path to copy from " + std::string(file_source));
  }
  parts.epoch_millis = std::strtoll(std::string(epoch_text).c_str(), nullptr, 10);
  parts.timestamp = std::string(ts);

  parts.file_id = std::string(segments[2]);
  if (!IsSafeSegment(parts.file_id) || parts.file_id.size() > kMaxFileIdLength) {
    return Err(ErrorKind::kInvalidArgument,
               "Invalid source file path to copy from " + std::string(file_source));
  }
  return parts;
}

std::string ObjectKeyPolicy::MakePosixKey(const SourcePath& parts) {
  // POSIX 的 key 与 FileSource 同形（去掉前导 '/'）：运维可以直接按 FileSource 找文件
  return parts.user_id + "/" + std::to_string(parts.epoch_millis) + "-" + parts.timestamp + "/" +
         parts.file_id;
}

std::string ObjectKeyPolicy::MakeS3Key(const SourcePath& parts) {
  // 日期分层：`<yyyy>/<MM>/<dd>/<fileID>`（契约 §2.1 的 URL 样例）
  const std::string& ts = parts.timestamp;  // yyyy-MM-dd-HH-mm-ss-SSS
  if (ts.size() < 10) return parts.file_id;
  const std::string yyyy = ts.substr(0, 4);
  const std::string mm = ts.substr(5, 2);
  const std::string dd = ts.substr(8, 2);
  return yyyy + "/" + mm + "/" + dd + "/" + parts.file_id;
}

namespace {

//  容器名的安全白名单（partition 名与**配置里的容器名**共用一套规则）：
//  非空、无控制字符、无路径分隔符、无 '%'（避免二次编码）。容器名随后会作为
//  `fs::LexicalJoin` 的 key 段使用，任何能构成路径语义的字符都不能放过。
Result<std::string> ValidateContainerName(std::string_view name, const char* what) {
  if (name.empty()) {
    return Err(ErrorKind::kInvalidArgument, std::string(what) + " 不能为空");
  }
  for (const char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F || c == '/' || c == '\\' || c == '%') {
      return Err(ErrorKind::kInvalidArgument,
                 std::string(what) + " 含非法字符：" + std::string(name));
    }
  }
  return std::string(name);
}

}  // namespace

Result<std::string> ObjectKeyPolicy::ContainerFor(std::string_view partition, domain::StorageZone zone) {
  FSS_TRY(name, ValidateContainerName(partition, "partition"));
  std::string out = UpperToLower(name);
  out += '-';
  out += (zone == domain::StorageZone::kStaging) ? "staging" : "persistent";
  return out;
}

Result<std::string> ObjectKeyPolicy::ContainerFor(const domain::PartitionConfig& config,
                                                  domain::StorageZone zone) {
  const std::string& override_name = (zone == domain::StorageZone::kStaging)
                                         ? config.staging_container
                                         : config.persistent_container;
  if (override_name.empty()) return ContainerFor(config.partition, zone);
  return ValidateContainerName(override_name, "配置的容器名");
}

Result<std::string> ObjectKeyPolicy::ContainerFor(domain::IPartitionRegistry& partitions,
                                                  std::string_view partition,
                                                  domain::StorageZone zone) {
  const auto config = partitions.Get(partition);
  if (config.ok()) return ContainerFor(config.value(), zone);
  //  注册表里没有这个 partition（例如内置注册表只登记 opendes）→ **接线前的默认命名**，
  //  绝不因为"查不到配置"而改变既有行为。
  return ContainerFor(partition, zone);
}

bool ObjectKeyPolicy::IsValidFileId(std::string_view file_id) {
  //  契约 §1.5：`^[\w,\s-]+(\.\w+)?$`，且长度有上限。
  //  ⚠️ `\s` 意味着**空格是允许的**（上游正则如此）—— 这里如实照做，
  //     但额外拒绝控制字符与分隔符（那些不在 `\w,\s-` 里，正则本身也会拒）。
  if (file_id.empty() || file_id.size() > kMaxFileIdLength) return false;
  std::size_t i = 0;
  std::size_t base_len = 0;
  for (; i < file_id.size(); ++i) {
    const auto u = static_cast<unsigned char>(file_id[i]);
    if (std::isalnum(u) != 0 || file_id[i] == '_' || file_id[i] == '-' || file_id[i] == ',' ||
        file_id[i] == ' ' || file_id[i] == '\t') {
      ++base_len;
      continue;
    }
    break;
  }
  if (base_len == 0) return false;
  if (i == file_id.size()) return true;
  // 余下必须是 `.` + 至少一个 `\w`
  if (file_id[i] != '.') return false;
  ++i;
  if (i >= file_id.size()) return false;
  for (; i < file_id.size(); ++i) {
    const auto u = static_cast<unsigned char>(file_id[i]);
    if (std::isalnum(u) == 0 && file_id[i] != '_') return false;
  }
  return true;
}

}  // namespace fss::app
