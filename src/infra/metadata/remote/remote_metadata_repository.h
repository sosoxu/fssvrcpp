// =============================================================================
//  RemoteStorageServiceRepository（L2）—— 远端 **Storage Service** 元数据仓储
//  （`metadata.repository=remote`）
// =============================================================================
//  决策与依据
//    · `docs/adr/ADR-004-persistence-strategy.md`：OSDU File Service **自己不存元数据**
//      记录（`docs/01-osdu-research.md` §5.5），记录归 **Storage Service** 所有
//      （`PUT {storage.api}/records`）。本适配器就是把 `IMetadataRepository`
//      映射到那些端点，从而让 OSDU **Search / Indexer** 能看到记录。
//    · 默认仍是 `metadata.repository=sqlite`（上游没有的本地实现，ADR-004 的"平台外扩展"）。
//
//  ★★ base_url 是**基址**，不是完整端点（与 `legal.remote.base_url` /
//     `schema.remote.base_url` **正好相反** —— 那两个是完整 URL，不追加任何路径，
//     见 `src/infra/legal/remote_legal_validator.h`）。混用是本项目真实存在的部署缺陷：
//       metadata.remote.base_url = http://host/api/storage/v2   → 本文件追加路径
//       legal.remote.base_url    = http://host/some/endpoint    → 原样 POST
//     若把完整端点填进 `metadata.remote.base_url`，请求会打到
//     `.../records/records`（404）或 `.../records/{id}` 的父路径上 —— 组合根**无法**
//     替你判断哪种约定，因此这里必须显式写死：**追加**。运维文档 §1.2.7 逐行说明。
//
//  线协议（`docs/01-osdu-research.md` 的 Storage Service 端点；**由本项目的 mock 钉住**）
//    Create  → `PUT  {base}/records`            记录 JSON（`domain::ToJson`）；响应 = 记录 JSON
//    Update  → `PUT  {base}/records`            同上（版本由 Storage Service 递增）
//    GetById → `GET  {base}/records/{id}`       200 = 记录 JSON；404 → kNotFound
//    GetLatestByFileSource
//            → `GET  {base}/records/{derived_id}`（同一 fileSource 恒映射到同一 derived id，
//              因此**不需要**任何未文档化的 query 端点；ADR-004 的三方法草图里没有它）
//    Delete  → `POST {base}/records/{id}:delete`
//              **必须 204**；其它一切（含 200）都是失败（ADR-004 的硬要求，
//              绝不把"删了一半"报成成功）
//    Probe   → `GET  {base}/records/{derived id of a 固定不存在的 source}`
//              **404 = 服务活着**（就绪探针；见下）
//
//  ⚠️ 本文件的"记录 JSON 响应"形状是**本项目与 mock 的约定**，不是从上游一手代码读到的：
//     本环境**没有真实 Storage Service**，**未联调**。真实 Storage Service 的
//     `PUT /records` 在 OSDU 里返回的是 `{recordCount, recordIds, versions, ...}` 信封 ——
//     本适配器**同时接受**该信封（取 `versions[0]` + `recordIds[0]`）与"整条记录 JSON"
//     两种形状（`ParseRecordEnvelope`），但只有前者被 mock 覆盖到（`--put-envelope`）。
//     这一点如实登记在 `docs/operations.md`、`docs/adr/ADR-004-*.md` 与
//     `docs/test-evidence/phase10.md` §26 的「未覆盖与未验证」。
//
//  ★★ 确定性记录 id（R5：幂等键必须建在幂等键上，不能建在随机主键上）
//      `id = "<partition>:dataset--File.Generic:<8-4-4-4-12 的 lowercase hex>"`
//      hex = `crypto::Sha256(partition || '\0' || file_source)` 的**前 32 个 hex 字符**
//      （即前 128 bit），按 8-4-4-4-12 分组以保持契约要求的 id 形状。
//      · 派生函数是**纯函数**（`DeriveRemoteRecordId`）：跨进程/跨运行稳定；
//        不同的 fileSource（或 partition）→ 不同 id。
//      · ⚠️ **偏离上游**：上游让 Storage Service **自己分配**随机 id
//        （`docs/01-osdu-research.md` §2.1 第 4 步：`"<partition>:dataset--File.Generic:<uuid>"`）。
//        我们改成确定性 id，理由正是 R5：上游那种随机 id 让"同 fileSource 重试"
//        变成**两条记录**，幂等只能靠应用层 check-then-insert（而那正是 R5 禁止的形态）。
//        代价：两条不同 fileSource 的 id 不再是"服务端不可猜的随机值"（记录 id 本来就
//        可由对端枚举，不构成额外信息泄露）。
//
//  ★★ 能力：`atomic_claim = false`（**不假装**）
//      ADR-009 §4.2 的原子领取需要"条件插入"原语 + 一个 `state` 列；远端两样都没有。
//      因此：
//        · `capabilities()` → `{atomic_claim=false, backend_name="remote"}`；
//        · `ClaimForWrite` / `MarkReady` / `ReleaseClaim` / `ReclaimStaleClaiming`
//          一律 `kUnimplemented`（消息点名能力与理由），**在本形态下永不被调用**
//          （用例按 `capabilities()` 分支走"无领取路径"，见
//          `src/app/usecases/usecases.cpp` 的 `CreateFileMetadata`）。
//        · 由此产生**有意的降级**：并发同一 `fileSource` 的创建**没有互斥**，
//          可能复制两次并产生两条版本（对象的物理位置相同，因此不会丢数据）。
//          这是"生产用 PG（multi 强制 postgres）、remote 仅单实例"的代价，不是 bug。
//          已写进 `docs/operations.md`、`docs/runbook.md` 与证据文件。
//
//  ★ fail-closed 错误映射（镜像 `remote_legal_validator`，绝不把失败映射成成功）
//      传输层失败（连不上 / 超时 / DNS / TLS）      → kUnavailable（HTTP 503）
//      HTTP 401                                     → kUnauthenticated（401；点名 token 键）
//      HTTP 403                                     → kPermissionDenied（403）
//      HTTP 404（GET/DELETE；DELETE 时=记录不存在） → kNotFound（404）
//      其它非 2xx（含 DELETE 的 200）               → kUnavailable（503）
//      非 JSON / 缺必需字段                         → kUnavailable（503）
//      **没有**任何一条映射到成功。
//
//  ★ 本文件是 L2：依赖 L3 端口与 L1（json/result/crypto/metrics/logging）+ libcurl。
// =============================================================================
#pragma once

#include "common/logging/logging.h"
#include "common/metrics/metrics.h"
#include "common/result/result.h"
#include "domain/ports/ports.h"

#include <string>
#include <string_view>

namespace fss::infra {

//  `metadata.remote.*` 的四个键 → Options（组合根逐键填充）
struct RemoteMetadataRepositoryOptions {
  //  **基址**（例如 `http://host/api/storage/v2`）：本适配器会追加 `/records` 等路径。
  //  空 → 组合根**拒绝启动**（exit 78）；本实现自身在没配地址时也不发起任何请求。
  std::string base_url;
  //  `metadata.remote.static_token`（secret；**不打印**）。非空 → `Authorization: Bearer <t>`。
  std::string static_token;
  //  `metadata.remote.timeout_ms`：**连接 + 整体**都受它约束（见下）。
  int timeout_ms = 5000;
  //  连接超时：0 → `min(1000, timeout_ms)`（与既有远端适配器一致）。
  int connect_timeout_ms = 0;
  bool verify_tls = true;
  std::string ca_bundle_path;
  //  指标注册表（可为 null）。组合根**只在选择 remote 时**注册
  //  `fss_metadata_remote_requests_total{op,outcome}`（R11：依赖必须可见，
  //  且默认 sqlite 形态**不得**多出这一族 —— M12 断言）。
  metrics::Registry* metrics = nullptr;
};

//  确定性记录 id（纯函数；`partition || '\0' || file_source` 的 SHA-256 前 128 bit）。
//  ★ 用 `'\0'` 分隔：否则 `("ab","c")` 与 `("a","bc")` 会撞同一个摘要。
//  ★ 导出它是为了让测试**直接**钉住派生规则（M3 的 golden 值）：静默改算法会失败。
std::string DeriveRemoteRecordId(std::string_view partition, std::string_view file_source);

class RemoteMetadataRepository final : public domain::IMetadataRepository {
 public:
  RemoteMetadataRepository(RemoteMetadataRepositoryOptions options,
                           const logging::ILogger& logger);

  //  ★ {atomic_claim=false, backend_name="remote"}：远端没有条件插入/state 列。
  domain::MetadataCapabilities capabilities() const override {
    return domain::MetadataCapabilities{/*atomic_claim=*/false, /*backend_name=*/"remote"};
  }

  //  PUT {base}/records（记录 JSON 用 `domain::ToJson`，**不手写第二套 OSDU 映射**）。
  fss::Result<domain::FileMetadataRecord> Create(std::string_view partition,
                                                 const domain::FileMetadataRecord& record) override;
  fss::Result<domain::FileMetadataRecord> Update(std::string_view partition,
                                                 const domain::FileMetadataRecord& record) override;
  fss::Result<domain::FileMetadataRecord> GetById(std::string_view partition,
                                                  std::string_view record_id) override;
  fss::Result<domain::FileMetadataRecord> GetLatestByFileSource(
      std::string_view partition, std::string_view file_source) override;
  //  POST {base}/records/{id}:delete —— **只有 204 算成功**（ADR-004）。
  fss::Result<void> Delete(std::string_view partition, std::string_view record_id) override;

  //  ---- 远端不存在的原语：一律 kUnimplemented（消息点名能力与理由）----
  fss::Result<domain::MetadataClaim> ClaimForWrite(
      std::string_view partition, const domain::FileMetadataRecord& record) override;
  fss::Result<domain::FileMetadataRecord> MarkReady(std::string_view partition,
                                                    std::string_view record_id,
                                                    std::int64_t version,
                                                    const domain::FileMetadataRecord& record) override;
  fss::Result<void> ReleaseClaim(std::string_view partition, std::string_view record_id,
                                 std::int64_t version) override;
  fss::Result<std::int64_t> ReclaimStaleClaiming(
      std::string_view partition, std::int64_t older_than_epoch_seconds, int limit,
      const std::vector<std::string>& live_expired_sources) override;

  //  ★ `List` 需要一个**未文档化**的查询端点（`?fileSource=` / 分页）——ADR-004 的
  //    三方法草图里没有它，`docs/01-osdu-research.md` 也没有依据 ⇒ 诚实返回
  //    `kUnimplemented`，而不是编一个端点。产品读路径用 `GetById` /
  //    `GetLatestByFileSource`，**不依赖 List**。
  //    ⚠️ readiness 兜底探针过去会调 `metadata.List("__readiness__", {})`；本切片同时
  //    把适配层改成"`shared_state_probe` 存在时它是**唯一**判据"，否则 remote 形态会
  //    因为这里恒 `kUnimplemented` 而永远 not ready（见 router.cpp / file_service_adapter.cpp）。
  fss::Result<domain::MetadataPage> List(std::string_view partition,
                                         const domain::MetadataQuery& query) override;

  //  ★ 就绪探针：`GET {base}/records/{固定不存在 source 的 derived id}`。
  //    **404 = 服务活着**（探针本身不成功的 404 正是"服务在正常工作"的证据）。
  //    2xx 也视为活着（那个固定 source 意外存在时不误报）；401/403 → 不可用（点名 token）；
  //    其余失败 → `Err(kUnavailable, 可读原因)` ⇒ readiness 503 而 **liveness 仍 200**。
  //    `op = "probe"` 也计入 `fss_metadata_remote_requests_total`。
  fss::Result<void> Probe();

  //  组合根启动自检（未配置 base_url 时拒绝启动）
  bool Ready() const { return !options_.base_url.empty(); }
  std::string NotReadyReason() const;

  const std::string& base_url() const { return options_.base_url; }
  int timeout_ms() const { return options_.timeout_ms; }

 private:
  struct HttpReply {
    bool transport_ok = false;
    int status = 0;
    std::string body;
    std::string transport_error;
  };

  //  发一次请求（`path` 追加到 base_url 之后）。失败**不**在这里映射 —— 调用方按 op 映射。
  HttpReply Call(const char* op, const char* method, const std::string& path,
                 const std::string& body, bool send_json_body);

  void Record(const char* op, bool ok);
  //  统一失败映射 + 一条 Warn（op + status + error kind）。返回映射后的 Error。
  fss::Error Fail(const char* op, const HttpReply& reply, const char* what) const;
  //  解析"记录 JSON 或 Storage 的 {recordCount,recordIds,versions} 信封"。
  fss::Result<domain::FileMetadataRecord> ParseRecord(const char* op, const HttpReply& reply,
                                                      const domain::FileMetadataRecord* fallback) const;

  RemoteMetadataRepositoryOptions options_;
  const logging::ILogger& logger_;
};

}  // namespace fss::infra
