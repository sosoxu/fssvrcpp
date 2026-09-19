// =============================================================================
//  WebhookEventPublisher（L2）—— 事件出站发布（`events.publisher=webhook`）
// =============================================================================
//  决策见 `docs/adr/ADR-013-file-service-extension-endpoints.md` §9。要点：
//
//  · **这是平台外扩展，不是 OSDU 接口**：上游 File Service 通过消息总线（Kafka 类）
//    发布 `statusChanged` / `datasetDetails`，**没有** webhook 传输形态
//    （`docs/01-osdu-research.md` / `docs/03-api-contract.md` §2.6 第 4/10 步）。
//    本适配器把同一份事件形状 POST 给运维给的端点，是**本项目约定**。
//  · **非致命是唯一的失败语义**（与本仓库的远端 legal/schema 校验器**方向相反**）：
//    上游只 `log.warning("Failed to publish ...")`（`src/domain/ports/ports.h` 的
//    `StatusChangedEvent` / `DatasetDetailsEvent` 注释）。因此：
//      连不上 / 超时 / 非 2xx / 坏响应 → 返回 `Err(kUnavailable, 可读消息)`，
//      并在这里记一条 `Warn`；**用例层丢弃这个 Err**（`(void)ports.events.Publish...`），
//      HTTP 请求照常 **201**，且记录**真的建出来**。
//    ★ 绝不把发布失败变成请求失败，也绝不重试（见 §9 的未交付项）。
//  · `url` 就是**完整端点 URL**（POST 到它，**不追加任何路径**）——与切片 6a 的
//    `*.remote.base_url` 同一约定：没有上游路径依据时，把完整 URL 的控制权交给运维
//    （ADR-013 §2）。空 + `publisher=webhook` → 组合根用 `Ready()`/`NotReadyReason()`
//    **拒绝启动**（exit 78），而不是起来后每次静默丢事件。
//  · 线上形状（`topic` 取**配置值** `events.webhook.topic`，两个事件同一个 topic）：
//      statusChanged  : {"topic":T,"kind":"statusChanged",
//                        "body":{"recordId","partition","status","datasetSync","version"}}
//      datasetDetails : {"topic":T,"kind":"datasetDetails",
//                        "body":[{"properties":{"correlationId","datasetId","datasetType",
//                                "datasetVersionId","recordCount","timestamp"}}]}
//    `datasetDetails` 的 `body` 是**长度为 1 的数组**，对齐上游
//    `FileDatasetDetailsPublisher.java` 的形状（见 `ports.h` 的注释）。
//
//  ⚠️ **同步发布的延迟代价**：本实现是**内联同步** POST（上游是异步消息总线）。
//     一次 `createMetadata` 会发 2~3 个事件，因此一个慢 webhook 最多给请求路径增加
//     `事件数 × timeout_ms`。**异步有界发布队列/重试退避未交付**（ADR-013 §9.4）。
//
//  ⚠️ **未与真实消息总线/中间件联调**（本环境没有）：协议形状是本项目与运维方的约定。
//
//  本文件是 L2：只依赖 L3 端口与 L1（json/result/logging/curl）。
// =============================================================================
#pragma once

#include "common/logging/logging.h"
#include "common/result/result.h"
#include "domain/ports/ports.h"

#include <string>
#include <string_view>

namespace fss::infra {

struct WebhookEventPublisherOptions {
  std::string url;      // 完整端点 URL（POST 到它，不追加路径）；空 → 不发起请求
  int timeout_ms = 3000;  // 整体超时（`events.webhook.timeout_ms`）
  //  连接超时：0 → `min(1000, timeout_ms)`（与既有远端适配器一致）。显式给值则用它。
  int connect_timeout_ms = 0;
  //  ★ `events.webhook.topic` 的**配置值**：两个事件的载荷都用它（不是端口传入的 topic）。
  //    这正是"配置真的在起作用"的可证点（R1 自证②：硬编码成 "status-changed" 会让
  //    `tests/integration/test_webhook_publisher.cpp` 的 topic 断言失败）。
  std::string topic;
  bool verify_tls = true;
  std::string ca_bundle_path;
};

class WebhookEventPublisher final : public domain::IEventPublisher {
 public:
  WebhookEventPublisher(WebhookEventPublisherOptions options, const logging::ILogger& logger)
      : options_(std::move(options)), logger_(logger) {
    //  `min(1000, timeout_ms)`：timeout 比 1s 还短时，连接超时不能反而更长
    if (options_.connect_timeout_ms <= 0) {
      options_.connect_timeout_ms = options_.timeout_ms < 1000 ? options_.timeout_ms : 1000;
    }
  }

  //  失败（连不上 / 超时 / 非 2xx）→ `Err(kUnavailable)` + 一条 Warn；
  //  **调用方（用例）按上游语义丢弃它**，请求不受影响。
  fss::Result<void> PublishStatusChanged(std::string_view topic,
                                         const domain::StatusChangedEvent& event) override;
  fss::Result<void> PublishDatasetDetails(std::string_view topic,
                                          const domain::DatasetDetailsEvent& event) override;

  //  组合根用它做启动自检（未配置 URL 时**拒绝启动**）
  bool Ready() const { return !options_.url.empty(); }
  std::string NotReadyReason() const;

 private:
  //  实际发一次 POST；2xx → Ok，其余 → Err(kUnavailable, 可读消息) 并 Warn。
  fss::Result<void> Post(const std::string& kind, const std::string& request_body);

  //  载荷里的 topic：优先用**配置值**；配置为空时退回端口传入的 topic（防退化）。
  std::string EffectiveTopic(std::string_view port_topic) const {
    return options_.topic.empty() ? std::string(port_topic) : options_.topic;
  }

  WebhookEventPublisherOptions options_;
  const logging::ILogger& logger_;
};

}  // namespace fss::infra
