# 各阶段状态明细（AGENTS.md §0 的原文，机械搬迁）

> **为什么有这个文件**：`AGENTS.md` 是每个新会话**自动读入**的指令文件，受会话指令预算
> （64 KiB）限制。原文的 §0 状态表按"每阶段一段长叙述"写，体积占了 37%，一旦超预算，
> **文件尾部会被静默截断**（§8/§9 先消失）—— 那是比"少几条状态"更危险的事。
> 因此把 §0 的明细**逐字搬到这里**（未做任何删改，包括每阶段的**未交付/未验证**登记），
> `AGENTS.md` §0 只保留索引与"必须知道的独立事实"。
>
> **维护约定**：新增阶段/切片的**详细叙述写在本文**（或 `docs/test-evidence/phaseN.md`），
> `AGENTS.md` §0 的表格只加一行状态。**不要把长叙述搬回 `AGENTS.md`。**
>
> 关联：门槛判据见 `04-implementation-plan.md`；实测命令与结论见 `test-evidence/phaseN.md`；
> 配置键三态见 `operations.md`。
>
> ⚠️ **下面是搬迁时的快照（逐字保留，不改写）**。快照之后发生的变更以 `AGENTS.md` §0.1、
> `04-implementation-plan.md` 与对应的 `test-evidence/phaseN.md` 为准。
> **已知的后继变更（截至本次搬迁）**：ADR-009 的**数据层三件已交付** —— PG 位置仓储、
> 在途租约（见 `test-evidence/phase10.md` §15）与**元数据仓储**（§16）；**`deployment.mode=multi`
> 运行形态与 leader election 已交付**（§17，`multi` 不再拒绝启动）；**ADR-009 §4.2 的跨步骤
> 原子领取 + `claiming→ready` 状态机已交付**（§18，`ClaimForWrite`/`MarkReady`/`ReleaseClaim`）。
> 因此下面 P6/P8/P9 行里"PG 版 `ILeaseRepository` / PG 仓储 / leader election / multi 运行形态
> **未交付**"的说法**已过时**。
> **仍未交付**：**崩溃者留下的 `claiming` 行按租约到期回收**、上传路径的租约
> `Acquire`/`Renew`（故 `leases.{ttl_seconds,renew_interval_seconds,time_source}` 仍
> 「已读但无效果」）、共享挂载探针、readiness 的 PG 探活与 `metadata.postgres.schema_version_check`、
> PG 连接预算（C9.28）、PG↔本地时钟偏移比对、真·多实例 E2E + 崩溃注入（C9.26）、NFS（C9.27）。
> 三态现为 **生效 122 / 拒绝启动 16 / 已读但无效果 18**（`operations.md` §1.3）。
>
> ⚠️ **B1 之后的更新（最新，优先于上面的搬迁快照）**：组合根的 `deployment.mode=multi`
> 接线**已交付**（见 `test-evidence/phase10.md` §17）——新增 L2 `src/infra/postgres/pg_leader_election.*`
> （专用锁连接的会话级 advisory lock），组合根创建 PG 位置/元数据仓储 + PG 租约 + leader election，
> 并让 GC 的周期调度与 `POST /v2/gc:run` 由 leader 门控；`deployment.instance_id` 在 multi 下
> 未配置/为空时自动生成；POSIX `.tmp.*` 补齐 ADR-009 §4.5 的随机后缀。
> 因此 `location.postgres.*`/`metadata.postgres.*`、`leases.enabled`、`leader_election.*`
> **不再是「已读但无效果」**，三态计数变为 **生效 122 / 拒绝启动 16 / 已读但无效果 18**。
> 快照行里"`multi` 仍拒绝启动""leader election 未交付"的说法**已过时**。
> **B1 仍未交付**：共享挂载探针、readiness PG `SELECT 1`+迁移版本校验、`instance_registry`/配置版本
> 一致性、PG 连接预算（C9.28）、PG-vs-本地时钟比较、上传路径租约 `Acquire`/`Renew`、
> `CreateFileMetadata` 跨步骤原子领取、完整多实例 E2E（C9.26）、NFS 语义（C9.27）、
> `/v2/info` 暴露 `instanceId`、`storage.posix.one_filesystem_per_partition`。
> ⚠️ **更正（E1a）**：上面列出的「`/v2/info` 暴露 `instanceId`」**已被 E1a 交付**
> （REST `instanceId` + gRPC `InfoResponse.instance_id`，双协议同源；见 `test-evidence/phase10.md` §23）。
> 上文作为历史登记保留。
>
> **C2 之后的更新（最新，优先于上面全部）**：ADR-009 §4.2/§4.3 的**在途租约侧已交付**
> （见 `test-evidence/phase10.md` §19）：上传路径 `GetUploadLocation` 发地址时 `Acquire`
> （别的实例持有未过期租约 → 503 且不发地址）、`CreateFileMetadata` 在复制/校验和期间
> `Renew`（新增 L4 `src/app/services/lease_renewer.*`，RAII、每条退出路径 `stop()+join()`、
> 续租失败在步骤边界 fail-closed）、`MarkReady`/回滚后 `Release`；GC 用 `ClaimExpired` 的返回
> 驱动新增端口方法 `IMetadataRepository::ReclaimStaleClaiming`（内存/SQLite/PG 三实现）回收
> **崩溃者留下的 `claiming` 行**（阈值 = `now - leases.ttl_seconds`，集合 = 已原子领取的过期租约）；
> PG 租约新增 `leases.time_source`（`database` 默认 / `local` 用注入 `IClock`）。
> 因此 `leases.{ttl_seconds,renew_interval_seconds,time_source}` **不再是「已读但无效果」**，
> 三态变为 **生效 125 / 拒绝启动 16 / 已读但无效果 15**（`operations.md` §1.3）。
> **C2 仍未交付/未验证**：`leases.enabled=false`（单实例默认）时崩溃者的 `claiming` 行
> **仍无自动回收**（没有租约就没有"已死"的证明）；真·多实例 E2E + **进程级 kill** 崩溃注入
> （C9.26；本切片用"放弃操作"在**进程内**模拟崩溃并如实登记）；共享挂载探针、readiness 的 PG
> 探活 + `metadata.postgres.schema_version_check`、PG 连接预算（C9.28）、PG↔本地时钟偏移比对、
> NFS 语义（C9.27）、`state='deleted'` 软删除语义、`/v2/info` 的 `instanceId`。
> ⚠️ **更正（E1a）**：上面两处「`/v2/info` 的 `instanceId`」（第 40 行与第 59 行）**已被 E1a 交付**
> （REST `instanceId` + gRPC `InfoResponse.instance_id`，双协议同源；见
> `test-evidence/phase10.md` §23）。上文作为历史登记保留（不删原句）。
> ⚠️ `AGENTS.md` §0.1 的「PG 多实例」行**已同步**（ADR-009 §10 收口时更新，E1a 亦在其未交付
> 清单里做了对应删减）；三态计数以 `operations.md` §1.3 为准。
> ⚠️ **更正（E1b）**：本文件里「`storage.posix.one_filesystem_per_partition` 未接通/未交付」的说法
> **已被 E1b 交付**（`true` 时启动期校验规则 A/B，违规 → exit 78；`storage.driver=s3` + `true` →
> 拒绝启动；见 `test-evidence/phase10.md` §24）。上文作为历史登记保留（不删原句）；
> 三态随之从 **129/15/13** 变为 **130/15/12**（键数仍 157，`operations.md` §1.3/§1.3.1）。
> ★ 本文件是**只追加**的历史日志，按设计保留被取代的中间值，因此**不在** E1b 新增的
> 跨文档三态护栏的扫描清单里（`tests/unit/test_operations_doc.cpp` 的注释写明了理由）。
>
> **C9.26 之后的更新（最新，优先于上面全部）**：ADR-009 §4.2/§4.3 的**进程级崩溃 E2E 已交付**
> （见 `test-evidence/phase10.md` §20）：`tests/integration/test_multi_crash_recovery.cpp` 拉起
> **两个真实 `fss_server` 进程**（共享同一 PG DSN 与同一 `storage.posix.root`），A 在"持 `claiming`
> 行 + 活租约"时被 **`kill -9`**，B 轮询接管 leader（`pg_locks` 的 backend pid 换人）并回收
> `claiming` 行、孤儿 staging 对象与位置记录；同一 `file_source` 重试 → **201** + SHA-256 与重传
> 字节一致。★ **共享存储是本地目录被两个进程共用，不是 NFS** ⇒ **C9.27（NFS 语义）仍未验证**。
> 崩溃窗口由**环境变量接缝 `FSS_CLAIM_HOLD_MS`**（**不是配置键**，故三态**不变**：仍
> **125 / 16 / 15**）确定化。`ctest` 默认构建 **87/87**；`ctest -L pg` 两引擎 **9/9**（原 8/8 + 新增 1）。
> **本切片仍未验证**：NFS 语义（C9.27）、默认 `leases.time_source=database` 的跨主机时钟偏移、
> 多于 2 个实例、`fileSource` 的产品级重试（LB 粘性）、gRPC 侧的 kill -9。
> ⚠️ 上面的搬迁快照、`AGENTS.md` §0.1、`04-implementation-plan.md` 的 C9.26 行仍写着"多实例 E2E
> 未交付" —— 以本段与 `test-evidence/phase10.md` §20 为准。
>
> **B2a 之后的更新（最新，优先于上面全部）**：ADR-009 的三条"共享状态可用/可比较"判据已交付
> （见 `test-evidence/phase10.md` §21）：① readiness 的 **PG 探活（`SELECT 1`）+ 迁移版本校验**
> （`src/infra/postgres/pg_schema.*`；期望值 `kExpectedSchemaVersion` 由
> `tests/unit/test_schema_version_constant.cpp` 从 `db/migrations/*.sql` 文件名**机械推导**，
> 加迁移不改常量 → 测试失败）⇒ `metadata.postgres.schema_version_check` **生效**；
> ② **C9.28 连接预算**（`deployment.expected_instances` **新键**；`实例数 × 每实例最坏池上限 ≤ PG
> max_connections` 否则 **exit 78**）；③ **PG↔本地钟偏移比对**（容忍 = `deployment.clock_skew_tolerance_seconds`
> —— 它**生效**；`deployment.max_clock_skew_seconds` 仍生效但只用于 multi 跨字段 + JWT `exp`/`nbf`）
> 否则 exit 78。测试接缝 `FSS_CLOCK_SKEW_INJECT_MS`（**环境变量，不是配置键**，runbook §10.3）。
> 三态 **生效 125 / 拒绝启动 16 / 已读但无效果 15 → 生效 128 / 拒绝启动 15 / 已读但无效果 14**，
> 键数 **156 → 157**。`ctest` 默认构建 **88/88**（+1 常量机械测试）；`ctest -L pg` 两引擎 **10/10**
> （含新增 `tests/integration/test_production_readiness.cpp`，4 用例 / 133 断言）。
> **仍未交付（B2b）**：共享挂载探针、`instance_registry` 心跳 / config-version 一致性；NFS 语义
> （C9.27）仍未验证。
>
> **B2b 之后的更新（最新，优先于上面全部）**：ADR-009 §5.3（滚动升级一致性）与 §8.1 item 4
> （`storage.posix.root` 是否真的共享）**已交付**（见 `test-evidence/phase10.md` §22）：
> ① 真的用 PG 时 upsert `instance_registry`（`instance_id`/`service_version`（= `FSS_BUILD_VERSION`）/
> `config_hash`（**脱敏**有效配置去掉每实例键后的 SHA-256）/`started_at`/`heartbeat_at`），
> 后台线程每 **10s** 刷新心跳；② 一个 peer 是 **live** 当且仅当心跳在 **3×10s = 30s** 内；
> live peer 的 `config_hash` 或 `service_version`（major.minor）不一致 → readiness **not ready**
> （可读原因指出对端 id 与差在哪）；stale 行被忽略（启动期按 300s 阈值清理）；③
> `storage.posix.shared_mount_required=true` → 写 `<root>/.fss_probe.<instance_id>` 并与每个
> live peer **交叉验证探针可见性**：启动期不可见 → **exit 78**，运行期不可见 → not ready；
> 启动时若无 live peer，横幅如实打印"**跨实例可见性尚未验证**"。readiness **复用 B2a 的**
> `ports.shared_state_probe`（REST/gRPC 同源）。因此 `storage.posix.shared_mount_required`
> **生效**：三态 **生效 128 / 拒绝启动 15 / 已读但无效果 14 → 生效 129 / 拒绝启动 15 /
> 已读但无效果 13**（键数仍 157）。新增 `tests/integration/test_shared_mount_and_registry.cpp`
> （4 用例 / **194** 断言）；测试接缝 `FSS_SERVICE_VERSION_OVERRIDE`（**环境变量，不是配置键**，
> runbook §10.4）。**仍未交付/未验证**：NFS 语义（C9.27 —— B2b 只证明"共享性"）、`/v2/info` 的
> `instanceId`、ADR-009 §10 的多实例部署/PG HA/按盘分区/滚动升级运维手册。见
> `docs/test-evidence/phase10.md` §22。
> ⚠️ **更正（后续两个切片）**：上面这段 B2b 的"仍未交付"三条**已全部收口**：
> ①「stale 行被忽略（**启动期**按 300s 阈值清理）」→ 陈旧行现在**启动期 + 运行期每 10 s tick**
> 都做一次尽力而为清理（阈值不变，仍是 300 s；清理失败只告警、不影响 readiness）—— **E1a**；
> ② 同段列出的「`/v2/info` 的 `instanceId`」已交付（REST/gRPC 双协议同源暴露）—— **E1a**；
> ③「ADR-009 §10 的多实例部署/PG HA/按盘分区/滚动升级运维手册」已交付 —— 落在
> `docs/operations.md` §10 + `docs/runbook.md` §8.1~§8.6。
> 见 `test-evidence/phase10.md` §23；上文作为历史登记保留（不删原句）。

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
| 阶段 4 后续（**已在阶段 10 接入**） | 组合根接 `config/fss.example.json`：**阶段 10 切片 1/2/3/4/5 已实现**（CLI > env > file > 默认；`--config`/`--set`/`--print-config`/`--once`）。156 个叶子键按**三态**逐键登记：**生效 113 / 拒绝启动 18 / 已读但无效果 25**（`docs/operations.md` §1.2/§1.3；切片 3 接通 9 键、C10.16 接通 1 键 + 2 键改为拒绝启动、**C10.16 续**接通 7 键 + 3 键改为拒绝启动、**切片 4**接通 4 键、**切片 5（C10.17）**接通 `self_signed` 3 键、**切片 6a（C10.18）**接通远端 legal/schema 校验器 6 键 + `auth.remote_entitlements.fail_closed` 1 键改为拒绝启动、**切片 6b（C10.19 / ADR-013 §9）**接通 `events.publisher` 与 `events.webhook.{url,timeout_ms,topic}` 4 键（发布失败**非致命**：连不上/超时/非 2xx 只告警，请求照常 201）、**C10.20** 接通 SQLite 数据库层组提交 6 键（`*.sqlite.{group_commit,group_commit_max_wait_ms,group_commit_max_batch}`；并发写一批一次 COMMIT、每操作 SAVEPOINT、读可能多等 ≤ `max_wait_ms`））；生产环境有"必须 jwt + 必须验签 + 密钥非空"的启动强校验 |
| **阶段 5（对象存储驱动（S3 SigV4））** | ✅ **已完成并通过门槛**（C5.1~C5.10 全部满足：自研 `SigV4Signer` + **AWS 官方向量 5 条逐字节匹配**（另 2 条已知有意不同并附理由）+ `mock_s3.py` **独立验签** + `S3BlobStore` 数据面（libcurl 流式）与**同一套端口契约**跑第三遍 + 分页 >1000 键 + 错误映射（新增 `kStorageAccessDenied`）+ S3 模式端到端（客户端直连存储端点）+ **按配置切驱动**（同一二进制 posix/s3 各跑一遍）+ `ADR-005` 定稿；`ctest -L phase5` **6 测试 / 1737 断言**。**未实测**：真实 S3/MinIO 端到端、分片上传 >5 GiB、退避重试、STS 刷新） |
| **阶段 6（元数据记录语义完整化）** | ✅ **已完成并通过门槛**（C6.1~C6.13 全部满足）：切片 1 `SqliteMetadataRepository`（版本链 + `is_latest` 部分唯一索引 + `data` 列无损往返，元数据契约在 SQLite 上跑第二遍 → 闭合 C2.10 元数据侧）；切片 2 校验和（增量 `Hasher` + RFC/FIPS 公开向量 + 上游语义的**覆写**而非"不符就 400"）；切片 3 12 步序列（统一回滚 + 补发 `datasetDetails` + 第 11 步审计告警 + 7 个故障注入点）；切片 4 `getFileList`（上游三条 fixture 逐字 400）+ **9 个角色常量**与端点映射（含两处"任一角色"、"403 先于 400"）+ `ParseIso8601` 越界校验；切片 5 DMS/Delivery 的**两套键集合**与端到端；切片 6 1 GiB 搬迁（RSS 3.4 MiB < 64 MiB）+ 并发幂等（内存与 SQLite 两连接）+ tmp 名唯一性；切片 7 **GC 租约**（dry-run / 在途不删 / 过期才回收 / 无租约不许删 / 有记录永不删 / 并发不重复删）+ C6.10 回归。`ctest -L phase6` **9 测试 / 2578 断言**。**未做（已登记，不阻塞判据）**：`.tmp_*` 与 transfer token 清理（P9 C9.25）、GC 调度/领导者选举/指标（P9）、PG 版 `ILeaseRepository`、远端 Storage Service 仓储（ADR-004 列为可选）、组合根改接 SQLite 元数据仓储、GC 的数据库时钟（P6-D18） |
| **阶段 7（gRPC 适配层 + 双协议等价性）** | ✅ **已完成并通过门槛**（**C7.1~C7.10 全部满足**）：切片 1 = 契约 §5 的**唯一权威表**下沉 L3（HTTP/gRPC 两适配器从同一张表派生）+ `app::CallerFromHeaders` + C7.7 proto 隔离护栏；切片 2 = **14 个一元 RPC** + `app::wire_shapes`（两条协议共用的线上形状）+ 契约 §6 等价性矩阵 **12 行（C7.3）** + 自签 URL 的 token 解码比对（C7.4，含 4 条反向测试）+ `json_name` 对齐（C7.5）；切片 3 = **3 个扩展 RPC**（`UploadFile` 客户端流 / `DownloadFile` 服务端流 / `ServerSideCopy`）+ 组合根**同时开两个端口**（`FSS_GRPC_PORT`）+ C7.6（1 GiB/0 字节/中途取消/慢客户端）+ C7.8（**真实二进制**上并发混合协议）+ C7.10（区间读 == HTTP `Range`，端口级记账证明无整文件读）；`ctest -L phase7` **6 测试 / 5378 断言**。**未做（已登记，不影响判据）**：真实 S3/MinIO 上的流式 RPC 端到端、多租户并发流控调优 |
| **阶段 8（认证授权与多租户）** | ✅ **已完成并通过门槛**（**C8.1~C8.8 全部满足**）：切片 1 = 本地 JWT 授权器（HS256 + `exp`/`nbf`/`iss`/`aud` + 角色 claim ∪ 静态角色表 + **租户绑定**，fail-closed）+ 路由级鉴权预检（`RouteAuthTable()`，鉴权先于 DTO 解析，未登记路由 fail-closed）+ 配置强校验 + `/v2/info` 的 `authMode`；切片 2 = **跨租户隔离**（两租户：跨租户头 → 403、B 读/删/列 A 的记录 → 404/空集、签名 URL 绑对象）+ **远端 Entitlements**（超时/5xx/坏 JSON/连不上/未配地址 → 一律 503）；切片 3 = **审计覆盖**（`AuditGuard` RAII：15 个受保护用例的成功/失败两侧 + actor/对象/结果/时间/correlation-id）+ 事件操作名表（契约 §4.6）+ **multi 模式 5 条启动校验**（含正例）+ `app::ClockSkewGuard`（参考时钟是唯一时间源；快/慢钟超容忍 → fail-closed）；`ctest -L phase8` **6 测试 / 1323 断言**。**未做（已登记）**：PG 版仓储/租约/数据库时钟与 `deployment.mode=multi` 的运行形态（P9；组合根当前对该模式**拒绝启动**）、RS256/JWKS、真实 Entitlements 联调、`kid` 多密钥轮换 |
| **阶段 9（硬化与交付）** | ✅ **已完成并通过门槛**（10 个切片要点，C9.1~C9.13/C9.15/C9.16/C9.25 全部满足；**C9.31 为 P9 补交，P10 期间完成**）：① **C9.1 并发**（100 并发 JSON 零 5xx；8 并发大文件逐路 SHA-256 + 存储侧 checksum 三方一致）；② **C9.2 故障注入 + 恢复 ≤30 秒**（503/500/502 + 非致命依赖；`mock_entitlements --fail-file` 注入与恢复）；③ **C9.3 六类资源上限**各一条"被拒绝" + 对照；④ **C9.6 指标**（`/metrics` 格式合法、值真的动过、不含 secret；**真实进程**已接入存储计量 —— 见 P9-D10）；⑤ **C9.5/C9.25 GC 清理**（`IBlobStore::remove_temp_files`：够旧删、在途保护且计数可观测、被位置记录"引用"也照删）；⑥ **C9.4/C9.11/C9.13/C9.15 容量基线**（独立进程 + 互不重叠绑核 + 每点位 3 次取中位数；`bench/capacity_bench.cpp` + `scripts/bench_baseline.sh` + `docs/appendix/capacity-baseline/BASELINE.tsv`）；⑦ **C9.12/C9.16 ADR-006 定稿**（同一负载生成器受控复核 `sendfile` vs httplib = **2.12x / 2.02x ≥ 1.5x → 采纳方向**，实现未交付）；⑧ **C9.7 门槛纳入 P9**（P0~P9 顺序全绿，**总耗时 225 s / 10 阶段**）；⑨ **C9.8 容器镜像**（多阶段 `Dockerfile` + `scripts/verify_image.sh`：readiness 200、强制 jwt、非 root、HEALTHCHECK **全部实测**）；⑩ **C9.9 运维文档 + C9.10 风险收口**（`operations.md`/`runbook.md` + 自动比对测试覆盖 156 个配置键；§16.1 覆盖全部 20 行风险）。修掉 **P9-D01~D10**（测试替身非线程安全；POSIX/S3 时间戳语义；数据面错误码被覆盖；流式 handler 不读体；护栏白名单；**组合根未接指标注册表与日志脱敏**）。`ctest -L phase9` **10 测试 / 892 断言**（含 C9.32 的 `tests/integration/test_startup_faults.cpp` 与 C9.30 的 `tests/integration/test_io_engine_exposure.cpp`）；`ctest` **84/84**。**未做/未验证（登记；★ 严格区分"未验证"与"未交付"）**：组合根当时未接 `config/fss.example.json`（**阶段 10 切片 1/2 已修**：三态 **113/18/25**，逐键见 `operations.md` §1.3）、PG 仓储/租约与 `mode=multi` 运行形态（**未交付**）、sendfile 数据面实现（**未交付**）；**真实硬件类「仍未验证」**（C9.14 真实存储/网卡、C9.17~C9.22 异步与 I/O 延迟基线、C9.24 `syncfs` 干扰 —— 需目标硬件/真实盘）；**多进程与目标存储类「仍未验证」**（C9.26 多实例 E2E、C9.27 NFS 语义、C9.28 PG 连接预算、C9.29 io_uring 收益 —— 需 PG / 可挂载 NFS / 允许 io_uring 的目标环境；其中 C9.26/C9.28/C9.29 同时**未交付**）；**容器/部署硬化「已在 Docker 上实测」**（C9.8 + 证据 `docs/test-evidence/phase9-image.md` §10：只读 rootfs（含/不含 `--tmpfs /tmp`）、`--cap-drop=ALL` + `no-new-privileges`（**Docker 默认 seccomp**）、`--memory=128m` 下 **1 GiB 流式**（进程峰值 RSS 20.9 MiB）、HEALTHCHECK + `--restart=on-failure` + SIGTERM 优雅退出（exit 0 / 0.30 s < 10 s grace）、健康检查负控 `unhealthy`、两条反向对照）。**C9.30 三半全部满足（第三半为 P9 补交、P10 期间完成）**：①「不允许 io_uring 的部署里端点行为不变」**已实测**（Docker 默认 seccomp + 上传读回）；②「指标暴露 `fss_io_engine{engine,requested}`」**已实测**，并新增 `fss_io_uring_available 0|1`（宿主能力探测，与生效引擎分列）；③「`/v2/info` 暴露 `ioEngine`/`ioUringAvailable`」**已交付**（REST 追加 camelCase 扩展字段、gRPC `InfoResponse` 追加字段号 10/11 且 `json_name` 对齐、两条协议**同源**于 `app::GetInfo`；判据 `tests/integration/test_io_engine_exposure.cpp` 7 用例 / 159 断言 + 探测注入接缝 `FSS_IO_PROBE_INJECT=available|blocked` 双向可证伪）。★ **独立事实不变**：`ioUringAvailable` 只表达**宿主能力**（**可用 ≠ 已启用**），**io_uring 引擎实现仍未交付**（ADR-010 U1~U4 未满足 ⇒ `ioEngine` 恒 `blocking`、`storage.io_engine=uring` 仍 exit 78）。**K8s（restricted PodSecurity / `readOnlyRootFilesystem`）、镜像 CVE 扫描、多架构（arm64）「仍未验证」**。**本轮新登记的部署陷阱（已修，C9.32）**：容器 `--pids-limit` < 默认线程数 **66** 时启动期线程创建 `EAGAIN` → 曾因未捕获 `std::system_error` 而 `ExitCode=139`（**不是 OOM**）。**C9.32 已修（P9 补交，P10 期间完成）**：`main()` 顶层兜底（`catch (const std::exception&)` + `catch (...)`，函数体搬进 `RunServer`）→ **退出码 70（EX_SOFTWARE）+ 可读「未预期异常」**，不再出现 `terminate`/139；既有退出码逐字不变（配置非法 78 / 未知 CLI 参数 2）。★ **只加顶层 catch 不够**（实测容器仍是 139）：工作线程池是 httplib 在 **runner 线程**里建的，部分创建失败会在 `httplib::ThreadPool` 里因析构 joinable 线程而 terminate；包装层用 `SafeThreadPool` + `Server::Start()` 把 runner 异常在**主线程**重抛（ADR-002 §4.1 的 **H-7**）后才真正拿到 70。实测下界**仍恰为 66**，建议 `--pids-limit ≥ 128`；容器回归断言见 `scripts/verify_image.sh` 的 H4a/H4a2，进程内判据见 `tests/integration/test_startup_faults.cpp`（`FSS_STARTUP_FAULT_INJECT` 故障注入接缝） |
| **阶段 10（配置面接线）** | ✅ **切片 1/2/3/4/5/6a/6b 全部完成（含 C10.16/C10.17/C10.18/C10.19/C10.20）+ ADR-008 的 P4（C9.23）**：让 `config/fss.example.json`（带注释 JSON）成为**真配置源**，优先级 **CLI > 环境变量 > 配置文件 > 默认值**（`fss::config::Load` 复用，未重写加载器）。**切片 1（C10.1~C10.8）**：`--config`/`FSS_CONFIG` + `--set` + `--print-config`（脱敏 + 逐键来源）；优先级与来源在横幅逐键可见；`server.http.*`/`storage.*`/`observability.*` 接进真实进程；`production` 强校验（必须 jwt + 验签 + 密钥非空）；自证：去掉配置文件层 → 8 用例失败。**切片 3（C10.13~C10.15）**：C10.13 审计 fail-closed（`UseCasePorts.audit_fail_closed` + `AuditGuard::Success()` 返回 `Result` + `FSS_AUDIT_FAULT_INJECT` 故障注入接缝）；C10.14 接通（`metadata/location.sqlite.busy_timeout_ms`、`metadata/location.sqlite.journal_mode`（WAL|DELETE，TRUNCATE→78）；**切片 4 补全** `metadata.sqlite.{journal_mode,synchronous}` 与 `location.sqlite.synchronous`）；C10.15 `auth.jwt.roles_claim`+`auth.local_roles.*`+`server.grpc.enabled`。**C10.16 已完成**：`partition.file.opendes.max_file_bytes` 超限上传 → 413、恰好等于上限 → 200（R16 正例）；`allowed/default_checksum_algorithm` 非法 → exit 78。三态 **113/18/25**（C10.20 后）。**切片 2（C10.9~C10.12）**：① **C10.9** 组合根**真的装配 `GcTask`** + 后台周期调度（`gc.interval_seconds`，第一轮立即跑；`gc.enabled=false`/`interval<=0` → 不启动并在横幅说明）；`gc.dry_run`/`require_lease_expiry`/`staging_ttl_hours`/`orphan_grace_hours` 生效；SIGINT/SIGTERM → 统一退出路径（先 `Stop()`+`join`，绝不留下 joinable thread）；`fss_gc_*` 在**真实进程** `/metrics` 可见；新增 `--once`（跑一轮 GC 退出 0）；单实例用 `src/infra/location/memory/memory_lease_repository.h`（PG 版未交付）；② **C10.10** `config/fss.example.json` 作为 `--config`（只覆盖路径/端口/密钥）**真的启动成功且 readiness 200**；改坏一个键 → exit 78；③ **C10.11** 16 个未实现能力的非默认值 → **拒绝启动（exit 78 + 「未实现 + 下一步」）**；`operations.md` 逐键改为**三态**：**生效 72 / 拒绝启动 16 / 已读但无效果 68**（合计 156）；④ **C10.12** `expiry.default`/`expiry.max` 接 `app::ExpiryPolicy`（作用于签发 URL 的 TTL；超上限**静默夹紧**、边界通过、非法仍 400 固定消息）。新测试 `tests/integration/test_config_wiring.cpp`（**31 用例 / 692 断言**，含 C10.16 续、切片 4 与切片 5；§10.6 父代理加固后 +3 断言）+ `tests/unit/test_transfer_token_key_id.cpp`（C10.17 codec 边界）+ `scripts/verify_config_wiring.sh`（**54 条断言**）；`run_all_gates.sh` 纳入 P10（全绿）；`ctest` **77/77**。**切片 5（C10.17）**：`self_signed.key_id` 进被签名 token 载荷（解码侧 fail-closed，换 id → 旧 URL **401**）、`self_signed.{default_ttl_seconds,max_ttl_seconds}` 作**自签分支的 TTL 上界**（`expiry.*` 语义不变，C10.12 用例不动）→ +3 键生效；**未交付**：多密钥轮换（ADR-009:227 的"多 key 并存"）。 **切片 6a（C10.18 / ADR-013）**：新增 L2 `src/infra/legal/remote_legal_validator.{h,cpp}` 与 `src/infra/schema/remote_schema_validator.{h,cpp}`（与 `RemoteEntitlementsAuthorizer` 同一套写法：`CURLOPT_NOSIGNAL`/`FOLLOWLOCATION=0`/连接超时+整体超时/**所有依赖故障 → `kUnavailable`（503）**/`Ready()`+`NotReadyReason()`）；组合根按 `legal.validator`/`schema.validator` 选 `noop`（默认，行为逐字不变、**不发请求**）或 `remote`（`base_url` = **完整端点 URL**，不追加路径；空 → exit 78）；`200+{"valid":true}` 通过、`{"valid":false,"message":M}` → **400 带 M**、其余（连不上/超时/非 200/非 JSON/缺 valid）→ **503 且无残留**（校验在用例第 3c 步、持久化之前）；`tests/tools/mock_validators.py` + `tests/framework/mock_validators.h`（`--observe-file` 断言请求体形状与"有没有发请求"）+ `tests/integration/test_remote_validators.cpp`（**8 用例 / 464 断言**）；**未交付**：与真实 Legal/Schema 服务联调、调用方身份透传（端口签名无 bearer）、校验缓存、重试退避。 **切片 6b（C10.19 / ADR-013 §9）**：新增 L2 `src/infra/event/webhook_event_publisher.{h,cpp}`（`NOSIGNAL`/`FOLLOWLOCATION=0`/连接超时 `min(1000,timeout_ms)`+整体超时/2xx 判定/`Ready()`+`NotReadyReason()`），组合根三分支 `log`（默认，行为逐字不变）/ `webhook`（出站 POST，**发布失败非致命**）/ `none`（内联 `NoopEventPublisher`，**显式关闭**），`webhook` + 空 `url` → exit 78；`PublishStatus` 补 `record_id`；`tests/integration/test_webhook_publisher.cpp`（**6 用例 / 330 断言**，含非致命三态 + 记录真的建出来）+ `mock_validators.py --mode webhook` 的 `bodies` 列表；**未交付**：异步有界发布队列、重试退避、投递保证、真实消息总线联调。**未做/降级（如实登记）**：**切片 4** 之后仍「已读但无效果」的只剩 `*.sqlite.max_write_concurrency`（单连接 + 互斥 ⇒ 实际并发恒为 1，改它无可观测效果；`group_commit*` 6 键已由 **C10.20** 接通）；**C10.16 续**：为 `storage.posix.{atomic_write,dir_mode,file_mode,fadvise_random,fadvise_dontneed_after_large_read}` 与 `partition.file.opendes.{staging,persistent}_container` 在 `PosixBlobStoreOptions`/`PartitionConfig` 上加真实字段并接通（+7 生效）；`storage.posix.{group_commit_max_batch,sync_dir_after_batch}`（ADR-008 的 P4 当时未实现）与 `partition.file.opendes.storage_driver`（驱动冲突）→ 拒绝启动（+3）；**本轮（ADR-008 的 P4 / C9.23）**：`storage.posix.durability=batch` 从 `kBySize` 近似改为**真两阶段批提交**（`write all tmp → syncfs → rename all → `fsync(dir)`），`group_commit_max_batch` → **生效**，`sync_dir_after_batch=false` **仍拒绝启动**（R2 不变量）→ 三态 **107/18/31**（当时；判据 `tests/integration/test_posix_batch_commit.cpp` + 真实进程 `/metrics` 的 `fss_posix_syncfs_total`；未验证项：真实断电、C9.24、吞吐数字不迁移）；`leases.*`（除 `enabled` 的守卫）/`leader_election.*`/`large_file_plane.*`/`proxy_mode` 等 **25 键已读但无效果**（逐键理由+下一步见 `operations.md` §1.3.3）；`storage.posix.durability=never` 只能经旧别名；`io_engine=uring` 即使内核可用也拒绝（ADR-010 U1~U4）。**P10 期间补交 P9 的 C9.31**：GC 的按需 HTTP 端点 `POST /v2/gc:run` + `GcTask` 单飞护栏（**不加配置键**，三态计数不变；证据 `docs/test-evidence/phase9.md` §12）。**ADR-006（本轮）**：大文件**下载**数据面落地（进程内第二个监听面 + `sendfile`，复用控制面同一个已包装 handler；7 键生效，三态 **130/15/12 → 137/14/6**；判据 `tests/integration/test_large_file_plane.cpp`，证据 `docs/test-evidence/phase10.md` §25；⚠️ ADR-006 §6 第 5 条真实存储/网卡复核仍未完成） |
| 已有产品代码 | `src/common/`（L1 十三个模块，含 `metrics/`）+ `src/domain/model/`（`dataset--File.Generic` 全字段模型）+ `src/domain/contract/`（契约 §5 的错误投影表）+ `src/domain/ports/`（15 个端口）+ `src/app/services/`（`expiry_policy`/`kind_validator`/`object_key_policy`/`location_issuer`/`clock_skew_guard`）+ `src/app/usecases/`（13 个用例）+ `src/infra/blob/posix/`（POSIX 驱动）+ `src/infra/blob/s3/`（SigV4 + S3 驱动）+ `src/infra/location/sqlite/`（SQLite 位置仓储）+ `src/infra/metadata/sqlite/`（SQLite 元数据仓储）+ `src/infra/io/`（I/O 引擎 + 落盘接缝）+ `src/infra/auth/local/`（本地 JWT 授权器）+ `src/infra/auth/remote/`（远端 Entitlements 授权器，P8）+ `src/infra/legal/`（远端法务校验器，P10 切片 6a）+ `src/infra/schema/`（远端 schema 校验器，P10 切片 6a）+ `src/infra/transfer/`（自签 token + 数据面内核）+ `src/infra/{blob,location,metadata}/memory/`（内存适配器）+ `src/adapters/http/`（REST 适配层：错误映射 + DTO + 路由）+ `src/adapters/grpc/`（gRPC 适配层：`FileServiceAdapter` + 错误映射 + proto↔领域 DTO + `grpc_streaming_io`）+ `proto/osdu/file/v1/file_service.proto`（RPC 契约，扩展面）+ `src/main/server_main.cpp`（组合根）+ `tests/framework/{port_contract.h,fake_ports.h}`（契约测试基类 + 端口替身）+ `tests/hardening/`（P9：并发/故障注入/资源上限/指标与 GC/数据面错误语义） |
| 当前可运行的验证 | `./scripts/check_docs.sh`（D1~D5）· `ctest -L phase0`（55 断言）· `scripts/verify_guard.sh`（护栏自证）· `scripts/verify_http_hardening.sh`（H-2 防护自证）· `scripts/verify_link_graph.sh`（C2.1 链接图，含越层注入自证）· `scripts/verify_capability_guard.sh`（C2.7 能力护栏自证）· `scripts/verify_composition_root.sh`（C4.9 组合根护栏自证）· `scripts/verify_driver_switch.sh`（C5.9：同一二进制 posix/s3 各跑一遍）· `scripts/verify_transfer_no_timeout.sh`（C4.11 的 ≥5 分钟慢传输实测，非日常门槛）· `ctest -L phase1`（14 测试 / 5017 断言）· `ctest -L phase2`（10 测试 / 996 断言）· `ctest -L phase3`（10 测试 / **849 断言**）· `ctest -L phase4`（8 测试 / 1060 断言）· `ctest -L phase5`（6 测试 / 1749 断言）· `ctest -L phase6`（9 测试 / **2703 断言**）· `ctest -L phase7`（6 测试 / 5378 断言）· `ctest -L phase8`（6 测试 / 1323 断言）· `ctest -L phase9`（**10 测试 / 892 断言**；含 C9.31 的 `tests/integration/test_gc_endpoint.cpp` 与 `tests/unit/test_gc_task_single_flight.cpp`、C9.32 的 `tests/integration/test_startup_faults.cpp`、C9.30 的 `tests/integration/test_io_engine_exposure.cpp`）· `scripts/run_sanitizers.sh`（ASan+UBSan+LSan，**phase0~phase10 全绿**；标签从 `run_all_gates.sh` 的 `IMPLEMENTED_PHASES` 推导，每个标签先断言"匹配到的测试数 > 0"）· `scripts/bench_logging.sh`（日志热路径基准）· `scripts/bench_baseline.sh`（容量基线；`--save`/`--check`）· `scripts/bench_sendfile_ab.sh`（ADR-006 的 A/B 复核）· `scripts/verify_image.sh`（C9.8：构建镜像 + readiness + 强制 jwt + 非 root + HEALTHCHECK；**+ 容器硬化 `[H*]` 段**：只读 rootfs（含/不含 `--tmpfs /tmp`）、`--cap-drop=ALL` + `no-new-privileges`（默认 seccomp）、`--memory=128m --pids-limit=64` → **ExitCode=70 + 可读「未预期异常」**（C9.32 的 **断言**；另有 pids=66 正控 H4a2）、`HEALTHCHECK` + `--restart=on-failure` + SIGTERM 优雅退出、健康检查负控、两条反向对照；默认约 50 s，`FSS_VERIFY_IMAGE_FULL=1` 追加 1 GiB 流式 + pids 下界，约 85 s）· `ctest -L phase10`（**6 测试二进制 / 61 用例 / 1914 断言** / 覆盖 C10.1~**C10.20** + ADR-008 的 C9.23（真实进程默认 batch 上传 200 且 `fss_posix_syncfs_total >= 1`）；含 `tests/integration/test_webhook_publisher.cpp`（+ `tests/tools/mock_validators.py --mode webhook` 与 `--observe-file` 的 `bodies` 列表）、**C10.20** 的 `tests/unit/test_sqlite_group_commit.cpp` 与 `tests/integration/test_sqlite_group_commit.cpp`（组提交：摊销 == `ceil(N/B)`、每操作 SAVEPOINT 原子性、`group_commit=false` 逐字回归、`max_wait_ms`、读不脏））· `scripts/verify_config_wiring.sh`（C10.1~C10.12 的真实二进制断言 54 条）· `docs/operations.md` + `docs/runbook.md`（C9.9；`test_operations_doc` 自动比对 156 个配置键 + 三态计数 113/18/25）· `FSS_GATES_WITH_PG=1`（+PG 基建 5 项） |
