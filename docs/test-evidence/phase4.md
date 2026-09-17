# 阶段 4 测试证据（进行中）

| 项 | 值 |
| --- | --- |
| 阶段 | P4（REST 适配层 + 端到端垂直切片（POSIX）） |
| 状态 | ✅ **切片 1~5/5 完成 —— C4.1~C4.11 全部满足**（1 错误映射+DTO；2 中间件+路由+组合根；3 自签数据面真实字节；4 DMS/Delivery/revoke + `/metrics` + 契约矩阵 + 护栏 + 超时；5 一手复核上游样例并**逐字**对齐期望消息） |
| 门槛命令 | `ctest -L phase4` |
| 退出码 | `0`（8 测试 / 1058 断言 / 32 用例） |

> 切片 4 结束时 C4.3 还差一条（`File_invalid_ScalarIndicator.json`"无结论"）。切片 5 在本机
> 找到了上游归档源码 `/home/ll/osdu-file-upstream`（commit `d7c25c2`），
> **用一手证据把这条补齐了**：既有枚举定义（`STANDARD`/`NOSCALE`/`OVERRIDE`）与消息格式
> （`Invalid value of <值> for ScalarIndicator`），也发现**上游自己把这行注释掉了**。
> 于是 C4.3 从"12/13 无结论"变成"**逐字对齐的 13/13**"（详见 §1e）。

---

## 1. 切片 1 交付物（错误映射 + DTO）

| 路径 | 内容 |
| --- | --- |
| `src/adapters/http/http_error_mapper.{h,cpp}` | `ErrorKind` → (HTTP 状态码, `reason`) 的**唯一权威表**（契约 §5）；三种错误体形态（契约 §1.6）；`http.error_format` 解析（非法值不静默折叠） |
| `src/adapters/http/dto/dto.{h,cpp}` | `LocationResponse`（`FileID` + `Location{SignedURL,FileSource}`）、`DownloadUrlResponse`（`SignedUrl`）、`FileLocationResponse`（`Driver`+`Location`）、`FileListResponse`（Spring Page）、`VersionInfoResponse`、元数据记录透传 |
| `tests/unit/test_http_error_mapper.cpp` | C4.5：12 个错误取值逐条比对状态码与 `reason`；三种形态的 `code`/`message` 一致性；契约 §1.6 的 **12 条固定消息**在三种形态下逐字节透出；配置解析负例 |
| `tests/unit/test_http_dto.cpp` | C4.4：**直接断言序列化后的字节**中出现 `FileID`/`Location`/`SignedURL`/`FileSource`/`SignedUrl`/`Content`/`NumberOfElements`/`Size`/`CreatedAt`，且**不得**出现 `fileId`/`fileSource`/`results`/`totalCount`；`CreatedAt` 为 `yyyy-MM-dd'T'HH:mm:ss.SSS+0000`；§2.1 响应不得含 `Driver` |

### 两处刻意的测试写法

| 写法 | 理由 |
| --- | --- |
| 期待表写成**显式清单并断言长度 == 12** | 新增 `ErrorKind` 时，映射实现与测试清单会同时提醒（与 `test_error_kind_coverage` 呼应）；否则"漏映射"会静默变成 500 |
| 大小写用**序列化后的字符串子串**断言，而不是 `value["FileID"]` | 后者在测试与实现犯**同样**的大小写错误时依然会绿；只有钉住线上字节才能发现漂移 |

### 命名冲突（P4-D01）

`namespace fss::adapters::http` 内部的 `http::Response` 会解析成**自己**（`fss::adapters::http::Response`），
而不是包装层 `fss::http::Response` → 编译报 `'Response' in namespace ... does not name a type`。
修法：显式写 `fss::http::Response`。教训：**当适配层 namespace 与依赖库 namespace 同名时，限定名必须写全**。

---

## 1b. 切片 2 交付物（中间件 + 路由 + 组合根）

| 路径 | 内容 |
| --- | --- |
| `src/adapters/http/router.{h,cpp}` | 本切片注册 **11 条路由**：运维 3（纯文本 + 免鉴权）+ 位置 5（uploadURL/getLocation/getFileLocation/downloadURL/getFileList）+ 元数据 3。`Wrap()` 是唯一横切入口：请求头 → `CallerContext` → 执行 → `ErrorToResponse` → 异常兜底为 500 |
| `src/main/server_main.cpp` | **组合根**（R12 唯一实例化具体实现处）：env 配置 → `PosixBlobStore`/`SqliteLocationRepository`/`InMemoryMetadataRepository`/`HmacTransferTokenCodec` → `LocationIssuer`/`UseCasePorts` → `Router`/`Server` → 启动并打印生效配置 + auth 告警 |
| `tests/conformance/test_ops_endpoints.cpp` | 真实回环端口上的 C4.6、C4.5(HTTP 侧)、C4.7(部分)、C4.2(部分) 与 **0 字节端到端**（uploadURL→空对象→metadata→GET→downloadURL→list→DELETE→404） |

### 本切片的三点工程记录

| 事项 | 说明 |
| --- | --- |
| `max_connections` 不得大于 `worker_threads` | 首次把测试配置写成 `worker_threads=4, max_connections=64` → `Bind()` 失败。这正是 **C1.12 的背压不变量**在起作用（上限大于线程数会让超限请求先排队，503 被延迟）——不是缺陷，是护栏 |
| 原始 HTTP 客户端**故意不补** `Content-Length` | `RawClient` 的设计是"允许构造畸形请求"，因此带体的请求必须自己补；第一次漏了导致服务端把体读成下一个请求、连接被关（无响应） |
| 组合根的路由对象必须活到服务停止 | 见 P4-D02 |

## 1c. 切片 3 交付物（自签数据面 + 真实字节端到端）

| 路径 | 内容 |
| --- | --- |
| `src/infra/transfer/blob_byte_source.h` | **L2** 适配：把 `IBlobStore` 的对象包成可定位读的 `bytes::ByteSource`（每次只读 64 KiB，RSS 与对象大小无关）。放在 L2 是因为适配层（L5）**不能**依赖 L2，只能持有 `shared_ptr<ByteSource>` |
| `src/adapters/http/router.{h,cpp}` | 新增 `TransferCallbacks{put, open_get}`（**由组合根绑定到 L2**，适配层不认识 `TransferEndpoint`——R12）与 2 条数据面路由：`PUT/GET /v1/transfer/:token`。PUT 走 `stream_body=true` + `max_body_bytes=0`（不限大小、不整块驻留）；GET 用 `Response::Stream` 直出 |
| `src/main/server_main.cpp` | 组合根把 `HmacTransferTokenCodec` 的真实字节通道接上（`TransferEndpoint` + `BlobByteSource`），并修正自签 base URL（见 P4-D04） |
| `tests/integration/test_upload_flow_posix.cpp` | **C4.8** 三条用例：① `uploadURL` → `PUT /v1/transfer/{token}`（**100 MiB 真实字节**）→ `POST metadata` → `GET downloadURL` → `GET /v1/transfer/{token}` → **SHA-256 逐字节比对** → `DELETE`(204) → 再读 **404**；② 0 字节边界；③ 数据面负例（对象不存在→404、`put` token 下载→403、过期→401、签名篡改→401） |

### 为什么这条用例非走真实端口不可

| 失败模式 | 只有端到端才暴露的原因 |
| --- | --- |
| 自签 URL 的 base 与路由 base path 不一致 | 单测里 `LocationIssuer` 与 `Router` 各自都"对"，拼在一起才 404（P4-D04） |
| `zone` 改了而 `extra.container` 没改 | 位置记录、token 载荷、存储容器三者一致时才对；单测只看其中一段（P4-D05） |
| 对象缺失被当成 0 字节对象 | 结果是 **200 + `Content-Length: 0`** 的"静默空文件"，比 404 危险得多（P4-D06） |
| 只比长度不比内容 | 100 MiB 的 `Content-Length` 一致但内容错位时长度断言依然绿 → 必须 **SHA-256** |

| 事项 | 说明 |
| --- | --- |
| 0 字节 PUT 必须显式 `Content-Length: 0` | `RawClient` 故意不补头；PUT 无长度且无 chunked 时服务端按 `stream_body` **等体**，客户端等响应 → 死锁到超时。测试的 `Do()` 改为"带体的方法一律补长度" |
| 规模可缩小 | 默认 **100 MiB**（判据要求）；`FSS_TEST_BIG_BYTES` 更小时取更小值（sanitizer 运行用它缩小规模，**该次运行不作为 C4.8 证据**） |
| `Range` 的断言取**尾部**区间 | 设计承诺数据面支持断点续传/分片（`BlobByteSource` 可寻址）。只测首段区间时，"每次都从头读"的实现也会通过；取最后 16 字节同时验证 `Seek` 真的生效（R15） |

---

## 1d. 切片 4 交付物（剩余 8 条路由 + `/metrics` + 契约矩阵 + 护栏 + 超时语义）

| 路径 | 内容 |
| --- | --- |
| `src/adapters/http/router.cpp` | 新增 **8 条路由**：DMS 6（`files/` 与 `file-collections/` 各 3：`storageInstructions`/`retrievalInstructions`/`copy`，两套路径**共用一个 lambda**，抄一份必然漂移）+ Delivery 1（`delivery/GetFileSignedUrl`）+ revoke 1（`files/revokeURL`，恒定 204 + `X-FSS-Notice`）。至此契约 §2 的 **19/19** 全部注册 |
| `src/adapters/http/dto/dto.{h,cpp}` | DMS/delivery 的 **camelCase** DTO：`StorageInstructionsResponse`（`providerKey`/`storageLocation`）、`RetrievalInstructionsResponse`（`datasets[]`）、`copy` 的**数组**响应、`UrlSigningResponse`（`processed` 是**对象**、`connectionString` 必须显式 `null`） |
| `src/adapters/http/metrics.{h,cpp}` | `/metrics`：Prometheus 文本（`text/plain; version=0.0.4`）。请求计数（route/method/status）+ 延迟直方图（9 桶 + `+Inf`）+ 传输 token 拒绝数 + 服务器在途/背压/拒绝计数。计数点**只在 `Wrap()` 一处**（唯一横切入口），并有 `RequestCount()` 让"内存计数"与"渲染文本"互为对照 |
| `tests/conformance/test_rest_contract.cpp` | C4.1（19 个端点 + `/metrics`，逐个断言路径/方法/状态码）、C4.2（黄金样例**全字段**比对）、C4.3（负向矩阵 12/13）、C4.7（缺省 3600s / `2H` / `8D→604800s` 数值） |
| `tests/conformance/test_error_formats.cpp` | C4.5 的 HTTP 侧：三种形态在同一端口各跑一遍；`X-FSS-Error-Kind` 响应头；契约 §1.6/§2.2 的固定消息逐字节；三种形态**必须互不相同**（否则"客户端兼容开关"这个能力等于不存在，R15） |
| `tests/integration/test_transfer_timeouts.cpp` | C4.11：慢而有进展的传输（6s = 3× 空闲超时）**不被整体超时**打断；空闲无进展 → **408**；普通 JSON 路由的整体超时**不外溢** |
| `tests/unit/test_composition_root_guard.cpp` + `scripts/verify_composition_root.sh` | C4.9：具体实现只能在 `src/main/`（含"限定名/`new`/静态工厂"形态与"纯引用不误报"的扫描器自证；组合根实测装配 7 种实现 → 非空洞性断言） |
| `tests/framework/http_fixture.h` | 两套 fixture：内存适配器（快）与 **`PosixStackFixture`（真实 POSIX + SQLite + 真实端口）**；装配逻辑抽成一个函数（`WireRouterAndServer`），避免"两个 fixture 各抄一份"再次漂移 |

### C4.11 的两条要求为什么必须一起测

| 只测一半 | 会漏掉的失败模式 |
| --- | --- |
| 只测"慢传输不被中断" | 把所有超时关掉的实现也能通过 → 卡住的客户端永久占着工作线程，C1.12 的背压静默失效 |
| 只测"空闲被断开" | 把整体超时设成 30s 的实现也能通过 → TB 级传输永远传不完（契约 §1.7 明确禁止对数据面做整体超时） |

实现上：socket 级 `SO_RCVTIMEO` 只能取一个值，因此取 `max(idle_timeout_seconds, transfer_idle_timeout_seconds)`，
另一个在 `CountingReceiver` 里**按块**检查；数据面的 `overall_timeout_ms` 显式为 **0**。
≥5 分钟的长跑版本见 `scripts/verify_transfer_no_timeout.sh`（不放进日常门槛，见 §2 的输出）。

---

## 1e. 切片 5 交付物（一手复核上游验收样例 → 逐字对齐）

切片 4 的留白不是"实现难"，而是**证据不足**。切片 5 在 `/home/ll/osdu-file-upstream`
（本机归档，commit `d7c25c2d7f5d2f42bed901c68a407098195389bb`）找到了上游的验收测试资源，
于是把 C4.3 从"消息要点"升级为"**逐字比对**"。

| 路径 | 内容 |
| --- | --- |
| `tests/conformance/fixtures/upstream/`（26 个文件 + README） | 上游的 `input_payloads/File_*.json` 与 `output_payloads/File_*_msg.json`，Apache-2.0，README 记录来源/commit/禁用行 |
| `docs/03-api-contract.md` §3.4 | 期望消息从"要点"改为**逐字**（并登记两条被上游注释掉的行） |
| `tests/conformance/test_rest_contract.cpp` | C4.3 改为**驱动真实上游样例**：装载 → 替换占位符 → POST → 逐字比对期望消息 |
| `src/domain/model/file_metadata.cpp` | 消息逐字对齐 + 枚举校验移到**解析阶段**（见 P4-D11） |
| `src/app/usecases/usecases.cpp` | 源路径不存在 → 上游消息 `Invalid source file path to copy from <path>`（P4-D10） |

### 三个由一手证据直接纠正的既有认知

| 之前的认知（我们的文档） | 一手证据 | 结果 |
| --- | --- | --- |
| "§3.4 有 **13** 个负向样例要实现" | 上游 Examples 表里 `File_invalid_ScalarIndicator` 与 `File_Datatype_Mismatch` 两行以 `#` 开头（`features/IntegrationTest_File_POST.feature` 第 24/25 行） | 上游在跑的是 **10** 条；两条禁用行我们**照实现并登记**（ScalarIndicator 逐字对齐，Datatype_Mismatch 只对齐状态码） |
| "`File_missing_data.json` 缺 `data`" | 样例里是 `"data": {}`（**空对象**，键存在） | 解析器把"缺失或空对象"统一为上游消息 `data cannot be empty` |
| "期望消息只需对齐要点"（如"acl 为空"） | `output_payloads/*_msg.json` 给出**完整文案**（如 `Record acl.viewers cannot be empty`） | 全部逐字对齐；客户端按 message 匹配时不再有兼容风险 |

### 校验时机为什么必须拆成两处（P4-D11）

上游把 `Endian` / `ScalarIndicator` 建模为带 `@JsonCreator` 的**枚举**，非法值在 Jackson
**反序列化**阶段就抛 `EnumValidationException`，早于 Bean Validation（`@NotNull`/`@ValidAcl`）。
这个顺序是**可观测的**：`File_invalid_Endian.json` 的 `FileSource` 同时是非法形状（`"string"`），
如果枚举校验留到校验阶段（排在 FileSource 之后），客户端拿到的会是
`Invalid source file path to copy from string`，而上游期望 `Invalid value of Small for Endian`。
因此：枚举检查进解析阶段，`@NotNull`/ACL/legal 留在校验阶段，两边都保留安全网（gRPC 入口）。

---

## 2. 门槛命令与输出

```console
$ cmake --build build -j8 && ctest --test-dir build -L phase4 --output-on-failure
    Start 34: test_http_error_mapper ...............   Passed    0.00 sec
    Start 35: test_ops_endpoints ...................   Passed    0.02 sec
    Start 36: test_http_dto ........................   Passed    0.00 sec
    Start 37: test_composition_root_guard ..........   Passed    0.56 sec
    Start 38: test_rest_contract ...................   Passed    0.07 sec
    Start 39: test_transfer_timeouts ...............   Passed   14.64 sec
    Start 40: test_error_formats ...................   Passed    0.03 sec
    Start 41: test_upload_flow .....................   Passed    0.80 sec
100% tests passed, 0 tests failed out of 8   （Total 16.13 sec）

逐测试断言数：
  test_http_error_mapper     133 assertions in 5 test cases   ← C4.5（适配层）
  test_http_dto               51 assertions in 7 test cases   ← C4.4（含 DMS/delivery 的 camelCase）
  test_ops_endpoints         102 assertions in 4 test cases   ← C4.6 + HTTP 侧
  test_rest_contract         396 assertions in 5 test cases   ← C4.1/C4.2/C4.3（上游样例逐字）/C4.7 + `/metrics`
  test_error_formats         172 assertions in 2 test cases   ← C4.5（HTTP 侧，三种形态）
  test_upload_flow           133 assertions in 4 test cases   ← C4.8（100 MiB + Range + 负例 + **真实 POSIX/SQLite 栈**）
  test_transfer_timeouts      54 assertions in 3 test cases   ← C4.11（无整体超时 + 空闲 408）
  test_composition_root_guard  17 assertions in 2 test cases  ← C4.9（R12 护栏）
  ─────────────────────────────────────────────
  合计 1058 个断言 / 32 个测试用例 / 8 个测试
```

### 组合根冒烟（真实进程 + 真实端口 + POSIX/SQLite 后端）

```console
$ FSS_HTTP_PORT=18080 FSS_STORAGE_ROOT=/tmp/fss-smoke ./build/bin/fss_server
{"level":"warn","msg":"auth.mode=disabled（allow-all）：仅用于开发/测试，禁止用于生产"}
fss_server 已启动
  bind           : 0.0.0.0:18080
  storage root   : /tmp/fss-smoke
  sqlite path    : /tmp/fss-smoke/location.db
  error format   : apperror

$ curl -i .../api/file/v2/liveness_check      → 200, Content-Type: text/plain, "File service is alive"
$ curl -i .../api/file/v2/info                → 200 (免鉴权)
$ curl -i -H 'authorization: Bearer t' -H 'data-partition-id: opendes' \
       '.../api/file/v2/files/uploadURL?expiryTime=2H' → 200
```

### C4.11 的 ≥5 分钟长跑（不放进日常门槛）

```console
$ ./scripts/verify_transfer_no_timeout.sh 330
  · 启动 fss_server（端口 18131，存储 /tmp/.../data）
  ✓ 服务就绪
  · 载荷 2703360 字节，限速 8192 B/s → 预计 ≈ 330 秒
  · 上行 SHA-256 7d567bb0da1761bede1b9a73c56a16d02d4a1ba454c49298b122c09b69f85c3e
  · PUT 完成：status=200，耗时 330s           ← ★ 超过 5 分钟未被整体超时打断
  ✓ 元数据已登记 / 下载后 SHA-256 一致
C4.11 长跑实测通过
```

> **这次长跑的直接产出不止一条证据**：它第一次用"真实 POSIX + SQLite + 真实 HTTP"跑完整条
> 上传→元数据→下载链路，立刻抓到 **P4-D09**（SQLite 仓储静默丢掉物理引用 → `POST metadata` 返回 **500**）。
> 修好后该链路成为常驻用例 `test_upload_flow` 的第 4 条（`PosixStackFixture`）。

### 收工全门槛

```console
$ ./scripts/check_docs.sh
  D4 已完成 ['0','1','2','3']，进行中 ['4']，门槛启用 ['0','1','2','3','4']   → 全部通过（D1~D5）

$ ./scripts/run_all_gates.sh
==> [docs] ✅ · [phase0] ✅ · [phase1] ✅ · [phase2] ✅ · [phase3] ✅ · [phase4] ✅ ctest
  失败: 无                                              （run_all_gates exit=0）

# sanitizer 覆盖到 phase4（含真实回环端口的 conformance 用例）：
  覆盖标签： phase0|phase1|phase2|phase3|phase4
  --- ctest -L phase4（sanitizer 构建，8 个测试：34~41） → 100% passed（27.75 sec）
      （sanitizer 下 test_upload_flow 用 FSS_TEST_BIG_BYTES=64MiB 缩小规模；该次运行不作为 C4.8 的字节数证据）
  --- scripts/verify_composition_root.sh（C4.9 护栏自证 ①~④） → 护栏有效
```

---

## 3. 判据进展

| 判据 | 状态 | 证据 |
| --- | --- | --- |
| **C4.1** 契约 §2 全部 19 个端点 | ✅（19/19 注册） | `test_rest_contract`：19 个端点 + `/metrics` 逐个用真实 HTTP 请求断言路径/方法/状态码（含 `§2.2` 传已存在 fileID → 400 的分支、`§2.6` 201 正例、`§2.8` DELETE 放最后以免污染其它行）；数据面 `PUT/GET /v1/transfer/:token` 另跑一遍真实字节 |
| **C4.2** 黄金样例全字段 | ✅ | `test_rest_contract`：契约 §3.3 的 `File_CorrectPayload.json`（只替换 `FileSource`）→ `POST` 201 → `GET` 200，与提交体做**语义全等比较**，仅允许 3 处契约规定改写（服务端 id、`version`、校验和 ×4）；另显式点名 12 个"最易丢"的开放字段 |
| **C4.3** 契约 §3.4 全部样例 | ✅ | `test_rest_contract`：**驱动 vendored 的上游样例**（`tests/conformance/fixtures/upstream/`），逐字断言期望消息 —— 10 条上游在跑的负向样例 + 2 条上游自己注释掉的行（ScalarIndicator **逐字对齐**、Datatype_Mismatch 状态码对齐）+ `File_Calculate_Checksum` 校验和被覆写（自证对照） |
| **C4.4** 字段大小写逐字符 | ✅ | `test_http_dto`（7 用例 / 51 断言）：§2.1 的 PascalCase 与 §2.9/§2.10 的 **camelCase 必须互不出现**；`SignedUrl` vs `SignedURL`；`expiryTime` 用 `+0000` 时间戳；`connectionString` 必须显式为 `null` |
| **C4.5** 三种错误体 + 固定消息 | ✅ | `test_http_error_mapper`（适配层，133 断言）+ `test_error_formats`（**HTTP 侧**，172 断言）：三种形态在同一端口各跑一遍、`code`/`message` 一致而外层不同、三种形态**互不相同**、`X-FSS-Error-Kind` 响应头、`Missing partitionID`/`Missing authorization token`/`Record Not Found`/`Location for fileID = x already exists`/`expiryTime pattern isn't supported...` 逐字节 |
| **C4.6** 健康检查纯文本 + `/v2/info` 免鉴权 | ✅ | `test_ops_endpoints`：`Content-Type` **恰好** `text/plain`；body 逐字节；`/v2/info` 无鉴权 200 |
| **C4.7** `expiryTime` 数值 | ✅ | `test_rest_contract`：缺省 → `now+3600s`（±5s **且**手动时钟下精确相等）；`2H` → `now+7200s`；`8D` 超上限 → **截断为 `now+604800s`**；`5X` → 400 + 固定消息（`uploadURL` 与 DMS 端点都验） |
| **C4.8** 端到端（POSIX） | ✅ | `test_upload_flow`（4 用例 / 133 断言）：100 MiB 上行/下行 **SHA-256 一致**、`Range` 尾部 206、0 字节边界、数据面负例 404/403/401/401；**第 4 条用真实 `PosixBlobStore` + `SqliteLocationRepository` 跑完整链路**（P4-D09 的常驻回归） |
| **C4.9** 组合根纪律（R12） | ✅ | `test_composition_root_guard`（扫描器自证 + 真实源码树扫描；组合根实测装配 **7** 种具体实现 → 非空洞性）+ `scripts/verify_composition_root.sh`（①基线通过 ②注入越权装配必须失败 ③移除后恢复 ④纯引用不误报） |
| **C4.10** sanitizer 干净 + phase0~3 全绿 | ✅ | `./scripts/run_all_gates.sh`：docs/phase0/1/2/3/4 全绿；ASan+UBSan+LSan 覆盖 `phase0|1|2|3|4` |
| **C4.11** 数据面无整体超时 + 空闲断开 | ✅ | `test_transfer_timeouts`（3 用例）：6s（=3×空闲超时）慢而有进展的传输 → **200**；空闲无进展 → **408** `timed out`；普通 JSON 路由的**整体**超时仍生效。长跑：`scripts/verify_transfer_no_timeout.sh` → **330s（>5 分钟）PUT 200 且 SHA-256 一致** |
| §7 `/metrics`（契约扩展） | ✅ | `test_rest_contract`：Prometheus 文本 + `version=0.0.4` + 计数行 + 直方图三件套 + 背压计数；并与 `HttpMetrics::RequestCount()` 两条观测路径互证 |

---

## 4. 本切片发现并修复的缺陷

| # | 缺陷 | 处置 |
| --- | --- | --- |
| P4-D01 | 适配层 namespace 与包装层同名：`namespace fss::adapters::http` 里的 `http::Response` 解析成自己 → 编译失败 | 显式写 `fss::http::Response`。教训：**同名 namespace 嵌套时，限定名必须从 `fss::` 写起** |
| **P4-D02** | **handler 闭包捕获的 `Router` 对象提前析构**：`Router` 是测试 fixture 构造函数里的**局部对象**，`Wrap()` 按 `this` 捕获它 → 构造返回后 `this` 悬空，第一个请求就 SIGSEGV。头两次运行表现为"所有端点都失败"，第三次才是崩溃 | `Router` 改成 fixture 的**成员**（生命周期覆盖 server）。教训：**任何被注册进长生命周期容器的回调，都必须证明其捕获物的生命周期更长**——这与 §4.3 的"回调在 handler 返回后执行"是同一类陷阱的另一面 |
| P4-D03 | `RawClient` 不带 `Content-Length`（这是它"允许构造畸形请求"的设计），测试漏补 → 带体 POST 被服务端读成下一个请求、连接被关（无响应） | 在测试的 `Do()` 里对非空体显式补 `Content-Length`。教训：**原始套接字客户端不会替调用方补协议细节**，这是它的价值也是它的代价 |
| **P4-D04** | **自签 URL 的 base 少了路由 base path**：组合根默认 `http://127.0.0.1:<port>`，而数据面挂在 `/api/file/v1/transfer/{token}`（契约 §2/§7）→ 发出去的上传地址 **404**。两个组件各自"对"，拼起来才错，单测抓不到 | 组合根默认值改为 `<host>:<port>` + base path，并把 base path 提成唯一来源 `kDefaultBasePath`（`router.h`），`RouterOptions` 默认值与组合根、测试三处共用 |
| **P4-D05** | **位置记录迁到 persistent 时只改了 `zone`，没改物理引用**：`extra.container` 仍是 staging 容器 → 自签下载 URL 去 staging 容器取对象 → 数据面返回 **200 + `Content-Length: 0`**（静默空文件），而不是 404 | `CreateFileMetadata` 在迁移时同步改写 `extra.container`/`extra.object_key` 为 `to_ref`。教训：**"逻辑位置"与"物理引用"是同一份记录的两个字段，改一个就必须改另一个** |
| **P4-D07** | **REST 元数据路径从未跑领域校验**：`CreateFileMetadata` 只查了 kind 与"FileSource 非空"，没调用 `ValidateMetadataRecord` → **非法 Endian 也能建记录（201）**、非法 ACL/legal 同样放行、非法 FileSource 路径退化成本地查找失败（消息不对）。负向矩阵用例一上手就抓到。副产物：`AppFixture::MakeRecord` 生成的 ACL 是 `viewer@example.com`（不合 §1.5 的 `data.*@` 正则）却一直"绿"—— 这本身证明校验从未被执行 | 在用例里显式调用 `domain::ValidateMetadataRecord`，并把两条固定消息映射到 `kFileSourceEmpty`/`kInvalidSourcePath`；解析阶段补"必需**段**缺失"检查（模型无法表达"段不存在"，否则 `File_missing_data.json` 会退化成"FileSource 为空"）；修正 fixture 使其生成契约合法的记录 |
| **P4-D08** | **`HandleBodyError` 的判定顺序让"超时"被误报成"长度不符"**：卡住的 PUT 同时满足"读失败"与"声明长度没读满"，原顺序先命中 `content-length mismatch` → 客户端与运维看到 400 去查客户端/网络，而根因是超时 | 把"空闲超时"判定提到长度不符之前 → **408 + `timed out`**；另外修掉"库在读失败时回调用零长度把 `last_progress` 刷新"的问题（否则空闲判定恒算出 0ms） |
| **P4-D10** | **"源路径不存在"给出的是中文消息**：`CreateFileMetadata` 在位置记录查不到时返回 `FileSource 没有对应的位置记录：<path>`，而上游（provider 的 copy 失败分支）给的是 `Invalid source file path to copy from <path>`。同为 400，但客户端按 message 匹配就不兼容 | 改为 `kInvalidSourcePath` + 上游**逐字**消息。触发它的正是上游样例 `File_invalid_fileSource.json`（其 `FileSource` 形状合法、只是不存在）——此前 C4.3 用"形状非法"的自造输入掩盖了这个差别 |
| **P4-D11** | **枚举校验的时机错了（顺序可观测）**：把 `Endian` 校验留在 `ValidateMetadataRecord`（排在 FileSource 检查之后），于是 `File_invalid_Endian.json` 报出的是 `Invalid source file path to copy from string` —— 因为该样例的 FileSource 同时非法。上游把枚举做成 `@JsonCreator`，非法值在**反序列化**时就失败 | 枚举检查（`Endian` / `ScalarIndicator`）移到 `ParseFileMetadataRecord`；`@NotNull`/ACL/legal 留在校验阶段；两边保留安全网以覆盖非 JSON 入口 |
| **P4-D09** | **SQLite 位置仓储静默丢掉物理引用**：`DumpLocation` 把 `extra` 的键**摊平**写到顶层，而 `KnownKeys()` 又把 `container`/`object_key` 当"已知键" → 回读时既进不了 `extra`、也没有领域字段接住 → `ObjectRefFromLocation` 报"位置记录缺少物理引用" → **`POST metadata` 500**。内存适配器永远发现不了它；契约测试也没覆盖（当时只用自造的 `CustomField` 测 extra 往返） | 从 `KnownKeys()` 移除这两个键（并写明理由）；**同时**把这两个真实键加进 `port_contract.h` 的往返用例 —— 这样 memory/sqlite/postgres 三个实现都会被同一份断言钉住（R16：原来的契约测试是"有洞的"） |
| **P4-D06** | **对象不存在被当成 0 字节对象**：`TransferEndpoint::Stat` 原样返回 `exists=false, size=0`，下载路由据此给出 200 空体 —— 比 404 危险（客户端会把"文件没了"读成"文件是空的"） | `Stat` 在 `!exists` 时返回 `kNotFound`（→ 404）。自证对照：P4-D05 未修时该断言表现为 200 + 0 字节，修好 D05 后补了"对象不存在 → 404"用例（90 断言中的 `missing.status == 404`）作为该分支的正例断言（R16） |

---

## 5. 结论

| 项 | 结论 |
| --- | --- |
| 切片 1 门槛（错误映射 + DTO） | ✅ `test_http_error_mapper` + `test_http_dto` |
| 切片 2 门槛（中间件 + 路由 + 组合根） | ✅ `test_ops_endpoints` 4 用例 / 102 断言 |
| 切片 3 门槛（自签数据面 + 真实字节） | ✅ `test_upload_flow` 前 3 用例（100 MiB SHA-256 一致 + Range 206 + 负例） |
| 切片 4 门槛（19 端点 + 契约矩阵 + 护栏 + 超时） | ✅ `test_rest_contract` / `test_error_formats` / `test_transfer_timeouts` / `test_composition_root_guard` |
| 切片 5 门槛（上游样例逐字对齐） | ✅ C4.3 改为驱动 vendored 上游样例 + 逐字期望消息；`ctest -L phase4` 合计 **8 测试 / 1058 断言 / 32 用例** |
| 已满足判据 | **C4.1 ~ C4.11 全部满足** |
| 未验证/无结论 | 无。唯一的"消息不对齐"是 `File_Datatype_Mismatch.json`（上游自己注释掉的行）：状态码已对齐、消息更具体，已显式登记 |
| 其他已知偏离（不影响判据，如实登记） | ① `src/common/http/server.cpp` 用具体 `UuidGenerator ids;` 生成相关性 ID，而不是注入 `IIdGenerator`（L1 内部工具，C4.9 范围外，已在护栏测试里注明）；② 组合根当前**按环境变量直读**配置，尚未接 `config/fss.example.json` 的 `config::Load()`（该文件的 schema/校验在 P1 已就绪；接线计划在 P9"硬化与交付"） |
| P4 是否收口 | ✅ **收口**：C4.1~C4.11 全部满足；`check_docs.sh` + `run_all_gates.sh` 全绿（含 ASan/UBSan/LSan）。证据命令、输出与缺陷记录即本文档 |
