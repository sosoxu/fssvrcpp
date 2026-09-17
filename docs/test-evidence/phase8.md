# 阶段 8 测试证据（🚧 进行中）

| 项 | 值 |
| --- | --- |
| 阶段 | P8（认证授权与多租户） |
| 状态 | 🚧 **进行中 —— 切片 1/3 完成**（本地 JWT + 路由级预检 + 租户绑定 + 配置强校验） |
| 门槛命令 | `ctest -L phase8` |
| 退出码 | `0`（phase8 标签 **2 测试 / 785 断言**：`test_jwt_authorizer` 6 用例 / 68、`test_auth_matrix` 3 用例 / 717；另在 phase7 标签下新增"缺结束标记"确定性用例，`test_grpc_streaming` 9 用例） |
| 决策记录 | `docs/adr/ADR-012-auth-and-tenant-binding.md`（已采纳） |

> **剩余**：切片 2 = 跨租户隔离的独立证据（C8.2，多分区仓储 + 签名 URL 绑定）+ 远端
> Entitlements 的 fail-closed（C8.4，含超时注入）；切片 3 = 事件（C8.6）+ 审计（C8.7）+
> `deployment.mode=multi` 的 5 条启动校验（C8.9）+ 时钟偏移（C8.10）。

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

## 2. 判据进展

| 判据 | 状态 | 证据 |
| --- | --- | --- |
| **C8.1** 端点 × 角色矩阵 | 🚧 **REST 面满足** | `test_auth_matrix`：16 条端点 × 8 个"只带一个角色"的 token；缺 token/partition → 401（固定消息）；角色不足 → **403**（不是 400）；命中 → 401/403 之外的码；另有一条"editors 上传 → 登记 → viewers 下载/读 → admin 删除（204）"的正向垂直切片。`test_roles.cpp` 继续在用例层钉同一张表 |
| **C8.2** 跨租户拒绝 | 🚧 **部分**（授权层的绑定已测；多分区仓储 + 签名 URL 的独立用例在切片 2） | `test_jwt_authorizer`：A 租户 token + B 请求头 → `403`；无 partition claim → `401`；显式关闭绑定则只看请求头（部署形态可见）。仓储层 `partition_id` 隔离由 P3/P6 的端口契约与 SQL 护栏覆盖 |
| **C8.3** JWT 边界 | ✅ | `test_jwt_authorizer`：过期、无 `exp`、`nbf` 未到、`iss`/`aud` 错（含 `aud` 数组形态）、错密钥签名、`alg=none`、`alg=RS256`、7 种畸形串、**载荷/签名混拼**（提权）→ 全部 401；`exp` 边界含 ±skew（C8.10 的机制基础） |
| **C8.4** 远端不可达 fail-closed | ⬜ 未实现（切片 2） | `auth.mode=remote-entitlements` 目前**拒绝启动**（`main` 明确报错），已登记在 ADR-012 §5.3 |
| **C8.5** `disabled` 告警 + `/v2/info` 标记 + production 校验 | ✅ | 组合根告警 + `authMode` 字段（REST 与 gRPC 同源）+ `test_config` 的三条拒绝 + 一条正例；`test_auth_matrix` 断言 `/v2/info` 的 `authMode == "disabled"`（fixture 未设模式时） |
| **C8.6** 事件 / **C8.7** 审计 | ⬜ 未开始（切片 3；P6 已实现事件与审计的**注入点**，这里补"真实鉴权下的完整覆盖"） | — |
| **C8.8** 回归（P0–P7 全绿） | ✅ | `ctest --test-dir build`：**65/65 通过**（含 phase4 的 401/403 契约用例；`HttpFixture`/`PosixStackFixture` 继续**显式注入** `AllowAllAuthorizer`，不依赖配置默认值） |
| **C8.9** multi 启动校验 / **C8.10** 时钟偏移 | ⬜ 未开始（切片 3） | 机制已就位：授权器的 `clock_skew_seconds` 与 `IClock` 注入 |

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
| `router.cpp` 的 `Wrap` 预检 | `if (false && !auth->roles.empty())`（关掉路由级鉴权） | 矩阵的"缺 token → 401"必须变成 400（正是本切片要消除的探测面） | ✅ `test_auth_matrix.cpp:173 FAILED`（`400 == 401`，端点 `POST /v2/files/metadata`） |
| `grpc_streaming_io.cpp` | `if (false && !saw_end_of_stream_)`（不要求显式结束标记） | "缺结束标记"的确定性用例必须失败（对象被截断提交） | ✅ `test_grpc_streaming.cpp:674 FAILED`（`downloaded.bytes == 0` 不成立）—— P8-D05 |
| `error_table`/`FillFileLocationProto` 等 P7 注入 | 见 `docs/test-evidence/phase7.md` §4 | — | ✅ 仍全部有效（本切片未改动那些路径） |

## 5. 本切片发现的实现陷阱

| 编号 | 症状 | 根因 | 规避 |
| --- | --- | --- | --- |
| **P8-D01** | `Err(Unauthenticated(...))` 编译失败：`cannot convert Error to ErrorKind` | `Err()` 只接受 `(ErrorKind, message)`；`Result<T>` 才是"从 Error 构造"的那一层 | 帮助函数直接返回 `fss::Error`，调用点 `return Unauthenticated("...")`（**不要**再包一层 `Err`） |
| **P8-D02** | 加完路由级预检后，phase4 的 `test_error_formats` 失败：期望 `Missing partitionID`，实得 `Missing authorization token` | 预检里先判 token、后判 partition，与用例入口（`AuthorizeCaller`）的顺序**相反** | 两层的**顺序与文案必须逐字一致**；预检改为先 partition、后 token，并把"为什么"写进代码注释 |
| **P8-D03** | 配置校验加上"必须有 `hmac_secret`"后，`Load({})`（默认配置）直接失败 | 默认值是 `auth.mode=jwt` + `verify_signature=true` + 空密钥 → 默认配置与自己的校验冲突 | 该强制**只在 `deployment.environment=production`** 触发；开发环境允许（运行时拒绝所有 token + 启动告警），并补"生产配置补齐后必须通过"的正例（R16） |
| **P8-D04** | 作者器把"缺 partition"一律判成 401 → 合法的 `revokeURL`（契约 §1.2 **不需要** partition）被拒 | "缺 partition → 401"是**用例层**（`AuthorizeCaller` 的 `require_partition`）的职责；作者器没有 `require_partition` 参数，无法区分端点 | 作者器只负责"token 有效 + 角色 + （给了 partition 时）租户绑定"；请求不带 partition 时跳过绑定比较 |
| **P8-D05** | **sanitizer 构建下**"上传中途取消"用例偶发失败：对象里出现了 512 KiB 的**截断**内容（普通构建全绿） | P7-D07 的修复只查了 `ServerContext::IsCancelled()`，但它的置位时机**晚于** `ServerReader::Read()` 的失败 —— ASan 构建更慢，于是"取消"在服务端看来与"正常读完"完全一样（两者都只是 `Read()==false`），截断字节被 rename 成正式对象 | ① proto 增加**显式结束标记** `UploadFileRequest.end_of_stream`：最后一个数据分片之后必须发它，否则服务端报 `UNAVAILABLE`、**绝不提交**（确定性判定，不依赖任何竞态）；② 取消用例不再在 `TryCancel()` 之后 `WritesDone()`（半关闭会被当成"正常结束"）；③ 新增确定性用例"缺结束标记 → 报错且对象保持空"+ 注入对照。ASan 下连跑 3 次全绿 |

## 6. 收工验证（切片 1）

| 命令 | 结果 |
| --- | --- |
| `ctest --test-dir build -L phase8 --output-on-failure` | ✅ **2 测试 / 785 断言**（`test_jwt_authorizer` 68 + `test_auth_matrix` 717） |
| `ctest --test-dir build`（全量回归，C8.8） | ✅ **65/65 通过**（含 phase4 的 401/403 契约用例与 phase7 的双协议用例） |
| `./scripts/check_docs.sh` | ✅ D1~D5（新增 ADR-012 后 ADR 索引仍完整：11 个文件 / 12 个编号） |
| `./scripts/run_all_gates.sh` | ✅ **phase0~8 全部通过**，`失败: 无`（日志 `build/gates-p8s1b.log`）；phase8 已加入 `IMPLEMENTED_PHASES` |
| sanitizer（`run_all_gates.sh` 内嵌，`build-asan`） | ✅ ASan + UBSan + LSan 全绿，**含 phase8 与 phase7**；"缺结束标记/取消"的用例在 ASan 下连跑 3 次全绿（P8-D05 的原始症状正是在这里暴露的） |

**结论**：切片 1 完成 —— REST 面的端点 × 角色矩阵、JWT 边界、租户绑定、fail-closed、
配置强校验、`authMode` 可见性均已落地并有机械证据。**剩余切片 2/3** 见开头"剩余"。

## 7. 未验证 / 已知限制（如实登记）

1. **RS256 / JWKS 未实现**：`auth.jwt.jwks_url` 非空会被配置校验拒绝启动（ADR-012 §5.3）。
2. **远端 Entitlements 未实现**：`auth.mode=remote-entitlements` 拒绝启动，**不是**静默放行。
3. **C8.4 的"超时注入"**：随远端实现一起做（切片 2）。
4. **`UploadFile` 的协议变更**（本切片）：`UploadFileRequest` 新增 `end_of_stream`
   字段（扩展面，无 REST 对应物）；不发送它的客户端会收到 `UNAVAILABLE`。契约 §4.4 已同步。
5. **gRPC 面的鉴权顺序**：gRPC 适配层没有路由级预检（proto 结构错误的解析先于用例授权）。
   由于 `MetadataFromProto` 是**纯结构**转换、不访问存储，它不泄露"数据是否存在"；
   与 REST 的差异已在此登记（低风险，切片 2 评估是否统一）。
6. **单密钥**：尚不支持 `kid` 多密钥轮换（ADR-012 §8）。
