# OSDU File Service 调研报告

> 调研方式：**直接拉取并阅读参考实现源码**，而非仅依赖文档转述。
> 取样版本：`osdu/platform/system/file` 仓库 `master` 分支（GitLab project id `90`，
> 归档 `file-master-d7c25c2d7f5d2f42bed901c68a407098195389bb`，
> 该仓库最后活动时间 2026-09-15，共 698 个文件）。
> 本报告中的"实测"均指在该归档中检索/阅读代码得到的一手结论；
> 标注 **UNVERIFIED** 的条目表示未能取得直接证据。

---

## 1. OSDU 是什么

**OSDU（Open Subsurface Data Universe）** 是由 The Open Group 主导的能源行业数据平台标准，目标是为地下/井筒/地震/油藏等勘探开发数据提供统一的"数据平台 + 领域数据服务"。

关键事实（对本项目有直接影响）：

- OSDU 是一个**微服务集合**。File Service 只是其中一个"核心服务（Core Service）"。
- 平台的服务边界由 **REST/HTTP+JSON + OpenAPI 3.x** 定义。**没有 gRPC/RPC**（详见 §9）。
- 平台的横切能力由其他服务提供，File Service **依赖**它们而非自己实现：
  - **Partition Service**：租户（partition）注册与租户级配置（partition properties）。
  - **Entitlements Service**：ACL 组与用户授权。File Service 通过 `service.file.viewers/editors/admin` 等组做鉴权。
  - **Legal Service**：法律标签（legal tag）校验。
  - **Storage Service**：记录（record）的元数据存储、版本管理、索引。
  - **Schema Service**：`File.Generic` 等 kind 的 schema 校验。
- 服务标识：**kind** 形如 `{partition}:{dataset-name}:{record-type}:{version}`，例如 `opendes:wks:dataset--File.Generic:1.0.0`；**record id** 形如 `{partition}:{type}:{uuid}`。

对本项目的启示：**完全的 OSDU 合规 = 需要整个平台**。本项目目标是"OSDU 兼容的 File Service 独立后端"，因此必须把对 Partition/Entitlements/Legal/Storage 的依赖做成**可插拔端口**（既可接真实 OSDU 服务，也可用本地实现），否则无法独立部署与测试。这一判断直接决定了 `docs/02-design.md` 中的端口划分。

---

## 2. File Service 的职责与工作流

File Service 的定位（引自官方文档 [File Service](https://osdu.pages.opengroup.org/platform/system/file/File-Service/)）：

> The File Service allows users to manage files on the data platform. File Management includes uploads, downloads, creation and retrieval of metadata record for files.

**核心特征：File Service 不代理文件字节。** 它只做三件事：

1. **发放位置**（landing zone）：为一次上传生成签名 URL + `FileSource`；
2. **登记元数据**：文件上传完成后，客户端提交元数据，服务把文件从 landing 区**复制到 persistent 区**，并在 Storage Service 中创建记录；
3. **发放下载位置**：为已登记的文件生成下载签名 URL。

### 三段式工作流（实测自 `docs/` 与参考实现）

```
 ①  POST /v2/files/uploadURL        ┌─────────────────────────┐
     (或 POST /v2/getLocation)      │ 响应: FileID + Location │
     ──────────────────────────────▶│  { SignedURL, ... }     │
                                    └─────────────────────────┘
                                                │
 ②  客户端 PUT 字节到 SignedURL                  ▼
     ─────────────────────────────────▶ [ landing / staging 区 ]
                                                │
 ③  POST /v2/files/metadata                     ▼
     body.FileSource = 上一步的 FileSource  ─────────────▶ 复制到 persistent 区
     ──────────────────────────────────────────────────▶ 创建 Storage 记录 → 返回 {id}
     副作用：landing 区的副本被删除
                                                │
 ④  GET /v2/files/{id}/downloadURL              ▼
     ─────────────────────────────────▶ 返回 persistent 区的签名 URL
```

**关键约束（实测自文档与 `ErrorMessages` / 校验器）：**

- `expiryTime` 参数的正则：`^[0-9]+M$`、`^[0-9]+H$`、`^[0-9]+D$`（分/时/天）。不匹配 → 400，错误消息固定为
  `expiryTime pattern isn't supported. Value should be one of these regex patterns ^[0-9]+M$ , ^[0-9]+H$ , ^[0-9]+D$`
- 未提供 `expiryTime` 时默认 **1 小时**；上限被 cap 到 **7 天**（各云厂商实现可能不同）。
- `FileID` 必须匹配 `^[\w,\s-]+(\.\w+)?$`（旧 `getLocation` 接口约束），且长度有上限（验收测试中有"file id length exceeding limit"用例）。
- 上传到 landing 区后若 **24 小时内**未登记元数据，文件会被自动删除（文档口径）。
  ★ **注意**：该"24 小时"是**部署侧的 bucket/container 生命周期规则**，
  本仓库 `devops/` 中**找不到**对应的 lifecycle 配置。**UNVERIFIED** 各云如何实现。
- 元数据请求缺少 `FileSource` → 错误 `FileSource can not be empty`。
- 指定的 `FileSource` 不存在 → 错误 `Invalid source file path to copy from <path>`。

### 2.1 `POST /v2/files/metadata` 的精确执行序列（实测 `FileMetadataService#saveMetadata`）

这是本项目 `CreateFileMetadata` 用例的**行为基准**，逐步列出：

| 步 | 动作 | 失败语义 |
| --- | --- | --- |
| 1 | `fileStatusPublisher.publishInProgressStatus()` —— 发布 `status` / `DATASET_SYNC` / `IN_PROGRESS` 事件 | 非致命 |
| 2 | `validateKind(kind)`：kind 按 `:` 切成 **4 段**，要求 `[1] == "wks"`、`[2] == "dataset--File.Generic"` | `400 Invalid kind` / `Invalid source in kind` / `Invalid entity in kind` |
| 3 | 取 `filePath = data.DatasetProperties.FileSourceInfo.FileSource` | 空 → `400 FileSource can not be empty` |
| 4 | 生成记录 id：`"<partition>:dataset--File.Generic:<uuid-去掉横线>"` | — |
| 5 | `stagingLocation = storageUtilService.getStagingLocation(filePath, partitionId)`；`persistentLocation = ...getPersistentLocation(...)` | — |
| 6 | `cloudStorageOperation.copyFile(stagingLocation, persistentLocation)` —— **服务端复制**（Azure `BlockBlobClient.beginCopy`；GCP `obmDriver.copyBlob`；S3 copy） | `StorageException` |
| 7 | `checksum = storageUtilService.getChecksum(persistentLocation)`，非空则**回写覆盖** `FileSourceInfo.Checksum` + `ChecksumAlgorithm`（Azure 用 MD5） | — |
| 8 | `fileMetadataRecordMapper.fileMetadataToRecord(fileMetadata)`（id/acl/legal/kind/ancestry/data-as-map/meta/tags） | — |
| 9 | `dataLakeStorage.upsertRecord(record)` → **Storage Service** 的 `PUT {storage.api}/records` | `StorageException` |
| 10 | `publishSuccessStatus(recordId, version)` + `publishDatasetDetails(recordId, version)`（**两个**事件；`datasetDetails` 的 `kind = "datasetDetails"`、`properties = {correlationId, datasetId, datasetType: FILE, datasetVersionId, recordCount: 1, timestamp}`，见 `status/FileDatasetDetailsPublisher.java`） | 非致命 |
| 11 | `cleanupStagingLocation(...)`：重新读取记录确认存在后删除 staging 对象；**删除失败被捕获并忽略**（上游 issue #76），**不得导致请求失败** | 忽略 |
| 12 | 出错时（`StorageException` 或一般异常）：`deleteFile(persistentLocation)` **回滚已复制的副本** + `publishFailureStatus`，然后重新抛出 | — |

### 2.2 `DELETE /v2/files/{id}/metadata` 的执行序列

```
getMetadataById(id)                                   —— 不存在 → 404 "Record Not Found"
  → dataLakeStorage.deleteRecord(id)                  —— POST {storage.api}/records/{id}:delete，
                                                         必须返回 204
  → cloudStorageOperation.deleteFile(persistentLocation)  —— 删除 persistent 区文件
```

### 2.3 关键行为要点（对本项目实现的硬约束）

| 要点 | 内容 |
| --- | --- |
| 同步执行 | 整个"复制 + 校验和 + 写记录 + 删 staging"在**同一个请求内同步完成**（不是异步任务） |
| kind 校验 | 是**硬编码字符串检查**（`wks` + `dataset--File.Generic`），**不调用 Schema Service** |
| Legal 校验 | File Service **不直接调用 Legal Service**；legal tag 的合规性由 **Storage Service 的 `PUT /records`** 内部校验 |
| ACL 校验 | 本地做**结构**校验（非空 + 邮箱式组名正则），组成员是否存在由 Entitlements/Storage 负责 |
| 校验和 | **服务端覆写**客户端传入的 `Checksum`/`ChecksumAlgorithm`（至少 Azure 实现如此） |
| staging 清理 | 失败**静默忽略**（有意的：不能因为清理失败而让成功的元数据登记变失败）；我们额外落一条审计告警（`createMetadataStagingCleanupFailure`），否则"staging 里堆孤儿"会无人察觉 |
| 事件 | 第 10 步发**两个**：`status`（`status-changed`）+ `datasetDetails`；**两者失败都只告警**（上游 `log.warning("Failed to publish dataset details")`），不得影响 `201` |

---

## 3. 源码结构（实测）

```
file-master-d7c25c2d.../
├── file-core/                       # 与云厂商无关的核心（Spring Boot 主实现）
│   └── src/main/java/org/opengroup/osdu/file/
│       ├── api/                     # ← REST 端点定义（权威契约，共 10 个类 / 902 行）
│       ├── config/                  # Web/校验/共享自动配置
│       ├── constant/                # 角色、错误消息、校验和算法、扩展名
│       ├── di/                      # DatalakeStorageClientFactory / EntitlementsClientFactory
│       ├── errors/                  # 错误模型（ErrorResponse / ErrorDetails / 四个具体错误）
│       ├── exception/               # 领域异常 + RestExceptionHandler
│       ├── logging/                 # AuditLogger / AuditOperation（审计事件）
│       ├── mapper/                  # FileMetadataRecordMapper
│       ├── middleware/              # AuthorizationFilter / ResponseHeaderFilter
│       ├── model/                   # 请求/响应模型（见 §5）
│       ├── provider/interfaces/     # ★ 存储抽象接口（14 个接口，见 §6）
│       ├── service/                 # 应用服务（FileMetadataService / storage / ...）
│       └── validation/              # 校验器与校验序列
├── file-core-plus/                  # GCP（"plus"）实现，含 docs/baremetal/ 部署说明
├── provider/
│   ├── file-azure/  file-gc/  file-ibm/
├── testing/
│   ├── file-test-core/  file-test-core-bdd/  file-test-azure/  file-test-baremetal/  file-test-gc/
├── file-acceptance-test/            # ★ Cucumber 验收测试（黄金样例载荷的重要来源）
└── docs/api/community/v2/openapi.yaml   # ★ 官方 OpenAPI（覆盖端点子集）
```

**架构风格（实测）**：典型 Spring Boot 分层 + **provider 接口/实现分离**：
`file-core` 定义接口与业务，`provider/file-*` 提供云厂商实现，`file-core-plus` 是 GCP 的特殊组合。
这正是"分层 + 依赖倒置"的一个成熟范例，也是本项目分层设计的直接参考。

**`*.proto` 检索结果：0 个匹配。**（实测：`find <repo> -iname "*.proto"` 无输出。）

---

## 4. HTTP API 完整清单（实测）

来源：`file-core/src/main/java/org/opengroup/osdu/file/api/*.java` 的注解，
`file-acceptance-test/src/test/resources/features/*.feature` 交叉验证，
并对 **tag v0.4.0 → v0.30.2** 以及 `v0.28.0-aws.1` 做过对照。

> ★ **所有端点都在部署 context path `/api/file/` 之下**
> （每个 provider 的 `application.properties` 中均为 `server.servlet.contextPath=/api/file/`）。
> 下表路径**已包含**该前缀。本项目的 base path 必须可配置，默认同为 `/api/file`。

图例：`@Hidden` = 注解 `io.swagger.v3.oas.annotations.Hidden`，
即**不进入生成的 OpenAPI 文档**（属内部/DMS 端点），但**仍然可用且被验收测试覆盖**。

### 4.0 重要的事实澄清（容易踩坑）

| # | 事实 | 说明 |
| --- | --- | --- |
| F1 | **当前 master 没有 `/v1` API** | 全部为 `/v2/...`。部分 `/v1` 端点只存在于 **≤ v0.5.0**（约 2021 年）的 tag 中，`v0.7.0` 起已全部迁到 `v2` |
| F2 | **不存在任何 `PUT` 映射** | 全仓库无 `@PutMapping`；`PUT /v2/files/{id}/metadata` **不存在** |
| F3 | **不存在 `/v2/files/{id}/versions`** | File Service 无版本端点（版本由 Storage Service 的 `/records/{id}/{version}` 承担） |
| F4 | **不存在 `/health`** | 只有 `/v2/liveness_check` 与 `/v2/readiness_check` |
| F5 | **唯一 query 参数是 `expiryTime`** | 无其他 query 参数 |
| F6 | **没有 `artifact` 字段** | `dataset--File.Generic` 任何已提交快照（含最早的 2021 年 commit、2020 年的 `File.1.0.0.json`、以及已删除的 legacy OpenAPI）中都不存在 `artifact`。`fileType` 位于 `data.ExtensionProperties.FileContentsDetails.FileType` |
| F7 | **`fileSource` / `fileType` 的小驼峰写法不是 OSDU 名称** | OSDU 用 PascalCase：`FileSource` / `FileType` |
| F8 | **`Driver` 在通用实现里被硬编码为 `GCS`** | `DriverType` 枚举唯一取值为 `GCS`，`LocationServiceImpl` 对所有云厂商硬编码 `.driver(DriverType.GCS)` —— Azure/AWS/IBM 部署下 `getFileLocation` 也返回 `"GCS"` |

---

## 4.1 文件位置 / 上传 / 下载

| 方法 | 完整路径 | 角色要求 | 请求 | 成功响应 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `GET` | `/api/file/v2/files/uploadURL` | `service.file.editors` | query `expiryTime`（可选） | `200 LocationResponse` | **推荐**的上传位置接口 |
| `POST` | `/api/file/v2/getLocation` | `service.file.editors` | `LocationRequest` = `{"FileID": "..."}` | `200 LocationResponse` | **`@Hidden`**，文档标记将废弃 |
| `POST` | `/api/file/v2/getFileLocation` | `service.file.editors` | `FileLocationRequest` = `{"FileID": "..."}` | `200 FileLocationResponse` | **`@Hidden`**，文档标记将废弃 |
| `GET` | `/api/file/v2/files/{id}/downloadURL` | `service.file.viewers` | path `id`，query `expiryTime` | `200 DownloadUrlResponse` | body 为 `{"SignedUrl": "..."}` |
| `POST` | `/api/file/v2/getFileList` | `service.file.editors` | `FileListRequest` | `200 FileListResponse` | **`@Hidden`** |

## 4.2 文件元数据

| 方法 | 完整路径 | 角色要求 | 请求 | 成功响应 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `POST` | `/api/file/v2/files/metadata` | `service.file.editors` | `FileMetadata` | `201 FileMetadataResponse` = `{"id": "..."}` | 触发 staging→persistent 复制 |
| `GET` | `/api/file/v2/files/{id}/metadata` | **`service.file.viewers`** | path `id` | `200 RecordVersion` | 含 `version` |
| `DELETE` | `/api/file/v2/files/{id}/metadata` | `service.file.editors` **或** `service.file.admin` | path `id` | `204`（无体） | 同时删除 persistent 区文件 |

> ★ **文档/代码分歧（须以代码为准）**：
> 官方文档把 `GET .../metadata` 写成需要 `service.file.editors`，
> 但**代码是 `service.file.viewers`**（`FileMetadataApi.java:77`）。本项目的契约测试以代码为准。

## 4.3 DMS（Dataset Management Service）接口

`FileDmsApi`（前缀 `/api/file/v2/files`）与 `FileCollectionDmsApi`（前缀 `/api/file/v2/file-collections`）**两组端点镜像**。
两个类都标注了 `@Hidden`。

| 方法 | 完整路径 | 角色要求 | 请求 | 成功响应 |
| --- | --- | --- | --- | --- |
| `POST` | `/api/file/v2/files/storageInstructions` | **`service.dataset.editors`** | query `expiryTime` | `200 StorageInstructionsResponse` |
| `POST` | `/api/file/v2/files/retrievalInstructions` | **`service.dataset.viewers`** | body + query `expiryTime` | `200 RetrievalInstructionsResponse` |
| `POST` | `/api/file/v2/files/copy` | `service.storage.creator` / `service.storage.admin` | `{"datasetSources": [...]}` | `200 [CopyDmsResponse]` |
| `POST` | `/api/file/v2/file-collections/storageInstructions` | `service.dataset.editors` | 同上 | 同上 |
| `POST` | `/api/file/v2/file-collections/retrievalInstructions` | `service.dataset.viewers` | 同上 | 同上 |
| `POST` | `/api/file/v2/file-collections/copy` | `service.storage.creator` / `service.storage.admin` | 同上 | 同上 |

> ★ **文档/代码分歧**：IBM 公开文档称 storage/retrievalInstructions "无需权限"，
> 但**代码要求** `service.dataset.editors` / `service.dataset.viewers`（`DatasetConstants`）。以代码为准。

## 4.4 交付（Delivery）接口

| 方法 | 完整路径 | 角色要求 | 请求 | 响应 |
| --- | --- | --- | --- | --- |
| `POST` | `/api/file/v2/delivery/GetFileSignedUrl` | **`service.delivery.viewer`** | `{"srns": ["..."]}` | `UrlSigningResponse` |

该接口以 **SRN**（Subsurface Resource Name）而非 fileID 为输入，是面向下游消费方的批量取址接口。

## 4.5 管理 / 运维 / 文档

| 方法 | 完整路径 | 角色要求 | 响应 |
| --- | --- | --- | --- |
| `POST` | `/api/file/v2/files/revokeURL` | `service.file.admin` | **`204`（无体）** |
| `GET` | `/api/file/v2/info` | **无需鉴权**（`@RequestMapping` 无安全注解；各云 authz policy 显式放行） | `VersionInfo`（`groupId`/`artifactId`/`version`/`buildTime`/`branch`/`commitId`/`commitMessage`/`connectedOuterServices`） |
| `GET` | `/api/file/v2/liveness_check` | `@PermitAll` | `200` **纯文本** `File service is alive` |
| `GET` | `/api/file/v2/readiness_check` | `@PermitAll` | `200` **纯文本** `File service is ready` |
| `GET` | `/api/file/v2/api-docs`、`/api/file/v2/api-docs.yaml`、`/api/file/v2/api-docs/swagger-config` | — | springdoc 运行时生成的 OpenAPI |
| `GET` | `/api/file/v2/swagger` → Swagger UI | — | HTML |

> `revokeURL` 的请求体是自由 `Map<String,String>`；**Azure（唯一实现者）要求**
> `{"resourceGroup": "<rg>", "storageAccount": "<account>"}`，任一为空则
> `400 Illegal argument for resourceGroup { x } or storageAccount { y }`。
> 该端点**不要求** `data-partition-id`。

> GCP（`file-core-plus`）额外在**独立管理端口**（`management.server.port` 默认 `8081`，
> base-path `/`，仅暴露 `health`）提供 Spring Boot actuator 健康端点。

## 4.6 `expiryTime` 的精确语义（实测 `ExpiryTimeUtil`）

唯一 query 参数，存在于 `uploadURL`(GET)、`downloadURL`(GET)、
`files/storageInstructions`、`files/retrievalInstructions`、
`file-collections/storageInstructions`、`file-collections/retrievalInstructions`。

| 方面 | 行为 |
| --- | --- |
| 接受格式 | `^[0-9]+M$` / `^[0-9]+H$` / `^[0-9]+D$`（分 / 时 / 天） |
| **缺省值** | **1 小时**（`DEFAULT_TTL = 1L, TimeUnit.HOURS`） |
| **上限** | **7 天**（`CAPPED_DEFAULT_TTL = 7L, TimeUnit.DAYS`），**超过时静默截断**（不报错） |
| 非法格式 | `400`，消息固定为：`expiryTime pattern isn't supported. Value should be one of these regex patterns ^[0-9]+M$ , ^[0-9]+H$ , ^[0-9]+D$` |

> ★ **文档/代码分歧**：`docs/docs/File-Service.md` 称下载 URL 缺省为 **7 天**；
> **代码是 1 小时，上限 7 天**。以代码为准。本项目的契约测试固化"缺省 1H、超限截断到 7D"。

## 4.7 请求 / 响应体（实测，含精确 JSON）

**`LocationRequest`**
```json
{ "FileID": "optional-existing-file-id" }
```

**`LocationResponse`** —— 注意 `Location` 是 `Map<String,String>`，`FileSource` 在 `Location` **内部**：
```json
{ "FileID": "da92f52401dc4d1cb93515f159c110d4",
  "Location": { "SignedURL": "https://...&sig=...",
                "FileSource": "/osdu-user/1614784413120-2021-03-03-15-13-33-120/da92f52401dc4d1cb93515f159c110d4" } }
```
键名 `SignedURL` / `FileSource` 在三个云厂商的 mapper 中都被定义为常量
（`AzureLocationMapper`、GCP `LocationMapper`、IBM `IBMLocationMapper`）。

**`FileLocationResponse`**（`getFileLocation`）：
```json
{ "Driver": "GCS", "Location": "https://<account>/<container>/<path>" }
```
`FileLocationResponse.driver` 在 core-common 中类型为 `Object`；序列化值来自 `DriverType`。

**`FileListRequest`** / **`FileListResponse`**（**Spring Page 字段名，不是 results/total**）：
```json
{ "TimeFrom": "2021-03-01T00:00:00", "TimeTo": "2021-03-31T00:00:00",
  "PageNum": 0, "Items": 10, "UserID": "user@x.com" }
```
```json
{ "Content": [ { "FileID": "...", "Driver": "GCS", "Location": "...",
                 "CreatedAt": "2021-03-03T15:13:33.120+0000", "CreatedBy": "osdu-user" } ],
  "Number": 0, "NumberOfElements": 1, "Size": 10 }
```
`FileLocation` 的字段恰为 `FileID` / `Driver` / `Location` / `CreatedAt`（格式 `yyyy-MM-dd'T'HH:mm:ss.SSSZ`）/ `CreatedBy`。

**`DownloadUrlResponse`**：`{ "SignedUrl": "https://..." }`

**`FileMetadataResponse`**：`{ "id": "opendes:dataset--File.Generic:0e1d0e29..." }`

**DMS bodies**（core-common `org.opengroup.osdu.core.common.dms.model`）：
```java
class StorageInstructionsResponse   { String providerKey; Map<String,Object> storageLocation; }
class RetrievalInstructionsResponse { List<DatasetRetrievalProperties> datasets; }
class DatasetRetrievalProperties    { String datasetRegistryId; Map<String,Object> retrievalProperties; String providerKey; }
class CopyDmsRequest                { List<Record> datasetSources; }
class CopyDmsResponse               { boolean success; String datasetBlobStoragePath; }
```
File DMS 路径下 Azure 返回 `providerKey = "AZURE"`、
`storageLocation = { signedUrl, fileSource, createdBy, expiryTime }`；
AWS 返回 `FileDmsStorageLocation{ unsignedUrl, signedUrl, fileSource, createdAt, connectionString, credentials, createdBy, signedUploadFileName, region }`。

**上传路径布局**（Azure `StorageServiceImpl.getFileLocationPrefix`）：
```
<file.location.userId>/<epochMillis>-<yyyy-MM-dd-HH-mm-ss-SSS>/<fileID>
示例：osdu-user/1614784413120-2021-03-03-15-13-33-120/da92f52401dc4d1cb93515f159c110d4
```
返回的 `FileSource` 是 `"/" + filepath`（**带前导斜杠**）。Azure 路径长度上限 **1024** 字符
（`StorageConstant.AZURE_MAX_FILEPATH`），在 `StorageServiceImpl.createSignedUrl` 中强制。

---

## 4.8 错误码与错误体

各端点的 `@ApiResponses` 声明同一矩阵：
`400`（输入/校验）、`401`（未认证/缺租户）、`403`（无权限）、`404`（记录不存在）、`500`、`502`、`503`，
成功为 `200` / `201` / `204`。

**没有 `409`**：已存在的 FileID 返回 **400**（`LocationAlreadyExistsException` 被
`handleBadRequest` 分支处理）。

**存在三种错误体形态（实测 `RestExceptionHandler`）**：

| 形态 | JSON | 触发场景 |
| --- | --- | --- |
| `ErrorResponse`（遗留） | `{"error": {"code": 400, "message": "...", "errors": [...]}}` （`@JsonTypeInfo(WRAPPER_OBJECT)` + `@JsonTypeName("error")`） | `StorageException`、`ApplicationException`、`OsduBadRequestException`、`NotFoundException`、`OsduUnauthorizedException`、方法参数校验失败 |
| `ApiError` | `{"status": "BAD_REQUEST", "message": "...", "errors": ["..."]}` | `ConstraintViolationException`（`LocationRequest`/`FileLocationRequest`/`FileListRequest` 上的 bean validation）、`JsonParseException`、`IllegalArgumentException`、`MismatchedInputException`、`HttpMessageNotReadableException` |
| `AppError`（**当前 OSDU 标准**） | `{"code": 400, "reason": "Bad Request", "message": "..."}` | provider 代码抛出的 `AppException`（如 AWS "Malformed URL"）；也是所有 `@ApiResponse` 注解声明的 schema |

实测的具体错误消息：
`"Invalid source file path to copy from <path>"`、`"FileSource can not be empty"`、
`"Record Not Found"`、`"Not found location for fileID : <id>"`、
`"Location for fileID = <id> already exists"`、`"Invalid kind"`、`"Invalid source in kind"`、
`"Invalid entity in kind"`、`"Missing authorization token"`、`"Missing partitionID"`。

验收测试还断言：`"ConstraintViolationException: Invalid FileLocationRequest"`、
`"Not found location for fileID : test"`，以及缺 `kind`/`acl`/`legal`/`data`/`FileSource`、
非法 `Endian` 的逐字段消息。

> **本项目决策**：对外主用 **`AppError`**（它是官方 OpenAPI 声明的 schema），
> 并提供 `http.error_format = apperror | legacy | api_error` 兼容开关，
> 默认 `apperror`。三种形态的契约测试各测一遍。风险登记见 `R-04`。

---

## 4.9 Legacy v1 接口（仅存在于 tag ≤ v0.5.0）

| 方法 | 路径（含 context path） |
| --- | --- |
| `POST` | `/api/file/v1/files/metadata` |
| `GET` | `/api/file/v1/files/{id}/metadata` |
| `GET` | `/api/file/v1/files/uploadURL` |
| `GET` | `/api/file/v1/files/{id}/downloadURL` |
| `POST` | `/api/file/getLocation`（**无版本段**） |
| `POST` | `/api/file/getFileLocation` |
| `POST` | `/api/file/getFileList` |
| `POST` | `/api/file/getFile` |

`v0.4.0` 尚无 metadata 路径；`v0.7.0` 起已全部迁至 `/v2/...`。
本项目**只实现 v2**（v1 已废弃 5 年，且 `getFile` 依赖已删除的 `FileApi`）。


---

## 5. 元数据模型

### 5.1 记录信封（实测自 `model/filemetadata/`）

`FileMetadata`：

| 字段 | 类型 | JSON 名 | 必需 |
| --- | --- | --- | --- |
| `id` | String | `id` | 否（服务端生成） |
| `kind` | String | `kind` | 是 |
| `acl` | `Acl` | `acl` | 是 |
| `legal` | `Legal` | `legal` | 是 |
| `data` | `FileData` | `data` | 是 |
| `ancestry` | `Ancestry` | `ancestry` | 否 |
| `meta` | `List<Map<String,Object>>` | `meta` | 否 |
| `tags` | `Map<String,String>` | `tags` | 否 |

`RecordVersion`（`GET .../metadata` 的响应）= 上面全部字段 **+ `version` : `Long`**。

`FileMetadataResponse`（`POST .../metadata` 的响应）= `{"id": String}`。

### 5.2 `data`（FileData）—— 注意 **PascalCase**

实测自 `model/filemetadata/filedetails/FileData.java`：每个字段都用 `@JsonProperty("PascalCase")` 显式命名。

| C++/Java 字段 | JSON 名 | 说明 |
| --- | --- | --- |
| `name` | `Name` | 文件名/数据集名 |
| `description` | `Description` | 描述 |
| `totalSize` | `TotalSize` | 总字节数（**字符串**） |
| `encodingFormatTypeID` | `EncodingFormatTypeID` | MIME 类型引用（`namespace:reference-data--EncodingFormatType:...`） |
| `schemaFormatTypeID` | `SchemaFormatTypeID` | schema 类型引用 |
| `resourceHomeRegionID` | `ResourceHomeRegionID` | 主区域 |
| `resourceHostRegionIDs` | `ResourceHostRegionIDs` | 宿主区域数组 |
| `resourceCurationStatus` | `ResourceCurationStatus` | 治理状态 |
| `resourceLifecycleStatus` | `ResourceLifecycleStatus` | 生命周期状态 |
| `resourceSecurityClassification` | `ResourceSecurityClassification` | 密级 |
| `source` | `Source` | 数据来源 |
| `datasetProperties` | `DatasetProperties` | 见下 |
| `existenceKind` | `ExistenceKind` | 存在类型 |
| `endian` | `Endian` | 枚举 `BIG` / `LITTLE`（`enum Endian`） |
| `checksum` | `Checksum` | 校验和 |
| `extensionProperties` | `ExtensionProperties` | **开放字段**：`Map<String,Object>`，承载 `FileContentsDetails` 等 |

`DatasetProperties.FileSourceInfo`（同样 PascalCase）：

| JSON 名 | 说明 |
| --- | --- |
| `FileSource` | ★ 关联已上传文件的关键字段。缺失 → 400 `FileSource can not be empty` |
| `PreloadFilePath` | 原始来源（如 `s3://...`） |
| `PreloadFileCreateUser` / `PreloadFileCreateDate` | 预载创建人 / 时间 |
| `PreloadFileModifyUser` / `PreloadFileModifyDate` | 预载修改人 / 时间 |
| `Name` | 文件名 |
| `FileSize` | 文件大小（字符串） |
| `EncodingFormatTypeID` | MIME 类型引用 |
| `Checksum` | 校验和 |
| `ChecksumAlgorithm` | 算法（如 `SHA-256`） |

`extensionProperties.FileContentsDetails`（实测自 `FileContentsDetails.java`）：
`kind`、`targetKind`、`fileType`、`frameOfReference`（`MetaItem` 数组）、`extensionProperties`、`parentReference`。

### 5.3 权威黄金样例（实测，直接引自验收测试资源）

`file-acceptance-test/src/test/resources/input_payloads/File_CorrectPayload.json` 是 OSDU 官方用于
**正向**验收的请求体。本项目的契约测试直接以它为基准（见 `tests/integration/test_phase0_toolchain.cpp`
中的 `kOsduFileMetadataJson`，仅把 `<placeholder>` 替换为具体值）：

```json
{
  "id": "string",
  "kind": "<tenant_name>:wks:dataset--File.Generic:1.0.0",
  "meta": [ { "name": { "id": [ { "kind": "12587932" } ] } } ],
  "tags": { "dataflowId": "dataflowId", "dataflowId-2": "dataflowId" },
  "acl": {
    "viewers": ["<acl_viewers>@<tenant_name>.<cloud_domain>"],
    "owners":  ["<acl_owners>@<tenant_name>.<cloud_domain>"]
  },
  "legal": {
    "legaltags": ["<legal_tags>"],
    "otherRelevantDataCountries": ["US"],
    "status": "compliant"
  },
  "data": {
    "ResourceHomeRegionID": "namespace:reference-data--OSDURegion:AWSEastUSA:",
    "ResourceHostRegionIDs": ["namespace:reference-data--OSDURegion:AWSEastUSA:"],
    "ResourceCurationStatus": "namespace:reference-data--ResourceCurationStatus:CREATED:",
    "ResourceLifecycleStatus": "namespace:reference-data--ResourceLifecycleStatus:LOADING:",
    "ResourceSecurityClassification": "namespace:reference-data--ResourceSecurityClassification:RESTRICTED:",
    "Source": "Example Data Source",
    "ExistenceKind": "namespace:reference-data--ExistenceKind:Prototype:",
    "Name": "Dataset X221/15",
    "Description": "As originally delivered by ACME.com.",
    "TotalSize": "13245217273",
    "EncodingFormatTypeID": "namespace:reference-data--EncodingFormatType:text%2Fcsv:",
    "SchemaFormatTypeID": "namespace:reference-data--SchemaFormatType:CWLS%20LAS3:",
    "Endian": "BIG",
    "DatasetProperties": {
      "FileSourceInfo": {
        "FileSource": "/osdu-user/1624011206350-2021-06-18-10-13-26-350/33a71a04d20f4240904b4b3fca4657b7",
        "PreloadFilePath": "s3://staging-area/r7/raw-data/provided/documents/1000.witsml",
        "PreloadFileCreateUser": "somebody@acme.org",
        "PreloadFileCreateDate": "2019-12-16T11:46:20.163Z",
        "PreloadFileModifyUser": "somebody.else@acme.org",
        "PreloadFileModifyDate": "2019-12-20T17:20:05.356Z",
        "Name": "1000.witsml",
        "FileSize": "95463",
        "EncodingFormatTypeID": "namespace:reference-data--EncodingFormatType:application%2Fgeo%2Bjson:",
        "Checksum": "d41d8cd98f00b204e9800998ecf8427e",
        "ChecksumAlgorithm": "SHA-256"
      }
    },
    "Checksum": "d41d8cd98f00b204e9800998ecf8427e",
    "ExtensionProperties": {}
  }
}
```

验收测试中还有一组**负向**样例（同目录），是错误语义的直接依据：

| 样例文件 | 期望 |
| --- | --- |
| `File_missing_kind.json` | 400（缺 `kind`） |
| `File_missing_acl.json` / `File_missing_owners.json` / `File_missing_viewers.json` | 400（ACL 结构不完整） |
| `File_missing_legal.json` | 400（缺 `legal`） |
| `File_missing_data.json` | 400（缺 `data`） |
| `File_missing_fileSource.json` / `File_empty_fileSource.json` | 400（`FileSource` 缺失或为空） |
| `File_invalid_fileSource.json` | 400（`FileSource` 指向的文件不存在） |
| `File_invalid_Endian.json` | 400（`Endian` 非枚举值） |
| `File_invalid_ScalarIndicator.json` | 400（⚠️ **上游 feature 表里被 `#` 注释，从未执行**） |
| `File_Datatype_Mismatch.json` | 400（⚠️ 同上，被注释） |
| `File_Calculate_Checksum.json` | 校验和计算场景 |

> **补充（P4 切片 5，一手复核）**：上表的"期望"已在归档源码
> `/home/ll/osdu-file-upstream`（commit `d7c25c2`）中逐条核对，并拿到**逐字**的期望消息
> （`testing/file-test-*/.../output_payloads/File_*_msg.json`）与场景表
> （`.../features/IntegrationTest_File_POST.feature`）。两点修正：
> ① `File_invalid_ScalarIndicator.json` 与 `File_Datatype_Mismatch.json` 在上游 Examples 表里
> 被 `#` 注释掉（从未执行）；② `File_missing_data.json` 的 `data` 是**空对象**而非缺失，
> 期望消息为 `data cannot be empty`。权威表见 `docs/03-api-contract.md` §3.4。

### 5.4 响应模型

精确的 JSON 形态、字段名与嵌套结构见 **§4.7**（那里是逐字段实测的结果，是契约测试的唯一依据）。
此处仅补充"内部模型 vs 线上契约"的区别：

| 内部类（`model/`） | 线上契约类（core-common） | 说明 |
| --- | --- | --- |
| `LocationResponse`（core-lib） | `{"FileID": "...", "Location": {"SignedURL": "...", "FileSource": "/..."}}` | `Location` 是 `Map<String,String>` |
| `DownloadUrlResponse` | `{"SignedUrl": "..."}` | 本仓库内的类，字段名 `SignedUrl` |
| `SignedUrl`（内部） | — | `{uri, url, fileSource, createdBy, createdAt, connectionString}`，**不出现在 HTTP 契约中** |
| `SignedObject`（内部） | — | `{url, uri}` |
| `SignedUrlParameters`（内部入参） | — | `{expiryTime, fileName, contentType}` |
| `FileListResponse`（core-lib） | Spring Page：`{Content, Number, NumberOfElements, Size}` | ★ 不是 `results/totalCount` |
| `FileLocationResponse`（core-lib） | `{"Driver": "GCS", "Location": "https://..."}` | `driver` 在 core-common 中类型是 `Object` |
| `delivery.SignedUrl` | — | `{uri, url, createdAt, connectionString}` |
| `UrlSigningResponse` | `{processed: Map<String,SrnFileData>, unprocessed: List<String>}` | `SrnFileData = {signedUrl, unsignedUrl, kind, connectionString}` |

**教训**：内部模型的名字具有误导性（例如内部 `SignedUrl` 与交付接口的 `SignedUrl` 是两个不同的类）。
契约测试必须直接断言**线上 JSON 字段名**，而不是依赖内部类名。

### 5.5 ★ 元数据持久化：File Service **自己不存元数据**

这一点对本项目的设计影响最大，必须明确：

| 数据 | 存放位置 | 说明 |
| --- | --- | --- |
| **文件元数据记录**（`File.Generic`） | **Storage Service**（`PUT/GET {storage.api}/records`） | File Service 通过 core-common 的 `DataLakeStorageService` 转发，**自己没有任何元数据表** |
| **位置记录**（`FileLocation`） | File Service **自己的** KV 存储 | Azure Cosmos DB（容器 `file-locations`）、IBM Cloudant、GCP OSM/Datastore、AWS DynamoDB |
| 索引 / 搜索 | **Storage Service** 负责（Elasticsearch） | File Service **从不写 Elasticsearch** |

实测检索结论：**本仓库中不存在 Elasticsearch 与 Postgres 的任何引用**（`grep` 干净）。
`/v2/info` 文档示例里出现的 `elasticSearch` / `postgresSql` 是从 Storage 服务复制过来的，
**不代表 File Service 的真实依赖**。

**本项目的设计取舍（有意偏离，需在文档中明确）**：

| 方案 | 取舍 |
| --- | --- |
| 完全复刻上游（元数据只存远端 Storage Service） | 服务**无法独立运行**，也无法独立测试——与本项目"每步可测"的要求冲突 |
| **双实现：默认内置 SQLite 本地元数据仓储 + 可选 `RemoteStorageServiceRepository`** | 可以独立部署与测试；同时保留在完整 OSDU 平台中对接 Storage Service 的能力 |

即：本项目**引入了一个上游没有的本地元数据存储**（`SqliteMetadataRepository`），
这是**能力增强**而非规范实现，必须在文档与配置中显式说明（见 `docs/02-design.md` §9.2）。
`RemoteStorageServiceRepository` 的存在保证"接口兼容"这一目标不受影响。

---

---

## 6. ★ 存储抽象：参考实现的 provider 接口（本项目直接借鉴）

实测路径：`file-core/src/main/java/org/opengroup/osdu/file/provider/interfaces/`（14 个接口，710 行）。

| 接口 | 关键方法 | 语义 |
| --- | --- | --- |
| **`IStorageService`** | `createSignedUrl(fileID, token, partitionID[, params])` → `SignedUrl`<br>`createStorageInstructions(datasetId, partitionID[, params])` → `StorageInstructionsResponse`<br>`createRetrievalInstructions(List<FileRetrievalData>[, params])` → `RetrievalInstructionsResponse`<br>`createSignedUrlFileLocation(unsignedUrl, token, params)` → `SignedUrl`<br>`revokeUrl(Map<String,String>)` → `Boolean` | ★ **面向"业务语义"的门面**：上传地址、下载地址、DMS 指令、吊销。多个方法有 `default` 空实现，说明不同云厂商能力不同 |
| **`IStorageRepository`** | `createSignedObject(bucketName, filepath[, params])` → `SignedObject`<br>`getSignedObject(bucketName, filepath[, params])` → `SignedObject`<br>`revokeUserDelegationKeys(Map)` | ★ **面向"物理桶/路径"的原语**：在指定 bucket+path 上创建对象并签名 |
| **`ICloudStorageOperation`** | `copyFile(src, dst)`<br>`copyFiles(List<FileCopyOperation>)` → `List<FileCopyOperationResponse>`<br>`deleteFile(filePath)`<br>`copyDirectories(List<FileCopyOperation>)` → `List<DatasetCopyOperation>` | 云存储侧的复制/删除（staging→persistent 搬迁） |
| `ILocationService` | `getLocation(request, headers[, params])`、`getFileLocation(request, headers)` | 位置业务门面 |
| `IFileLocationRepository` | `findByFileID(fileID)` → `FileLocation`<br>`save(FileLocation)` → `FileLocation`<br>`findAll(FileListRequest)` → `FileListResponse` | ★ **位置记录的持久化**（键值/表存储） |
| `IStorageUtilService` / `IFileCollectionStorageUtilService` | 路径/URL 工具 | 路径构造 |
| `IAuthenticationService` | token 获取 | 服务间认证 |
| `IValidationService` | 校验 | 校验门面 |
| `IFileListService` | 列表 | 列表业务 |
| `ILocationMapper` | 映射 | `Location` ↔ 内部模型 |
| `IFileCollectionStorageService` | 集合版存储服务 | 面向"文件集合" |
| `IDeliveryStorageService` / `IDeliveryUnsignedUrlLocationMapper` | 交付签名 | 面向 SRN 的交付 |

**架构结论（本项目直接采纳）**：

参考实现把存储抽象**分成两层**——"业务语义门面"（`IStorageService`）与"物理原语"（`IStorageRepository` + `ICloudStorageOperation`）。
本项目在自己的分层中做了**等价的划分**，但更彻底：

| OSDU 参考实现 | 本项目对应 |
| --- | --- |
| `IStorageRepository` + `ICloudStorageOperation` | `domain::IBlobStore`（单一端口，含 `capabilities()`） |
| `IStorageService` | `app::LocationService` / `app::StorageInstructionService`（应用层用例，不再抽象成接口，因为它没有多实现需求） |
| `IFileLocationRepository` | `domain::IFileLocationRepository`（SQLite / 内存 实现） |
| `ILocationService` + `ILocationMapper` | 合并进 `app::LocationService` |

### 6.1 三个必须澄清的实测事实

| # | 事实 | 说明 |
| --- | --- | --- |
| C1 | **`BlobStore` 不在本仓库** | 它是 `os-core-lib-azure` 中的**具体类**（`org.opengroup.osdu.azure.blobstorage.BlobStore`），方法包括 `readFromStorageContainer`/`writeToStorageContainer`/`deleteFromStorageContainer`/`createBlobContainer`/`getSasToken`/`generatePreSignedURL`/`generatePreSignedUrlWithUserDelegationSas`/`copyFile`/`readBlobProperties`/`getBlobInputStream`。File Service 仓库里**没有** `IBlobStore`/`BlobStoreFactory`/`StorageAccount` |
| C2 | **驱动选择靠 Spring `@Primary`/`@Qualifier`，不是工厂** | 这是本项目要改进的点：C++ 里我们用显式的 `IBlobStoreFactory` 按 partition 解析，可测试、可推理 |
| C3 | **仓库中不存在任何本地文件系统驱动** | 实测：Azure（Blob + ADLS Gen2/Cosmos）、GCP（OBM/OSM）、IBM（COS/Cloudant）、AWS（S3/DynamoDB，仅存在于 `v*-aws.*` tag，**不在 master**）。**没有任何 FileSystem 实现** |

**各云实现的关键接口映射（实测）**：

| 云 | 物理原语 | 签名 | 位置 KV |
| --- | --- | --- | --- |
| Azure | 本地 `Storage` 接口 `{ Blob create(partition, BlobInfo, byte[]); URL signUrl(BlobInfo, long, TimeUnit) }` + `StorageImpl` / `StorageRepository` | SAS（含用户委派 SAS） | Cosmos DB，容器 `file-locations`（`FileLocationEntity`，MapStruct 映射） |
| GCP | `ObmStorageRepository`（`org.opengroup.osdu.core.obm.core.Driver`：`getSignedUrlWithParams` / `getBlob` / `copyBlob` / `copyBlobs` / `listBlobsByPrefix` / `deleteBlob`） | OBM 签名 URL | OSM（Datastore / Postgres） |
| IBM | `IBMStorageServiceImpl`（`com.ibm.cloud.objectstorage` AmazonS3 + `GeneratePresignedUrlRequest`） | S3 预签名 + 临时凭证 | Cloudant/CouchDB |
| AWS（仅 tag） | `StorageServiceImpl` + `S3Location` | S3 预签名 + STS 凭证 | DynamoDB |

**与参考实现的关键差异（本项目的改进点）**：参考实现没有"驱动能力声明"，因此 5 个 `IStorageService`
方法只能靠 `default` 空实现 + 各云厂商覆写来区分能力，**上层无法在编译期或运行期可靠地知道某个能力是否存在**，
只能"调用后拿到 null 再判断"。本项目用显式的 `BlobCapabilities` 消除这种隐式约定（见 ADR-003）。

---

## 7. "集中存储"在参考实现中的真实形态

实测：`file-core-plus/docs/baremetal/README.md` 是 OSDU 官方给出的 **baremetal（本地/私有化）部署**配置说明。

关键发现（**与直觉相反**）：

- baremetal 部署**并不使用本地文件系统作为文件存储**，而是使用 **S3 兼容对象存储（SeaweedFS）**：
  - `OBMDRIVER=s3`（Object Storage Manager driver）
  - 分区属性：`obm.s3.endpoint` / `obm.s3.accessKey` / `obm.s3.secretKey` / `obm.s3.region`
  - 所需权限：`ListObjects, CRUDObject, SignedURLs`
  - bucket 命名：`<projectId-PartitionInfo.name>-$GCP_STORAGE_STAGING_AREA` 与 `...-persistent-area`
- 元数据/位置存储用 **Postgres**：`OSMDRIVER=postgres`，表结构为
  ```sql
  CREATE TABLE osdu."file_locations_osm"(
      id text NOT NULL,
      pk bigint NOT NULL GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
      data jsonb NOT NULL,
      CONSTRAINT file_locations_osm_id UNIQUE (id)
  );
  CREATE INDEX file_locations_osm_datagin ON osdu."file_locations_osm" USING GIN (data);
  ```
  注意：**单表 + `data jsonb`**，即"一个 kind 一张表、内容全在 JSONB 里"的通用 KV 模型（`osm` = Object Storage Manager 的 KV 抽象）。
- 消息用 **RabbitMQ**：`OQMDRIVER=rabbitmq`，`status-changed` 交换机用于状态变更事件。
- 认证：`PARTITION_AUTH_ENABLED`、`SERVICE_TOKEN_PROVIDER`（`GCP` 或 `OPENID`，如 Keycloak）、
  `OPENID_PROVIDER_URL` + `.../.well-known/openid-configuration`。

**对本项目的启示**：

1. OSDU 的"两区（staging/persistent）"语义**必须**保留，且区地址是**分区级配置**而非全局配置。
2. "集中存储"在 OSDU 语境下没有官方定义。本项目将其定义为"服务自身作为数据面的 POSIX 存储"，
   是对平台的**能力增强**而非规范实现，因此必须与 OSDU 端点命名空间隔离（见 ADR-003）。
3. 参考实现的 KV 抽象（`id` + `data jsonb` 单表）值得借鉴：本项目的位置记录表也采用
   "`file_id` 主键 + `data` JSON 列 + 少量提取出的查询列"的结构，兼顾灵活性与可索引性。
4. 分区级配置（`obm.s3.*` 等）的存在说明：**多租户的存储后端配置必须可按分区覆盖**。
   本项目将 `IBlobStore` 的实例化做成"按分区解析"的工厂，而不是全局单例。

---

## 8. 鉴权、错误模型与标准请求头

### 8.1 鉴权（实测）

- **角色常量**（`constant/FileServiceRole.java`）：
  - `service.file.viewers`、`service.file.editors`、`service.file.admin`
- 鉴权方式：`@PreAuthorize("@authorizationFilter.hasPermission('<role>')")`，
  由 `middleware/AuthorizationFilter` 实现，内部调用 **Entitlements Service**。
- 角色分配（实测汇总）：

| 端点组 | 角色 |
| --- | --- |
| 上传位置（`uploadURL` / `getLocation`） | `service.file.editors` |
| `getFileLocation` / `getFileList` | `service.file.editors` |
| 创建 / 删除元数据 | `service.file.editors`（删除另允许 `service.file.admin`） |
| 读取元数据 | `service.file.viewers` |
| 下载 URL | `service.file.viewers` |
| `revokeURL` | `service.file.admin` |
| DMS `storageInstructions` | `service.storage.editor` |
| DMS `retrievalInstructions` | `service.storage.viewer` |
| DMS `copy` | `service.storage.creator` / `service.storage.admin` |

- 文档明确（[Validations](https://osdu.pages.opengroup.org/platform/system/file/File-Service/)）：
  服务在生成位置前会校验 **授权 token 与 partition ID**；访问文件元数据时会校验 **legal tags 与 ACL**；
  但**不校验 payload 中属性值的合法性**（那是 schema 的责任），也**不检查文件内容**。

### 8.2 错误模型（实测 + 交叉验证，**存在两套并存，需注意**）

**（A）当前 OSDU 标准：`AppError`** —— 所有 `@ApiResponse` 注解的 schema 都指向
`org.opengroup.osdu.core.common.model.http.AppError`：

```json
{ "code": 400, "reason": "Bad Request", "message": "Invalid record format." }
```

[Storage OpenAPI](https://community.opengroup.org/osdu/platform/system/storage/-/blob/master/docs/api/community/v2/openapi.yaml)
将其描述为 "Standard error response returned by OSDU services"。

**（B）本仓库遗留实现：`errors/ErrorResponse`** —— `RestExceptionHandler` 实际构造的是这个类：

```java
@JsonTypeInfo(include = As.WRAPPER_OBJECT, use = Id.NAME, visible = true)
@JsonTypeName("error")
public class ErrorResponse {
    private int code;
    private String message;
    private List<ErrorDetails> errors;   // 每个 ErrorDetails 有 reason/message 等
}
```

即实际响应形如 `{"error": {"code": 400, "message": "...", "errors": [{"reason": "...", "message": "..."}]}}`。
另外 `handleInvalidBody` 分支还会返回 `ApiError{status, message, errors[]}`。

**处理方式（本项目的决策）**：以 **(A) `AppError` 作为对外标准错误体**（因为它是官方 OpenAPI 声明的 schema），
同时提供配置开关 `http.error_format=apperror|legacy`，默认 `apperror`。
**理由**：以 OpenAPI 声明的契约为准才能保证客户端兼容；遗留格式仅用于与旧版部署对接。
这一取舍已登记为风险项 `R-04`（见 `docs/04-implementation-plan.md` §6）。

HTTP 状态码集合（实测自各端点的 `@ApiResponses`）：
`200 / 201 / 204 / 400 / 401 / 403 / 404 / 500 / 502 / 503`。

### 8.3 标准请求头（交叉验证）

| 头 | 必要性 | 说明 |
| --- | --- | --- |
| `Authorization: Bearer <JWT>` | **必需** | 所有 OpenAPI 的 `securitySchemes` 仅此一项（`http`/`bearer`） |
| `data-partition-id` | **必需** | 租户标识；缺失 → 401（验收测试有专门用例） |
| `correlation-id` | 标准，用于追踪 | `DpsHeaders` 管理，逐跳转发 |
| `x-user-id` | Entitlements/Policy 调用必需 | 用于记录/授权主体 |
| `On-Behalf-Of` | 可选 | 服务间代调用（`DpsHeaders`） |
| `x-collaboration` | 可选 | `id=<uuid>,application=<name>` |
| `frame-of-reference` | 可选 | Storage 记录查询用 |
| `AppKey` | **UNVERIFIED** 是否为现行标准头 | 见于历史文档与工作流 payload，未出现在现代 OpenAPI 中 |

### 8.5 ★ OpenAPI 规格与代码生成（实测）—— 决定了"契约以什么为准"

| 事实 | 说明 |
| --- | --- |
| **没有任何代码生成** | 仓库中所有 `pom.xml` 都不含 openapi-generator / swagger-codegen 插件。契约**不是**从 spec 生成代码，而是**从代码生成 spec** |
| **规格由 springdoc 在运行时生成** | `springdoc.api-docs.path=/v2/api-docs`、`springdoc.swagger-ui.path=/v2/swagger`、`openapi_3_1` 开关。CI 暴露的路由：`/api/file/v2/api-docs/swagger-config`、`/api/file/v2/api-docs.yaml`、`/api/file/v2/swagger-ui/index.html` |
| **唯一提交到仓库的 spec 文件** | `docs/api/community/v2/openapi.yaml`（938 行，openapi 3.1.0，`info.version 2.0.0`，`servers[0].url = /api/file/`），于 2026-03-24 由 commit `c069d6f87f`（"OpenAPI spec check CI job"）加入，**仅供 CI 检查用**，`mkdocs.yml` 甚至没有引用它 |
| ⚠️ **该 spec 文件是不完整的** | 它**遗漏了所有 `@Hidden` 端点**：`getLocation`、`getFileLocation`、`getFileList`、全部 DMS 与 `file-collections`、以及 delivery |
| ✅ **真正的线上契约** | 控制器注解（`@GetMapping` 等）+ 运行时 `/api/file/v2/api-docs.yaml` |

**对本项目的直接结论**：

1. **不能把 `docs/api/community/v2/openapi.yaml` 当作完整契约**。契约基准 = `file-core/.../api/*.java` 的注解
   + 验收测试 feature 文件 + §4.7 的实测 JSON。本项目的 `docs/03-api-contract.md` 由此汇总而成。
2. 本项目**手工维护**契约文档与 proto（不做代码生成），并用**契约符合性测试**（黄金样例）保证不漂移。
   理由：OSDU 的 JSON 契约（PascalCase schema 字段、三种错误体、Spring Page 结构）无法从 OpenAPI/IDL
   自动生成出可靠结果；生成器反而会引入不可控漂移（见 ADR-001 方案 B 的否决理由）。
3. 上游升级时，比对方式为：拉取新 tag → 重新提取控制器注解 → 与 `docs/03-api-contract.md` 逐条 diff。

### 8.6 其他常量（实测）
- `ChecksumAlgorithm`、`FileExtension` 枚举类（`constant/`）。
- 审计日志：`AuditLogger` + `AuditOperation`（如 `createLocationSuccess`、`readFileLocationSuccess`）——
  说明**审计事件是参考实现的一等公民**，本项目需在设计中预留审计端口。
- 响应头过滤：`middleware/ResponseHeaderFilter`。

---

## 9. ★ 核心问题：OSDU 是否支持 RPC / gRPC？

### 结论

**不支持。** OSDU 的规范接口面是 **REST/HTTP+JSON + OpenAPI 3.x**，
**没有任何已发布的 OSDU API 契约或文档定义 gRPC / protobuf / JSON-RPC / 通用二进制 RPC 接口**。

### 直接证据

| # | 证据 | 来源 |
| --- | --- | --- |
| 1 | **实测**：`osdu/platform/system/file` 全仓库检索 `*.proto` → **0 个结果** | 本报告取样归档 |
| 2 | File Service 的所有 API 均为 Spring `@RestController` + `@GetMapping/@PostMapping` + Swagger 注解 | `file-core/.../api/*.java` |
| 3 | 各服务的 API 规格是 `docs/api/<version>/openapi.yaml`，版本为 OpenAPI 3.x | `docs/api/community/v2/openapi.yaml` |
| 4 | Core Services Overview 中每个服务的 "Open API Spec" 列都是 `.yaml`/`.json` OpenAPI 文件 | [Core Services Overview](https://community.opengroup.org/groups/osdu/platform/-/wikis/Core-Services-Overview) |
| 5 | 文档明确："These are **code-generated swagger docs** running from the OSDU pre-shipping instances" | [Core Services API Docs](https://community.opengroup.org/groups/osdu/platform/-/wikis/Core-Services-API-Docs) |
| 6 | M26（最新已验证发布版本）版本说明未列出任何 gRPC/protobuf/RPC 特性、修复或依赖 | [M26 Release Notes](https://community.opengroup.org/osdu/governance/project-management-committee/-/wikis/M26-Release-Notes) |
| 7 | OSDU GitLab 中 "grpc" 的命中仅为云 SDK 传递依赖（GCP GCS Java 客户端内部、`google-cloud-cpp`），非 API 定义 | [File MR !136](https://community.opengroup.org/osdu/platform/system/file/-/merge_requests/136)、Seismic `sdapi` README |
| 8 | 唯一的二进制协议 ETP 用 **WebSocket + Apache Avro**，且规范明确声明 **"ETP does not use the Avro RPC facility"** | [ETP 1.1 规范](https://docs.energistics.org/ETP/ETP_TOPICS/ETP-000-012-0-C-sv1100.html)、[ETP_protocols.md](https://community.opengroup.org/maap/reservoir-ddms-home/-/blob/master/ETP_protocols.md) |
| 9 | 事件通知是 REST 订阅 + HTTP(S) webhook 推送，消息体为 JSON | [DataNotification 文档](https://osdu.pages.opengroup.org/platform/system/notification/DataNotification/) |
| 10 | 平台唯一的第三方扩展点是 **DDMS 注册**，且它只接受 **OpenAPI 3.0 文档** | [HowToBecomeADDMS](https://osdu.pages.opengroup.org/platform/system/register/HowToBecomeADDMS/)、[Register OpenAPI](https://community.opengroup.org/osdu/platform/system/register/-/blob/master/docs/api/community/v1/openapi.yaml) |

### UNVERIFIED（诚实标注）

- GitLab 匿名全局 blob 检索被拒（`GET /api/v4/groups/osdu/search?scope=blobs` → **401**），
  GitLab UI 的匿名搜索退化为项目名搜索。因此"**OSDU 全组任何仓库都不存在 `.proto` 文件**"
  **未能穷尽证明**。
- 已证明的是：**没有任何已发布的 OSDU API 契约、规范文档或版本说明定义 protobuf/gRPC 接口**。
- 未能查阅 **OSDU Data Platform Standard v1.0**（The Open Group 2026-06-09 发布，需注册/付费）
  正文，因此无法确认其对协议措辞的表述。
- 未能找到公开的权威 **"OSDU API Design Guidelines"** 文档；下述约定是从已发布规格中**归纳**的。

### OSDU 中最接近 RPC 的东西

| 机制 | 协议 | 是否 RPC | 说明 |
| --- | --- | --- | --- |
| Reservoir DDMS —— ETP | WebSocket + Apache Avro（二进制消息） | **否**（规范明确声明） | 面向井/油藏实时数据流的领域协议 |
| Reservoir DDMS —— GraphQL | HTTP/JSON | 否 | `/graphql`，REST 网关之上的查询语言 |
| Eventing（Notification/Register） | REST 订阅 + HTTP webhook | 否 | 底层可为 Kafka / RabbitMQ / 云消息总线 |
| Stream Admin Service | REST（自身接口由 OpenAPI 生成） | 否 | 管理 Kafka 流 |
| Seismic `sdapi` | REST（C++ `libcurl` 客户端） | 否 | 唯一用 C++ 的 OSDU 服务，但仍为 REST |

### 实际采用的扩展路线

既然 OSDU 没有 RPC，而需求要求"http + rpc 双协议"，本项目的做法是：

1. **REST 面**严格对齐 OSDU（唯一合规面）；
2. **gRPC 面**作为**平台外扩展**，共享同一应用层，独立端口，命名空间隔离；
3. 用**双协议等价性测试**把"契约漂移"变成构建失败；
4. 明确告知使用方：gRPC 面不参与 OSDU 合规性认证。

完整论证、备选方案与取舍见 **[ADR-001](adr/ADR-001-rpc-as-extension.md)**。

---

## 10. 差距分析：从参考实现到本项目

| 维度 | OSDU 参考实现 | 本项目目标 | 处理策略 |
| --- | --- | --- | --- |
| 语言/运行时 | Java + Spring Boot（需 JVM） | **C++20** | 重写；proto 与 JSON 契约保持对齐 |
| 接口协议 | 仅 REST | **REST + gRPC** | gRPC 作为扩展（ADR-001） |
| 存储后端 | 各云厂商一份实现（Azure/GCP/IBM），本地用 S3(SeaweedFS) | **集中存储（POSIX）+ 对象存储（S3）同一进程内可切换** | `IBlobStore` 端口 + `capabilities()`（ADR-003） |
| 位置/元数据存储 | Postgres / Datastore + Storage Service | 内置 SQLite（可插拔为远端 Storage Service） | `IFileLocationRepository` / `IMetadataRepository` 端口 |
| 鉴权 | Entitlements Service（必需） | 可插拔：本地 JWT + 静态角色表 / 远端 Entitlements | `IAuthorizationService` 端口，默认本地，配置切换到远端 |
| Legal / Schema 校验 | 依赖 Legal & Schema Service | 可插拔：默认宽松（仅结构校验）+ 可选远端校验 | `ILegalService` / `ISchemaValidator` 端口 |
| 消息/事件 | RabbitMQ / PubSub（`status-changed`） | 可插拔：默认写日志 / 可选 HTTP webhook | `IEventPublisher` 端口 |
| 部署 | 每云一套 Chart/部署 | 单二进制 + 配置文件，容器化 | 无外部强依赖即可启动（便于测试） |
| 多租户 | Partition Service 提供租户配置 | 配置文件 + 本地 partition 注册表，可选远端 Partition Service | `IPartitionRegistry` 端口 |

**核心设计原则**：**所有外部 OSDU 服务依赖都必须有本地实现（默认）与远端实现（可选）两套**，
以保证：(a) 服务可独立启动、独立测试（这是"每步测试"要求的前提）；(b) 可在完整 OSDU 平台中作为 File Service 的替代实现运行。

---

## 11. 本报告使用的关键来源

- 源码归档：`https://community.opengroup.org/api/v4/projects/90/repository/archive.tar.gz?sha=master`
  （project id 90 = `osdu/platform/system/file`，default branch `master`）
- [File Service 文档](https://osdu.pages.opengroup.org/platform/system/file/File-Service/)
- [File Service 官方 OpenAPI（v2, community）](https://community.opengroup.org/osdu/platform/system/file/-/blob/master/docs/api/community/v2/openapi.yaml)
- [Generic File Schema（File.Generic.1.0.0）](https://community.opengroup.org/osdu/data/data-definitions/-/blob/master/Generated/dataset/File.Generic.1.0.0.json)
- [Core Services Overview](https://community.opengroup.org/groups/osdu/platform/-/wikis/Core-Services-Overview)
- [Storage OpenAPI（AppError / bearer / cursor 分页）](https://community.opengroup.org/osdu/platform/system/storage/-/blob/master/docs/api/community/v2/openapi.yaml)
- [Register OpenAPI（DDMS 注册）](https://community.opengroup.org/osdu/platform/system/register/-/blob/master/docs/api/community/v1/openapi.yaml)
- [HowToBecomeADDMS](https://osdu.pages.opengroup.org/platform/system/register/HowToBecomeADDMS/)
- [M26 Release Notes](https://community.opengroup.org/osdu/governance/project-management-committee/-/wikis/M26-Release-Notes)
- [ETP 1.1 规范](https://docs.energistics.org/ETP/ETP_TOPICS/ETP-000-012-0-C-sv1100.html)
- [DataNotification 文档](https://osdu.pages.opengroup.org/platform/system/notification/DataNotification/)
- [ADR-001（RPC 作为扩展）](adr/ADR-001-rpc-as-extension.md)
- [ADR-003（存储抽象）](adr/ADR-003-storage-abstraction.md)
