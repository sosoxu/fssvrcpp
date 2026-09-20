// =============================================================================
//  Router（L5 适配层）—— 把 OSDU 端点接到用例上
// =============================================================================
//  职责（`docs/02-design.md` §7.3 的三条纪律）
//    · 只做"HTTP ⇄ 领域"的翻译：解析请求 → 调用例 → 渲染 DTO；
//    · **不含业务判断**（不重算有效期、不拼路径、不判 kind）；
//    · **不含直接 IO**（除了把请求体交给 `TransferEndpoint` 的流式转发）。
//
//  `Wrap()` 是唯一的横切入口：构造 `CallerContext` → **鉴权预检** → 执行 →
//  把 `Error` 经 `ErrorToResponse` 映射成三种错误体之一 → 兜住异常并映射为 500。
//
//  ★ 为什么适配层也要判一次角色（P8，契约 §1.3 末段）
//    上游的鉴权在 Spring Security 过滤器里，**先于** controller 的参数绑定。
//    若本实现只在用例入口判，那么"形状非法的请求体"会先被 DTO 解析成 400 ——
//    未授权调用者就能靠 400/403 的差异做探测。因此 `Wrap` 先按路由声明的角色
//    拦截（401/403），用例入口的授权保留为**第二道**（纵深防御，且 gRPC 面只有它）。
//
//  ⚠️ P8 之前的 `user_id`：没有 JWT 解析器，因此先用可注入的 `x-user-id`
//     头（缺省值见 `RouterOptions`）。这是**显式占位**，P8 会替换成 token 里的 claim。
#pragma once

#include "adapters/http/dto/dto.h"
#include "adapters/http/http_error_mapper.h"
#include "adapters/http/metrics.h"
#include "app/tasks/gc_task.h"
#include "app/usecases/usecases.h"
#include "common/http/http.h"
#include "common/metrics/metrics.h"
#include "common/bytes/bytes.h"
#include "common/result/result.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fss::adapters::http {

//  数据面转发回调：由**组合根**绑定到 `infra::TransferEndpoint`（适配层不依赖 L2）
struct TransferCallbacks {
  //  PUT：把请求体流写进存储
  std::function<fss::Result<void>(std::string_view token, std::string_view expires,
                                  std::string_view signature, std::string_view partition,
                                  bytes::ByteSource& body)> put;
  //  GET：校验一次并返回一个**可定位读的字节源**（由组合根用 BlobByteSource 实现）
  std::function<fss::Result<std::shared_ptr<bytes::ByteSource>>(
      std::string_view token, std::string_view expires, std::string_view signature,
      std::string_view partition)> open_get;
};

//  REST base path（契约 §2）。**唯一来源**：自签 URL 的 `<base>` 必须包含它，
//  否则发出去的上传/下载地址会指向 404（P4-D04）。
inline constexpr std::string_view kDefaultBasePath = "/api/file";

//  按需 GC 回调（C9.31 / ADR-013 §10）：由**组合根**绑定到 `app::GcTask`。
//  为什么用回调而不是让适配层直接持有 `GcTask`（R12）：`GcTask` 是 L4 的具体实现，
//  适配层只认"跑一轮、拿到 `GcReport`"这个形状；周期调度（`GcScheduler`）与按需端点
//  因此共享**同一个** `GcTask` 实例（连带它的单飞护栏）。
struct GcCallbacks {
  //  `force_dry_run` 只能让本轮**更保守**：有效值 = `配置 gc.dry_run || 请求 dryRun`。
  //  ★ 绝不允许用查询参数把配置里的 dry-run 翻成"真删"（运维的预览开关不能被调用方降级）。
  std::function<fss::Result<app::GcReport>(const app::CallerContext&, bool force_dry_run)> run;
  //  本轮针对的 partition：`ops.gc_run` **不需要** `data-partition-id`（admin 运维动作），
  //  数据分区由组合根（单租户注册表）决定。
  std::string partition = "opendes";
  //  **运行态**字段：周期调度是否真的在跑。`gc.enabled=false` 时端点仍可用（按需 GC 与
  //  周期调度是两件事），此时报告里 `scheduled=false` —— 调用方一眼能区分
  //  "端点不可用"与"调度没开"。
  bool scheduled = false;
};

//  路由的角色要求（契约 §1.3 的手抄表；空 `roles` = 免鉴权路由，如 /v2/info 与数据面）
struct RouteAuth {
  std::vector<std::string_view> roles;  // "任一即通过"
  bool require_partition = true;        // `revokeURL` 为 false（契约 §1.2）
};

struct RouterOptions {
  //  ★ P9/C9.6：非 HTTP 类指标（存储操作/字节、GC）来自 L1 的注册表；
  //    为空时 `/metrics` 只渲染 HTTP 自己的指标（测试与嵌入式用法不受影响）
  fss::metrics::Registry* metrics_registry = nullptr;
  std::string base_path = std::string(kDefaultBasePath);
  ErrorFormat error_format = ErrorFormat::kAppError;
  //  P8 之前 allow-all（启动时必须打印显著告警，契约 §8 的 C8.5）
  bool auth_disabled = true;
  std::string default_user_id = "osdu-user";
  std::int64_t json_body_limit_bytes = 10 * 1024 * 1024;
  std::int64_t small_body_limit_bytes = 256 * 1024;
  //  ★ C10.16：`partition.file.<partition>.max_file_bytes` 的落点（0 = 不限，保持接线前行为）。
  //    数据面 `/v1/transfer/{token}` 的请求体上限；由组合根从**分区配置**填入。
  //    HTTP 包装层的既有语义（`common/http/http.h` 的 `RouteOptions::max_body_bytes`）：
  //    带 `Content-Length` → **413**（读体前前置拒绝）；chunked → 400。
  std::int64_t transfer_put_max_body_bytes = 0;
  //  契约 §7：指标是**非 OSDU 扩展**，因此不走 base path（否则污染 OSDU 路径空间）
  bool metrics_enabled = true;
  std::string metrics_path = "/metrics";
};

class Router {
 public:
  Router(app::UseCasePorts& ports, TransferCallbacks transfers, RouterOptions options = {},
         GcCallbacks gc = {})
      : ports_(ports),
        transfers_(std::move(transfers)),
        gc_(std::move(gc)),
        options_(std::move(options)) {}
  //  不要数据面时的便捷构造（运维/位置/元数据仍可用）
  explicit Router(app::UseCasePorts& ports, RouterOptions options = {}, GcCallbacks gc = {})
      : ports_(ports), gc_(std::move(gc)), options_(std::move(options)) {}

  //  注册契约 §2 的端点（运维 + 位置 + 元数据 + DMS/delivery/revoke + 数据面 + /metrics）
  void Register(fss::http::Server& server);

  //  ★ ADR-006：把**已经包装好**的 `transfer.get` handler 暴露给组合根，
  //    供进程内的大文件下载数据面复用。语义与 `Register` 注册的**完全同一个**：
  //      构造 `CallerContext` → 角色查表 → `authorizer.AuthorizeAny` →
  //      `ErrorToResponse(..., error_format)` → `metrics_.Observe(...)`。
  //    数据面**不得**绕过它去直接调 `transfers_.open_get`（那就是 ADR-006 §4 第 1 条
  //    禁止的"第二套 HTTP 语义"；用例 I1 专门注入这个错误实现并断言 P3 失败）。
  //    `transfers_.open_get` 为空时仍返回一个 handler（调用它会得到可读错误），
  //    由调用方决定要不要建数据面。
  fss::http::Handler BuildTransferGetHandler();

  //  供组合根打印生效配置
  const RouterOptions& options() const { return options_; }

  //  供测试断言计数（只读；`/metrics` 的渲染也从这里取）
  const HttpMetrics& metrics() const { return metrics_; }

 private:
  using Action =
      std::function<fss::Result<fss::http::Response>(fss::http::Request&, const app::CallerContext&)>;

  //  横切：上下文构造 + 鉴权预检 + 错误映射 + 异常兜底 + 指标计数
  //  ★ 鉴权要求按 `RouteOptions.name` 在 `RouteAuthTable()` 里查（单一表；
  //    未登记的路由 **fail-closed**，见 router.cpp）。
  fss::http::Handler Wrap(Action action);

  //  数据面 token 被拒的计数（只算"凭证类"错误：签名错/过期 → 401；用途/租户不符 → 403；
  //  对象缺失是 404，属于资源语义，不计入）
  void RecordTransferRejection(const fss::Error& error);

  app::UseCasePorts& ports_;
  TransferCallbacks transfers_;
  GcCallbacks gc_;
  RouterOptions options_;
  //  ★ 计数必须在**唯一横切入口** `Wrap()` 里做：否则每个 handler 各记一次，
  //    迟早漏掉某条路由（新增 19 条路由时这就是"指标静默缺失"的来源）
  HttpMetrics metrics_;
};

}  // namespace fss::adapters::http
