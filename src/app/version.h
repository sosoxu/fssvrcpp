// 应用层版本信息。应用层只依赖 fss_domain（见 src/CMakeLists.txt）。
#pragma once

#ifndef FSS_BUILD_VERSION
#define FSS_BUILD_VERSION "0.0.0-unknown"
#endif

namespace fss::app {
const char* LayerName() noexcept;
int LayerIndex() noexcept;
const char* BuildVersion() noexcept;
}  // namespace fss::app
