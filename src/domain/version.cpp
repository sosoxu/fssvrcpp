// 领域层占位 TU。
//
// 为什么需要它：fss_domain 必须是 STATIC 而不是 INTERFACE —— 只有真正编译一个 TU，
// 才能让"领域层误引入 grpc/sqlite/curl/openssl/httplib"在**构建期**直接失败，
// 而不是等到运行时才发现分层已被破坏。
#include "domain/version.h"

namespace fss::domain {
const char* LayerName() noexcept { return "domain"; }
int LayerIndex() noexcept { return 3; }
const char* BuildVersion() noexcept { return FSS_BUILD_VERSION; }
}  // namespace fss::domain
