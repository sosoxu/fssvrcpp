# 接口契约（实现与测试的唯一基准）

> 本文档是**实现与测试的唯一契约基准**。任何实现细节与本文档冲突时：
> 若冲突源于"实测上游代码"，改实现；若冲突源于"本文档笔误"，改文档并同步测试。
>
> 契约来源与优先级：
> 1. **上游控制器注解**：`osdu/platform/system/file@master` → `file-core/src/main/java/org/opengroup/osdu/file/api/*.java`
> 2. **上游验收测试**：`file-acceptance-test/src/test/resources/features/*.feature` 与 `input_payloads/`、`output_payloads/`
> 3. **core-common 模型类**（请求/响应字段名）：`org.opengroup.osdu.core.common.model.file.*`、`...dms.model.*`
> 4. `docs/api/community/v2/openapi.yaml` —— ⚠️ **仅作参考，不完整**（遗漏所有 `@Hidden` 端点）
>
> 取样 HEAD：`d7c25c2d7f5d2f42bed901c68a407098195389bb`（2026-09-11）。
> 详细调研过程与证据见 `docs/01-osdu-research.md` 与 `docs/appendix/osdu-file-service-source-notes.md`。

---

## 1. 全局约定

### 1.1 Base path 与版本

| 项 | 值 |
| --- | --- |
| 上游部署 context path | **`/api/file/`**（`server.servlet.contextPath`） |
| 本服务默认 base path | `/api/file`（**可配置**：`server.http.base_path`） |
| API 版本段 | `/v2`（唯一的现行版本；`/v1` 已于上游 v0.7.0 起移除，本项目不实现） |
| 端口 | HTTP `8080`（可配）；gRPC **默认关闭**（`FSS_GRPC_PORT`，`-1` = 系统分配，独立端口） |
| 内容类型 | 请求/响应 `application/json`（`/v2/info` 等纯文本端点除外） |

> 为便于测试与灰度，本服务在**同时**暴露"带 base path"的路径。
> 例如 `GET /api/file/v2/files/uploadURL`。
> 测试必须使用完整路径，不得依赖"恰好挂在根路径"的假设。

### 1.2 请求头

| 头 | 适用端点 | 必需 | 缺失/非法时的行为 |
| --- | --- | --- | --- |
| `Authorization: Bearer <JWT>` | 除 `revokeURL`、`info`、`liveness_check`、`readiness_check` 外的全部 | ✅ | `401`，消息 `Missing authorization token` |
| `data-partition-id` | 除 `revokeURL`、`info`、`liveness_check`、`readiness_check` 外的全部 | ✅ | `401`，消息 `Missing partitionID` |
| `correlation-id` | 可选（全部） | ❌ | 服务生成一个并回填响应头 |
| `x-user-id` | 可选 | ❌ | 从 JWT 的 `email`/`sub` 取；用于审计与位置记录的 `CreatedBy` |

> 实测：`revokeURL` 在控制器上**没有** `data-partition-id` 依赖（body 里带
> `resourceGroup` + `storageAccount`）；`info` 与两个健康检查在部署的 authz policy 中被显式放行。

### 1.3 鉴权角色（`@PreAuthorize` 实测值）

| 角色常量 | 字面值 |
| --- | --- |
| `FileServiceRole.VIEWERS` | `service.file.viewers` |
| `FileServiceRole.EDITORS` | `service.file.editors` |
| `FileServiceRole.ADMIN` | `service.file.admin` |
| `DeliveryRole.VIEWER` | `service.delivery.viewer` |
| `DatasetConstants.DATASET_VIEWER_ROLE` | `service.dataset.viewers` |
| `DatasetConstants.DATASET_EDITOR_ROLE` | `service.dataset.editors` |
| `StorageRole.VIEWER` / `CREATOR` / `ADMIN` | `service.storage.viewer` / `service.storage.creator` / `service.storage.admin` |

鉴权判定：`authorizeAny(headers, roles)` —— **拥有任一角色即通过**。

**端点 ↔ 角色（实测自上游各 `api/*.java` 的 `@PreAuthorize`，vendored commit `d7c25c2d`）**

| 端点 | 上游注解 | 角色 |
| --- | --- | --- |
| `POST /v2/files/metadata` | `FileMetadataApi:56` | `service.file.editors` |
| `GET /v2/files/{id}/metadata` | `FileMetadataApi:77` | `service.file.viewers` |
| `DELETE /v2/files/{id}/metadata` | `FileMetadataApi:96` | `service.file.editors` **或** `service.file.admin` |
| `POST /v2/getFileList` | `FileListApi:72` | `service.file.editors` |
| `POST /v2/getLocation`、`POST /v2/getFileLocation`、`GET /v2/files/uploadURL` | `FileLocationApi:80/104/126` | `service.file.editors` |
| `GET /v2/files/{id}/downloadURL` | `FileDeliveryApi:51` | **`service.file.viewers`**（不是 editors） |
| `POST /v2/files/storageInstructions`、`POST /v2/file-collections/storageInstructions` | `FileDmsApi:82`、`FileCollectionDmsApi:78` | `service.dataset.editors` |
| `POST /v2/files/retrievalInstructions`、`POST /v2/file-collections/retrievalInstructions` | `FileDmsApi:102`、`FileCollectionDmsApi:98` | `service.dataset.viewers` |
| `POST /v2/files/copy`、`POST /v2/file-collections/copy` | `FileDmsApi:123`、`FileCollectionDmsApi:119` | `service.storage.creator` **或** `service.storage.admin` |
| `POST /v2/delivery/GetFileSignedUrl` | `DeliveryApi:83` | `service.delivery.viewer` |
| `POST /v2/files/revokeURL` | `FileAdminApi:47` | `service.file.admin` |
| `/v2/info`、`/v2/liveness_check`、`/v2/readiness_check` | — | 不需要角色 |

**授权发生在输入校验之前**（Spring Security 过滤器先于 controller）：未授权调用者拿到 `403`，
不能靠 `400` 的差异探测数据是否存在。

本实现在**两处**判同一张表（P8）：

| 位置 | 作用 | 证据 |
| --- | --- | --- |
| HTTP 适配层 `Router::Wrap` 的**路由级预检**（`RouteAuthTable()`，按 `RouteOptions.name` 查表） | 在**任何 DTO 解析之前**给出 `401`/`403`，与上游"过滤器先于 controller"一致；未登记的路由 **fail-closed** | `tests/integration/test_auth_matrix.cpp`（含"畸形请求体也是 401/403"） |
| 用例入口 `AuthorizeCaller`/`AuthorizeCallerAny`（**第一步**） | 纵深防御；gRPC 面**只有**这一道 | `tests/unit/test_roles.cpp`（含"授权先于校验"与"403 先于 400"） |

`/v1/transfer/{token}` 数据面**不走 JWT**：它用自签 token 自证（绑定 `op`/`partition`/`exp`，
见威胁模型 T3/T4），因此不在上表的角色矩阵里。

角色来源有两种**可配置**的形态（ADR-012）：`auth.mode=jwt` = 本地从 token 的 `roles` claim
（∪ 静态角色表）取；`auth.mode=remote-entitlements` = 每个请求向 Entitlements 问一次
（接口见 §4.5，**依赖不可用一律 503、绝不放行**）。`auth.mode=disabled` 仅开发可用。

### 1.4 `expiryTime` 语义（唯一 query 参数）

```
语法：^[0-9]+(M|H|D)$        分 / 时 / 天
缺省：1 小时                   ← 实测 ExpiryTimeUtil.DEFAULT_TTL = 1 HOURS
上限：7 天                     ← 实测 CAPPED_DEFAULT_TTL = 7 DAYS
超限行为：静默截断为 7 天      ← 不报错
非法行为：400 "expiryTime pattern isn't supported. Value should be one of these regex patterns ^[0-9]+M$ , ^[0-9]+H$ , ^[0-9]+D$"
```

出现在：`files/uploadURL`(GET)、`files/{id}/downloadURL`(GET)、
`files/storageInstructions`、`files/retrievalInstructions`、
`file-collections/storageInstructions`、`file-collections/retrievalInstructions`。

### 1.5 ID 与 kind 规则（实测正则）

| 项 | 规则 |
| --- | --- |
| `kind` | `^[\w\-\.]+:[\w\-\.]+:[\w\-\.]+:[0-9]+.[0-9]+.[0-9]+$`（`KindValidator` / `ValidationDoc.KIND_REGEX`） |
| kind 语义校验 | 按 `:` 切成**恰好 4 段**，且 `[1] == "wks"`、`[2] == "dataset--File.Generic"`，否则 `400 Invalid kind` / `Invalid source in kind` / `Invalid entity in kind` |
| 记录 id | 服务端生成：`"<partition>:dataset--File.Generic:<uuid 去横线>"` |
| 文件 `FileID`（`getLocation` 旧接口） | `^[\w,\s-]+(\.\w+)?$`，且有长度上限（验收测试有"length exceeding limit"用例） |
| ACL 组名 | `^data\.[a-zA-Z0-9_+&*-]+(?:\.[a-zA-Z0-9_+&*-]+)*@(?:[a-zA-Z](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$` |
| `FileSource` | `"/" + "<userId>/<epochMillis>-<yyyy-MM-dd-HH-mm-ss-SSS>/<fileID>"`，**带前导斜杠**；Azure 路径上限 **1024** 字符 |

### 1.6 错误体：三种形态 + 兼容开关

本项目对外**默认使用 `AppError`**（它是上游所有 `@ApiResponse` 声明的 schema，也是当前 OSDU 标准）。

| 开关值 | 形态 | JSON |
| --- | --- | --- |
| `apperror`（**默认**） | `AppError` | `{"code": 400, "reason": "Bad Request", "message": "..."}` |
| `legacy` | `ErrorResponse` | `{"error": {"code": 400, "message": "...", "errors": [{"reason": "...", "message": "..."}]}}` |
| `api_error` | `ApiError` | `{"status": "BAD_REQUEST", "message": "...", "errors": ["..."]}` |

配置项：`http.error_format`。契约测试对三种形态各跑一遍（同一请求的 `code`/`message` 必须一致，
只有外层包装不同）。

**通用 `reason` 映射表**（`code` 即 HTTP 状态码）：

| HTTP | `reason` | 触发条件 |
| --- | --- | --- |
| 400 | `Bad Request` | 校验失败、非法 `expiryTime`、`FileSource` 缺失/不存在、kind 非法、fileID 已存在 |
| 401 | `Unauthorized` | 缺 token / 缺 partition / token 无效 |
| 403 | `Forbidden` | 角色不足 |
| 404 | `Not Found` | 记录或位置不存在（消息 `Record Not Found` / `Not found location for fileID : <id>`） |
| 500 | `Internal Server Error` | 未预期异常 |
| 502 | `Bad Gateway` | 依赖服务（Storage/远端）返回异常 |
| 503 | `Service Unavailable` | 存储后端不可用或过载 |

**上游实测的固定消息**（契约测试逐条断言）：

```
"FileSource can not be empty"
"Invalid source file path to copy from <path>"
"Record Not Found"
"Not found location for fileID : <id>"
"Location for fileID = <id> already exists"
"Invalid kind"
"Invalid source in kind"
"Invalid entity in kind"
"Missing authorization token"
"Missing partitionID"
"ConstraintViolationException: Invalid FileLocationRequest"
"expiryTime pattern isn't supported. Value should be one of these regex patterns ^[0-9]+M$ , ^[0-9]+H$ , ^[0-9]+D$"
```

---

### 1.6b 多实例（`deployment.mode=multi`）的强制启动校验（C8.9）

`multi` 模式下配置校验**必须**拒绝以下任一情况（每条都有"拒绝启动"的测试，且五条全满足
时**必须通过** —— 正例断言防"校验恒真"）：

| # | 配置项 | 要求 | 理由 |
| --- | --- | --- | --- |
| 1 | `metadata.repository` / `location.repository` | `postgres` | 各实例共享 SQLite 会让状态发散 |
| 2 | `leases.enabled` / `leader_election.enabled` | `true` | 租约与领导者选举是多实例一致性的前提（ADR-009） |
| 3 | `gc.require_lease_expiry` | `true` | 否则 GC 会把**在途上传**当孤儿删（实测 20/20 误删） |
| 4 | `storage.posix.shared_mount_required` | `true` | 存储根必须在共享挂载上 |
| 5 | `deployment.max_clock_skew_seconds` | `0 < 值 ≤ 60` | 时钟偏差过大会让租约/过期误判（C8.10） |

**运行形态**：PG 版仓储 + PG 租约 + 数据库时钟（ADR-009）尚未交付（计划 P9），因此
组合根对 `FSS_DEPLOYMENT_MODE=multi` **拒绝启动**（明确报错），而不是以单实例状态跑在多实例里。

---

### 1.7 HTTP 层硬上限与拒绝语义

由 `fss_http` 包装层（`src/common/http/`）强制，**不依赖第三方库的默认行为**。
下表的值是**实测行为**（不是设计意图），并由阶段 1 的 `test_httplib_hardening.cpp` 锁定。

| 场景 | 行为 | 备注 |
| --- | --- | --- |
| `Content-Length` > 路由上限 | **413** `Payload Too Large` | 包装层在读体之前按 `Content-Length` 前置拦截 |
| chunked 且累计字节 > 路由上限 | **400** `Bad Request` | 底层库的读失败语义；包装层中止读取，**handler 不得产生副作用** |
| `Content-Length` 与实际读取字节数不符 | **400** | 包装层的防御性一致性断言（防"静默截断"） |
| 重复的 `Content-Length` | **400** | 请求走私防护 |
| `Content-Length` 与 `Transfer-Encoding: chunked` 并存 | **400** | 请求走私防护 |
| 单个请求头 > 8192 字节 | **400** | 实测：8000 字节的头正常，9000 字节 → 400（`CPPHTTPLIB_HEADER_MAX_LENGTH`） |
| **请求行/target ≥ 8192 字节** | **连接被关闭且无响应** | ★ **实测更正**：target ≤ 8191 正常处理；≥ 8192 时库在解析请求行阶段直接关闭连接，**不写任何响应**（其 `414` 分支在这条路径上不可达）。原表写的 "8192 → 400" 是按编译期常量推断的，已作废 |
| 已知路径 + 未注册的方法 | **404**（不是 405） | 实测行为，**决定接受**：OSDU 只定义已注册方法，兼容性无影响 |
| 未实现的 `Transfer-Encoding` | **400** | 包装层归一化 |

**各端点的请求体上限**（`server.http` 配置）

| 端点组 | 默认上限 | 配置项 |
| --- | --- | --- |
| JSON 端点（`/v2/files/metadata`、DMS、Delivery 等） | 10 MiB | `server.http.max_body_bytes` |
| 数据面 `/v1/transfer/{token}` | 不限（受租户配额与磁盘水位约束） | `server.http.transfer_max_body_bytes` |
| `/v2/getFileList`、`/v2/getFileLocation`、`revokeURL` | 256 KiB | 按端点覆盖 |

**由包装层（`fss_http`）**额外强制**的项**（库在这两条路径上不检查）：

| 场景 | 行为 | 为什么必须自己做 |
| --- | --- | --- |
| 重复的 `Content-Length` | **400** | 实测：走流式（`ContentReader`）路径时库**不会**替我们检查，请求会带着两个长度进 handler → 请求走私的入口 |
| `Content-Length` 与 `Transfer-Encoding: chunked` 并存 | **400** | 同上 |

**并发连接突发**：`listen backlog` 被库硬编码为 5，包装层在 include 前覆盖为 **512**。
实测后果（不覆盖时）：50 个 400 ms 的并发请求总耗时 **1436 ms**（本该 ~400 ms）——
accept 队列只有 5 个位置，多余的 SYN 被内核丢弃，客户端按 1 s/3 s 重试。
覆盖后：总耗时 **414 ms**，峰值在途 **50**。回归断言见 `test_http_server.cpp` 的并发用例。

**关键约束（回归测试项）**：任何"超限被拒绝"的场景，**handler 都不得产生副作用**
（不得写出对象、不得写位置记录、不得写元数据）。
这条是被一个真实缺陷驱动的（见 ADR-002 的 H-2：底层库在超限时曾返回 201 且交出 0 字节，
若不设此约束会产生"上传成功但文件为空"的静默数据丢失）。

### 1.8 部分内容（`Range` / 206 / 416）语义

数据面的下载端点支持 `Range`。**权威依据是 RFC 7233**（§2.1 语法、§4.4 的 416 条件）；
下表是 `src/common/bytes/ParseRangeHeader()` 实现的判定，由 `tests/unit/test_bytes.cpp` 锁定。

| 请求 | 行为 | 依据 / 备注 |
| --- | --- | --- |
| 无 `Range` 头（或只有空白） | **200** + 全量 | 等价于"未请求部分内容" |
| `bytes=a-b` | **206** `Content-Range: bytes a-b/total` | `b` 超过末尾时**截断**到 `total-1`（不报错） |
| `bytes=a-` | **206** 到末尾 | |
| `bytes=-N`（后缀） | **206** 最后 N 字节 | `N >= total` → 整个表示；`N == 0` → **416** |
| `bytes=a-b,c-d`（多段） | **206** `multipart/byteranges` | 各段顺序**按客户端给出的顺序**，不重排 |
| `a >= total`（起点越界） | **416** + `Content-Range: bytes */total` | ★ 这是 H-1 缺陷的防线：**不得**返回 0 字节的 206，也不得算出下溢的 `Content-Length` |
| 多段中**部分**不可满足 | **206**，丢弃不可满足的段 | RFC 7233 §4.4 |
| 多段中**全部**不可满足 | **416** | RFC 7233 §4.4 |
| `total == 0` | 任何 `Range` → **416** | 空表示没有可满足的区间 |
| 语法非法（非数字、缺 `-`、`b < a`、未知单位、`%` 溢出…） | **忽略该头 → 200** + 全量 | ★ 416 **只**用于"语法合法但不可满足"；把非法语法也判 416 会让客户端陷入重试循环 |
| 区间数量 > 32 | **忽略该头 → 200** | 防御性上限：避免病态请求构造出巨型 multipart 响应 |

补充约定：

- `Content-Range` 的格式为 `bytes first-last/total`；不可满足时为 `bytes */total`。
- 总长度未知（例如流式来源）时**不做**可满足性判定（只做语法校验），后缀形式 `-N` 此时无法换算 → 判为非法并忽略。
- `Accept-Ranges: bytes` 在数据面响应上始终返回，便于客户端发现能力。

---

## 2. REST 端点规格

**端点总数：19 个**（§2.1–§2.12；其中 §2.9 含 6 个 DMS 端点、§2.12 含 3 个运维端点）。
阶段 4 的门槛 C4.1 与阶段 8 的 C8.1 都以此数量为准。


### 2.1 `GET /api/file/v2/files/uploadURL`

| 项 | 值 |
| --- | --- |
| 角色 | `service.file.editors` |
| Query | `expiryTime`（可选） |
| 请求体 | 无 |
| 成功 | `200` `LocationResponse` |
| 失败 | `400`（`expiryTime` 非法）、`401`、`403`、`500`、`502`、`503` |
| 副作用 | 在 staging 区创建空对象 + 写入一条位置记录 |

**响应样例**
```json
{
  "FileID": "da92f52401dc4d1cb93515f159c110d4",
  "Location": {
    "SignedURL": "http://127.0.0.1:9000/opendes-staging/2025/09/16/da92f524...?X-Amz-Algorithm=AWS4-HMAC-SHA256&...",
    "FileSource": "/osdu-user/1614784413120-2021-03-03-15-13-33-120/da92f52401dc4d1cb93515f159c110d4"
  }
}
```

**契约要点（易错项）**
- `FileID` / `Location` / `SignedURL` / `FileSource` 的**大小写必须逐字符一致**。
- `FileSource` 在 `Location` **内部**，不是顶层字段。
- 对象存储模式下 `SignedURL` 指向存储端点；集中存储模式下指向本服务的
  `/api/file/v1/transfer/{token}`（**扩展**，见 §7）。
- **不返回 `Driver`**（上游该端点响应无此字段）。本项目若需要暴露驱动，
  只能通过 RPC 的扩展字段（见 §4）。

### 2.2 `POST /api/file/v2/getLocation` ⚠️ 已废弃

| 项 | 值 |
| --- | --- |
| 角色 | `service.file.editors` |
| 请求体 | `{"FileID": "optional-existing-file-id"}` |
| 成功 | `200` `LocationResponse`（同 2.1） |
| 失败 | `400`（fileID 已存在 → `Location for fileID = <id> already exists`；fileID 长度超限；位置非法）、`401`、`403` |
| 备注 | 上游 `@Hidden`，文档标记将废弃。**本项目实现但标记 deprecated，响应加 `Deprecation` 头** |

### 2.3 `POST /api/file/v2/getFileLocation` ⚠️ 已废弃

| 项 | 值 |
| --- | --- |
| 角色 | `service.file.editors` |
| 请求体 | `{"FileID": "..."}` |
| 成功 | `200` `FileLocationResponse` |
| 失败 | `400`（`Not found location for fileID : <id>`；空体 → `ConstraintViolationException: Invalid FileLocationRequest`）、`401`、`403` |

**响应样例**
```json
{ "Driver": "posix", "Location": "/var/lib/fss/data/opendes/persistent/2025/09/16/da92f524..." }
```

**兼容开关**：上游把所有云的 `Driver` 硬编码为 `"GCS"`（见调研 §4.0 F8）。
若对接方依赖该字面值，设置 `storage.driver_report_override: "GCS"`。
**默认不开启**（上报真实驱动），并在 `/v2/info` 中声明该行为差异。

### 2.4 `GET /api/file/v2/files/{id}/downloadURL`

| 项 | 值 |
| --- | --- |
| 角色 | `service.file.viewers` |
| Query | `expiryTime`（可选） |
| 成功 | `200` `{"SignedUrl": "..."}` |
| 失败 | `400`、`401`、`403`、`404`、`500` |
| 前置 | 该 `id` 必须已有元数据记录 |

### 2.5 `POST /api/file/v2/getFileList`

| 项 | 值 |
| --- | --- |
| 角色 | `service.file.editors` |
| 请求体 | `FileListRequest` |
| 成功 | `200` `FileListResponse`（**Spring Page 结构**） |
| 失败 | `400`（空体/非法/无记录）、`401`、`403` |

**请求样例**
```json
{ "TimeFrom": "2021-03-01T00:00:00", "TimeTo": "2021-03-31T00:00:00",
  "PageNum": 0, "Items": 10, "UserID": "user@example.com" }
```

**响应样例**
```json
{
  "Content": [
    { "FileID": "da92f52401dc4d1cb93515f159c110d4",
      "Driver": "posix",
      "Location": "/var/lib/fss/data/opendes/persistent/2025/09/16/da92f524...",
      "CreatedAt": "2021-03-03T15:13:33.120+0000",
      "CreatedBy": "osdu-user" }
  ],
  "Number": 0,
  "NumberOfElements": 1,
  "Size": 10
}
```

**契约要点**
- 字段是 `Content` / `Number` / `NumberOfElements` / `Size` —— **不是** `results` / `totalCount`；
  `Content` 每项恰好 5 个键：`FileID` / `Driver` / `Location` / `CreatedAt` / `CreatedBy`。
- `CreatedAt` 格式 `yyyy-MM-dd'T'HH:mm:ss.SSSZ`（注意 `Z` 位置是 `+0000` 风格的偏移）。
- `Driver` 是**驱动上报的真实名字的小写形态**（`posix` / `s3` / `memory`），与 §2.2/§2.3 一致
  （上游把所有云硬编码成 `"GCS"`，复刻该行为的开关见 §6 `storage.driver_report_override`）。
- `PageNum` 从 **0** 开始；排序是**稳定全序**（`CreatedAt` 升序、同秒按 `FileID` 升序），
  否则 offset 分页会漏项/重项。
- 时间边界**含端点**（`>= TimeFrom` 且 `<= TimeTo`，`LocationQuery` 的语义）。
- `Items` **必填**（上游 DTO 是基本类型，缺省 0 → `@Positive` 失败）：`{}` 与"缺 `Items`"都是 `400`。
- 无匹配记录（含翻到超出范围的页）→ `400`，消息逐字对齐上游 provider：
  `Nothing found for such filter and page(num: <n>, size: <m>).`
  （`provider/file-azure/.../repository/FileLocationRepository.java` 的
  `FileLocationNotFoundException`；排序也用 `PageRequest.of(pageNum, pageSize, ASC, CREATED_AT)`）。
- `TimeFrom > TimeTo` → `400`（消息含 `should be before TimeTo`）；**不能**退化成"区间内无记录"。

**⚠️ 一处刻意的放宽（与上游参考实现的差异，已登记）**
上游参考实现的校验器把 `TimeFrom`/`TimeTo`/`UserID` 定为**必填**
（`ValidationServiceTest#fileListRequestProvider` → `@NotNull`/`@NotBlank`）。
本实现把三者视为**可选过滤器**（缺省 = 不过滤）。理由：
① 上游**验收样例**没有覆盖这一差异（它的"非法"样例缺的是 `Items`，三条负向样例
`File_GetList_{Empty,Invalid,NoRecord}Payload.json` 我们逐字驱动且全部 `400`）；
② 该放宽方向只会让"上游能发的请求"继续成功，不会让上游的合法请求失败。
上游 unit test 的必填规则若要在本仓库生效，只需在适配层补结构校验（不影响用例层）。

**上游负向样例（vendored 到 `tests/conformance/fixtures/upstream/list/`）**

| 文件 | 内容 | 期望 |
| --- | --- | --- |
| `File_GetList_EmptyPayload.json` | `{}` | `400` |
| `File_GetList_InvalidPayload.json` | 缺 `Items` | `400` |
| `File_GetList_NoRecordPayload.json` | 完整请求、库中无匹配 | `400` |

### 2.6 `POST /api/file/v2/files/metadata`

| 项 | 值 |
| --- | --- |
| 角色 | `service.file.editors` |
| 请求体 | `FileMetadata`（`File.Generic` 记录，见 §3） |
| 成功 | `201` `{"id": "<partition>:dataset--File.Generic:<uuid>"}` |
| 失败 | `400`（结构校验、kind 非法、`FileSource` 空/不存在）、`401`、`403`、`500`、`502`、`503` |
| 副作用 | staging→persistent **服务端复制** → 计算并**覆写**校验和 → 写记录 → 发布事件 → 删除 staging 对象 |

**执行序列（必须逐条实现，见调研 §2.1）**

```
1.  publish status(DATASET_SYNC, IN_PROGRESS)                      [非致命]
2.  validateKind: 4 段, [1]=="wks", [2]=="dataset--File.Generic"
3.  filePath = data.DatasetProperties.FileSourceInfo.FileSource     [空 → 400 "FileSource can not be empty"]
4.  id = "<partition>:dataset--File.Generic:<uuid-no-dashes>"
5.  stagingLocation / persistentLocation = storageUtil 解析
6.  blob.copy(staging → persistent)                                 [失败 → 502/500]
7.  checksum = 计算(persistent); 非空则覆写 FileSourceInfo.Checksum + ChecksumAlgorithm
8.  record = 组装(id, acl, legal, kind, ancestry, data, meta, tags)
9.  metadataRepo.Save(record, version=1)                            [失败 → 回滚删除 persistent, 500]
10. publish status(SUCCESS) + datasetDetails                        [非致命]
11. remove(staging)                                                 [失败 → 忽略 + 审计告警 `createMetadataStagingCleanupFailure`, 不影响响应]
12. 任一步 6/7/9 失败 → remove(persistent) 回滚 + publish status(FAILED)
                       + 审计 `createMetadataFailure` → 重抛
```

**第 7 步的校验和语义（C6.4）**

```
checksum = storageUtil.getChecksum(persistentLocation)
非空 → 覆写 FileSourceInfo.Checksum + FileSourceInfo.ChecksumAlgorithm
        （并同步 data.Checksum / data.ChecksumAlgorithm）
失败语义：**无**（这一步不产生 4xx/5xx）
```

> **客户端传入的 `Checksum`/`ChecksumAlgorithm` 是"会被覆写的输入"，不是"待校验的断言"。**
> 上游证据：① `docs/01-osdu-research.md` §2.3「校验和：**服务端覆写**客户端传入的
> `Checksum`/`ChecksumAlgorithm`（至少 Azure 实现如此）」，第 7 条失败语义为 `—`；
> ② 权威样例 `File_CorrectPayload.json` 客户端给的是 `MD5("") = d41d8cd9…` 却声明
> `ChecksumAlgorithm: "SHA-256"`，**期望响应 `201`**。
> （本项目计划曾写"提供但不符 → `400` + 删除对象"，**没有上游依据**，已在
> `docs/00-final-design.md` §5 登记为被推翻的结论。）

| 情形 | 服务端行为 | 响应 |
| --- | --- | --- |
| 客户端给了 `Checksum`/`ChecksumAlgorithm`（任意值：错值、长度不符、不认识的算法、为空） | **忽略并覆写**（不比对、不拒绝、不回滚） | `201` |
| 驱动给出了**合法**的原生校验和（hex 且长度与该算法匹配） | 直接采用（零额外读盘），算法名写**规范名**：`SHA256` / `SHA1` / `MD5`（Azure 的 `getChecksum` 给 MD5） | `201` |
| 驱动**没给**、算法不认识、或值不是该算法的合法 hex（如 `ETAG`） | **流式回算 SHA-256**；**不得**把 `ETAG` 之类当校验和写进记录 | `201` |

三条实现约束（C6.4 / C6.9）：

1. 校验和必须**流式**计算 —— 对象不得整体驻留内存（含跨 store 复制的兜底路径）。
2. 覆写必须同时落到**两处**（`data.*` 与 `FileSourceInfo.*`），且两处一致。
3. 算法必须**跟随驱动**：记录里写的是"值实际所用的算法"的规范名，不能把 MD5 的值标成 `SHA256`。

**第 10 步的两个事件（都**非致命**：发布失败只告警，不影响 `201`）**

| 事件 | topic | 载荷 |
| --- | --- | --- |
| `status`（第 1/10/12 步共用） | `status-changed` | `{partition, status ∈ {IN_PROGRESS, SUCCESS, FAILED}, dataset_sync: "DATASET_SYNC", version}` |
| `datasetDetails`（第 10 步第二个） | `datasetDetails` | `kind: "datasetDetails"` + `properties: {correlationId, datasetId(= 记录 id), datasetType: "FILE", datasetVersionId(= 版本字符串), recordCount: 1, timestamp(毫秒)}`；body 是**长度为 1 的数组**（与上游一致） |

> 上游依据（一手，vendored 到 `/home/ll/osdu-file-upstream`）：
> `file-core/src/main/java/org/opengroup/osdu/file/service/status/FileDatasetDetailsPublisher.java`
> 定义 `KIND = "datasetDetails"`、`DATASET_TYPE = FILE`、`recordCount = 1`、
> `timestamp = System.currentTimeMillis()`，并在失败时只 `log.warning("Failed to publish dataset details")`；
> `FileMetadataService` 在 `publishSuccessStatus(recordIds.get(0), recordIdVersions.get(0))` **之后立刻**
> 调用 `publishDatasetDetails(...)`（即"先 status、再 datasetDetails"的顺序）。
> `properties.correlationId` 取自请求头 `x-correlation-id`（本实现由适配层填入 `CallerContext`）。

**请求样例（权威黄金样例，实测自 `input_payloads/File_CorrectPayload.json`）**
见 §3.3。

### 2.7 `GET /api/file/v2/files/{id}/metadata`

| 项 | 值 |
| --- | --- |
| 角色 | **`service.file.viewers`**（★ 官方文档误写为 editors，以代码为准） |
| 成功 | `200` `RecordVersion`（= 记录全字段 + `version`） |
| 失败 | `400`、`401`、`403`、`404`（`Record Not Found`）、`500` |

### 2.8 `DELETE /api/file/v2/files/{id}/metadata`

| 项 | 值 |
| --- | --- |
| 角色 | `service.file.editors` **或** `service.file.admin` |
| 成功 | `204`（**无响应体**） |
| 失败 | `400`、`401`、`403`、`404`、`500` |
| 序列 | `getMetadataById` → `metadataRepo.Delete`（上层要求等价于 Storage 的 `POST /records/{id}:delete` 且返回 204）→ 删除 persistent 对象 |

### 2.9 DMS 端点（6 个）

| 路径 | 角色 | 请求 | 响应 |
| --- | --- | --- | --- |
| `POST /api/file/v2/files/storageInstructions?expiryTime=` | `service.dataset.editors` | 空体 | `StorageInstructionsResponse` |
| `POST /api/file/v2/files/retrievalInstructions?expiryTime=` | `service.dataset.viewers` | `{"datasetRegistryIds": [...]}` | `RetrievalInstructionsResponse` |
| `POST /api/file/v2/files/copy` | `service.storage.creator`/`admin` | `{"datasetSources": [ ...records... ]}` | `[CopyDmsResponse]` |
| `POST /api/file/v2/file-collections/storageInstructions?expiryTime=` | `service.dataset.editors` | 空体 | 同上 |
| `POST /api/file/v2/file-collections/retrievalInstructions?expiryTime=` | `service.dataset.viewers` | 同上 | 同上 |
| `POST /api/file/v2/file-collections/copy` | `service.storage.creator`/`admin` | 同上 | 同上 |

**响应结构（逐键实测自上游 provider，vendored commit `d7c25c2d`）**

```json
// StorageInstructionsResponse（files 版）
{ "providerKey": "POSIX",
  "storageLocation": { "signedUrl": "...", "fileSource": "...", "createdBy": "...",
                       "expiryTime": "2023-11-14T23:13:20.000+0000" } }

// StorageInstructionsResponse（file-collections 版）★ 键名不同
{ "providerKey": "POSIX",
  "storageLocation": { "signedUrl": "...", "fileCollectionSource": "...", "fileCount": 1,
                       "fileNames": ["<文件名>"], "createdBy": "...", "expiryTime": "..." } }

// RetrievalInstructionsResponse（files 版）
{ "datasets": [ { "datasetRegistryId": "...", "providerKey": "POSIX",
                  "retrievalProperties": { "signedUrl": "...", "fileSource": "...",
                                           "createdBy": "...", "expiryTime": "..." } } ] }

// RetrievalInstructionsResponse（file-collections 版）
{ "datasets": [ { "datasetRegistryId": "...", "providerKey": "POSIX",
                  "retrievalProperties": { "signedUrl": "...", "fileCollectionSource": "...",
                                           "fileCount": 1, "fileNames": ["..."],
                                           "createdBy": "...", "expiryTime": "..." } } ] }

// CopyDmsResponse（数组元素；`datasetBlobStoragePath` 是**目标（persistent）**位置）
{ "success": true, "datasetBlobStoragePath": "..." }
```

> 一手依据：
> `provider/file-azure/.../service/StorageServiceImpl.java:293`（`AzureFileDmsUploadLocation` =
> `signedUrl`/`fileSource`/`createdBy`/`expiryTime`）与 `:268`（下载侧同形状）；
> `provider/file-azure/.../service/FileCollectionStorageServiceImpl.java:103/179`
> （`AzureFileCollectionDmsUploadLocation` = `signedUrl`/**`fileCollectionSource`**/`fileCount`/
> `fileNames`/`createdBy`/`expiryTime`）；`file-core/.../service/FileDmsServiceImpl.java:113`
> （copy 的路径取 `destinationPath`）。
> **本项目一个指令 = 一个对象**：集合版的 `fileCount` 恒为 1、`fileNames` 是
> `fileCollectionSource` 的最后一段（上游是按目录列出目录内全部文件——本仓库不实现"目录指令"）。
> `expiryTime` 用与 `CreatedAt` 相同的 OSDU 时间戳格式（末尾 `+0000`，不是 `Z`）。
>
> ★ `/v2/files/*` 与 `/v2/file-collections/*` **不是同一套键**：集合版有
> `fileCollectionSource` + `fileCount` + `fileNames`，且**没有** `fileSource`。
> 逐键断言见 `tests/conformance/test_dms_delivery.cpp`。

**`providerKey` 取值**：上游各家不同（Azure `"AZURE"`）。
本项目用 `"POSIX"` / `"S3"`，可通过 `storage.provider_key_override` 覆盖以兼容既有客户端。

### 2.10 `POST /api/file/v2/delivery/GetFileSignedUrl`

| 项 | 值 |
| --- | --- |
| 角色 | `service.delivery.viewer` |
| 请求体 | `{"srns": ["..."]}` |
| 成功 | `200` `UrlSigningResponse` |

**响应样例**
```json
{
  "processed": {
    "srn:file/...": { "signedUrl": "https://...", "unsignedUrl": "https://...",
                      "kind": "opendes:wks:dataset--File.Generic:1.0.0", "connectionString": null }
  },
  "unprocessed": ["srn:file/unknown"]
}
```

- `processed` 的每个元素**恰好 4 个键**，且 `connectionString` 必须**存在且为 `null`**
  （上游客户端按字段存在性判断）。
- `srns` 里无法解析/查不到的条目进 `unprocessed`（**不是错误**）；`{"srns":[]}` 合法 → `200`。

### 2.11 `POST /api/file/v2/files/revokeURL`

| 项 | 值 |
| --- | --- |
| 角色 | `service.file.admin` |
| 认证 | **不要求 `data-partition-id`** |
| 请求体 | 自由字符串映射 |
| 成功 | **`204`（无体）** |
| 失败 | `400`（Azure：`Illegal argument for resourceGroup { x } or storageAccount { y }`）、`401`、`403` |

> 本项目的集中存储驱动没有"用户委派密钥"概念，因此该端点：
> - S3 驱动：返回 `204` 并真正吊销（若配置了 STS/临时凭证），否则 `204` + 审计告警；
> - POSIX 驱动：`204` + 审计告警（无密钥可吊销），并在响应头 `X-FSS-Notice` 中说明。
> 保持 **204 语义**不变，避免破坏客户端。

### 2.12 运维端点

| 路径 | 认证 | 响应 |
| --- | --- | --- |
| `GET /api/file/v2/info` | 无需 | `200` JSON `VersionInfo` |
| `GET /api/file/v2/liveness_check` | 无需（`@PermitAll`） | `200` **纯文本** `File service is alive` |
| `GET /api/file/v2/readiness_check` | 无需（`@PermitAll`） | `200` **纯文本** `File service is ready` |

> ★ **纯文本**，不是 JSON。这是最常见的重写错误之一，契约测试专门断言 `Content-Type: text/plain`
> 与响应体逐字节相等。

**`/v2/info` 的扩展字段 `authMode`**（非 OSDU 规范，C8.5）：取值 `jwt` / `remote-entitlements`
/ `disabled`，与 gRPC 的 `InfoResponse.auth_mode` 同源。**为什么必须暴露它**：
`auth.mode=disabled` 的实例与正常实例在行为上只差"是否校验 token"，运维与自动化必须能
**看到**这个状态，否则"忘了开鉴权"就是静默的。

---

## 3. 元数据契约（`dataset--File.Generic`）

### 3.1 必需字段（schema `required` + Bean Validation 合并后的有效集合）

| 字段 | 必需 | 校验 |
| --- | --- | --- |
| `kind` | ✅ | `@NotNull` + 4 段 + `wks` + `dataset--File.Generic` |
| `acl.viewers` | ✅ | 非空数组，元素匹配 ACL 正则 |
| `acl.owners` | ✅ | 同上 |
| `legal.legaltags` | ✅ | `minItems: 1`、元素唯一 |
| `legal.otherRelevantDataCountries` | ✅ | `minItems: 1`、元素唯一 |
| `legal.status` | 服务端设置 | `compliant` / `incompliant` |
| `data` | ✅ | `@NotNull` |
| `data.DatasetProperties` | ✅ | `@NotNull` |
| `data.DatasetProperties.FileSourceInfo` | ✅ | `@NotNull` |
| `data.DatasetProperties.FileSourceInfo.FileSource` | ✅ | **`@NotEmpty`** → `FileSource can not be empty` |
| `data.Name`、`TotalSize`、`EncodingFormatTypeID`、`SchemaFormatTypeID`、`Endian`、`Checksum`、`ExtensionProperties` | ❌ | `Endian` 必须是 `BIG`/`LITTLE` |
| `data.DatasetProperties.FileSourceInfo.{PreloadFilePath, PreloadFileCreateUser, PreloadFileCreateDate, PreloadFileModifyUser, PreloadFileModifyDate, Name, FileSize, EncodingFormatTypeID, Checksum, ChecksumAlgorithm}` | ❌ | 服务端会**覆写** `Checksum`/`ChecksumAlgorithm` |
| `id`、`version` | 服务端设置 | `id` 格式 `"<partition>:dataset--File.Generic:<uuid>"` |
| `ancestry`、`meta`、`tags` | ❌ | — |

### 3.2 字段大小写规则（**不可弄错**）

| 层 | 命名风格 | 例 |
| --- | --- | --- |
| 信封 | lowerCamel / 小写单词 | `id`、`kind`、`acl`、`legal`、`data`、`meta`、`tags`、`version`、`ancestry` |
| `data` 及其子层 | **PascalCase** | `Name`、`Description`、`TotalSize`、`DatasetProperties`、`FileSourceInfo`、`FileSource`、`FileSize`、`Checksum`、`ChecksumAlgorithm`、`Endian`、`ExtensionProperties` |
| ACL/Legal 内部 | 小写单词 | `viewers`、`owners`、`legaltags`、`otherRelevantDataCountries`、`status` |
| REST 响应 | PascalCase 字段名 | `FileID`、`Location`、`SignedURL`、`FileSource`、`SignedUrl`、`Driver`、`Content`、`NumberOfElements` |

> **不存在** `artifact` 字段；**不存在** 小驼峰的 `fileSource` / `fileType`。
> `FileType` 位于 `data.ExtensionProperties.FileContentsDetails.FileType`。

### 3.3 权威黄金样例

**请求体**（`File_CorrectPayload.json`，占位符已具体化）—— 同时作为
`tests/conformance/fixtures/File_CorrectPayload.json`：

```json
{
  "id": "opendes:dataset--File.Generic:33a71a04d20f4240904b4b3fca4657b7",
  "kind": "opendes:wks:dataset--File.Generic:1.0.0",
  "meta": [ { "name": { "id": [ { "kind": "12587932" } ] } } ],
  "tags": { "dataflowId": "dataflowId", "dataflowId-2": "dataflowId" },
  "acl": {
    "viewers": ["data.default.viewers@opendes.example.com"],
    "owners":  ["data.default.owners@opendes.example.com"]
  },
  "legal": {
    "legaltags": ["opendes-storage-tag"],
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

**`GET .../metadata` 响应增量**：以上全部字段 + `"version": 1`。

### 3.4 负向样例矩阵（来自上游验收测试，逐条实现）

**期望消息逐字取自上游**（一手证据，vendored 到 `tests/conformance/fixtures/upstream/`）：

| 项 | 值 |
| --- | --- |
| 来源 | `osdu-file-service`，commit `d7c25c2d7f5d2f42bed901c68a407098195389bb` |
| 请求体 | `testing/file-test-baremetal/src/test/resources/input_payloads/File_*.json` |
| 期望报文 | `.../output_payloads/File_*_msg.json`（取值：`error.errors[].message`） |
| 场景表 | `.../features/IntegrationTest_File_POST.feature`（Examples 表） |
| 许可证 | Apache-2.0 |

| 样例 | 期望状态 | 期望消息（逐字） |
| --- | --- | --- |
| `File_missing_kind.json` | 400 | `kind must not be null` |
| `File_missing_acl.json` | 400 | `acl must not be null` |
| `File_missing_owners.json` | 400 | `Record acl.owners cannot be empty` |
| `File_missing_viewers.json` | 400 | `Record acl.viewers cannot be empty` |
| `File_missing_legal.json` | 400 | `legal tag cannot be empty` |
| `File_missing_data.json` | 400 | `data cannot be empty` |
| `File_missing_fileSource.json` | 400 | `FileSource can not be empty` |
| `File_empty_fileSource.json` | 400 | `FileSource can not be empty` |
| `File_invalid_fileSource.json` | 400 | `Invalid source file path to copy from <FileSource>`（源路径在存储侧不存在） |
| `File_invalid_Endian.json` | 400 | `Invalid value of <值> for Endian`（样例值 `Small`） |
| `File_invalid_ScalarIndicator.json` ⚠️ | 400 | `Invalid value of <值> for ScalarIndicator`（样例值 `Scale`；合法值 `STANDARD`/`NOSCALE`/`OVERRIDE`） |
| `File_Datatype_Mismatch.json` ⚠️ | 400 | 上游期望 `Bad Request. Invalid Input.`；本仓库给更具体的约束消息（见下） |
| `File_Calculate_Checksum.json` | 200 / 201 | 服务端覆写校验和（`FileSourceInfo.Checksum` 与 `data.Checksum` 同步） |

**两条被上游自己禁用的行**（`IntegrationTest_File_POST.feature` 第 24/25 行以 `#` 注释，
即上游从未真正执行）：

- `File_invalid_ScalarIndicator.json`：**本仓库照实现**。上游的枚举定义与消息在源码里是明确的
  （`file-core/.../filedetails/ScalarIndicator.java`、`exception/EnumValidationException.java`），
  我们据此实现并逐字断言 —— 比上游测得更严。
- `File_Datatype_Mismatch.json`：**状态码对齐（400），消息不对齐**。上游期望的是 Jackson
  反序列化失败的通用文案；我们给出的是更具体的约束消息（对排障更有用）。该差异为**显式登记**
  的偏离，不是遗漏。

**校验时机（顺序是可观测的）**：上游把 `Endian` / `ScalarIndicator` 建模为带 `@JsonCreator`
的枚举，非法值在**反序列化**阶段就抛错，早于 Bean Validation。因此本实现把它们放在
`ParseFileMetadataRecord`（解析阶段），而 `@NotNull` / ACL 这类放在 `ValidateMetadataRecord`。
反例说明必要性：`File_invalid_Endian.json` 的 `FileSource` 同时是非法形状（`"string"`），
若枚举校验排在校验阶段之后，客户端拿到的会是 `Invalid source file path to copy from string`。

## 4. gRPC 契约映射

完整定义见 `proto/osdu/file/v1/file_service.proto`。映射表如下
（"扩展"列标注该字段/RPC 是否为本项目新增）。

### 4.1 方法映射

| RPC | REST 等价 | 扩展 |
| --- | --- | --- |
| `GetUploadLocation` | `GET /v2/files/uploadURL`、`POST /v2/getLocation` | 否 |
| `GetFileLocation` | `POST /v2/getFileLocation` | 否 |
| `GetDownloadLocation` | `GET /v2/files/{id}/downloadURL` | 否 |
| `GetFileList` | `POST /v2/getFileList` | 否 |
| `CreateFileMetadata` | `POST /v2/files/metadata` | 否 |
| `GetFileMetadata` | `GET /v2/files/{id}/metadata` | 否 |
| `DeleteFileMetadata` | `DELETE /v2/files/{id}/metadata` | 否 |
| `GetStorageInstructions` | `POST /v2/files/storageInstructions` | 否 |
| `GetRetrievalInstructions` | `POST /v2/files/retrievalInstructions` | 否 |
| `CopyFilesToPersistent` | `POST /v2/files/copy` | 否 |
| `GetFileSignedUrl` | `POST /v2/delivery/GetFileSignedUrl` | 否 |
| `RevokeUrl` | `POST /v2/files/revokeURL` | 否 |
| `GetInfo` | `GET /v2/info` | 否 |
| `Check` | `GET /v2/{liveness,readiness}_check` | 否 |
| `UploadFile` / `DownloadFile` / `ServerSideCopy` | — | ✅ **扩展** |

> **实现状态（P7 切片 3）**：上表**全部 17 个 RPC 已实现**并跑在真实 gRPC 端口上
> （前 14 行是一元 RPC；最后一行的 3 个是本项目**扩展**，见 §4.4）。

### 4.4 扩展 RPC 的语义（本合同之外的**扩展面**，实现即合同）

这 3 个 RPC 没有 REST 等价端点（ADR-001：REST 是唯一合规面，RPC 是平台外扩展），
因此它们的语义由本节钉住（实现、测试、文档同源）。

| RPC | 授权角色 | 语义 | 副作用 |
| --- | --- | --- | --- |
| `UploadFile`（客户端流） | `users.datalake.editors`（同 `GetUploadLocation`） | 首片必须是 `info`（带 `file_source`）；其后是 `chunk`；**最后一个数据分片之后必须发 `end_of_stream = true`**（显式结束标记），再 `WritesDone`。**只允许写到已签发的位置记录**：`info.container`/`key` 非空时只能与记录**一致**（不一致 → `PERMISSION_DENIED`，不接受坐标覆盖，与 `/v1/transfer` 内核同一条防线）。`registerMetadata=true` 时上传完成后复用 `CreateFileMetadata` 的 **12 步**用例（`metadata.data.FileSource` 必须等于上传目标） | 覆盖 staging 对象字节；`registerMetadata=true` 时额外：staging→persistent 复制、写元数据 v1、位置记录迁到 persistent |
| `DownloadFile`（服务端流） | `users.datalake.viewers`（同 `GetDownloadLocation`） | `file_id` 优先，否则用 `file_source`；`offset`/`length` 语义 = HTTP `Range`（`length == 0` = 从 `offset` 到末尾；`offset` 越界 → `INVALID_ARGUMENT`）。数据分片 ≤ 64 KiB，**最后一个分片的 `chunk` 为空**，携带 `totalSize` 与 `checksum` | 无 |
| `ServerSideCopy`（一元） | `service.storage.creator` / `service.storage.admin`（同 `/v2/files/copy`） | 把 `source_file_source` 的对象复制到 `target_file_source` 在 `target_zone`（`UNSPECIFIED` 按 **persistent** 处理）下的对象键；**纯字节原语，不改动任何位置/元数据记录**；目标已存在则覆盖（幂等） | 写入目标对象 |

**为什么把语义写进合同**：这 3 个 RPC 是"实现即合同"的扩展面。不写下来，
"取消算不算成功""`length=0` 是什么意思""复制是否要动记录"就只能靠读代码 ——
而双协议等价性（§6）恰恰要求这些语义**可复述、可测试**。

**取消 / 中断语义（`UploadFile`）**：客户端中途取消或断开时，服务端必须把**不完整的字节流**
当作失败（`UNAVAILABLE`），**不能**当成"正常读完" —— 后者会把截断的对象
rename 成正式对象（静默数据损坏，P7-D07）。失败路径必须清掉 `.tmp.*` 临时文件。

**为什么需要 `end_of_stream` 这个显式标记**：gRPC 同步 API 里 `ServerReader::Read()`
返回 `false` 同时表示"客户端正常半关闭"与"流被中断/取消"，**二者无法区分**；而
`ServerContext::IsCancelled()` 的置位时机比 `Read()` 的失败**更晚**（实测：ASan 构建下
取消时 `Read()==false` 而 `IsCancelled()==false`，于是 512 KiB 的截断对象被 commit ——
P8-D05，普通构建碰不到）。显式标记把这个判定变成**确定性**的：没有标记就报错、绝不提交。
`end_of_stream` 之后不允许再出现数据分片，否则 `INVALID_ARGUMENT`。

### 4.2 消息 ↔ JSON 字段映射（关键项）

| proto 字段 | `json_name` | REST 对应 |
| --- | --- | --- |
| `LocationResponse.file_id` | `FileID` | ✅ |
| `LocationResponse.location.signed_url` | `SignedURL` | ✅ |
| `LocationResponse.location.file_source` | `FileSource` | ✅ |
| `LocationResponse.driver` / `.zone` / `.expires_at` | `Driver` / `Zone` / `expiresAt` | ❌ **扩展字段** |
| `FileListResponse.content[].file_id` | `FileID` | ✅ |
| `FileListResponse.number` / `.number_of_elements` / `.size` | `Number` / `NumberOfElements` / `Size` | ✅ |
| `FileData.*` | PascalCase（`Name`/`TotalSize`/…） | ✅ |
| `FileSourceInfo.file_source` | `FileSource` | ✅ |
| `StorageInstructionsResponse.provider_key` / `.storage_location` | `providerKey` / `storageLocation` | ✅ |
| `StorageZone` / `StorageDriver` / 各扩展 RPC | — | ❌ **扩展** |

### 4.6 审计与事件的操作名（C8.6/C8.7）

审计事件（`IAuditLogger`）与状态事件（`IEventPublisher`）的操作名/状态由下表钉住，
测试逐条比对（`tests/unit/test_audit_coverage.cpp`、`tests/integration/test_metadata_lifecycle.cpp`）。

| 用例 | 审计操作（`operation`） | 状态事件（`status`） |
| --- | --- | --- |
| `GetUploadLocation` | `createLocationSuccess` / `createLocationFailure` | — |
| `GetFileLocation` | `readFileLocationSuccess` / `readFileLocationFailure` | — |
| `GetDownloadLocation` | `createDownloadLocationSuccess` / `…Failure` | — |
| `GetFileList` | `getFileListSuccess` / `…Failure` | — |
| `CreateFileMetadata` | `createMetadataSuccess` / `createMetadataFailure`（另有 `createMetadataStagingCleanupFailure` 诊断项） | **`IN_PROGRESS` → `SUCCESS`**（成功）/ **`IN_PROGRESS` → `FAILED`**（失败） |
| `GetFileMetadata` | `readMetadataSuccess` / `readMetadataFailure` | — |
| `DeleteFileMetadata` | `deleteMetadataSuccess` / `deleteMetadataFailure` | — |
| `GetStorageInstructions` | `getStorageInstructionsSuccess` / `…Failure` | — |
| `GetRetrievalInstructions` | `getRetrievalInstructionsSuccess` / `…Failure` | — |
| `CopyFiles` | `copyFilesSuccess` / `copyFilesFailure` | — |
| `GetFileSignedUrl` | `getFileSignedUrlSuccess` / `…Failure` | — |
| `RevokeUrl` | `revokeUrlSuccess` / `revokeUrlFailure` | — |
| `UploadFile` / `DownloadFile` / `ServerSideCopy`（扩展 RPC） | `uploadFileSuccess` / `downloadFileSuccess` / `serverSideCopySuccess`（及各自 `…Failure`） | 上传 + `registerMetadata=true` 时由 `CreateFileMetadata` 发事件 |

**每个审计事件必须含**：`user`（actor）、`partition`、`object_id`（受影响对象，未知时为空）、
`result`（`success`/`failure`）、`epoch_millis`、**`correlation_id`**（跨服务追踪）。
实现上由 `AuditGuard`（RAII）保证"任何提前 return 都会记账"——手写调用会漏掉失败出口，
而**漏审计是静默的**。

`createLocationSuccess` / `readFileLocationSuccess` 是**上游实测**的操作名；其余按同一命名
约定（`<动作><对象><Success|Failure>`）扩展，属于本项目的补充（已在证据文件登记）。

---

### 4.5 与 Entitlements 的接口（`auth.mode=remote-entitlements`）

本项目对 Entitlements 的**要求**（实现见 `src/infra/auth/remote/remote_entitlements_authorizer.cpp`；
mock 见 `tests/tools/mock_entitlements.py`）：

```
POST {auth.remote_entitlements.base_url}{auth.remote_entitlements.authorize_path}
headers: Authorization: <bearer 原样透传>
         data-partition-id: <请求的 partition>
         content-type: application/json
body:    {"roles": ["service.file.editors", ...]}          // 任一命中即可
200 + {"allowed": true,  "grantedRoles": [...]}            → 放行
200 + {"allowed": false}                                   → 403
401                                                        → 401（凭证不行）
其它状态码 / 超时 / 连不上 / 响应不是 JSON / 缺布尔 allowed → 503（**fail-closed**）
```

**为什么把这条也写进合同**：它是本项目与外部依赖的**接口约定**，不是实现细节。
`authorize_path` 可配置，便于适配真实的 Entitlements 服务；但无论路径如何，
"读不懂或够不着依赖 → 不放行"这一条**不可配置**（`fail_closed=false` 会被配置校验拒绝）。

⚠️ **未与真实 OSDU Entitlements 联调**（本机网络不可达）：响应形状是本项目的约定，
已登记在 `docs/adr/ADR-012-auth-and-tenant-binding.md` §5.3 与 `docs/test-evidence/phase8.md`。

---

### 4.3 gRPC 调用元数据（metadata）

| metadata key | 对应 REST 头 | 必需 |
| --- | --- | --- |
| `authorization` | `Authorization` | ✅ |
| `data-partition-id` | `data-partition-id` | ✅ |
| `correlation-id` | `correlation-id` | ❌ |

> gRPC 的 `authorization` 值同样携带 `Bearer <JWT>` 前缀，与 REST 完全一致，
> 使同一个鉴权中间件可以服务两条链路。

---

## 5. 错误码双向映射（唯一权威表）

| 领域 `ErrorKind` | REST 状态 + `reason` | gRPC `StatusCode` | 备注 |
| --- | --- | --- | --- |
| `kInvalidArgument` | `400 Bad Request` | `INVALID_ARGUMENT` | 校验失败、非法 expiryTime、kind 非法 |
| `kFileSourceEmpty` | `400 Bad Request` | `INVALID_ARGUMENT` | 消息 `FileSource can not be empty` |
| `kInvalidSourcePath` | `400 Bad Request` | `INVALID_ARGUMENT` | 消息 `Invalid source file path to copy from <path>` |
| `kLocationAlreadyExists` | **`400 Bad Request`** | `ALREADY_EXISTS` | ★ 上游**不是** 409 |
| `kUnauthenticated` | `401 Unauthorized` | `UNAUTHENTICATED` | 缺 token / 缺 partition |
| `kPermissionDenied` | `403 Forbidden` | `PERMISSION_DENIED` | **调用方**角色不足（本服务拒绝该用户） |
| `kStorageAccessDenied` | `403 Forbidden` | `PERMISSION_DENIED` | **存储侧**拒绝本服务（凭证错/桶策略不允许；如 S3 `AccessDenied`/`SignatureDoesNotMatch`）—— 对客户端是依赖故障，不是"你没权限" |
| `kNotFound` | `404 Not Found` | `NOT_FOUND` | 记录/位置不存在 |
| `kChecksumMismatch` | `400 Bad Request` | `INVALID_ARGUMENT` | 校验和不符（存储 `put` 的 `expected_checksum`、数据面校验）；`details` 携带期望/实际。**注意**：这不是"客户端在 metadata 里填了错校验和"——那是被**覆写**的（§2.6 第 7 步） |
| `kUnimplemented` | `501 Not Implemented` | `UNIMPLEMENTED` | 扩展端点未启用时 |
| `kInternal` | `500 Internal Server Error` | `INTERNAL` | — |
| `kBadGateway` | `502 Bad Gateway` | `UNAVAILABLE` | 依赖服务异常 |
| `kUnavailable` | `503 Service Unavailable` | `UNAVAILABLE` | 存储后端不可用 |

**等价性强制**：`tests/conformance/test_error_equivalence.cpp` 对每个 `ErrorKind`
分别经 REST 与 gRPC 触发一次，断言 `(REST status, reason)` 与 `gRPC status`
都落在本表的同一行。新增 `ErrorKind` 必须同时更新本表与测试，否则测试失败。

---

## 6. 双协议等价性矩阵（阶段 7 的门槛）

对每个操作，用**同一份输入**分别经 REST 与 gRPC 调用，断言下表中的每一项相等。

| 操作 | 领域结果 | 错误分类 | 存储副作用 | 位置记录 | 元数据记录 |
| --- | --- | --- | --- | --- | --- |
| `GetUploadLocation` | `{file_id, file_source, driver, zone}` | ✅ | 创建空对象 | 新增 1 条 | 不变 |
| `GetFileLocation` | `{driver, location}` | ✅ | 无 | 不变 | 不变 |
| `GetDownloadLocation` | `signed_url`（**内容可不同**，见下） | ✅ | 无 | 不变 | 不变 |
| `GetFileList` | 分页结果集合与计数 | ✅ | 无 | 不变 | 不变 |
| `CreateFileMetadata` | `id` 前缀与格式 | ✅ | staging→persistent 复制 | 更新 zone | 新增 v1 |
| `GetFileMetadata` | 全字段 + `version` | ✅ | 无 | 不变 | 不变 |
| `DeleteFileMetadata` | 无 | ✅ | 删除 persistent 对象 | 标记删除 | 标记删除 |
| `GetStorageInstructions` | `providerKey`, `storageLocation` 键集合 | ✅ | 创建空对象 | 新增 1 条 | 不变 |
| `GetRetrievalInstructions` | `datasets[].datasetRegistryId` 集合 | ✅ | 无 | 不变 | 不变 |
| `CopyFilesToPersistent` | `[].success` | ✅ | 复制 | zone 更新 | 不变 |
| `GetFileSignedUrl` | `processed` 的键集合 / `unprocessed` | ✅ | 无 | 不变 | 不变 |
| `RevokeUrl` | 无（204） | ✅ | 无 | 不变 | 不变 |

**关于签名 URL 的等价性判定（重要）**：
REST 与 RPC 两次调用会生成**不同的**签名 URL（含不同时间戳/nonce；自签形态的
`/v1/transfer/<token>` 路径里内嵌的是**密文**，逐字节必然不同），因此"等价"的判据是
**语义等价而非字面相等**。按签名所在位置分两种形态：

```
① 通用形态（签名在 query 里，如 S3 预签名）：
   等价 ⟺  scheme 相同 ∧ host 相同 ∧ path 相同 ∧
           {query 参数名集合} 相同 ∧
           过期时间相差 ≤ 5 秒

② 自签传输形态（path 形如 /v1/transfer/<token>?exp=…&sig=…）：
   等价 ⟺  scheme 相同 ∧ host 相同 ∧
           path 的 `/v1/transfer/` 前缀相同 ∧
           {query 参数名集合} 相同 ∧
           过期时间相差 ≤ 5 秒 ∧
           两侧 token 都能用同一密钥**验签并解码** ∧
           解码后的声明逐项相等：
             partition ∧ file_id ∧ container ∧ object_key ∧ zone ∧ op
```

② 之所以不是"path 相同"，是因为自签 token 每次签发都带新 nonce，
字面相等**永远不可能成立**——用字面比较会把"实现正确"判成失败。
但也不能退化成只比"都返回 200"：必须解码到**同一条位置记录**（`object_key` + `zone` + `op`）
才算等价。

该判据由 `tests/conformance/test_protocol_equivalence.cpp` 中的
`SignedUrlEquivalent(a, b)` 实现，并有**反向测试**（故意改一个 query 参数名、
或换一条位置记录 → 判定必须为不等）。

**扩展 RPC 与 REST 数据面的等价性（不在矩阵的 12 行里，但同样机械检查）**：
`UploadFile`/`DownloadFile` 对应 REST 的 `/v1/transfer` 数据面，二者覆盖**同一份对象**，
因此用"内容一致"而不是"响应字段一致"来判定：

| 对照 | 判据 | 证据 |
| --- | --- | --- |
| `UploadFile` ↔ `PUT /v1/transfer/{token}` | 同一批字节写入后，`stat` 的大小与 SHA-256 相同 | `test_grpc_streaming` 的 1 GiB 用例（客户端独立算摘要） |
| `DownloadFile` ↔ `GET /v1/transfer/{token}` + `Range` | **相同 `offset`/`length`** 下字节完全一致（SHA-256），且两者都**不整文件读取**（在 `IBlobStore::get` 边界上断言区间） | `test_grpc_streaming` 的 C7.10 用例（含"整块读回再切片"的注入对照） |
| 权限 | 扩展 RPC 的角色要求与同语义 REST 端点一致（见 §4.4） | `test_roles` + `test_grpc_streaming` 的授权锚点用例 |

---

## 7. 非规范扩展清单（必须与 OSDU 命名空间隔离）

| 扩展 | 位置 | 隔离方式 | 默认 |
| --- | --- | --- | --- |
| gRPC 服务 | 独立端口（组合根用 `FSS_GRPC_PORT` 配置；`-1` = 系统分配） | 不同端口 | **默认关闭**（`FSS_GRPC_PORT=0`）：RPC 是平台外扩展，按需开启 |
| 集中存储字节通道 | `/api/file/v1/transfer/{token}` | `/v1` 段（OSDU v1 已废弃，无冲突） | 仅 POSIX 驱动启用 |
| 指标端点 | `/metrics` | 不在 `/api/file` 下 | 启用（可关） |
| 错误格式开关 | `http.error_format` | 配置驱动，默认取规范值 | `apperror` |
| 驱动上报覆盖 | `storage.driver_report_override` | 配置驱动，默认读真实驱动 | 关闭 |
| providerKey 覆盖 | `storage.provider_key_override` | 配置驱动 | 关闭 |
| 健康/就绪的依赖明细 | `X-FSS-Dependencies` 响应头 | 响应头（不影响 body） | 启用 |

**原则**：任何扩展都**不得**修改 OSDU 端点的既有状态码、字段名或字段语义。
扩展只能以"新端口 / 新路径段 / 新响应头 / 配置开关"的形式存在。

---

## 8. 契约测试清单（实现与测试的对应关系）

| 测试文件 | 覆盖内容 | 阶段 |
| --- | --- | --- |
| `tests/unit/test_json_codec.cpp` | PascalCase/camelCase 序列化、未知字段保留、往返一致 | 1 |
| `tests/unit/test_bytes.cpp` | §1.8 全部 `Range` 分支（含越界 416、非法忽略、多段合并、上限） | 1 |
| `tests/unit/test_expiry_policy.cpp` | `expiryTime` 全部分支：合法/缺省 1H/超限截断 7D/非法 400 + 固定消息 | 2 |
| `tests/unit/test_object_key_policy.cpp` | `FileSource` 构造（前导斜杠、日期分层、1024 上限）、`^[\w,\s-]+(\.\w+)?$` 校验 | 2 |
| `tests/unit/test_kind_validator.cpp` | kind 正则 + 4 段 + `wks` + `dataset--File.Generic` + 三条固定消息 | 2 |
| `tests/conformance/test_metadata_payloads.cpp` | §3.3 黄金样例 + §3.4 全部负向样例 | 6 |
| `tests/conformance/test_rest_contract.cpp` | §2 全部端点的路径/方法/状态码/字段名（对真实 HTTP 端口） | 4 |
| `tests/conformance/test_error_formats.cpp` | 三种错误体形态 + §5 映射表 | 4 |
| `tests/conformance/test_error_equivalence.cpp` | §5 每个 `ErrorKind` 的双协议一致 | 7 |
| `tests/conformance/test_protocol_equivalence.cpp` | §6 等价性矩阵 + `SignedUrlEquivalent` | 7 |
| `tests/conformance/test_ops_endpoints.cpp` | 纯文本健康检查、`/v2/info` 字段、免鉴权行为 | 4 |
| `tests/integration/test_upload_flow_posix.cpp` | 端到端：uploadURL→PUT→metadata→downloadURL→GET→delete | 4 |
| `tests/integration/test_upload_flow_s3.cpp` | 同上，S3 驱动 + mock-S3 独立验签 | 5 |

**门槛规则**：阶段 N 的测试失败 → 不得进入阶段 N+1。
阶段测试证据（命令、输出摘要、结论）归档到 `docs/test-evidence/phaseN.md`。
