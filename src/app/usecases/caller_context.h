// =============================================================================
//  caller_context（L4）—— 两条协议链路**共用**的调用方解析
// =============================================================================
//  REST 从 HTTP 头取 `data-partition-id` / `authorization` / `x-user-id`，
//  gRPC 从调用元数据取同名键（契约 §4.3）。规则必须**只有一份**，否则两条链路的
//  鉴权/租户行为会悄悄分叉（那正是"双协议等价"最怕的事）。
//
//  这里只做"取键 → 填 `CallerContext`"，不查角色、不做校验：
//     · 缺 token / 缺 partition 由用例入口判定（契约 §1.2：中间件与用例是同一约束的两道防线）
//     · 角色判定由 `IAuthorizer` 负责
// =============================================================================
#pragma once

#include "app/usecases/usecases.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace fss::app {

//  按名字取一个头/元数据值；返回 nullopt 表示"没有这个键"
using HeaderLookup = std::function<std::optional<std::string>(std::string_view)>;

//  解析调用方。`correlation_id` 由适配层自己解析（HTTP 用 `x-correlation-id`，
//  gRPC 用 `correlation-id`），`default_user_id` 来自配置（缺 `x-user-id` 时兜底）。
CallerContext CallerFromHeaders(const HeaderLookup& lookup, std::string_view correlation_id,
                                std::string_view default_user_id);

}  // namespace fss::app
