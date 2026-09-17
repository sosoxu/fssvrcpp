#include "common/result/result.h"

#include <sstream>

namespace fss {

std::string_view ErrorKindName(ErrorKind kind) noexcept {
  switch (kind) {
    case ErrorKind::kOk: return "kOk";
    case ErrorKind::kInvalidArgument: return "kInvalidArgument";
    case ErrorKind::kFileSourceEmpty: return "kFileSourceEmpty";
    case ErrorKind::kInvalidSourcePath: return "kInvalidSourcePath";
    case ErrorKind::kLocationAlreadyExists: return "kLocationAlreadyExists";
    case ErrorKind::kChecksumMismatch: return "kChecksumMismatch";
    case ErrorKind::kUnauthenticated: return "kUnauthenticated";
    case ErrorKind::kPermissionDenied: return "kPermissionDenied";
    case ErrorKind::kStorageAccessDenied: return "kStorageAccessDenied";
    case ErrorKind::kNotFound: return "kNotFound";
    case ErrorKind::kUnimplemented: return "kUnimplemented";
    case ErrorKind::kInternal: return "kInternal";
    case ErrorKind::kBadGateway: return "kBadGateway";
    case ErrorKind::kUnavailable: return "kUnavailable";
  }
  return "kUnknown";
}

std::optional<std::string> Error::Find(std::string_view key) const {
  for (const auto& [k, v] : details_) {
    if (k == key) return v;
  }
  return std::nullopt;
}

std::string Error::ToString() const {
  std::ostringstream os;
  os << ErrorKindName(kind_) << ": " << message_;
  if (!details_.empty()) {
    os << " [";
    bool first = true;
    for (const auto& [k, v] : details_) {
      if (!first) os << ", ";
      first = false;
      os << k << "=" << v;
    }
    os << "]";
  }
  return os.str();
}

}  // namespace fss
