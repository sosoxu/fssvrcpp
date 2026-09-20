# 运维手册（operations）

> 适用范围：`fssvrcpp`（C++20 / OSDU File Service 兼容后端）的**单实例**部署形态。
> 本文的**配置项参考表**与 `config/fss.example.json` 的**每一个叶子键**一一对应，并由
> [`tests/unit/test_operations_doc.cpp`](../tests/unit/test_operations_doc.cpp) 自动比对
> （`ctest -L phase9` 会跑）：缺一个键、或写了一个不存在的键，测试都会失败并一次性列出全部差异。
>
> ★ **先读 §1.1**：阶段 10 切片 1 起，组合根
> （[`src/main/server_main.cpp`](../src/main/server_main.cpp)）**加载配置面**：
> 命令行 `--set` > 环境变量（通用名 / 旧别名）> `--config`/`FSS_CONFIG` 指向的
> 带注释 JSON > 组合根历史默认值。阶段 10 切片 2（C10.9~C10.12）进一步把
> **GC 调度、`expiry.*`、以及 16 个"未实现键"的显式拒绝**接进组合根。
> 因此本手册对每个键都标注**三态**（`生效` / `拒绝启动（触发条件）` / `已读但无效果`，
> 见 §1.2 与 §1.3 的逐键清单）。这是一条**如实登记**。
> 配置非法（未知键 / 类型不符 / 越界 / 跨字段冲突 / 生产强校验不过）→ 打印全部问题并
> **以退出码 78（EX_CONFIG）拒绝启动**。
> **未预期异常**（资源耗尽，如容器 `--pids-limit` 过小导致线程创建 `EAGAIN`；或
> `std::bad_alloc`）→ `main()` **顶层兜底**打印可读原因并
> **以退出码 70（EX_SOFTWARE）结束**（C9.32；**不再是** `terminate` / 139）。

---

## 0. 一分钟速查

```bash
# 启动：推荐用配置文件（带注释的 JSON）；环境变量仍可作为覆盖层
# ⚠️ config/fss.example.json 里 7 个密文键写成了 ${ENV:VAR}：**这些变量必须先注入**，
#    否则加载器会把"引用未解析"计为配置问题并拒绝启动（exit 78）—— 这是 fail-closed 的有意行为。
./build/bin/fss_server --config config/fss.example.json

# 只看"这次到底用了什么配置"（脱敏 + 每项来源），不启动服务
./build/bin/fss_server --config config/fss.example.json --print-config

# 命令行覆盖单个键（可重复；优先级最高）
./build/bin/fss_server --config config/fss.example.json --set server.http.port=9090

# 只跑一轮 GC 后退出（便于 cron；退出码 0；摘要打印 GcReport 字段）
./build/bin/fss_server --config config/fss.example.json --once --set gc.dry_run=false

# 等价的环境变量启动（通用名 / 旧别名都支持）
FSS_STORAGE_DRIVER=posix \
FSS_STORAGE_ROOT=/var/lib/fss/data \
FSS_SQLITE_PATH=/var/lib/fss/location.db \
FSS_METADATA_SQLITE_PATH=/var/lib/fss/metadata.db \
FSS_HTTP_PORT=8080 \
FSS_AUTH_MODE=jwt \
FSS_JWT_HMAC_SECRET="$(cat /run/secrets/jwt_hmac_secret)" \
FSS_TRANSFER_SECRET="$(cat /run/secrets/transfer_secret)" \
./build/bin/fss_server

# 探活 / 就绪 / 版本 / 指标
curl -sS http://127.0.0.1:8080/api/file/v2/liveness_check     # File service is alive
curl -sS -w '\n%{http_code}\n' http://127.0.0.1:8080/api/file/v2/readiness_check
curl -sS http://127.0.0.1:8080/api/file/v2/info
curl -sS http://127.0.0.1:8080/metrics | head -40
```

| 项 | 值 |
| --- | --- |
| REST base path | `/api/file`（契约 §1.1；`server.http.base_path`，默认 `/api/file`） |
| 默认 HTTP 端口 | `8080`（`server.http.port` / `FSS_HTTP_PORT`） |
| 默认 gRPC 端口 | **关闭**（`server.grpc.port=0`；`-1` = 系统分配） |
| 指标端点 | `/metrics`（`observability.metrics_path`；**不在** base path 之下，契约 §7） |
| 数据面 | `/api/file/v1/transfer/{token}`（仅 POSIX 驱动 + `self_signed.enabled=true`） |
| 默认存储驱动 | `posix`（`storage.driver` / `FSS_STORAGE_DRIVER`） |
| 默认鉴权 | 组合根默认 `disabled`（`auth.mode`，**与 schema 默认 `jwt` 不同** —— 见 §1.4）；生产必须 `jwt` |
| 配置非法 | 打印全部问题 + **退出码 78（EX_CONFIG）** |
| 未知命令行参数 | 打印用法 + **退出码 2** |
| **未预期异常**（线程创建 `EAGAIN` / `bad_alloc` 等） | 顶层 catch → 打印可读原因（`未预期异常（exit 70）：…`）+ **退出码 70（EX_SOFTWARE）**；**不再**是 `terminate` / 139 |

---

## 1. 配置项参考

### 1.1 组合根当前如何读配置（★ 必读）

**来源与优先级（高 → 低）**

| 优先级 | 来源 | 写法 | 备注 |
| --- | --- | --- | --- |
| 1 | 命令行覆盖 | `--set <json.path>=<值>`（可重复） | 合法路径/类型仍由 schema 校验；未知路径 → 拒绝启动 |
| 2 | 环境变量（通用名） | `FSS_` + 路径大写、`.`→`_`（如 `server.http.port` → `FSS_SERVER_HTTP_PORT`） | 由 `fss::config::Load` 直接映射 |
| 3 | 环境变量（旧别名，deprecated） | `FSS_HTTP_PORT` / `FSS_STORAGE_ROOT` / `FSS_POSIX_DURABILITY` / … | 现有脚本/镜像在用；见 §1.4 别名表。**通用名与别名同时存在时通用名胜出**，并在启动横幅/日志里给出告警 |
| 4 | 配置文件 | `--config <path>`（等价 `FSS_CONFIG`）；**带注释的 JSON** | 未知键/类型不符/越界/跨字段冲突 → 拒绝启动 |
| 5 | 组合根历史默认值 | —— | 与 schema 默认值不同的键逐个登记在 §1.4；保证"不提供配置时行为与接线前一致" |

* **命令行**：`--config <path>`、`--set <path>=<value>`、`--once`、`--print-config`、`--help`。
  未知参数 → 打印用法并拒绝启动（退出码 2）。`--print-config` 打印**脱敏后**的完整有效配置
  与每项来源（`cli` / `env` / `file` / `default`，旧别名额外标注 `别名 FSS_XXX`），随后退出 0。
* **失败语义**：配置非法 → 一次性打印**全部**问题（`path + 原因 + 来源`）→ **退出码 78（EX_CONFIG）**。
  这条对运维很重要：只报第一个问题会导致"改一个、重启、再发现一个"的迭代。
* **未预期异常**（`main()` 顶层兜底，C9.32）→ `未预期异常（exit 70）：<what()>` + 一行静态提示
  → **退出码 70（EX_SOFTWARE）**。典型触发是容器 `--pids-limit` 过小（见 §8）或内存耗尽。
  **绝不**把这类异常归到 78（那是配置问题，会误导排查方向）或 2（那是命令行用法错误）。
  可直接驱动这条路径的故障注入接缝：环境变量 `FSS_STARTUP_FAULT_INJECT`
  （取值与禁令见 [`runbook.md`](runbook.md) 的测试/演练小节；**不要在生产设置**）。
* **`${ENV:VAR}` 引用**：示例文件里的密文键写成 `${ENV:VAR_NAME}` —— 现在由
  `fss::config` 加载器解析（组合根走的就是加载器），因此这些引用**已生效**。
* 示例文件里的内嵌 `${ENV:...}` 名与组合根支持的别名**不完全一致**（3 处），见 §1.4 的别名表。
* **启动横幅**：`stdout` 的**第一行**给出配置来源（文件路径，或"无配置文件（仅环境变量）"），
  随后逐键打印**生效**键（以及有来源的守卫键）的来源缩写。

### 1.2 逐键参考表（157 个叶子键，最后一列 = 接通状态三态）

> 读法：**示例值**取自 `config/fss.example.json`（它是"给人抄的样例"，不等于 schema 默认值）；
> **取值约束**取自 `fss::config::CoreSchema()`（[`src/common/config/core_schema.cpp`](../src/common/config/core_schema.cpp)）；
> 两者不一致处已显式标注。
>
> ★ **最后一列是「接通状态」三态**（阶段 10 切片 2 起，C10.11），每行**以状态标记开头**：
>
> | 状态 | 含义 |
> | --- | --- |
> | **生效** | 组合根读取该键，且（在默认/合法值下）它**真的改变运行行为**；括号里给出可用的来源（配置路径 + 通用环境变量名 + 旧别名），以及"非法值拒绝启动"的触发条件 |
> | **拒绝启动（触发条件）** | 该键在组合根里**唯一**的作用就是"非默认/不支持的值 → exit 78"；默认/合法值不产生额外行为。触发条件与"下一步"逐条写在该单元格里 |
> | **已读但无效果** | 组合根**不接线**该键（或读了但行为未实现）→ 改它对真实进程没有影响。必须给出**理由与下一步** |
>
> 三态计数见 §1.3（**生效 128 / 拒绝启动 15 / 已读但无效果 14；合计 157**，与 `config/fss.example.json` 的叶子键一一对应）。


#### 1.2.1 `deployment`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 接通状态（三态：生效 / 拒绝启动 / 已读但无效果） |
| --- | --- | --- | --- | --- |
| `deployment.environment` | 部署环境；`production` 触发更严格校验（禁 `disabled`、必须验签、jwt 必须有密钥） | `development` | enum `development` / `production`；默认 `development` | **生效**：已接通（`--config`/`--set`/`FSS_DEPLOYMENT_ENVIRONMENT`）。`production` 时：`auth.mode` 必须 `jwt`、`auth.jwt.verify_signature` 必须 true、`auth.jwt.hmac_secret` 必须非空，否则 **exit 78** |
| `deployment.max_clock_skew_seconds` | 本地钟与参考钟允许偏差（秒） | `5` | int 0..300；`multi` 时要求 `0 < 值 <= 60` | **生效**：已接通（`deployment.max_clock_skew_seconds`；旧别名 `FSS_MAX_CLOCK_SKEW_SECONDS`）。**两处**用途：① `Multi` 跨字段要求 `0 < 值 <= 60`；② 赋值给 `LocalJwtOptions.clock_skew_seconds`（JWT `exp`/`nbf` 的容忍，仅 `auth.mode=jwt` 时有意义）。★ **不是**启动期 PG↔本地钟比较的容忍值 —— 那个是 `deployment.clock_skew_tolerance_seconds`（见下行） |
| `deployment.mode` | 单实例 / 多实例 | `single` | enum `single` / `multi`；`multi` 触发 7 条强制校验 | **生效**（B1）：已接通（`deployment.mode` / 通用名 `FSS_DEPLOYMENT_MODE`）。`single` = 内置 SQLite / 内存租约；`multi` = **真的启动**：强制要求 `metadata.repository=postgres` + `location.repository=postgres` + `leases.enabled=true` + `leader_election.enabled=true` + `storage.posix.shared_mount_required=true` + `gc.require_lease_expiry=true` + `0 < max_clock_skew_seconds <= 60`（`core_schema.cpp` 跨字段）；组合根随后创建 PG 仓储 / PG 租约 / leader election，任一失败 → **exit 78**。⚠️ 本构建未找到 libpq 时 multi **仍 exit 78**（可执行修复指令）——见 §8 |
| `deployment.expected_instances` | 部署在该 PostgreSQL 上的实例数（含本实例） | `1` | int 1..100000；默认 `1` | **生效**（B2a / C9.28）：已接通（`deployment.expected_instances` / 通用名 `FSS_DEPLOYMENT_EXPECTED_INSTANCES`）。启动期读 PG `max_connections`，要求 `expected_instances × 每实例最坏连接数 ≤ max_connections`，否则 **exit 78**（消息给出两组数字与三种修复方向）。每实例最坏连接数 = `metadata.postgres.max_connections` + `location.postgres.max_connections` +（`leases.enabled=true` 时**再加**一份 `location.postgres.max_connections`，租约是独立池）+ 1（专用 leader 锁连接）。★ 只在真的用 PG 时校验 |
| `deployment.instance_id` | 实例标识（K8s 建议注入 `POD_NAME`） | `""` | string | **生效**（B1）：已接通（`deployment.instance_id`）→ `PosixBlobStoreOptions.instance_id`（临时文件名 `<key>.tmp.<instance_id>.<pid>.<counter>.<random>`，随机后缀见 ADR-009 §4.5）。★ **multi 语义**：未显式配置（有效默认 `local`）或显式为空 → 组合根**自动生成**唯一 UUID 并在横幅打印（默认 `local` 会让多实例共享同一标识 → 共享挂载上临时名确定性撞名，ADR-009 M1 静默串数据）；显式配置 → 原样使用。**single 逐字不变**（空仍是空、`local` 仍是 `local`） |
| `deployment.clock_skew_tolerance_seconds` | 与数据库 `now()` 的偏移容忍（秒） | `60` | int 0..86400 | **生效**（B2a，ADR-009 §6.4/§8.1）：已接通（`deployment.clock_skew_tolerance_seconds` / 通用名 `FSS_DEPLOYMENT_CLOCK_SKEW_TOLERANCE_SECONDS`）。启动期（真的用 PG 时）读 `SELECT (extract(epoch from now())*1000)::bigint` 与本实例本地钟比对，偏差绝对值 > 容忍 → **exit 78**（消息给出本地值、数据库值、偏差毫秒与容忍值）。横幅打印 `clock skew :` 的实际结论。★ 测试接缝 `FSS_CLOCK_SKEW_INJECT_MS`（**环境变量，不是配置键**）可注入可控偏差，见 [runbook §10.3](runbook.md) |

#### 1.2.2 server.http 子树

| JSON 路径 | 含义 | 示例值 | 取值约束 | 接通状态（三态：生效 / 拒绝启动 / 已读但无效果） |
| --- | --- | --- | --- | --- |
| `server.http.base_path` | REST context path，必须与上游一致 | `/api/file` | string，Required；默认 `/api/file` | **生效**：已接通（`server.http.base_path`）→ `RouterOptions.base_path`；也是自签 URL 基址的默认组成部分 |
| `server.http.bind` | 监听地址 | `0.0.0.0` | string；默认 `0.0.0.0` | **生效**：已接通（`server.http.bind`；旧别名 `FSS_BIND_ADDRESS`，**HTTP 与 gRPC 共用**） |
| `server.http.port` | HTTP 端口 | `8080` | int 1..65535；默认 `8080` | **生效**：已接通（`server.http.port`；旧别名 `FSS_HTTP_PORT`，允许 `0` = 系统分配） |
| `server.http.max_header_bytes` | 单请求头上限 | `16384` | int 1024..1048576；默认 `16384` | **生效**：已接通（`server.http.max_header_bytes`）→ `http::ServerOptions.max_header_bytes` |
| `server.http.max_uri_bytes` | 请求行/target 上限 | `8192` | int 128..1048576；默认 `8192` | **生效**：已接通（`server.http.max_uri_bytes`）→ `http::ServerOptions.max_uri_bytes` |
| `server.http.max_body_bytes` | JSON 端点请求体上限 | `10485760` | int 1..1073741824；默认 `10485760`（10 MiB） | **生效**：已接通（`server.http.max_body_bytes`）→ `RouterOptions.json_body_limit_bytes` |
| `server.http.tcp_nodelay` | 小请求必须开启，否则 ~40 ms delayed-ACK 停顿（实测 950 倍） | `true` | bool；默认 `true` | **生效**：已接通（`server.http.tcp_nodelay`）→ `http::ServerOptions.tcp_nodelay` |
| `server.http.worker_threads` | 并发连接上限（一条 keep-alive 连接占一个线程直到结束） | `0` | int 0..65536；0 = 按公式推导 | **生效**：已接通（`server.http.worker_threads`）；`0` → 实现公式 `max(16, 4×核数)`（本机 16 核 → 64），可在 `/metrics` 的 `fss_http_worker_threads` 上看到实际值 |
| `server.http.max_connections` | 在途请求上限（超过 → 503 + `Retry-After`） | `0` | int 0..65536；0 = 等于 worker_threads | **生效**：已接通（`server.http.max_connections`）；`0` → = worker_threads；**大于 worker_threads → 拒绝启动**（否则背压静默失效） |
| `server.http.max_connections_per_partition` | 每租户并发上限 | `0` | int 0..65536 | **拒绝启动（触发条件）**：非 `0` → exit 78（每租户并发上限未实现）。下一步：保持 `0`，用 `server.http.max_connections` 表达全局上限 |
| `server.http.idle_timeout_seconds` | 普通路由空闲读超时 | `60` | int 1..86400；默认 `60` | **生效**：已接通（`server.http.idle_timeout_seconds`）→ `http::ServerOptions.read_timeout_sec` |
| `server.http.json_request_timeout_seconds` | 普通路由**整体**超时 | `15` | int 1..86400；默认 `15` | **生效**：已接通（`server.http.json_request_timeout_seconds`）→ `http::ServerOptions.json_request_timeout_sec` |
| `server.http.transfer_idle_timeout_seconds` | 数据面**空闲**超时（**没有**整体超时） | `120` | int 1..86400；默认 `120` | **生效**：已接通（`server.http.transfer_idle_timeout_seconds`）→ `http::ServerOptions.transfer_idle_timeout_sec`（同时赋给 `S3Options.transfer_idle_timeout_seconds`） |
| `server.http.transfer_max_body_bytes` | 数据面请求体上限 | `0` | int 0..1099511627776；`0` = 不限，`>0` = 上限（字节） | **生效**：已接通（`server.http.transfer_max_body_bytes`）→ 数据面 PUT 的 `RouteOptions::max_body_bytes`。**落点 = 全局键与 `partition.file.<p>.max_file_bytes` 的较小者**（0 = 该侧不限；两个都为 0 → 逐字保持接线前的"不限"）。超限由 HTTP 包装层在读体前拒绝（`Content-Length` → 413）；启动横幅打印实际生效值。证据：`tests/integration/test_config_wiring.cpp` 的切片 4 用例（2 MiB → 413、恰好 1 MiB → 200、默认不限 → 200、partition 更小 → 取较小者） |
| `server.http.transfer_buffer_bytes` | 数据面单块缓冲 | `262144` | int 4096..67108864；默认 `262144` | **生效**：已接通（`server.http.transfer_buffer_bytes`）→ `http::ServerOptions.transfer_buffer_bytes` |
| `server.http.transfer_memory_budget_bytes` | 传输内存预算；`并发 × 缓冲 > 该值` → 拒绝启动 | `268435456` | int ≥1048576；跨字段：`worker_threads > 0` 时 `buffer × workers <= budget` | **生效**：已接通（`server.http.transfer_memory_budget_bytes`）→ `http::ServerOptions.transfer_memory_budget_bytes`；schema 跨字段 + `http::ValidateOptions` 双重校验，违反 → **exit 78** |
| `server.http.large_file_plane.enabled` | ADR-006 独立大文件数据面（`sendfile`）开关 | `false` | bool；默认 `false` | **拒绝启动（触发条件）**：`=true` → exit 78（sendfile 数据面未交付，ADR-006 只定稿方向）。下一步：保持 `false`（默认，走 httplib 内容提供者） |
| `server.http.large_file_plane.bind` | 大文件数据面监听地址 | `0.0.0.0` | string；默认 `0.0.0.0` | **已读但无效果**：组合根未接线（登记为未实现） |
| `server.http.large_file_plane.port` | 大文件数据面端口 | `8081` | int 1..65535；默认 `8081` | **已读但无效果**：组合根未接线（登记为未实现） |
| `server.http.large_file_plane.use_sendfile` | 是否用 `sendfile` | `true` | bool；默认 `true` | **已读但无效果**：组合根未接线（登记为未实现） |
| `server.http.large_file_plane.sendfile_chunk_bytes` | `sendfile` 单次长度 | `1073741824` | int ≥65536；默认 `1073741824` | **已读但无效果**：组合根未接线（登记为未实现） |
| `server.http.large_file_plane.workers` | 大文件数据面工作线程数 | `0` | int 0..65536；默认 `0` | **已读但无效果**：组合根未接线（登记为未实现） |
| `server.http.large_file_plane.max_connections` | 大文件数据面连接上限 | `0` | int 0..65536；默认 `0` | **已读但无效果**：组合根未接线（登记为未实现） |

#### 1.2.3 server.grpc 子树

| JSON 路径 | 含义 | 示例值 | 取值约束 | 接通状态（三态：生效 / 拒绝启动 / 已读但无效果） |
| --- | --- | --- | --- | --- |
| `server.grpc.enabled` | gRPC 面开关（平台外扩展，ADR-001） | `true` | bool；默认 `true` | **生效**：已接通（`server.grpc.enabled`）：`false` → **不开** gRPC 面（即使 `server.grpc.port` 非 0）；`true` 且 `server.grpc.port≠0` → 开且 `GetInfo` 可用。`FSS_GRPC_PORT=0` 的既有语义（`port=0` 即关闭）保持不变 |
| `server.grpc.bind` | gRPC 监听地址 | `0.0.0.0` | string；默认 `0.0.0.0` | **生效**：已接通（`server.grpc.bind`；旧别名 `FSS_BIND_ADDRESS`，与 HTTP 共用同一个变量） |
| `server.grpc.port` | gRPC 端口 | `50051` | int 1..65535；默认 `50051` | **生效**：已接通（`server.grpc.port`；旧别名 `FSS_GRPC_PORT`，允许 `0`=关闭 / `-1`=系统分配 —— 旧别名的值域比 schema 更宽，见 §1.4） |
| `server.grpc.max_message_bytes` | 单条消息上限 | `4194304` | int ≥1024；默认 `4194304` | **拒绝启动（触发条件）**：非默认（`4194304`）→ exit 78（gRPC 侧消息上限未接通）。下一步：保持默认 |
| `server.grpc.streaming_chunk_bytes` | 流式分块 | `262144` | int ≥4096；默认 `262144` | **拒绝启动（触发条件）**：非默认（`262144`）→ exit 78（gRPC 流式分块未接通）。下一步：保持默认 |

#### 1.2.4 `storage`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 接通状态（三态：生效 / 拒绝启动 / 已读但无效果） |
| --- | --- | --- | --- | --- |
| `storage.driver` | 存储驱动 | `posix` | enum `posix` / `s3`；默认 `posix` | **生效**：已接通（`storage.driver` / 通用名 `FSS_STORAGE_DRIVER`；非法值 → exit 78） |
| `storage.proxy_mode` | `auto` = 按驱动能力决定地址形态；`always` = 强制服务代理字节 | `auto` | enum `auto` / `always`；默认 `auto` | **拒绝启动（触发条件）**：`always` → exit 78（「强制服务代理所有字节」未实现）。下一步：保持 `auto` |
| `storage.driver_report_override` | 覆盖上报的 Driver（上游硬编码 `GCS`） | `""` | string；默认 `""` | **生效**：已接通（`storage.driver_report_override`）→ `S3Options.driver_report_override` |
| `storage.provider_key_override` | 覆盖 DMS 的 `providerKey` | `""` | string；默认 `""` | **生效**：已接通（`storage.provider_key_override`）→ `S3Options.provider_key_override` |
| `storage.io_engine` | I/O 引擎（ADR-010） | `blocking` | enum `blocking` / `uring` / `auto`；默认 `blocking` | **生效**：已接通（`storage.io_engine`）：探测 + 决策在组合根执行；`uring` 不可用 → **exit 78**（提示 `scripts/check_io_uring.sh`）；`auto` → 回退 blocking，且**横幅 + `/metrics` 的 `fss_io_engine{engine,requested}` + 新增 `fss_io_uring_available`（宿主能力）+ `/v2/info` 的 `ioEngine`/`ioUringAvailable` 四处可见**（C9.30）。当前 `UringIoEngine::enabled()=false`（ADR-010 U1~U4），故实际生效恒为 blocking（**可用 ≠ 已启用**） |
| `storage.io_uring.queue_depth` | io_uring 队列深度 | `64` | int 1..4096；默认 `64` | **生效**：已接通（`storage.io_uring.queue_depth`）→ `sys::ProbeIoUring(entries)` |
| `storage.io_uring.register_files` | io_uring 注册文件表 | `false` | bool；默认 `false` | **拒绝启动（触发条件）**：`true` → exit 78（io_uring 引擎未启用，ADR-010）。下一步：保持 `false` |
| `storage.posix.root` | 集中存储根目录 | `/var/lib/fss/data` | string，Required；默认 `/var/lib/fss/data` | **生效**：已接通（`storage.posix.root`；旧别名 `FSS_STORAGE_ROOT`；**组合根历史默认 `/tmp/fss-data`**）；对象落在 `<root>/blobs` |
| `storage.posix.durability` | 落盘档位 | `batch` | enum `batch` / `per_file`（★ **schema 未含 `never`**）；默认 `batch` | **生效**：已接通（`storage.posix.durability`；旧别名 `FSS_POSIX_DURABILITY`，**接受 `never`**）。映射见 §5.1；**组合根历史默认 `per_file`**。★ **本轮起 `batch` 是真批提交**（ADR-008 的 P4：`write all tmp → syncfs → rename all → fsync(dir)`）：**并发**写才有摊销，**顺序单文件写**仍是"一文件一次提交"（拿不到 ADR 的吞吐摊销）。此前把它近似成 `kBySize`（语义 = "小文件不 fsync，靠不存在的批提交摊销"）⇒ 默认配置下小文件**从不落盘**，是耐久性谎言（记录见 `docs/00-final-design.md` §5） |
| `storage.posix.group_commit_max_batch` | 批提交最大批大小 | `500` | int 1..100000；默认 `500` | **生效**（本轮 / ADR-008 的 P4）：接通 `storage.posix.group_commit_max_batch` → `PosixBlobStoreOptions::group_commit_max_batch`。**批内对象数上限**：并发驱动的组提交里，批满 `C` 个对象**立即**提交（`C=1` = 每文件一次提交）。真实进程证据：`/metrics` 的 `fss_posix_group_commits_total` 与 `fss_posix_batch_objects_total` 之比 = 平均批大小；进程内确定性判据见 `tests/integration/test_posix_batch_commit.cpp`（N=8/C=4 → `syncfs == 2`） |
| `storage.posix.sync_dir_after_batch` | 批末 `fsync(dir)`（ADR-008 的 R2 不变量） | `true` | bool；默认 `true` | **拒绝启动（触发条件）**：`false` → exit 78（**ADR-008 §5 的 R2 是**不变量**：rename 之后必须 `fsync(目录)`，否则「已确认」的对象在崩溃后可能消失**；schema 描述也写明必须 `true`）。下一步：保持 `true`（不为"多一个生效键"放宽这条护栏） |
| `storage.posix.fsync_threshold_bytes` | ≥ 该值的对象强制 per-file 落盘（只对 `batch` 生效） | `1048576` | int 0..1099511627776；默认 `0`（0 = 所有对象都必须 fsync） | **生效**：已接通（`storage.posix.fsync_threshold_bytes`；旧别名 `FSS_POSIX_FSYNC_THRESHOLD_BYTES`）。**只在 durability=batch 时生效**；组合根历史默认 1 MiB（1048576）。⚠️ 阈值 `0` 会让 batch 退化成 per_file 的耐久性与速度（见 §5.1） |
| `storage.posix.atomic_write` | tmp + rename 原子写 | `true` | bool；默认 `true` | **生效**：接通 `storage.posix.atomic_write` → `PosixBlobStoreOptions::atomic_write`（默认 `true` = 既有 tmp + rename）。`false` → **直接写目标文件**；失败路径**删除目标**，目标与 `.tmp.*` 都不残留（驱动级故障注入断言，正反两模式对照） |
| `storage.posix.dir_mode` | 目录权限 | `0750` | string；默认 `0750` | **生效**：接通 `storage.posix.dir_mode` → `PosixBlobStoreOptions::dir_mode`（建容器目录与对象父目录时显式 `chmod`）。非法八进制 → exit 78。证据：真实进程 `stat` 目录 = `0700`（配置 `"0700"`） |
| `storage.posix.file_mode` | 文件权限 | `0640` | string；默认 `0640` | **生效**：接通 `storage.posix.file_mode` → `PosixBlobStoreOptions::file_mode`（`open(2)` 的 mode）。⚠️ 接线前实现固定 `0644`，现按配置落盘（默认 `0640`）。证据：真实进程 `stat` 对象 = `0600`（配置 `"0600"`） |
| `storage.posix.fadvise_random` | 随机读提示 | `true` | bool；默认 `true` | **生效**：接通 `storage.posix.fadvise_random` → 写路径（`put`）与读路径（`get`）的打开 fd 各下发一次 `POSIX_FADV_RANDOM`；可注入 `IFadviseSink` 计数接缝断言（关闭对照 = 0 次） |
| `storage.posix.fadvise_dontneed_after_large_read` | 大段读后 `DONTNEED` | `false` | bool；默认 `false` | **生效**：接通 `storage.posix.fadvise_dontneed_after_large_read` → 读出的字节数 **> 1 MiB** 时下发 `POSIX_FADV_DONTNEED`；1 KiB 的读不触发（阈值边界在用例里断言） |
| `storage.posix.shared_mount_required` | `multi` 下必须 true（共享挂载探针） | `false` | bool；默认 `false`；`multi` 必须 true | **已读但无效果**：**共享挂载探针未实现** —— 该键当前只参与 schema 的 multi 强制校验（`multi` 且不 true → exit 78，这在接线前就已如此）；没有任何"写探针文件看别的实例"的探测。下一步：实现探针后才算接通（§1.3.3） |
| `storage.posix.one_filesystem_per_partition` | 每 partition 独占文件系统（`syncfs` 隔离） | `false` | bool；默认 `false` | **已读但无效果**：组合根未接线（登记为未实现） |
| `storage.s3.endpoint` | S3 端点 | `http://127.0.0.1:9000` | string；默认 `http://127.0.0.1:9000` | **生效**：已接通（`storage.s3.endpoint` / 通用名 `FSS_STORAGE_S3_ENDPOINT`）；**组合根历史默认为空**，空值 + `driver=s3` → exit 78 |
| `storage.s3.region` | 区域 | `us-east-1` | string；默认 `us-east-1` | **生效**：已接通（`storage.s3.region` / `FSS_STORAGE_S3_REGION`）→ `S3Options.region` |
| `storage.s3.access_key` | Access Key（密文） | `${ENV:FSS_S3_ACCESS_KEY}` | string，secret；默认 `""` | **生效**：已接通（`storage.s3.access_key`；通用名 `FSS_STORAGE_S3_ACCESS_KEY`，旧别名 `FSS_S3_ACCESS_KEY`）→ `S3Options.credentials` |
| `storage.s3.secret_key` | Secret Key（密文） | `${ENV:FSS_S3_SECRET_KEY}` | string，secret；默认 `""` | **生效**：已接通（`storage.s3.secret_key`；通用名 `FSS_STORAGE_S3_SECRET_KEY`，旧别名 `FSS_S3_SECRET_KEY`）→ `S3Options.credentials` |
| `storage.s3.force_path_style` | MinIO/Ceph/SeaweedFS 需要 true | `true` | bool；默认 `true` | **生效**：已接通（`storage.s3.force_path_style` / `FSS_STORAGE_S3_FORCE_PATH_STYLE`）→ `S3Options.force_path_style` |
| `storage.s3.verify_tls` | 校验 TLS 证书 | `true` | bool；默认 `true` | **生效**：已接通（`storage.s3.verify_tls` / `FSS_STORAGE_S3_VERIFY_TLS`）→ `S3Options.verify_tls` |
| `storage.s3.connect_timeout_ms` | 连接超时 | `3000` | int 1..600000；默认 `3000` | **生效**：已接通（`storage.s3.connect_timeout_ms`）→ `S3Options.connect_timeout_ms` |
| `storage.s3.total_timeout_ms` | 总超时 | `30000` | int 1..3600000；默认 `30000` | **生效**：已接通（`storage.s3.total_timeout_ms`）→ `S3Options.total_timeout_ms` |
| `storage.s3.presign_default_seconds` | 预签名默认有效期 | `3600` | int 1..604800；默认 `3600` | **生效**：已接通（`storage.s3.presign_default_seconds`）→ `S3Options.presign_default_seconds` |
| `storage.s3.presign_max_seconds` | 预签名最长有效期（对齐 OSDU 上限 7 天） | `604800` | int 1..604800；默认 `604800` | **生效**：已接通（`storage.s3.presign_max_seconds`）→ `S3Options.presign_max_seconds` |

#### 1.2.5 `self_signed`（集中存储数据面的自签 URL）

| JSON 路径 | 含义 | 示例值 | 取值约束 | 接通状态（三态：生效 / 拒绝启动 / 已读但无效果） |
| --- | --- | --- | --- | --- |
| `self_signed.enabled` | 自签数据面开关 | `true` | bool；默认 `true` | **生效**：已接通（`self_signed.enabled`）：`false` 时**不注册** `/v1/transfer` 数据面路由 |
| `self_signed.public_base_url` | 对外可达基址（反向代理后填外部地址） | `http://127.0.0.1:8080` | string；默认 `http://127.0.0.1:8080` | **生效**：已接通（`self_signed.public_base_url`；旧别名 `FSS_SELF_BASE_URL`）；组合根历史默认 `http://127.0.0.1:<port>/api/file`（**必须含 base path**） |
| `self_signed.signing_key` | HMAC 签名密钥；所有实例必须共享（密文） | `${ENV:FSS_TRANSFER_SIGNING_KEY}` | string，secret；默认 `""` | **生效**：已接通（`self_signed.signing_key`；旧别名 `FSS_TRANSFER_SECRET`、`FSS_TRANSFER_SIGNING_KEY`）→ `HmacTransferTokenCodec`；组合根历史默认 `dev-secret-change-me`（**生产必须改**） |
| `self_signed.key_id` | 密钥标识；非空时进**被签名的 token 载荷**，解码侧要求一致（fail-closed） | `k1` | string；默认 `k1` | **生效**（切片 5）：接通 `self_signed.key_id` → `HmacTransferTokenCodec` 的第 3 个参数。非空时 `Encode` 把 `key_id` 写进**被签名的载荷**，`Decode` 要求载荷里的 `key_id` **完全相等**（**缺失也拒绝** → `kUnauthenticated` / HTTP **401**）。真实进程证据：`key_id=k1` 签发的 URL PUT/GET 200；以 `k2` 重启（同签名密钥）后重放同一 URL → **401**。启动横幅打印 `key_id`（**不打印密钥**）。⚠️ **多密钥轮换未交付**（ADR-009:227 的"多 key 并存"仍未做）：这里只是"把 id 绑进签名载荷并在解码侧拒绝不匹配"，**没有**"按 id 选密钥/多 key 并存"的能力；换 `key_id` = 旧 URL 全量作废 |
| `self_signed.default_ttl_seconds` | **自签分支**的缺省 TTL **上界**（请求未提供 `expiryTime` 时生效） | `3600` | int 1..604800；默认 `3600` | **生效**（切片 5）：接通 → `app::LocationIssuer` 的 `SelfSignedTtlOptions.default_seconds`。⚠️ **语义是上界**（`ttl = min(ttl, 值)`），且**仅 `!native_presign`（自签）分支**；`native_presign` 分支**完全不受**这两个键影响。与 `expiry.*` 的关系：`expiry.default` 仍是 **`expiryTime` 参数的缺省**（两条分支共用），本键只在其**之后**对自签分支再夹一次。真实进程证据：`default_ttl_seconds=60` 且不带 `expiryTime` → `exp=now+60`；给了 `expiryTime=1M`（60s）→ 仍是 60s（缺省上界**只在未给 `expiryTime` 时叠加**）。启动横幅打印实际取值与「TTL 自签上界」字样 |
| `self_signed.max_ttl_seconds` | **自签分支**的**绝对** TTL 上界（任何请求都夹紧） | `604800` | int 1..604800；默认 `604800` | **生效**（切片 5）：接通 → `SelfSignedTtlOptions.max_seconds`（**仅自签分支**）。与 `expiry.max` 的关系：`expiry.max` 先夹 `expiryTime` 的**解析结果**，本键再对自签分支夹一次（两者取更小者）；`default > max` 时绝对上界胜出（缺省上界被压掉，不报错）。真实进程证据：`max_ttl_seconds=120` + `expiryTime=9H`（32400s，远小于 `expiry.max=7D`）→ `exp=now+120`；`expiryTime=1M`（60s）→ 仍是 60s（上界不是"常量改写"）。⚠️ 若要改成「`self_signed.*` 覆盖 `expiry.default` 作缺省」，**必须先推翻 C10.12 的既定语义**并同步契约与测试（见 §1.3.3 的下一步表） |
| `self_signed.single_use_nonce` | 一次性 nonce（多实例下必须 false） | `false` | bool；默认 `false` | **拒绝启动（触发条件）**：`true` → exit 78（nonce 存储未交付，ADR-009 M5）。下一步：保持 `false` |
| `self_signed.nonce_store` | nonce 存储 | `memory` | enum `memory` / `postgres`；默认 `memory` | **拒绝启动（触发条件）**：非 `memory` → exit 78（只有内存 nonce 存储，且 `single_use_nonce=false` 时不用它）。下一步：保持 `memory` |

#### 1.2.6 `expiry` / `http`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 接通状态（三态：生效 / 拒绝启动 / 已读但无效果） |
| --- | --- | --- | --- | --- |
| `expiry.default` | `expiryTime` 缺省有效期 | `1H` | string；默认 `1H` | **生效**：`expiry.default`（`--config`/`--set`；无旧别名）→ `app::ExpiryPolicy` 的**缺省值**（默认 `1H` = 3600s）。启动横幅打印实际取值；非法形态 → **exit 78** |
| `expiry.max` | `expiryTime` 上限（超限静默截断） | `7D` | string；默认 `7D` | **生效**：`expiry.max`（默认 `7D`）→ `ExpiryPolicy` 的**夹紧上限**；请求值超上限时**静默夹紧**（不是拒绝，契约 §1.4 的真实语义）；`expiry.default > expiry.max` → **exit 78** |
| `http.error_format` | 错误体格式 | `apperror` | enum `apperror` / `legacy` / `api_error`；默认 `apperror` | **生效**：已接通（`http.error_format`）→ `RouterOptions.error_format` 与 `http::ServerOptions.error_format`；非法值 → exit 78 |

#### 1.2.7 `metadata`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 接通状态（三态：生效 / 拒绝启动 / 已读但无效果） |
| --- | --- | --- | --- | --- |
| `metadata.repository` | 元数据仓储实现 | `sqlite` | enum `sqlite` / `postgres` / `remote`；默认 `sqlite` | **生效**（B1）：已接通（`metadata.repository`）：`sqlite` → 内置 SQLite；`postgres` → `PostgresMetadataRepository`；`remote` **未交付** → exit 78（组合根显式拒绝，绝不静默降级）。非法取值由 schema 枚举拒绝 |
| `metadata.sqlite.path` | 元数据库路径 | `/var/lib/fss/meta.db` | string；默认 `/var/lib/fss/meta.db` | **生效**：已接通（`metadata.sqlite.path`；旧别名 `FSS_METADATA_SQLITE_PATH`）→ `SqliteMetadataRepository::Open`；组合根历史默认 `<storage_root>/metadata.db` |
| `metadata.sqlite.busy_timeout_ms` | SQLite busy 超时 | `5000` | int 0..600000；默认 `5000` | **生效**：已接通（`metadata.sqlite.busy_timeout_ms`）→ `SqliteMetadataRepositoryOptions.busy_timeout_millis`（`sqlite3_busy_timeout`）；启动横幅打印实际取值 |
| `metadata.sqlite.journal_mode` | SQLite journal 模式 | `WAL` | enum `WAL` / `DELETE` / `TRUNCATE`；默认 `WAL` | **生效**：已接通（`metadata.sqlite.journal_mode`）→ `SqliteMetadataRepositoryOptions.wal`：`WAL`→`PRAGMA journal_mode=WAL`、`DELETE`→不执行 PRAGMA（SQLite 默认 `DELETE`，可用 `python3 sqlite3` 从库文件读回）；`TRUNCATE` **未接通** → exit 78（不静默当成 DELETE） |
| `metadata.sqlite.synchronous` | SQLite 同步级别 | `NORMAL` | enum `NORMAL` / `FULL` / `OFF`；默认 `NORMAL` | **生效**：已接通（`metadata.sqlite.synchronous`）→ `SqliteMetadataRepositoryOptions.synchronous_level`（`NORMAL`→1 / `FULL`→2 / `OFF`→0）→ `PRAGMA synchronous=<n>`；启动横幅打印实际取值。⚠ 该 PRAGMA **不落盘**，只能用**同连接**访问器 `SqliteMetadataRepository::AppliedPragma("synchronous")` 读回（另开 sqlite3 连接读到的是新连接默认值 FULL=2）；进程内断言见 `tests/integration/test_sqlite_metadata_repository.cpp` |
| `metadata.sqlite.max_write_concurrency` | 有界写并发 | `8` | int 1..256；默认 `8` | **已读但无效果**：写入并发的**上限**；当前实现是「单连接 + 互斥」（实际并发恒为 1 ≤ 上限），改它无从观测（§1.3.3）。下一步：连接池交付后才有意义 |
| `metadata.sqlite.group_commit` | 组提交 | `true` | bool；默认 `true` | **生效**（C10.20）：接通 `SqliteMetadataRepositoryOptions.group_commit` → 共用协调器 `src/infra/sqlite/sqlite_group_commit.h`。`true`（默认）= 并发写操作凑成**一批**、**一个事务一次 `COMMIT`**（批由 `group_commit_max_batch` / `group_commit_max_wait_ms` 决定）；`false` = **逐操作**提交（与接线前逐字一致：一次 `BEGIN IMMEDIATE…COMMIT`）。⚠️ **读路径会多等**：批事务期间领队持有连接互斥 ⇒ 同仓储的读最坏多等 ≤ `group_commit_max_wait_ms`。真实进程证据：`--set metadata.sqlite.group_commit_max_wait_ms=400` 时一次 `createMetadata` 时延 = **416ms**，同窗口 + `group_commit=false` = **14ms**（`tests/integration/test_config_wiring.cpp` 的 C10.20）；进程内确定性判据见 `tests/integration/test_sqlite_group_commit.cpp`（N=6/B=3 → 2 次提交） |
| `metadata.sqlite.group_commit_max_wait_ms` | 组提交最大等待 | `5` | int 0..1000；默认 `5` | **生效**（C10.20）：从**第一个待处理操作**起算的等待窗口；窗口到期即提交（批不满也提交），`0` → 立即提交。★ 这个窗口是**读可能多等的上界**，也是**顺序单文件写**的额外时延（每操作最多多等一个窗口，见 §1.3.1 的取舍说明） |
| `metadata.sqlite.group_commit_max_batch` | 组提交最大批 | `64` | int 1..100000；默认 `64` | **生效**（C10.20）：一票一批的**操作数上限**（批满立即提交，批不会超过上限）；`1` = 每操作一批（无摊销）。可观测：`/metrics` 的 `fss_sqlite_ops_total{repo="metadata"} / fss_sqlite_group_commits_total{repo="metadata"}` = 平均批大小 |
| `metadata.postgres.dsn` | PG 连接串（密文） | `${ENV:FSS_PG_DSN}` | string，secret；默认 `""` | **生效**（B1）：已接通（`metadata.postgres.dsn`）→ `PostgresMetadataRepositoryOptions.pg.dsn`。`metadata.repository=postgres` 时组合根真的建池；为空或不可达 → **exit 78** + libpq 原文（绝不回退 SQLite）。★ 写进启动横幅的是 `metadata.postgres.dsn=***`（不打印密文） |
| `metadata.postgres.max_connections` | PG 连接数（实例数 × 该值 ≤ PG `max_connections`） | `16` | int 1..10000；默认 `16` | **生效**（B1）：已接通 → `PgOptions.max_connections`（有界连接池的上限，见 `src/infra/postgres/pg_connection.h`）；启动横幅打印实际取值 |
| `metadata.postgres.statement_timeout_ms` | 语句超时 | `5000` | int 1..600000；默认 `5000` | **生效**（B1）：已接通 → `PgOptions.statement_timeout_millis`，`PgConnection::Connect` 对新连接下发 `SET statement_timeout`。⚠️ 该键**不**用于 leader election 的锁连接（后者取本键值，见 `leader_election.*` 行）——锁连接只跑短语句，滞留窗口被夹在该值以内（PG 12.6 实测，ADR-009 §4.4） |
| `metadata.postgres.schema_version_check` | readiness 是否校验迁移版本 | `true` | bool；默认 `true` | **生效**（B2a，ADR-009 §5.3）：已接通（`metadata.postgres.schema_version_check` / 通用名 `FSS_METADATA_POSTGRES_SCHEMA_VERSION_CHECK`）。`true`（默认）→ readiness（REST `/v2/readiness_check` 与 gRPC `Check(PROBE_READINESS)` 同源）除 `SELECT 1` 探活外，还比对 `schema_migrations.max(version)` 与二进制期望的 `kExpectedSchemaVersion`（`src/infra/postgres/pg_schema.h`）；「表缺失 / 表为空 / 版本落后 / 版本超前」→ **not ready（REST 503，文本带可读原因）**。`false` → **只**做 `SELECT 1` 探活，跳过版本比对。期望值由 `tests/unit/test_schema_version_constant.cpp` 从 `db/migrations/*.sql` 文件名机械推导（加迁移不改常量 → 测试失败）。★ 只在真的用 PG 时装配探针 |
| `metadata.remote.base_url` | 远端 Storage Service 地址 | `""` | string；默认 `""` | **已读但无效果**：组合根未接线（登记为未实现） |
| `metadata.remote.token_provider` | 远端 token 来源 | `static` | string；默认 `static` | **已读但无效果**：组合根未接线（登记为未实现） |
| `metadata.remote.static_token` | 静态 token（密文） | `${ENV:FSS_STORAGE_TOKEN}` | string，secret；默认 `""` | **已读但无效果**：组合根未接线（登记为未实现） |
| `metadata.remote.timeout_ms` | 远端调用超时 | `5000` | int 1..600000；默认 `5000` | **已读但无效果**：组合根未接线（登记为未实现） |

#### 1.2.8 `location`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 接通状态（三态：生效 / 拒绝启动 / 已读但无效果） |
| --- | --- | --- | --- | --- |
| `location.repository` | 位置仓储实现 | `sqlite` | enum `sqlite` / `postgres`；默认 `sqlite` | **生效**（B1）：已接通（`location.repository`）：`sqlite` → 内置 SQLite；`postgres` → `PostgresLocationRepository`（不再 exit 78）。两值之外的取值由 schema 枚举拒绝；组合根对 `remote`（若枚举被改宽）仍 **exit 78** |
| `location.sqlite.path` | 位置数据库路径 | `/var/lib/fss/location.db` | string；默认 `/var/lib/fss/location.db` | **生效**：已接通（`location.sqlite.path`；旧别名 `FSS_SQLITE_PATH`）→ `SqliteLocationRepository::Open`；组合根历史默认 `<storage_root>/location.db` |
| `location.sqlite.busy_timeout_ms` | SQLite busy 超时 | `10000` | int 0..600000；默认 `10000` | **生效**：已接通（`location.sqlite.busy_timeout_ms`）→ `SqliteLocationRepositoryOptions.busy_timeout_millis`（`sqlite3_busy_timeout`）；启动横幅打印实际取值 |
| `location.sqlite.journal_mode` | journal 模式 | `WAL` | enum `WAL` / `DELETE` / `TRUNCATE`；默认 `WAL` | **生效**：已接通（`location.sqlite.journal_mode`）→ `SqliteLocationRepositoryOptions.wal`：`WAL`→`PRAGMA journal_mode=WAL`、`DELETE`→`DELETE`（可用 `python3 sqlite3` 从库文件读回）；`TRUNCATE` **未接通** → exit 78（不静默当成 DELETE）。`metadata.sqlite.journal_mode` 同样已接通（见 §1.2.7） |
| `location.sqlite.synchronous` | 同步级别 | `NORMAL` | enum `NORMAL` / `FULL` / `OFF`；默认 `NORMAL` | **生效**：已接通（`location.sqlite.synchronous`）→ `SqliteLocationRepositoryOptions.synchronous_level`（`NORMAL`→1 / `FULL`→2 / `OFF`→0）→ `PRAGMA synchronous=<n>`；启动横幅打印实际取值。⚠ 该 PRAGMA **不落盘**，只能用**同连接**访问器 `SqliteLocationRepository::AppliedPragma("synchronous")` 读回；进程内断言见 `tests/integration/test_sqlite_location_repository.cpp` |
| `location.sqlite.max_write_concurrency` | 有界写并发 | `8` | int 1..256；默认 `8` | **已读但无效果**：写入并发的**上限**；当前实现是「单连接 + 互斥」（实际并发恒为 1 ≤ 上限），改它无从观测（§1.3.3）。下一步：连接池交付后才有意义 |
| `location.sqlite.group_commit` | 组提交 | `true` | bool；默认 `true` | **生效**（C10.20）：接通 `SqliteLocationRepositoryOptions.group_commit` → 同一个共用协调器。`true`（默认）= 并发写操作凑成**一批**、**一个事务一次 `COMMIT`**（每操作 `SAVEPOINT` 保原子性）；`false` = **逐操作**提交（与接线前逐字一致：单条自动提交语句 / 一次 `BEGIN IMMEDIATE…COMMIT`）。⚠️ **读路径会多等**：批事务期间领队持有连接互斥 ⇒ 同仓储的读最坏多等 ≤ `group_commit_max_wait_ms`。进程内确定性判据：N=8、B=4 → **2 次提交**（== `ceil(N/B)`）；B=1 → 8 次（`tests/integration/test_sqlite_group_commit.cpp`） |
| `location.sqlite.group_commit_max_wait_ms` | 组提交最大等待 | `5` | int 0..1000；默认 `5` | **生效**（C10.20）：从**第一个待处理操作**起算的等待窗口；窗口到期即提交（批不满也提交），`0` → 立即提交。★ 该窗口是**读可能多等的上界**，也是顺序单写的额外时延（`tests/integration/test_sqlite_group_commit.cpp` 用 `0` 与 `400` 两档断言等待窗口真的生效） |
| `location.sqlite.group_commit_max_batch` | 组提交最大批 | `64` | int 1..100000；默认 `64` | **生效**（C10.20）：一票一批的**操作数上限**（批满立即提交）；`1` = 每操作一批。可观测：`/metrics` 的 `fss_sqlite_ops_total{repo="location"} / fss_sqlite_group_commits_total{repo="location"}` = 平均批大小 |
| `location.postgres.dsn` | PG 连接串（密文） | `${ENV:FSS_PG_DSN}` | string，secret；默认 `""` | **生效**（B1）：已接通 → `PostgresLocationRepositoryOptions.pg.dsn`（以及 `leases.enabled=true` 时的 `PostgresLeaseRepository`，租约表与位置记录同库）。`location.repository=postgres` 时为空或不可达 → **exit 78** + libpq 原文。横幅打印 `location.postgres.dsn=***` |
| `location.postgres.max_connections` | PG 连接数 | `8` | int 1..10000；默认 `8` | **生效**（B1）：已接通 → `PgOptions.max_connections`（位置仓储池；`leases.enabled=true` 时租约仓储共用该值）。★ 该组**没有** `statement_timeout_ms` 键 → 用 `PgOptions` 默认 `5000ms` |

#### 1.2.9 `leases` / `leader_election`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 接通状态（三态：生效 / 拒绝启动 / 已读但无效果） |
| --- | --- | --- | --- | --- |
| `leases.enabled` | 在途状态租约（多实例必需） | `false` | bool；默认 `false`；`multi` 必须 true | **生效**（B1 + C2）：已接通：`false`（默认）= 内存租约（单实例语义逐字不变，上传路径**不做**租约动作）；`true` = **PG 租约** `PostgresLeaseRepository`（表 `staging_leases`，时间源由 `leases.time_source` 决定）——创建失败（DSN 空/不可达）→ **exit 78** + 可读原因。★ C2 起上传路径**真的**有调用方：`GetUploadLocation` 发地址时 `Acquire`（别的实例持有未过期租约 → 503 且不发地址），`CreateFileMetadata` 在复制/校验和期间 `Renew`（`LeaseRenewer`）、`MarkReady`/回滚后 `Release`；GC 用 `ClaimExpired` 的结果驱动 `ReclaimStaleClaiming` 回收崩溃者的 claiming 行。横幅 `lease backend` 行给出实际后端与时间源 |
| `leases.ttl_seconds` | 租约 TTL | `60` | int 1..86400；默认 `60` | **生效**（C2）：接通 → `UseCasePorts.lease_ttl_seconds`（上传时 `Acquire` 的 TTL）与 `GcOptions.lease_ttl_seconds`（回收 claiming 行的**二级年龄阈值**）。★ 可观测效果**要求 `leases.enabled=true`**（默认 `false` 时上传路径不做租约动作，单实例行为逐字不变）。证据：`tests/integration/test_postgres_lease_lifecycle.cpp` 直接查 `staging_leases`（`expires_at - now() ≈ ttl_seconds`，行确实存在）；内存租约仓储**按构造是本地钟**，不兑现 `database` |
| `leases.renew_interval_seconds` | 续租间隔（必须小于 TTL） | `20` | int 1..86400；默认 `20`；跨字段 `renew < ttl` | **生效**（C2）：接通 → `LeaseRenewer` 的续租周期（`CreateFileMetadata` 的复制/校验和期间的后台线程）。★ 同样**要求 `leases.enabled=true`**（`false` 时连线程都不创建）。证据：`tests/integration/test_lease_lifecycle.cpp` 用会阻塞 `copy` 的存储替身证明 `expires_at` 真的往后走、且并发 GC 不回收在途 staging 对象 |
| `leases.time_source` | 时间源（`database` = PG `now()`） | `database` | enum `database` / `local`；默认 `database` | **生效**（C2）：接通 → `PostgresLeaseRepositoryOptions.time_source`。`database`（默认）= `Acquire`/`Renew`/`ClaimExpired` **一致**用 PG `now()`（推荐：消除实例间时钟偏移）；`local` = 三者一致用注入的 `IClock`（`expires_at = to_timestamp(clock.NowEpochSeconds() + ttl)`），供单实例/受控测试跟随可注入时钟。★ **内存租约仓储按构造就是本地钟**，不兑现 `database`（它只在 `leases.enabled=false` 下装配）。证据：`tests/integration/test_postgres_lease_lifecycle.cpp`（`local` 跑与 `database` 相同的共享契约测试 + 两者与墙钟/手钟的对照） |
| `leader_election.enabled` | 单例后台任务的领导者选举 | `false` | bool；默认 `false`；`multi` 必须 true | **生效**（B1）：已接通：`true` = 创建 `PgLeaderElection`（**专用锁连接**，只跑短语句）+ 启动时 `pg_try_advisory_lock`；**GC 的周期调度与按需端点都以 `IsLeader()` 门控**（非 leader 不调用 `GcTask::Run` ⇒ `fss_gc_runs_total` 不增加）。创建/取锁失败 → **exit 78**。`false` = 不做门控（单实例语义逐字不变）。横幅新增 `leader` 行给出 backend/lock_key/启动时角色 |
| `leader_election.backend` | 选举后端 | `postgres_advisory_lock` | enum `postgres_advisory_lock`；默认 `postgres_advisory_lock` | **生效**（B1）：已接通（组合根校验并写入横幅）。当前只有这一个取值（schema 枚举拒绝其它值；组合根对改宽的枚举再兜一层 → exit 78） |
| `leader_election.lock_key` | advisory lock key | `1179865927` | int 0..2147483647；默认 `1179865927` | **生效**（B1）：已接通 → `pg_try_advisory_lock($1::bigint)` / `pg_advisory_unlock($1::bigint)`；可 `SELECT objid,pid FROM pg_locks WHERE locktype='advisory'` 直接观察。★ 同键的两个实例**恰好一个** leader（`tests/integration/test_multi_mode.cpp` 的 B1-3/B1-4） |

#### 1.2.10 `auth`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 接通状态（三态：生效 / 拒绝启动 / 已读但无效果） |
| --- | --- | --- | --- | --- |
| `auth.mode` | 鉴权模式 | `jwt` | enum `jwt` / `remote-entitlements` / `disabled`；schema 默认 `jwt` | **生效**：已接通（`auth.mode` / 通用名 `FSS_AUTH_MODE`；**组合根历史默认 `disabled`**；非法值 → exit 78）。`deployment.environment=production` 时必须是 `jwt` |
| `auth.jwt.jwks_url` | RS256/JWKS 地址（**未实现**） | `""` | string；默认 `""`；非空 → 拒绝启动 | **拒绝启动（触发条件）**：非空 → exit 78（RS256/JWKS 未实现，ADR-012 §5.3）。下一步：保持空的字符串，只用 HS256 |
| `auth.jwt.issuer` | 必须匹配的 `iss` | `""` | string；默认 `""` | **生效**：已接通（`auth.jwt.issuer`；旧别名 `FSS_JWT_ISSUER`）→ `LocalJwtOptions.issuer` |
| `auth.jwt.audience` | 必须包含的 `aud` | `""` | string；默认 `""` | **生效**：已接通（`auth.jwt.audience`；旧别名 `FSS_JWT_AUDIENCE`）→ `LocalJwtOptions.audience` |
| `auth.jwt.verify_signature` | 是否验签 | `true` | bool；默认 `true`；production 必须 true | **生效**：已接通（`auth.jwt.verify_signature`；旧别名 `FSS_JWT_VERIFY_SIGNATURE`，旧语义"只有字面 `false` 才关"）→ `LocalJwtOptions.verify_signature` |
| `auth.jwt.roles_claim` | 角色 claim 名 | `roles` | string；默认 `roles` | **生效**：已接通（`auth.jwt.roles_claim`，默认 `roles`）→ `LocalJwtOptions.roles_claim`；token 的角色 claim 名不再固定为 `roles`（启动横幅打印实际 claim 名） |
| `auth.jwt.user_id_claim` | 用户 claim 名 | `email` | string；默认 `email` | **生效**：已接通（`auth.jwt.user_id_claim`；旧别名 `FSS_JWT_USER_ID_CLAIM`）→ `LocalJwtOptions.user_id_claim` |
| `auth.jwt.hmac_secret` | HS256 共享密钥（密文） | `${ENV:FSS_JWT_HMAC_SECRET}` | string，secret；默认 `""` | **生效**：已接通（`auth.jwt.hmac_secret`；旧别名 `FSS_JWT_HMAC_SECRET`）→ `LocalJwtOptions.hmac_secret` / `HmacTransferTokenCodec` 无关。空且 `auth.mode=jwt` → **exit 78** |
| `auth.jwt.partition_claim` | 租户绑定 claim 名 | `data-partition-id` | string；默认 `data-partition-id` | **生效**：已接通（`auth.jwt.partition_claim`；旧别名 `FSS_JWT_PARTITION_CLAIM`）→ `LocalJwtOptions.partition_claim` |
| `auth.jwt.require_partition_claim` | 是否强制租户 claim | `true` | bool；默认 `true` | **生效**：已接通（`auth.jwt.require_partition_claim`；旧别名 `FSS_JWT_REQUIRE_PARTITION_CLAIM`）→ `LocalJwtOptions.require_partition_claim` |
| `auth.local_roles.admin@example.com` | 静态角色表（用户 → 角色数组） | 8 个 `service.*` 角色 | 动态子树（`auth.local_roles` 前缀），无逐键约束 | **生效**：已接通 → `LocalJwtOptions.local_roles`（组合根从 `auth.local_roles` 动态子树枚举）；与 token 里的角色取**并集**，真的参与 200/403 判定 |
| `auth.local_roles.editor@example.com` | 静态角色表项 | `["service.file.editors","service.file.viewers"]` | 同上 | **生效**：同上（`LocalJwtOptions.local_roles`） |
| `auth.local_roles.viewer@example.com` | 静态角色表项 | `["service.file.viewers"]` | 同上 | **生效**：同上（`LocalJwtOptions.local_roles`） |
| `auth.remote_entitlements.base_url` | 远端 Entitlements 地址 | `""` | string；默认 `""`；`remote-entitlements` 时必须非空 | **生效**：已接通（`auth.remote_entitlements.base_url`；旧别名 `FSS_ENTITLEMENTS_URL`）→ `RemoteEntitlementsOptions.base_url`；空值 + `remote-entitlements` → exit 78 |
| `auth.remote_entitlements.authorize_path` | authorizeAny 路径 | `/api/entitlements/v2/authorizeAny` | string；默认 `/api/entitlements/v2/authorizeAny` | **生效**：已接通（`auth.remote_entitlements.authorize_path`；旧别名 `FSS_ENTITLEMENTS_AUTHORIZE_PATH`） |
| `auth.remote_entitlements.connect_timeout_ms` | 连接超时 | `1000` | int 1..600000；默认 `1000` | **生效**：已接通（`auth.remote_entitlements.connect_timeout_ms`）→ `RemoteEntitlementsOptions.connect_timeout_ms` |
| `auth.remote_entitlements.timeout_ms` | 整体超时 | `3000` | int 1..600000；默认 `3000` | **生效**：已接通（`auth.remote_entitlements.timeout_ms`；旧别名 `FSS_ENTITLEMENTS_TIMEOUT_MS`）→ `RemoteEntitlementsOptions.timeout_ms` |
| `auth.remote_entitlements.fail_closed` | 依赖不可用不得降级为放行 | `true` | bool；默认 `true`；**`auth.mode=remote-entitlements` 时必须 true** | **拒绝启动（触发条件）**：`auth.mode=remote-entitlements` **且** 该键非 `true` → **exit 78** + 「必须为 true：依赖不可用不可降级为放行」（`core_schema.cpp` 的跨字段校验；ADR-012 §5.1）。**触发条件是模式相关的**：`auth.mode=jwt`/`disabled` 时 `false` 被接受且**无任何影响**（实现内恒为 fail-closed，不存在"失败即放行"的分支）。证据：`tests/integration/test_config_wiring.cpp` 的 C10.11 两个 SECTION（反例 exit 78 + R16 正例不被拒 + 模式无关性） |

#### 1.2.11 `legal` / `schema` / `events`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 接通状态（三态：生效 / 拒绝启动 / 已读但无效果） |
| --- | --- | --- | --- | --- |
| `legal.validator` | 法务标签校验器 | `noop` | enum `noop` / `remote`；默认 `noop` | **生效**：已接通（`legal.validator`）。`noop`（默认）= 只保留本地空值防线（`legaltags` 为空 → 400），**不发起任何请求**（行为与接线前逐字一致）；`remote` = 装配 `RemoteLegalValidator`（ADR-013），校验请求发到 `legal.remote.base_url`（**完整端点**）。非法取值由 schema 的 enum 拒绝 → exit 78 |
| `legal.remote.base_url` | 远端法务校验**完整端点 URL** | `""` | string；默认 `""`；`validator=remote` 时必须非空 | **生效**：已接通（`legal.remote.base_url`）→ `RemoteLegalValidatorOptions.base_url`。**POST 到该 URL，不追加任何路径**（与 `auth.remote_entitlements.authorize_path` 那种"base_url + path"不同：legal 没有 path 键，见 ADR-013 §2）。空值 + `validator=remote` → **exit 78**（`Ready()`/`NotReadyReason()`）。⚠️ 端口签名不带 bearer token → 端点必须允许**无 per-request 认证**访问（集群内网 / mTLS 终结），且**未与真实 Legal 服务联调** |
| `legal.remote.timeout_ms` | 远端法务校验整体超时 | `3000` | int 1..600000；默认 `3000` | **生效**：已接通（`legal.remote.timeout_ms`）→ `RemoteLegalValidatorOptions.timeout_ms`（libcurl `CURLOPT_TIMEOUT_MS`）。超时 → **503**（fail-closed）。证据：**同一个 mock** 下 `300ms` → 503、`3000ms` → 201（证明配置真的在起作用，而不是"永远 503"） |
| `schema.validator` | schema 校验器 | `noop` | enum `noop` / `remote`；默认 `noop` | **生效**：已接通（`schema.validator`）。`noop`（默认）**不发起任何请求**；`remote` = 装配 `RemoteSchemaValidator`（ADR-013），请求发到 `schema.remote.base_url`（**完整端点**）。非法取值 → exit 78 |
| `schema.remote.base_url` | 远端 schema 校验**完整端点 URL** | `""` | string；默认 `""`；`validator=remote` 时必须非空 | **生效**：已接通（`schema.remote.base_url`）→ `RemoteSchemaValidatorOptions.base_url`。**POST 到该 URL，不追加任何路径**。空值 + `validator=remote` → **exit 78**。⚠️ 同样不透传调用方身份 → 端点必须允许**无 per-request 认证**访问；**未与真实 Schema 服务联调** |
| `schema.remote.timeout_ms` | 远端 schema 校验整体超时 | `3000` | int 1..600000；默认 `3000` | **生效**：已接通（`schema.remote.timeout_ms`）→ `RemoteSchemaValidatorOptions.timeout_ms`（`CURLOPT_TIMEOUT_MS`）。超时 → **503**（fail-closed） |
| `events.publisher` | 事件发布器 | `log` | enum `log` / `webhook` / `none`；默认 `log` | **生效**：已接通（`events.publisher`）。`log`（默认）= 既有 `LogEventPublisher`（写日志，**不发请求**，行为逐字不变）；`webhook` = 装配 `WebhookEventPublisher`（ADR-013 §9），把 `statusChanged` / `datasetDetails` POST 到 `events.webhook.url`（**完整端点**）；`none` = **显式关闭**事件发布（不发任何请求，**不是**"没实现"）。非法取值由 schema 的 enum 拒绝 → exit 78 |
| `events.webhook.url` | webhook **完整端点 URL** | `""` | string；默认 `""`；`publisher=webhook` 时必须非空 | **生效**：已接通（`events.webhook.url`）→ `WebhookEventPublisherOptions.url`。**POST 到该 URL，不追加任何路径**（与切片 6a 的 `*.remote.base_url` 同一约定，ADR-013 §2/§9）。空值 + `publisher=webhook` → **exit 78** + 可读原因（`Ready()`/`NotReadyReason()`）。⚠️ **发布失败非致命**（见 `timeout_ms` 行）；**未与真实消息总线/中间件联调** |
| `events.webhook.timeout_ms` | webhook 整体超时 | `3000` | int 1..600000；默认 `3000` | **生效**：已接通（`events.webhook.timeout_ms`）→ `CURLOPT_TIMEOUT_MS`（连接超时 = `min(1000, timeout_ms)`）。**2xx = 成功；非 2xx / 连不上 / 超时 → 记一条可读告警并继续**（仅 `publisher=webhook`）：请求照常 **201**、记录照常落库（上游依据：契约 §2.6 第 4/10 步只 `log.warning("Failed to publish ...")`）。⚠️ **本实现是内联同步发布**（上游是异步消息总线）：一次 `createMetadata` 会发 2~3 个事件，因此一个慢 webhook 最多给请求路径增加 **事件数 × timeout_ms**；**异步有界发布队列 / 重试退避未交付**（ADR-013 §9.4） |
| `events.webhook.topic` | webhook topic（**载荷里的 `topic`**） | `status-changed` | string；默认 `status-changed` | **生效**：已接通（`events.webhook.topic`）→ 两个事件载荷里的 `topic` 字段（`statusChanged` 与 `datasetDetails` **都用它**，而不是端口传入的 kind 名）。证据：真实进程 + mock 配非默认值 `fss-events-test` → mock 收到的每一个 body 的 `topic` 都是它 |

#### 1.2.12 `partition`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 接通状态（三态：生效 / 拒绝启动 / 已读但无效果） |
| --- | --- | --- | --- | --- |
| `partition.registry` | 租户注册表来源 | `file` | enum `file` / `remote`；默认 `file` | **拒绝启动（触发条件）**：`remote` → exit 78（远端租户注册表未交付）。下一步：保持 `file` |
| `partition.file.opendes.staging_container` | staging 容器名 | `opendes-staging` | 动态子树（`partition.file` 前缀），无逐键约束 | **生效**：接通 `partition.file.<p>.staging_container` → `PartitionConfig::staging_container` → `ObjectKeyPolicy::ContainerFor(PartitionConfig&, zone)`（uploadURL 签发 / 用例 / GC / 启动建目录都走同一条）；空串 = 默认 `<partition>-staging`（逐字一致）。非法字符（控制字符 / `/` / `\\` / `%`）→ exit 78。证据：真实进程下 `custom-stage-1` 目录出现、`opendes-staging` 不出现 |
| `partition.file.opendes.persistent_container` | persistent 容器名 | `opendes-persistent` | 同上 | **生效**：同 `staging_container`（→ `PartitionConfig::persistent_container`）；空串 = 默认 `<partition>-persistent`。证据：真实进程下 `custom-persist-1` 目录出现 |
| `partition.file.opendes.storage_driver` | 分区级驱动覆盖 | `posix` | 同上 | **拒绝启动（触发条件）**：与顶层 `storage.driver` **不一致** → exit 78（分区级驱动覆盖未交付：组合根只装配一个 BlobStore，`SingleStoreFactory` 的所有 partition/zone 共用它）；等于顶层值（或空）→ 正常启动（R16 正例）。下一步：删除该键（跟随顶层），或等按 partition/zone 分盘的工厂交付 |
| `partition.file.opendes.max_file_bytes` | 单对象上限（0 = 不限） | `0` | 同上 | **生效**（C10.16）：接到 `PartitionConfig.max_object_bytes`（0 → -1 = 不限），并落到数据面 PUT 的 `RouteOptions::max_body_bytes`；超限 → **413**（带 `Content-Length`，读体前前置拒绝）/ 400（chunked）。上传用例见 `tests/integration/test_config_wiring.cpp` 的 C10.16 |
| `partition.file.opendes.allowed_checksum_algorithms` | 允许的校验和算法 | `["SHA-256","MD5","SHA-1"]` | 同上 | **拒绝启动（触发条件）**：含未知算法名 → exit 78。**为什么不是"请求期 400"**：C6.4 有上游一手证据 —— 客户端声明的算法是**被覆写**的输入，不是待校验断言（`File_CorrectPayload.json` 声明 SHA-256 却给 MD5 值，期望 201）。下一步：保持 SHA-256 / SHA-1 / MD5 的子集 |
| `partition.file.opendes.default_checksum_algorithm` | 缺省校验和算法 | `SHA-256` | 同上 | **拒绝启动（触发条件）**：未知算法名，或不在 `allowed_checksum_algorithms` 集合内 → exit 78（与上一键是跨字段校验）。下一步：保证它属于 allowed 集合 |

#### 1.2.13 `gc`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 接通状态（三态：生效 / 拒绝启动 / 已读但无效果） |
| --- | --- | --- | --- | --- |
| `gc.enabled` | GC 开关 | `false` | bool；默认 `false` | **生效**：`true` → 组合根启动 GC 后台调度（`GcTask` 真实装配）；`false`（默认）→ 不启动并在横幅说明原因。不提供配置时保持「不跑 GC」（与接线前一致） |
| `gc.dry_run` | 只记录不删除 | `true` | bool；默认 `true` | **生效**：`true`（默认）只报候选不删除；`false` 才真的删（`--once` 与周期调度同源） |
| `gc.require_lease_expiry` | 必须"租约到期 + 无记录"才回收 | `true` | bool；默认 `true`；`multi` 必须 true | **生效**：`true`（默认）= 只回收「租约到期 **且** 无元数据记录」的对象；`false` = 单实例降级为按 staging TTL 扫描（多实例下 schema 强制 true） |
| `gc.staging_ttl_hours` | staging TTL（小时） | `24` | int 1..87600；默认 `24` | **生效**：staging 对象与 `.tmp.*` 残留的 TTL 判据（`cutoff = now - N*3600`） |
| `gc.orphan_grace_hours` | persistent 孤儿宽限（小时） | `72` | int 1..87600；默认 `72` | **生效**：persistent 孤儿的宽限期（没有任何位置记录引用且超过该值才回收） |
| `gc.interval_seconds` | GC 调度间隔 | `3600` | int 1..86400；默认 `3600` | **生效**：后台调度周期（秒）；第一轮立即跑，之后按它重复。`<=0` → 不启动调度并在横幅说明（schema 下界 1，防止空转压满机器） |

#### 1.2.14 `observability`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 接通状态（三态：生效 / 拒绝启动 / 已读但无效果） |
| --- | --- | --- | --- | --- |
| `observability.log_level` | 最低日志级别 | `info` | enum `debug` / `info` / `warn` / `error`；默认 `info` | **生效**：已接通（`observability.log_level`；旧别名 `FSS_LOG_LEVEL`）→ `logging::OptionsFromConfig` 的 `min_level` |
| `observability.log_format` | 日志格式 | `json` | enum `json` / `text`；默认 `json` | **生效**：已接通（`observability.log_format`；旧别名 `FSS_LOG_FORMAT`）→ `LogOptions.format` |
| `observability.audit_enabled` | 审计开关 | `true` | bool；默认 `true` | **生效**：已接通（`observability.audit_enabled`）：`false` → 审计器换成 **no-op**（端口仍有实现，只是不记录） |
| `observability.audit_fail_closed` | 审计失败是否影响操作结果 | `false` | bool；默认 `false` | **生效**：已接通（`observability.audit_fail_closed`）→ `UseCasePorts.audit_fail_closed`：`true` 时审计写入失败让请求以 **500** 结束（契约 §5 的 `kInternal`，绝不谎报成功）；`false`（默认）→ 审计失败非致命（接线前的行为）。可驱动接缝：`FSS_AUDIT_FAULT_INJECT=1`（故障注入，**不是**配置键；见 §8） |
| `observability.metrics_enabled` | 指标端点开关 | `true` | bool；默认 `true` | **生效**：已接通（`observability.metrics_enabled`）→ `RouterOptions.metrics_enabled`；`false` 时**不注册** `/metrics` 路由 |
| `observability.metrics_path` | 指标端点路径 | `/metrics` | string；默认 `/metrics` | **生效**：已接通（`observability.metrics_path`）→ `RouterOptions.metrics_path`；必须以 `/` 开头，否则 exit 78 |
| `observability.redact_keys` | 日志/诊断输出要打码的键名（子串匹配，忽略大小写与 `_`/`-`） | 11 个键名 | list；默认 `secret_key,access_key,token,sig,signature,authorization,x-amz-signature,password,signing_key,dsn,static_token` | **生效**：已接通（`observability.redact_keys`；旧别名 `FSS_LOG_REDACT_KEYS`，逗号分隔）→ `LogOptions.redact_keys`（经 `logging::OptionsFromConfig`）；启动横幅打印实际键数 |

### 1.3 接通状态三态（逐键核对；合计 **157** 个叶子键）

> 口径：**组合根在真实进程里对每个键做了什么**。三态的定义见 §1.2 开头的表。
> 计数由本节的三个清单逐条相加得出：**128 + 15 + 14 = 157**。
>
> ⚠️ **C9.31（按需 GC 端点）不改这个计数**：它是**能力补齐**（把已实现的 `GcTask` 接到
> HTTP 上），**没有新增/删除任何配置键**，`gc.*` 5 个键的三态与接线前完全一致。
> 端点的可用性由代码决定，**不引入**任何"端点开关"之类的新键（避免配置面为一个端点膨胀，
> 也避免三态清单失真）。

| 状态 | 键数 | 说明 |
| --- | --- | --- |
| **生效** | **128** | 读取后真的改变运行行为（含"非法值拒绝启动"的触发条件，写在 §1.2 对应行） |
| **拒绝启动（触发条件）** | **15** | 非默认/不支持的值 → **exit 78（EX_CONFIG）** + 「未实现 + 下一步」 |
| **已读但无效果** | **14** | 组合根未接线（或读了但行为未实现）→ 改它对真实进程没有影响；理由与下一步逐个登记 |

#### 1.3.1 生效（128）

`auth.jwt.audience`、`auth.jwt.hmac_secret`、`auth.jwt.issuer`、`auth.jwt.partition_claim`、`auth.jwt.require_partition_claim`、`auth.jwt.roles_claim`、`auth.jwt.user_id_claim`、`auth.jwt.verify_signature`、`auth.local_roles.admin@example.com`、`auth.local_roles.editor@example.com`、`auth.local_roles.viewer@example.com`、`auth.mode`、`auth.remote_entitlements.authorize_path`、`auth.remote_entitlements.base_url`、`auth.remote_entitlements.connect_timeout_ms`、`auth.remote_entitlements.timeout_ms`、`deployment.clock_skew_tolerance_seconds`、`deployment.environment`、`deployment.expected_instances`、`deployment.instance_id`、`deployment.max_clock_skew_seconds`、`deployment.mode`、`events.publisher`、`events.webhook.timeout_ms`、`events.webhook.topic`、`events.webhook.url`、`expiry.default`、`expiry.max`、`gc.dry_run`、`gc.enabled`、`gc.interval_seconds`、`gc.orphan_grace_hours`、`gc.require_lease_expiry`、`gc.staging_ttl_hours`、`http.error_format`、`leader_election.backend`、`leader_election.enabled`、`leader_election.lock_key`、`leases.enabled`、`leases.renew_interval_seconds`、`leases.time_source`、`leases.ttl_seconds`、`legal.remote.base_url`、`legal.remote.timeout_ms`、`legal.validator`、`location.postgres.dsn`、`location.postgres.max_connections`、`location.repository`、`location.sqlite.busy_timeout_ms`、`location.sqlite.journal_mode`、`location.sqlite.path`、`location.sqlite.synchronous`、`metadata.postgres.dsn`、`metadata.postgres.max_connections`、`metadata.postgres.schema_version_check`、`metadata.postgres.statement_timeout_ms`、`metadata.repository`、`metadata.sqlite.busy_timeout_ms`、`metadata.sqlite.journal_mode`、`metadata.sqlite.path`、`metadata.sqlite.synchronous`、`observability.audit_enabled`、`observability.audit_fail_closed`、`observability.log_format`、`observability.log_level`、`observability.metrics_enabled`、`observability.metrics_path`、`observability.redact_keys`、`partition.file.opendes.max_file_bytes`、`partition.file.opendes.persistent_container`、`partition.file.opendes.staging_container`、`schema.remote.base_url`、`schema.remote.timeout_ms`、`schema.validator`、`self_signed.default_ttl_seconds`、`self_signed.enabled`、`self_signed.key_id`、`self_signed.max_ttl_seconds`、`self_signed.public_base_url`、`self_signed.signing_key`、`server.grpc.bind`、`server.grpc.enabled`、`server.grpc.port`、`server.http.base_path`、`server.http.bind`、`server.http.idle_timeout_seconds`、`server.http.json_request_timeout_seconds`、`server.http.max_body_bytes`、`server.http.max_connections`、`server.http.max_header_bytes`、`server.http.max_uri_bytes`、`server.http.port`、`server.http.tcp_nodelay`、`server.http.transfer_buffer_bytes`、`server.http.transfer_idle_timeout_seconds`、`server.http.transfer_max_body_bytes`、`server.http.transfer_memory_budget_bytes`、`server.http.worker_threads`、`storage.driver_report_override`、`storage.driver`、`storage.io_engine`、`storage.io_uring.queue_depth`、`storage.posix.atomic_write`、`storage.posix.dir_mode`、`storage.posix.durability`、`storage.posix.fadvise_dontneed_after_large_read`、`storage.posix.fadvise_random`、`storage.posix.file_mode`、`storage.posix.fsync_threshold_bytes`、`storage.posix.group_commit_max_batch`、`storage.posix.root`、`storage.provider_key_override`、`storage.s3.access_key`、`storage.s3.connect_timeout_ms`、`storage.s3.endpoint`、`storage.s3.force_path_style`、`storage.s3.presign_default_seconds`、`storage.s3.presign_max_seconds`、`storage.s3.region`、`storage.s3.secret_key`、`storage.s3.total_timeout_ms`、`storage.s3.verify_tls`

#### 1.3.2 拒绝启动（触发条件）（15）

| 键 | 触发条件（非默认/不支持的值） |
| --- | --- |
| `auth.remote_entitlements.fail_closed` | `auth.mode=remote-entitlements` 且该键非 `true` → exit 78（"依赖不可用不可降级为放行"，ADR-012 §5.1；`core_schema.cpp` 跨字段校验）。**模式相关**：`jwt`/`disabled` 下 `false` 被接受且无影响。下一步：保持 `true` |
| `auth.jwt.jwks_url` | 非空 → exit 78（RS256/JWKS 未实现，ADR-012 §5.3）。下一步：保持空的字符串，只用 HS256 |
| `partition.registry` | `remote` → exit 78（远端租户注册表未交付）。下一步：保持 `file` |
| `partition.file.opendes.allowed_checksum_algorithms` | 含未知算法名（不在 SHA-256 / SHA-1 / MD5 内）→ exit 78。下一步：保持这三个名字的子集（大小写与 `-`/`_` 不计） |
| `partition.file.opendes.default_checksum_algorithm` | 未知算法名，或不在 `allowed_checksum_algorithms` 内 → exit 78。下一步：保证它属于 allowed 集合 |
| `partition.file.opendes.storage_driver` | 与顶层 `storage.driver` 不一致 → exit 78（分区级驱动覆盖未交付；等于顶层值或为空 → 正常启动）。下一步：删除该键（跟随顶层），或等按 partition/zone 分盘的工厂交付 |
| `self_signed.nonce_store` | 非 `memory` → exit 78（只有内存 nonce 存储，且 `single_use_nonce=false` 时不用它）。下一步：保持 `memory` |
| `self_signed.single_use_nonce` | `true` → exit 78（nonce 存储未交付，ADR-009 M5）。下一步：保持 `false` |
| `server.grpc.max_message_bytes` | 非默认（`4194304`）→ exit 78（gRPC 侧消息上限未接通）。下一步：保持默认 |
| `server.grpc.streaming_chunk_bytes` | 非默认（`262144`）→ exit 78（gRPC 流式分块未接通）。下一步：保持默认 |
| `server.http.large_file_plane.enabled` | `=true` → exit 78（sendfile 数据面未交付，ADR-006 只定稿方向）。下一步：保持 `false`（默认，走 httplib 内容提供者） |
| `server.http.max_connections_per_partition` | 非 `0` → exit 78（每租户并发上限未实现）。下一步：保持 `0`，用 `server.http.max_connections` 表达全局上限 |
| `storage.io_uring.register_files` | `true` → exit 78（io_uring 引擎未启用，ADR-010）。下一步：保持 `false` |
| `storage.proxy_mode` | `always` → exit 78（「强制服务代理所有字节」未实现）。下一步：保持 `auto` |
| `storage.posix.sync_dir_after_batch` | `false` → exit 78（**ADR-008 §5 的 R2 是**不变量**：rename 之后必须 `fsync(目录)`**，schema 描述也写明必须 `true`）。下一步：保持 `true` |

#### 1.3.3 已读但无效果（14）

> 这些键**改了不生效**（组合根不读，或读了但没有行为分支）。每一条都给出**下一步**；
> 其中 8 个键（`expiry.*` 与 `gc.*`）在切片 2、**9 个键**在切片 3 已从本清单移入「生效」，见 §1.3.1
> （切片 3：`observability.audit_fail_closed`、`metadata.sqlite.busy_timeout_ms`、`location.sqlite.{busy_timeout_ms,journal_mode}`、`auth.jwt.roles_claim`、`auth.local_roles.*`（3）、`server.grpc.enabled`）。
> **C10.16 的净变化**：`partition.file.opendes.max_file_bytes` 移入「生效」（§1.3.1）；`allowed/default_checksum_algorithm` 移入「拒绝启动」（§1.3.2）。
> **C10.16 续（本轮）的净变化**：`storage.posix.{atomic_write,dir_mode,file_mode,fadvise_random,fadvise_dontneed_after_large_read}` 与 `partition.file.opendes.{staging,persistent}_container` 移入「生效」（+7，§1.3.1）；`storage.posix.{group_commit_max_batch,sync_dir_after_batch}`（ADR-008 的 P4 未实现）与 `partition.file.opendes.storage_driver`（与顶层驱动冲突）移入「拒绝启动」（+3，§1.3.2）。此前「这 10 个键没有对应结构体字段」的结论**已被本轮推翻**：`PosixBlobStoreOptions` / `PartitionConfig` 已加上真实字段（记录见 `docs/00-final-design.md` §5）。
> **切片 4（本轮）的净变化**：`server.http.transfer_max_body_bytes`、`metadata.sqlite.{journal_mode,synchronous}`、`location.sqlite.synchronous` 移入「生效」（+4，§1.3.1）。两个 `Sqlite*RepositoryOptions` 新增了 `wal` / `synchronous_level` 真实字段并真的执行 PRAGMA；`synchronous` **不落盘**，用新增的同连接访问器 `AppliedPragma("synchronous")` 断言（"另开 sqlite3 连接读回"对它无效）。此前「元数据仓储内硬编码 WAL、Options 里没有这些字段」的结论**已被本轮推翻**。
> **切片 5（本轮）的净变化**：`self_signed.{key_id,default_ttl_seconds,max_ttl_seconds}` 移入「生效」（+3，§1.3.1）。`key_id` 进**被签名的 token 载荷**并在解码侧 fail-closed；两个 TTL 键是**自签分支的上界**（`expiry.*` 语义不变）。
> **切片 6a（C10.18）的净变化**：`legal.validator` / `schema.validator` 移出「拒绝启动」（不再是"未实现 → exit 78"），`legal.remote.{base_url,timeout_ms}` / `schema.remote.{base_url,timeout_ms}` 移出本清单 —— **6 个键**（2 个来自「拒绝启动」+ 4 个来自本清单）全部移入「生效」（+6，§1.3.1）。实现是新增的 L2 适配器 `src/infra/legal/remote_legal_validator.*` 与 `src/infra/schema/remote_schema_validator.*`（ADR-013；**平台外扩展**端点，`base_url` 即完整 URL，不追加路径）。⚠️ **未与真实 Legal/Schema 服务联调**；端口签名不带 bearer token → 端点必须允许无 per-request 认证访问（见 §1.2.11 的逐行说明与契约 §7）。
> **伴随更正（独立理由，与切片 6a 的 6 个键无关）**：`auth.remote_entitlements.fail_closed` 从本清单移入「拒绝启动」（§1.3.2）—— 它有一条**真实可观测**的效果：`auth.mode=remote-entitlements` 且该键非 `true` → exit 78（`core_schema.cpp` 的跨字段校验）。触发条件是**模式相关**的（`jwt`/`disabled` 下 `false` 无影响）。原先标成「已读但无效果」与同一行里"`remote` 模式下 schema 会拒绝 `false`"的说明**自相矛盾**，这才是更正的理由；它**不是**为了凑任何数字（落点也不同：本清单 −1、拒绝启动 +1）。
> **切片 6b（C10.19）的净变化**：`events.publisher` 移出「拒绝启动」（不再是「`webhook`/`none` → exit 78」），`events.webhook.{url,timeout_ms,topic}` 移出本清单 —— **4 个键**（1 个来自「拒绝启动」+ 3 个来自本清单）全部移入「生效」（+4，§1.3.1）。实现是新增的 L2 适配器 `src/infra/event/webhook_event_publisher.*`（ADR-013 §9）＋组合根内联的 `NoopEventPublisher`（`publisher=none` 的**显式关闭**）。⚠️ **发布失败非致命**（连不上 / 超时 / 非 2xx → 只记告警，请求照常 **201** 且记录真的落库）；**内联同步发布**（一个慢 webhook 给请求路径增加 事件数 × timeout；异步队列 / 重试退避 / 投递保证**未交付**）；**未与真实消息总线 / 中间件联调**。
> **C10.20（本切片）的净变化**：`metadata.sqlite.{group_commit,group_commit_max_wait_ms,group_commit_max_batch}` 与 `location.sqlite.{group_commit,group_commit_max_wait_ms,group_commit_max_batch}` —— **6 个键**从本清单移入「生效」（+6，§1.3.1）；三态从 **107/18/31** 变为 **113/18/25**。实现是新增的 L2 共用小工具 `src/infra/sqlite/sqlite_group_commit.h`：并发写操作凑成一批 → **一个事务一次 `COMMIT`**；批内每操作 `SAVEPOINT`（**每操作原子性**：只有失败的那个操作回滚）；整批 `COMMIT` 失败 → 批内**所有**操作返回该错误（**有意的语义**，见该文件头注释）。⚠️ **代价**：批事务期间领队持有连接互斥 ⇒ 同仓储的**读可能多等 ≤ `group_commit_max_wait_ms`**；且顺序单文件写每操作最多多等一个窗口（并发才有摊销）——**组提交不是纯免费收益**。`*.sqlite.max_write_concurrency` **仍留本清单**：单连接 + 互斥 ⇒ 实际并发恒为 1，改它无可观测效果。
> 可观测性新增两族 `fss_sqlite_{group_commits,ops}_total{repo=...}`（§4）；真实进程证据见 `docs/test-evidence/phase10.md` 的 C10.20 一节。
> **ADR-008 的 P4 / C9.23（本切片）的净变化**：`storage.posix.group_commit_max_batch` 从「拒绝启动」移入「生效」（+1，§1.3.1）；三态从 **106/19/31** 变为 **107/18/31**。`storage.posix.durability=batch` 从"近似成 `kBySize`"改为**真两阶段批提交**；`sync_dir_after_batch=false` **仍是拒绝启动**（R2 不变量，§1.3.2，理由与"未实现"无关）。可观测性新增三族 `fss_posix_{syncfs,group_commits,batch_objects}_total`（§4）。
> **B1（本切片）的净变化**：`metadata.postgres.{dsn,max_connections,statement_timeout_ms}`（3）、`location.postgres.{dsn,max_connections}`（2）、`leader_election.{enabled,backend,lock_key}`（3）从「已读但无效果」/「拒绝启动」移入「生效」，`leases.enabled`（1）从「拒绝启动」移入「生效」—— **9 个键**（2 个来自「拒绝启动」+ 7 个来自本清单）；三态从 **113/18/25** 变为 **122/16/18**。实现：组合根创建 PG 位置/元数据仓储、PG 租约（`staging_leases`）与新增 L2 类 `src/infra/postgres/pg_leader_election.*`（**专用锁连接** + `pg_try_advisory_lock`，`IsLeader()` 每轮**验证连接健康**）；GC 的周期调度与按需端点都由 leader 门控（非 leader 不调用 `GcTask::Run` ⇒ `fss_gc_runs_total` 不增加）。
> **C2（本切片）的净变化**：`leases.{ttl_seconds,renew_interval_seconds,time_source}` 从本清单移入「生效」（+3，§1.3.1）——三态从 **122/16/18** 变为 **125/16/15**。实现：上传路径 `Acquire`（`GetUploadLocation`）+ 复制/校验和期间 `Renew`（新增 L4 小工具 `src/app/services/lease_renewer.*`，RAII、`stop()+join()` 覆盖每条退出路径、失败记录并在步骤边界 fail-closed）+ `MarkReady`/回滚后 `Release`；新增端口方法 `IMetadataRepository::ReclaimStaleClaiming`（内存/SQLite/PG 三实现）由 GC 用 `ClaimExpired` 的返回（**已原子领取的过期租约**）驱动，回收崩溃者留下的 `claiming` 行；PG 租约新增 `time_source`。可观测性新增 `fss_gc_reclaimed_claiming_total`（§4）。⚠️ `leases.enabled=false`（单实例默认）时上传路径不做任何租约动作 ⇒ 该形态下崩溃者的 `claiming` 行**仍无自动回收**（只有租约存在时才有"已死"的证明）；这是本切片如实登记的边界。
> ★ **仍然留在本清单的键（不随 B1 移动，避免"文档说生效、实际没有"）**：`storage.posix.{shared_mount_required,one_filesystem_per_partition}`（**共享挂载探针未实现**；`shared_mount_required` 只参与 schema 的 multi 拒绝启动判定，那在接线前就已如此）。
> **B2a（本切片）的净变化**：新增 `deployment.expected_instances`（1）+ `metadata.postgres.schema_version_check`（从本清单移入）+ `deployment.clock_skew_tolerance_seconds`（从「拒绝启动」移入）——**3 个键**进入「生效」（§1.3.1）；三态从 **125/16/15** 变为 **128/15/14**，键数 156 → **157**。实现：① readiness 的 PG 探活 + 迁移版本比对（`src/infra/postgres/pg_schema.*`；期望值 = `kExpectedSchemaVersion`，由 `tests/unit/test_schema_version_constant.cpp` 从 `db/migrations/*.sql` 文件名**机械推导**，加迁移不改常量 → 测试失败）；② C9.28 连接预算（`expected_instances × 每实例最坏连接数 ≤ PG max_connections`，否则 exit 78）；③ ADR-009 §6.4/§8.1 的本地钟 vs PG `now()` 偏移比对（超限 exit 78）。★ `deployment.max_clock_skew_seconds` **仍生效**但**不**参与第 ③ 条：它继续是 multi 跨字段校验与 JWT `exp`/`nbf` 容忍。测试接缝 `FSS_CLOCK_SKEW_INJECT_MS`（**环境变量，不是配置键**，见 runbook §10.3）。

`location.sqlite.group_commit_max_batch`、`location.sqlite.group_commit_max_wait_ms`、`location.sqlite.group_commit`、`location.sqlite.max_write_concurrency`、`metadata.remote.base_url`、`metadata.remote.static_token`、`metadata.remote.timeout_ms`、`metadata.remote.token_provider`、`metadata.sqlite.group_commit_max_batch`、`metadata.sqlite.group_commit_max_wait_ms`、`metadata.sqlite.group_commit`、`metadata.sqlite.max_write_concurrency`、`server.http.large_file_plane.bind`、`server.http.large_file_plane.max_connections`、`server.http.large_file_plane.port`、`server.http.large_file_plane.sendfile_chunk_bytes`、`server.http.large_file_plane.use_sendfile`、`server.http.large_file_plane.workers`、`storage.posix.one_filesystem_per_partition`、`storage.posix.shared_mount_required`

**理由与下一步（按前缀归类）**

| 前缀 | 键数 | 为什么不生效 / 下一步 |
| --- | --- | --- |
| server.http | 6 | `large_file_plane.bind/port/use_sendfile/sendfile_chunk_bytes/workers/max_connections`（6 个）随 sendfile 数据面一起未交付；`transfer_max_body_bytes` 已在**切片 4** 接通（§1.3.1）。下一步：ADR-006 §6 落地数据面后才接通（`large_file_plane.enabled` 见 §1.3.2） |
| `storage` | 2 | 只剩 `shared_mount_required`/`one_filesystem_per_partition`（**共享挂载探针未实现** —— B1 交付了 multi 运行形态，但**没有**交付"写探针文件看别的实例"这条探测）。C10.16 续已把 `atomic_write`/`dir_mode`/`file_mode`/`fadvise_random`/`fadvise_dontneed_after_large_read` 接到驱动层（§1.3.1）；`group_commit_max_batch` 也已接通（ADR-008 的 P4，§1.3.1），`sync_dir_after_batch=false` 保持拒绝启动（R2 不变量，§1.3.2）。下一步：实现共享挂载探针（与 PG 协作）后才接通这两个 |
| `self_signed` | 0 | **切片 5 已清空**：`key_id`/`default_ttl_seconds`/`max_ttl_seconds` 全部接通（§1.3.1），`single_use_nonce`/`nonce_store` 见 §1.3.2。⚠️ **下一步（有界差异）**：多密钥轮换（ADR-009:227 的"多 key 并存"）**未交付** —— 当前 `key_id` 只是"绑定 + 解码侧拒绝不匹配"，**没有**"按 id 选密钥"。若要做轮换，必须把 `key_id → 密钥` 表引入 codec 并同步契约/文档/测试。另：**若要改成「`self_signed.*` 覆盖 `expiry.default` 作缺省」，必须先推翻 C10.12 的既定语义**（`expiry.default`/`expiry.max` = `expiryTime` 参数的解析规则与缺省）并同步契约与 `tests/integration/test_config_wiring.cpp` 的 C10.12 用例 —— 本切片刻意**不**这么做，改用「上界夹紧」以免静默改变已定稿语义 |
| `metadata` | 5 | `busy_timeout_ms`（切片 3）、`journal_mode`/`synchronous`（**切片 4**）、`group_commit*` 三键（**C10.20**）与 `postgres.{dsn,max_connections,statement_timeout_ms}`（**B1**）已接通（§1.3.1）；`postgres.schema_version_check` 已在 **B2a** 接通（readiness 的 `SELECT 1` + 迁移版本比对，§1.3.1）；`max_write_concurrency`（1）仍未接线 —— 单连接 + 互斥 ⇒ 实际并发恒为 1，改它无可观测效果。仍未接线：`remote.*`（4，远端元数据仓储未交付）。下一步：连接池交付后 `max_write_concurrency` 才有意义 |
| `location` | 1 | `busy_timeout_ms`/`journal_mode`（切片 3）、`synchronous`（**切片 4**）、`group_commit*` 三键（**C10.20**）与 `postgres.{dsn,max_connections}`（**B1**）已接通（§1.3.1）；只剩 `max_write_concurrency`（1）未接线 —— 单连接串行实现下它不改变行为。下一步：连接池交付后才接通 |
| `leases` | 0 | **C2 已清空**：`ttl_seconds`/`renew_interval_seconds`/`time_source` 在 C2 接通（§1.3.1）——上传路径 `Acquire`/`Renew`、GC 用过期租约驱动 claiming 回收。⚠️ 可观测效果**要求 `leases.enabled=true`**（单实例默认 `false` 时上传路径不做租约动作，行为逐字不变）；内存租约仓储**按构造是本地钟**，不兑现 `database`（它只在 `enabled=false` 下装配） |
| `leader_election` | 0 | **B1 已清空**：`enabled`/`backend`/`lock_key` 全部接通（§1.3.1）；实现是 `src/infra/postgres/pg_leader_election.*` + GC 的 leader 门控 |
| `auth` | 0 | **已清空**：`jwt.roles_claim` 与 `local_roles.*`（3 个）在切片 3 接通（§1.3.1）；`remote_entitlements.fail_closed` 在切片 6a 的**伴随更正**中移入「拒绝启动」（模式相关的 exit 78，§1.3.2）；`jwt.jwks_url` 见 §1.3.2 |
| `events` | 0 | **切片 6b 已清空**：`webhook.url`/`webhook.timeout_ms`/`webhook.topic` 全部接通（§1.3.1），`publisher` 的三态（`log`/`webhook`/`none`）也都已接通。⚠️ **下一步（有界差异）**：本实现是**内联同步** POST（不是异步消息总线），**没有**异步有界队列 / 重试退避 / 投递保证；要交付这些必须先在 ADR-013 §9.4 定「队列上界 + 丢弃策略 + 退避参数」，并同步 `config/fss.example.json`（新增键）+ `operations.md` + `test_operations_doc` |

---

### 1.4 旧环境变量别名 + 默认值分歧

**旧环境变量别名（deprecated，继续可用）** —— 通用名优先级更高；两者同时给出时**通用名胜出**，
并在启动日志里打印一条告警（避免"到底以谁为准"的歧义）：

| 旧环境变量 | 映射到的配置路径 | 备注 |
| --- | --- | --- |
| `FSS_STORAGE_ROOT` | `storage.posix.root` | |
| `FSS_SQLITE_PATH` | `location.sqlite.path` | |
| `FSS_METADATA_SQLITE_PATH` | `metadata.sqlite.path` | 与通用名**同字**（不算歧义） |
| `FSS_POSIX_DURABILITY` | `storage.posix.durability` | 值域比 schema 宽：额外接受 `never` |
| `FSS_POSIX_FSYNC_THRESHOLD_BYTES` | `storage.posix.fsync_threshold_bytes` | 通用名 `FSS_STORAGE_POSIX_FSYNC_THRESHOLD_BYTES` 优先 |
| `FSS_HTTP_PORT` | `server.http.port` | 额外接受 `0` = 系统分配 |
| `FSS_GRPC_PORT` | `server.grpc.port` | 额外接受 `0`=关闭 / `-1`=系统分配 |
| `FSS_BIND_ADDRESS` | `server.http.bind`、`server.grpc.bind` | 一个变量同时喂两个键（HTTP 与 gRPC 共用） |
| `FSS_SELF_BASE_URL` | `self_signed.public_base_url` | |
| `FSS_TRANSFER_SECRET` | `self_signed.signing_key` | **生产必须替换** |
| `FSS_TRANSFER_SIGNING_KEY` | `self_signed.signing_key` | 示例文件内嵌名；`FSS_TRANSFER_SECRET` 优先 |
| `FSS_S3_ACCESS_KEY` | `storage.s3.access_key` | 示例文件内嵌名；通用名 `FSS_STORAGE_S3_ACCESS_KEY` 优先 |
| `FSS_S3_SECRET_KEY` | `storage.s3.secret_key` | 同上 |
| `FSS_MAX_CLOCK_SKEW_SECONDS` | `deployment.max_clock_skew_seconds` | |
| `FSS_JWT_HMAC_SECRET` | `auth.jwt.hmac_secret` | |
| `FSS_JWT_ISSUER` | `auth.jwt.issuer` | |
| `FSS_JWT_AUDIENCE` | `auth.jwt.audience` | |
| `FSS_JWT_VERIFY_SIGNATURE` | `auth.jwt.verify_signature` | 沿用旧语义：只有字面 `false` 才关 |
| `FSS_JWT_USER_ID_CLAIM` | `auth.jwt.user_id_claim` | |
| `FSS_JWT_PARTITION_CLAIM` | `auth.jwt.partition_claim` | |
| `FSS_JWT_REQUIRE_PARTITION_CLAIM` | `auth.jwt.require_partition_claim` | 同上（只有字面 `false` 才关） |
| `FSS_ENTITLEMENTS_URL` | `auth.remote_entitlements.base_url` | |
| `FSS_ENTITLEMENTS_AUTHORIZE_PATH` | `auth.remote_entitlements.authorize_path` | |
| `FSS_ENTITLEMENTS_TIMEOUT_MS` | `auth.remote_entitlements.timeout_ms` | |
| `FSS_LOG_LEVEL` | `observability.log_level` | |
| `FSS_LOG_FORMAT` | `observability.log_format` | |
| `FSS_LOG_REDACT_KEYS` | `observability.redact_keys` | 逗号分隔 |

**默认值分歧（组合根历史默认 ≠ schema 默认；以组合根为准）**

| JSON 键 | schema 默认 | 组合根在"无任何来源"时的默认 | 为什么保留 |
| --- | --- | --- | --- |
| `auth.mode` | `jwt` | `disabled` | 接线前就是 `disabled`；改成 `jwt` 会让所有开发/测试脚本因"空密钥"拒绝启动（**破坏既有行为**）。生产用 `deployment.environment=production` 强制 `jwt` |
| `storage.posix.durability` | `batch` | `per_file` | 接线前的默认；改成 `batch` 会改变**未配置部署**的持久化语义（现在 `batch` 是真批提交，见 ADR-008 的 P4；但"无任何配置启动"仍保持每文件 `fdatasync` 的既有行为） |
| `storage.posix.fsync_threshold_bytes` | `0`（0 = 所有对象都 fsync） | `1048576`（1 MiB） | `bench_baseline.sh` 等脚本按 1 MiB 测过；`0` 会让 `batch` 退化成 per_file 的耐久性/速度 |
| `storage.posix.root` | `/var/lib/fss/data` | `/tmp/fss-data` | 接线前的默认（容器镜像另行注入 `/data`） |
| `metadata.sqlite.path` | `/var/lib/fss/meta.db` | `<storage_root>/metadata.db` | 接线前的默认（跟随存储根，便于本地/测试） |
| `location.sqlite.path` | `/var/lib/fss/location.db` | `<storage_root>/location.db` | 同上 |
| `self_signed.public_base_url` | `http://127.0.0.1:8080` | `http://127.0.0.1:<实际端口>/api/file` | 必须**含 base path**，否则发出去的上传地址 404（P4-D04） |
| `self_signed.signing_key` | `""` | `dev-secret-change-me` | 接线前的默认（生产必须替换） |
| `storage.s3.endpoint` | `http://127.0.0.1:9000` | `""` | 接线前为空；`driver=s3` 且为空 → 拒绝启动（不静默指向本机 9000） |
| `server.grpc.port` | `50051` | `0`（关闭） | 接线前默认关闭（ADR-001：RPC 是平台外扩展） |

**`${ENV:...}` 引用**：示例文件里的密文键写成 `${ENV:VAR_NAME}`（如
`storage.s3.access_key` 内嵌 `FSS_S3_ACCESS_KEY`）。这些引用现在**由加载器解析并生效**；
上表的旧别名与之**兼容**（示例文件写什么名就注入什么名，两个名字都能工作）。

这意味着**不提供任何配置与环境变量启动 = 无鉴权 + 每文件 fsync**（与接线前一致）；
生产必须显式设置 `deployment.environment=production` + `auth.mode=jwt` +
`auth.jwt.hmac_secret`，否则进程**拒绝启动**（exit 78）。

---

## 2. 启动 / 探活 / 关闭

### 2.1 启动

```bash
./build/bin/fss_server --config config/fss.example.json            # 推荐
./build/bin/fss_server --config /etc/fss/config.json --set server.http.port=9090
./build/bin/fss_server --print-config --config /etc/fss/config.json  # 只看生效值（不启动）
```

启动横幅**第一行**是配置来源（文件路径或"无配置文件（仅环境变量）"），随后打印实际
`bind`、`grpc bind`、`base path`、`storage driver`、`storage root`、`sqlite path`、
`metadata path`、`io engine`、`error format`、`log`、`metrics`、`audit`、`auth`、
`environment`、`gc`、`expiry`、`leases`，最后逐键列出**生效**键（以及有来源的守卫键）的来源缩写（`cli`/`env`/`file`/`default`）。
任何配置错误在**启动时**以 **退出码 78（EX_CONFIG）** 失败，并**一次性**打印全部问题
（`path + 原因 + 来源`）：

| stderr 内容 | 含义 |
| --- | --- |
| `配置校验失败，共 N 个问题：` | 加载器汇总（未知键 / 类型 / 范围 / 枚举 / 跨字段 / 生产强校验） |
| `拒绝启动（exit 78，EX_CONFIG）` | 上面的问题导致拒绝启动 |
| `拒绝启动：本配置需要 PostgreSQL（...）但本二进制在构建时**未找到 libpq**` | B1：multi / `*.repository=postgres` / `leases.enabled=true` / `leader_election.enabled=true` 需要 libpq（安装 `libpq-dev` 后重新 cmake + 重编） |
| `拒绝启动：storage.io_engine=uring ... scripts/check_io_uring.sh` | io_uring 不可用（ADR-010） |
| `拒绝启动：server.http.* 配置非法：...` | `http::ValidateOptions`（如 `max_connections > worker_threads`） |
| `拒绝启动：{metadata,location}.repository 只支持 sqlite \| postgres` | `remote`（metadata 专属）或枚举被改宽后的非法值 → exit 78 |
| `拒绝启动：{metadata,location}.repository=postgres 但打开 PG ... 仓储失败：...` / `拒绝启动：leases.enabled=true 但打开 PG 租约仓储失败` / `拒绝启动：leader_election.enabled=true 但创建 leader election 失败` | B1：PG 不可达 / DSN 为空 / 锁连接失败 → exit 78 + libpq 原文（**绝不**回退 SQLite）；见 §8 |
| `拒绝启动：server.http.large_file_plane.enabled=true` / `self_signed.single_use_nonce=true` 等（C10.11 的守卫键） | C10.11 的守卫：未实现能力的非默认值 → exit 78（见 §1.3.2） |
| `拒绝启动：server.http.large_file_plane.enabled=true ...` 等（C10.11 的 16 个守卫键） | 非默认/不支持的值一律拒绝启动，绝不静默忽略 |
| `拒绝启动：<原因>`（jwt 分支） | `auth.mode=jwt` 但 `auth.jwt.hmac_secret` 为空 |
| `拒绝启动：<原因>`（远端分支） | `auth.mode=remote-entitlements` 但未配 `auth.remote_entitlements.base_url` |
| `拒绝启动：storage.driver=s3 需要 ...` | S3 三要素缺失 |
| `打开位置仓储失败: ...` / `打开元数据仓储失败: ...` | SQLite 打不开（见 §6.2） |
| `监听失败: ...` | 端口被占用/无权限 |
| `参数错误：未知参数：...` | 命令行用法错误（退出码 2） |

### 2.2 探活端点（都在 base path 下，免鉴权）

| 端点 | 用途 | 预期 |
| --- | --- | --- |
| `GET /api/file/v2/liveness_check` | 进程存活（`@PermitAll`） | `200` **纯文本** `File service is alive`。**不碰任何依赖**，所以它 200 不代表能服务 |
| `GET /api/file/v2/readiness_check` | 依赖就绪 | 探针 = 元数据仓储 `List("__readiness__")`；成功 `200` 纯文本 `File service is ready`；失败 `503` 纯文本 `File service is not ready` |
| `GET /api/file/v2/info` | 版本信息 | `200` JSON：`version`（`"v2"`）、`buildVersion`、`connectedOuterServices`（内置 SQLite 时为 `["storage"]`）、`authMode`（`jwt` / `remote-entitlements` / `disabled`，非规范扩展字段，**必须**用它来确认鉴权是否真的开着） |
| `GET /metrics` | 指标（§3.1） | `200`，`Content-Type: text/plain; version=0.0.4; charset=utf-8` |

> `readiness_check` **只**探元数据仓储，不探存储/磁盘水位；磁盘满时它仍可能返回 200（见 §6.1）。
> 编排探针请用 readiness 做流量准入、用 liveness 做重启判定；两者不得互换。

### 2.2.1 运维动作端点（**需要**鉴权；不是探活）

| 端点 | 用途 | 预期 |
| --- | --- | --- |
| `POST /api/file/v2/gc:run` | **按需**跑一轮 GC（不重启进程） | 需 `service.file.admin`（与 `revokeURL` 同类，**不需要** `data-partition-id`）。`200` + JSON 报告（`GcReport` 的 snake_case 字段 + 运行态 `partition` / `scheduled`）；无 token `401`、非 admin `403`、已有一轮在跑 `503`（单飞护栏：不排队/不并行）。`?dryRun=true` **只能更保守**（有效值 = 配置 `gc.dry_run` ‖ 请求）；`gc.enabled=false` 时端点**仍可用**（报告 `scheduled=false`）。审计事件 `operation=gcRun`（成功/失败两侧）。详见契约 §7.3、ADR-013 §10、runbook §4.1 |

> 这是**本服务的运维扩展**：上游 OSDU **没有** GC 端点（契约 §7.3）。它**不属于** §2.2 的免鉴权探活集合，
> 编排探针**不要**打它（会真的扫一轮 GC）。

### 2.3 关闭

进程只处理 `SIGINT` / `SIGTERM` 的默认语义；退出路径会先 `Shutdown()` gRPC 服务再退出
（避免挂在 gRPC 线程池上）。建议：

```bash
kill -TERM "$(cat /run/fss.pid)"      # 优雅退出
# 等待端口释放（不要立刻重启，见 §6.1 "磁盘满时不要重启"）
for i in $(seq 1 30); do ss -ltn | grep -q ':8080 ' || break; sleep 1; done
```

**不要**用 `kill -9`：数据面正在 rename 的对象会留下 `.tmp.*`（由 GC 清理；`gc.enabled=true` 时组合根**已装配**周期调度，见 §6.5）。

---

## 3. 可观测性

### 3.1 `/metrics` 指标族（来自代码的真实名字与标签）

来源：HTTP 侧 `src/adapters/http/metrics.cpp`（`HttpMetrics` 类），存储侧
`src/infra/blob/metered/metered_blob_store.cpp`（`MeteredBlobStore`），GC 侧
`src/app/tasks/gc_task.cpp`（`GcTask::Run`），渲染入口 `src/adapters/http/router.cpp`。

**HTTP / 传输层（`HttpMetrics`）**

| 指标名 | 类型 | 标签 | 含义 |
| --- | --- | --- | --- |
| `fss_http_requests_total` | counter | `route`、`method`、`status` | 请求总数（`route` 是适配层的路由名，如元数据读取路由；未匹配为 `<unmatched>`） |
| `fss_http_request_duration_seconds` | histogram | `route`、`method` | 请求耗时；桶上界 `le` = 0.001/0.005/0.010/0.050/0.100/0.500/1.000/5.000/10.000/`+Inf`，另有 `_sum` / `_count` |
| `fss_transfer_token_rejected_total` | counter | 无 | 自签传输 token 被拒次数（签名错/过期/用途或租户不符） |
| `fss_http_in_flight_requests` | gauge | 无 | 当前在途请求数 |
| `fss_http_peak_in_flight_requests` | gauge | 无 | 在途请求峰值 |
| `fss_http_worker_threads` | gauge | 无 | 工作线程数（= 并发连接上限） |
| `fss_http_max_connections` | gauge | 无 | 允许的最大并发连接数 |
| `fss_http_rejected_total` | counter | `reason` ∈ `busy` / `too_large` / `body_limit_aborted` / `length_mismatch` / `not_found` | 被拒绝的请求（`busy` 即 503 背压） |

**存储层（`MeteredBlobStore`）**

| 指标名 | 类型 | 标签 | 含义 |
| --- | --- | --- | --- |
| `fss_storage_operations_total` | counter | `driver`、`op`、`outcome` ∈ `ok` / `error` | 存储操作次数。`op` ∈ `ensure_container`、`presign_put`、`presign_get`、`put`、`get`、`stat`、`remove`、`copy`、`list`、`remove_temp_files` |
| `fss_storage_bytes_total` | counter | `direction` ∈ `in` / `out` | 流经存储的字节数（`in` = PUT 入流量，`out` = GET 出流量） |
| `fss_io_engine` | gauge | `engine`（实际生效）、`requested`（`storage.io_engine` 的配置值） | 恒为 `1`；用来回答"`auto` 到底选了哪个引擎"（R11：探测与回退结果必须可见） |
| `fss_io_uring_available` | gauge | 无 | `1` = 宿主能力探测**通过**（本部署允许 `io_uring_setup`）；`0` = 被 seccomp/`kernel.io_uring_disabled` 阻断。★ **可用 ≠ 已启用**：`1` **不**代表服务正在用 io_uring（引擎实现未交付 ⇒ `ioEngine` 仍是 `blocking`）。回答的是"这台机器能不能用"（决定要不要改 seccomp profile）。与 `/v2/info` 的 `ioUringAvailable` 同源 |

**POSIX 批提交（ADR-008 的 P4；`PosixBlobStore` 直接写注册表）**

| 指标名 | 类型 | 标签 | 含义 |
| --- | --- | --- | --- |
| `fss_posix_syncfs_total` | counter | 无 | `syncfs(2)` 调用次数（每批恰好 1 次；**"默认 batch 真的会 syncfs"的可证事实**） |
| `fss_posix_group_commits_total` | counter | 无 | 批提交的**批次数**（每批 1 次 `syncfs` + 1 次 `fsync(dir)`） |
| `fss_posix_batch_objects_total` | counter | 无 | 通过两阶段批提交提交的**对象数** |

> **怎么看摊销是否发生**：`fss_posix_batch_objects_total / fss_posix_group_commits_total`
> = 平均批大小。顺序单文件写下平均批大小 ≈ 1（每文件一次提交，符合 ADR-008 的偏差说明）；
> **并发**写下才会接近 `storage.posix.group_commit_max_batch`。
> ⚠️ 三族只在 `storage.driver=posix` 且 `durability=batch` 下真的增长（`per_file`/`never`
> 不产生 `syncfs`，值恒为 0）。真实进程证据见 `tests/integration/test_config_wiring.cpp`
> 的 C9.23 用例（上传后 `/metrics` 的 `fss_posix_syncfs_total >= 1`）。

**SQLite 组提交（C10.20；两个仓储共用 `SqliteGroupCommitter`）**

| 指标名 | 类型 | 标签 | 含义 |
| --- | --- | --- | --- |
| `fss_sqlite_group_commits_total` | counter | `repo` ∈ `metadata` / `location` | 组提交的**批次数**（每批 1 次事务 `COMMIT`；`group_commit=false` 时每操作一批） |
| `fss_sqlite_ops_total` | counter | `repo` ∈ `metadata` / `location` | 交给协调器的**写操作数** |

> **怎么看摊销是否发生**：`fss_sqlite_ops_total{repo} / fss_sqlite_group_commits_total{repo}`
> = 平均批大小。**顺序**单文件写下平均批大小 ≈ 1（每操作一次提交，`group_commit=false` 时
> 恒等于 1）；**并发**写下才会接近 `*.sqlite.group_commit_max_batch`。
> 真实进程证据：`tests/integration/test_config_wiring.cpp` 的 C10.20 用例
> （`--set metadata.sqlite.group_commit=false` 后 `commits == ops`）。

**GC 层（`GcTask`）**

| 指标名 | 类型 | 标签 | 含义 |
| --- | --- | --- | --- |
| `fss_gc_runs_total` | counter | `mode` ∈ `dry_run` / `real`、`outcome` ∈ `ok` / `error` | GC 运行次数 |
| `fss_gc_objects_deleted_total` | counter | 无 | 删除的对象数（dry-run 下为候选数） |
| `fss_gc_tmp_removed_total` | counter | 无 | 清理的 `.tmp_*` 临时文件数 |
| `fss_gc_reclaimed_claiming_total` | counter | 无 | 回收的"崩溃领取者"`claiming` 行数（C2：租约已过期并被本 GC 原子领取才计数） |
| `fss_gc_skipped_total` | counter | `reason` ∈ `has_record` / `too_young` / `tmp_too_young` / `no_location` | 跳过的对象数（`tmp_too_young` = 在途上传被保护，可观测） |
| `fss_gc_last_run_epoch_seconds` | gauge | 无 | 最近一次 GC 运行时间（epoch 秒） |

> ✔ **真实进程已接通**（P9-D10 接指标注册表与 `MeteredBlobStore`；阶段 10 切片 2 / C10.9
> 接入 `GcTask`）：`router_options.metrics_registry` 指向组合根的注册表，因此
> **`fss_gc_*` 只会在 `GcTask::Run` 至少跑过一轮后出现**（即 `gc.enabled=true` 且调度启动，
> 或跑过 `--once`）。`gc.enabled=false`（默认）时 `/metrics` 里**没有** GC 族。
> 指标格式与取值由 [`tests/hardening/test_metrics_and_gc.cpp`](../tests/hardening/test_metrics_and_gc.cpp)
> 与 `tests/integration/test_config_wiring.cpp` 的 C10.9 用例共同钉住。

**抓取示例**

```bash
curl -sS http://127.0.0.1:8080/metrics | grep -E '^# (HELP|TYPE)'
curl -sS http://127.0.0.1:8080/metrics | grep 'fss_http_requests_total{' | sort
curl -sS http://127.0.0.1:8080/metrics | grep 'fss_http_rejected_total{reason="busy"}'
```

### 3.2 日志级别、脱敏与访问日志字段

* **格式**：默认每行一条 JSON（`json`）。级别 `debug` / `info` / `warn` / `error`。
* **访问日志**：消息名 `http_request`，字段固定为
  `method`、`path`、`status`、`duration_ms`、`correlation_id`、`remote_addr`、`route`、
  `content_length`、`note`（`correlation_id` 取自入站 `x-correlation-id`，缺省自动生成并回写该响应头）。
* **审计日志**：消息名 `audit`，字段 `operation`、`user`、`partition`、`object_id`、`result`。
* **事件日志**：消息名 `status-changed` / `datasetDetails`。
* **脱敏** `observability.redact_keys`：列表语义，**子串**匹配、大小写不敏感、忽略 `_` 与 `-`
  （`secret_key` / `secretKey` / `SECRET-KEY` 等效）；命中后结构化字段整值替换为 `***`，
  自由文本里 `key: value` / `key=value` 的值也会被打码（宁可多打码）。
  **已接通**：组合根用 `logging::OptionsFromConfig` 构造 logger，默认 11 个键与示例文件一致；
  可用 `observability.redact_keys`（或旧别名 `FSS_LOG_REDACT_KEYS`）覆盖；启动横幅打印实际键数。
  排障时仍不要把 `Authorization` / `X-Amz-Signature` 之外的自定义密钥名写进日志。
* **级别/格式已接通**：`observability.log_level` / `observability.log_format`（旧别名
  `FSS_LOG_LEVEL` / `FSS_LOG_FORMAT`）生效；日志 `service` 字段用组合根专用环境变量
  `FSS_LOG_SERVICE`（不在 schema 内，默认 `file-service`）。

**排查用的日志检索**

```bash
journalctl -u fss --since '10 min ago' | grep '"msg":"http_request"' | tail -50
journalctl -u fss --since '10 min ago' | grep '"status":5' 
journalctl -u fss --since '10 min ago' | grep '"msg":"audit"'
```

---

## 4. 容量与容量基线

* 脚本：[`scripts/bench_baseline.sh`](../scripts/bench_baseline.sh)
  （`--save` 写入基线；`--check` 跑一遍并对**回归 >20%** 直接返回非零；`FSS_BENCH_QUICK=1` 冒烟）。
* 基线数据：[`docs/appendix/capacity-baseline/BASELINE.tsv`](appendix/capacity-baseline/BASELINE.tsv)（C9.11）。
* 方法学与结论：[`docs/05-capacity-and-concurrency.md`](05-capacity-and-concurrency.md) §1.9；设计侧汇总见
  [`docs/02-design.md`](02-design.md) §13.1 / §13.4。
* 方法学铁律（R2/R3/R4）：服务端与负载生成器是**两个进程**且绑**不重叠**的核
  （默认服务端 `4-7`、客户端 `8-15`），协议 HTTP/1.1 keep-alive，数据面走**真实**自签 token 校验；
  每个点位重复 **3 次取中位数**，并报告客户端 CPU（接近客户端核数 ×100 说明该点位受客户端限制）。

```bash
scripts/bench_baseline.sh                 # 跑一遍并与基线对比
scripts/bench_baseline.sh --save          # 跑一遍并把基线写回 BASELINE.tsv
scripts/bench_baseline.sh --check         # 回归 >20% → 退出码 1（门禁用）
```

**适用范围（必须随数字一起引用）**

1. **绝对数字只作本机量级参考**：本机是 WSL2 + 虚拟盘，`1343 MiB/s` 之类的大文件数字是**页缓存/内存带宽**
   量级，不能当磁盘吞吐用；生产容量必须在目标硬件/网卡上复测（C9.14 未验证）。
2. **回归 >20% 判据的判定力有限**：跨会话实测漂移可达 ≈40%（CI/共享主机上尤其明显），
   该判据只在**同一会话、独占硬件**上有判定力；`fsync`/磁盘类点位（`data_put_*`、`upload_chain_*`、
   `sqlite_write_*`）在本机逐次散布 >20%，脚本对它们**只告警、不判失败**（R4）。
3. **延迟用绝对阈值兜底**，不能套用"退化 >20%"（吞吐类才适用相对判据）。
4. **批量删除必须与测量解耦**：实测把写入点位的 p99 从 7~44 ms 抬到 293~890 ms。
5. `sqlite_write_t1` 是**报告型**点位（本机不可复现），不参与回归判定。

---

## 5. 存储与耐久性

### 5.1 `storage.posix.durability` 三档与 `FSS_POSIX_DURABILITY` 映射

组合根（`src/main/server_main.cpp`）把配置映射到 `PosixBlobStoreOptions`：

| 取值（`storage.posix.durability` / 旧别名 `FSS_POSIX_DURABILITY`） | 映射到 | 语义 | 阈值来源 |
| --- | --- | --- | --- |
| `per_file` | `FsyncPolicy::kAlways` | 每个对象都 `fdatasync` + `fsync(dir)`：精确但慢 | 不使用 |
| `batch` | `batch_commit=true` + `FsyncPolicy::kBySize` + `storage.posix.fsync_threshold_bytes` | **真两阶段批提交**（ADR-008 的 P4）：小对象（< 阈值）写 `.tmp` → 入批 → 领队一次 `syncfs` → 统一 `rename` → `fsync(dir)`；**≥ 阈值**的对象强制单独 `fdatasync`（不靠批摊销）。⚠️ **摊销只在并发写时发生**：顺序单文件写 = 一文件一次提交（ADR 的 31,478 文件/秒是显式批量写场景，**不迁移**到本实现） | 配置键（默认 0）或旧别名（历史默认 1 MiB） |
| `never` | `FsyncPolicy::kNever` | 完全不 fsync；**只允许用于可重建数据**，进程崩溃可能丢已确认的写入 | 不使用 |

其他要点：

* 组合根默认值是 **`per_file`**（不是 schema 默认的 `batch`），且 `batch` 档的阈值默认 **1 MiB**。
* ⚠️ **阈值 `0` 的语义是"所有对象都必须 fsync"**（`ShouldFsync` 对 `threshold<=0` 一律返回 true），
  即 `batch` + 阈值 0 = per_file 的耐久性，但**没有 per_file 的速度**。示例文件已写成 1 MiB。
* `storage.posix.durability` 的 schema enum 未包含 `never`（只有旧别名能表达 `never`）。
* 取值为未知字符串 → **拒绝启动**（exit 78）并提示可选三档。
* `never` 启动时会在 stderr 打印显式告警，**不做静默降级**。

### 5.2 实测吞吐差异（同一次运行内 3 次取中位数）

> ⚠️ **该表对 `batch` 一行已作废（无效）**：数字是在 ADR-008 的 P4 **交付之前**测的，
> 当时组合根把 `durability=batch` 近似成 `FsyncPolicy::kBySize` —— 语义是"小文件**不 fsync**、
> 靠**不存在**的批提交摊销"，因此那正是"默认配置下小文件从不落盘"的耐久性谎言
> （推翻记录见 `docs/00-final-design.md` §5）。现在 `batch` 是**真批提交**，
> 顺序单文件写每文件一次提交，摊销只在并发下发生。**不得**把下表的 `batch` 值当作
> 本实现的成绩；本切片**未重测**基线（`bench_baseline.sh --save` 会覆盖基线，环境漂移已知）。

| 点位（c4，4 KiB 对象，上传链） | files/s | 相对 `per_file` |
| --- | --- | --- |
| `per_file` | **109.9** | 1.0× |
| `batch` | ~~**468.6**~~（**已作废**：P4 交付前的近似映射） | ~~4.3×~~ |
| `never` | **452.0** | 4.1× |

来源：[`docs/05-capacity-and-concurrency.md`](05-capacity-and-concurrency.md) §1.9（命令
`scripts/bench_baseline.sh`）。另一次独立记录为 `per_file` 96.4 / `batch` 410.1 / `never` 426.0，
比值同样是 **4.3×**（[`docs/02-design.md`](02-design.md) §13.4）——⚠️ 其中的 `batch` 值
**同样已作废**（同一原因）；`per_file` 与 `never` 两档不受 P4 影响，仅作本机量级参考。
⚠️ 均为 WSL2 虚拟盘，**不可作为基线**。

### 5.3 禁令：绝不允许 rename 早于数据 durable

**必须**保持的顺序（ADR-008 的两阶段批提交）：

```
① 写整批 .tmp_i   →   ② syncfs()   →   ③ 统一 rename(.tmp_i → f_i)   →   ④ fsync(dir)   →   ⑤ 确认
```

* **禁止** `write → rename → syncfs`（P3 式顺序）：`rename` 只保证命名的原子性，不保证数据已落盘，
  断电会留下"文件存在但内容为空/残缺"的**静默数据损坏**。
* 本不变量在测试中可判定（ADR-008 把 R1 建模为"某个 `rename` 之前，该 tmp 的数据是否已 durable"）。
* 为什么选批提交：ADR-008 实测 `per_file` 383 文件/s vs 批提交 31,478 文件/s（**82.3×**），
  而 P3 的不安全顺序只快 11% —— 用 11% 的性能换数据完整性事故，不划算。
  ⚠️ 这两个数字来自 ADR-008 的**显式批量写**场景（5000 文件 / 批 500），**不迁移**到本实现：
  本实现是**并发驱动**的组提交，顺序单文件写仍是一文件一次提交（见 §5.2 的作废标注）。
* `syncfs` 是**文件系统级**操作，会 flush 整个文件系统；多业务共盘会互相牵连 →
  部署上建议按 partition 分盘（配置键 `storage.posix.one_filesystem_per_partition` 与
  `storage.posix.shared_mount_required`）。⚠️ **B1 交付了 multi 运行形态，但这两个键的
  共享挂载探针仍未实现**（`shared_mount_required=true` 只参与 schema 的 multi 强制校验；
  没有任何"写探针文件看别的实例"的探测）——见 §1.3.3。

细节见 [`docs/adr/ADR-008-write-durability-protocol.md`](adr/ADR-008-write-durability-protocol.md)。

---

## 6. 运行手册（runbook）

### 6.0 通用排查入口

```bash
curl -sS http://127.0.0.1:8080/api/file/v2/info                  # authMode / buildVersion
curl -sS -o /dev/null -w 'liveness=%{http_code}\n'  http://127.0.0.1:8080/api/file/v2/liveness_check
curl -sS -o /dev/null -w 'readiness=%{http_code}\n' http://127.0.0.1:8080/api/file/v2/readiness_check
curl -sSi http://127.0.0.1:8080/api/file/v2/files/metadata \
  -H 'data-partition-id: opendes' | head -20                     # 看 X-FSS-Error-Kind
curl -sS http://127.0.0.1:8080/metrics | grep -E 'status="5|rejected_total'
```

* 错误的机器可读分类在响应头 `X-FSS-Error-Kind`（不污染错误体字段集）。
* 常见状态码 ↔ 领域错误：`401` = `kUnauthenticated`、`403` = `kPermissionDenied`（或存储 `kStorageAccessDenied`）、
  `500` = `kInternal`、`502` = `kBadGateway`（如磁盘满）、`503` = `kUnavailable`（背压/依赖不可用）。

### 6.1 磁盘满

**症状**：写路径 `502` + `X-FSS-Error-Kind: kBadGateway`；对象写入会**回滚**（不会留下半截对象）；
启动可能失败在"创建存储根失败/打开仓储失败"。

**诊断**

```bash
df -h /var/lib/fss          # 数据盘与 DB 盘是否同一文件系统
df -i /var/lib/fss          # inode 也会耗尽
du -sh /var/lib/fss/data/blobs/* 2>/dev/null | sort -h | tail
find /var/lib/fss/data/blobs -name '*.tmp.*' -printf '%TY-%Tm-%Td %TH:%TM %s %p\n' | sort | tail -20
curl -sS http://127.0.0.1:8080/metrics | grep -E 'fss_storage_operations_total.*outcome="error"'
```

**处置**：扩容或清理（优先清 `*.tmp.*`，见 §6.5 的判据）；恢复后确认 502 计数停止增长、readiness 回到 200。

**不要做**

* **不要**只删 `*.fssmeta` 侧车而保留对象（或反之）—— 会造成"对象存在但元数据丢失"。
* **不要**在磁盘满时反复重启：重启只会在打开 SQLite / 建目录处失败，还丢掉内存中的重试。
* **不要**用 `rm -rf` 清 `blobs/` 根目录来"释放空间"（那是全部用户数据）。

### 6.2 SQLite 打不开

**症状**：启动 stderr `打开位置仓储失败: ...` 或 `打开元数据仓储失败: ...`，退出码 1。

**诊断**

```bash
ls -l /var/lib/fss/location.db* /var/lib/fss/metadata.db*   # 含 -wal / -shm，检查属主与权限
ls -ld /var/lib/fss                                        # 目录必须可写（SQLite 要建 WAL/SHM）
df -h /var/lib/fss
```

镜像内**没有 `sqlite3` CLI**（实测 `command -v sqlite3` 返回 1），用 Python 标准库代替：

```bash
python3 - <<'PY'
import sqlite3, sys
for p in ("/var/lib/fss/location.db", "/var/lib/fss/metadata.db"):
    try:
        c = sqlite3.connect(f"file:{p}?mode=ro", uri=True)
        print(p, c.execute("pragma integrity_check").fetchone())
    except Exception as e:
        print(p, "ERROR", e)
PY
```

若目标机有 CLI，等价命令是 `sqlite3 /var/lib/fss/location.db 'pragma integrity_check;'`。

**不要做**

* **不要**删 `-wal` / `-shm` 来"清锁"（可能丢已提交未 checkpoint 的事务）。
* **不要**把 SQLite 放到多实例共享盘上（ADR-009 / ADR-004）。B1 起 `deployment.mode=multi` **能启动**，但它强制 `metadata.repository=postgres` + `location.repository=postgres`（schema 跨字段校验），因此 SQLite 不会出现在 multi 形态里。
* **不要**在 `integrity_check` 报错时直接覆盖数据库；先备份 `*.db` / `*.db-wal` 再处置。

### 6.3 远端 Entitlements 不可用

**症状**：所有受保护端点 `503` + `X-FSS-Error-Kind: kUnavailable`；启动日志里有
`auth.mode=remote-entitlements：依赖不可用时 fail-closed（503），绝不放行`。

**诊断**

```bash
curl -sS -o /dev/null -w 'entitlements=%{http_code} time=%{time_total}s\n' \
  "${FSS_ENTITLEMENTS_URL}${FSS_ENTITLEMENTS_AUTHORIZE_PATH}"
curl -sS -w '\nreadiness=%{http_code}\n' http://127.0.0.1:8080/api/file/v2/readiness_check
curl -sS http://127.0.0.1:8080/api/file/v2/info       # 确认 authMode=remote-entitlements
```

**处置**：恢复 Entitlements 服务/网络/DNS 后，请求应自动恢复（无缓存、无粘性失败）。
依赖不可用时**唯一**正确的行为就是拒绝（fail-closed）。

**不要做**

* **不要**把 `auth.remote_entitlements.fail_closed` 改成 `false`（schema 直接拒绝；语义上也不允许
  "依赖坏了就放行"）。
* **不要**临时切 `auth.mode=disabled` 绕过：生产环境 schema 拒绝该组合；即使开发环境，也会让
  `/v2/info` 暴露 `authMode=disabled` 且审计里出现"静默放行"。
* **不要**靠调大 `FSS_ENTITLEMENTS_TIMEOUT_MS` 来"等它好"（超时过大只会把 503 变成慢 503）。

### 6.4 就绪检查失败

**症状**：liveness `200`，readiness `503` + 纯文本 `File service is not ready`。
**原因**：readiness 的探针是元数据仓储 `List("__readiness__")`（**不是**磁盘水位探针）。

**诊断顺序**：① 用 §6.2 检查元数据库；② 看日志有没有仓储错误；③ 确认 `FSS_METADATA_SQLITE_PATH` 指向的文件系统还有空间；
④ 确认没有另一个进程以写方式独占该库。

**不要做**

* **不要**把 readiness 探针改成 liveness（依赖坏了就重启，会放大故障）。
* **不要**为了"让它绿"而把探针路径/端口改到不存在的端点。

### 6.5 GC 残留 `.tmp_*`

**背景（很重要）**：阶段 10 切片 2（C10.9）起，组合根**真的装配了 `GcTask`** 并按
`gc.interval_seconds` 周期运行（第一轮立即跑）。但 **`gc.enabled` 的 schema 与组合根默认都是
`false`**：不提供配置时**不跑 GC**（与接线前一致）。要自动清理必须显式
`gc.enabled=true`（`gc.dry_run=false` 才真删）。周期调度的停止与进程退出同一路径
（SIGINT/SIGTERM → 置位 → join），临时文件命名形如
`<对象路径>.tmp.<instance_id>.<pid>.<counter>`（实例标识来自 `deployment.instance_id`）。

**诊断**

```bash
find /var/lib/fss/data/blobs -name '*.tmp.*' -printf '%TY-%Tm-%Td %TH:%TM %s %p\n' | sort | tail -50
find /var/lib/fss/data/blobs -name '*.tmp.*' -mmin -1440 | wc -l   # 24h 内 = 可能在途
du -sh /var/lib/fss/data/blobs
```

**处置（首选：让 GC 自己清）**：加 `gc.enabled=true` + `gc.dry_run=false` 并重启，先看
`/metrics` 的 `fss_gc_tmp_removed_total` 与 `fss_gc_skipped_total{reason="tmp_too_young"}`
确认「够旧的删了、在途的保护着」；一次性清理可用
`./build/bin/fss_server --config <cfg> --once`。

**处置（人工兜底）**：只能删 `mtime` 早于 staging TTL（`gc.staging_ttl_hours`，默认 24 h = `-mmin +1440`）的
`.tmp.*`：

```bash
# 先 dry-run 列出将删除的内容
find /var/lib/fss/data/blobs -name '*.tmp.*' -mmin +1440 -print
# 确认后再删
find /var/lib/fss/data/blobs -name '*.tmp.*' -mmin +1440 -delete
```

清理判据与实现同源：`IBlobStore::remove_temp_files(container, older_than_epoch_seconds, dry_run)` ——
够旧才删；mtime 未知的一律**保护**（删除方向必须保守）。

**不要做**

* **不要**删 `mtime` 在 TTL 内的 `.tmp.*`：那是在途上传，删了就是丢数据/让上传失败。
* **不要**删 `*.fssmeta`（侧车）；它随对象一起删。
* **不要**指望 `list()` 或 S3 生命周期规则来清临时文件：`list()` **有意**看不见临时文件与侧车
  （"临时文件永远不是对象"），这是设计而不是缺陷。
* **不要**用 `find ... -delete` 而不带 `-mmin` 下限。

### 6.6 token 校验失败（401）的排查顺序

按这个顺序做，每一步都能把范围缩小一半：

1. **确认鉴权模式**：`curl -sS .../api/file/v2/info` 看 `authMode`。
   `disabled` = 只做"缺 token/缺 partition"的形式检查，任何非空 token 都过；
   `jwt` / `remote-entitlements` 才真正校验。
2. **确认请求头齐全**：受保护端点同时需要 `Authorization: Bearer <token>` 与 `data-partition-id`。
   缺任一个 → `401`，消息分别是 `Missing authorization token` / `Missing partitionID`
   （**partition 先判**）。
3. **看 `X-FSS-Error-Kind` 与 `message`**：JWT 失败用固定文案定位，逐条对应处置：
   `本实例未配置 JWT 验签密钥，拒绝所有 token（fail-closed）`（补 `FSS_JWT_HMAC_SECRET`）、
   `JWT 格式非法（期望 header.payload.signature 三段）`、`JWT 段不是合法的 base64url`、
   `不支持的 JWT 算法（只接受 HS256）`、`JWT 签名长度非法（HS256 期望 32 字节）`、
   `JWT 验签失败`（密钥不一致）、`JWT 缺少 exp（拒绝无过期时间的 token）`、`JWT 已过期`、
   `JWT 尚未生效（nbf）`、`JWT 的 iss 不匹配`、`JWT 的 aud 不匹配`、
   `JWT 缺少 data-partition-id`（租户绑定，ADR-012 §3）。
4. **核对实例配置**：`FSS_JWT_HMAC_SECRET`、`FSS_JWT_ISSUER`、`FSS_JWT_AUDIENCE`、
   `FSS_JWT_PARTITION_CLAIM`、`FSS_JWT_REQUIRE_PARTITION_CLAIM`。多副本必须**同名同值**。
5. **核对时钟**：`FSS_MAX_CLOCK_SKEW_SECONDS`（默认 5 s）决定 `exp` 早到/`nbf` 晚到能容忍多少抖动。
6. **区分 401 与 403**：token 有效但角色不够 → `403 kPermissionDenied`；不要把 403 当 401 排。
7. **数据面（自签 transfer token）**：401/403 会计入 `fss_transfer_token_rejected_total`
   （`/metrics`）。检查签名、`exp`、`op`（PUT token 不能用于 GET）、租户是否一致；
   多实例下所有实例必须共享同一签名密钥；组合根当前只读 `FSS_TRANSFER_SECRET`。

**不要做**

* **不要**为了排障把 `auth.mode` 切成 `disabled`（生产 schema 直接拒绝；开发环境也会留下
  `authMode=disabled` 的可见证据）。
* **不要**给"无 `exp` 的 token"加白名单 —— 实现明确拒绝无过期时间的 token。
* **不要**在日志里打印 `Authorization` 头（组合根当前没有脱敏规则，见 §3.2）。

---

## 7. 安全

### 7.1 `auth.mode` 三档与生产要求

| 模式 | 行为 | 适用 |
| --- | --- | --- |
| `disabled` | allow-all：只检查"有 token + 有 `data-partition-id`"，不校验签名/角色 | **仅开发/测试**。启动时打印显著告警，`/v2/info` 暴露 `authMode=disabled` |
| `jwt` | 本地 HS256 校验：`alg` 白名单（只接受 HS256）、`exp` 必需、`nbf`/`iss`/`aud` 可选校验、租户 claim 与请求头绑定；角色来自 claim ∪ 静态角色表（静态表当前未接通） | **生产必须用这一档** |
| `remote-entitlements` | 调远端 entitlements authorizeAny；依赖不可用（超时/5xx/坏 JSON/连不上/未配地址）**一律 503**，绝不降级放行 | 需要与平台 Entitlements 联调时 |

**生产硬要求**（由 schema 跨字段规则定义，见 `tests/unit/test_config.cpp` 的 C8.5 用例）：
`deployment.environment=production` 时 ① `auth.mode` 必须是 `jwt`（`disabled` 与
`remote-entitlements` 都不接受）；② `auth.jwt.verify_signature` 必须为 true；
③ `auth.jwt.hmac_secret` 必须非空。
★ 这三条校验**现在就在组合根启动路径上**（`Load` 之后 `config.ok()==false` →
打印全部问题 → **exit 78**），因此"忘了开鉴权就上生产"不再可能。

### 7.2 密钥注入

* **只走环境变量或 `${ENV:...}` 引用，不入镜像**：`auth.jwt.hmac_secret`、
  `self_signed.signing_key`、`storage.s3.access_key` / `secret_key`。
  容器平台用 Secret → 环境变量（或 `*_FILE` 包装）注入，**不要**把明文写进镜像层或 JSON 样例。
  配置文件里推荐写 `${ENV:FSS_JWT_HMAC_SECRET}` 这类引用（加载器会展开）。
* `auth.mode=jwt` 且 `auth.jwt.hmac_secret` 为空 → **拒绝启动**（不是"起来但拒绝所有 token"）。
* `self_signed.signing_key` 的组合根默认是 `dev-secret-change-me` —— **生产必须替换**。
* 多副本必须共享同一签名密钥与 JWT 密钥，否则 A 签发的 URL/token 到 B 会被拒。
* `/v2/info` 不输出任何密钥；`--print-config` 与日志都按 schema 的 secret 标记打码
  （`***`），日志脱敏键来自 `observability.redact_keys`（已接通，见 §3.2）。

### 7.3 `deployment.mode=multi`：现在**真的启动**（B1 起）

`deployment.mode=multi`（或 `FSS_DEPLOYMENT_MODE=multi`）**不再被一刀切拒绝**。要跑起来，
schema 的 7 条跨字段校验必须全部满足（缺一条 → exit 78 + 逐条原因）：

```text
metadata.repository=postgres      location.repository=postgres
leases.enabled=true               leader_election.enabled=true
storage.posix.shared_mount_required=true
gc.require_lease_expiry=true      0 < deployment.max_clock_skew_seconds <= 60
```

组合根随后**真的创建** PG 元数据仓储、PG 位置仓储、PG 租约（`staging_leases`）与
`PgLeaderElection`（专用锁连接 + `pg_try_advisory_lock`），并让 GC 的周期调度与
`POST /v2/gc:run` 都以 leader 门控（非 leader 的 `fss_gc_runs_total` **不增加**）。
任一创建/取锁失败 → **exit 78** + libpq 原文，**绝不**回退 SQLite。

⚠️ **本构建未找到 libpq 时**：multi（以及任何 `*.repository=postgres` /
`leases.enabled=true` / `leader_election.enabled=true`）→ **exit 78**，并打印
「安装 libpq 开发文件后重新 cmake + 重编」的可执行指令（**不需要** `-DFSS_WITH_PG=ON`，
后者只用于注册 PG 测试）。

⚠️ **仍未交付（不要把"能启动"读成"多实例已完整验证"）**：共享挂载探针
（`storage.posix.shared_mount_required` 只参与强制校验）、readiness 的 PG `SELECT 1` +
迁移版本校验（`metadata.postgres.schema_version_check`）、`instance_registry` /
配置版本一致性、PG 连接预算（C9.28）、PG-vs-本地时钟比较
（`deployment.clock_skew_tolerance_seconds`）、上传路径的租约 `Acquire`/`Renew`、
`CreateFileMetadata` 跨步骤原子领取 + `claiming→ready`、**完整多实例 E2E 与崩溃注入
（C9.26）**、NFS 语义（C9.27）、`/v2/info` 暴露 `instanceId`。见 §8 与
[`docs/adr/ADR-009-multi-instance-consistency.md`](adr/ADR-009-multi-instance-consistency.md)、
`docs/test-evidence/phase10.md` §17。

---

## 8. 未实现 / 未验证清单（如实登记）

| 项 | 状态 | 说明 / 证据 |
| --- | --- | --- |
| 组合根接 `config/fss.example.json` | **切片 1/2/3/4/5/6a/6b 已接通（含 C10.16/C10.17/C10.18/C10.19）+ ADR-008 的 P4（C9.23）+ B2a** | `--config`/`FSS_CONFIG` + `--set` + 环境变量；157 键的三态计数为 **生效 128 / 拒绝启动 15 / 已读但无效果 14**（B2a 后；逐键见 §1.3；样例配置在 C10.10 用例里真的启动成功）。**切片 6a（C10.18）**：`legal.{validator,remote.base_url,remote.timeout_ms}` 与 `schema.{validator,remote.base_url,remote.timeout_ms}` → **生效**（+6；ADR-013 远端校验器，fail-closed → 503）；**伴随更正**：`auth.remote_entitlements.fail_closed` → **拒绝启动**（`remote-entitlements` 模式下非 true → exit 78，模式相关）。切片 3 新接通 9 键：`observability.audit_fail_closed`、`metadata.sqlite.busy_timeout_ms`、`location.sqlite.{busy_timeout_ms,journal_mode}`、`auth.jwt.roles_claim`、`auth.local_roles.*`（3）、`server.grpc.enabled`；C10.16 新增：`partition.file.opendes.max_file_bytes` → 生效，`partition.file.opendes.{allowed,default}_checksum_algorithm` → 拒绝启动。**C10.16 续**：`storage.posix.{atomic_write,dir_mode,file_mode,fadvise_random,fadvise_dontneed_after_large_read}` 与 `partition.file.opendes.{staging,persistent}_container` → 生效（+7）；`storage.posix.{group_commit_max_batch,sync_dir_after_batch}`（ADR-008 的 P4 未实现）与 `partition.file.opendes.storage_driver`（驱动冲突）→ 拒绝启动（+3）。**切片 4**：`server.http.transfer_max_body_bytes`（数据面 PUT 上限 = 全局键与 `partition.file.<p>.max_file_bytes` 取较小者）、`metadata.sqlite.{journal_mode,synchronous}`、`location.sqlite.synchronous` → 生效（+4）。**切片 5（本轮）**：`self_signed.{key_id,default_ttl_seconds,max_ttl_seconds}` → 生效（+3）：`key_id` 进被签名载荷并在解码侧 fail-closed（换 id → 旧 URL 401），两个 TTL 键是**自签分支的上界**（`expiry.*` 语义不变）。**切片 6b（C10.19）**：`events.publisher` 与 `events.webhook.{url,timeout_ms,topic}` → **生效**（+4；ADR-013 §9 的 webhook 发布器，**发布失败非致命** —— 连不上/超时/非 2xx 只告警，请求照常 201；`none` 显式关闭）。**ADR-008 的 P4（C9.23）**：`storage.posix.group_commit_max_batch` → **生效**（+1）；`storage.posix.durability=batch` 从 `FsyncPolicy::kBySize` 近似改为**真两阶段批提交**（此前小文件从不落盘 = 耐久性谎言）；`sync_dir_after_batch=false` 仍拒绝启动（R2 不变量）。**C10.20（本轮）**：`*.sqlite.{group_commit,group_commit_max_wait_ms,group_commit_max_batch}` **6 个键** → **生效**（+6；L2 共用协调器 `src/infra/sqlite/sqlite_group_commit.h`：并发写一批一次 COMMIT + 每操作 SAVEPOINT；**读可能多等 ≤ max_wait_ms**）。三态 **128/15/14**（B2a 后）|
| `observability.audit_fail_closed` | **已接通（C10.13）** | `UseCasePorts.audit_fail_closed`：`true` 时审计写入失败让请求以 **500** 结束；`false`（默认）保持非致命。真实进程用例：`FSS_AUDIT_FAULT_INJECT=1` 注入"必然失败"的审计后端（**故障注入开关，不是配置键**）→ `fail_closed=true` 时 `uploadURL` = 500、`false` 时 = 200（正例对照，R16）。★ 该注入开关只用于测试/演练：生产环境**不要**设置 `FSS_AUDIT_FAULT_INJECT` |
| `storage.io_engine=uring` | **不可用（引擎未启用）** | 组合根会真实探测（`sys::ProbeIoUring`）并按 ADR-010 拒绝/回退；但 `UringIoEngine::enabled()=false`（U1~U4 未满足）→ 显式要求 `uring` 一律 **exit 78**，`auto` 回退 blocking（横幅 + `fss_io_engine` 可见） |
| `metadata.repository`/`location.repository` 的 `postgres` | **已实现（B1）** | 组合根创建 `PostgresMetadataRepository` / `PostgresLocationRepository`，DSN 不可达 → **exit 78**（+ libpq 原文），绝不静默降级为 SQLite。`metadata.repository=remote` 仍**未实现**（显式拒绝） |
| PG 租约 / leader election | **已实现（B1 + B2a）** | `leases.enabled=true` → `PostgresLeaseRepository`（`staging_leases`）；`leader_election.enabled=true` → `PgLeaderElection`（专用锁连接 + GC 门控）。数据库时钟 `now()` 用于租约判定；**PG↔本地时钟偏移比对已实现（B2a）**：启动期 `|本地钟 − now()| > deployment.clock_skew_tolerance_seconds` → **exit 78**（横幅 `clock skew :` 可见） |
| 多实例运行形态（启动 + PG 落库 + 两进程一主） | **已实测（B1 + C9.26 + B2a）** | 真实进程 + 直连 libpq：multi/single 落 PG、`pg_locks` 持锁与释放、共键两进程恰好一个 leader（非 leader 的 `fss_gc_runs_total` 不增加）、空 instance_id 自动生成；崩溃注入（C9.26 的 kill -9 + 租约回收）与 **PG 连接预算（C9.28，B2a）**、**时钟偏移（B2a）**、**readiness 的 PG 探活 + 迁移版本校验（B2a）** 均已交付。**未有**：共享挂载探针（B2b）、`instance_registry` 心跳 / config-version 一致性（B2b）、NFS 语义（C9.27） |
| `metadata.sqlite.{journal_mode,synchronous}` / `location.sqlite.synchronous` | **已接通（切片 4）** | 两个 `Sqlite*RepositoryOptions` 新增 `wal` / `synchronous_level` 真实字段并真的执行 PRAGMA；`synchronous` **不落盘**，用同连接访问器 `AppliedPragma("synchronous")` 验证（另开 sqlite3 连接读回无效）。真实进程侧：`metadata.sqlite.journal_mode=DELETE` 用 `python3 sqlite3` 读回 `delete`，横幅打印 `synchronous=<实际取值>`；TRUNCATE / 非枚举值 → exit 78（§1.3.1） |
| `metadata.sqlite.max_write_concurrency` / `location.sqlite.max_write_concurrency` | **未接通（如实登记）** | 两个仓储都是「单连接 + 互斥」⇒ 实际写并发恒为 1 ≤ 上限，改它不改变行为。下一步：连接池交付后才接通（`group_commit*` 三键已由 **C10.20** 接通，见 §1.3.1） |
| `storage.posix.{atomic_write,dir_mode,file_mode,fadvise_random,fadvise_dontneed_after_large_read,group_commit_max_batch}` 与 `partition.file.opendes.{staging_container,persistent_container,storage_driver}` | **已定案（C10.16 续 + ADR-008 的 P4）** | `PosixBlobStoreOptions` 已加 6 个真实字段（atomic_write/dir_mode/file_mode/fadvise_*/batch_commit+group_commit_max_batch）并接通；`PartitionConfig` 已加容器名/分区驱动字段：容器名接通、分区驱动冲突 → 拒绝启动；`sync_dir_after_batch=false` → 拒绝启动（R2 不变量，§1.3.2） |
| 远端 Storage Service 仓储（`metadata.repository=remote`） | **未实现** | ADR-004 列为可选 |
| RS256 / JWKS（`auth.jwt.jwks_url`） | **未实现** | 配置非空 → schema 拒绝启动（ADR-012 §5.3）；只支持 HS256 |
| `auth.jwt.roles_claim` / `auth.local_roles` | **已接通（C10.15）** | 接到 `LocalJwtOptions.roles_claim` / `local_roles`；真实 JWT 用例证明"配置里的 claim 名与用户→角色表"决定 200/403 |
| io_uring（`storage.io_engine=uring`） | **未接通 / 环境相关** | 引擎实现未交付（ADR-010 U1~U4）⇒ 显式 `uring` 一律 exit 78，与本机探测结果无关。探测结果本身**已可见**：`/v2/info` 的 `ioUringAvailable`、`/metrics` 的 `fss_io_uring_available`、启动横幅（C9.30）。环境差异实测：本工作机（WSL2，`kernel.io_uring_disabled=0`）宿主**可用**；**Docker 默认 seccomp** 下 `EPERM`（不可用）。先跑 [`scripts/check_io_uring.sh`](../scripts/check_io_uring.sh)（退出码 2 = 无结论） |
| 真实 S3 / MinIO 端到端 | **未验证** | 仅 AWS 官方向量 + 独立验签的 mock（ADR-005）；分片上传 >5 GiB、退避重试、STS 刷新未测 |
| **断电 / 崩溃后的耐久性（P4）** | **未验证** | 本环境**无 root、不能 `mount`、无电源故障注入**，无法验证 ADR-008 的 R1/R2 在真实断电下成立。已验证的是**顺序不变量**（`syncfs` 在所有 rename 之前、`fsync(dir)` 在所有 rename 之后，可注入失败，见 `tests/integration/test_posix_batch_commit.cpp`）、摊销、数据正确性、失败路径。ADR-008 §4.3 的 `SIGKILL` 可见性证据属于**协议研究**，不是对本实现的验证 |
| **`syncfs` 全局 flush 对他人的影响（C9.24）** | **未验证** | `syncfs` 是文件系统级操作：多租户共盘会牵连他人写入。本环境没有多租户共盘场景，无法测量；缓解方向（按 partition 分盘）依赖 `storage.posix.one_filesystem_per_partition`，该键**未接通**（§1.3.3） |
| P4 本实现的吞吐 | **未验证（数字不迁移）** | ADR-008 的 31,478 文件/秒 / 82.3× 是**显式批量写** 5000 文件 / 批 500 场景；本实现是**并发驱动**的组提交，顺序单文件写仍一文件一次提交。§5.2 的 `batch` 行已标**作废**，本切片未重测基线 |
| 真实 NFS / 多客户端共享挂载语义 | **未验证** | ADR-009 登记为**上生产硬前提**（C9.27 未验证）；实现不依赖 NFS 文件锁 |
| 容器镜像 | **已验证（Docker 实测）** | [`docs/test-evidence/phase9-image.md`](test-evidence/phase9-image.md)（C9.8 基础断言：构建 / 启动 / `readiness_check` 200 / 强制 jwt（无 token、伪 token 均 401）/ 非 root uid 10001 / HEALTHCHECK `healthy` / `auth.mode=disabled` 与空密钥 exit 64）**+ 同文件 §10 容器硬化实测**（只读 rootfs、最小权限、资源上限下的 1 GiB 流式、HEALTHCHECK + `--restart=on-failure` + SIGTERM 优雅退出、健康检查负控、反向对照；命令 = `scripts/verify_image.sh` 的 `[H*]` 段） |
| **容器硬化运行形态（`--read-only` / `--cap-drop=ALL` / 资源上限 / 重启 + SIGTERM）** | **已在 Docker 上实测** | 证据 §10。要点：`--read-only`（**不带** `--tmpfs /tmp`）即可 readiness 200 + 上传读回，根文件系统 `touch` 被 `Read-only file system` 拒绝 ⇒ **本服务不需要可写 rootfs，也不需要可写 `/tmp`**；`--read-only --tmpfs /tmp:rw,size=64m,mode=1777` 同样通过；`--cap-drop=ALL --security-opt no-new-privileges`（**Docker 默认 seccomp**，未用 `unconfined`）下 `CapBnd=0`/`NoNewPrivs=1` 且通过；`--memory=128m --pids-limit=256` 下 **1 GiB** 上传+读回 SHA-256 一致（5.1 s）、**进程峰值 RSS 20.9 MiB**（cgroup `memory.peak` 128.1 MiB 是写 1 GiB 的页缓存记账后被回收，非进程占用）；`--restart=on-failure` + HEALTHCHECK `healthy` + `docker stop -t 10` → `ExitCode=0`、**0.30 s** < grace、`RestartCount=0`、无 terminate/死锁痕迹（GC 周期调度线程在跑） |
| **容器 `--pids-limit` 过小（< 66）** | **已实测的部署陷阱（已修，C9.32）** | 现象（修复前）：`--memory=128m --pids-limit=64` **启动即终止**：`terminate called after throwing an instance of 'std::system_error'` / `what(): Resource temporarily unavailable`、`ExitCode=139`、`OOMKilled=false`。根因：默认 `server.http.worker_threads = max(16, 4×nproc)` ⇒ 本机 16 核 = **64 个 HTTP 工作线程 + 主/监听线程 = 66 个任务**，pids cgroup 上限先于内存耗尽。**修复后（C9.32）**：`main()` 顶层 `catch (std::exception)` + `catch (...)` → **ExitCode=70（EX_SOFTWARE）+ 可读 `未预期异常（exit 70）：…`**，**不再出现 `terminate`/139**（容器回归断言见 `scripts/verify_image.sh` 的 H4a/H4a2）。实测下界**未变**：`--pids-limit=65` 失败、**66 成功**。建议 `--pids-limit ≥ 128`（或显式调小 `server.http.worker_threads`） |
| **`/v2/info` 与指标暴露 `ioEngine`/`ioUringAvailable`（C9.30）** | **已满足（三半全部交付；P10 期间补交第三半）** | ① 端点行为不变：Docker 默认 seccomp（io_uring 被阻断）下 readiness 200 + 上传读回 SHA-256 一致（`phase9-image.md` §10 H8，另本轮以 `python3` 直接 `syscall(425)` 双向复验：宿主 `rc>=0` / 默认 seccomp 容器 `EPERM`）；② 指标：真实进程 `/metrics` 同时有 `fss_io_engine{engine="blocking",requested="blocking"} 1` **与** `fss_io_uring_available 0\|1`；③ `/v2/info`：`"ioEngine":"blocking"` + `"ioUringAvailable":<bool>`（camelCase 扩展字段），gRPC `InfoResponse` 字段号 10/11 且 `json_name` 对齐，两条协议**同源**（`app::GetInfo`）。★ **可用 ≠ 已启用**：`ioUringAvailable=true`/指标 `1` 只说明宿主允许 io_uring，引擎未启用 ⇒ `ioEngine` 恒 `blocking`。判据 `tests/integration/test_io_engine_exposure.cpp`（7 用例 / 147 断言）；探测注入接缝 `FSS_IO_PROBE_INJECT=available\|blocked`（**不是配置键**，见 runbook §10）使之双向可证伪 |
| **K8s（`readOnlyRootFilesystem` / restricted PodSecurity / `runAsNonRoot` / `fsGroup`）** | **未验证** | 无集群。Docker 的 `--read-only` / `--cap-drop=ALL` / 默认 seccomp 与 K8s 的对应项**等价但非同一实现**；K8s restricted 的 seccomp 是 `RuntimeDefault`（≈ Docker 默认 profile），但**不等价于**自建/更严的 profile。需：真实集群 + 目标运行时 |
| **镜像 CVE 扫描 / 多架构（arm64）** | **未验证** | 本机无 trivy/grype/syft/clair，且 **Docker registry 不可达**（拉不到扫描器镜像）→ 需要"能拉 registry 的环境 + 扫描器"；多架构需要 qemu/binfmt 或原生 arm64 构建器（本机无，未做 `--platform`） |
| **entrypoint 不转发命令行参数** | **已实测的行为（未修，登记）** | `Dockerfile` 的 `entrypoint.sh` 以 `exec /usr/local/bin/fss_server`（**不带 `"$@"`**）收尾 ⇒ `docker run <image> --set k=v` 会被**静默忽略**。本轮要打开 GC 周期调度时只能改用 `-e FSS_CONFIG=/etc/fss/gc.json`（env 通路正常）。**不建议**简单加 `"$@"`：那会让 `--set auth.mode=disabled` 绕过 entrypoint 的 jwt 强制校验；若要支持，须同时约束用户参数与 `--set auth.mode=jwt`/`--config` 的先后与白名单 |
| sendfile 大文件数据面 | **方向已定稿、实现未交付** | ADR-006 受控复核 2.12x ≥ 1.5x；默认仍走 httplib 内容提供者 |
| 组合根装配指标注册表 / `MeteredBlobStore` | **已接通（P9-D10）** | 真实进程 `/metrics` 含 HTTP 族 + 存储计量族 + `fss_io_engine` / `fss_io_uring_available` 两个 I/O 引擎仪表 |
| 组合根装配 `GcTask`（**周期调度**） | **已接通（C10.9）** | `gc.enabled=true`（默认 false）时启动后台调度，第一轮立即跑、之后按 `gc.interval_seconds`；`fss_gc_*` 在**真实进程** `/metrics` 可见；`--once` 支持跑一轮即退出。`gc.dry_run`/`require_lease_expiry`/`staging_ttl_hours`/`orphan_grace_hours` 从配置读入 `GcOptions` |
| GC 的 **HTTP 端点**（手动触发/查询） | **已实现（C9.31，P10 期间补交）** | `POST {base_path}/v2/gc:run`：授权 `service.file.admin`、**不需要** `data-partition-id`；`?dryRun=true` 只能更保守；`GcTask::Run` 单飞护栏（已在跑 → **503**，不排队不并行）；`gc.enabled=false` 时端点仍可用且报告 `scheduled=false`。实现：`src/adapters/http/router.cpp`、`src/adapters/http/dto/dto.h`；证据：[`docs/test-evidence/phase9.md`](test-evidence/phase9.md) §12 |
| PG 版 `ILeaseRepository` | **已实现（B1）** | `src/infra/location/postgres/postgres_lease_repository.*`（`staging_leases`）；`leases.enabled=true` 时装配。⚠️ 上传路径仍**没有调用方** `Acquire`/`Renew`（后续切片）⇒ `leases.{ttl_seconds,renew_interval_seconds}` 暂无可观测效果（§1.3.3） |
| 磁盘水位 / 存储可达性 readiness 细化 | **未实现** | readiness 只探元数据仓储 |
| `X-FSS-Dependencies` 响应头 | **未实现** | 契约 §7 列为扩展，但当前代码没有该头（登记为未实现） |
| 生产容量（真实硬件/网卡） | **未验证** | C9.14；本机为 WSL2 虚拟盘 |

---

## 9. 相关文档与自动检查

| 我想知道 | 看这里 |
| --- | --- |
| 端点契约与错误码映射 | [`docs/03-api-contract.md`](03-api-contract.md) §2.12 / §5 / §7 |
| 分层、并发模型、资源上限 | [`docs/02-design.md`](02-design.md) §13 / §14 |
| 实测容量与并发 | [`docs/05-capacity-and-concurrency.md`](05-capacity-and-concurrency.md) |
| 写入耐久性协议 | [`docs/adr/ADR-008-write-durability-protocol.md`](adr/ADR-008-write-durability-protocol.md) |
| 多实例一致性 | [`docs/adr/ADR-009-multi-instance-consistency.md`](adr/ADR-009-multi-instance-consistency.md) |
| 认证与租户绑定 | [`docs/adr/ADR-012-auth-and-tenant-binding.md`](adr/ADR-012-auth-and-tenant-binding.md) |
| I/O 引擎选择 | [`docs/adr/ADR-010-io-engine-choice.md`](adr/ADR-010-io-engine-choice.md) |
| 大文件数据面 | [`docs/adr/ADR-006-large-file-data-plane.md`](adr/ADR-006-large-file-data-plane.md) |
| P9 测试证据 | [`docs/test-evidence/phase9.md`](test-evidence/phase9.md)、[`docs/test-evidence/phase9-adr006.md`](test-evidence/phase9-adr006.md) |
| 配置面接线（阶段 10 切片 1/2）的证据 | `ctest -L phase10`（`tests/integration/test_config_wiring.cpp`：C10.1~C10.12）+ [`scripts/verify_config_wiring.sh`](../scripts/verify_config_wiring.sh) |
| 开发环境与构建 | [`docs/development.md`](development.md) |

机械检查：

```bash
cmake --build build --target test_operations_doc -j"$(nproc)" && ./build/bin/test_operations_doc
./scripts/check_docs.sh              # D1 链接 / D2 作废数字 / D3 ADR 索引 / D4 阶段表 / D5 门槛编号
./scripts/verify_config_wiring.sh    # 真实二进制：配置生效 + 非法配置拒绝启动（exit 78）
```
