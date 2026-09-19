# ADR-013：本服务的**平台外扩展端点**约定 —— 远端 legal / schema 校验器（为后续 webhook 立同一套规矩）

- 状态：**已采纳（Accepted）**（P10 切片 6a 落地；webhook 同类扩展见 §9；未验证项见 §5.3 与 §9.4）
- 日期：2026-09
- 相关：`docs/01-osdu-research.md:110`（上游不调用 Legal/Schema）、`docs/03-api-contract.md` §7（扩展清单）、
  `docs/operations.md` §1.2.11（7 个键的三态）、`docs/02-design.md` §L2（模块布局）与 §17（索引）；
  `src/infra/legal/remote_legal_validator.{h,cpp}`、`src/infra/schema/remote_schema_validator.{h,cpp}`；
  先例：[ADR-001](ADR-001-rpc-as-extension.md)（gRPC 扩展面）、[ADR-012](ADR-012-auth-and-tenant-binding.md)（远端依赖 fail-closed）

---

## 1. 结论

**legal / schema 的远端校验以"本服务自定义端点 + fail-closed"落地；端点 URL 由运维在
`legal.remote.base_url` / `schema.remote.base_url` 里给出完整地址（POST 到它，不追加任何路径）。**

三条不可协商的规则：

1. **这是扩展，不是 OSDU 接口**：上游 File Service **不直接调用** Legal/Schema 服务 ——
   `docs/01-osdu-research.md:110` 的一手结论是"legal tag 的合规性由 Storage Service 的
   PUT /records 内部校验"。因此本端点与 `/v1/transfer`、gRPC 同属**平台外扩展**（ADR-001），
   必须按契约 §7 的方式与 OSDU 命名空间隔离：**新配置键 + 新出站依赖**，不改任何 OSDU 端点
   的状态码、字段名或字段语义。
2. **fail-closed 是唯一允许的失败方向**：连接失败 / 超时 / 非 200（含 401/403/500）/
   非 JSON / 缺 `valid` 字段 / `valid` 不是 bool → 一律 `kUnavailable`（HTTP **503**）。
   **绝不**把"依赖故障"当成"校验通过"，也**绝不**把它当成"校验不通过（400）"——
   前者是安全缺陷，后者会把运维的排障引向错误方向。
3. **`base_url` 就是完整端点**：配置里**没有** path 键。宁可把完整 URL 的控制权交给运维，
   也不要在代码里发明一个路径（§3）。

---

## 2. 线协议（本项目的约定）

```
legal  —— POST <legal.remote.base_url>            Content-Type: application/json
          {"partition": "<partition>", "legaltags": ["<tag>", ...]}
schema —— POST <schema.remote.base_url>           Content-Type: application/json
          {"kind": "<kind>", "record": { ...完整记录 JSON... }}

200 + {"valid": true}                     → 通过
200 + {"valid": false, "message": "<原因>"} → **不通过** → kInvalidArgument（HTTP 400），
                                            消息里带上远端给的 message
其余一切（连不上 / 超时 / 非 200 / 非 JSON / 缺 valid / valid 非 bool）
                                          → kUnavailable（HTTP 503，fail-closed）
```

**为什么不是"base_url + path"（与 `auth.remote_entitlements` 不同）**：
Entitlements 是**上游真实存在的服务**，它的路径（`/api/entitlements/v2/authorizeAny`）有
一手依据，所以那里既有 `base_url` 也有 `authorize_path`（并允许运维适配真实服务）。
legal/schema 的路径**没有**任何上游依据 —— 在这种情况下再造一个 `*_path` 键，等于把
一个**我们编出来的路径**伪装成"可配置的约定"，运维还得先猜默认值长什么样。
把完整 URL 交给运维是更诚实的选择：**协议形状（body/响应）由本 ADR 定义，路由由部署决定。**

**调用方身份不进线协议**：端口签名是 `Validate(partition, tags)` / `Validate(kind, record)`
（`src/domain/ports/ports.h`），**没有** bearer token 参数，因此适配器**不透传**调用方凭证。
后果（必须写进运维前提）：**端点必须允许无 per-request 认证的访问**（集群内网 /
mTLS 终结 / 网络策略白名单）。若将来需要在协议里带上身份，那是**新增端口参数**的改契约动作，
不是本 ADR 的实现细节。

---

## 3. 为什么现在做（而不是继续拒绝启动）

* `legal.validator=remote` / `schema.validator=remote` 此前是"未实现 → exit 78"。
  拒绝启动把"未实现"诚实地表达出来了，但**也把能力锁死了**：需要合规校验的部署只能
  在网关侧另做一个服务，本服务无法参与。
* 成本是**有界的**：一个 L2 适配器 + 组合根选择器，形状与已在生产路径上的
  `RemoteEntitlementsAuthorizer`（P8）**逐条对齐**（`CURLOPT_NOSIGNAL` /
  `FOLLOWLOCATION=0` / 连接超时 + 整体超时 / `WriteToString` / 状态码判定 /
  所有依赖故障 → `kUnavailable` / `Ready()` + `NotReadyReason()` 供组合根启动自检）。
* 收益是**可证伪的**：11 条真实进程用例（`tests/integration/test_remote_validators.cpp`）
  把正例、400、五态 503、无残留、启动拒绝、noop 不发请求、`timeout_ms` 真生效全部钉住。

---

## 4. 备选方案（≥3，含未实测项）

| # | 方案 | 优点 | 代价 / 不选的理由 | 实测？ |
| --- | --- | --- | --- | --- |
| 1 | **不实现远端校验，保持 `noop`**（`validator=remote` 继续拒绝启动） | 零出站依赖、零新面；把合规判断留在调用方/网关 | 本服务无法参与合规判定；运维必须在别处再写一个服务与一套配置；"拒绝启动"只是把问题推迟到部署期 | ✅ 已实测：这就是接线前的状态，也是本切片的**默认路径**（`noop` 用例证明它不发任何请求） |
| 2 | **复用 Storage Service 的记录写入校验**（把校验交给上游 `PUT /records`） | 与上游语义同源，不需要新协议 | 本服务**不经过** Storage Service 写记录（内置 SQLite / 可选远端仓储），这条路径在我们的部署形态里**根本不存在**；而且校验失败会发生在"对象已经搬完"之后，回滚语义完全不同 | ⬜ **未实测**：我们的部署里没有 Storage Service 的 records 写入面（ADR-004 把它列为可选、未交付） |
| 3 | **本服务自定义端点 + fail-closed**（**采纳**） | 校验点回到**持久化之前**（用例第 3c 步），失败不留残留；协议由我们定义、可被 mock 精确注入故障；与 P8 的远端 Entitlements 同一套纪律与实现形状 | 引入一个新的出站依赖与一个新协议；**未与真实 Legal/Schema 服务联调**；不透传调用方身份 → 端点必须允许无 per-request 认证 | ✅ 已实测：`tests/integration/test_remote_validators.cpp`（真实二进制 + mock） |
| 4 | **在网关侧做校验，本服务只信任头部/令牌** | 零新依赖；与 OSDU 常见部署形态一致 | 与 ADR-012 拒绝过的"假设前面一定有可信网关"是同一个错误（R8：不要在部署假设上做设计）；单实例/直连形态下等于**没有校验** | ⬜ 未实测（也不需要：这是"不设防"的变体） |

**没有第 5 个候选就下结论**的诱惑在于"上游就是 Storage Service 校验的" —— 但那正是 R7
警告的外推：**上游的调用链不等于本项目的调用链**。方案 2 在我们的仓储选择下不可用，
方案 1 把问题推给部署期，方案 4 回到部署假设。选方案 3 并如实登记它的代价。

---

## 5. 决策细节

### 5.1 失败方向的映射（逐条，全部有测试）

| 依赖侧事实 | 本服务的判定 | HTTP | 理由 |
| --- | --- | --- | --- |
| `legaltags` 为空（本地） | `kInvalidArgument` | **400** | 与 `NoopLegalValidator` 逐字同义，且**不发起请求**（空集合没有可校验的内容） |
| `200 + {"valid": true}` | 通过 | 201（建记录） | — |
| `200 + {"valid": false, "message": M}` | `kInvalidArgument`，消息 = `M` | **400** | 远端**明确**回答了"不通过"；`M` 原样透传，便于调用方归因 |
| 连接失败 / DNS / TLS | `kUnavailable` | **503** | 不知道远端是否校验过 → 绝不放行 |
| 超时（`timeout_ms` / 连接超时） | `kUnavailable` | **503** | 同上 |
| 非 200（含 401/403/500，以及 3xx —— `FOLLOWLOCATION=0`） | `kUnavailable` | **503** | 同上；**特别地**：401/403 也不映射成 401/403（这是**服务端**的凭证问题，不是调用方的） |
| 200 但响应不是 JSON 对象 | `kUnavailable` | **503** | 读不懂依赖的回答 = 依赖故障 |
| 200 + 合法 JSON 但缺 `valid` / `valid` 不是 bool | `kUnavailable` | **503** | 同上（**不**默认成 true 或 false） |
| `base_url` 为空且选择器为 `remote` | 组合根 `Ready()`/`NotReadyReason()` | **exit 78** | 起来之后"每次校验都 503"只会制造误导性的排障路径（与 P8 一致） |

**校验发生在持久化之前**：用例序列（契约 §2.6）的第 **3c** 步，远在复制到 persistent（第 6 步）
与写元数据（第 9 步）之前。因此"依赖故障 → 503"的可证副作用是
**staging 对象仍在原位、persistent 侧没有对应文件**（测试断言这两个事实，而不是只看状态码）。

### 5.2 与既有约定的关系

* **`noop` 是默认，且行为逐字不变**：`noop` 只保留本地空值防线，**不发起任何请求**。
  测试用两条独立证据钉住这一点：mock 在窗口内 `requests == 0`；`*.remote.*` 指向一个
  **未监听**端口时仍然 201。
* **`expiry.*` / `self_signed.*` 等语义完全不受影响**：本 ADR 不改任何既有键的语义。
* **R12**：具体实现（`RemoteLegalValidator` / `RemoteSchemaValidator`）只在组合根
  `src/main/server_main.cpp` 创建；用例层只见 `ILegalValidator` / `ISchemaValidator` 端口。
  `tests/unit/test_composition_root_guard.cpp` 的清单已同步收录这两个类型。
* **R15**：线协议不是"文档里的一句话" —— mock 侧断言了**请求体形状**
  （legal 有 `partition`/`legaltags` 且**不含** `record`；schema 有 `kind`/`record`
  且**不含** `partition`/`legaltags`）。

### 5.3 本轮未实现 / 未验证（如实登记，不静默降级）

1. **未与真实 Legal / Schema 服务联调**（本环境没有该服务，外网受限）。协议形状是本项目
   与运维方的约定；`base_url` 由运维给出正是为了适配真实服务的实际路由。
2. **不透传调用方身份**（端口签名没有 token 参数）。端点必须允许无 per-request 认证访问。
   若真实服务要求鉴权，需要**改端口契约**（新增参数）并同步契约 §5/§6 与双协议适配层 ——
   本切片不做，也不假装做了。
3. **不缓存校验结果**：每个建记录请求一次远端调用。缓存会引入"撤权/撤 tag 延迟"，
   而正确性优先于 RTT；需要时按需再加（与 ADR-012 §5.1 对 Entitlements 的取舍同源）。
4. **没有重试/退避**：失败即 503，由调用方决定是否重试。自动重试会把"依赖降级"
   伪装成"只是慢"，与 fail-closed 的初衷冲突。
5. **`connect_timeout_ms` 未暴露为配置键**：目前固定 1000ms（与 Entitlements 的键不同）。
   需要时再加键（要同步 `config/fss.example.json` + `operations.md` + 自动比对测试）。
6. **webhook（`events.publisher=webhook`）已落地，但方向相反**（见 **§9**）：它复用 §2 的
   「完整 URL + 不透传身份」两条规矩，**不**照抄 §5.1 的 fail-closed 矩阵 —— 事件发布是
   **非致命**的（连不上/超时/非 2xx 只记告警，请求照常 201）。**异步有界发布队列 /
   重试退避 / 投递保证**仍未交付（§9.4）。

---

## 6. 后果

* **正面**：合规校验点回到持久化之前；失败方向安全；协议与实现形状与 P8 同源，
  维护者只需要理解一套模式；运维通过两个 URL + 两个超时即可接入任意校验服务。
* **负面 / 风险**：多一个有界出站依赖（每次建记录一次 RTT）；运维必须保证端点可达且
  允许匿名访问（否则所有建记录请求 503）；协议形状若与真实服务不一致，需要一次性对齐
  （`base_url` 让它至少不用改代码）。
* **不改变**：OSDU 端点的任何状态码/字段语义；`noop` 默认路径；现有 19 条"拒绝启动"里
  与本决策无关的那些。

---

## 7. 门槛（新增/衔接）

| 判据 | 内容 |
| --- | --- |
| **C10.18** | **6 个键**（`legal.validator`、`legal.remote.{base_url,timeout_ms}`、`schema.validator`、`schema.remote.{base_url,timeout_ms}`）接通为**生效**（其中 2 个此前是「拒绝启动」、4 个是「已读但无效果」）；真实二进制 + 可控 mock 上验证：正例 201、远端不通过 400（带 message）、**fail-closed 五态一律 503 且无残留**、`base_url` 为空 → exit 78、`noop` 不发请求、`timeout_ms` 真的来自配置；R1 自证：fail-open / "缺 valid 也算 true" / 忽略 timeout 三种错误实现都必须让对应用例失败 |
| **C10.18 伴随更正** | `auth.remote_entitlements.fail_closed`：从「已读但无效果」更正为「拒绝启动（触发条件）」（`remote-entitlements` + 非 `true` → exit 78；**模式相关**）。**独立理由**，与上面 6 个键的迁移是两件事 |

证据：`docs/test-evidence/phase10.md` §11（其中写明**规格勘误**：初版把键数写成 7，实际 6）。

---

## 8. 待办（不阻塞本轮判据）

* 与真实 Legal / Schema 服务联调（拿到真实响应形状后回填 §2 的差异）。
* 若真实服务要求鉴权：**改端口契约**带上传入身份，并同步契约 §5/§6、HTTP/gRPC 两个适配器
  与等价性矩阵。
* 若接入点的延迟成为瓶颈：评估"按 (partition, tags) 的有界 TTL 缓存"，并**先定义**
  撤权延迟的可接受上界（不要默认缓存）。
* webhook 发布器**已落地**（见 **§9**）：复用 §2 的「完整 URL + 不透传身份」，
  但**失败方向相反**（非致命）；异步有界队列/重试退避/投递保证仍未交付（§9.4）。

---

## 9. 事件 webhook（同类扩展；切片 6b / C10.19）

§2 的三条约定（**完整 URL**、不透传身份、协议形状由本 ADR 定义）在 webhook 上**继续适用**，
但**失败方向相反**：事件发布是**非致命**的。本节是它的定案。

### 9.1 线协议

```
POST <events.webhook.url>              Content-Type: application/json
                                       （url 就是完整端点，不追加路径）

statusChanged  : {"topic":T,"kind":"statusChanged",
                  "body":{"recordId","partition","status","datasetSync","version"}}
datasetDetails : {"topic":T,"kind":"datasetDetails",
                  "body":[{"properties":{"correlationId","datasetId","datasetType":"FILE",
                          "datasetVersionId","recordCount":1,"timestamp"}}]}

2xx            → 成功
其余一切（非 2xx / 连不上 / 超时 / 坏响应）→ **非致命**：记一条可读 Warn 并继续
```

* `T` 取**配置值** `events.webhook.topic`（两个事件都用它）。
* `datasetDetails` 的 `body` 是**长度为 1 的数组**（对齐上游 `FileDatasetDetailsPublisher.java`）。
* 超时 = `events.webhook.timeout_ms`（整体）+ 连接超时 `min(1000, timeout_ms)`。
* `statusChanged.body.recordId` 在记录 id 已知时带真实 id（第 1 步 `IN_PROGRESS` 在建记录之前 → 空）。

**为什么是"完整 URL"**：与 §2 同源 —— 上游**没有** webhook 路径依据，不发明 `*_path` 键。

**为什么"非致命"（一手依据）**：`docs/03-api-contract.md` §2.6 第 4/10 步 +
`src/domain/ports/ports.h` —— 上游只 `log.warning("Failed to publish ...")`。因此即使 webhook
完全不可达，`createMetadata` 仍必须 **201**、记录仍必须**真的建出来**。用例层保持
`(void)ports.events.Publish...`（**不得**改成 `FSS_TRY`）。这与 §5.1 的 fail-closed 矩阵
**方向相反**，两条语义各自被测（`test_webhook_publisher.cpp` vs `test_remote_validators.cpp`）。

### 9.2 三态选择器

| `events.publisher` | 行为 |
| --- | --- |
| `log`（默认） | 既有 `LogEventPublisher`（写日志，**不发请求**），行为逐字不变 |
| `webhook` | 本节的 POST；`url` 为空 → **exit 78**（`Ready()` / `NotReadyReason()`） |
| `none` | 组合根内联 `NoopEventPublisher` —— **显式关闭**（不发请求），不是"没实现" |

### 9.3 备选方案（≥3）

| # | 方案 | 优点 | 代价 / 不选的理由 | 实测？ |
| --- | --- | --- | --- | --- |
| 1 | 保持 `webhook`/`none` **拒绝启动**（不实现） | 零出站依赖 | 能力锁死；需要事件通知的部署只能在别处再写消费者与配置；"拒绝启动"只是把问题推到部署期 | ✅ 已实测：这就是接线前的状态 |
| 2 | **复用真实消息总线客户端**（Kafka 类） | 与上游形态一致，天然异步 | 引入重依赖与 broker/序列化契约；本环境**没有** broker，无法联调，等于交付未验证的东西 | ⬜ **未实测**（无 broker；不引入新依赖） |
| 3 | **本服务自定义 webhook + 非致命**（**采纳**） | 零新依赖；协议可被可控 mock 精确注入故障；失败方向与上游一致（非致命） | **同步发布**增加请求延迟；异步队列/重试/**投递保证未交付** | ✅ 已实测：`tests/integration/test_webhook_publisher.cpp`（真实二进制 + mock） |
| 4 | 在网关/边车侧做事件转发 | 本服务零出站依赖 | 需要本服务把事件交给边车（又回到"消息总线/队列"）；且与 ADR-012 拒绝过的"假设前面一定有可信组件"同族（R8） | ⬜ 未实测（也不需要：它只是把方案 2 的依赖挪走） |

选方案 3 并如实登记它的代价（同步延迟、无投递保证），不假装它是消息总线。

### 9.4 未实现 / 未验证（如实登记，不静默降级）

1. **异步有界发布队列未交付**：本实现是**内联同步** POST。一次 `createMetadata` 发 2~3 个事件，
   一个慢 webhook 最多给请求路径增加 **事件数 × `events.webhook.timeout_ms`**。要交付必须先定义
   「队列上界 + 丢弃策略 + 退避参数」并新增配置键（同步 `config/fss.example.json`、
   `docs/operations.md`、`test_operations_doc`）。
2. **无重试 / 退避、无投递保证**：单次尝试，失败即丢弃（只留一条告警）。上游消息总线的
   at-least-once 语义**没有**被复现。
3. **未与真实消息总线 / 中间件联调**（本环境没有）：协议形状是本项目与运维方的约定。
4. **不透传调用方身份**（与 §2 同源）：载荷里没有 bearer/tenant 凭证；端点须允许
   无 per-request 认证访问。这是端口签名决定的，不代表身份问题已被解决。
5. `connect_timeout_ms` **未暴露为配置键**（固定 `min(1000, timeout_ms)`）。

### 9.5 门槛（新增）

| 判据 | 内容 |
| --- | --- |
| **C10.19** | **4 个键**（`events.publisher` 来自「拒绝启动」；`events.webhook.{url,timeout_ms,topic}` 来自「已读但无效果」）接通为**生效**；真实二进制 + 可控 mock 验证：正例 201（`statusChanged` 的 `IN_PROGRESS`/`SUCCESS` 与 `datasetDetails` 都收到、`topic` 来自**配置**、字段逐个断言、datasetDetails 的 `body` 是长度 1 的数组）、**非致命三态**（连不上 / 非 2xx / 超时 → 请求仍 **201** 且 persistent 侧**有文件**）、`none` 一个请求都不发、`log` 不受影响、空 `url` → **exit 78**、`timeout_ms` 真的来自配置；R1 自证：①把发布失败改**致命**、②忽略 `events.webhook.topic`、③让 `none` 仍然发布 —— 三种错误实现都必须让对应用例**失败** |

证据：`docs/test-evidence/phase10.md` §12。
