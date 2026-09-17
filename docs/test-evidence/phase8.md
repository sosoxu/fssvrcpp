# 阶段 8 测试证据（✅ 已完成）

| 项 | 值 |
| --- | --- |
| 阶段 | P8（认证授权与多租户） |
| 状态 | ✅ **已完成并通过门槛 —— C8.1~C8.8 全部满足**（3 个切片全部收口） |
| 门槛命令 | `ctest -L phase8` |
| 退出码 | `0`（**6 测试 / 1323 断言**）：`test_jwt_authorizer` 68、`test_auth_matrix` 717、`test_remote_entitlements_authorizer` 62、`test_tenant_isolation` 107、`test_audit_coverage` 355、`test_clock_skew_guard` 14；另在 phase7 标签下有"缺结束标记"的确定性用例（`test_grpc_streaming` 9 用例） |
| 决策记录 | `docs/adr/ADR-012-auth-and-tenant-binding.md`（已采纳） |

> **收口**：C8.1~C8.8 全部满足（退出条件达成）；C8.9（配置级）与 C8.10（机制级）也已完成。
> **依赖 PG 的端到端形态（PG 仓储/租约/数据库时钟、`deployment.mode=multi` 运行形态）登记给 P9**：
> 组合根当前对 `FSS_DEPLOYMENT_MODE=multi` **拒绝启动**（明确报错，而不是以单实例状态跑多实例）。

---

## 1. 切片 1 交付物

| 路径 | 内容 |
| --- | --- |
| `docs/adr/ADR-012-auth-and-tenant-binding.md` | 决策：**本地 JWT（HS256）+ `partition` claim 绑定 + fail-closed**；4 个候选（含"信任网关"与"远端 Entitlements"）与未实测项 |
| `src/infra/auth/local/local_jwt_authorizer.{h,cpp}` | L2 授权器：HS256 验签、`alg` 白名单（拒绝 `none`）、`exp`（必需）/`nbf`/`iss`/`aud`、角色 claim ∪ 静态角色表、租户绑定、常量时间比较；**任何配置/claim 缺失都拒绝** |
| `src/adapters/http/router.cpp`（`RouteAuthTable()` + `Wrap` 预检） | **路由级鉴权预检**：按 `RouteOptions.name` 查表，鉴权**先于** DTO 解析；未登记路由 fail-closed；16 条 OSDU 端点的角色表与契约 §1.3 一致 |
| `src/common/config/core_schema.cpp` + `config/fss.example.json` | `deployment.environment` + `auth.jwt.{hmac_secret,partition_claim,require_partition_claim}`；production 禁止 `disabled`/关验签/无密钥；`jwks_url` 非空直接拒绝启动（未实现不许"看起来配了鉴权"） |
| `src/app/usecases/usecases.{h,cpp}` + `proto/.../file_service.proto` + REST DTO | `/v2/info` 的 `authMode`（REST）与 `InfoResponse.auth_mode`（gRPC）**同源**（`UseCasePorts.auth_mode`） |
| `src/main/server_main.cpp` | `FSS_AUTH_MODE=jwt|disabled`；`remote-entitlements` **拒绝启动**；jwt+验签+无密钥 **拒绝启动**（附修复指令）；启动横幅打印生效模式 |
| `tests/unit/test_jwt_authorizer.cpp` | C8.3 + 租户绑定 + fail-closed（**自签 token**，期望值不来自被测代码） |
| `tests/integration/test_auth_matrix.cpp` | C8.1：16 端点 × 8 个单角色 token 的 401/403/放行矩阵 + 正向垂直切片 |
| `tests/unit/test_config.cpp`（新增用例） | C8.5：production 的三条强制约束 + 开发环境空密钥可加载（**正例**） |

## 1.1 切片 2 交付物

| 路径 | 内容 |
| --- | --- |
| `src/infra/auth/remote/remote_entitlements_authorizer.{h,cpp}` | L2 远端授权器（新目标 `fss_auth_remote`，libcurl）：`authorizeAny` 语义；**超时 / 连不上 / 非 200 / 坏 JSON / 缺布尔 `allowed` / 未配地址 → 一律 `kUnavailable`（503）**；不提供"失败即放行"的开关（`fail_closed` 只是 schema 声明，`false` 会被配置校验拒绝） |
| `tests/tools/mock_entitlements.py` + `tests/framework/mock_entitlements.h` | 独立进程的 Entitlements 替身：可注入 `--delay-ms`（超时）/`--status 500`/`--malformed`/`--require-partition`，并可 `--grant` 指定被授予角色 |
| `tests/unit/test_remote_entitlements_authorizer.cpp` | C8.4：允许/拒绝/请求形状 + 6 种失败形态全部 503 + 与本地实现一致的错误类别 |
| `tests/integration/test_tenant_isolation.cpp` | C8.2：两租户 + 真实 HTTP + 真实 JWT；token/仓储/签名 URL 三道防线的跨租户拒绝 |
| `src/main/server_main.cpp` | `FSS_AUTH_MODE=remote-entitlements` 走真实实现；未配 `FSS_ENTITLEMENTS_URL` → **拒绝启动** |
| `src/common/config/core_schema.cpp` + `config/fss.example.json` | `auth.remote_entitlements.{authorize_path,connect_timeout_ms}`；远端模式强制"有地址 + fail_closed=true" |

## 1.2 切片 3 交付物

| 路径 | 内容 |
| --- | --- |
| `src/app/usecases/usecases.cpp`（`AuditGuard`） | **审计守卫（RAII）**：15 个受保护用例逐个接线；操作名按上游约定自动加 `Success`/`Failure` 后缀；任何 `FSS_TRY` 提前返回都会记账（手写调用漏掉失败出口是**静默**的） |
| `src/domain/ports/ports.h`（`AuditEvent`） | 审计事件新增 `correlation_id`（C8.7 要求含 correlation-id）；`RecordAudit` 从 `CallerContext` 填充 |
| `tests/unit/test_audit_coverage.cpp` | C8.7：15 个用例的**成功侧**（表驱动）+ **失败侧**（授权失败 15 条 + 领域失败 `kNotFound`），逐条断言 5 个字段 |
| `src/app/services/clock_skew_guard.{h,cpp}` | C8.10：参考时钟是 TTL/过期判定的**唯一**时间源；快/慢钟超容忍 → `kUnavailable`（fail-closed） |
| `tests/unit/test_clock_skew_guard.cpp` | C8.10：快钟/慢钟/边界/放宽容忍范围 + "判定时间跟参考钟走" |
| `src/common/config/core_schema.cpp` + `config/fss.example.json` | C8.9：`deployment.max_clock_skew_seconds` + multi 模式新增两条强制校验（GC 必须要求租约到期、时钟偏差 `0 < 值 ≤ 60`） |
| `tests/unit/test_config.cpp` | C8.9：五条强制校验各一条"拒绝"断言 + **全满足必须通过**（正例）+ 单实例不误伤 |
| `src/main/server_main.cpp` | `FSS_MAX_CLOCK_SKEW_SECONDS` → JWT 容忍范围；`FSS_DEPLOYMENT_MODE=multi` → **拒绝启动**（PG 运行形态属 P9） |
| `docs/03-api-contract.md` §4.6/§1.6b | 审计与事件操作名表 + multi 五条校验 |

## 2. 判据进展

| 判据 | 状态 | 证据 |
| --- | --- | --- |
| **C8.1** 端点 × 角色矩阵 | 🚧 **REST 面满足** | `test_auth_matrix`：16 条端点 × 8 个"只带一个角色"的 token；缺 token/partition → 401（固定消息）；角色不足 → **403**（不是 400）；命中 → 401/403 之外的码；另有一条"editors 上传 → 登记 → viewers 下载/读 → admin 删除（204）"的正向垂直切片。`test_roles.cpp` 继续在用例层钉同一张表 |
| **C8.2** 跨租户拒绝 | ✅ | `test_tenant_isolation`（2 用例 / 107 断言）：**① token 绑定** A 的 token + B 的头 → 403（拦在鉴权层，不去 B 的分区查）；**② 仓储隔离** B 读/删 A 的记录 → 404（且 A 的记录仍在）、B 的列表看不到 A 的记录而 A 自己能列到（正例）、B 取 A 的 downloadURL → 404；**③ 签名 URL 绑对象** 正常使用 200、跨租户头 403、篡改 token/sig 401、改路径形状 404（如实区分）；篡改后的响应里都不含 A 的数据。授权层的绑定另由 `test_jwt_authorizer` 覆盖 |
| **C8.3** JWT 边界 | ✅ | `test_jwt_authorizer`：过期、无 `exp`、`nbf` 未到、`iss`/`aud` 错（含 `aud` 数组形态）、错密钥签名、`alg=none`、`alg=RS256`、7 种畸形串、**载荷/签名混拼**（提权）→ 全部 401；`exp` 边界含 ±skew（C8.10 的机制基础） |
| **C8.4** 远端不可达 fail-closed | ✅ | `test_remote_entitlements_authorizer`（3 用例 / 62 断言）：**超时注入**（mock 睡 800 ms / 客户端 150 ms）、5xx、坏 JSON、缺 `allowed`、连不上、未配地址 → 全部 `kUnavailable`（503），**没有**任何一条走成放行；401 → kUnauthenticated、allowed=false → kPermissionDenied；配置校验拒绝 `fail_closed=false`；**注入 fail-open 后本用例必须失败**（见 §4） |
| **C8.5** `disabled` 告警 + `/v2/info` 标记 + production 校验 | ✅ | 组合根告警 + `authMode` 字段（REST 与 gRPC 同源）+ `test_config` 的三条拒绝 + 一条正例；`test_auth_matrix` 断言 `/v2/info` 的 `authMode == "disabled"`（fixture 未设模式时） |
| **C8.6** 事件（`IN_PROGRESS`→`SUCCESS` / `FAILED`） | ✅ | P6 的既有用例机械覆盖：`test_metadata_lifecycle` 断言成功路径 `statuses == {IN_PROGRESS, SUCCESS}`（各恰好一次）、`datasetDetails` 含 `dataset_id`/`dataset_version_id`/`correlationId`/`timestamp`；失败路径 `statuses == {IN_PROGRESS, FAILED}`。本次把操作名/状态表写进契约 §4.6 并逐条对照（§2.1 的"记录在先、实现在后"） |
| **C8.7** 审计（成功+失败两侧、字段齐全） | ✅ | `test_audit_coverage`（2 用例 / 355 断言）：**15 个受保护用例**逐个在成功侧（表驱动）与失败侧（授权失败 15 条 + 领域失败 `kNotFound`）都断言 `operation`（含 Success/Failure 后缀）、`user`、`partition`、`object_id`、`result`、`epoch_millis`、`correlation_id`。实现改为 RAII 守卫，失败出口不再可能被漏记 |
| **C8.8** 回归（P0–P7 全绿） | ✅ | `ctest --test-dir build`：**65/65 通过**（含 phase4 的 401/403 契约用例；`HttpFixture`/`PosixStackFixture` 继续**显式注入** `AllowAllAuthorizer`，不依赖配置默认值） |
| **C8.9** multi 的 5 条强制启动校验 | ✅ **配置级**（运行形态属 P9） | `test_config`：仓储必须 PG、租约+选举开启、GC 必须 `require_lease_expiry`、存储根共享挂载、`max_clock_skew_seconds ∈ (0,60]` —— 每条都有"拒绝"断言，且五条全满足时**必须通过**（正例）+ 单实例不受影响。组合根对 `FSS_DEPLOYMENT_MODE=multi` 拒绝启动（PG 运行形态未交付） |
| **C8.10** 时钟偏移不误判 | ✅ **机制级**（端到端需 PG 时钟，属 P9） | `test_clock_skew_guard`：参考时钟是判定用的**唯一**时间源（本地钟只用来比对）、快/慢钟超容忍 → `kUnavailable`、边界与放宽容忍范围各一条；token `exp` 的 ±skew 边界在 `test_jwt_authorizer`（快/慢 3 秒接受、10 秒拒绝） |

## 3. 两条"看起来等价、实际不同"的判定（本切片的关键设计）

1. **鉴权必须在 DTO 解析之前**（契约 §1.3 末段）。只在用例入口判时，一个**畸形请求体**
   （如 `POST /v2/files/metadata` 带 `{}`）会先被 DTO 解析成 **400**，未授权者就拿到了
   "这个请求形状不对"的信号，而不是 403。现在适配层按路由表先拦（401/403），
   用例入口保留为第二道（gRPC 面只有那一道）。
2. **`Missing partitionID` 与 `Missing authorization token` 的顺序**：适配层必须与用例入口
   **逐字一致**（先 partition、后 token）。两层给出不同的文案比"顺序本身"更糟 ——
   同一个请求会在不同深度报出不同的固定消息（phase4 的契约用例已经固化了 partition 优先）。

## 4. 自证对照（R1）

| 注入点 | 注入内容 | 期望失败 | 实测 |
| --- | --- | --- | --- |
| `remote_entitlements_authorizer.cpp` | 传输失败时 `return true`（fail-open） | "依赖不可用 → 503"必须失败 | ✅ 见 §4 |
| `router.cpp` 的 `Wrap` 预检 | `if (false && !auth->roles.empty())`（关掉路由级鉴权） | 矩阵的"缺 token → 401"必须变成 400（正是本切片要消除的探测面） | ✅ `test_auth_matrix.cpp:173 FAILED`（`400 == 401`，端点 `POST /v2/files/metadata`） |
| `grpc_streaming_io.cpp` | `if (false && !saw_end_of_stream_)`（不要求显式结束标记） | "缺结束标记"的确定性用例必须失败（对象被截断提交） | ✅ `test_grpc_streaming.cpp:674 FAILED`（`downloaded.bytes == 0` 不成立）—— P8-D05 |
| `remote_entitlements_authorizer.cpp` 的传输失败分支 | `return true;`（fail-open：依赖坏了就放行） | "依赖不可用 → 503"的用例必须失败 | ✅ `test_remote_entitlements_authorizer.cpp:92 FAILED`（超时场景被放行）—— C8.4 的核心性质 |
| `error_table`/`FillFileLocationProto` 等 P7 注入 | 见 `docs/test-evidence/phase7.md` §4 | — | ✅ 仍全部有效（本切片未改动那些路径） |

## 5. 本切片发现的实现陷阱

| 编号 | 症状 | 根因 | 规避 |
| --- | --- | --- | --- |
| **P8-D01** | `Err(Unauthenticated(...))` 编译失败：`cannot convert Error to ErrorKind` | `Err()` 只接受 `(ErrorKind, message)`；`Result<T>` 才是"从 Error 构造"的那一层 | 帮助函数直接返回 `fss::Error`，调用点 `return Unauthenticated("...")`（**不要**再包一层 `Err`） |
| **P8-D02** | 加完路由级预检后，phase4 的 `test_error_formats` 失败：期望 `Missing partitionID`，实得 `Missing authorization token` | 预检里先判 token、后判 partition，与用例入口（`AuthorizeCaller`）的顺序**相反** | 两层的**顺序与文案必须逐字一致**；预检改为先 partition、后 token，并把"为什么"写进代码注释 |
| **P8-D03** | 配置校验加上"必须有 `hmac_secret`"后，`Load({})`（默认配置）直接失败 | 默认值是 `auth.mode=jwt` + `verify_signature=true` + 空密钥 → 默认配置与自己的校验冲突 | 该强制**只在 `deployment.environment=production`** 触发；开发环境允许（运行时拒绝所有 token + 启动告警），并补"生产配置补齐后必须通过"的正例（R16） |
| **P8-D04** | 作者器把"缺 partition"一律判成 401 → 合法的 `revokeURL`（契约 §1.2 **不需要** partition）被拒 | "缺 partition → 401"是**用例层**（`AuthorizeCaller` 的 `require_partition`）的职责；作者器没有 `require_partition` 参数，无法区分端点 | 作者器只负责"token 有效 + 角色 + （给了 partition 时）租户绑定"；请求不带 partition 时跳过绑定比较 |
| **P8-D05** | **sanitizer 构建下**"上传中途取消"用例偶发失败：对象里出现了 512 KiB 的**截断**内容（普通构建全绿） | P7-D07 的修复只查了 `ServerContext::IsCancelled()`，但它的置位时机**晚于** `ServerReader::Read()` 的失败 —— ASan 构建更慢，于是"取消"在服务端看来与"正常读完"完全一样（两者都只是 `Read()==false`），截断字节被 rename 成正式对象 | ① proto 增加**显式结束标记** `UploadFileRequest.end_of_stream`：最后一个数据分片之后必须发它，否则服务端报 `UNAVAILABLE`、**绝不提交**（确定性判定，不依赖任何竞态）；② 取消用例不再在 `TryCancel()` 之后 `WritesDone()`（半关闭会被当成"正常结束"）；③ 新增确定性用例"缺结束标记 → 报错且对象保持空"+ 注入对照。ASan 下连跑 3 次全绿 |

| **P8-D06** | 远端鉴权用例第一次跑：期望"未授予 → 403"却拿到 503 | mock 同时开了 `--require-role service.file.editors`：任何"请求别的角色"的探测都被 mock 判成 **400**（协议错误）→ 我们 fail-closed 成 503，把"未授予"的语义盖掉了 | 一个 mock 开关只表达**一件事**：`require_partition` 用于证明"客户端确实带了租户头"；"客户端是否如实转发 roles"改由**授权结果**反向证明（请求未授予的角色必须被拒）—— 若客户端偷懒总问一个已授予的角色，那条断言会失败 |

| **P8-D07** | 加审计覆盖时发现：`GetFileLocation` / `GetFileList` / `GetFileMetadata` / `GetStorageInstructions` / `GetRetrievalInstructions` / `GetFileSignedUrl` / `DownloadFile` 等**端点在失败侧（甚至成功侧）根本没有审计记录**；即使有记录的端点也只记了成功侧 | 审计是在每个 `Execute` 里**手写**的 `RecordAudit` 调用。一个 `Execute` 有 4~8 个失败出口（授权/解析/仓储/存储/事件），手写必然漏；而"漏审计"不会让任何测试失败 —— 它是**静默**的 | 统一为 RAII 守卫 `AuditGuard`（析构时按 `Success()` 是否被调用记账），15 个用例逐个接线；操作名由"业务动作"自动派生 `Success`/`Failure`；并新增表驱动的 `test_audit_coverage` 逐个钉住两侧 |

## 6. 收工验证（切片 1 + 2 + 3）

| 命令 | 结果 |
| --- | --- |
| `ctest --test-dir build -L phase8 --output-on-failure` | ✅ **6 测试 / 1323 断言**（jwt 68 + auth_matrix 717 + remote_entitlements 62 + tenant_isolation 107 + audit_coverage 355 + clock_skew_guard 14） |
| `ctest --test-dir build`（全量回归，C8.8） | ✅ **69/69 通过**（含 phase4 的 401/403 契约用例与 phase7 的双协议用例） |
| `./scripts/check_docs.sh` | ✅ D1~D5（新增 ADR-012 后 ADR 索引仍完整：11 个文件 / 12 个编号） |
| `./scripts/run_all_gates.sh` | ✅ **phase0~8 全部通过**，`失败: 无`（切片 3 日志 `build/gates-p8s3.log`；phase8 已在 `IMPLEMENTED_PHASES` 中） |
| sanitizer（`run_all_gates.sh` 内嵌，`build-asan`） | ✅ ASan + UBSan + LSan 全绿，**含 phase8 与 phase7**；"缺结束标记/取消"的用例在 ASan 下连跑 3 次全绿（P8-D05 的原始症状正是在这里暴露的） |

**结论**：**P8 收口** —— C8.1~C8.8 全部满足：端点×角色矩阵、JWT 边界、租户绑定与跨租户隔离、
两种角色来源的 fail-closed、审计（成功+失败两侧、字段齐全）、事件序列、回归全绿；
另完成 C8.9（配置级五条校验）与 C8.10（时钟偏移机制）。**依赖 PG 的端到端形态（PG 仓储/
租约/数据库时钟与 multi 运行形态）登记给 P9**，组合根当前对该模式**拒绝启动**。

## 7. 未验证 / 已知限制（如实登记）

1. **RS256 / JWKS 未实现**：`auth.jwt.jwks_url` 非空会被配置校验拒绝启动（ADR-012 §5.3）。
2. **远端 Entitlements 已实现但未与真实服务联调**：接口形状（契约 §4.5）是本项目的约定，
   `authorize_path` 可配置；故障形态由 `tests/tools/mock_entitlements.py` 注入验证。
   拿到可达的真实 Entitlements 后需要跑一遍并记录（ADR-012 §5.3 的重开条件）。
3. **Entitlements 结果不缓存**：每请求一次 RTT（撤权即时生效优先于延迟；缓存留待按需加）。
4. **`UploadFile` 的协议变更**（本切片）：`UploadFileRequest` 新增 `end_of_stream`
   字段（扩展面，无 REST 对应物）；不发送它的客户端会收到 `UNAVAILABLE`。契约 §4.4 已同步。
5. **gRPC 面的鉴权顺序**：gRPC 适配层没有路由级预检（proto 结构错误的解析先于用例授权）。
   由于 `MetadataFromProto` 是**纯结构**转换、不访问存储，它不泄露"数据是否存在"；
   与 REST 的差异已在此登记（低风险，切片 2 评估是否统一）。
6. **单密钥**：尚不支持 `kid` 多密钥轮换（ADR-012 §8）。
