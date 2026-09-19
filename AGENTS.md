# AGENTS.md —— 本仓库对 AI 编码代理的硬约束

> **这份文件是给"从零开始的会话"看的。** 它不重复设计细节，只写：
> ① 不可违反的约束；② 会被机械检查的规则；③ 我们**已经踩过**的坑及其规避方式；
> ④ 真相在哪里。
>
> 读完本文件后，按 §6 的文档地图按需深入。**不要**在没有读 `docs/00-final-design.md`
> 与相关 ADR 的情况下改动架构或契约。

---

## 0. 项目与当前状态

**`fssvrcpp`**：C++20 实现的 **OSDU File Service 兼容**后端。
支持集中存储（POSIX）与对象存储（S3 兼容）两种后端，REST 为唯一合规面 + gRPC 作为平台外扩展。

| 项 | 状态 |
| --- | --- |
| 阶段 0（环境与契约可行性） | ✅ 已完成并通过门槛 |
| **阶段 1（分层骨架 + 通用库 + HTTP 传输层）** | ✅ **已完成并通过门槛**（C1.1~C1.15 全部满足：12 个 L1 模块 + `fss_http` 包装层 + 护栏自证 + H-2 防护自证 + ASan/UBSan 全绿） |
| **阶段 2（领域模型 + 15 个端口 + 应用层逻辑）** | ✅ **已完成并通过门槛**（C2.1~C2.9 全部满足：领域模型 + 三个纯策略 + 15 个端口 + 端口契约测试基类与三个内存适配器 + `LocationIssuer`/能力护栏 + **13 个用例与 `ErrorKind` 覆盖率矩阵 13/13**；`ctest -L phase2` 10 测试 / 952 断言；C2.10 的跨实现复用待 P3/P6 实现到位） |
| **阶段 3（集中存储驱动 + 位置仓储 + 数据面）** | ✅ **已完成并通过门槛**（C3.1~C3.12 全部满足：`PosixBlobStore`+增量 SHA-256、`SqliteLocationRepository`+SQL 护栏（闭合 C2.10 SQLite 侧）、`BlockingIoEngine`+`fsync` 分级、自签传输 token+`/v1/transfer` 内核、1 GiB 流式（RSS 增长 2.6 MiB）+32 线程零 busy、`UringIoEngine` 骨架；`ctest -L phase3` 9 测试 / 580 断言） |
| **阶段 4（REST 适配层 + 端到端垂直切片（POSIX））** | ✅ **已完成并通过门槛**（C4.1~C4.11 全部满足：19/19 端点 + `/metrics` + **C4.3 驱动 vendored 上游样例并逐字对齐期望消息** + 组合根护栏（R12 自证）+ 数据面"无整体超时/空闲 408"；`ctest -L phase4` **8 测试 / 1058 断言**；真实 POSIX+SQLite 栈端到端通过；C4.11 长跑 330s PUT 200 且 SHA-256 一致） |
| 阶段 4 后续（**已在阶段 10 接入**） | 组合根接 `config/fss.example.json`：**阶段 10 切片 1/2/3/4/5 已实现**（CLI > env > file > 默认；`--config`/`--set`/`--print-config`/`--once`）。156 个叶子键按**三态**逐键登记：**生效 106 / 拒绝启动 19 / 已读但无效果 31**（`docs/operations.md` §1.2/§1.3；切片 3 接通 9 键、C10.16 接通 1 键 + 2 键改为拒绝启动、**C10.16 续**接通 7 键 + 3 键改为拒绝启动、**切片 4**接通 4 键、**切片 5（C10.17）**接通 `self_signed` 3 键、**切片 6a（C10.18）**接通远端 legal/schema 校验器 6 键 + `auth.remote_entitlements.fail_closed` 1 键改为拒绝启动、**切片 6b（C10.19 / ADR-013 §9）**接通 `events.publisher` 与 `events.webhook.{url,timeout_ms,topic}` 4 键（发布失败**非致命**：连不上/超时/非 2xx 只告警，请求照常 201））；生产环境有"必须 jwt + 必须验签 + 密钥非空"的启动强校验 |
| **阶段 5（对象存储驱动（S3 SigV4））** | ✅ **已完成并通过门槛**（C5.1~C5.10 全部满足：自研 `SigV4Signer` + **AWS 官方向量 5 条逐字节匹配**（另 2 条已知有意不同并附理由）+ `mock_s3.py` **独立验签** + `S3BlobStore` 数据面（libcurl 流式）与**同一套端口契约**跑第三遍 + 分页 >1000 键 + 错误映射（新增 `kStorageAccessDenied`）+ S3 模式端到端（客户端直连存储端点）+ **按配置切驱动**（同一二进制 posix/s3 各跑一遍）+ `ADR-005` 定稿；`ctest -L phase5` **6 测试 / 1737 断言**。**未实测**：真实 S3/MinIO 端到端、分片上传 >5 GiB、退避重试、STS 刷新） |
| **阶段 6（元数据记录语义完整化）** | ✅ **已完成并通过门槛**（C6.1~C6.13 全部满足）：切片 1 `SqliteMetadataRepository`（版本链 + `is_latest` 部分唯一索引 + `data` 列无损往返，元数据契约在 SQLite 上跑第二遍 → 闭合 C2.10 元数据侧）；切片 2 校验和（增量 `Hasher` + RFC/FIPS 公开向量 + 上游语义的**覆写**而非"不符就 400"）；切片 3 12 步序列（统一回滚 + 补发 `datasetDetails` + 第 11 步审计告警 + 7 个故障注入点）；切片 4 `getFileList`（上游三条 fixture 逐字 400）+ **9 个角色常量**与端点映射（含两处"任一角色"、"403 先于 400"）+ `ParseIso8601` 越界校验；切片 5 DMS/Delivery 的**两套键集合**与端到端；切片 6 1 GiB 搬迁（RSS 3.4 MiB < 64 MiB）+ 并发幂等（内存与 SQLite 两连接）+ tmp 名唯一性；切片 7 **GC 租约**（dry-run / 在途不删 / 过期才回收 / 无租约不许删 / 有记录永不删 / 并发不重复删）+ C6.10 回归。`ctest -L phase6` **9 测试 / 2578 断言**。**未做（已登记，不阻塞判据）**：`.tmp_*` 与 transfer token 清理（P9 C9.25）、GC 调度/领导者选举/指标（P9）、PG 版 `ILeaseRepository`、远端 Storage Service 仓储（ADR-004 列为可选）、组合根改接 SQLite 元数据仓储、GC 的数据库时钟（P6-D18） |
| **阶段 7（gRPC 适配层 + 双协议等价性）** | ✅ **已完成并通过门槛**（**C7.1~C7.10 全部满足**）：切片 1 = 契约 §5 的**唯一权威表**下沉 L3（HTTP/gRPC 两适配器从同一张表派生）+ `app::CallerFromHeaders` + C7.7 proto 隔离护栏；切片 2 = **14 个一元 RPC** + `app::wire_shapes`（两条协议共用的线上形状）+ 契约 §6 等价性矩阵 **12 行（C7.3）** + 自签 URL 的 token 解码比对（C7.4，含 4 条反向测试）+ `json_name` 对齐（C7.5）；切片 3 = **3 个扩展 RPC**（`UploadFile` 客户端流 / `DownloadFile` 服务端流 / `ServerSideCopy`）+ 组合根**同时开两个端口**（`FSS_GRPC_PORT`）+ C7.6（1 GiB/0 字节/中途取消/慢客户端）+ C7.8（**真实二进制**上并发混合协议）+ C7.10（区间读 == HTTP `Range`，端口级记账证明无整文件读）；`ctest -L phase7` **6 测试 / 5378 断言**。**未做（已登记，不影响判据）**：真实 S3/MinIO 上的流式 RPC 端到端、多租户并发流控调优 |
| **阶段 8（认证授权与多租户）** | ✅ **已完成并通过门槛**（**C8.1~C8.8 全部满足**）：切片 1 = 本地 JWT 授权器（HS256 + `exp`/`nbf`/`iss`/`aud` + 角色 claim ∪ 静态角色表 + **租户绑定**，fail-closed）+ 路由级鉴权预检（`RouteAuthTable()`，鉴权先于 DTO 解析，未登记路由 fail-closed）+ 配置强校验 + `/v2/info` 的 `authMode`；切片 2 = **跨租户隔离**（两租户：跨租户头 → 403、B 读/删/列 A 的记录 → 404/空集、签名 URL 绑对象）+ **远端 Entitlements**（超时/5xx/坏 JSON/连不上/未配地址 → 一律 503）；切片 3 = **审计覆盖**（`AuditGuard` RAII：15 个受保护用例的成功/失败两侧 + actor/对象/结果/时间/correlation-id）+ 事件操作名表（契约 §4.6）+ **multi 模式 5 条启动校验**（含正例）+ `app::ClockSkewGuard`（参考时钟是唯一时间源；快/慢钟超容忍 → fail-closed）；`ctest -L phase8` **6 测试 / 1323 断言**。**未做（已登记）**：PG 版仓储/租约/数据库时钟与 `deployment.mode=multi` 的运行形态（P9；组合根当前对该模式**拒绝启动**）、RS256/JWKS、真实 Entitlements 联调、`kid` 多密钥轮换 |
| **阶段 9（硬化与交付）** | ✅ **已完成并通过门槛**（10 个切片要点，C9.1~C9.13/C9.15/C9.16/C9.25 全部满足；**C9.31 为 P9 补交，P10 期间完成**）：① **C9.1 并发**（100 并发 JSON 零 5xx；8 并发大文件逐路 SHA-256 + 存储侧 checksum 三方一致）；② **C9.2 故障注入 + 恢复 ≤30 秒**（503/500/502 + 非致命依赖；`mock_entitlements --fail-file` 注入与恢复）；③ **C9.3 六类资源上限**各一条"被拒绝" + 对照；④ **C9.6 指标**（`/metrics` 格式合法、值真的动过、不含 secret；**真实进程**已接入存储计量 —— 见 P9-D10）；⑤ **C9.5/C9.25 GC 清理**（`IBlobStore::remove_temp_files`：够旧删、在途保护且计数可观测、被位置记录"引用"也照删）；⑥ **C9.4/C9.11/C9.13/C9.15 容量基线**（独立进程 + 互不重叠绑核 + 每点位 3 次取中位数；`bench/capacity_bench.cpp` + `scripts/bench_baseline.sh` + `docs/appendix/capacity-baseline/BASELINE.tsv`）；⑦ **C9.12/C9.16 ADR-006 定稿**（同一负载生成器受控复核 `sendfile` vs httplib = **2.12x / 2.02x ≥ 1.5x → 采纳方向**，实现未交付）；⑧ **C9.7 门槛纳入 P9**（P0~P9 顺序全绿，**总耗时 225 s / 10 阶段**）；⑨ **C9.8 容器镜像**（多阶段 `Dockerfile` + `scripts/verify_image.sh`：readiness 200、强制 jwt、非 root、HEALTHCHECK **全部实测**）；⑩ **C9.9 运维文档 + C9.10 风险收口**（`operations.md`/`runbook.md` + 自动比对测试覆盖 156 个配置键；§16.1 覆盖全部 20 行风险）。修掉 **P9-D01~D10**（测试替身非线程安全；POSIX/S3 时间戳语义；数据面错误码被覆盖；流式 handler 不读体；护栏白名单；**组合根未接指标注册表与日志脱敏**）。`ctest -L phase9` **8 测试 / 696 断言**；`ctest` **81/81**。**未做/未验证（登记）**：组合根当时未接 `config/fss.example.json`（**阶段 10 切片 1/2 已修**：三态 106/19/31，逐键见 `operations.md` §1.3）、PG 仓储/租约与 `mode=multi` 运行形态、sendfile 数据面实现、真实硬件/多进程/容器类判据（C9.14/C9.17~C9.22/C9.24/C9.26~C9.30） |
| **阶段 10（配置面接线）** | ✅ **切片 1/2/3/4/5/6a/6b 全部完成（含 C10.16/C10.17/C10.18/C10.19）**：让 `config/fss.example.json`（带注释 JSON）成为**真配置源**，优先级 **CLI > 环境变量 > 配置文件 > 默认值**（`fss::config::Load` 复用，未重写加载器）。**切片 1（C10.1~C10.8）**：`--config`/`FSS_CONFIG` + `--set` + `--print-config`（脱敏 + 逐键来源）；优先级与来源在横幅逐键可见；`server.http.*`/`storage.*`/`observability.*` 接进真实进程；`production` 强校验（必须 jwt + 验签 + 密钥非空）；自证：去掉配置文件层 → 8 用例失败。**切片 3（C10.13~C10.15）**：C10.13 审计 fail-closed（`UseCasePorts.audit_fail_closed` + `AuditGuard::Success()` 返回 `Result` + `FSS_AUDIT_FAULT_INJECT` 故障注入接缝）；C10.14 接通（`metadata/location.sqlite.busy_timeout_ms`、`metadata/location.sqlite.journal_mode`（WAL|DELETE，TRUNCATE→78）；**切片 4 补全** `metadata.sqlite.{journal_mode,synchronous}` 与 `location.sqlite.synchronous`）；C10.15 `auth.jwt.roles_claim`+`auth.local_roles.*`+`server.grpc.enabled`。**C10.16 已完成**：`partition.file.opendes.max_file_bytes` 超限上传 → 413、恰好等于上限 → 200（R16 正例）；`allowed/default_checksum_algorithm` 非法 → exit 78。三态 **106/19/31**（切片 6b 后）。**切片 2（C10.9~C10.12）**：① **C10.9** 组合根**真的装配 `GcTask`** + 后台周期调度（`gc.interval_seconds`，第一轮立即跑；`gc.enabled=false`/`interval<=0` → 不启动并在横幅说明）；`gc.dry_run`/`require_lease_expiry`/`staging_ttl_hours`/`orphan_grace_hours` 生效；SIGINT/SIGTERM → 统一退出路径（先 `Stop()`+`join`，绝不留下 joinable thread）；`fss_gc_*` 在**真实进程** `/metrics` 可见；新增 `--once`（跑一轮 GC 退出 0）；单实例用 `src/infra/location/memory/memory_lease_repository.h`（PG 版未交付）；② **C10.10** `config/fss.example.json` 作为 `--config`（只覆盖路径/端口/密钥）**真的启动成功且 readiness 200**；改坏一个键 → exit 78；③ **C10.11** 16 个未实现能力的非默认值 → **拒绝启动（exit 78 + 「未实现 + 下一步」）**；`operations.md` 逐键改为**三态**：**生效 72 / 拒绝启动 16 / 已读但无效果 68**（合计 156）；④ **C10.12** `expiry.default`/`expiry.max` 接 `app::ExpiryPolicy`（作用于签发 URL 的 TTL；超上限**静默夹紧**、边界通过、非法仍 400 固定消息）。新测试 `tests/integration/test_config_wiring.cpp`（**31 用例 / 692 断言**，含 C10.16 续、切片 4 与切片 5；§10.6 父代理加固后 +3 断言）+ `tests/unit/test_transfer_token_key_id.cpp`（C10.17 codec 边界）+ `scripts/verify_config_wiring.sh`（**54 条断言**）；`run_all_gates.sh` 纳入 P10（全绿）；`ctest` **77/77**。**切片 5（C10.17）**：`self_signed.key_id` 进被签名 token 载荷（解码侧 fail-closed，换 id → 旧 URL **401**）、`self_signed.{default_ttl_seconds,max_ttl_seconds}` 作**自签分支的 TTL 上界**（`expiry.*` 语义不变，C10.12 用例不动）→ +3 键生效；**未交付**：多密钥轮换（ADR-009:227 的"多 key 并存"）。 **切片 6a（C10.18 / ADR-013）**：新增 L2 `src/infra/legal/remote_legal_validator.{h,cpp}` 与 `src/infra/schema/remote_schema_validator.{h,cpp}`（与 `RemoteEntitlementsAuthorizer` 同一套写法：`CURLOPT_NOSIGNAL`/`FOLLOWLOCATION=0`/连接超时+整体超时/**所有依赖故障 → `kUnavailable`（503）**/`Ready()`+`NotReadyReason()`）；组合根按 `legal.validator`/`schema.validator` 选 `noop`（默认，行为逐字不变、**不发请求**）或 `remote`（`base_url` = **完整端点 URL**，不追加路径；空 → exit 78）；`200+{"valid":true}` 通过、`{"valid":false,"message":M}` → **400 带 M**、其余（连不上/超时/非 200/非 JSON/缺 valid）→ **503 且无残留**（校验在用例第 3c 步、持久化之前）；`tests/tools/mock_validators.py` + `tests/framework/mock_validators.h`（`--observe-file` 断言请求体形状与"有没有发请求"）+ `tests/integration/test_remote_validators.cpp`（**8 用例 / 464 断言**）；**未交付**：与真实 Legal/Schema 服务联调、调用方身份透传（端口签名无 bearer）、校验缓存、重试退避。 **切片 6b（C10.19 / ADR-013 §9）**：新增 L2 `src/infra/event/webhook_event_publisher.{h,cpp}`（`NOSIGNAL`/`FOLLOWLOCATION=0`/连接超时 `min(1000,timeout_ms)`+整体超时/2xx 判定/`Ready()`+`NotReadyReason()`），组合根三分支 `log`（默认，行为逐字不变）/ `webhook`（出站 POST，**发布失败非致命**）/ `none`（内联 `NoopEventPublisher`，**显式关闭**），`webhook` + 空 `url` → exit 78；`PublishStatus` 补 `record_id`；`tests/integration/test_webhook_publisher.cpp`（**6 用例 / 330 断言**，含非致命三态 + 记录真的建出来）+ `mock_validators.py --mode webhook` 的 `bodies` 列表；**未交付**：异步有界发布队列、重试退避、投递保证、真实消息总线联调。**未做/降级（如实登记）**：**切片 4** 之后仍「已读但无效果」的是 `*.sqlite.{max_write_concurrency,group_commit*}`（单连接 + 互斥、无组提交实现）；**C10.16 续**：为 `storage.posix.{atomic_write,dir_mode,file_mode,fadvise_random,fadvise_dontneed_after_large_read}` 与 `partition.file.opendes.{staging,persistent}_container` 在 `PosixBlobStoreOptions`/`PartitionConfig` 上加真实字段并接通（+7 生效）；`storage.posix.{group_commit_max_batch,sync_dir_after_batch}`（ADR-008 的 P4 未实现）与 `partition.file.opendes.storage_driver`（驱动冲突）→ 拒绝启动（+3）；`leases.*`（除 `enabled` 的守卫）/`leader_election.*`/`large_file_plane.*`/`proxy_mode` 等 **31 键已读但无效果**（逐键理由+下一步见 `operations.md` §1.3.3）；`storage.posix.durability=never` 只能经旧别名；`io_engine=uring` 即使内核可用也拒绝（ADR-010 U1~U4）。**P10 期间补交 P9 的 C9.31**：GC 的按需 HTTP 端点 `POST /v2/gc:run` + `GcTask` 单飞护栏（**不加配置键**，三态 106/19/31 不变；证据 `docs/test-evidence/phase9.md` §12） |
| 已有产品代码 | `src/common/`（L1 十三个模块，含 `metrics/`）+ `src/domain/model/`（`dataset--File.Generic` 全字段模型）+ `src/domain/contract/`（契约 §5 的错误投影表）+ `src/domain/ports/`（15 个端口）+ `src/app/services/`（`expiry_policy`/`kind_validator`/`object_key_policy`/`location_issuer`/`clock_skew_guard`）+ `src/app/usecases/`（13 个用例）+ `src/infra/blob/posix/`（POSIX 驱动）+ `src/infra/blob/s3/`（SigV4 + S3 驱动）+ `src/infra/location/sqlite/`（SQLite 位置仓储）+ `src/infra/metadata/sqlite/`（SQLite 元数据仓储）+ `src/infra/io/`（I/O 引擎 + 落盘接缝）+ `src/infra/auth/local/`（本地 JWT 授权器）+ `src/infra/auth/remote/`（远端 Entitlements 授权器，P8）+ `src/infra/legal/`（远端法务校验器，P10 切片 6a）+ `src/infra/schema/`（远端 schema 校验器，P10 切片 6a）+ `src/infra/transfer/`（自签 token + 数据面内核）+ `src/infra/{blob,location,metadata}/memory/`（内存适配器）+ `src/adapters/http/`（REST 适配层：错误映射 + DTO + 路由）+ `src/adapters/grpc/`（gRPC 适配层：`FileServiceAdapter` + 错误映射 + proto↔领域 DTO + `grpc_streaming_io`）+ `proto/osdu/file/v1/file_service.proto`（RPC 契约，扩展面）+ `src/main/server_main.cpp`（组合根）+ `tests/framework/{port_contract.h,fake_ports.h}`（契约测试基类 + 端口替身）+ `tests/hardening/`（P9：并发/故障注入/资源上限/指标与 GC/数据面错误语义） |
| 当前可运行的验证 | `./scripts/check_docs.sh`（D1~D5）· `ctest -L phase0`（55 断言）· `scripts/verify_guard.sh`（护栏自证）· `scripts/verify_http_hardening.sh`（H-2 防护自证）· `scripts/verify_link_graph.sh`（C2.1 链接图，含越层注入自证）· `scripts/verify_capability_guard.sh`（C2.7 能力护栏自证）· `scripts/verify_composition_root.sh`（C4.9 组合根护栏自证）· `scripts/verify_driver_switch.sh`（C5.9：同一二进制 posix/s3 各跑一遍）· `scripts/verify_transfer_no_timeout.sh`（C4.11 的 ≥5 分钟慢传输实测，非日常门槛）· `ctest -L phase1`（14 测试 / 5017 断言）· `ctest -L phase2`（10 测试 / 996 断言）· `ctest -L phase3`（9 测试 / 594 断言）· `ctest -L phase4`（8 测试 / 1060 断言）· `ctest -L phase5`（6 测试 / 1749 断言）· `ctest -L phase6`（9 测试 / 2586 断言）· `ctest -L phase7`（6 测试 / 5378 断言）· `ctest -L phase8`（6 测试 / 1323 断言）· `ctest -L phase9`（8 测试 / 696 断言；含 C9.31 的 `tests/integration/test_gc_endpoint.cpp` 与 `tests/unit/test_gc_task_single_flight.cpp`）· `scripts/run_sanitizers.sh`（ASan+UBSan+LSan，phase0~7 全绿）· `scripts/bench_logging.sh`（日志热路径基准）· `scripts/bench_baseline.sh`（容量基线；`--save`/`--check`）· `scripts/bench_sendfile_ab.sh`（ADR-006 的 A/B 复核）· `scripts/verify_image.sh`（C9.8：构建镜像 + readiness + 强制 jwt + 非 root + HEALTHCHECK）· `ctest -L phase10`（4 测试二进制 / 48 用例 / 覆盖 C10.1~C10.19；含 `tests/integration/test_webhook_publisher.cpp` + `tests/tools/mock_validators.py --mode webhook` 与 `--observe-file` 的 `bodies` 列表）· `scripts/verify_config_wiring.sh`（C10.1~C10.12 的真实二进制断言 54 条）· `docs/operations.md` + `docs/runbook.md`（C9.9；`test_operations_doc` 自动比对 156 个配置键 + 三态计数 106/19/31）· `FSS_GATES_WITH_PG=1`（+PG 基建 5 项） |

**新增的硬性约定（改代码前必须知道）**

| 约定 | 说明 |
| --- | --- |
| **配置文件是"带注释的 JSON"**，不是 YAML | 本仓库不引入 YAML 解析器（镜像 pool 只有 yaml-cpp 0.5.x 的老 API）。用 `json::ParseWithComments`；**不支持尾随逗号**。样例：`config/fss.example.json` |
| **测得的"可注入"接口声明在 L1** | `IClock` 在 `src/common/time/clock.h`、`IIdGenerator` 在 `src/common/ids/id_generator.h`（理由见 `docs/02-design.md` §5.2 的 ⓛ 标注） |
| **测试主入口只有一个** | `tests/framework/catch_main.cpp`，由 `fss_add_test` 统一注入；测试文件里**不要**写 `CATCH_CONFIG_MAIN` |
| **L1 每个子目录一个独立 CMake 目标** | 新增 L1 模块要同时改 `src/common/CMakeLists.txt` 与 `tests/CMakeLists.txt` |

---

## 1. 环境硬约束（不可协商；先验证再假设）

| 约束 | 事实 | 后果 |
| --- | --- | --- |
| **无 root** | `sudo` 需要密码 | 依赖一律 `apt-get download` + `dpkg-deb -x`；**不能** `mount`、不能装系统包 |
| **外网受限** | `github.com` 直连不可达；apt 镜像（USTC）可用 | 第三方库只能来自 apt 或镜像 pool |
| **protobuf 3.12.4** | 不支持 `proto3 optional`（需 3.15+） | `.proto` 中**禁止**使用 `optional`；用 message 包裹表达可选性 |
| 编译器 | g++ 11.4.0；**无** `clang++`、**无** `ninja` | 不用需要这些的特性 |
| 可用依赖 | OpenSSL 3.0.2、SQLite3 3.37.2、libcurl 7.81、gRPC 1.30.2、zlib | — |
| 缺失 | boost、aws-sdk-cpp | 设计上不依赖；`third_party/` 有 nlohmann/json + Catch2 + cpp-httplib |

**恢复 `third_party/` 的方式见 `third_party/README.md`。**
⚠️ `cpp-httplib` **不要**用 Ubuntu 的二进制包（jammy 的 0.10.3 有 `Range` 下溢缺陷 H-1）。

---

## 2. 工作流契约（违反即视为未完成）

### 2.1 每阶段一道门槛，通过才前进

```
P0 环境与契约可行性        ✅ 已完成
P1 分层骨架 + 通用库 + HTTP 传输层   ✅ 已完成（C1.1~C1.15 全部满足）
P2 领域模型 + 15 个端口 + 应用层逻辑   ✅ 已完成（C2.1~C2.9）
P3 集中存储驱动 + 位置仓储 + 数据面   ✅ 已完成（C3.1~C3.12）
P4 REST 适配层 + 端到端垂直切片   ✅ 已完成（C4.1~C4.11 全部满足）
P5 对象存储驱动（S3 SigV4）   ✅ 已完成（C5.1~C5.10 全部满足）
P6 元数据语义完整化      ✅ 已完成（C6.1~C6.13 全部满足）
P7 gRPC 适配层 + 双协议等价性   ✅ 已完成（C7.1~C7.10：17/17 RPC + 矩阵 + 流式 + 双协议并发）
P8 认证授权与多租户   ✅ 已完成（C8.1~C8.8：JWT + 路由预检 + 跨租户 + 远端 Entitlements + 审计 + multi 校验 + 时钟偏差）
P9 硬化与交付   ✅ 已完成（并发 C9.1 + 故障注入 C9.2 + 资源上限 C9.3 + 指标 C9.6 + GC C9.5/C9.25 + **按需 GC 端点 C9.31（P10 期间补交）** + 容量基线 C9.4/C9.11/C9.13/C9.15 + ADR-006 C9.12/C9.16 + 门槛并入 C9.7 + 镜像 C9.8 + 运维文档 C9.9 + 风险收口 C9.10；`ctest -L phase9` 8 测试 / 696 断言）
P10 配置面接线   ✅ 切片 1/2/3/4/5/6a/6b 全部完成（--config/--set/--print-config/--once + 优先级与来源可见 + server.http/storage/auth/observability 接通 + 生产强校验 + GC 周期调度与 expiry 接通 + 16 个未实现键显式拒绝 + **C10.16**：`partition.file.opendes.max_file_bytes` 超限 → 413、校验算法非法 → exit 78 + 156 键三态 **生效 106 / 拒绝启动 19 / 已读但无效果 31** + 新测试 31 用例；**C10.16 续**：`storage.posix.*` 5 键与 `partition.file.*` 容器名 2 键生效、3 键改为拒绝启动；**切片 4**：`server.http.transfer_max_body_bytes`、`metadata.sqlite.{journal_mode,synchronous}`、`location.sqlite.synchronous` 生效；**切片 5（C10.17）**：`self_signed.{key_id,default_ttl_seconds,max_ttl_seconds}` 生效 —— `key_id` 进签名载荷并在解码侧 fail-closed（换 id → 旧 URL 401），两个 TTL 键是自签分支的上界（`expiry.*` 语义不变）；**未交付**：多密钥轮换；**切片 6a（C10.18 / ADR-013）**：远端 legal/schema 校验器 6 键生效（+真实进程 fail-closed 五态用例），`auth.remote_entitlements.fail_closed` 更正为拒绝启动（模式相关）；**切片 6b（C10.19 / ADR-013 §9）**：`events.publisher` 与 `events.webhook.{url,timeout_ms,topic}` 4 键生效（**发布失败非致命**：连不上/超时/非 2xx 只告警，请求照常 201 且记录真的建出来；`none` 显式关闭、`log` 默认不发请求）；**未交付**：真实 Legal/Schema 联调、身份透传；异步有界发布队列、重试退避、投递保证；**期间补交 P9 的 C9.31**：GC 的按需端点 `POST /v2/gc:run` + `GcTask` 单飞护栏（不加配置键、三态 106/19/31 不变））
```

- **门槛未通过 → 不得开始下一阶段的任务**（包括"顺便先写一点"）。
- 每阶段结束把**命令、输出摘要、结论、以及本阶段发现并修复的缺陷**写入
  `docs/test-evidence/phaseN.md`。
- 新增阶段时，把编号加入 `scripts/run_all_gates.sh` 的 `IMPLEMENTED_PHASES`。

### 2.2 改动决策时必须同步的东西

| 改了什么 | 必须同时做 |
| --- | --- |
| `docs/03-api-contract.md`（契约） | 更新对应契约测试（**先让测试失败**），并在证据文件说明上游依据（commit/文件/行号） |
| 协议（proto / JSON 字段） | 更新 `proto/`、契约文档 §4 映射表、以及双协议等价性测试 |
| 新增 `ErrorKind` | 更新契约 §5 映射表 + `test_error_equivalence`（否则测试因空缺失败） |
| 架构决策 | 新增/修订 `docs/adr/ADR-00N-*.md`，并在 `docs/00-final-design.md` §4 表与 `docs/02-design.md` §17 索引登记 |
| 配置项 | 同步 `config/fss.example.json` 与 `docs/operations.md`（有自动比对测试） |
| **推翻既有结论** | 在 `docs/00-final-design.md` §5 记录"旧结论 → 现状 → 影响"，**不要静默改掉** |

### 2.3 收工前必做

```bash
./scripts/check_docs.sh          # 文档链接 / 过期数字 / ADR 索引 / 阶段表一致性
./scripts/run_all_gates.sh       # 全部已启用阶段门槛
```

---

## 3. 铁律（每条都有代价；违反会重复已经付过的学费）

| # | 铁律 | 依据 |
| --- | --- | --- |
| **R1** | **关键断言必须配"自证对照"**：能用错误实现复现问题的对照，必须写。否则无法区分"实现正确"与"测试无效" | 已抓出 ADR-008 协议 9 对照（11/12 检出）、`003` 的 C3（去掉约束必须复现多赢家） |
| **R2** | **性能测量必须独立进程 + 绑核**。禁止进程内客户端压测 | 曾得出"并发 32 比 4 慢 15 倍"的**方向性错误结论** |
| **R3** | **性能数字必须标注协议与安全性** | 曾用"不安全协议的 58,741 文件/秒"当设计依据 |
| **R4** | **无法区分的实验必须如实标注为"无结论"**，不得当作证据 | O_DIRECT 读回、页缓存饱和实验都属此类 |
| **R5** | **幂等/唯一约束必须建在"幂等键"上**，不能建在随机主键上 | 曾在 UUID 主键上建约束 → 幂等完全失效 |
| **R6** | **部分唯一索引的谓词必须含业务语义** | `ux_mr_source` 缺 `is_latest` → 阻断合法版本链 |
| **R7** | **不要从一个局部现象推出全称结论**；ADR 的"备选方案"表必须真有备选 | ADR-002 初版从"一个二进制包不可用"推出"自研整个 HTTP 栈" |
| **R8** | **不要在部署层假设之上做设计**；部署约束（seccomp / 内核 / NFS 语义）必须在**目标环境**验证 | io_uring 在容器默认 seccomp 下 `EPERM` |
| **R9** | **测试的前置条件要显式断言**，并给出可执行的修复指令 | 曾因索引被误删而得到费解的 SQL 报错 |
| **R10** | **清理顺序**：先删数据，再重建唯一约束 | 曾因残留重复行导致 `CREATE UNIQUE INDEX` 失败 |
| **R11** | **可选加速能力必须先有无依赖的默认路径**，且探测结果要可见（`/v2/info` + 指标） | ADR-010 |
| **R12** | **所有具体实现只能在组合根 `src/main/` 创建**；能力分支只允许出现在 `LocationIssuer` | `docs/02-design.md` §4 |
| **R13** | **"记录在先、实现在后"的文档必须随实现同步更新**：AGENTS.md 的状态表、计划的阶段标记、证据文件，都是"会被读的真相" | 本轮发现 AGENTS.md 仍写着"业务代码尚不存在""13 个端口"（当时已过时） |
| **R14** | **不要把 `Type name(Type(other))` 当构造**：C++ 会解析成函数声明（most vexing parse），报错信息完全看不出根因 | P1-D04：`path root_p(std::string(root))` → `weakly_canonical(path (&)(std::string), error_code&)` |
| **R15** | **凡是写进文档的"能力型"约定（"支持注释""支持流式""支持断点续传"），必须有测试真的执行过那条路径**；只有描述没有测试的约定，等同于不存在 | P1-D07：全仓 4 份文档 + 配置样例都写着"带注释的 JSON"，而 `ParseWithComments` 因**位置参数传错**从未生效，8 个 json 用例没有一条碰过它 |
| **R16** | **校验/判定逻辑要有"正例"断言**：只断言"错误配置被拒"无法区分"校验正确"与"校验恒真" | P1-D08：`multi` 模式的三条跨字段校验对布尔值恒判失败，写对的配置也被拒 —— 只有补上"补齐后必须通过"才暴露 |
| **R17** | **调用第三方 API 时先核对形参顺序**（尤其是"位置参数 + bool"），并把顺序写进代码注释 | P1-D07：nlohmann 迭代器重载是 `parse(first,last,cb,allow_exceptions,ignore_comments)`，第 4 个不是 `ignore_comments` |
| **R18** | **手写的安全敏感代码（转义/校验/解析）必须有一个"参照实现"并做**等价比对**，能穷举就穷举**；只靠"看起来对"的用例不够 | P1-D13：手写 JSON 转义的第一版把"结构不完整"与"范围越界"混为一谈，穷举 47 万个字节序列立刻抓出两处不一致 |
| **R19** | **优化前先算"预算占比"**：把 ns/操作 换算成"峰值负载下的单核百分比"，再决定值不值得优化或引依赖 | P1-D11：日志从 2838 → 1084 ns/条（2.3% 单核）；再往下抠的收益 <1% 单核，故**主动停止**优化 |

---

## 4. 已知陷阱清单（我们自己踩过的，逐条给出规避方式）

### 4.1 推理与决策类

| 陷阱 | 规避 |
| --- | --- |
| **过度外推**：一个包/一个版本的问题 → "重写整个组件" | 先问"换个分发形态/版本能不能解决"；把结论限定在证据范围内 |
| **单候选 ADR**：只评估 1 个方案就下全称结论 | ADR 必须列 ≥3 个候选，并标注哪些**未实测**及理由 |
| **陈旧数字复活**：被推翻的性能/结论仍在别处被引用 | `./scripts/check_docs.sh` 会扫描并报错；推翻结论必须写进 `00-final-design.md` §5 |
| **把"能跑"当"安全"**：用不安全协议的数字支撑设计 | 先写不变量（R1/R2 那类），再测性能；协议与安全性一并标注 |

### 4.2 测量类

| 陷阱 | 规避 |
| --- | --- |
| 进程内客户端 + 服务端抢同一批核 | 负载生成器**独立进程**，`taskset` 绑不同核 |
| 稀疏文件代表真实磁盘 | 只用它验证**语义正确性**；性能必须真实存储 |
| 页缓存/内存带宽把差异抹平 | 先确认测试没有被别的瓶颈饱和；饱和了就标注为无结论 |
| httplib 默认 `TCP_NODELAY=false` | 服务端与客户端都设 `true`（否则 950x 损失） |
| 线程池当成"CPU 倍数" | 它是**并发连接上限**；一条 keep-alive 连接占一个线程直到结束 |
| `keep_alive_max_count` 默认 100 | 原始 socket 客户端要处理服务端主动断连 |

### 4.3 实现与测试代码类

| 陷阱 | 规避 |
| --- | --- |
| `set_content_provider` 回调在 handler 返回**之后**执行 | 不要**引用**捕获 handler 的局部变量（用 `shared_ptr` 按值捕获） |
| 提前 `return` 留下 joinable `std::thread` → `terminate` | 统一退出路径：先 `stop()` 再 `join()` |
| `void`/异步状态用固定 `sleep` 等待 | **轮询**实际条件（如 `pg_locks` 归零） |
| `PID="$(spawn)"` 取后台任务 PID | 命令替换会创建子 shell，任务成孤儿，`wait`/`kill` 失效 → 用**全局变量**在函数内赋值 |
| 同一个"会话级"检查拆成多次进程调用 | 会话级状态（PG advisory lock）必须在**同一次调用/同一会话**内完成检查 |
| `INSERT ... RETURNING` 写在 PL/pgSQL 里 | 必须有目的地（`INTO`/`PERFORM`）或去掉 `RETURNING` |
| `gcc` 编译 `.c` 却在用 C++ 语法（`::open`） | 确认语言与文件后缀一致 |
| `env` 输出引用未定义变量 | 用 `${VAR:-}`，否则在 `set -u` 下报 unbound variable |
| 配置只在首次初始化时写入 | **每次启动都重写**配置片段（否则改配置后 `restart` 不生效） |
| `is_X() ? v.get<X>() : fallback` 这类"宽松取值"助手 | 它会把**类型不匹配静默折叠成缺省值**。若该值参与**判定**（不只显示），校验会恒真/恒假 → 用 `RenderScalar()` 显式渲染标量（P1-D08） |
| 位置参数里夹着 `bool` 的第三方 API | 先核对形参顺序再写，并把顺序留在注释里（P1-D07；nlohmann 的 `ignore_comments` 是第 **5** 个参数） |
| 用**一套**分隔符集合解析两种语法形态 | `Authorization: Bearer x` 与 `?sig=x&k=y` 的"值到哪里结束"不同；一套规则必然让其中一种静默失效（P1-D09：只打码了 `Bearer`，token 泄漏且输出里**看起来**有 `***`） |
| 键名匹配只做大小写折叠 | 还要归一化分隔符（丢 `_`/`-`），否则 `secretKey` 不命中 `secret_key` 规则（P1-D10）。方向必须选"宁可多打码" |
| **"结构不完整"与"范围越界"混为一谈** | 多字节编码的校验是**逐字节按范围**做的，越界那个字节要重新处理，已通过的续字节才跳过（P1-D13） |
| `str.replace('', x)`（Python 脚本改文件时 old 串取空） | 会把 x 插到**每个字符之间**，文件当场报废。取区间前先断言区间非空；改完必须能编译；区间取完先 `print(repr(...))` 确认 |
| `namespace fss::x {` 后面配**两个**闭合 `}` | `namespace A::B {}` 是单层嵌套声明，只需**一个** `}`。多写一个的报错是 `expected declaration before '}' token`（指在文件末尾），根因在文件开头 |
| 把 ASCII 双引号写进 C++ 字符串字面量 | `"…"形状"…"` 会把字符串截断，报 `expected primary-expression`。中文文案里用「」或转义（本仓库已踩两次：`core_schema.cpp`、`test_net.cpp`） |
| 用 `is_invocable` 检查"参数类型是否被禁止" | 隐式转换会让它恒为真/假命题（`uint32_t` → `uint64_t`）。要钉签名就用**成员函数指针类型比对**（P2-D04） |
| 测试期望值直接照抄直觉而不查 RFC | `= : @` 是 RFC 3986 的 sub-delims，**路径段里合法**，不该被百分号编码。先查 RFC 再写断言，否则会把正确实现"改成错的" |
| **第三方库的"默认值"不适配我们的场景**（不止是缺陷） | `listen backlog` 硬编码 5 → 50 并发时丢 SYN、客户端 1s 重试，实测 1436 ms（本该 414 ms）。这类问题**只有"真实并发 + 阻塞 handler + 计时"测得出来**（P1-D16） |
| 库在**解析阶段**返回的错误会**跳过路由** | `pre_routing_handler` 拦不到，唯一入口是 `error_handler`（P1-D14）。判断依据：访问日志里**没有**这条请求 |
| 认为"库在 A 路径做了 X"⇒"B 路径也做了 X" | 缓冲路径与流式 `ContentReader` 路径的行为**不同**：走私检查、超限语义都不一样（P1-D17，H-2 的同类教训第二次应验） |
| 一条路径上有两个"归还/递减点" | 要么互斥、要么加"结束后计数归零"断言；计数变负会让**背压静默失效**（P1-D18 实测 -1） |
| **sanitizer 构建沿用 `-j$(nproc)`** | 插桩后的 Catch2/httplib 单 TU 编译可达 1 GB+ 内存，16 核机器上会 **OOM 杀掉 cc1plus**（实测 `Killed signal terminated program cc1plus`）。用 `scripts/run_sanitizers.sh`（默认 `-j4`，可用 `FSS_ASAN_JOBS` 调） |
| **把临时 `std::string` 的 `c_str()` 存下来稍后用** | 临时的生命周期只到该完整表达式结束，之后解引用 `end` 就是读已析构存储（P1-D19，ASan 报 `stack-use-after-scope`；普通构建"碰巧对"所以更难发现）→ 先绑到具名变量 |
| **`-fsanitize=address` 会改变"头文件定义布局 + 预编译库"的 ABI** | gRPC 的 `port_platform.h` 在 `__SANITIZE_ADDRESS__` 下定义 `GRPC_ASAN_ENABLED`，`sizeof(grpc::ClientContext)` 从 **504 变 512** → 与不带 ASan 的 `libgrpc++.so` 布局不一致 → 垃圾值/巨型 malloc（P1-D20）。修法：`-DGRPC_ASAN_SUPPRESSED=1`（已挂在 `PkgConfig::GRPCPP` 的 INTERFACE 上） |
| **停在 sanitizer 报的第一个症状上** | 关掉 `bool` 检查只是让报告换张脸（变成 `allocation-size-too-big`）。用"打印 `sizeof` 与内部状态"把猜测变成事实，才找到真正原因（P1-D20） |
| **把"列表"压成单词再遍历**（`tr -d ' '` 处理 `(0 1 2)` → `012`） | 遍历到的元素根本不是你以为的元素。`run_sanitizers.sh` 因此把标签推成 `phase012`（不存在），"C1.7 全绿"其实是**空集合**：只有 phase0 跑过（P2-D07）。修法：按数字切词，并**断言每个标签匹配到的测试数 > 0**（与 C2.1 的"静态库 `link.txt` 是空证据"同类） |
| **手写含内嵌 NUL 的字面量字节数**（`std::string("a\0b", N)`） | `N` 数错就会读字面量之外的字节。ASan 报 `global-buffer-overflow`，而普通构建"碰巧能过"（P2-D08）。改用不会数错的构造（`std::string("a") + '\0' + 'b'`） |
| **markdown 里写真的 NUL 字节**（想表达 `` `'\0'` `` 却敲成 0x00） | 文件会被工具判定为二进制，编辑器/编辑工具拒绝修改，grep 只回一句 "binary file matches"。写转义文本 `\0`，不要写裸字节（P1 证据文件里踩过，P2 清理） |
| **打开 `sqlite3_extended_result_codes` 后仍按主码比较 `rc`** | `sqlite3_step` 此时返回**扩展码**（唯一约束是 `SQLITE_CONSTRAINT_UNIQUE=2067`，不是 `SQLITE_CONSTRAINT=19`）→ 冲突被当成 500。比较必须写成 `(rc & 0xFF) == SQLITE_CONSTRAINT`（P3-D03） |
| **把第三方类型的前向声明写进自己的 namespace / 成员签名用 `struct X*`** | `namespace fss::infra { struct sqlite3; }` 声明出的是 `fss::infra::sqlite3`；成员签名里的 `struct sqlite3_stmt*` 也会**就地**在最近命名空间声明新类型。报错是 `cannot convert fss::infra::sqlite3_stmt* to sqlite3_stmt*`。前向声明放全局，签名用 `::sqlite3_stmt*`（P3-D04） |
| **`FSS_TRY(var, expr)` 是"声明 var"，不是"给 var 赋值"** | 写成 `FSS_TRY(signed_loc, ...)` 时，它展开出的 `auto signed_loc = ...` 会在内层块里**遮蔽**外层变量 —— 值丢了却编译通过（`-Wshadow` 只是告警）。要用已存在的变量就先 `FSS_TRY(tmp, ...)` 再 `x = std::move(tmp)`（P2-D09） |
| **命名空间名与第三方库同名会遮蔽全局名字** | `namespace fss::adapters::grpc` 内部写 `grpc::Status` 会解析到**本命名空间**，报 `'Status' in namespace 'fss::adapters::grpc' does not name a type`。库类型一律写全局限定 `::grpc::Status` / `::grpc::StatusCode`（P7-D01）。`namespace fss::adapters::http` 用 `httplib::` 不出问题，**只有与库同名的命名空间才会踩** |
| **测试框架里比较"每次都会变"的生成型值** | 签名 URL 的 path 内嵌密文 + 随机 nonce，字面比较**永远不相等**；proto3-JSON 又**省略默认值字段**（`"Number":0` 不出现）。判据必须是"解码到同一语义"或"非默认值必须出现"，并配**反向测试**防止判据恒真（P7-D02/P7-D05） |
| **`set -euo pipefail` 的脚本里用 `\| head -N` 截断** | `head` 读够行数就退出并关闭管道，上游命令收到 **SIGPIPE**（退出码 141）→ `pipefail` 让**整条管道**变非 0 → `set -e` 中止脚本。症状是"门槛因为一个与检查内容毫不相干的原因失败"：`verify_link_graph.sh` 的 `find … \| head -50` 在 `link.txt` 从 49 涨到 61 个后把"C2.1 通过"变成"C2.1 越层依赖"（P7-D06）。规避：需要"前 N 行"就用 `sed -n '1,Np'`（读完全部输入）或先收集到变量再取；并且**截断型证据的判据不要依赖"随便挑一个"**（挑中合法样例就会误报） |
| **门槛脚本里的"抽样佐证"必须证明它还能失败** | 把"挑第一个匹配文件"改成"遍历全部"之后，要做一次注入自证（伪造一份违规输入 → 必须判失败），否则可能只是把检查变成恒真（P7-D06 §4 的自证） |
| **编号类正则写 `(\d)` 而不是 `(\d+)`：检查器会对两位数阶段"整体失明"** | `check_docs.sh` 的 D4/D5 曾用 `\*\*P(\d)\*\*` / `(C\d\.\d+…)`，且从 `IMPLEMENTED_PHASES` 取号用 `findall(r'\d')`（`10` 被拆成 `1`+`0`，恰好已被 0/1 覆盖）→ **P10 行与 `C10.1~C10.16` 共 16 条门槛从未进入检查范围**，"阶段表一致""无断号"对阶段 10 恒为真（"通过"是空集，与 P2-D07 的 `phase012` 同类；两个方向都失效：标 ✅ 不加门槛查不出、加了门槛不写计划也查不出）。修法：正则一律 `\d+`、`sorted` 加 `key=int`，并让 `--selftest` 用 **P99 / C10.16** 注入锁住回归（`docs/test-evidence/phase10.md` §8） |
| **`if cmd \| tail -N; then` 判断成败会被管道吃掉退出码** | 上一条是"`pipefail` 让检查误失败"，这条是**反方向**：**没有** `pipefail` 时，`if <重要命令> \| tail -2; then` 判的是 `tail`（退出码恒 0）→ **命令失败了却被当成成功**。实测：`git push` 明明报 `GnuTLS recv error (-110)` / `Connection timed out`（rc=128），循环仍打印 `PUSH_OK`，差点把"未推送"写成"已推送"。规避：既要显示"最后 N 行"又要判成败，就先赋值再判退出码 —— `out="$(cmd 2>&1)"; rc=$?; printf '%s\n' "$out" \| tail -N; [[ $rc -eq 0 ]]`，或用 `${PIPESTATUS[0]}`；并且**不可逆动作（push/发布/清理）必须回读远端状态确认**（`git ls-remote`、比对 commit），不能只凭自己打印的 OK |
| **否定式判据（`REQUIRE_FALSE`）必须配"正控"** | 测试里"某东西**不存在**"这类断言，如果路径/名字写错，会**静默恒真**。实测两次：① 切片 6a 的"persistent 侧没有文件"用`exists("opendes-persistent/…")`（容器相对路径），而物理根是 `<storage.posix.root>/blobs` → 在测试进程 CWD 下恒为 false，于是 `REQUIRE_FALSE` 恒真，"无残留"从未被验证；② 切片 6b 写同型断言时才发现（`exists(location)` 同样恒 false）。修法：**同一条路径解析上再加一条正控** —— `REQUIRE(exists(staging_path))` 证明"该存在的确实存在"，`REQUIRE_FALSE(exists(persistent_path))` 才有意义；并把目标判据**反转**跑一次，确认它会失败（`docs/test-evidence/phase10.md` §11.4.2）。与"两条防线重合导致判据无区分力"（§11.4.1）同族 |
| **当机械护栏挡住了"架构上必须存在"的代码时，不要放宽护栏** | C7.7 的护栏规定"除 `adapters/grpc/` 外 `src/` 全树不得出现 `<grpcpp/`"，而 R12 要求**组合根创建** gRPC 服务 → 两者看似冲突。正确解法是"把第三方类型收进适配层"（`grpc_server.h` 只暴露启动/端口/`Shutdown()`），而不是给组合根加豁免 —— 与 `fss_http` 把 httplib 挡在 `common/http/` 内是同一条纪律（P7-D09） |
| **流式入口把"被取消"当成"正常读完"** | 客户端中途断开时 `ServerReader::Read()` 也返回 false，与 `WritesDone` 无法区分。当成 EOF 会让存储层把**截断的**字节 rename 成正式对象 —— 静默数据损坏（P7-D07，1 GiB 用例的 4 MiB 取消场景抓到）。**只查 `IsCancelled()` 还不够**：它的置位时机晚于 `Read()` 的失败（ASan 下必现，P8-D05）→ 正确做法是**显式结束标记**（`UploadFileRequest.end_of_stream`）：没有标记 = 流不完整 = 报错，确定性判定，不依赖竞态 |
| **只在"快"的构建里验证竞态类修复** | "取消当 EOF"的竞态在普通构建里 100% 通过、在 ASan 构建下必现（P8-D05）。竞态/时序类修复必须在**两种构建**下各跑一遍（`scripts/run_sanitizers.sh` 就是第二遍），否则"修好了"只是运气 |
| **proto3 里"段是否存在"必须用 message presence 表达，测试 fixture 也要置位** | `dataset_properties.present` 不置位时，`FillData` 会跳过整段 → 记录经 proto 往返**整段丢失**，表现为"FileSource 不一致"（P7-D08）。proto3 没有字段 presence，**段级**存在性只能靠 message 字段（`has_dataset_properties()`）；测试构造的记录必须显式置 `present = true` |
| **"退化了 >20%" 的判据要分清指标的**极性**与**适用范围** | 第一版把 20% 带宽同时套在"吞吐"和"延迟"上：`p99 4346 → 3185 µs`（**快了 27%**）被判成"退化 -26.7%"而让门槛失败。吞吐类（越大越好）才适用"退化 >20%"，延迟类用**绝对阈值**兜（本机高并发点位的 p99 逐次散布有 ±25%）；**fsync/磁盘类**吞吐在本机（WSL2 虚拟盘）散布 >20%（实测 78.6 / 96.4 files/s），只能**告警**、不能判失败（R4：如实标注"该判据在本环境无判定条件"）。见 `scripts/bench_baseline.sh` |
| **基准里的"请求成功"必须显式前置断言** | 播种器用错了 id 形态（`uploadURL` 返回的 `FileID` vs `POST metadata` 返回的 `dataset--File.Generic:<uuid>`），于是控制面点位**全部 4xx**：`rps=0`、`errs=44094`。若只看"跑完了、有表格"就会把这行当成"0 req/s 的正常结果"。判据必须是 `errs==0 && reqs>0`，播种失败立即中止（R9） |
| **`keep_alive_max_count`（httplib 默认 100）会让"正常断连"计成错误** | 服务端按协议主动关闭长连接是**合法**行为；基准客户端必须**重连并重试同一次请求**（最多 3 次），否则 c16 点位会凭空多出 183 次 `errs`，把协议行为当成服务端故障 |
| **"测试夹具接上了、组合根没接"= 产品里不存在** | 指标注册表/计量装饰器在 `test_metrics_and_gc` 的夹具里接得好好的，**组合根却从不创建它们** → 真实进程 `/metrics` 只有 HTTP 族；日志也一样（夹具里脱敏、组合根用 `LogOptions{}` → 生产日志**不打码**）。修法：P9-D10 在组合根接入并让启动横幅打印 `log redact : 11 个键` / `metrics : 已接入（含存储计量）`。**判据要认"真实进程"，不能只认夹具**（与"组合根没读的配置"同族） |
| **组合根没读的配置 = 不存在的配置** | `storage.posix.durability` / `fsync_threshold_bytes` 早已在 schema 与 `config/fss.example.json` 里，但组合根从未读取它们 → "三档吞吐差异"在**真实进程**上根本无法配置（C9.15 没法测）。默认值还用了 `InMemoryMetadataRepository`（ADR-004 说单实例 = 内置 SQLite）→ 重启丢记录。**文档里有的配置项必须在组合根可达**，否则它只是摆设 |
| **`PRAGMA synchronous` 不落盘** | 与 `journal_mode`（写进库文件头，另开连接可读回）不同，`PRAGMA synchronous` 是**连接级**设置、**不随库文件持久化**：`Open` 之后另开一个 sqlite3 连接读回，只会得到**新连接自己的默认值**（FULL=2），与被测仓储是否真的下发了 OFF/NORMAL/FULL 无关 —— 这种"验证"是恒真的假证据。规避：在两个仓储上加**窄诊断访问器** `AppliedPragma("synchronous")`（在**本连接**上读回），用它做进程内 L2 断言（`tests/integration/test_sqlite_{location,metadata}_repository.cpp` 的切片 4 用例：默认 `"1"`、OFF `"0"`、FULL `"2"`，且白名单外名字被拒）；真实进程侧只断言横幅打印了实际取值 |
| **"写完才算数"的断言不能立刻做** | 客户端 `Finish()` 返回（`CANCELLED`）时服务端线程可能还在 `put` 里；"临时文件必须被清掉"要**轮询实际条件**（本例最多 5 s），固定 sleep 既慢又不稳（P7-D07） |

### 4.4 分布式/并发类

| 陷阱 | 规避 |
| --- | --- |
| 临时文件名不含实例标识 | 必须含 `instance_id` + `pid` + 计数（否则共享存储上**静默串数据**，实测 21/40） |
| GC 用"无元数据记录即删" | 在途上传**恰恰就是**没有记录 → 必须用**租约到期 + 原子领取** |
| 幂等靠应用层 check-then-insert | 必须靠**数据库唯一约束 + `ON CONFLICT` 原子领取** |
| 用 NFS 文件锁做互斥 | 用 PG advisory lock（NFS 锁语义脆弱） |
| 用实例本地时钟做租约/过期判定 | 一律用数据库 `now()`（时钟偏移会误判） |
| 持锁会话里跑长查询 | PG 默认 `client_connection_check_interval=0` 时崩溃的会话**不释放锁** → 持锁会话保持空闲/只做心跳，并设置该参数为 1s |
| 期望 `syncfs` 只影响自己的写入 | 它是**文件系统级**操作；多实例共盘会互相干扰 → 按 partition 分盘 |

---

## 5. 会被机械检查的规则（不要靠记忆）

| 检查 | 执行方式 |
| --- | --- |
| 文档链接有效 | `./scripts/check_docs.sh` |
| 被推翻的数字不再被引用 | 同上（扫描 `58,741` 等已作废值，要求同处有作废标记） |
| ADR 索引完整（无孤儿 ADR） | 同上 |
| 阶段表与 `IMPLEMENTED_PHASES` 一致 | 同上 |
| 门槛编号连续（`C<阶段>.<序号>` 无断号、无重复；阶段/序号按**多位数**解析，P10 与 `C10.*` 必须在范围内） | 同上（D5；检查器自身由 `--selftest` 的 P99 / C10.16 注入自证） |
| 分层依赖方向 | CMake 目标图（编译期）+ 源码检索护栏测试（P1 交付） |
| 自证脚本的**注入残留**不得进入门槛 | `scripts/run_all_gates.sh` 前置检查（`src/` 下 `_*selftest*`/`*_injected*` 文件、`git diff -- src` 里的注入标记；P6-D04） |
| `<httplib.h>` 只出现在 `src/common/http/` | 源码检索护栏 |
| 仓储 SQL 必须带 `partition_id` | 源码检索护栏 |
| PG schema 不变量 | `db/tests/001_verify_invariants.sql`（含"索引键与谓词"的回归断言） |
| 跨会话 advisory lock 行为 | `db/tests/002_advisory_lock.sh` |
| 并发领取幂等（含自证对照） | `db/tests/003_concurrent_claim.sh` |
| io_uring 可用性 | `./scripts/check_io_uring.sh`（退出码：0=可用 / 1=不可用 / **2=环境不具备探测条件 → 无结论**） |

**原则**：能机械化的规则就机械化。写进 AGENTS.md 但无法检查的规则，视为"建议"而非"约束"。

---

## 6. 文档地图（真相在哪里）

| 我想知道 | 看这里 |
| --- | --- |
| **最终方案是什么**（只看一份） | **`docs/00-final-design.md`** |
| OSDU 的真实接口是什么（一手调研） | `docs/01-osdu-research.md` |
| 架构分层、端口、威胁模型、风险登记 | `docs/02-design.md` |
| **接口契约**（实现与测试的唯一基准） | `docs/03-api-contract.md` |
| 阶段划分与逐条门槛判据 | `docs/04-implementation-plan.md` |
| 并发/容量/小文件/大文件/协程 的实测 | `docs/05-capacity-and-concurrency.md` |
| 开发环境、构建、PG 基建、陷阱 | `docs/development.md` |
| **运维**：配置项全表（156 键逐条 + **三态清单**：生效/拒绝启动/已读但无效果）、可观测性、容量、安全 | `docs/operations.md` |
| **故障处置**：症状 → 诊断 → 处置（含命令与禁令） | `docs/runbook.md` |
| 决策记录（含被推翻的） | `docs/adr/ADR-001` … `ADR-013` |
| 每个阶段的测试证据 | `docs/test-evidence/phaseN.md` |
| 探针原始数据（6 组，可重跑） | `docs/appendix/*/` |

---

## 7. 已定案：不要重开（除非有新证据）

| 决策 | 结论 | 重开的唯一条件 |
| --- | --- | --- |
| ADR-001 | REST 为唯一合规面；gRPC 为平台外扩展 | 上游 OSDU 发布 RPC 规范 |
| ADR-002 | cpp-httplib 0.26.0 源码 + `fss_http` 包装层 | 发现包装层无法兜住的缺陷 → 转评估 Boost.Beast |
| ADR-003 | `IBlobStore` + `BlobCapabilities` + `LocationIssuer` | 出现第三种存储语义无法用能力布尔表达 |
| ADR-004 | 内置 SQLite（single）/ 远端 Storage Service（可选） | — |
| ADR-007 | **现在不引入协程/Asio** | 触发 T1/T2/T3 之一（ADR-007 §4） |
| ADR-008 | 两阶段批提交（写批 → `syncfs` → 统一 rename → `fsync(dir)`） | 真实断电测试证明该协议不足（需 root/虚拟化） |
| ADR-009 | 多实例：PG 强一致 + 租约 + 领导者选举 | NFS 语义验证失败（C9.27） |
| ADR-010 | I/O 引擎默认 `blocking`；io_uring 可选（seccomp 阻断） | U1–U4 全部满足（ADR-010 §5.2） |
| ADR-011 | 日志**自研最小实现**，不引入 spdlog | 需要文件轮转/syslog、需要异步有界队列、或自研转义/脱敏在真实流量下出现缺陷（ADR-011 §5） |
| gRPC × ASan | 链接 gRPC 的目标统一带 `GRPC_ASAN_SUPPRESSED=1`（打断布局耦合，P1-D20） | 升级 gRPC/编译器后 sanitizer 构建失败 → 重新核对 `sizeof` 与库的编译标志 |
| ADR-002 补充 | `fss_http` 包装层覆盖了库的 4 处行为：**listen backlog 5→512**、非法 `Range` 的 416→忽略、后缀区间按 RFC 归一化、流式路径补走私检查 | 升级 httplib 后 `scripts/verify_http_hardening.sh` 或 `ctest -L phase1` 失败 → 重新实测并更新 ADR-002 §4.1 |
| ADR-005（S3 驱动） | **自研 SigV4**（OpenSSL）+ libcurl 数据面 + 原生预签名；驱动切换只改配置 | 真实 MinIO/S3 端到端暴露兼容性问题，或出现 >5 GiB 对象需要分片上传 |
| ADR-006（大文件数据面） | **已采纳方向（P9 受控复核）**：`sendfile` vs httplib 内容提供者 = **2.12x**（几何平均，≥1.5x）→ 采纳"实现 sendfile 数据面"的方向；**实现未交付**，且落地必须与控制面校验**同源**、可关闭、默认仍走 httplib（`docs/adr/ADR-006-large-file-data-plane.md`） | 真实存储/网卡上的受控复核 <1.5x；或"复用控制面校验"做不到（只能复制一套 HTTP 语义）；或 TLS/mTLS 成为硬需求 |

**不要**在没有新实测证据的情况下重新讨论上表内容；也不要因为"听起来更好"而替换技术选型。

---

## 8. 常用命令

```bash
# 构建 + 阶段门槛
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
./scripts/run_all_gates.sh                       # 全部已启用阶段
FSS_GATES_WITH_PG=1 ./scripts/run_all_gates.sh   # 额外纳入 PG 基建门槛

# 文档一致性（收工前必跑）
./scripts/check_docs.sh

# 日志热路径基准（改 logging 热路径后必跑，见 ADR-011）
./scripts/bench_logging.sh

# C1.7：ASan + UBSan 全量（独立构建目录 build-asan；首次数分钟，之后增量）
./scripts/run_sanitizers.sh
FSS_GATES_SKIP_SANITIZERS=1 ./scripts/run_all_gates.sh   # 临时跳过 sanitizer 检查

# 开发/测试用 PostgreSQL（无需 root）
./scripts/dev_postgres.sh start|stop|status|reset|psql|dsn|env|logs|destroy
cmake -S . -B build -DFSS_WITH_PG=ON && ctest --test-dir build -L pg --output-on-failure

# 环境探测（部署前）
./scripts/check_io_uring.sh
```

---

## 9. 交付礼节

- 创建/修改文件后，在最终回复里用**行内代码**给出路径（便于点击），
  并且**必须**用 `present` 声明用户要求的最终交付物。
- 结论要**分层给出**：先给答案与数字，再给依据，最后给未验证项。
- **诚实优先于好看**：无法验证就写"未验证"；实验无结论就写"无结论"；
  自己的结论被推翻时，在 `docs/00-final-design.md` §5 明确记录。
