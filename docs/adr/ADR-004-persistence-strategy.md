# ADR-004：位置记录与元数据记录的持久化策略

- 状态：已采纳（Accepted；**2025 复核更新：可选实现 `RemoteStorageServiceRepository` 已交付**
  —— `metadata.repository=remote` 现在是**真的可运行**的形态，不再是 exit 78；见文末
  「本切片的交付与偏离」）
- 日期：2025（复核更新：`metadata.remote.*` 落地切片）
- 相关文档：`docs/01-osdu-research.md` §5.5、`docs/02-design.md` §9、`docs/operations.md` §1.2.7/§1.3

## 背景

调研得到一个对本项目影响最大的事实（实测）：

> **OSDU File Service 自己不存文件元数据。**
> 元数据（`dataset--File.Generic` 记录）通过 core-common 的 `DataLakeStorageService`
> 转发给 **Storage Service**（`PUT {storage.api}/records`）。
> File Service 自己的数据库（Azure Cosmos DB / IBM Cloudant / GCP OSM / AWS DynamoDB）
> **只存 `FileLocation` 行**（fileID → driver → location），供 landing zone 相关的旧接口使用。
> 本仓库中**不存在 Elasticsearch 与 Postgres 的任何引用**；索引是 Storage Service 的职责。

这带来一个直接冲突：

| 约束 | 说明 |
| --- | --- |
| 需求 | 提供一个**可独立部署、可独立测试**的 OSDU 兼容后端（"每一步完成后先做测试"要求服务能在无整个 OSDU 平台的环境下运行） |
| 上游实现 | 强依赖 Storage Service；无 Storage Service 则 `POST /v2/files/metadata` 无法工作 |

## 决策

采用**双实现 + 默认内置**策略：

| 记录类型 | 默认实现（独立部署） | 可选实现（OSDU 平台内） | 端口 |
| --- | --- | --- | --- |
| **位置记录** `FileLocation` | `SqliteLocationRepository` | `InMemoryLocationRepository`（仅测试） | `IFileLocationRepository` |
| **元数据记录** `dataset--File.Generic` | **`SqliteMetadataRepository`**（**上游没有对应物**） | `RemoteStorageServiceRepository`（对接 Storage Service） | `IMetadataRepository` |

`IMetadataRepository` 的语义被设计成**同时满足两种后端**：

```cpp
class IMetadataRepository {
 public:
  virtual ~IMetadataRepository() = default;

  // 等价于 Storage 的 PUT /records（upsert）。返回分配到的 version。
  virtual Result<std::int64_t> Upsert(const CallerContext&, const FileMetadataRecord&) = 0;

  // 等价于 Storage 的 GET /records/{id}（取最新版本）。
  virtual Result<FileMetadataRecord> GetLatest(const CallerContext&,
                                              const std::string& id) = 0;

  // 等价于 Storage 的 POST /records/{id}:delete（上层要求返回 204 语义）。
  virtual Result<void> Delete(const CallerContext&, const std::string& id) = 0;
};
```

- **`SqliteMetadataRepository`**：自管版本链（`(partition_id, id, version)` 联合主键 +
  `is_latest` 标记 + `previous_version`），ACL/Legal 抽出为独立列以便后续鉴权与过滤。
- **`RemoteStorageServiceRepository`**：把上述三个方法映射到 Storage Service 的
  `/api/storage/v2/records` 端点；`Delete` 要求远端返回 204，否则视为失败。

选择哪一种由配置决定：`metadata.repository = sqlite | remote`。**默认 `sqlite`**。

## 明确记录的偏离（必须在交付文档中说明）

1. **`SqliteMetadataRepository` 是上游没有的能力**，属"平台外扩展"。
   在完整 OSDU 平台中部署时应改为 `remote`，以保持与上游一致的记录归属
   （Search/Indexer 依赖 Storage Service 的记录）。
2. 因此本项目**不声称**"元数据可被 OSDU Search 检索"——那是 Storage Service + Indexer 的职责。
   若需要该能力，必须使用 `metadata.repository=remote`。
3. 位置记录的 `partition_id` 被显式纳入主键/索引。上游 `file_locations_osm` 只有
   `id` 单列唯一约束，在多租户下依赖"fileID 全局唯一"。
   UUID 碰撞概率极低，但**契约上不应依赖概率**——本项目的做法是显式加租户维度。

## 备选方案与取舍

| 方案 | 优点 | 缺点 | 结论 |
| --- | --- | --- | --- |
| A. 完全复刻上游（只支持远端 Storage Service） | 与上游行为 100% 一致；元数据可被 Search 检索 | **服务无法独立运行/测试**，与"每步可测"的要求直接冲突；集成测试需要一整套 OSDU | 否决为默认（保留为可选实现） |
| B. **双实现，默认内置 SQLite** | 可独立部署与测试；同时保留平台内对接能力 | 需要维护两套实现的语义一致性；需在文档中明确偏离 | **采纳** |
| C. 只用内存元数据仓储 | 最简单 | 重启即丢数据；不具备生产可用性 | 否决（仅用于单元测试） |
| D. 用嵌入式 KV（如 RocksDB/LMDB）替代 SQLite | 性能可能更好 | 需要额外依赖，且本机无 root 无法安装；SQLite3 已是系统可用依赖 | 否决 |

## 后果

- 正面：P2–P7 的集成测试**不需要任何外部服务**（SQLite + 本地文件系统即可跑通全链路），
  这直接支撑了"每阶段先测"的流程。
- 负面：`RemoteStorageServiceRepository` 的语义一致性无法在无 OSDU 平台的环境中做真实端到端验证。
  **缓解**：用 `tests/tools/mock_storage_service.py`（Python 标准库）实现一个符合
  `/api/storage/v2/records` 契约的 mock，对 `RemoteStorageServiceRepository` 做契约测试；
  并在文档中标注"真实 Storage Service 的端到端验证**未做**"（诚实标注为 UNVERIFIED）。
- 需要在 `/v2/info` 的 `connectedOuterServices` 中反映实际使用的后端，
  便于运维判断当前部署形态。

## 待办（阶段 2 定义端口，阶段 6 实现）

- [x] `IMetadataRepository` / `IFileLocationRepository` 签名定稿（含 `CallerContext`）
- [x] `SqliteMetadataRepository` 的版本链与并发写测试
- [x] `tests/tools/mock_storage_service.py`（`PUT/GET /records`、`POST /records/{id}:delete` → 204）
      —— **实际落在** `tests/tools/mock_validators.py --mode storage`（复用既有 mock 基建，
      见 `tests/framework/mock_storage.h`）
- [x] `RemoteStorageServiceRepository` 契约测试（对 mock）：`tests/integration/test_remote_metadata_repository.cpp`
- [x] 在 `docs/operations.md` 中说明两种 `metadata.repository` 的适用场景与迁移注意事项（§1.2.7/§1.3）
- [ ] 在 `/v2/info` 中输出当前仓储实现，供运维核对 —— **未交付**：`/v2/info` 的
      `connectedOuterServices` 仍是恒定的 `["storage"]`（与本仓库既有实现一致），
      运维核对仓储实现的入口是**启动横幅**的 `repositories : metadata=remote（…）` 一行（R11）。

## 本切片的交付与偏离（`metadata.repository=remote`）

> 实现：`src/infra/metadata/remote/remote_metadata_repository.{h,cpp}`（L2）；
> 组合根 `src/main/server_main.cpp` 的四条 fail-closed 前置条件；用例
> `src/app/usecases/usecases.cpp` 的**无领取路径**；指标
> `fss_metadata_remote_requests_total{op,outcome}`；就绪探针经 `ports.shared_state_probe`
> 接入 REST 与 gRPC（同源）。判据见 `docs/test-evidence/phase10.md` §26。

`IMetadataRepository` 的实际签名**比上面的 3 方法草图丰富**（多了 C1 的 `ClaimForWrite` /
`MarkReady` / `ReleaseClaim` / `ReclaimStaleClaiming` 与 `State`/`is_latest` 语义）。
本切片**没有**假装远端能实现它们，而是把差异显式化：

| # | 偏离 | 说明与理由 |
| --- | --- | --- |
| 1 | **确定性记录 id**（偏离上游"服务端分配随机 id"） | `id = "<partition>:dataset--File.Generic:<8-4-4-4-12>"`，hex = `SHA-256(partition \|\| '\0' \|\| file_source)` 的前 128 bit。理由 = **R5**：幂等键必须建在幂等键上；上游那种随机 id 会让"同 `fileSource` 重试"变成两条记录。代价：id 不再是服务端不可猜的随机值（记录 id 本就可由对端枚举，不构成额外泄露）。`docs/01-osdu-research.md` §2.1 第 4 步是上游的随机形态 |
| 2 | **`atomic_claim = false`** | 远端 Storage Service 既没有"条件插入"原语，也没有一个**不属于 OSDU 记录**的 `state` 列 ⇒ ADR-009 §4.2 的原子领取在远端无法诚实实现。`capabilities()` 如实报告；四个领取原语返回 `kUnimplemented`；用例走**无领取路径**（幂等预检 → 复制 → `Create`）。**有意的降级**：并发同一 `fileSource` 没有互斥，可能复制两次并产生两条版本 —— 因此组合根强制 `deployment.mode=single` + `leases.enabled=false`（否则 exit 78）。生产多实例用 `postgres` |
| 3 | **`state` / `is_latest` 在远端没有对应物** | 它们是本仓库 SQLite/PG 仓储的**列**（`claiming → ready` 状态机 + 版本链的最新标记）。远端记录的版本链由 Storage Service 自己管（`GET /records/{id}` 返回最新版本），本服务不把 `state` 写进 OSDU JSON（这是既有硬要求）。代价：远端形态下"读路径只返回 ready"这条不变量由**没有 claiming 状态**来保证（没有在途记录） |
| 4 | **不使用未文档化的 query 端点** | `GetLatestByFileSource` = `GET {base}/records/{派生 id}`（不是 `?fileSource=`）。`List` 诚实地返回 `kUnimplemented`（ADR-004 的三方法草图与 `docs/01-osdu-research.md` 都没有"按条件列举记录"的端点依据）。附带修正：readiness 的兜底探针改为"`shared_state_probe` 存在时它是唯一判据"，否则 remote 的 `List` 会让 readiness **永远 503** |
| 5 | **`base_url` 是基址（与 `legal/schema.remote.base_url` 相反）** | 本适配器追加 `/records`、`/records/{id}`、`/records/{id}:delete`；legal/schema 的 `base_url` 是完整端点（ADR-013 §2）。两者混用会打到 `.../records/records`（404）。已在配置行、适配器头注释与运维文档显式写明 |
| 6 | **`token_provider` 只交付 `static`** | 静态 `Authorization: Bearer`；OAuth / service-account 换取流程**未实现**（非 `static` → exit 78）。**未与真实 Storage Service 联调** |

`/v2/info` 的 `connectedOuterServices` 未按原"后果"一节改动（仍是 `["storage"]`），
登记为未交付项（见上）；运维可见性由启动横幅承担。
