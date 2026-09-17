// 领域层版本信息。领域层只依赖 fss_common（见 src/CMakeLists.txt）。
#pragma once

#ifndef FSS_BUILD_VERSION
#define FSS_BUILD_VERSION "0.0.0-unknown"
#endif

namespace fss::domain {
const char* LayerName() noexcept;
int LayerIndex() noexcept;
const char* BuildVersion() noexcept;
}  // namespace fss::domain
