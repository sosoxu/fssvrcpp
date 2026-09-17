# 阶段 6 测试证据（进行中）

| 项 | 值 |
| --- | --- |
| 阶段 | P6（元数据记录语义完整化） |
| 状态 | 🚧 **进行中 —— 切片 6/7 完成**（1：仓储；2：校验和 C6.4；3：12 步序列 C6.3；4：`getFileList` C6.6 + 角色 C6.8；5：DMS/Delivery C6.7；6：大文件 C6.9 + 幂等并发 C6.11 + tmp 名 C6.13） |
| 门槛命令 | `ctest -L phase6` |
| 退出码 | `0`（8 测试 / 2447 断言） |

> 剩余：
> GC 租约与幂等并发与 tmp 名唯一性（C6.11/C6.12/C6.13）、DMS/Delivery 语义收口（C6.7）、
> **1 GiB** 大文件搬迁 RSS（C6.9；64 MiB 流式已证，绝对上限 < 64 MiB 尚未测）、
> 远端 Storage Service 仓储、组合根改接 `SqliteMetadataRepository`。

---

## 1. 切片 1：SQLite 元数据仓储（C2.10 元数据侧 / C6.5）

| 路径 | 内容 |
| --- | --- |
| `src/infra/metadata/sqlite/sqlite_metadata_repository.{h,cpp}` | `(partition_id, id, version)` 主键、`is_latest` / `previous_version` 列、`data` 列存整条记录 JSON（往返无损）、`kind`/`name`/`created_at` 抽出列 |
| `tests/integration/test_sqlite_metadata_repository.cpp` | **C2.10 元数据侧**：与内存实现共用 `CheckMetadataRepositoryContract`；另加"版本链在数据库层面的真实形态"用例 |

### 三条数据库层面的不变量（都有可执行断言）

| 不变量 | 实现 | 断言 |
| --- | --- | --- |
| 幂等键 `(partition, file_source)` 唯一 | `ux_metadata_source ... WHERE is_latest = 1` | 契约"Create 幂等"用例：第二次 `Create` 返回**第一次那条**，且 `GetById(second.id)` → `kNotFound`、`List.total == 1` |
| 每个 id 只有一个最新版 | `ux_metadata_latest ... WHERE is_latest = 1` | 契约"版本链"用例 + **自证对照**：直接往库里插第二条 `is_latest=1` 必须被唯一约束拒绝（若索引缺失，这条会通过 —— 那样"只有一个 latest"就只是巧合） |
| `Update` 不得改写幂等键 | 用例层显式比较 `file_source` | 契约"Update 不得改写 (partition,file_source)"用例 |

> **R6 的落点**：两个唯一索引的谓词都含 `is_latest = 1`。去掉谓词会**阻断合法版本链**
> （同一 `file_source` 的后续版本会被唯一约束拒掉）—— 这正是 ADR-008/R6 记录过的那个坑。

### 一处刻意的实现选择

`List` 的过滤（`kind` / `name_prefix` / 时间区间）在 **C++ 侧**做，SQL 只负责"按 partition 取 latest 行"。
原因：`name_prefix` 需要读 JSON 里的 `data.Name`，而 SQLite 的 JSON1 扩展在 3.37 上**不保证编译进来**
（3.38 起才默认启用）。分区内记录数在本阶段可控，因此选择"先隔离、后过滤"，并把 `name` 单独抽成列
以便将来把过滤下推到 SQL。**这是有意的取舍，不是遗漏**。

---

## 2. 切片 2：校验和（C6.4）

契约依据：`docs/03-api-contract.md` §2.6 第 7 步与 §3.4 的 `File_Calculate_Checksum`。

> ⚠️ **本切片推翻了一句既有结论**（P6-D05）：计划里写的「客户端提供但不符 → `400` + 对象被删除」
> 与上游证据冲突，已按 **`docs/00-final-design.md` §5** 更正为「**服务端无条件覆写**客户端传入的
> `Checksum`/`ChecksumAlgorithm`」。证据：① 调研 §2.3「校验和：**服务端覆写**客户端传入的
> `Checksum`/`ChecksumAlgorithm`（至少 Azure 实现如此）」，第 7 步失败语义为 `—`（非致命）；
> ② 权威样例 `tests/conformance/fixtures/upstream/File_CorrectPayload.json` 客户端给的是
> `MD5("") = d41d8cd9…` 却声明 `ChecksumAlgorithm: "SHA-256"`，**期望响应 201**。
> 详见 §5 的 P6-D05。

### 交付物

| 路径 | 内容 |
| --- | --- |
| `src/common/crypto/crypto.{h,cpp}` | `ChecksumAlgorithm{kSha256,kSha1,kMd5}` + `ParseChecksumAlgorithm` / `CanonicalChecksumName` / `ChecksumHexLength` / `IsHexDigestOf` + **增量** `Hasher`（OpenSSL EVP） |
| `src/common/bytes/bytes.h` | `BufferSink`：写进调用方固定缓冲（"推式 → 拉式"的 L1 通用件），`BlobByteSource` 改为复用 |
| `src/app/usecases/usecases.cpp` | `StoreByteSource`（可定位读的存储字节源）+ `HashingSink` + `ComputeChecksumStreaming`；**跨 store 复制改为流式**；第 7 步改为"原生优先 / 不可用则流式回算 / **覆写两处**" |
| `tests/integration/test_metadata_lifecycle.cpp` | 6 用例 / 230 断言（覆写语义 + MD5 驱动 + ETAG 回退 + 真实 POSIX 栈的 RSS 上限与自证对照） |
| `tests/unit/test_checksum.cpp` | 6 用例 / 106 断言：算法名解析（接受/拒绝两向）、规范名与 hex 长度自洽、hex 结构校验、**RFC/FIPS 公开向量**（SHA-256/SHA-1/MD5，含"一百万个 `a`"）、`HexDigest` 取完即重置 |
| `tests/framework/fake_ports.h` | 能力替身新增两个**驱动替身**开关：`hide_checksum`（模拟驱动不报校验和）、`copy_checksum_override/_algorithm`（模拟 Azure 那样返回 MD5） |
| `scripts/run_all_gates.sh`（前置检查） | 注入残留机械防线（P6-D04，见 §5） |

### 判定表（每一行都有断言）

| 情形 | 期望行为 | 测试 |
| --- | --- | --- |
| 客户端**未提供** `Checksum` | 服务端计算 **SHA-256**，覆写 `data.Checksum`/`data.ChecksumAlgorithm` 与 `FileSourceInfo.*` **两处** | `客户端未提供校验和 → 服务端计算 SHA-256 并写回两处` |
| 客户端提供了**错值且不自洽**的值（与 golden 样例同形：`MD5("")` + 声明 `SHA-256`） | **仍然 `201`**；记录里是服务端算出的真实 SHA-256（自证对照 R1：若原样回传客户端值则断言失败）；persistent 对象**不删除**；幂等键被占用（再提交返回同一条 id） | `客户端提供的校验和被**覆写**（不是待校验的断言）` |
| 客户端按**自己声明的算法**给了**正确**值（`SHA-256` / `MD5` / `SHA-1`） | 同样被覆写 —— 记录里是**驱动侧**的算法与值（内存驱动恒为 `SHA256`） | `客户端点名别的算法（MD5/SHA-1）也**不改变**服务端结果` |
| 驱动原生给 **MD5**（Azure 风格；写法小写 `md5`） | 采用原生值，算法名写**规范名** `MD5`（C6.4 的"算法覆盖"） | `算法跟随驱动：原生给 MD5 → 记录写回 MD5` |
| 驱动原生**不可用**（`ETAG` / 算法为空 / 不是合法 hex / 无值） | **流式回算 SHA-256**；**绝不**把 `ETAG` 之类写进记录（"看起来有值"的假象） | `原生校验和不可用 → 回退**流式回算 SHA-256**` |

### 流式（RSS）证据 + 自证对照（R1）

对象 64 MiB，持久区驱动替身 `hide_checksum = true`（模拟"驱动不提供校验和"）→ 逼出**流式回算**路径。
跑在**真实 POSIX 栈**（staging / persistent 两个不同 `PosixBlobStore`，跨 store 流式复制）上：

```console
$ ./build/bin/test_metadata_lifecycle -s
  流式回算前后 RSS 增长 80 KiB（对象 65536 KiB，上限 8192 KiB）
  对照（整块读回）RSS 增长 65664 KiB
All tests passed (230 assertions in 6 test cases)
```

**为什么用内存适配器测不出这一条**：`InMemoryBlobStore::copy` 本身就在 RAM 里复制一份对象，
RSS 增长必然 ≈ 对象大小 —— 那是**适配器**的固有开销，会把"校验和是否流式"的信号淹没。
**自证对照**：同进程内再做一次故意的整块读回（`StringSink`），RSS 增长 65664 KiB；
若没有这一步，"增长 < 8 MiB"无法区分"实现流式"与"测量根本没生效"。

**sanitizer 构建下的取舍（如实标注）**：ASan 的影子内存/隔离区会把这条路径的 RSS 增长抬到
**约 10 MiB**（同一测试在 ASan 下的整块读回对照是 139392 KiB）—— 插桩下"流式"不再度量同一件事。
因此该上限可用 `FSS_TEST_RSS_LIMIT_KIB` 覆盖（`run_sanitizers.sh` 设为 512 MiB），
**紧的那个上限（8 MiB）在普通门槛里跑**。这与 `big_file.h` / `run_sanitizers.sh` 对 1 GiB 用例的
既有约定一致，是刻意的取舍而非"绕过失败"；插桩下仍保留"整块读回必须被测量抓到"这条对照。

---

## 3. 切片 3：12 步序列与故障注入（C6.3）

契约依据：`docs/03-api-contract.md` §2.6 的 12 步序列 + 第 10/11/12 步的事件与回滚语义。
上游依据：`docs/01-osdu-research.md` §2.1 逐步表（含第 11 步的上游 issue #76）与
`status/FileDatasetDetailsPublisher.java`（第 10 步的第二个事件）。

### 交付物

| 路径 | 变化 |
| --- | --- |
| `src/app/usecases/usecases.cpp` | ① 新增统一的第 12 步回滚 `RollbackCreatedObject`（删 persistent + `FAILED` 事件 + 审计），**第 6/7/9 步共用**；② **第 7 步失败此前漏掉回滚**（`FSS_TRY` 直接 return）→ 已修；③ 第 10 步补发 `datasetDetails`；④ 第 11 步清理失败补审计告警 |
| `src/domain/ports/ports.h` | `DatasetDetailsEvent`（`kind`/`properties` 形状对齐上游）+ `IEventPublisher::PublishDatasetDetails` |
| `src/app/usecases/usecases.h`、`src/adapters/http/router.cpp` | `CallerContext.correlation_id`，由 `x-correlation-id` 头填（上游 `properties.correlationId`） |
| `src/main/server_main.cpp` | `LogEventPublisher::PublishDatasetDetails`（默认写日志） |
| `tests/framework/fake_ports.h`、`app_fixture.h` | `RecordingEventPublisher`：按 status 选择性失败 + `datasetDetails` 记录/注入失败；`FaultyMetadataRepository`（可注入 `Create` 失败）；`AppFixture::UseMetadata()`（换仓储重建端口集合） |
| `tests/integration/test_metadata_lifecycle.cpp` | 追加 9 个用例：正常路径顺序 + **7 个故障注入点** + 端到端 `x-correlation-id` 透传 |

### 故障注入矩阵（每一行都有断言，且都配"顺序/副作用"观测）

| # | 注入点 | 期望 | 顺序/副作用证据 |
| --- | --- | --- | --- |
| ① | 第 1 步 `IN_PROGRESS` 事件失败 | **非致命**（`SUCCESS` 仍发出） | `statuses == {SUCCESS}` |
| ② | 第 6 步 复制失败 | `502` `kBadGateway` + `FAILED` + 审计失败 | 记录数 `0`（证明复制在写记录**之前**）；persistent 不存在；staging 保留 |
| ③ | 第 7 步 校验和回算失败 | `502` + **回滚删除 persistent** + `FAILED` | persistent 不存在、staging 保留、无记录 |
| ④ | 第 9 步 写记录失败 | `500` `kInternal` + **回滚删除 persistent** | staging **仍存在**（证明第 11 步在第 9 步之后） |
| ⑤ | 第 10 步 `SUCCESS` 事件失败 | **非致命**，仍 `201` | 记录已落地、staging 已清理 |
| ⑥ | 第 11 步 删 staging 失败 | 仍 `201`（上游 issue #76）+ 审计告警 `createMetadataStagingCleanupFailure` | staging 对象仍在（注入生效），审计计数恰为 1 |
| ⑦ | 第 10 步 `datasetDetails` 失败（**追加点**） | **非致命**，仍 `201` | `details_calls == 1` 但 `details` 为空 |

> **自证对照（R1）**：把"第 7 步失败时回滚"和"第 11 步失败记审计"两处代码临时拆掉后重跑，
> 对应用例**必须失败**（实测分别命中 `statuses == {IN_PROGRESS, FAILED}` 与
> `HasAudit(createMetadataStagingCleanupFailure)` 两条断言）；恢复后全绿。

### 第 10 步的第二个事件（此前**根本没有实现**）

`datasetDetails` 是上游在 `publishSuccessStatus` 之后立刻发的第二个事件。本仓库 4 份文档都写着它，
但代码里只有 `status` —— 属于 R15 说的"只有描述、没有实现"的约定。现已实现并断言：
`kind == "datasetDetails"`、`datasetId == 记录 id`、`datasetVersionId == 版本`、
`datasetType == "FILE"`、`recordCount == 1`、`correlationId` 来自 `x-correlation-id` 头
（端到端用例走真实端口验证）。

---

## 4. 切片 4：`getFileList`（C6.6）+ 鉴权角色（C6.8）

### 交付物

| 路径 | 变化 |
| --- | --- |
| `tests/integration/test_file_list.cpp` | 新增：5 用例 / 313 断言（字段名精确、`CreatedAt` 格式、分页不重叠、时间/用户过滤、上游三条 fixture 逐字驱动、非法参数正反例） |
| `tests/unit/test_roles.cpp` | 新增：3 用例 / 74 断言（9 个常量逐字节、端点↔角色映射、403 先于 400） |
| `tests/conformance/fixtures/upstream/list/*.json` + README | vendored 上游 `getFileList` 的三条验收 payload（同 commit/Apache-2.0） |
| `src/domain/ports/ports.h` | 9 个角色常量**集中为唯一真相**（补齐 `service.storage.viewer` / `service.storage.admin`）；`IAuthorizer::AuthorizeAny`（任一角色即通过） |
| `src/app/usecases/usecases.{h,cpp}` | `FileListRequest.items` 缺省 10 → **0**（上游语义）；`TimeFrom > TimeTo` → 400；无记录消息对齐上游；`Driver` 改小写；`DeleteFileMetadata` / `CopyFiles` 改"任一角色" |
| `src/common/time/time_format.cpp` | ISO-8601 解析补**形状 + 字段范围**校验（`timegm` 会静默归一化越界字段） |
| `tests/framework/{app_fixture.h,fake_ports.h}` | `UseAuthorizer`；`AllowAllAuthorizer::AuthorizeAny` + 记录角色集合 |
| `tests/conformance/test_ops_endpoints.cpp` | 修正一处**错误期望**：发 `{}` 却期望 200（与上游 `File_GetList_EmptyPayload.json` 的 400 冲突） |

### C6.6 判定表

| 情形 | 期望 | 断言点 |
| --- | --- | --- |
| 响应字段名 | 恰好 `Content`/`Number`/`NumberOfElements`/`Size`；每项恰好 5 个键 | 用**集合相等**断言（多一个少一个都失败） |
| `CreatedAt` | `yyyy-MM-dd'T'HH:mm:ss.SSS+0000`（28 字符、末尾不是 `Z`） | 逐位 + 与注入时钟一致 |
| `Driver` | 小写驱动名（内存装配 → `memory`） | 与 §2.2/§2.3 一致（此前 getFileList 返回大写，P6-D09） |
| 分页 | `PageNum=0`（2 条）与 `1`（1 条）**不重叠**、并集完整、顺序稳定；超出范围 → 400 | 3 条记录 + `Items=2` |
| 时间过滤 | 闭区间：`[t0,t1]`→2 条、`[t1,t1]`→恰好 1 条、`[t2,t2]`→1 条；区间内无记录 → 400 | memory 与 SQLite 语义一致（`LocationQuery`） |
| 用户过滤 | `UserID=alice`→2 条、`bob`→1 条；与时间过滤**同时**生效 | 两个用户 + 三个时刻 |
| 上游三条负向 | `{}`、缺 `Items`、无记录 → 全部 **400** | **逐字** fixture；并把"无记录"的消息与"解析失败"区分开 |
| 非法参数 | `Items=0/-1`、`PageNum=-1`、非法/越界时间、`TimeFrom>TimeTo` → 400；合法参数 → **200**（正例对照 R16） | 6 条非法 + 1 条正例 |

### C6.8 判定表

| 检查 | 断言 |
| --- | --- |
| 9 个常量逐字节 | 字面量表（含新补的 `service.storage.viewer`/`admin`）；无重复值；`viewers`/`viewer` 不可互换 |
| 单一真相 | app 层短名 == `domain` 常量 |
| 端点 ↔ 角色 | 12 个用例逐个驱动（**一律拒绝**的作者器 → 授权是第一步，不需要合法输入），角色集合必须等于上游 `@PreAuthorize`（含两处"任一角色"） |
| 安全属性 | 非法输入 + 拒绝的作者器 → 必须是 `403`（**不能**是 400/404），即授权先于输入校验 |

> **自证对照（R1）**：① 把 `service.storage.viewer` 改成 `...viewers` → 逐字节用例必失败；
> ② 把 `DeleteFileMetadata` 退回单角色 `editors` → 映射用例必失败。两处都实测过。

---

## 5. 切片 5：DMS 6 端点 + Delivery（C6.7）

### 交付物

| 路径 | 变化 |
| --- | --- |
| `tests/conformance/test_dms_delivery.cpp` | 新增：7 用例 / 196 断言（每个响应的**键集合**、上游 DMS 端到端场景、copy/delivery 形状、状态码正反例） |
| `src/adapters/http/dto/dto.{h,cpp}` | 上传/下载位置的两套键集合（files `fileSource`；collections `fileCollectionSource` + `fileCount` + `fileNames`）；`expiryTime` 同 OSDU 时间戳格式 |
| `src/adapters/http/router.cpp` | DMS handler 改成**按前缀参数化**（此前两条路由共用同一个 lambda，集合版被写成 `fileSource`） |
| `src/app/usecases/usecases.{h,cpp}` | `RetrievalInstruction` 补 `fileSource`/`createdBy`/`expiresAt`（上游下载位置是 4 个键，只回 `signedUrl` 少三个） |
| `tests/unit/test_http_dto.cpp` | 一条断言改为**顺序无关**（`nlohmann::json` 的对象是 map，`Dump` 按字典序输出；原来把它当成了插入序） |

### 键集合判定表（每一行都是"集合相等"，多一个少一个都失败）

| 端点 | 顶层 | 位置对象 |
| --- | --- | --- |
| `files/storageInstructions` | `{providerKey, storageLocation}` | `{signedUrl, fileSource, createdBy, expiryTime}`；**无** `fileCollectionSource` |
| `file-collections/storageInstructions` | 同上 | `{signedUrl, fileCollectionSource, fileCount, fileNames, createdBy, expiryTime}`；**无** `fileSource` |
| `files/retrievalInstructions` | `{datasets}`；`datasets[]` = `{datasetRegistryId, retrievalProperties, providerKey}` | `{signedUrl, fileSource, createdBy, expiryTime}` |
| `file-collections/retrievalInstructions` | 同上 | 集合版（含 `fileCollectionSource`/`fileCount`/`fileNames`） |
| `files|file-collections/copy` | 数组；元素 = `{success, datasetBlobStoragePath}` | `datasetBlobStoragePath` 是**目标**路径 |
| `delivery/GetFileSignedUrl` | `{processed, unprocessed}` | 每个 `processed[srn]` = `{signedUrl, unsignedUrl, kind, connectionString}`，**`connectionString` 存在且为 `null`** |

### 上游 DMS 端到端场景（逐字照 `IntegrationTest_DMS.feature`）

`storageInstructions → 用返回的 signedUrl 上传 → 登记元数据 → retrievalInstructions → 用返回的 signedUrl 取回`
——最后**断言取回的字节与上传的一模一样**（上游 feature 的最后一步就是这句）。

### 状态码

`datasetRegistryIds` 不是数组 / `datasetSources` 缺失 / `srns` 不是数组 → **400**；
`{"srns":[]}` 与 `{"datasetSources":[]}` 是**合法**请求 → **200**（正例对照 R16，让 400 不至于"恒真"）。
角色（`dataset.editors`/`dataset.viewers`/`storage.creator|admin`/`delivery.viewer`）与
"403 先于 400"由切片 4 的 `test_roles` 覆盖 —— 本切片只补响应形状与状态码。

---

## 6. 切片 6：大文件搬迁（C6.9）+ 幂等并发（C6.11）+ tmp 名唯一性（C6.13）

> 计划原写"6 个切片"，实际切成 7 个：C6.11~C6.13（多实例）与远端仓储的工作量比预估大，
> 且这一片**抓出了 6 个真缺陷**（§9 的 P6-D12~P6-D17）。最后一刀（C6.12 GC 租约 + C6.10 回归）留下。

### C6.9 大文件搬迁（≥1 GiB）

`test_metadata_lifecycle.cpp` 新增一例：1 GiB 对象 staging→persistent + 流式回算校验和。

```console
$ ./build/bin/test_metadata_lifecycle "[c6.9]" -s
  对象 1024 MiB，RSS 增长 3432 KiB（上限 65536 KiB）
All tests passed (1040 assertions in 1 test case)
```

- **为什么用 `CurrentRssKib()` 增量而不是 `PeakRssKib()`（VmHWM）**：VmHWM 单调不减，
  同一二进制里前面那例（带 64 MiB"整块读回"对照）会把它抬上去，之后测到的 Δ 会失真、
  甚至恒为 0 —— 那是一条**恒真**的假断言（R4/R16 的同类教训）。规模/上限可被构建方式覆盖
  （ASan 缩到 64 MiB / 放宽上限，见 `big_file.h`）。
- 校验和与**独立**流式算出的 SHA-256 一致（1 GiB 上也证明读的是完整对象）。

### C6.11 多实例幂等（`tests/integration/test_multi_instance.cpp`，151 断言）

| 用例 | 断言 |
| --- | --- |
| 内存装配：两个实例（同一份存储 + 同一个仓储）并发提交同一 `FileSource` | 两个都成功、拿到**同一条**记录 id；每轮**恰好新增 1 条**记录；persistent 恰 1 个对象且校验和正确；staging 被清掉 |
| **SQLite 两个连接**（两个仓储对象指向同一个库文件 = 真·2 实例） | 覆盖两种最坏情况：**同 id**（撞主键）与**不同 id**（撞 `ux_metadata_source`）；两种都必须**幂等返回既有记录而不是 500** |
| 对照：朴素实现（按 record id 建键 + check-then-insert，无幂等键约束） | 用栅栏固定"两个实例都查完、都还没插"的窗口 → **必然出现 2 条记录**（证明上一条的"恰 1 条"能失败） |

**自证对照（R1）**：把 SQLite 的冲突处理与用例里的 zone 守卫拆掉后重跑，3 个用例里
**2 个失败**（实测）；恢复后 151 断言全绿。

### C6.13 临时文件名唯一性（`tests/integration/test_posix_tmp_names.cpp`，63 断言）

- 正例：两个 `PosixBlobStore`（**故意共用同一个 `instance_id`**）并发写**同一个 key** × 12 轮 →
  两个 put 都必须成功，且结果**恰好等于其中一份完整数据**（头尾标记 + 大小都断言）。
- 对照：测试内自带的**朴素写入器**（固定临时名、不含实例标识）+ 栅栏固定交错 →
  产物 = A 的头 + B 的尾：**大小与正常对象一致、内容却不是任何一份** —— 只有校验和能发现它。
  这条证明"结果 == 其中一份完整数据"这条断言**能失败**。
- 产品侧修（P6-D17）：`tmp_counter_` 从"每个 store 各自从 0 开始"改成**进程级**静态序号
  （`instance_id` 管跨实例、`pid` 管跨进程、进程级序号管同进程内的多个 store 对象 —— 三者缺一不可）。

### 附带修掉的"内存适配器线程安全"（P6-D14~D16）

并发用例第一次跑就炸出堆损坏/挂死，根因是三个内存适配器**都没有锁**（而且给其中两个加锁后
又踩到"公开方法持锁后互调"造成的**自死锁**，用 gdb 回溯定位）。三个适配器现在都：
① 内部一把互斥量；② 内部查询改成不加锁的私有实现。`ManualClock` 的读数也改成原子。

---

## 7. 切片 7：GC 租约（C6.12）+ C6.10 回归 —— **P6 收口**

### 交付物

| 路径 | 内容 |
| --- | --- |
| `src/app/tasks/gc_task.{h,cpp}` | L4 的 GC 任务：过期租约（**原子领取**）→ 位置记录 → 对象；单实例降级的 staging TTL 扫描；persistent 孤儿（宽限期）；`dry_run` 默认只报告 |
| `tests/integration/test_gc_lease.cpp` | 6 用例 / 131 断言：dry-run、在途不删、过期回收、**无租约不许删**（含单实例正例对照）、**有记录永不删**、**两个 GC 并发不重复删** |

### C6.12 判定表（每条都配了反向/正例对照）

| 情形 | 期望 | 断言点 |
| --- | --- | --- |
| `dry_run=true`（默认） | 报告候选（`deleted_objects==1`）但**一个都不删** | 对象 + 位置记录 + 租约都还在 |
| 在途租约**未过期**（且已续租） | **不删**（哪怕 staging TTL 早过、`dry_run=false`） | `expired_leases_claimed==0`、对象仍在 |
| 租约**过期且无记录** | 回收：对象 + 位置记录都删，报告列出 file_id | 再跑一轮 → `claimed==0 / deleted==0`（幂等） |
| `require_lease_expiry=true` 且**完全没有租约** | **不许删**（这就是 M3 的护栏） | 对象仍在；**正例对照**：显式切成单实例（`require_lease_expiry=false`）后同一对象**必须**被 TTL 回收 |
| 有**元数据记录**的对象（哪怕过期租约指着它） | `skipped_has_record==1`、**永不删** | 对象 + 位置记录 + 元数据记录都在 |
| **两个 GC 并发**（不同 instance id） | 每个对象**恰好删一次** | 两侧 `claimed` 之和 == 对象数、`deleted` 之和 == 对象数、两份 `deleted_file_ids` **交集为空** |

**自证对照（R1）**：把"有记录永不删"的守卫与租约的"原子领取"（只扫描、不迁移 owner）拆掉后重跑，
6 个用例里 **3 个失败**（实测）；恢复后 131 断言全绿。

> ⚠️ **未做（如实登记）**：`.tmp_*` 残留清理（C9.25，需要 `IBlobStore` 暴露内部键）、
> 过期 transfer token 清理（自签 token 无状态、无表）、调度/领导者选举/GC 指标（P9）、
> PG 版 `ILeaseRepository`（目前只有测试替身）、**多实例下 GC 的 TTL 判定应改用数据库时钟**
> （ADR-009 §6.4；见缺陷表 P6-D18）。

### C6.10 回归（P0–P5 全绿）

```console
$ ctest --test-dir build -L "phase0|phase1|phase2|phase3|phase4|phase5"
100% tests passed, 0 tests failed out of 48
Total Test time (real) = 31.39 sec
$ ./scripts/run_all_gates.sh          # 含 ASan/UBSan/LSan 与 5 个自证脚本
✅ 全部已启用阶段门槛通过。
```

---

## 8. 门槛命令与输出（累计）

```console
$ cmake --build build -j8 && ctest --test-dir build -L phase6 --output-on-failure
    Start 34: test_sqlite_metadata_repository ......   Passed    0.01 sec
    Start 35: test_metadata_lifecycle ..............   Passed    0.54 sec
    Start 36: test_checksum ........................   Passed    0.01 sec
    Start 37: test_file_list .......................   Passed    0.05 sec
    Start 38: test_roles ...........................   Passed    0.01 sec
    Start 39: test_dms_delivery ....................   Passed    0.03 sec
    Start 40: test_multi_instance ..................   Passed    0.05 sec
    Start 41: test_posix_tmp_names .................   Passed    0.09 sec
    Start 42: test_gc_lease ........................   Passed    0.06 sec
100% tests passed, 0 tests failed out of 9

逐测试断言数：
  test_sqlite_metadata_repository   122 assertions in 2 test cases
                                    ← C2.10（元数据侧契约）+ C6.5（版本链 + 唯一索引自证）
  test_metadata_lifecycle          1422 assertions in 16 test cases
                                    ← C6.4/C6.3/C6.9（含 1 GiB 搬迁 + 流式回算：1040 断言）
  test_checksum                     106 assertions in 6 test cases
                                    ← C6.4 的 L1 基座（算法解析 / hex 结构 / 公开向量）
  test_file_list                    313 assertions in 5 test cases
                                    ← C6.6（字段名 / 分页 / 时间与用户过滤 / 上游三条 fixture）
  test_roles                         74 assertions in 3 test cases
                                    ← C6.8（9 个常量 + 端点↔角色映射 + 403 先于 400）
  test_dms_delivery                 196 assertions in 7 test cases
                                    ← C6.7（DMS/Delivery 键集合 + 端到端场景 + 状态码）
  test_multi_instance               151 assertions in 3 test cases
                                    ← C6.11（内存 + SQLite 两连接 + 朴素主键对照）
  test_posix_tmp_names               63 assertions in 2 test cases
                                    ← C6.13（临时名唯一性 + 朴素临时名必错乱对照）
  test_gc_lease                     131 assertions in 6 test cases
                                    ← C6.12（dry-run / 在途不删 / 过期回收 / 并发不重复删）
  ─────────────────────────────────────────────
  合计 2578 个断言 / 50 个测试用例 / 9 个测试
```

相关护栏（改动仓储 SQL 后必跑）：

```console
$ ./build/bin/test_sql_guardrail          → 9 assertions in 2 test cases（每条 SQL 必须带 partition_id）
$ ./build/bin/test_layering_guard         → 20 assertions in 3 test cases（L2 只依赖 L3 端口 + L1）
```

---

## 9. 判据进展

| 判据 | 状态 | 证据 |
| --- | --- | --- |
| **C2.10（元数据侧）** 同一套契约跑第二遍 | ✅ | `test_sqlite_metadata_repository` 复用 `CheckMetadataRepositoryContract`（与内存实现逐条相同） |
| **C6.5** 版本链 | ✅ | 契约的版本用例 + 切片 1 的"数据库真实形态"用例（3 版共存、1 个 latest、部分唯一索引自证） |
| **C6.1** 黄金样例字段级往返 | ✅ | 仓储侧：`data` 列存整条 JSON → SQLite 仓储无损（`test_sqlite_metadata_repository` 与内存实现跑同一份契约）；REST 侧的全字段逐字段比对见 P4 的 C4.2（`test_rest_contract` 的"只允许 3 处服务端改写"用例） |
| **C6.2** §3.4 全部负向样例 + kind 消息逐字节 | ✅ | P4 的 C4.3：vendored 上游 10 条在跑样例 + 2 条上游自己注释掉的行，逐字比对期望消息（`File_*_msg.json`） |
| **C6.4** 校验和（服务端计算并**覆写** / 算法跟随驱动 / 大对象流式） | ✅ | 切片 2：判定表 5 行全部有断言（未提供 / 客户端值被覆写 / 客户端点名别的算法 / 原生 MD5 / 原生不可用回退）+ L1 公开向量与算法解析两向断言。**旧表述**"客户端提供但不符 → 400 + 删除对象"已推翻（P6-D05） |
| **C6.3** 12 步序列 + 故障注入 | ✅ | 切片 3：正常路径的顺序/副作用 + **7 个注入点**（计划要求 6 个，追加 `datasetDetails` 非致命点）；第 9/11 步的顺序用"staging 是否还在"证明；自证对照两处 | 
| **C6.6** `getFileList` 语义 | ✅ | 切片 4：字段名集合相等、`CreatedAt` 格式、分页不重叠、闭区间时间过滤、用户过滤、上游三条 fixture 逐字 400、非法参数正反例 |
| **C6.8** 角色常量 | ✅ | 切片 4：9 个字面量逐字节 + 端点↔角色映射（含两处"任一角色"）+ "403 先于 400"；两处自证对照 |
| **C6.7** DMS 6 端点 + Delivery | ✅ | 切片 5：6 个端点 + delivery 的**键集合**逐键断言、上游 DMS 端到端（上传→登记→取回字节一致）、copy 目标路径、`connectionString: null`、状态码正反例 |
| **C6.9** 大文件搬迁 RSS | ✅ | 切片 6：**1 GiB** staging→persistent + 流式回算，RSS 增长 **3432 KiB < 64 MiB**；另有 64 MiB 那一例带"整块读回"对照（65664 KiB）|
| **C6.11** 幂等并发 | ✅ | 切片 6：内存装配 + **SQLite 两个连接**（同 id / 不同 id 两种冲突路径）都"恰 1 条记录、1 份对象"；对照（朴素主键 check-then-insert）**必现重复** |
| **C6.13** tmp 名唯一性 | ✅ | 切片 6：同 `instance_id` 的两个 store 并发写同一 key × 12 轮无错乱（进程级序号）；对照证明朴素临时名会产出"大小正常、内容混装"的对象 |
| **C6.12** GC 租约 | ✅ | 切片 7：dry-run 不删 / 在途不删 / 过期且无记录才回收 / **无租约不许删**（含单实例正例对照）/ **有记录永不删** / **两个 GC 并发每对象恰好删一次** |
| **C6.10** 回归（P0–P5） | ✅ | 切片 7：`ctest -L "phase0|...|phase5"` **48 测试全绿** + `run_all_gates.sh`（含 sanitizer 与 5 个自证脚本）全绿 |
| C6.2 / C6.3 / C6.6 / C6.7 / C6.8 / C6.11 / C6.12 / C6.13 | ⬜ 未开始 | 见开头"剩余" |

---

## 10. 本阶段发现并修复的缺陷

| 编号 | 症状 / 根因 | 复现方式 | 修复 |
| --- | --- | --- | --- |
| ~~**P6-D02**~~（**已撤回**） | 切片 2 第一版把计划 C6.4 的"客户端提供但不符 → 400"当成**上游语义**，据此在新用例里断言"客户端给 MD5/SHA-1 的正确值必须通过"，并据此把旧实现判为缺陷。**这个判断是错的**：上游是**无条件覆写**（本表 P6-D05） | —— | **撤回**：不是代码缺陷，而是"**从计划里的一句臆断出发写测试**，没有先读 vendored 样例"。用例已按覆写语义重写。保留编号以留下教训 |
| **P6-D03** | **跨 store 复制把整个对象读进内存**：`ReadObject` → `StringSink` → `put`，1 GiB 对象会让 RSS 抬高 1 GiB —— 直接顶穿 C6.9 的红线 | 切片 2 的流式用例（跨 store + 64 MiB）；同一测试内"整块读回"对照给出 65664 KiB 增长，证明该测量确实能抓到整块驻留 | `StoreByteSource`（`BufferSink` + 分段 `get`）+ 边读边写；校验和走 `HashingSink` 增量计算 |
| **P6-D04** | **门槛可能在"被拆掉防线"的源码树上运行**：`scripts/verify_http_hardening.sh` 等自证脚本临时改源码、靠 `trap` 恢复；脚本被 **SIGKILL** 强杀时 trap 不执行 → H-2 的两道防护补丁残留在 `src/common/http/server.cpp`，下一次门槛的基线直接失败（假警报）；反过来若残留的是"让测试更容易通过"的注入，就会**静默**削弱门槛 | 复现：强杀 `run_all_gates.sh`（本轮真实发生）→ `git diff src/common/http/server.cpp` 里能看到 `自证注入` | ① `git checkout -- src` 恢复；② `scripts/run_all_gates.sh` 增加**前置机械检查**：`src/` 下不得存在 `_*selftest*`/`*_injected*` 文件，且 `git diff -- src` 不得含注入标记，命中即拒绝开跑并给出修复指令。**两向自证**：造一个残留文件/一行标记 → 前置检查必须退出 1；清理后必须通过 |
| **P6-D05** | **计划里的一条判据没有上游依据**：C6.4 写的「客户端提供 `Checksum` 但不符 → `400` + 删除已搬迁对象」与上游冲突。按它实现后，**phase4 的 C4.2/C4.3 两条已收口用例立刻失败**（`test_rest_contract`：golden 样例与 `File_Calculate_Checksum` 都期望 `201`，实测得到 `400`） | 先按旧判据实现 → `ctest -R test_rest_contract` 两条 `REQUIRE(status == 201)` 失败；证据见 `docs/01-osdu-research.md` §2.3 与 `File_CorrectPayload.json` | 实现改为"**无条件覆写 + 不校验 + 不回滚**"；契约 §2.6 第 7 步改写；计划 C6.4 更正并在 `docs/00-final-design.md` §5 登记为**被推翻的结论**（不静默改）。这条差异属于"能测出来的差异"，不是"看起来更好" |
| **P6-D06** | **第 7 步失败漏掉回滚**：第 12 步要求"任一步 6/7/9 失败 → remove(persistent) 回滚"，但第 7 步用的是 `FSS_TRY(...)`，失败时**直接 return** —— 已搬迁的 persistent 对象被留下成为无主副本（GC 会把它当成在途对象，或永久占空间） | 切片 3 的故障注入点③（对 `get` 注入故障）→ 旧实现下 `REQUIRE_FALSE(stat(persistent).exists)` 失败 | 抽出统一的 `RollbackCreatedObject`（删 persistent + `FAILED` + 审计），第 6/7/9 步共用；自证：拆掉回滚后用例必失败 |
| **P6-D07** | **第 10 步只发了一个事件**：4 份文档（调研 §2.1/§2.3、设计 §2.6、契约 §2.6、计划任务 6）都写着"`status` + `datasetDetails`"，代码里**只有** `status` —— 典型的"只有描述没有实现"（R15） | 切片 3 的正常路径用例断言 `events.details.size() == 1` → 旧实现下为 0 | 新增 `DatasetDetailsEvent` + 端口方法 + 组合根实现 + 用例调用；`correlationId` 由 `x-correlation-id` 头透传（端到端用例证明） |
| **P6-D08** | **第 11 步清理失败静默**：契约要求"忽略 + **审计告警**"，实现里只有 `(void)store->remove(...)` —— staging 里堆孤儿无人察觉 | 切片 3 的故障注入点⑥ | 记 `createMetadataStagingCleanupFailure`（result=failure），响应仍 `201`；自证：拆掉审计后用例必失败 |
| **P6-D11** | **DMS 的两个响应形状与上游不符**：① `/v2/file-collections/{storageInstructions,retrievalInstructions}` 与 `/v2/files/*` 共用同一个 handler，返回 `fileSource` —— 上游集合版是 **`fileCollectionSource`** + `fileCount` + `fileNames`，**没有** `fileSource`（`FileCollectionStorageServiceImpl`）；② `retrievalInstructions` 的 `retrievalProperties` 只回了 `signedUrl`，上游下载位置还有 `fileSource`/`createdBy`/`expiryTime`（`StorageServiceImpl:268`）。客户端按键名取值会拿到空值 | 切片 5 的 `test_dms_delivery`（集合版键集合断言 + `retrievalProperties` 断言；旧实现 3 条失败） | handler 按前缀参数化（`make_*_instructions(collection)`）；DTO 拆出 `LocationToJson` 支持两套键集合；用例补 `fileSource`/`createdBy`/`expiresAt` |
| **P6-D09** | **`getFileList` 的三处上游不兼容**：① `FileListRequest.items` 缺省 `10` → 上游 `{}`（缺 `Items`）必须 `400`，我们返回 `200`；② `Driver` 返回**大写** provider key，与 §2.2/§2.3（小写）及契约 §2.5 样例不一致；③ 无记录消息是自造的 `No record found`，上游 provider 是 `Nothing found for such filter and page(num: N, size: M).`。三处都被"发 `{}` 期望 200"的旧用例挡住（那条期望本身就是错的） | 切片 4 的 `test_file_list`（三条上游 fixture 逐字驱动）；`test_ops_endpoints` 修正后立即暴露 | `items` 缺省改 `0`；`Driver` 统一小写；消息对齐上游；同时纠正 `test_ops_endpoints` 的错误期望并登记 |
| **P6-D10** | **ISO-8601 解析静默归一化越界时间**：`ParseIso8601` 直接交给 `timegm`，后者把 `2020-13-45T99:99:99Z` **归一化**成一个"看似合理"的错误时刻（月份 13 → 次年、小时 99 → +4 天）。用它当 `TimeFrom`/`TimeTo` 边界会**静默筛错数据**；`2020-1-01T...` 这类位数不符也被接受 | 切片 4 的 `TimeFrom: "2020-13-45T99:99:99Z"` 分支（旧实现返回 200） | 解析前补**形状**（`consumed == 19`）与**字段范围**（月/日/时/分/秒、含闰年 `DaysInMonth`、时区偏移）校验；`test_time.cpp` 增 11 条越界/形状用例 + 闰日正例对照（phase1 断言数 4970 → **4984**） |
| **P6-D12** | **SQLite 并发写入不是幂等的**：两个实例（两个连接）同时 `Create` 同一个 file_source 时，输家的 `INSERT` 撞唯一索引 → 直接把 `UNIQUE constraint failed` 映射成 **500**；另一条路径（两个实例生成**同一个 id**）在"同 id 已存在"预检处就报 `同 id 已存在且 file_source 不同`（其实 source 相同）。客户端重试/并发提交拿到 500 而不是既有记录 | 切片 6 的 SQLite 两连接用例（同 id 与不同 id 两种组合；修复前 2 条 `REQUIRE(...ok())` 失败） | 两条路径都补"按幂等键**回读既有记录**"：预检命中时回读；`(rc & 0xFF) == SQLITE_CONSTRAINT` 时回读（扩展码必须按主码比较，P3-D03） |
| **P6-D18**（**未修复，登记 P9**） | **多实例下 GC 的时间判定用了服务时钟**：ADR-009 §6.4 要求"租约/过期判定一律用数据库 `now()`"（实例时钟偏移会误判），GC 的租约到期由 `ClaimExpired` 走数据库侧 ✓，但 staging TTL / orphan 宽限期的比较用的是 `ports.clock` | 切片 7 写用例时暴露：租约替身的时钟与服务时钟是**两个**时钟，必须显式同步（`SetNowMillis`）才让"过期"生效 | 登记到 P9：在端口上暴露数据库时间（或让 GC 走仓储查询），使 TTL 判定也来自数据库时钟。**当前未修复**，因此多实例下应把 `staging_ttl_hours` / `orphan_grace_hours` 留足余量 |
| **P6-D13** | **12 步序列在"重试/并发"下会删掉活对象**（三种形态）：① 位置记录的 `zone` 被上一次成功的请求改成 persistent，第二个请求据此把 **persistent 对象当 staging 清理**；② 复制失败时的回滚会删掉**并发赢家**刚写好的对象；③ staging 已被赢家清理 → 输家复制失败直接 502（而不是幂等成功） | 切片 6 的并发用例（修复前：`stat(persistent).exists == false`、`zone == kStaging`、`复制源不存在`） | ① `source_is_staging` 守卫复制与第 11 步清理；② `RollbackCreatedObject` 加"已有记录指向该对象则不动"的守卫；③ 第 4b 步加**幂等快路径**（同 fileSource 已有记录 → 直接返回），复制失败时也回读一次 |
| **P6-D14** | **`InMemoryBlobStore` 不是线程安全的**：组合根 `single` 模式用它，并发请求同时改 `std::map` → **静默堆损坏**（实测 `double free or corruption` / `free(): invalid next size`），而不是给出错误 | 切片 6 的并发用例（两线程共享同一个 store） | 内部加一把互斥量（与 `InMemoryMetadataRepository` 的既有做法对齐），头文件写明线程安全 |
| **P6-D15** | **`InMemoryLocationRepository` 不是线程安全的**，而且加锁后 **`FindByFileSource` 自死锁**（持锁后又调用同样加锁的 `Find()`） | 并发用例**挂死**；gdb 回溯：`FindByFileSource` → `Find` 等在同一个非递归互斥量上 | 加锁 + 内部查询内联成不加锁实现。★ 教训：给类加锁后要**逐个审"公开方法之间的调用"** |
| **P6-D16** | **`InMemoryMetadataRepository` 不是线程安全的**，且 **`GetLatestByFileSource` 自死锁**（持锁后调 `GetById`） | 同上（gdb 回溯到 `GetById` 的 `lock_guard`） | 同上：加锁 + 用不加锁的 `FindChain` 实现查询；`ManualClock` 的读数改成原子 |
| **P6-D17** | **POSIX 临时文件序号是"每个 store 各自从 0 开始"**：同一进程里两个 store（同 `instance_id`）会算出同一个临时路径 → 后者 `O_EXCL` 失败（极端情况下互相覆盖） | 切片 6 的 tmp 名用例（修复前两个 put 有一个失败） | 改成**进程级**静态序号（`instance_id` + `pid` + 进程级序号三者合一）；自证：去掉序号后用例必失败 |

## 11. 结论

| 项 | 结论 |
| --- | --- |
| 切片 1 门槛（SQLite 元数据仓储 + 元数据契约） | ✅ 契约在 SQLite 上跑第二遍；两个护栏全绿 |
| 切片 2 门槛（校验和 C6.4） | ✅ 覆写语义 + 算法跟随驱动 + 流式回算 + L1 公开向量都有断言 |
| 切片 3 门槛（12 步序列 C6.3） | ✅ 正常路径的顺序/副作用 + 7 个故障注入点 + 端到端 `x-correlation-id` 透传 |
| 切片 4 门槛（`getFileList` C6.6 + 角色 C6.8） | ✅ 上游三条 fixture 逐字 400 + 分页/时间/用户过滤 + 9 个角色常量与端点映射（顺带：phase1 因时间解析修复 4970 → 4984） |
| 切片 5 门槛（DMS/Delivery C6.7） | ✅ 6 端点 + delivery 的键集合逐键断言 + 上游 DMS 端到端 |
| 切片 6 门槛（C6.9 + C6.11 + C6.13） | ✅ 1 GiB 搬迁 RSS 3.4 MiB（< 64 MiB）；并发幂等在内存与 SQLite 两连接上都"恰 1 条"；tmp 名唯一性（含对照） |
| 切片 7 门槛（C6.12 + C6.10） | ✅ GC 租约 6 用例（含 5 条反向/对照断言）+ P0–P5 回归 48 测试全绿；`ctest -L phase6` **9 测试 / 2578 断言** |
| 已满足判据 | **C2.10（元数据侧）+ C6.1~C6.13 全部满足**（逐条证据见 §9 判据进展） |
| P6 是否收口 | ✅ **收口**（退出条件"C6.1–C6.10 满足"已达成；C6.11~C6.13 亦完成） |
| 未做但已登记（不阻塞判据） | `.tmp_*` 残留清理与 transfer token 清理（P9 C9.25）、GC 调度/领导者选举/指标（P9）、PG 版 `ILeaseRepository`、远端 Storage Service 仓储（ADR-004 列为**可选**）、组合根改接 SQLite 元数据仓储、GC 的数据库时钟（P6-D18） |
