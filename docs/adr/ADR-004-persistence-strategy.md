# ADR-004：位置记录与元数据记录的持久化策略

- 状态：已采纳（Accepted，待阶段 6 实现后复核）
- 日期：2025
- 相关文档：`docs/01-osdu-research.md` §5.5、`docs/02-design.md` §9

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

- [ ] `IMetadataRepository` / `IFileLocationRepository` 签名定稿（含 `CallerContext`）
- [ ] `SqliteMetadataRepository` 的版本链与并发写测试
- [ ] `tests/tools/mock_storage_service.py`（`PUT/GET /records`、`POST /records/{id}:delete` → 204）
- [ ] `RemoteStorageServiceRepository` 契约测试（对 mock）
- [ ] 在 `docs/operations.md` 中说明两种 `metadata.repository` 的适用场景与迁移注意事项
- [ ] 在 `/v2/info` 中输出当前仓储实现，供运维核对
