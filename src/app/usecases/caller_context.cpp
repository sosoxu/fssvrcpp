// caller_context 实现。规则与 REST 适配层的历史行为逐条一致（P7 抽出以便 gRPC 复用）。
#include "app/usecases/caller_context.h"

namespace fss::app {

CallerContext CallerFromHeaders(const HeaderLookup& lookup, std::string_view correlation_id,
                               std::string_view default_user_id) {
  CallerContext caller;
  if (const auto value = lookup("data-partition-id"); value.has_value()) {
    caller.partition = *value;
  }
  if (const auto value = lookup("authorization"); value.has_value()) {
    caller.bearer_token = *value;
  }
  if (const auto value = lookup("x-user-id"); value.has_value()) {
    caller.user_id = *value;
  }
  if (caller.user_id.empty()) caller.user_id = std::string(default_user_id);
  caller.correlation_id = std::string(correlation_id);
  return caller;
}

}  // namespace fss::app
