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
| 阶段 4 后续（未做） | 组合根接 `config/fss.example.json`（现为环境变量直读；schema 已在 P1 就绪）→ 计划 P9 |
| **阶段 5（对象存储驱动（S3 SigV4））** | ✅ **已完成并通过门槛**（C5.1~C5.10 全部满足：自研 `SigV4Signer` + **AWS 官方向量 5 条逐字节匹配**（另 2 条已知有意不同并附理由）+ `mock_s3.py` **独立验签** + `S3BlobStore` 数据面（libcurl 流式）与**同一套端口契约**跑第三遍 + 分页 >1000 键 + 错误映射（新增 `kStorageAccessDenied`）+ S3 模式端到端（客户端直连存储端点）+ **按配置切驱动**（同一二进制 posix/s3 各跑一遍）+ `ADR-005` 定稿；`ctest -L phase5` **6 测试 / 1737 断言**。**未实测**：真实 S3/MinIO 端到端、分片上传 >5 GiB、退避重试、STS 刷新） |
| **阶段 6（元数据记录语义完整化）** | ✅ **已完成并通过门槛**（C6.1~C6.13 全部满足）：切片 1 `SqliteMetadataRepository`（版本链 + `is_latest` 部分唯一索引 + `data` 列无损往返，元数据契约在 SQLite 上跑第二遍 → 闭合 C2.10 元数据侧）；切片 2 校验和（增量 `Hasher` + RFC/FIPS 公开向量 + 上游语义的**覆写**而非"不符就 400"）；切片 3 12 步序列（统一回滚 + 补发 `datasetDetails` + 第 11 步审计告警 + 7 个故障注入点）；切片 4 `getFileList`（上游三条 fixture 逐字 400）+ **9 个角色常量**与端点映射（含两处"任一角色"、"403 先于 400"）+ `ParseIso8601` 越界校验；切片 5 DMS/Delivery 的**两套键集合**与端到端；切片 6 1 GiB 搬迁（RSS 3.4 MiB < 64 MiB）+ 并发幂等（内存与 SQLite 两连接）+ tmp 名唯一性；切片 7 **GC 租约**（dry-run / 在途不删 / 过期才回收 / 无租约不许删 / 有记录永不删 / 并发不重复删）+ C6.10 回归。`ctest -L phase6` **9 测试 / 2578 断言**。**未做（已登记，不阻塞判据）**：`.tmp_*` 与 transfer token 清理（P9 C9.25）、GC 调度/领导者选举/指标（P9）、PG 版 `ILeaseRepository`、远端 Storage Service 仓储（ADR-004 列为可选）、组合根改接 SQLite 元数据仓储、GC 的数据库时钟（P6-D18） |
| **阶段 7（gRPC 适配层 + 双协议等价性）** | ✅ **已完成并通过门槛**（**C7.1~C7.10 全部满足**）：切片 1 = 契约 §5 的**唯一权威表**下沉 L3（HTTP/gRPC 两适配器从同一张表派生）+ `app::CallerFromHeaders` + C7.7 proto 隔离护栏；切片 2 = **14 个一元 RPC** + `app::wire_shapes`（两条协议共用的线上形状）+ 契约 §6 等价性矩阵 **12 行（C7.3）** + 自签 URL 的 token 解码比对（C7.4，含 4 条反向测试）+ `json_name` 对齐（C7.5）；切片 3 = **3 个扩展 RPC**（`UploadFile` 客户端流 / `DownloadFile` 服务端流 / `ServerSideCopy`）+ 组合根**同时开两个端口**（`FSS_GRPC_PORT`）+ C7.6（1 GiB/0 字节/中途取消/慢客户端）+ C7.8（**真实二进制**上并发混合协议）+ C7.10（区间读 == HTTP `Range`，端口级记账证明无整文件读）；`ctest -L phase7` **6 测试 / 5354 断言**。**未做（已登记，不影响判据）**：真实 S3/MinIO 上的流式 RPC 端到端、多租户并发流控调优 |
| 已有产品代码 | `src/common/`（L1 十二个模块）+ `src/domain/model/`（`dataset--File.Generic` 全字段模型）+ `src/domain/contract/`（契约 §5 的错误投影表）+ `src/domain/ports/`（15 个端口）+ `src/app/services/`（`expiry_policy`/`kind_validator`/`object_key_policy`/`location_issuer`）+ `src/app/usecases/`（13 个用例）+ `src/infra/blob/posix/`（POSIX 驱动）+ `src/infra/blob/s3/`（SigV4 + S3 驱动）+ `src/infra/location/sqlite/`（SQLite 位置仓储）+ `src/infra/metadata/sqlite/`（SQLite 元数据仓储）+ `src/infra/io/`（I/O 引擎 + 落盘接缝）+ `src/infra/transfer/`（自签 token + 数据面内核）+ `src/infra/{blob,location,metadata}/memory/`（内存适配器）+ `src/adapters/http/`（REST 适配层：错误映射 + DTO + 路由）+ `src/adapters/grpc/`（gRPC 适配层：`FileServiceAdapter` + 错误映射 + proto↔领域 DTO + `grpc_streaming_io`）+ `proto/osdu/file/v1/file_service.proto`（RPC 契约，扩展面）+ `src/main/server_main.cpp`（组合根）+ `tests/framework/{port_contract.h,fake_ports.h}`（契约测试基类 + 端口替身） |
| 当前可运行的验证 | `./scripts/check_docs.sh`（D1~D5）· `ctest -L phase0`（55 断言）· `scripts/verify_guard.sh`（护栏自证）· `scripts/verify_http_hardening.sh`（H-2 防护自证）· `scripts/verify_link_graph.sh`（C2.1 链接图，含越层注入自证）· `scripts/verify_capability_guard.sh`（C2.7 能力护栏自证）· `scripts/verify_composition_root.sh`（C4.9 组合根护栏自证）· `scripts/verify_driver_switch.sh`（C5.9：同一二进制 posix/s3 各跑一遍）· `scripts/verify_transfer_no_timeout.sh`（C4.11 的 ≥5 分钟慢传输实测，非日常门槛）· `ctest -L phase1`（14 测试 / 4984 断言）· `ctest -L phase2`（10 测试 / 952 断言）· `ctest -L phase3`（9 测试 / 580 断言）· `ctest -L phase4`（8 测试 / 1058 断言）· `ctest -L phase5`（6 测试 / 1737 断言）· `ctest -L phase6`（9 测试 / 2578 断言）· `ctest -L phase7`（6 测试 / 5354 断言）· `scripts/run_sanitizers.sh`（ASan+UBSan+LSan，phase0~7 全绿）· `scripts/bench_logging.sh`（日志热路径基准）· `FSS_GATES_WITH_PG=1`（+PG 基建 5 项） |

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
P8 认证授权与多租户
P9 硬化与交付
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
| **当机械护栏挡住了"架构上必须存在"的代码时，不要放宽护栏** | C7.7 的护栏规定"除 `adapters/grpc/` 外 `src/` 全树不得出现 `<grpcpp/`"，而 R12 要求**组合根创建** gRPC 服务 → 两者看似冲突。正确解法是"把第三方类型收进适配层"（`grpc_server.h` 只暴露启动/端口/`Shutdown()`），而不是给组合根加豁免 —— 与 `fss_http` 把 httplib 挡在 `common/http/` 内是同一条纪律（P7-D09） |
| **流式入口把"被取消"当成"正常读完"** | 客户端中途断开时 `ServerReader::Read()` 也返回 false，与 `WritesDone` 无法区分。当成 EOF 会让存储层把**截断的**字节 rename 成正式对象 —— 静默数据损坏（P7-D07，1 GiB 用例的 4 MiB 取消场景抓到）。判据：`Read()` 为 false 后必须查 `ServerContext::IsCancelled()`，取消 → 报错（`UNAVAILABLE`）并走失败清理 |
| **proto3 里"段是否存在"必须用 message presence 表达，测试 fixture 也要置位** | `dataset_properties.present` 不置位时，`FillData` 会跳过整段 → 记录经 proto 往返**整段丢失**，表现为"FileSource 不一致"（P7-D08）。proto3 没有字段 presence，**段级**存在性只能靠 message 字段（`has_dataset_properties()`）；测试构造的记录必须显式置 `present = true` |
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
| 决策记录（含被推翻的） | `docs/adr/ADR-001` … `ADR-011` |
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
| ADR-006（大文件数据面） | **尚未定稿**（P9 产出） | — |

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
