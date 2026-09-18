# 最终方案（唯一权威汇总）

> 本文档把分散在 9 个 ADR、5 份设计文档里的**现行结论**收敛到一处。
> 评审过程中有若干早期结论被推翻，本文档**只反映现行版本**，
> 被推翻的结论集中列在 §5（避免读者误用过时数据）。
>
> 阶段状态：**阶段 0、阶段 1 已完成并通过门槛；阶段 2 未开始**。
> 阶段 1 的完整证据见 `docs/test-evidence/phase1.md`（C1.1~C1.15 全部满足：14 个测试 / 4959 断言 + 两项自证 + ASan/UBSan 全绿）。
> 最后更新：本次会话。

---

## 1. 一句话结论

用 **C++20** 实现一个 **OSDU File Service 兼容**的文件服务后端：
**REST 为唯一合规面**（gRPC 作为平台外扩展协议），
用 **`IBlobStore` 端口统一"集中存储（POSIX）"与"对象存储（S3 兼容）"**，
用 **PostgreSQL + 租约 + 领导者选举**支撑多实例强一致，
用 **两阶段批提交**保证小文件写入的耐久性；
**五层架构 + 编译期依赖护栏**保证低耦合；
**10 个阶段，每阶段一道可执行的测试门槛，通过才进入下一阶段**。

---

## 2. 六个核心问题的最终答案

### Q1 OSDU 的接口支持 RPC 吗？→ **不支持**

实测上游仓库（`osdu/platform/system/file@master`）：**全仓库 0 个 `.proto` 文件**，
所有 API 都是 Spring `@RestController` + OpenAPI。平台唯一的二进制协议 ETP
（Reservoir DDMS）是 WebSocket + Avro，且规范明确声明"不使用 RPC 机制"。

| 面 | 性质 | 端口 |
| --- | --- | --- |
| REST/HTTP+JSON | **唯一 OSDU 合规面**：19 个端点，路径/字段名（含 PascalCase）/状态码/错误体严格对齐 | 8080，base path `/api/file` |
| gRPC | **平台外扩展**：17 个 RPC（14 一元 + 3 流式/代理），独立端口、命名空间隔离 | 50051 |

强制手段：**双协议等价性测试**（同一输入经两条链路必须产出等价领域结果 + 等价错误分类），
把"契约漂移"变成构建失败。→ [ADR-001](adr/ADR-001-rpc-as-extension.md)

### Q2 如何同时支持集中存储与对象存储？→ **端口 + 能力声明 + 单点策略**

```
应用层 LocationIssuer（全项目唯一的能力分支点）
   caps = blobStore.capabilities()
   ├─ native_presign = true  → 对象存储原生预签名 URL，客户端直连（服务不在字节路径上）
   └─ native_presign = false → 集中存储自签 URL + /v1/transfer 数据面（服务代理字节）
```

| 驱动 | 关键实现 | Driver 上报 |
| --- | --- | --- |
| `PosixBlobStore` | 原子写（tmp+fsync+rename）、`pread` 定位读、路径安全（逐段校验 + `O_NOFOLLOW` + 真实路径复核） | `posix` |
| `S3BlobStore` | 自研 SigV4（OpenSSL，无 AWS SDK）、libcurl 数据面、`x-amz-copy-source` | `s3` |

护栏：`capabilities()` 的调用点**只允许**出现在 `LocationIssuer`，防止能力分支扩散。
同一套契约测试对**三个实现**（内存 / POSIX / S3）各跑一遍。→ [ADR-003](adr/ADR-003-storage-abstraction.md)

### Q3 能否支撑高并发？→ **能，但先要修掉三个致命默认值**

| 问题 | 实测 | 最终配置 |
| --- | --- | --- |
| `TCP_NODELAY` 默认 false（http 库） | **23 req/s vs 21,062 req/s（950x）**，40ms delayed-ACK | **强制开启** |
| 线程池 = 并发硬上限（一条 keep-alive 连接占一个池线程） | 默认池 15，64 并发客户端只有 15 个被服务 | 按**并发连接数**公式配置，**不按核数** |
| 小文件写的 DB 天花板 | WAL+`NORMAL` 25,471 tx/s；`FULL` 仅 1,219 tx/s（**差 21x**） | `synchronous=NORMAL` + 有界写并发 8 + 组提交 |

实测能力（本机 16 核，量级参考）：小段读 **~40,000 req/s**；JSON 端点 **~200,000 req/s**；
大文件单流 1.4 GiB/s（httplib 路径）。→ [docs/05](05-capacity-and-concurrency.md)

### Q4 集中存储有大量 I/O，用协程异步更好吗？→ **不是协程，而是"异步文件 I/O"+"摊销 fsync"**

决定性实验：**同一个协程服务端、同样 4 个 io 线程、同样 2048 并发、同样 50ms 等待**，
只改"怎么等"：

| 方式 | 吞吐 |
| --- | --- |
| 协程里**阻塞调用** | 763 req/s（比线程模型**更差**：并发度退化为 io 线程数） |
| 协程里 offload 到线程池 | 758 req/s（等价于线程模型） |
| 协程里**真异步等待** | **40,246 req/s（52x）** |

而 POSIX 的 `pread`/`sendfile`/SQLite/`fsync` **都是阻塞的**，协程救不了它们。
**Linux 上的真正杠杆是 io_uring**（实测：1 线程 132,934 IOPS vs 128 线程 118,148 IOPS），
而且 **io_uring ≠ 协程**——那些数据是用**零协程**的手写 submit/reap 循环跑出来的。

**最终决定：现在不引入协程/Asio**（控制面在 pool=512 时已达 79,970 req/s，
高于 SQLite 天花板 25,000 tx/s 5 倍，瓶颈是 DB 不是线程）。
仅当触发 T1（需要 >2,000 并发连接）/ T2（DB 优化后仍受线程限制）/ T3（socket 侧成瓶颈）时，
**只对数据面**引入。→ [ADR-007](adr/ADR-007-async-and-coroutines.md)、
[docs/05 §5.5](05-capacity-and-concurrency.md)

**io_uring 也不作为必需依赖**（详见 Q4b）。

### Q4b 是否使用 io_uring？→ **不作必需依赖；作为可选引擎，默认 `blocking`**

**决定性约束（实测）**：默认容器运行时**直接拒绝** io_uring。

| 环境 | `io_uring_setup` |
| --- | --- |
| 宿主机 | ✅ 成功 |
| **容器 + 默认 seccomp profile** | ❌ **`EPERM`** |
| 容器 + `seccomp=unconfined` | ✅ 成功 |
| `kernel.io_uring_disabled` | `0`（**问题在容器运行时，不在内核**） |

Docker 自 2023 年起在默认 seccomp profile 中屏蔽 `io_uring_*`；K8s 普遍沿用该默认。
要让服务依赖它，就必须改 seccomp profile 或用 `unconfined`——**这是部署方才能决定的事**。

**收益本身也有上限**：

| 路径 | io_uring 的价值 |
| --- | --- |
| 缓存命中读（0.7 µs） | **无**（纯开销） |
| 非缓存读（120.9 µs） | 1 线程达 132,934 IOPS vs 128 线程 118,148 —— 设备是天花板（~136k），**只是省线程** |
| 小文件写 | io_uring 20,497 文件/s **< 两阶段批提交 31,478** —— **摊销 fsync 才是杠杆** |
| 大文件零拷贝 | `sendfile` 的 **2.05x** 收益**不依赖** io_uring（且本机 liburing 2.1 无 `io_uring_prep_sendfile`） |
| socket 侧（accept/recv/send） | ❌ 拿不到：httplib 持有 socket（这也是 ADR-006 要自持 socket 的原因） |

**最终决定**：`IIoEngine` 抽象 + `BlockingIoEngine`（默认，pread/pwrite + 有界线程池）
+ `UringIoEngine`（可选，启动探测 + 优雅回退）；
`io_engine: blocking | uring | auto`，**默认 `blocking`**；
探测结果在 `/v2/info` 与指标里可见。
启用需同时满足 **U1**（目标环境允许，用 `scripts/check_io_uring.sh` 验证）、
**U2**（目标存储上 ≥1.5x）、**U3**（触发 T1/T3）、**U4**（存储延迟高 —— HDD/NFS 上 io_uring 才有决定性价值）。
→ [ADR-010](adr/ADR-010-io-engine-choice.md)

### Q5 小文件写入的耐久性怎么保证？→ **两阶段批提交（同步先行、改名后置）**

```
阶段 A: write .tmp_0 … .tmp_N-1      （数据进页缓存）
阶段 B: syncfs(dirfd)                ← 一次系统调用让【全批数据】durable  （满足 R1）
阶段 C: rename .tmp_i → f_i          （纯元数据操作，廉价）
阶段 D: fsync(dirfd)                 ← 一次系统调用让【全批改名】durable  （满足 R2）
阶段 E: 对外确认（201 / 登记元数据）
```

| 协议 | 文件/秒 | 安全性 |
| --- | --- | --- |
| 每文件 `fdatasync` + 每文件 `fsync(dir)`（最朴素） | 383 | ✅ |
| 每文件 `fdatasync` + 每批 `fsync(dir)` | 784 | ✅（只快 2x，**没用**） |
| **★ 两阶段批提交（P4）** | **31,478** | ✅ **最终采用（82.3x）** |
| 无数据 `fsync` + 批末 `syncfs`（**早期错误方案**） | 35,389 | ❌ **不安全，只快 11%** |

安全性由**机器可检查的顺序不变量**验证（解析 strace，枚举崩溃点），
并用**对照组自证**（故意不安全的协议必须被检出残缺文件：11/12 次）。
→ [ADR-008](adr/ADR-008-write-durability-protocol.md)

### Q6 多实例部署如何保证一致性？→ **强一致控制面（PG）+ 租约 + 领导者选举**

实测复现了 **5 个问题**，其中 3 个会造成数据损坏或丢失：

| # | 问题 | 实测 | 修复 |
| --- | --- | --- | --- |
| M1 | tmp 名不含实例标识 | **21/40 次静默内容错乱**（B 的文件里是 A 的内容） | tmp 名含 `instance_id`+`pid`+计数 |
| M2 | 无幂等键唯一约束 | **20/20 重复记录 + 2 次复制** | 幂等键唯一索引 + `ON CONFLICT` **原子领取** |
| M3 | GC 删"无元数据记录"的对象 | **20/20 误删在途上传** | **租约（到期才回收）+ 原子领取** |
| M4 | SQLite 做共享状态 | 状态发散；NFS 上不安全 | 改用 **PostgreSQL** |
| M5 | 本地 nonce 表 | 跨实例无法拒绝重放 | nonce 入共享存储或**默认关闭** |

```
客户端 → 任意实例（无状态）
           ├── PostgreSQL：位置记录 / 元数据记录 / 在途租约 / advisory lock（选举）
           └── 共享 POSIX 存储（NFSv4/SAN/CephFS）：staging / persistent / tmp（名含实例标识）
```

**禁令**：禁止"DB 不可用降级到本地 SQLite"（那是状态发散的起点）→ 一律 `503 Retry-After`。
`deployment.mode=multi` 时**强制 5 条启动校验并拒绝不合规配置**。
→ [ADR-009](adr/ADR-009-multi-instance-consistency.md)

---

## 3. 最终架构

### 3.1 五层 + 编译期依赖护栏

```
L5 协议适配    adapters/http（OSDU REST，base path /api/file）   adapters/grpc（扩展，:50051）
                    └──────────────────┬──────────────────┘  只依赖 L4 用例（纯 C++ 结构体）
L4 应用层      usecases/（13 个用例）  services/（LocationIssuer/ExpiryPolicy/ObjectKeyPolicy/…）
                    └──────────────────┬──────────────────┘  只依赖 L3 类型与端口
L3 领域层      model/（FileLocation/FileMetadataRecord/…）  ports/（13 个端口）
                    ▲                  实现（依赖倒置）
L2 基础设施    infra/blob/{posix,s3,memory}  location/{sqlite,postgres}  metadata/{sqlite,postgres,remote}
               leases/  auth/{local,remote}  partition/  legal/  event/  transfer/
                    └──────────────────┬──────────────────┘  只依赖 L1
L1 通用库      result/ json/ http/（fss_http 包装层） crypto/ ids/ time/ fs/ config/ logging/ net/ bytes/
```

**依赖规则由 CMake 目标图在编译期强制**（`fss_domain` 只链接 `fss_common`，
一旦有人在领域层 `#include <grpcpp/...>` 就编译失败），并叠加**源码检索护栏**
（`<httplib.h>` 只允许出现在 `src/common/http/`）。具体实现只能在组合根 `src/main/` 创建。

### 3.2 HTTP 传输栈

```
adapters/http  →  fss_http（本项目：硬上限 / Range 归一化 / 中间件 / 错误归一化 / H-2 三重防护）
               →  cpp-httplib 0.26.0（third_party/httplib.h，MIT，源码单头文件，视为不可信组件）
```

不用 Ubuntu 的二进制包（jammy 的 0.10.3 有 `Range` 下溢缺陷）。
两个已复现的上游缺陷已固化回归测试（H-1 越界 Range 下溢；H-2 流式超限返回 201 且交 0 字节）。
版本锁定 + 校验和 + 构建期断言。→ [ADR-002](adr/ADR-002-http-framework.md)

### 3.3 两种部署形态

| 形态 | 位置/元数据存储 | 文件数据 | 适用 |
| --- | --- | --- | --- |
| `deployment.mode=single` | 本地 **SQLite**（默认） | 本地盘或共享盘 | 单实例、开发、小规模 |
| `deployment.mode=multi` | **PostgreSQL**（强制） | **共享挂载** 或对象存储 | 多实例、生产 |

两种仓储（sqlite / postgres）**共用同一套契约测试**。

---

## 4. 最终决策清单

| ADR | 决策 | 状态 |
| --- | --- | --- |
| [ADR-001](adr/ADR-001-rpc-as-extension.md) | gRPC 作为**平台外扩展**，REST 为唯一合规面 | 已采纳 |
| [ADR-002](adr/ADR-002-http-framework.md) | 用 **cpp-httplib 0.26.0 源码**作传输层 + **自建 `fss_http` 强化包装层** | 已采纳（**修订版**） |
| [ADR-003](adr/ADR-003-storage-abstraction.md) | 单一 `IBlobStore` 端口 + `BlobCapabilities` 统一两种存储 | 已采纳 |
| [ADR-004](adr/ADR-004-persistence-strategy.md) | 位置/元数据持久化：内置 SQLite + 可选远端 Storage Service | 已采纳（P6 复核） |
| [**ADR-005**](adr/ADR-005-s3-driver.md) | S3 驱动：**自研 SigV4** + libcurl 数据面 + 原生预签名；编码/寻址/错误映射与兼容矩阵 | **已采纳（P5 定稿）** |
| [**ADR-006**](adr/ADR-006-large-file-data-plane.md) | 大文件数据面：**采纳 sendfile 方向**（受控复核 2.12x ≥ 1.5x），但**实现未交付**；必须复用控制面校验、可关闭、默认走 httplib 内容提供者 | **已采纳（方向）**，P9/C9.12 定稿 |
| [ADR-007](adr/ADR-007-async-and-coroutines.md) | 异步/协程：**现在不做**，按 T1–T3 触发条件分步推进 | 已采纳 |
| [ADR-008](adr/ADR-008-write-durability-protocol.md) | 小文件写入：**两阶段批提交**（同步先行、改名后置） | 已采纳（含机器可检查的不变量验证） |
| [ADR-009](adr/ADR-009-multi-instance-consistency.md) | 多实例：**PG 强一致 + 租约 + 领导者选举** | 已采纳（5 个竞态已实测复现并验证修复） |
| [ADR-010](adr/ADR-010-io-engine-choice.md) | I/O 引擎：**阻塞线程池为默认**，io_uring 为可选加速引擎（默认容器 seccomp 阻断，实测 EPERM） | 已采纳 |
| [ADR-011](adr/ADR-011-logging-library.md) | 日志：**保留自研最小实现**（spdlog 只覆盖约 3% 的耗时与 0% 的核心需求），并把热路径优化到实测地板的同一量级 | 已采纳（附 4 条重开触发条件） |
| [ADR-012](adr/ADR-012-auth-and-tenant-binding.md) | 认证与租户绑定：**本地 JWT 校验（HS256）+ `partition` claim 绑定 + fail-closed**；不假设"前面一定有可信网关"；远端 Entitlements / RS256-JWKS 登记为未实现（`remote-entitlements` 模式**拒绝启动**而不是静默放行） | 已采纳（P8；证据 `docs/test-evidence/phase8.md`） |

---

## 5. ⚠️ 评审中修正的结论（不要再用旧数据）

| 早期结论 | 现状 | 影响 |
| --- | --- | --- |
| "**完全自研 HTTP/1.1 内核**"（ADR-002 初版） | **推翻**：改用 cpp-httplib 0.26.0 源码 + 强化包装层。初版从"某个二进制包不可用"推到了"自写整个 HTTP 栈"，是推理跳跃；且未评估 Boost.Beast 等候选 | 省下 600–900 行及其边界测试负担 |
| "**组提交 = 58,741 文件/秒**"（ADR-007 §8.3） | **作废**：该数字来自**没有对文件数据做 fsync** 的不安全协议。正确值 **31,478**（P4，安全） | 原数字会误导"用安全换性能"的决策；实际不安全方案只快 **11%** |
| `worker_threads: 8` | **改为按"并发连接数"公式推导**（该值是并发硬上限，不是 CPU 倍数） | 否则 64 并发的负载只有 8 个被服务 |
| "sendfile 比 httplib 快 **5–7x**"（`docs/05` §1.7 初版） | **修正为 2.12x**：旧数字来自**不同探针 + 不同客户端**的拼装比较（不可比、方法学不成立）。P9 用**同一负载生成器 + 独立进程 + 互不重叠绑核**做受控 A/B：httplib 2387/7320/6990 MiB/s vs sendfile 5488/14785/14274（c1/c4/c16）→ 2.30/2.02/2.04x，几何平均 **2.12x**（`docs/test-evidence/phase9-adr006.md`） | 结论**方向不变**（仍 ≥1.5x → ADR-006 采纳 sendfile 方向），但幅度只有旧值的 1/3；据此**不**接受"独立进程 + 复制一套控制面校验"的方案，改要求数据面与控制面**同源**（ADR-006 §4）。旧数字已在 `docs/05` §1.7 就地标注 |
| "单节点小段读上限 ~4 万 req/s"（`docs/05` §1.5 探针） | **未推翻，但适用范围要收窄**：那是**自有探针 + 简化服务端**（无鉴权/无 SQLite 读/无审计）。P9 在产品进程上实测控制面读 **14.3k req/s @c4**（c16/c64 回落到 11.0k/10.3k，p99 4.3/19.1 ms） | 两个数字**不能混用**：§1.5 用于定方向，§1.9 的基线用于回归判定；容量规划不得直接引用 4 万 req/s |
| `fsync_policy: by_size` | **改为 `durability: batch \| per_file`**（默认 `batch` = 两阶段批提交，批大小 500） | 语义更准确；并显式声明"批级耐久性窗口" |
| "`fss_http` 负责 Range 归一化" | **降级**：httplib 的内容提供者本身已 Range 感知（声明**完整**大小即可），`fss_http` 只做越界/416 归一化 | 避免过度设计 |
| `ux_mr_source` 谓词只有 `state <> 'deleted'` | **修正**：必须加 `is_latest`，否则**阻断合法的版本链**（同一记录的新版本共享 `FileSource`）。已做成回归断言 I9b | schema 自检抓到的真实设计缺陷 |
| "13 个端口" vs 计划里写"12 个端口" | 统一为 **13** | 文档一致性 |
| 领域层/应用层"可以便利地 include json" | **明确禁止**：开放字段用 `Json` 类型承载，JSON 细节不渗透 | 由目标图 + 源码检索双护栏强制 |
| "`ParseFileMetadataRecord` 对缺必需段应保持宽松，交给 `ValidateMetadataRecord`"（P2 单测的注释如此写着） | **推翻（P4-D07）**：必需**段**（`kind`/`acl`/`legal`/`data`）缺失时在**解析阶段**报错。模型无法表达"段不存在"，宽松解析会让 `File_missing_data.json` 退化成 `FileSource can not be empty`，与契约 §3.4 要求的"data 为空"不符 | 解析 = 结构（含必需段存在性），校验 = 语义；两条固定消息分别映射 `kFileSourceEmpty` / `kInvalidSourcePath` |
| "契约测试基类已经覆盖了 `FileLocation::extra` 往返"（P2/P3 的结论） | **有洞（P4-D09）**：原用例只用自造的 `CustomField`，而实现真正写进 `extra` 的是 `container`/`object_key`。SQLite 仓储把这两个键当"已知键"吞掉 → 物理引用静默丢失 → `POST metadata` **500** | 契约测试加入这两个**真实键**；memory/sqlite/postgres 三个实现从此共用同一份断言 |
| "契约 §3.4 有 **13** 个负向样例需要实现"（P0/P4 的契约表） | **修正为 10 条上游在跑 + 2 条上游自己注释掉的行**：归档源码 `testing/file-test-*/.../features/IntegrationTest_File_POST.feature` 第 24/25 行的 Examples 条目以 `#` 开头（从未执行）。我们**照实现**这两条（ScalarIndicator 逐字对齐上游枚举与消息；Datatype_Mismatch 状态码对齐、消息更具体） | 判据 C4.3 的表述随之更正；同时拿到了**逐字**的上游期望消息（此前只有"消息要点"） |
| "校验只在 `ValidateMetadataRecord` 一处"（P4-D07 的处置） | **拆成两处**（P4-D11）：上游把 `Endian`/`ScalarIndicator` 建模成带 `@JsonCreator` 的枚举，非法值在**反序列化**时抛错，早于 Bean Validation。顺序可观测：`File_invalid_Endian.json` 的 `FileSource` 同时非法，上游仍报 Endian | 枚举检查放解析阶段；`@NotNull`/ACL/legal 留在校验阶段；两者在非 JSON 入口（gRPC）都保留安全网 |
| "数据面只需要一个读超时"（P4 之前的 `read_timeout_sec` 单值） | **细化为三类**（C4.11）：普通路由空闲 = `read_timeout_sec`；普通路由**整体** = `json_request_timeout_sec`；数据面**无整体超时** + 空闲 = `transfer_idle_timeout_sec`。socket 级只能一个值，取较大者，其余按块检查 | 卡住的传输会在空闲超时后收到 **408**（此前会被误报成 400 `content-length mismatch`，P4-D08） |

---

### 5.y 本轮更正的既有结论（P6 元数据语义）

| # | 旧结论（记录于） | 现状（依据） | 影响 |
| --- | --- | --- | --- |
| 1 | 「客户端提供了 `Checksum` 但不符 → `400` + **删除**已搬迁的 persistent 对象」（`docs/04-implementation-plan.md` C6.4） | **推翻（P6-D05）**：上游第 7 步是 `checksum = storageUtil.getChecksum(persistentLocation)`，非空则**无条件覆写** `FileSourceInfo.Checksum` + `ChecksumAlgorithm`，失败语义为 `—`（非致命）—— `docs/01-osdu-research.md` §2.1/§2.3。**权威样例** `tests/conformance/fixtures/upstream/File_CorrectPayload.json` 客户端给的正是 `MD5("") = d41d8cd9…` 却声明 `ChecksumAlgorithm: "SHA-256"`，期望 **`201`**；按旧结论实现会把这条样例判成 `400`，并让 phase4 已收口的 C4.2/C4.3 两条用例失败（实测） | 实现改为"覆写 + 不比对 + 不回滚"；C6.4 判据更正为「未提供 → 服务端计算并写入；提供 → **被覆写**；算法覆盖 SHA-256/MD5/SHA-1 **跟随驱动**」。**G1（OSDU 兼容）优先于自造约束** |

---

### 5.z 本轮更正的既有结论（P7 gRPC 适配层）

| # | 旧结论（记录于） | 现状（实测） | 影响 |
| --- | --- | --- | --- |
| 1 | 契约 §6 的签名 URL 等价判据 = 「`scheme` 相同 ∧ `host` 相同 ∧ **`path` 相同** ∧ query 键集合相同 ∧ 过期 ±5s」（`docs/03-api-contract.md` §6） | **判据不可满足（P7-D02）**：集中存储模式下两条链路返回的都是**自签传输 URL**（`/v1/transfer/<token>?exp=…&sig=…`），其中 `path` 内嵌的是**密文**且每次签发带新 nonce —— 字面 `path` 相等**永远不成立**。若照旧判据写测试，实现正确也会被判成失败；反过来若退化成只比"都返回 200"，则无法区分"两条链路指向同一条位置记录"与"各自签发了一个 URL" | 判据改为**分形态**：通用形态（签名在 query，如 S3 预签名）仍比 `path`；自签形态比 `/v1/transfer/` 前缀 + query 键集合 + 过期 ±5s + **解密验签后的 token 声明逐项相等**（`partition`/`file_id`/`container`/`object_key`/`zone`/`op`）。这是**收紧**而非放宽；`test_protocol_equivalence` 另附 4 条反向测试保证判据不恒真 |
| 2 | 「gRPC 的 `data.extra` 未识别字段可随 proto 往返」（P7 切片 1 前的隐含假设） | **不成立**：proto3 **没有未知字段保留**，`Struct` 只能表达已建模的 `ExtensionProperties` | 契约 §6 的等价性按**已建模字段 + 领域结果**比对；"全字段无损往返"只对 REST 链路成立（`docs/test-evidence/phase7.md` §6 登记） |

---

### 5.x 本轮更正的既有结论（HTTP 传输层）

| # | 旧结论（记录于） | 现状（实测） | 影响 |
| --- | --- | --- | --- |
| 1 | 「请求 URI > 8192 → 400/414」（ADR-002 §4 表，按编译期常量推断） | **target ≤ 8191 正常；≥ 8192 → 连接被关闭且无响应**（`test_http_server.cpp` / 探针实测） | 契约 §1.7 已更正；不改变实现（这是库的行为），但**不能**再向客户端承诺 414 |
| 2 | 「越界 Range → 416」并把 `bytes=999999999-8388607` 当作用例（ADR-002 §4 / H-1 回归） | 该输入是 **`last < first`（RFC 7233 §2.1 判为语法非法）→ 应忽略该头返回 200 全量**；真正"语法合法但越界"的 `bytes=999999999-` 才 → 416 | H-1 的**不变量**（不得下溢 `Content-Length`）保持不变并在两条输入上都断言；状态码按 RFC 与契约 §1.8 更正 |
| 3 | 「`Range` 语法非法时返回什么」未记录 | 库在**解析阶段**就把非法 Range 变成 416 并**跳过路由**（与 RFC 7233 §4.4 冲突） | 包装层在错误处理器里重新分发为 200 全量，并记 `malformed_range_ignored_by_wrapper` 警告 |

## 6. 最终关键参数（默认值及其依据）

| 参数 | 最终值 | 依据 |
| --- | --- | --- |
| `server.http.tcp_nodelay` | **`true`** | 关闭时 40ms delayed-ACK，**950x** 差距 |
| `server.http.worker_threads` | 0 = 按 `max(64, 2×核数)` 推导 | 线程池 = 并发连接上限 |
| `server.http.max_connections` | 超限 → **503 + `Retry-After`** | 排队只会把延迟推到超时 |
| `server.http.transfer_memory_budget_bytes` | 256 MiB（超限**拒绝启动**） | `并发 × 缓冲 ≤ 预算` |
| `http.error_format` | `apperror`（OSDU 标准） | 另提供 `legacy` / `api_error` 开关 |
| `expiry.default` / `.max` | **`1H`** / `7D`（超限**静默截断**） | 对齐上游**代码**（文档写 7 天是错的） |
| `storage.driver_report_override` | `""`（上报真实驱动） | 上游把所有云硬编码成 `"GCS"`；需要时用开关复刻 |
| `storage.posix.durability` | **`batch`** | 两阶段批提交；`per_file` 为精确模式 |
| `storage.io_engine` | **`blocking`** | 默认容器 seccomp 阻断 io_uring（实测 EPERM）；可选 `uring`/`auto` |
| `storage.posix.group_commit_max_batch` | 500 | 吞吐 vs 崩溃丢失窗口的折中（建议 500–2000） |
| `location.sqlite.synchronous` | **`NORMAL`** | `FULL` 差 **21x** |
| `location.sqlite.max_write_concurrency` | **8** | 实测 8 线程为峰值，32 线程反而下降 |
| `metadata.repository` | `sqlite`（single）/ **`postgres`（multi 强制）** | ADR-004 / ADR-009 |
| `leases.enabled` / `leader_election.enabled` | `false`（single）/ **`true`（multi 强制）** | ADR-009 |
| `gc.require_lease_expiry` | **`true`**（**禁止**"无记录即删"） | naive GC 实测 20/20 误删在途上传 |
| `self_signed.single_use_nonce` | **`false`** | 本地 nonce 表在多实例下不成立 |
| `deployment.mode=multi` 强制校验 | **5 条，任一条不满足 → 拒绝启动** | 防止把 SQLite 误配进多实例 |
| PG `client_connection_check_interval` | **`1s`** | 默认 0 时崩溃的持锁会话不会释放锁 → leader 永久失联 |

---

## 7. 交付节奏：10 个阶段，每阶段一道门槛

```
P0 环境与契约可行性                    ✅ 已完成（7 用例 / 55 断言）
P1 分层骨架 + 通用库 + HTTP 传输层      ✅ 已完成（12 个 L1 模块 + fss_http；见 test-evidence/phase1.md）
P2 领域模型 + 13 个端口 + 应用层纯逻辑     ✅ 已完成（10 测试 / 952 断言）
P3 集中存储驱动 + 位置仓储 + 数据面        ✅ 已完成（9 测试 / 580 断言）
P4 REST 适配层 + 端到端垂直切片（POSIX）   ✅ 已完成（8 测试 / 1058 断言）← 此阶段服务可被 OSDU 客户端真实使用
P5 对象存储驱动（S3 SigV4 + mock-S3 独立验签） ✅ 已完成（6 测试 / 1737 断言）
P6 元数据记录语义完整化（12 步序列 + 回滚 + 版本链 + DMS + Delivery） ✅ 已完成（9 测试 / 2578 断言；C6.1~C6.13）
P7 gRPC 适配层 + 双协议等价性            ✅ 已完成（C7.1~C7.10；17/17 RPC + 契约 §6 矩阵 + 流式 + 双协议并发；6 测试 / 5378 断言）← 此阶段"双协议"达成
P8 认证授权与多租户                        ✅ 已完成（C8.1~C8.8；JWT + 路由预检 + 跨租户隔离 + 远端 Entitlements fail-closed + 审计覆盖 + multi 校验 + 时钟偏差；6 测试 / 1323 断言）
P9 硬化与交付（容量基线 / 故障注入 / GC / 打包 / 定稿 ADR-006）
```

**铁律**：门槛未通过 → 不得开始下一阶段。每阶段证据归档到 `docs/test-evidence/phaseN.md`。

统一的门槛入口：

```bash
./scripts/run_all_gates.sh                        # 全部已启用阶段
FSS_GATES_WITH_PG=1 ./scripts/run_all_gates.sh    # 额外纳入 PostgreSQL 基建门槛
```

**写测试的三条纪律**（已因此抓到多个真实缺陷）：
① 关键断言必须配**自证对照**（否则无法区分"实现正确"与"测试无效"）；
② 性能测量必须**独立进程 + 绑核**；
③ 性能数字必须**标注协议与安全性**。

---

## 8. 已就绪的资产

| 资产 | 内容 |
| --- | --- |
| 构建 | `CMakeLists.txt` + `cmake/{Dependencies,ProtoGen}.cmake`；proto 代码生成链路已验证 |
| 契约 | `proto/osdu/file/v1/file_service.proto`（17 个 RPC，`json_name` 对齐 OSDU JSON） |
| 依赖 | `third_party/`：nlohmann/json 3.10.5、Catch2 2.13.8、**cpp-httplib 0.26.0**（均源码 + 校验和） |
| 测试基建 | 阶段 0 门槛；`scripts/dev_postgres.sh`（**无需 root** 的真实 PG）+ CTest `pg` fixture |
| 环境探测 | `scripts/check_io_uring.sh`（宿主机/容器/回退三态，产出 ADR-010 的 U1 结论） |
| 数据库层验证 | `db/migrations/001_init.sql`；`db/tests/001~003`（schema 不变量 / 跨会话锁 / 真并发领取） |
| 文档 | `docs/00`（本文）+ `01` 调研 + `02` 设计 + `03` 契约 + `04` 计划 + `05` 容量 + `development` + 9 个 ADR |
| 证据 | `docs/appendix/` 6 组可重跑探针（httplib 缺陷 / 容量 / 协程 / POSIX I/O / 写入耐久性 / 多实例竞态） |

**当前实测状态**：`./scripts/run_all_gates.sh` 与 `FSS_GATES_WITH_PG=1` 两个入口全绿
（阶段 0 门槛 + 5 个 PG 基建测试，约 30 秒）。

---

## 9. 未验证 / 未决事项（诚实清单）

### 9.1 上生产前的**硬前提**

| # | 项 | 为什么未验证 | 门槛 |
| --- | --- | --- | --- |
| 1 | **NFS/SAN 的 `rename` 跨客户端原子性** | 本机无 NFS 可挂载 | **C9.27** |
| 2 | **NFS 上 `fsync`/`syncfs` 的耐久性语义** | 同上；而 ADR-008 的 P4 依赖 `syncfs` 覆盖全批数据 | **C9.27** |
| 3 | close-to-open 一致性（A 写 B 立刻读） | 同上 | **C9.27** |
| 4 | 不要依赖 NFS 文件锁 | 设计上已改用 PG advisory lock；但需确认实现里没有残留 | C9.27 |

### 9.2 待实测/待定稿

| # | 项 | 门槛 |
| --- | --- | --- |
| 5 | ~~**ADR-006 的受控复核**~~（`sendfile` 数据面 vs httplib，同一负载生成器；≥1.5x 才定稿，否则放弃） | ✅ **已完成（P9）**：**2.12x ≥ 1.5x → 采纳方向**，见 `docs/adr/ADR-006-large-file-data-plane.md`；实现未交付 |
| 6 | ADR-005（S3 驱动）定稿 | P5 |
| 7 | 真实存储（NVMe/HDD/NFS）上的 I/O 延迟与容量数字 | C9.14 / C9.22 |
| 8 | io_uring 在**目标环境**（内核 + 容器 seccomp）的可用性与收益；HDD/NFS 上可能才有决定性价值 | C1.15 / C9.18 / C9.29 |
| 9 | 真实断电/VM 快照下的耐久性（需 root 或虚拟化） | ADR-008 §4.5（维持"未验证"标注） |
| 10 | 真实 OSDU Storage Service 的端到端（目前只有 mock） | ADR-004 |
| 11 | 真实 MinIO/S3 的端到端（目前 mock-S3 做独立验签） | C5.2 备注 |
| 12 | 多实例下 PG 的容量（连接数/QPS） | C9.28 |

### 9.3 明确不做（非目标）

`/v1` API（上游已移除）、`PUT /v2/files/{id}/metadata`（上游不存在）、
`/v2/files/{id}/versions`（上游不存在）、HTTP/2、TLS 终止、`multipart/form-data`、
自行实现分布式共识、多主写同一 partition、跨实例共享内存缓存、依赖 NFS 文件锁。

### 9.4 一处需要你决策的开放项

**① io_uring 是否可用取决于你们的容器 seccomp profile。**
用 `./scripts/check_io_uring.sh` 在**目标环境**跑一次即可知道。默认（blocking 引擎）
在任何环境都能跑，所以这不是阻塞项；但如果你们的存储是 **HDD 或 NFS**（延迟高 1–2 个数量级），
io_uring 的价值会显著上升，届时值得改 profile 放行。——见 [ADR-010](adr/ADR-010-io-engine-choice.md)

**② `syncfs` 是文件系统级操作**。如果你们的集中存储是**多业务共盘**，
P4 的批提交会 flush 整个盘，可能干扰其他写入。
应对：① 按 partition 分盘（推荐）；② 或该场景改用 `durability: per_file`（精确但只有 383 文件/秒）。
门槛 **C9.24** 就是测这个的，需要你确认实际部署是否共盘。

---

## 10. 下一步

**阶段 1：分层骨架 + 通用库 + HTTP 传输层**，顺序为：

1. 建完整 CMake 目标图（含空的 `fss_domain`/`fss_app`）+ 分层护栏脚本，并**证明护栏能失败**；
2. `common/result`（`Result<T>`/`Error`/`ErrorKind`/`FSS_TRY`）→ `common/crypto`（复用阶段 0 已固化的 HMAC 链式派生）→ `json`/`ids`/`time`/`fs`/`config`/`logging`/`net`/`bytes`；
3. `fss_http` 包装层（含 **H-1/H-2 回归**、`TCP_NODELAY` 自证、并发上限、内存预算）；
4. `ctest -L phase1` 满足 **C1.1–C1.13**，归档 `docs/test-evidence/phase1.md`。

详细的交付物、任务与逐条判据见 [`docs/04-implementation-plan.md`](04-implementation-plan.md) §3。
