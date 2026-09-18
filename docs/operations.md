# 运维手册（operations）

> 适用范围：`fssvrcpp`（C++20 / OSDU File Service 兼容后端）的**单实例**部署形态。
> 本文的**配置项参考表**与 `config/fss.example.json` 的**每一个叶子键**一一对应，并由
> [`tests/unit/test_operations_doc.cpp`](../tests/unit/test_operations_doc.cpp) 自动比对
> （`ctest -L phase9` 会跑）：缺一个键、或写了一个不存在的键，测试都会失败并一次性列出全部差异。
>
> ★ **先读 §1.1**：组合根（[`src/main/server_main.cpp`](../src/main/server_main.cpp)）目前
> **只读环境变量**，**不读** `config/fss.example.json`。因此本手册对每个键都明确标注
> "组合根当前读取方式"：能接通的给出真实 `FSS_*` 环境变量名，接不通的写
> **未接通（登记为未实现）**，并在 §1.3 单独成表。这是一条**如实登记**，不是设计意图。

---

## 0. 一分钟速查

```bash
# 启动（组合根只认环境变量；示例里的 JSON 目前不会被读取）
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
| REST base path | `/api/file`（契约 §1.1；组合根硬编码 `kDefaultBasePath`，**不可配**） |
| 默认 HTTP 端口 | `8080`（`FSS_HTTP_PORT`） |
| 默认 gRPC 端口 | **关闭**（`FSS_GRPC_PORT=0`；`-1` = 系统分配） |
| 指标端点 | `/metrics`（**不在** base path 之下，契约 §7） |
| 数据面 | `/api/file/v1/transfer/{token}`（仅 POSIX 驱动注册） |
| 默认存储驱动 | `posix`（`FSS_STORAGE_DRIVER`） |
| 默认鉴权 | 组合根默认 `disabled`（`FSS_AUTH_MODE`）；**示例文件与 schema 默认是 `jwt`** —— 见 §7 |

---

## 1. 配置项参考

### 1.1 组合根当前如何读配置（★ 必读）

* **来源**：只有环境变量。`main()` 里没有命令行解析，也没有加载 `config/fss.example.json`
  （文件头的"命令行 > 环境变量 > 文件 > 内置默认值"目前**只有第二级可用**）。
* **每个键的命名规律**（`config/fss.example.json` 头部注释里的约定）：
  JSON 路径大写、点换下划线、加前缀 `FSS_`。例如 `storage.driver` → `FSS_STORAGE_DRIVER`。
  ⚠️ 这个规律**没有被组合根普遍执行**：下面表格的"组合根读取方式"列给的是
  `server_main.cpp` 里**实际出现**的字符串；凡是实际名字与该规律不同的，都单独标注。
* **`${ENV:VAR}` 引用**：示例文件里 7 个密文键写成了 `${ENV:VAR_NAME}`。它属于
  `fss::config` 加载器的能力（[`tests/unit/test_config.cpp`](../tests/unit/test_config.cpp) 有用例），
  但组合根不经过加载器，所以这 7 个引用**当前不生效**；生效的是组合根自己读的环境变量。
* 示例文件里的内嵌 `${ENV:...}` 名与组合根实际读的名字**不一致**的有 3 处，见 §1.4。

### 1.2 逐键参考表（156 个叶子键）

> 读法：**示例值**取自 `config/fss.example.json`（它是"给人抄的样例"，不等于 schema 默认值）；
> **取值约束**取自 `fss::config::CoreSchema()`（[`src/common/config/core_schema.cpp`](../src/common/config/core_schema.cpp)）；
> 两者不一致处已显式标注。

#### 1.2.1 `deployment`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 组合根当前读取方式 |
| --- | --- | --- | --- | --- |
| `deployment.environment` | 部署环境；`production` 触发更严格校验（禁 `disabled`、必须验签、jwt 必须有密钥） | `development` | enum `development` / `production`；默认 `development` | 未接通（登记为未实现；生产校验在 CoreSchema 跨字段规则里，组合根未加载 schema） |
| `deployment.max_clock_skew_seconds` | 本地钟与参考钟允许偏差（秒） | `5` | int 0..300；`multi` 时要求 `0 < 值 <= 60` | `FSS_MAX_CLOCK_SKEW_SECONDS`（**只在 `FSS_AUTH_MODE=jwt` 时读取**；缺省 5，赋给 `LocalJwtOptions.clock_skew_seconds`） |
| `deployment.mode` | 单实例 / 多实例 | `single` | enum `single` / `multi`；`multi` 触发 5 条强制校验 | `FSS_DEPLOYMENT_MODE`（默认 `single`；`multi` → **明确拒绝启动**，见 §8） |
| `deployment.instance_id` | 实例标识（K8s 建议注入 `POD_NAME`） | `""` | string | 未接通（登记为未实现；POSIX 临时名里的实例标识固定为 `local`） |
| `deployment.clock_skew_tolerance_seconds` | 与数据库 `now()` 的偏移容忍（秒） | `60` | int 0..86400 | 未接通（登记为未实现） |

#### 1.2.2 server.http 子树

| JSON 路径 | 含义 | 示例值 | 取值约束 | 组合根当前读取方式 |
| --- | --- | --- | --- | --- |
| `server.http.base_path` | REST context path，必须与上游一致 | `/api/file` | string，Required；默认 `/api/file` | 未接通（组合根硬编码 `/api/file`） |
| `server.http.bind` | 监听地址 | `0.0.0.0` | string；默认 `0.0.0.0` | `FSS_BIND_ADDRESS`（默认 `0.0.0.0`；**HTTP 与 gRPC 共用**） |
| `server.http.port` | HTTP 端口 | `8080` | int 1..65535；默认 `8080` | `FSS_HTTP_PORT`（默认 `8080`） |
| `server.http.max_header_bytes` | 单请求头上限 | `16384` | int 1024..1048576；默认 `16384` | 未接通（登记为未实现；实现内有固定上限） |
| `server.http.max_uri_bytes` | 请求行/target 上限 | `8192` | int 128..1048576；默认 `8192` | 未接通（登记为未实现） |
| `server.http.max_body_bytes` | JSON 端点请求体上限 | `10485760` | int 1..1073741824；默认 `10485760`（10 MiB） | 未接通（登记为未实现；适配层用固定 10 MiB / 256 KiB 两档） |
| `server.http.tcp_nodelay` | 小请求必须开启，否则 ~40 ms delayed-ACK 停顿（实测 950 倍） | `true` | bool；默认 `true` | 未接通（实现内**强制** `true`，不可关） |
| `server.http.worker_threads` | 并发连接上限（一条 keep-alive 连接占一个线程直到结束） | `0` | int 0..65536；0 = 按公式推导 | 未接通（实现内固定公式：`max(16, 4×核数)`；本机 16 核 → **64**）。⚠️ 示例文件注释写的是 `max(64, 2×核数)`，与代码不一致，以代码为准 |
| `server.http.max_connections` | 在途请求上限（超过 → 503 + `Retry-After`） | `0` | int 0..65536；0 = 等于 worker_threads | 未接通（登记为未实现；实现内 0 → = worker_threads） |
| `server.http.max_connections_per_partition` | 每租户并发上限 | `0` | int 0..65536 | 未接通（登记为未实现） |
| `server.http.idle_timeout_seconds` | 普通路由空闲读超时 | `60` | int 1..86400；默认 `60` | 未接通（实现内固定 60 s） |
| `server.http.json_request_timeout_seconds` | 普通路由**整体**超时 | `15` | int 1..86400；默认 `15` | 未接通（实现内固定 15 s） |
| `server.http.transfer_idle_timeout_seconds` | 数据面**空闲**超时（**没有**整体超时） | `120` | int 1..86400；默认 `120` | 未接通（实现内固定 120 s） |
| `server.http.transfer_max_body_bytes` | 数据面请求体上限 | `0` | int 只能是 `0`（0 = 不限） | 未接通（登记为未实现） |
| `server.http.transfer_buffer_bytes` | 数据面单块缓冲 | `262144` | int 4096..67108864；默认 `262144` | 未接通（登记为未实现） |
| `server.http.transfer_memory_budget_bytes` | 传输内存预算；`并发 × 缓冲 > 该值` → 拒绝启动 | `268435456` | int ≥1048576；跨字段：`worker_threads > 0` 时 `buffer × workers <= budget` | 未接通（登记为未实现；schema 的跨字段规则不在启动路径上） |
| `server.http.large_file_plane.enabled` | ADR-006 独立大文件数据面（`sendfile`）开关 | `false` | bool；默认 `false` | 未接通（登记为未实现；实现未交付，默认走 httplib） |
| `server.http.large_file_plane.bind` | 大文件数据面监听地址 | `0.0.0.0` | string；默认 `0.0.0.0` | 未接通（登记为未实现） |
| `server.http.large_file_plane.port` | 大文件数据面端口 | `8081` | int 1..65535；默认 `8081` | 未接通（登记为未实现） |
| `server.http.large_file_plane.use_sendfile` | 是否用 `sendfile` | `true` | bool；默认 `true` | 未接通（登记为未实现） |
| `server.http.large_file_plane.sendfile_chunk_bytes` | `sendfile` 单次长度 | `1073741824` | int ≥65536；默认 `1073741824` | 未接通（登记为未实现） |
| `server.http.large_file_plane.workers` | 大文件数据面工作线程数 | `0` | int 0..65536；默认 `0` | 未接通（登记为未实现） |
| `server.http.large_file_plane.max_connections` | 大文件数据面连接上限 | `0` | int 0..65536；默认 `0` | 未接通（登记为未实现） |

#### 1.2.3 server.grpc 子树

| JSON 路径 | 含义 | 示例值 | 取值约束 | 组合根当前读取方式 |
| --- | --- | --- | --- | --- |
| `server.grpc.enabled` | gRPC 面开关（平台外扩展，ADR-001） | `true` | bool；默认 `true` | 未接通（组合根用 `FSS_GRPC_PORT=0` 表达"关闭"） |
| `server.grpc.bind` | gRPC 监听地址 | `0.0.0.0` | string；默认 `0.0.0.0` | `FSS_BIND_ADDRESS`（与 HTTP 共用同一个变量） |
| `server.grpc.port` | gRPC 端口 | `50051` | int 1..65535；默认 `50051` | `FSS_GRPC_PORT`（默认 `0` = 关闭；`-1` = 系统分配） |
| `server.grpc.max_message_bytes` | 单条消息上限 | `4194304` | int ≥1024；默认 `4194304` | 未接通（登记为未实现） |
| `server.grpc.streaming_chunk_bytes` | 流式分块 | `262144` | int ≥4096；默认 `262144` | 未接通（登记为未实现） |

#### 1.2.4 `storage`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 组合根当前读取方式 |
| --- | --- | --- | --- | --- |
| `storage.driver` | 存储驱动 | `posix` | enum `posix` / `s3`；默认 `posix` | `FSS_STORAGE_DRIVER`（默认 `posix`；非法值拒绝启动） |
| `storage.proxy_mode` | `auto` = 按驱动能力决定地址形态；`always` = 强制服务代理字节 | `auto` | enum `auto` / `always`；默认 `auto` | 未接通（登记为未实现） |
| `storage.driver_report_override` | 覆盖上报的 Driver（上游硬编码 `GCS`） | `""` | string；默认 `""` | 未接通（登记为未实现） |
| `storage.provider_key_override` | 覆盖 DMS 的 `providerKey` | `""` | string；默认 `""` | 未接通（登记为未实现） |
| `storage.io_engine` | I/O 引擎（ADR-010） | `blocking` | enum `blocking` / `uring` / `auto`；默认 `blocking` | 未接通（登记为未实现；实现只有 blocking 路径） |
| `storage.io_uring.queue_depth` | io_uring 队列深度 | `64` | int 1..4096；默认 `64` | 未接通（登记为未实现） |
| `storage.io_uring.register_files` | io_uring 注册文件表 | `false` | bool；默认 `false` | 未接通（登记为未实现） |
| `storage.posix.root` | 集中存储根目录 | `/var/lib/fss/data` | string，Required；默认 `/var/lib/fss/data` | `FSS_STORAGE_ROOT`（默认 `/tmp/fss-data`；对象落在 `<root>/blobs`） |
| `storage.posix.durability` | 落盘档位 | `batch` | enum `batch` / `per_file`（★ **schema 未含 `never`**）；默认 `batch` | `FSS_POSIX_DURABILITY`（默认 `per_file`；接受 `per_file`/`batch`/`never`，见 §5） |
| `storage.posix.group_commit_max_batch` | 批提交最大批大小 | `500` | int 1..100000；默认 `500` | 未接通（登记为未实现） |
| `storage.posix.sync_dir_after_batch` | 批末 `fsync(dir)`（ADR-008 的 R2 不变量） | `true` | bool；默认 `true` | 未接通（登记为未实现） |
| `storage.posix.fsync_threshold_bytes` | 大于该值的对象强制 per-file 落盘 | `0` | int；★ schema 只允许 `0` | `FSS_POSIX_FSYNC_THRESHOLD_BYTES`（默认 `1048576`，即 1 MiB；**只在 durability=batch 时生效**） |
| `storage.posix.atomic_write` | tmp + rename 原子写 | `true` | bool；默认 `true` | 未接通（实现内固定开启） |
| `storage.posix.dir_mode` | 目录权限 | `0750` | string；默认 `0750` | 未接通（登记为未实现） |
| `storage.posix.file_mode` | 文件权限 | `0640` | string；默认 `0640` | 未接通（登记为未实现） |
| `storage.posix.fadvise_random` | 随机读提示 | `true` | bool；默认 `true` | 未接通（登记为未实现） |
| `storage.posix.fadvise_dontneed_after_large_read` | 大段读后 `DONTNEED` | `false` | bool；默认 `false` | 未接通（登记为未实现） |
| `storage.posix.shared_mount_required` | `multi` 下必须 true（共享挂载探针） | `false` | bool；默认 `false`；`multi` 必须 true | 未接通（登记为未实现；`multi` 本身拒绝启动） |
| `storage.posix.one_filesystem_per_partition` | 每 partition 独占文件系统（`syncfs` 隔离） | `false` | bool；默认 `false` | 未接通（登记为未实现） |
| `storage.s3.endpoint` | S3 端点 | `http://127.0.0.1:9000` | string；默认 `http://127.0.0.1:9000` | `FSS_STORAGE_S3_ENDPOINT`（无默认，空值 + `driver=s3` → 拒绝启动） |
| `storage.s3.region` | 区域 | `us-east-1` | string；默认 `us-east-1` | `FSS_STORAGE_S3_REGION`（默认 `us-east-1`） |
| `storage.s3.access_key` | Access Key（密文） | `${ENV:FSS_S3_ACCESS_KEY}` | string，secret；默认 `""` | `FSS_STORAGE_S3_ACCESS_KEY`（★ 内嵌 `${ENV:}` 名与实际变量名**不同**） |
| `storage.s3.secret_key` | Secret Key（密文） | `${ENV:FSS_S3_SECRET_KEY}` | string，secret；默认 `""` | `FSS_STORAGE_S3_SECRET_KEY`（★ 内嵌 `${ENV:}` 名与实际变量名**不同**） |
| `storage.s3.force_path_style` | MinIO/Ceph/SeaweedFS 需要 true | `true` | bool；默认 `true` | `FSS_STORAGE_S3_FORCE_PATH_STYLE`（默认 `true`；**只有字面 `false` 才关**） |
| `storage.s3.verify_tls` | 校验 TLS 证书 | `true` | bool；默认 `true` | `FSS_STORAGE_S3_VERIFY_TLS`（默认 `true`；只有字面 `false` 才关） |
| `storage.s3.connect_timeout_ms` | 连接超时 | `3000` | int 1..600000；默认 `3000` | 未接通（登记为未实现） |
| `storage.s3.total_timeout_ms` | 总超时 | `30000` | int 1..3600000；默认 `30000` | 未接通（登记为未实现） |
| `storage.s3.presign_default_seconds` | 预签名默认有效期 | `3600` | int 1..604800；默认 `3600` | 未接通（登记为未实现） |
| `storage.s3.presign_max_seconds` | 预签名最长有效期（对齐 OSDU 上限 7 天） | `604800` | int 1..604800；默认 `604800` | 未接通（登记为未实现） |

#### 1.2.5 `self_signed`（集中存储数据面的自签 URL）

| JSON 路径 | 含义 | 示例值 | 取值约束 | 组合根当前读取方式 |
| --- | --- | --- | --- | --- |
| `self_signed.enabled` | 自签数据面开关 | `true` | bool；默认 `true` | 未接通（登记为未实现；POSIX 模式下数据面总是注册） |
| `self_signed.public_base_url` | 对外可达基址（反向代理后填外部地址） | `http://127.0.0.1:8080` | string；默认 `http://127.0.0.1:8080` | `FSS_SELF_BASE_URL`（默认 `http://127.0.0.1:<port>/api/file`；**必须含 base path**） |
| `self_signed.signing_key` | HMAC 签名密钥；所有实例必须共享（密文） | `${ENV:FSS_TRANSFER_SIGNING_KEY}` | string，secret；默认 `""` | `FSS_TRANSFER_SECRET`（默认 `dev-secret-change-me` —— **生产必须改**） |
| `self_signed.key_id` | 密钥标识 | `k1` | string；默认 `k1` | 未接通（登记为未实现） |
| `self_signed.default_ttl_seconds` | 默认有效期 | `3600` | int 1..604800；默认 `3600` | 未接通（登记为未实现） |
| `self_signed.max_ttl_seconds` | 最长有效期 | `604800` | int 1..604800；默认 `604800` | 未接通（登记为未实现） |
| `self_signed.single_use_nonce` | 一次性 nonce（多实例下必须 false） | `false` | bool；默认 `false` | 未接通（登记为未实现） |
| `self_signed.nonce_store` | nonce 存储 | `memory` | enum `memory` / `postgres`；默认 `memory` | 未接通（登记为未实现） |

#### 1.2.6 `expiry` / `http`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 组合根当前读取方式 |
| --- | --- | --- | --- | --- |
| `expiry.default` | `expiryTime` 缺省有效期 | `1H` | string；默认 `1H` | 未接通（登记为未实现） |
| `expiry.max` | `expiryTime` 上限（超限静默截断） | `7D` | string；默认 `7D` | 未接通（登记为未实现） |
| `http.error_format` | 错误体格式 | `apperror` | enum `apperror` / `legacy` / `api_error`；默认 `apperror` | 未接通（组合根硬编码 `apperror`） |

#### 1.2.7 `metadata`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 组合根当前读取方式 |
| --- | --- | --- | --- | --- |
| `metadata.repository` | 元数据仓储实现 | `sqlite` | enum `sqlite` / `postgres` / `remote`；默认 `sqlite` | 未接通（组合根**总是**创建 SQLite 元数据仓储） |
| `metadata.sqlite.path` | 元数据库路径 | `/var/lib/fss/meta.db` | string；默认 `/var/lib/fss/meta.db` | `FSS_METADATA_SQLITE_PATH`（默认 `<storage_root>/metadata.db`） |
| `metadata.sqlite.busy_timeout_ms` | SQLite busy 超时 | `5000` | int 0..600000；默认 `5000` | 未接通（登记为未实现） |
| `metadata.sqlite.journal_mode` | SQLite journal 模式 | `WAL` | enum `WAL` / `DELETE` / `TRUNCATE`；默认 `WAL` | 未接通（登记为未实现） |
| `metadata.sqlite.synchronous` | SQLite 同步级别 | `NORMAL` | enum `NORMAL` / `FULL` / `OFF`；默认 `NORMAL` | 未接通（登记为未实现） |
| `metadata.sqlite.max_write_concurrency` | 有界写并发 | `8` | int 1..256；默认 `8` | 未接通（登记为未实现） |
| `metadata.sqlite.group_commit` | 组提交 | `true` | bool；默认 `true` | 未接通（登记为未实现） |
| `metadata.sqlite.group_commit_max_wait_ms` | 组提交最大等待 | `5` | int 0..1000；默认 `5` | 未接通（登记为未实现） |
| `metadata.sqlite.group_commit_max_batch` | 组提交最大批 | `64` | int 1..100000；默认 `64` | 未接通（登记为未实现） |
| `metadata.postgres.dsn` | PG 连接串（密文） | `${ENV:FSS_PG_DSN}` | string，secret；默认 `""` | 未接通（登记为未实现；`multi` 运行形态未交付） |
| `metadata.postgres.max_connections` | PG 连接数（实例数 × 该值 ≤ PG `max_connections`） | `16` | int 1..10000；默认 `16` | 未接通（登记为未实现） |
| `metadata.postgres.statement_timeout_ms` | 语句超时 | `5000` | int 1..600000；默认 `5000` | 未接通（登记为未实现） |
| `metadata.postgres.schema_version_check` | 启动时校验 schema 版本 | `true` | bool；默认 `true` | 未接通（登记为未实现） |
| `metadata.remote.base_url` | 远端 Storage Service 地址 | `""` | string；默认 `""` | 未接通（登记为未实现） |
| `metadata.remote.token_provider` | 远端 token 来源 | `static` | string；默认 `static` | 未接通（登记为未实现） |
| `metadata.remote.static_token` | 静态 token（密文） | `${ENV:FSS_STORAGE_TOKEN}` | string，secret；默认 `""` | 未接通（登记为未实现） |
| `metadata.remote.timeout_ms` | 远端调用超时 | `5000` | int 1..600000；默认 `5000` | 未接通（登记为未实现） |

#### 1.2.8 `location`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 组合根当前读取方式 |
| --- | --- | --- | --- | --- |
| `location.repository` | 位置仓储实现 | `sqlite` | enum `sqlite` / `postgres`；默认 `sqlite` | 未接通（组合根**总是**创建 SQLite 位置仓储） |
| `location.sqlite.path` | 位置数据库路径 | `/var/lib/fss/location.db` | string；默认 `/var/lib/fss/location.db` | `FSS_SQLITE_PATH`（默认 `<storage_root>/location.db`） |
| `location.sqlite.busy_timeout_ms` | SQLite busy 超时 | `10000` | int 0..600000；默认 `10000` | 未接通（登记为未实现） |
| `location.sqlite.journal_mode` | journal 模式 | `WAL` | enum `WAL` / `DELETE` / `TRUNCATE`；默认 `WAL` | 未接通（登记为未实现） |
| `location.sqlite.synchronous` | 同步级别 | `NORMAL` | enum `NORMAL` / `FULL` / `OFF`；默认 `NORMAL` | 未接通（登记为未实现） |
| `location.sqlite.max_write_concurrency` | 有界写并发 | `8` | int 1..256；默认 `8` | 未接通（登记为未实现） |
| `location.sqlite.group_commit` | 组提交 | `true` | bool；默认 `true` | 未接通（登记为未实现） |
| `location.sqlite.group_commit_max_wait_ms` | 组提交最大等待 | `5` | int 0..1000；默认 `5` | 未接通（登记为未实现） |
| `location.sqlite.group_commit_max_batch` | 组提交最大批 | `64` | int 1..100000；默认 `64` | 未接通（登记为未实现） |
| `location.postgres.dsn` | PG 连接串（密文） | `${ENV:FSS_PG_DSN}` | string，secret；默认 `""` | 未接通（登记为未实现） |
| `location.postgres.max_connections` | PG 连接数 | `8` | int 1..10000；默认 `8` | 未接通（登记为未实现） |

#### 1.2.9 `leases` / `leader_election`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 组合根当前读取方式 |
| --- | --- | --- | --- | --- |
| `leases.enabled` | 在途状态租约（多实例必需） | `false` | bool；默认 `false`；`multi` 必须 true | 未接通（登记为未实现） |
| `leases.ttl_seconds` | 租约 TTL | `60` | int 1..86400；默认 `60` | 未接通（登记为未实现） |
| `leases.renew_interval_seconds` | 续租间隔（必须小于 TTL） | `20` | int 1..86400；默认 `20`；跨字段 `renew < ttl` | 未接通（登记为未实现） |
| `leases.time_source` | 时间源（`database` = PG `now()`） | `database` | enum `database` / `local`；默认 `database` | 未接通（登记为未实现） |
| `leader_election.enabled` | 单例后台任务的领导者选举 | `false` | bool；默认 `false`；`multi` 必须 true | 未接通（登记为未实现） |
| `leader_election.backend` | 选举后端 | `postgres_advisory_lock` | enum `postgres_advisory_lock`；默认 `postgres_advisory_lock` | 未接通（登记为未实现） |
| `leader_election.lock_key` | advisory lock key | `1179865927` | int 0..2147483647；默认 `1179865927` | 未接通（登记为未实现） |

#### 1.2.10 `auth`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 组合根当前读取方式 |
| --- | --- | --- | --- | --- |
| `auth.mode` | 鉴权模式 | `jwt` | enum `jwt` / `remote-entitlements` / `disabled`；schema 默认 `jwt` | `FSS_AUTH_MODE`（**组合根默认 `disabled`**；非法值拒绝启动） |
| `auth.jwt.jwks_url` | RS256/JWKS 地址（**未实现**） | `""` | string；默认 `""`；非空 → 拒绝启动 | 未接通（登记为未实现；schema 直接拒绝非空值） |
| `auth.jwt.issuer` | 必须匹配的 `iss` | `""` | string；默认 `""` | `FSS_JWT_ISSUER`（默认空 = 不校验 `iss`） |
| `auth.jwt.audience` | 必须包含的 `aud` | `""` | string；默认 `""` | `FSS_JWT_AUDIENCE`（默认空 = 不校验 `aud`） |
| `auth.jwt.verify_signature` | 是否验签 | `true` | bool；默认 `true`；production 必须 true | `FSS_JWT_VERIFY_SIGNATURE`（默认 `true`；只有字面 `false` 才关） |
| `auth.jwt.roles_claim` | 角色 claim 名 | `roles` | string；默认 `roles` | 未接通（登记为未实现；实现内固定默认值 `roles`） |
| `auth.jwt.user_id_claim` | 用户 claim 名 | `email` | string；默认 `email` | `FSS_JWT_USER_ID_CLAIM`（默认 `email`） |
| `auth.jwt.hmac_secret` | HS256 共享密钥（密文） | `${ENV:FSS_JWT_HMAC_SECRET}` | string，secret；默认 `""` | `FSS_JWT_HMAC_SECRET`（默认空；`auth.mode=jwt` 且为空 → **拒绝启动**） |
| `auth.jwt.partition_claim` | 租户绑定 claim 名 | `data-partition-id` | string；默认 `data-partition-id` | `FSS_JWT_PARTITION_CLAIM`（默认 `data-partition-id`） |
| `auth.jwt.require_partition_claim` | 是否强制租户 claim | `true` | bool；默认 `true` | `FSS_JWT_REQUIRE_PARTITION_CLAIM`（默认 `true`；只有字面 `false` 才关） |
| `auth.local_roles.admin@example.com` | 静态角色表（用户 → 角色数组） | 8 个 `service.*` 角色 | 动态子树（`auth.local_roles` 前缀），无逐键约束 | 未接通（登记为未实现；组合根不装配静态角色表） |
| `auth.local_roles.editor@example.com` | 静态角色表项 | `["service.file.editors","service.file.viewers"]` | 同上 | 未接通（登记为未实现） |
| `auth.local_roles.viewer@example.com` | 静态角色表项 | `["service.file.viewers"]` | 同上 | 未接通（登记为未实现） |
| `auth.remote_entitlements.base_url` | 远端 Entitlements 地址 | `""` | string；默认 `""`；`remote-entitlements` 时必须非空 | `FSS_ENTITLEMENTS_URL`（默认空；非空校验在 schema，组合根用 `Ready()` 判空并拒绝启动） |
| `auth.remote_entitlements.authorize_path` | authorizeAny 路径 | `/api/entitlements/v2/authorizeAny` | string；默认 `/api/entitlements/v2/authorizeAny` | `FSS_ENTITLEMENTS_AUTHORIZE_PATH`（默认同示例值） |
| `auth.remote_entitlements.connect_timeout_ms` | 连接超时 | `1000` | int 1..600000；默认 `1000` | 未接通（登记为未实现；实现内固定默认 1000 ms） |
| `auth.remote_entitlements.timeout_ms` | 整体超时 | `3000` | int 1..600000；默认 `3000` | `FSS_ENTITLEMENTS_TIMEOUT_MS`（默认 3000 ms） |
| `auth.remote_entitlements.fail_closed` | 依赖不可用不得降级为放行 | `true` | bool；默认 `true`；必须 true | 未接通（实现内**恒为** fail-closed，不可关） |

#### 1.2.11 `legal` / `schema` / `events`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 组合根当前读取方式 |
| --- | --- | --- | --- | --- |
| `legal.validator` | 法务标签校验器 | `noop` | enum `noop` / `remote`；默认 `noop` | 未接通（实现内固定 noop） |
| `legal.remote.base_url` | 远端法务校验地址 | `""` | string；默认 `""` | 未接通（登记为未实现） |
| `legal.remote.timeout_ms` | 远端法务校验超时 | `3000` | int 1..600000；默认 `3000` | 未接通（登记为未实现） |
| `schema.validator` | schema 校验器 | `noop` | enum `noop` / `remote`；默认 `noop` | 未接通（实现内固定 noop） |
| `schema.remote.base_url` | 远端 schema 校验地址 | `""` | string；默认 `""` | 未接通（登记为未实现） |
| `schema.remote.timeout_ms` | 远端 schema 校验超时 | `3000` | int 1..600000；默认 `3000` | 未接通（登记为未实现） |
| `events.publisher` | 事件发布器 | `log` | enum `log` / `webhook` / `none`；默认 `log` | 未接通（实现内固定写日志） |
| `events.webhook.url` | webhook 地址 | `""` | string；默认 `""` | 未接通（登记为未实现） |
| `events.webhook.timeout_ms` | webhook 超时 | `3000` | int 1..600000；默认 `3000` | 未接通（登记为未实现） |
| `events.webhook.topic` | webhook topic | `status-changed` | string；默认 `status-changed` | 未接通（登记为未实现） |

#### 1.2.12 `partition`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 组合根当前读取方式 |
| --- | --- | --- | --- | --- |
| `partition.registry` | 租户注册表来源 | `file` | enum `file` / `remote`；默认 `file` | 未接通（组合根只装配**单个**内置租户 `opendes`） |
| `partition.file.opendes.staging_container` | staging 容器名 | `opendes-staging` | 动态子树（`partition.file` 前缀），无逐键约束 | 未接通（登记为未实现；实现按 `ObjectKeyPolicy::ContainerFor` 生成 `<partition>-staging`） |
| `partition.file.opendes.persistent_container` | persistent 容器名 | `opendes-persistent` | 同上 | 未接通（登记为未实现；实现生成 `<partition>-persistent`） |
| `partition.file.opendes.storage_driver` | 分区级驱动覆盖 | `posix` | 同上 | 未接通（登记为未实现；组合根的租户驱动由 `FSS_STORAGE_DRIVER` 决定） |
| `partition.file.opendes.max_file_bytes` | 单对象上限（0 = 不限） | `0` | 同上 | 未接通（登记为未实现） |
| `partition.file.opendes.allowed_checksum_algorithms` | 允许的校验和算法 | `["SHA-256","MD5","SHA-1"]` | 同上 | 未接通（登记为未实现） |
| `partition.file.opendes.default_checksum_algorithm` | 缺省校验和算法 | `SHA-256` | 同上 | 未接通（登记为未实现） |

#### 1.2.13 `gc`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 组合根当前读取方式 |
| --- | --- | --- | --- | --- |
| `gc.enabled` | GC 开关 | `false` | bool；默认 `false` | 未接通（登记为未实现；组合根**不装配 GC 任务**，见 §6.5） |
| `gc.dry_run` | 只记录不删除 | `true` | bool；默认 `true` | 未接通（登记为未实现） |
| `gc.require_lease_expiry` | 必须"租约到期 + 无记录"才回收 | `true` | bool；默认 `true`；`multi` 必须 true | 未接通（登记为未实现） |
| `gc.staging_ttl_hours` | staging TTL（小时） | `24` | int 1..87600；默认 `24` | 未接通（登记为未实现；`.tmp_*` 清理判据与它同源） |
| `gc.orphan_grace_hours` | persistent 孤儿宽限（小时） | `72` | int 1..87600；默认 `72` | 未接通（登记为未实现） |
| `gc.interval_seconds` | GC 调度间隔 | `3600` | int 1..86400；默认 `3600` | 未接通（登记为未实现；无调度器） |

#### 1.2.14 `observability`

| JSON 路径 | 含义 | 示例值 | 取值约束 | 组合根当前读取方式 |
| --- | --- | --- | --- | --- |
| `observability.log_level` | 最低日志级别 | `info` | enum `debug` / `info` / `warn` / `error`；默认 `info` | 未接通（实现内固定 `info`） |
| `observability.log_format` | 日志格式 | `json` | enum `json` / `text`；默认 `json` | 未接通（实现内固定 `json`） |
| `observability.audit_enabled` | 审计开关 | `true` | bool；默认 `true` | 未接通（实现内固定开启审计记录） |
| `observability.audit_fail_closed` | 审计失败是否影响操作结果 | `false` | bool；默认 `false` | 未接通（登记为未实现） |
| `observability.metrics_enabled` | 指标端点开关 | `true` | bool；默认 `true` | 未接通（`/metrics` 恒开） |
| `observability.metrics_path` | 指标端点路径 | `/metrics` | string；默认 `/metrics` | 未接通（实现内固定 `/metrics`） |
| `observability.redact_keys` | 日志/诊断输出要打码的键名（子串匹配，忽略大小写与 `_`/`-`） | 11 个键名 | list；默认 `secret_key,access_key,token,sig,signature,authorization,x-amz-signature,password,signing_key,dsn,static_token` | 未接通（登记为未实现）。⚠️ 组合根用 `LogOptions{}` 构造 logger → `redact_keys` 为空 → **真实进程当前不做任何打码**；加载器侧桥接函数 `logging::OptionsFromConfig` 已存在但未被调用 |

### 1.3 未接通清单（单独成表）

> 口径：**组合根没有读取**该键（既无 JSON 加载，也无对应 `FSS_*`）。
> 它们目前只有在 `tests/` 里被直接构造的对象读取（或根本没有实现），因此**运维改它们不生效**。
> 共 **125** 个键；能接通的 31 个见 §1.2 各表（下文 §1.4 归纳）。

| 前缀 | 键数 | 键 |
| --- | --- | --- |
| `deployment` | 3 | `deployment.environment`、`deployment.instance_id`、`deployment.clock_skew_tolerance_seconds` |
| server.http | 21 | `server.http.base_path`、`server.http.max_header_bytes`、`server.http.max_uri_bytes`、`server.http.max_body_bytes`、`server.http.tcp_nodelay`、`server.http.worker_threads`、`server.http.max_connections`、`server.http.max_connections_per_partition`、`server.http.idle_timeout_seconds`、`server.http.json_request_timeout_seconds`、`server.http.transfer_idle_timeout_seconds`、`server.http.transfer_max_body_bytes`、`server.http.transfer_buffer_bytes`、`server.http.transfer_memory_budget_bytes`、`server.http.large_file_plane.enabled`、`server.http.large_file_plane.bind`、`server.http.large_file_plane.port`、`server.http.large_file_plane.use_sendfile`、`server.http.large_file_plane.sendfile_chunk_bytes`、`server.http.large_file_plane.workers`、`server.http.large_file_plane.max_connections` |
| server.grpc | 3 | `server.grpc.enabled`、`server.grpc.max_message_bytes`、`server.grpc.streaming_chunk_bytes` |
| `storage` | 19 | `storage.proxy_mode`、`storage.driver_report_override`、`storage.provider_key_override`、`storage.io_engine`、`storage.io_uring.queue_depth`、`storage.io_uring.register_files`、`storage.posix.group_commit_max_batch`、`storage.posix.sync_dir_after_batch`、`storage.posix.atomic_write`、`storage.posix.dir_mode`、`storage.posix.file_mode`、`storage.posix.fadvise_random`、`storage.posix.fadvise_dontneed_after_large_read`、`storage.posix.shared_mount_required`、`storage.posix.one_filesystem_per_partition`、`storage.s3.connect_timeout_ms`、`storage.s3.total_timeout_ms`、`storage.s3.presign_default_seconds`、`storage.s3.presign_max_seconds` |
| `self_signed` | 6 | `self_signed.enabled`、`self_signed.key_id`、`self_signed.default_ttl_seconds`、`self_signed.max_ttl_seconds`、`self_signed.single_use_nonce`、`self_signed.nonce_store` |
| `expiry` | 2 | `expiry.default`、`expiry.max` |
| `http` | 1 | `http.error_format` |
| `metadata` | 16 | `metadata.repository`、`metadata.sqlite.busy_timeout_ms`、`metadata.sqlite.journal_mode`、`metadata.sqlite.synchronous`、`metadata.sqlite.max_write_concurrency`、`metadata.sqlite.group_commit`、`metadata.sqlite.group_commit_max_wait_ms`、`metadata.sqlite.group_commit_max_batch`、`metadata.postgres.dsn`、`metadata.postgres.max_connections`、`metadata.postgres.statement_timeout_ms`、`metadata.postgres.schema_version_check`、`metadata.remote.base_url`、`metadata.remote.token_provider`、`metadata.remote.static_token`、`metadata.remote.timeout_ms` |
| `location` | 10 | `location.repository`、`location.sqlite.busy_timeout_ms`、`location.sqlite.journal_mode`、`location.sqlite.synchronous`、`location.sqlite.max_write_concurrency`、`location.sqlite.group_commit`、`location.sqlite.group_commit_max_wait_ms`、`location.sqlite.group_commit_max_batch`、`location.postgres.dsn`、`location.postgres.max_connections` |
| `leases` | 4 | `leases.enabled`、`leases.ttl_seconds`、`leases.renew_interval_seconds`、`leases.time_source` |
| `leader_election` | 3 | `leader_election.enabled`、`leader_election.backend`、`leader_election.lock_key` |
| `auth` | 7 | `auth.jwt.jwks_url`、`auth.jwt.roles_claim`、`auth.local_roles.admin@example.com`、`auth.local_roles.editor@example.com`、`auth.local_roles.viewer@example.com`、`auth.remote_entitlements.connect_timeout_ms`、`auth.remote_entitlements.fail_closed` |
| `legal` | 3 | `legal.validator`、`legal.remote.base_url`、`legal.remote.timeout_ms` |
| `schema` | 3 | `schema.validator`、`schema.remote.base_url`、`schema.remote.timeout_ms` |
| `events` | 4 | `events.publisher`、`events.webhook.url`、`events.webhook.timeout_ms`、`events.webhook.topic` |
| `partition` | 7 | `partition.registry`、`partition.file.opendes.staging_container`、`partition.file.opendes.persistent_container`、`partition.file.opendes.storage_driver`、`partition.file.opendes.max_file_bytes`、`partition.file.opendes.allowed_checksum_algorithms`、`partition.file.opendes.default_checksum_algorithm` |
| `gc` | 6 | `gc.enabled`、`gc.dry_run`、`gc.require_lease_expiry`、`gc.staging_ttl_hours`、`gc.orphan_grace_hours`、`gc.interval_seconds` |
| `observability` | 7 | `observability.log_level`、`observability.log_format`、`observability.audit_enabled`、`observability.audit_fail_closed`、`observability.metrics_enabled`、`observability.metrics_path`、`observability.redact_keys` |

### 1.4 已接通清单（31 个键）与命名不一致

已接通（组合根会读 `FSS_*`）：

| JSON 路径 | 实际环境变量 | 组合根默认 |
| --- | --- | --- |
| `deployment.max_clock_skew_seconds` | `FSS_MAX_CLOCK_SKEW_SECONDS` | 5（仅 jwt 模式读取） |
| `deployment.mode` | `FSS_DEPLOYMENT_MODE` | `single` |
| `server.http.bind` | `FSS_BIND_ADDRESS` | `0.0.0.0` |
| `server.http.port` | `FSS_HTTP_PORT` | `8080` |
| `server.grpc.bind` | `FSS_BIND_ADDRESS` | `0.0.0.0`（与 HTTP 共用） |
| `server.grpc.port` | `FSS_GRPC_PORT` | `0`（关闭） |
| `storage.driver` | `FSS_STORAGE_DRIVER` | `posix` |
| `storage.posix.root` | `FSS_STORAGE_ROOT` | `/tmp/fss-data` |
| `storage.posix.durability` | `FSS_POSIX_DURABILITY` | `per_file` |
| `storage.posix.fsync_threshold_bytes` | `FSS_POSIX_FSYNC_THRESHOLD_BYTES` | `1048576` |
| `storage.s3.endpoint` | `FSS_STORAGE_S3_ENDPOINT` | `""` |
| `storage.s3.region` | `FSS_STORAGE_S3_REGION` | `us-east-1` |
| `storage.s3.access_key` | `FSS_STORAGE_S3_ACCESS_KEY` | `""` |
| `storage.s3.secret_key` | `FSS_STORAGE_S3_SECRET_KEY` | `""` |
| `storage.s3.force_path_style` | `FSS_STORAGE_S3_FORCE_PATH_STYLE` | `true` |
| `storage.s3.verify_tls` | `FSS_STORAGE_S3_VERIFY_TLS` | `true` |
| `self_signed.public_base_url` | `FSS_SELF_BASE_URL` | `http://127.0.0.1:<port>/api/file` |
| `self_signed.signing_key` | `FSS_TRANSFER_SECRET` | `dev-secret-change-me`（生产必须替换） |
| `metadata.sqlite.path` | `FSS_METADATA_SQLITE_PATH` | `<storage_root>/metadata.db` |
| `location.sqlite.path` | `FSS_SQLITE_PATH` | `<storage_root>/location.db` |
| `auth.mode` | `FSS_AUTH_MODE` | `disabled`（★ 与 schema 默认 `jwt` 不同） |
| `auth.jwt.issuer` | `FSS_JWT_ISSUER` | `""` |
| `auth.jwt.audience` | `FSS_JWT_AUDIENCE` | `""` |
| `auth.jwt.verify_signature` | `FSS_JWT_VERIFY_SIGNATURE` | `true` |
| `auth.jwt.user_id_claim` | `FSS_JWT_USER_ID_CLAIM` | `email` |
| `auth.jwt.hmac_secret` | `FSS_JWT_HMAC_SECRET` | `""` |
| `auth.jwt.partition_claim` | `FSS_JWT_PARTITION_CLAIM` | `data-partition-id` |
| `auth.jwt.require_partition_claim` | `FSS_JWT_REQUIRE_PARTITION_CLAIM` | `true` |
| `auth.remote_entitlements.base_url` | `FSS_ENTITLEMENTS_URL` | `""` |
| `auth.remote_entitlements.authorize_path` | `FSS_ENTITLEMENTS_AUTHORIZE_PATH` | `/api/entitlements/v2/authorizeAny` |
| `auth.remote_entitlements.timeout_ms` | `FSS_ENTITLEMENTS_TIMEOUT_MS` | `3000` |

**命名不一致（容易配错，务必注意）**

| JSON 键 | 示例文件内嵌的 `${ENV:...}` | 组合根实际读的 | 结论 |
| --- | --- | --- | --- |
| `storage.s3.access_key` | `FSS_S3_ACCESS_KEY` | `FSS_STORAGE_S3_ACCESS_KEY` | 名字不同；按**组合根**的名字注入 |
| `storage.s3.secret_key` | `FSS_S3_SECRET_KEY` | `FSS_STORAGE_S3_SECRET_KEY` | 同上 |
| `self_signed.signing_key` | `FSS_TRANSFER_SIGNING_KEY` | `FSS_TRANSFER_SECRET` | 同上 |
| `auth.jwt.hmac_secret` | `FSS_JWT_HMAC_SECRET` | `FSS_JWT_HMAC_SECRET` | 一致 |
| `metadata.postgres.dsn` | `FSS_PG_DSN` | 无（未接通） | 目前不生效 |
| `location.postgres.dsn` | `FSS_PG_DSN` | 无（未接通） | 目前不生效 |
| `metadata.remote.static_token` | `FSS_STORAGE_TOKEN` | 无（未接通） | 目前不生效 |

**两处默认值分歧（以组合根为准）**：`auth.mode`（组合根 `disabled` vs schema `jwt`）、
`storage.posix.durability`（组合根 `per_file` vs schema `batch`）与
`storage.posix.fsync_threshold_bytes`（组合根 1 MiB vs schema 只允许 0）。
这意味着**不设任何环境变量启动 = 无鉴权 + 每文件 fsync**；生产必须显式设置
`FSS_AUTH_MODE=jwt`、`FSS_JWT_HMAC_SECRET`、`FSS_POSIX_DURABILITY=batch`。

---

## 2. 启动 / 探活 / 关闭

### 2.1 启动

`./build/bin/fss_server`（无命令行参数）。启动横幅会打印实际 `bind`、`grpc bind`、
`base path`、`storage driver`、`storage root`、`sqlite path`、`error format`、`auth`。
任何配置错误在**启动时**以非零退出码失败，并打印一行原因（stderr）：

| stderr 前缀 | 含义 |
| --- | --- |
| `拒绝启动：deployment.mode=multi ...` | 多实例运行形态未交付 |
| `拒绝启动：<原因>`（jwt 分支） | `auth.mode=jwt` 但 `FSS_JWT_HMAC_SECRET` 为空 |
| `拒绝启动：<原因>`（远端分支） | `auth.mode=remote-entitlements` 但未配 `FSS_ENTITLEMENTS_URL` |
| `未知的 storage.driver: ...` | 只接受 `posix` / `s3` |
| `未知的 storage.posix.durability: ...` | 只接受 `per_file` / `batch` / `never` |
| `未知的 auth.mode: ...` | 只接受 `jwt` / `remote-entitlements` / `disabled` |
| `storage.driver=s3 需要 FSS_STORAGE_S3_ENDPOINT / _ACCESS_KEY / _SECRET_KEY` | S3 三要素缺失 |
| `打开位置仓储失败: ...` / `打开元数据仓储失败: ...` | SQLite 打不开（见 §6.2） |
| `监听失败: ...` | 端口被占用/无权限 |

### 2.2 探活端点（都在 base path 下，免鉴权）

| 端点 | 用途 | 预期 |
| --- | --- | --- |
| `GET /api/file/v2/liveness_check` | 进程存活（`@PermitAll`） | `200` **纯文本** `File service is alive`。**不碰任何依赖**，所以它 200 不代表能服务 |
| `GET /api/file/v2/readiness_check` | 依赖就绪 | 探针 = 元数据仓储 `List("__readiness__")`；成功 `200` 纯文本 `File service is ready`；失败 `503` 纯文本 `File service is not ready` |
| `GET /api/file/v2/info` | 版本信息 | `200` JSON：`version`（`"v2"`）、`buildVersion`、`connectedOuterServices`（内置 SQLite 时为 `["storage"]`）、`authMode`（`jwt` / `remote-entitlements` / `disabled`，非规范扩展字段，**必须**用它来确认鉴权是否真的开着） |
| `GET /metrics` | 指标（§3.1） | `200`，`Content-Type: text/plain; version=0.0.4; charset=utf-8` |

> `readiness_check` **只**探元数据仓储，不探存储/磁盘水位；磁盘满时它仍可能返回 200（见 §6.1）。
> 编排探针请用 readiness 做流量准入、用 liveness 做重启判定；两者不得互换。

### 2.3 关闭

进程只处理 `SIGINT` / `SIGTERM` 的默认语义；退出路径会先 `Shutdown()` gRPC 服务再退出
（避免挂在 gRPC 线程池上）。建议：

```bash
kill -TERM "$(cat /run/fss.pid)"      # 优雅退出
# 等待端口释放（不要立刻重启，见 §6.1 "磁盘满时不要重启"）
for i in $(seq 1 30); do ss -ltn | grep -q ':8080 ' || break; sleep 1; done
```

**不要**用 `kill -9`：数据面正在 rename 的对象会留下 `.tmp.*`（由 GC 清理，而 GC 尚未接入，见 §6.5）。

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

**GC 层（`GcTask`）**

| 指标名 | 类型 | 标签 | 含义 |
| --- | --- | --- | --- |
| `fss_gc_runs_total` | counter | `mode` ∈ `dry_run` / `real`、`outcome` ∈ `ok` / `error` | GC 运行次数 |
| `fss_gc_objects_deleted_total` | counter | 无 | 删除的对象数（dry-run 下为候选数） |
| `fss_gc_tmp_removed_total` | counter | 无 | 清理的 `.tmp_*` 临时文件数 |
| `fss_gc_skipped_total` | counter | `reason` ∈ `has_record` / `too_young` / `tmp_too_young` / `no_location` | 跳过的对象数（`tmp_too_young` = 在途上传被保护，可观测） |
| `fss_gc_last_run_epoch_seconds` | gauge | 无 | 最近一次 GC 运行时间（epoch 秒） |

> ⚠️ **未接通（登记为未实现）**：组合根**没有**创建 `fss::metrics::Registry`、也没有包
> `MeteredBlobStore`、更没有装配 `GcTask`，`RouterOptions::metrics_registry` 保持为
> `nullptr`。因此**真实进程的 `/metrics` 当前只渲染 HTTP 那一族**（存储/GC 族不会出现）。
> 这些指标名与标签由 [`tests/hardening/test_metrics_and_gc.cpp`](../tests/hardening/test_metrics_and_gc.cpp)
> 在真实夹具上验证（C9.6：格式合法、值真的动过、不含 secret）。接上组合根属于 §8 的未实现项。

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
  ⚠️ **未接通**：组合根用空 options 构造 logger，**当前真实进程没有任何打码规则**（见 §1.2.14）。
  排障时不要用日志打印 `Authorization` / `X-Amz-Signature` 等头——目前不会被自动遮盖。
* **级别/格式未接通**：真实进程固定 `info` + `json`；改 `observability.log_level` 不生效。

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

组合根（`src/main/server_main.cpp`）把环境变量映射到 `PosixBlobStoreOptions`：

| `FSS_POSIX_DURABILITY` | 映射到 | 语义 | 示例文件对应值 |
| --- | --- | --- | --- |
| `per_file` | `FsyncPolicy::kAlways` | 每个对象都 `fdatasync` + `fsync(dir)`：精确但慢 | `per_file`（schema 允许） |
| `batch` | `FsyncPolicy::kBySize` + 阈值 | 小于 `FSS_POSIX_FSYNC_THRESHOLD_BYTES` 的对象不单独 fsync，靠批提交/调用方摊销 | `batch`（**schema 默认**） |
| `never` | `FsyncPolicy::kNever` | 完全不 fsync；**只允许用于可重建数据**，进程崩溃可能丢已确认的写入 | ★ **schema 的 enum 未包含 `never`**，所以 `storage.posix.durability` 写 `never` 过不了配置校验；目前只能通过环境变量 `FSS_POSIX_DURABILITY=never` 打开 |

其他要点：

* 组合根默认值是 **`per_file`**（不是 schema/示例的 `batch`），且 `batch` 档的阈值默认 **1 MiB**。
* 取值为未知字符串 → **拒绝启动**并提示可选三档。
* `never` 启动时会在 stderr 打印显式告警，**不做静默降级**。

### 5.2 实测吞吐差异（同一次运行内 3 次取中位数）

| 点位（c4，4 KiB 对象，上传链） | files/s | 相对 `per_file` |
| --- | --- | --- |
| `per_file` | **109.9** | 1.0× |
| `batch` | **468.6** | **4.3×** |
| `never` | **452.0** | 4.1× |

来源：[`docs/05-capacity-and-concurrency.md`](05-capacity-and-concurrency.md) §1.9（命令
`scripts/bench_baseline.sh`）。另一次独立记录为 `per_file` 96.4 / `batch` 410.1 / `never` 426.0，
比值同样是 **4.3×**（[`docs/02-design.md`](02-design.md) §13.4）——**结论一致**：收益来自
"摊销 fsync"，不是"放弃耐久性"（`batch ≈ never`）。⚠️ 均为 WSL2 虚拟盘，仅作本机量级参考。

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
* `syncfs` 是**文件系统级**操作，会 flush 整个文件系统；多业务共盘会互相牵连 →
  部署上建议按 partition 分盘（配置键 `storage.posix.one_filesystem_per_partition` 与
  `storage.posix.shared_mount_required`，目前**未接通**）。

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
* **不要**把 SQLite 放到多实例共享盘上（ADR-009 / ADR-004；`deployment.mode=multi` 已被组合根拒绝启动）。
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

**背景（很重要）**：`GcTask` 与 `IBlobStore::remove_temp_files` **已实现并有测试**，但组合根
**没有装配 GC**（`gc.*` 全部未接通，且没有 CLI / 调度器）。所以在真实进程上：
**`.tmp_*` 不会被自动清理**。临时文件命名形如
`<对象路径>.tmp.<instance_id>.<pid>.<counter>`（实例标识固定为 `local`）。

**诊断**

```bash
find /var/lib/fss/data/blobs -name '*.tmp.*' -printf '%TY-%Tm-%Td %TH:%TM %s %p\n' | sort | tail -50
find /var/lib/fss/data/blobs -name '*.tmp.*' -mmin -1440 | wc -l   # 24h 内 = 可能在途
du -sh /var/lib/fss/data/blobs
```

**处置**：人工清理**只**能删 `mtime` 早于 staging TTL（`gc.staging_ttl_hours`，默认 24 h = `-mmin +1440`）的
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
`deployment.environment=production` 时 ① 禁止 `auth.mode=disabled`；② 禁止
`auth.jwt.verify_signature=false`；③ `auth.mode=jwt` 必须有非空 `auth.jwt.hmac_secret`。
⚠️ 这三条校验目前**不在组合根启动路径上**（组合根不加载 schema），只是"文档与配置层的规则"——
所以生产上必须靠环境变量注入正确值 + 外部检查 `authMode`。

### 7.2 密钥注入

* **只走环境变量，不入镜像**：`FSS_JWT_HMAC_SECRET`、`FSS_TRANSFER_SECRET`、
  `FSS_STORAGE_S3_ACCESS_KEY` / `FSS_STORAGE_S3_SECRET_KEY`。
  容器平台用 Secret → 环境变量（或 `*_FILE` 包装）注入，**不要**把明文写进镜像层或 JSON 样例。
* `auth.mode=jwt` 且 `FSS_JWT_HMAC_SECRET` 为空 → **拒绝启动**（不是"起来但拒绝所有 token"）。
* `self_signed.signing_key` 的示例值是 `dev-secret-change-me` —— **生产必须替换**。
* 多副本必须共享同一签名密钥与 JWT 密钥，否则 A 签发的 URL/token 到 B 会被拒。
* `/v2/info` 不输出任何密钥；日志脱敏依赖 `observability.redact_keys`（**当前未接通**，见 §3.2）。

### 7.3 `deployment.mode=multi` 当前**拒绝启动**

组合根在 `FSS_DEPLOYMENT_MODE=multi` 时打印
`拒绝启动：deployment.mode=multi 需要 PG 仓储 + PG 租约 + 数据库时钟（ADR-009），本版本尚未交付（计划 P9）。`
并返回退出码 1。这是**有意的**：多实例所需的状态外置（PG 仓储/租约/数据库时钟）未交付，
静默"以单实例状态跑在多实例里"会让各实例状态发散。见
[`docs/adr/ADR-009-multi-instance-consistency.md`](adr/ADR-009-multi-instance-consistency.md)。

---

## 8. 未实现 / 未验证清单（如实登记）

| 项 | 状态 | 说明 / 证据 |
| --- | --- | --- |
| 组合根接 `config/fss.example.json` | **未接通** | 只读环境变量；§1.3 的 125 个键不可配 |
| PG 仓储 / PG 租约 / 数据库时钟 | **未实现** | `deployment.mode=multi` 运行形态不存在，组合根拒绝启动（ADR-009） |
| 多实例运行形态 | **未验证** | 无 PG 版仓储与租约；`leases.*`、`leader_election.*` 全部未接通 |
| 远端 Storage Service 仓储（`metadata.repository=remote`） | **未实现** | ADR-004 列为可选 |
| RS256 / JWKS（`auth.jwt.jwks_url`） | **未实现** | 配置非空 → schema 拒绝启动（ADR-012 §5.3）；只支持 HS256 |
| `auth.jwt.roles_claim` / `auth.local_roles` | **未接通** | 角色 claim 名固定 `roles`；静态角色表未装配 |
| io_uring（`storage.io_engine=uring`） | **未接通 / 环境阻断** | 默认容器 seccomp 实测 `EPERM`；先跑 [`scripts/check_io_uring.sh`](../scripts/check_io_uring.sh)（退出码 2 = 无结论）；ADR-010 |
| 真实 S3 / MinIO 端到端 | **未验证** | 仅 AWS 官方向量 + 独立验签的 mock（ADR-005）；分片上传 >5 GiB、退避重试、STS 刷新未测 |
| 真实 NFS / 多客户端共享挂载语义 | **未验证** | ADR-009 登记为**上生产硬前提**（C9.27 未验证）；实现不依赖 NFS 文件锁 |
| 容器镜像 | **见 P9 证据** | 本仓库当前没有 `docs/test-evidence/phase9-image.md`；镜像相关结论见 [`docs/test-evidence/phase9.md`](test-evidence/phase9.md) |
| sendfile 大文件数据面 | **方向已定稿、实现未交付** | ADR-006 受控复核 2.12x ≥ 1.5x；默认仍走 httplib 内容提供者 |
| 组合根装配指标注册表 / `MeteredBlobStore` / `GcTask` | **未接通** | 真实进程 `/metrics` 只有 HTTP 族；存储/GC 族在 `tests/hardening/test_metrics_and_gc.cpp` 上验证 |
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
| 开发环境与构建 | [`docs/development.md`](development.md) |

机械检查：

```bash
cmake --build build --target test_operations_doc -j"$(nproc)" && ./build/bin/test_operations_doc
./scripts/check_docs.sh      # D1 链接 / D2 作废数字 / D3 ADR 索引 / D4 阶段表 / D5 门槛编号
```
