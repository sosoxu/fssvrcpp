# fssvrcpp —— OSDU 兼容的文件服务后端（C++20）

一个用 C++20 实现的 **OSDU File Service 兼容后端**，支持：

- **两种存储方式**：集中存储（POSIX / NFS 共享卷）与对象存储（S3 兼容：AWS S3 / MinIO / Ceph RGW / SeaweedFS / 华为 OBS 等）
- **两种协议接口**：OSDU 兼容的 **REST/HTTP+JSON**，以及 **gRPC/RPC**（流式上传下载）
- **分层架构**：L1 通用库 → L2 基础设施适配 → L3 领域 → L4 应用 → L5 协议适配，依赖方向由 CMake 目标图**在编译期强制**

> 当前状态：**阶段 0、阶段 1 已完成并通过门槛**（分层骨架 + 12 个 L1 通用库 + `fss_http` 强化包装层；
> 14 个测试 / 4959 断言 + 两项生效性自证 + ASan/UBSan/LSan 全绿）。业务实现从阶段 2 起。

---

## 1. 关于"OSDU 是否支持 RPC"

**不支持。** OSDU 的规范接口面是 REST/HTTP+JSON + OpenAPI 3.x。
我们实测了上游源码仓库（`osdu/platform/system/file@master`）：**全仓库 0 个 `.proto` 文件**，
所有 API 都是 Spring `@RestController`。平台唯一的二进制协议是 Reservoir DDMS 的
**ETP（WebSocket + Apache Avro）**，且规范明确声明它"不使用 Avro RPC 机制"。

因此本项目的做法是：

| 面 | 性质 | 说明 |
| --- | --- | --- |
| REST/HTTP+JSON | **OSDU 合规面** | 路径、JSON 字段名（含 PascalCase 的 schema 字段）、状态码、错误体、请求头严格对齐 |
| gRPC/RPC | **平台外扩展** | 独立端口、独立命名空间；与 REST 共享同一应用层，并由**双协议等价性测试**强制语义一致 |

完整论证与证据见 [`docs/01-osdu-research.md`](docs/01-osdu-research.md) §9 与
[`docs/adr/ADR-001-rpc-as-extension.md`](docs/adr/ADR-001-rpc-as-extension.md)。

---

## 2. 文档索引

| 文档 | 内容 |
| --- | --- |
| [`AGENTS.md`](AGENTS.md) | ★ **给 AI 编码代理的硬约束**：环境约束、工作流契约、铁律、我们踩过的坑、已定案不要重开、会被机械检查的规则 |
| [`docs/00-final-design.md`](docs/00-final-design.md) | ★ **最终方案（唯一权威汇总）**：六个核心问题的答案、现行架构与决策清单、评审中被推翻的旧结论、关键参数、门槛节奏、未验证清单 |
| [`docs/01-osdu-research.md`](docs/01-osdu-research.md) | **OSDU 调研报告**：源码结构、完整 API 清单、元数据模型、存储抽象、鉴权、错误模型、RPC 结论、差距分析 |
| [`docs/02-design.md`](docs/02-design.md) | **架构设计文档**：目标与非目标、五层架构、编译期护栏、领域模型与端口、双模式存储、双协议适配、持久化、并发、安全与威胁模型、风险登记 |
| [`docs/03-api-contract.md`](docs/03-api-contract.md) | **接口契约**（实现与测试的唯一基准）：全局约定、19 个 REST 端点逐条规格、元数据契约与黄金样例、gRPC 映射、错误码双向映射、双协议等价性矩阵、扩展清单 |
| [`docs/04-implementation-plan.md`](docs/04-implementation-plan.md) | **实现计划**：10 个阶段，每阶段的交付物、任务、**门槛命令与通过判据** |
| [`docs/05-capacity-and-concurrency.md`](docs/05-capacity-and-concurrency.md) | **容量模型与并发设计**（实测）：高并发/小文件/TB 级大文件分段读的实测数据、原设计缺陷清单与必须的修正 |
| [`docs/adr/ADR-007-async-and-coroutines.md`](docs/adr/ADR-007-async-and-coroutines.md) | **异步/协程采纳策略**（实测）：协程何时有效、何时反而更差，以及本项目的量化触发条件 |
| [`docs/adr/ADR-008-write-durability-protocol.md`](docs/adr/ADR-008-write-durability-protocol.md) | **小文件写入耐久性协议**（实测）：两阶段批提交；更正了此前一个基于不安全协议的性能数字 |
| [`docs/adr/ADR-009-multi-instance-consistency.md`](docs/adr/ADR-009-multi-instance-consistency.md) | **多实例一致性**（实测）：5 个竞态的复现与修复、共享状态设计、领导者选举与租约 |
| [`docs/adr/`](docs/adr/) | 关键决策记录（ADR-001 RPC 扩展 / ADR-002 HTTP 传输栈 / ADR-003 存储抽象 / ADR-004 持久化策略） |
| [`docs/test-evidence/phase0.md`](docs/test-evidence/phase0.md) | 阶段 0 测试证据（含 4 个在门槛中发现并修复的缺陷） |
| [`docs/development.md`](docs/development.md) | **开发指南**：环境事实、构建/测试、`scripts/dev_postgres.sh` 用法、写测试的三条纪律、已知陷阱 |
| [`docs/appendix/osdu-file-service-source-notes.md`](docs/appendix/osdu-file-service-source-notes.md) | 上游源码逐文件调研原始笔记（~60 个已验证 URL、逐字段实测证据） |
| `proto/osdu/file/v1/file_service.proto` | RPC 契约（含 `json_name` 与 OSDU JSON 的逐字段对齐） |
| `config/fss.example.json` | 完整配置样例（与运维文档一一对应） |

**建议阅读顺序**：本 README → **`docs/00-final-design.md`（只看一份就看这个）** → `docs/01-osdu-research.md`（了解 OSDU）
→ `docs/02-design.md`（了解方案）→ `docs/03-api-contract.md`（了解接口）
→ `docs/04-implementation-plan.md`（了解如何落地）。

---

## 3. 快速开始（构建与运行阶段 0 门槛）

### 3.1 依赖

本项目刻意保持依赖最小，且**不引入需要包管理器安装的第三方库**：

| 依赖 | 版本（本机实测） | 获取方式 |
| --- | --- | --- |
| g++ | 11.4.0（C++20） | 系统 |
| CMake + GNU Make | 3.22.1 / 4.3 | 系统 |
| gRPC / protobuf | 1.30.2 / 3.12.4（含 `protoc`、`grpc_cpp_plugin`） | 系统 |
| OpenSSL | 3.0.2 | 系统 |
| SQLite3 | 3.37.2 | 系统 |
| libcurl | 7.81 | 系统 |
| nlohmann/json | 3.10.5 | 已 vendored 到 `third_party/`（MIT） |
| Catch2 | 2.13.8 | 已 vendored 到 `third_party/`（BSL-1.0） |
| cpp-httplib | **0.26.0** | 已 vendored 到 `third_party/httplib.h`（MIT，**源码单头文件**；仅作 HTTP 传输层，见下） |

> ⚠️ **硬约束**：本机 protobuf 为 3.12.4，**不支持 proto3 `optional`**（需 3.15+）。
> `.proto` 文件中禁止使用 `optional`，可选性用 message 包裹表达。

若 `third_party/` 缺失，可用以下方式恢复（**无需 root**，详见 `third_party/README.md`）：

```bash
# nlohmann/json + Catch2：apt 的 -dev 包解包即可
cd /tmp && apt-get download nlohmann-json3-dev catch2
for f in *.deb; do dpkg-deb -x "$f" ext/; done
cp -r ext/usr/include/nlohmann        third_party/
mkdir -p third_party/catch2 && cp ext/usr/include/catch2/catch.hpp third_party/catch2/

# cpp-httplib：⚠️ 不要用 Ubuntu 的二进制包（jammy 只有 0.10.3，有 Range 下溢缺陷）
#   从镜像 pool 取上游 0.26.0 源码单头文件
curl -O 'https://mirrors.ustc.edu.cn/ubuntu/pool/universe/c/cpp-httplib/cpp-httplib_0.26.0%2Bds.orig.tar.xz'
tar xJf 'cpp-httplib_0.26.0+ds.orig.tar.xz'
cp cpp-httplib-0.26.0+ds/httplib.h third_party/httplib.h
cp cpp-httplib-0.26.0+ds/LICENSE   third_party/licenses/cpp-httplib-LICENSE

# 校验
sha256sum -c third_party/CHECKSUMS.txt
```

### 3.2 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"

# 运行阶段 0 门槛（环境与契约可行性验证）
ctest --test-dir build -L phase0 --output-on-failure
```

期望输出：

```
100% tests passed, 0 tests failed out of 1
All tests passed (55 assertions in 7 test cases)
```

阶段 0 验证的内容：

| 用例 | 验证 |
| --- | --- |
| Q1.1 | C++20 语言特性（指定初始化器 / `constexpr` / `requires`） |
| Q1.2 | nlohmann/json 对 OSDU **PascalCase** 字段的精确处理（camelCase 必须报错） |
| Q1.3 | OpenSSL HMAC-SHA256 链式派生对齐 **AWS SigV4 官方向量** |
| Q1.4 | SQLite3 持久化文件位置记录 |
| Q1.5 | libcurl 完成真实 HTTP 往返 |
| Q2 | `.proto` 可生成、可编译、可在回环上完成真实 gRPC 调用 |
| Q3 | **同一份 OSDU JSON 经 REST 语义与 RPC 语义两条链路产出等价领域结果** |

（阶段 1 起的测试目标将在实现相应功能后加入。服务端二进制 `fss_server` 在阶段 4 交付。）

### 3.3 开发/测试用 PostgreSQL（可选）

多实例一致性相关的门槛需要**真实 PostgreSQL**。仓库自带无需 root 的基建：

```bash
./scripts/dev_postgres.sh start          # 自动解包二进制 + initdb + 应用迁移
./scripts/dev_postgres.sh status
eval "$(./scripts/dev_postgres.sh env)"  # 之后可直接用 psql

# 通过 CTest 运行数据库层验证（含 fixture 自动管理 PG 生命周期）
cmake -S . -B build -DFSS_WITH_PG=ON
ctest --test-dir build -L pg --output-on-failure

# 或把 PG 门槛一并纳入全部门槛
FSS_GATES_WITH_PG=1 ./scripts/run_all_gates.sh
```

**仅供开发/测试**（`trust` 认证、监听 127.0.0.1、关闭 `fsync`）；生产请用托管 PG。
详见 [`docs/development.md`](docs/development.md) §3。

### 3.4 常用命令

```bash
# 重新配置（改了 CMakeLists / proto 后）
cmake -S . -B build

# 只构建某个目标
cmake --build build --target fss_proto

# 列出全部测试
ctest --test-dir build -N

# 查看 proto 生成的代码
ls build/fss_gen/osdu/file/v1/
```

---

## 4. 架构一览

```
L5 协议适配层     adapters/http（OSDU REST）        adapters/grpc（RPC 扩展）
                     └──────────────┬───────────────────────┘
                                    ▼  只依赖应用层用例（纯 C++ 结构体）
L4 应用层         usecases/（13 个用例）  services/（LocationIssuer 等策略）
                                    ▼  只依赖领域类型与端口
L3 领域层         model/（FileLocation, FileMetadataRecord, …）  ports/（15 个端口）
                                    ▲  实现（依赖倒置）
L2 基础设施适配   infra/blob/{posix,s3,memory}  location/sqlite  metadata/{sqlite,remote}
                  auth/  partition/  legal/  event/  transfer/
                                    ▼  只依赖通用库
L1 通用库         result/ json/ http/（fss_http：httplib 之上的强化包装层） crypto/ ids/ time/ fs/ config/ logging/ net/ bytes/
```

**依赖规则（编译期强制）**

| 规则 | 强制方式 |
| --- | --- |
| 领域层/应用层**不得**依赖 protobuf、HTTP、sqlite3、libcurl、OpenSSL | `fss_domain`/`fss_app` 的 `target_link_libraries` 只有 `fss_common` |
| 两个协议适配器**互不依赖** | `fss_http_adapter` 与 `fss_grpc_adapter` 无相互链接 |
| 具体实现**只能**在组合根创建 | 源码检索护栏 + `scripts/verify_composition_root.sh` |
| 存储驱动的"能力分支"**只能**出现在 `LocationIssuer` | `capabilities()` 调用点护栏 |
| 仓储 SQL **必须**带 `partition_id` | 源码检索护栏 |

详见 [`docs/02-design.md`](docs/02-design.md) §3、§4。

---

## 5. 两种存储方式如何统一

```
                    ┌──────────────────────────────────────────┐
                    │ 应用层 LocationIssuer                     │
                    │   caps = blobStore.capabilities()         │
                    │   if caps.native_presign:  原生预签名 URL │ ← 对象存储
                    │   else:                    自签传输 URL    │ ← 集中存储
                    └──────────────────────────────────────────┘
                                      │
                ┌─────────────────────┴─────────────────────┐
                ▼                                           ▼
   ┌────────────────────────┐                 ┌──────────────────────────┐
   │ S3BlobStore            │                 │ PosixBlobStore           │
   │ · SigV4 预签名（OpenSSL）│                 │ · 原子写（tmp+fsync+rename）│
   │ · 客户端直连存储，服务不搬运│                 │ · 服务自身作为数据面         │
   │ · Driver = "s3"        │                 │ · 自签 URL + /v1/transfer │
   └────────────────────────┘                 │ · Driver = "posix"        │
                                              └──────────────────────────┘
```

关键点：**应用层与领域层没有任何 `if driver == ...` 分支**，
差异只体现在一次 `capabilities()` 查询与响应中的 `Driver` 字段值。
详见 [`docs/adr/ADR-003-storage-abstraction.md`](docs/adr/ADR-003-storage-abstraction.md)。

---

## 6. HTTP 传输栈：为什么是 cpp-httplib + 自建包装层

初版设计曾决定"完全自研 HTTP 内核"，评审时被质疑后复核，**确认该结论错误并已推翻**
（完整论证见 [`docs/adr/ADR-002-http-framework.md`](docs/adr/ADR-002-http-framework.md)）。现在的做法是：

```
adapters/http  →  fss_http（本项目：硬上限 / Range 归一化 / 中间件 / 错误归一化）
               →  cpp-httplib 0.26.0（vendored 源码，仅作传输层，视为不可信组件）
```

**否决"完全自研"**：Ubuntu 的 `libcpp-httplib-dev` 二进制包确实有 ABI/宏耦合问题，
但镜像 **pool 里有上游 0.26.0 的单头文件源码**——从"某个二进制包不可用"推出"必须自己写整个
HTTP 栈"是推理跳跃；而且自研意味着自己承担请求走私防范、keep-alive 状态机、分块编码等
全部边界的正确性，没有社区与模糊测试兜底，是更差的选择。

**但也不能裸用**：实测在**本项目最关键的流式路径**上复现了两个缺陷——

| ID | 版本 | 缺陷 | 后果 |
| --- | --- | --- | --- |
| **H-1** | 0.10.3 | 流式响应 + 越界 `Range` 无 416 分支，`size_t` 下溢 | 响应头非法（`Content-Length: 18446744072717940225`）→ 客户端挂死 |
| **H-2** | 0.26.0 | 流式请求体 + 超限时返回 **201 且交出 0 字节** | **静默数据丢失**：上传"成功"、对象为空 |

所以 `fss_http` 承担三重防护（按 `Content-Length` 前置拒绝 → 413；自封装计数 reader；
读取字节数一致性断言），并把 H-1/H-2 固化为**回归测试**，作为阶段 1 门槛的一部分。
复现源码与原始输出在 [`docs/appendix/httplib-hardening-probe/`](docs/appendix/httplib-hardening-probe/)。

**约束**：只有 `src/common/http/` 允许 `#include <httplib.h>`（分层护栏强制），因此换库
不必触碰领域层、应用层与协议适配层。

---

## 7. 开发流程（每一步先测）

| 阶段 | 内容 | 门槛 | 状态 |
| --- | --- | --- | --- |
| P0 | 环境与契约可行性 | `ctest -L phase0` | ✅ 通过 |
| P1 | 分层骨架 + 通用库 + HTTP 内核 | `ctest -L phase1` + 两项自证 + ASan/UBSan | ✅ |
| P2 | 领域模型 + 端口 + 应用层纯逻辑 | `ctest -L phase2` | ⬜ |
| P3 | 集中存储驱动 + 位置仓储 + 数据面 | `ctest -L phase3` | ⬜ |
| P4 | REST 适配层 + 端到端垂直切片 | `ctest -L phase4` | ⬜ |
| P5 | 对象存储驱动（S3 SigV4） | `ctest -L phase5` | ⬜ |
| P6 | 元数据记录语义完整化 | `ctest -L phase6` | ⬜ |
| P7 | gRPC 适配层 + 双协议等价性 | `ctest -L phase7` | ⬜ |
| P8 | 认证授权与多租户 | `ctest -L phase8` | ⬜ |
| P9 | 硬化与交付 | `ctest -L phase9 && scripts/run_all_gates.sh` | ⬜ |

**铁律**：阶段门槛未通过 → 不得开始下一阶段。
每个阶段的证据（命令、输出、结论、发现的缺陷）归档到 `docs/test-evidence/phaseN.md`。

详见 [`docs/04-implementation-plan.md`](docs/04-implementation-plan.md)。

---

## 8. 针对实际负载的能力与限制

> 实测数据、方法与探针见 [`docs/05-capacity-and-concurrency.md`](docs/05-capacity-and-concurrency.md)。

| 负载 | 支持情况 | 实测数字（本机 16 核，**仅量级参考**） |
| --- | --- | --- |
| 大量频繁读小文件 | ✅ 集中存储模式单节点约 **35,000 读/秒**（4 KiB 段）；⚠️ 但每并发传输占一个线程 | 16 KiB 段峰值 **39,516 req/s** |
| 大量频繁写小文件 | ⚠️ **DB 是瓶颈**：实测 WAL+`synchronous=NORMAL` 约 **25,000 事务/秒**（每文件 2 次事务）；`synchronous=FULL` 只有 **1,219/秒**（差 21 倍） | 默认改用 `NORMAL` + `fsync_policy=by_size` |
| TB 级大文件分段读 | ✅ 64 位偏移正确、**无读放大**（1 MiB 段 1.5 ms）、越界 416；⚠️ 集中存储模式下服务在字节路径上 | 单流 1.4 GiB/s（httplib 路径）/ 6–10 GiB/s（`sendfile` 数据面，ADR-006 拟定） |

**三条最重要的实测结论**（都已写进设计与门槛）：

1. **`TCP_NODELAY` 必须开启** —— cpp-httplib 默认为 `false`，关闭时 keep-alive 小请求
   有 **40 ms delayed-ACK 停顿**：**23 req/s vs 21,062 req/s（950 倍）**。
2. **线程池大小 = 并发上限** —— 一条 keep-alive 连接占用一个线程直到连接结束；
   httplib 默认池 `max(8, nproc-1)` = 15 是**硬上限**。原设计的 `worker_threads: 8` 是错的。
3. **`sendfile` 比 `read+write` 快 2.05x、每 GiB CPU 少 42%** —— 而 httplib 的 handler
   **拿不到 socket fd**，无法零拷贝。因此大文件数据面计划独立成进程（ADR-006，待受控复核定稿）。

**关于协程**（实测，详见 [ADR-007](docs/adr/ADR-007-async-and-coroutines.md)）：

| 等待方式 | 效果 |
| --- | --- |
| 协程 + **真异步等待** | ✅ 16 线程 **254k req/s** vs 线程模型 2,049 线程 127k req/s（2x 吞吐 / 128x 线程 / 9x 内存） |
| 协程 + **阻塞调用**（`pread`/SQLite/fsync） | ❌ **比线程模型更差**（763 req/s）——阻塞时不会让出，并发度退化为 io 线程数 |
| 协程 + **offload 到线程池** | ➖ 等价于线程模型（758 req/s），上限 = 池大小 |

**当前不用协程**：控制面在 pool=512 时已达 79,970 req/s，而 SQLite 天花板是 25,000 tx/s
→ 瓶颈是 **DB 不是线程**；对象存储模式下字节不经过服务 → 协程收益为零。
触发条件（T1–T3）与分步方案见 ADR-007。

**POSIX 集中存储的 I/O 专项实测**（详见 [ADR-007 §8](docs/adr/ADR-007-async-and-coroutines.md)）：

| 单次 I/O（4 KiB） | 平均延迟 | 单线程上限 | 结论 |
| --- | --- | --- | --- |
| 页缓存命中读 | **0.7 µs** | 1,534,599 ops/s | **不要异步** |
| O_DIRECT 非缓存读 | **120.9 µs** | 8,269 ops/s | 线程够用；io_uring 可 1 线程吃满设备 |
| **写 + fsync** | **1,438.7 µs** | **695 ops/s** | **★ 摊销 fsync，不是异步** |

| 写路径方案 | 文件/秒 | 安全性 |
| --- | --- | --- |
| 1 线程，每文件 fsync | 760 | ✅ |
| 64 线程，每文件 fsync | 16,104 | ✅ |
| io_uring（深度 16，单线程） | 20,497 | ✅ |
| 每文件 `fdatasync` + 每文件 `fsync(dir)` | 383 | ✅ |
| 每文件 `fdatasync` + 每批 `fsync(dir)` | 784 | ✅ |
| **★ 两阶段批提交**（写批 → `syncfs` → 统一 rename → `fsync(dir)`） | **31,478** | ✅ **推荐**（ADR-008） |
| ~~无数据 fsync + 批末 syncfs~~ | ~~35,389~~ | ❌ **不安全，禁止使用**（只快 11%） |

> ⚠️ **更正**：此前 README/ADR-007 给出的 **58,741 文件/秒** 来自一个**没有对文件数据做 fsync**
> 的协议，断电后可能产生"文件存在但内容为空"。正确协议见
> [ADR-008](docs/adr/ADR-008-write-durability-protocol.md)（含 strace 顺序不变量验证 +
> 崩溃注入 + 对照组自证）。

**io_uring 用 1 个线程达 132,934 IOPS，线程模型用 128 个线程才 118,148 IOPS** ——
但设备上限约 136k IOPS，**异步只是省线程，不能突破设备**。
另：**io_uring ≠ 协程**，上表 io_uring 数据是**零协程**的手写 submit/reap 循环跑出来的。

**多实例部署**（详见 [ADR-009](docs/adr/ADR-009-multi-instance-consistency.md)）

单实例的默认值在多实例下**会直接造成数据损坏或丢失**，已实测复现：

| # | 问题 | 实测 | 修复 |
| --- | --- | --- | --- |
| M1 | tmp 名不含实例标识 | **21/40 次静默内容错乱**（B 的文件里是 A 的内容） | tmp 名加 `instance_id` |
| M2 | 无幂等键唯一约束 | **20/20 重复记录 + 重复复制** | 幂等键唯一 + `ON CONFLICT` 原子领取 |
| M3 | GC 删"无元数据记录"的对象 | **20/20 误删在途上传** | 租约（到期才回收）+ 原子领取 |
| M4 | SQLite 做共享状态 | 状态发散；NFS 上不安全 | 改用 **PostgreSQL** |
| M5 | 本地 nonce 表 | 跨实例无法拒绝重放 | nonce 入共享存储或**默认关闭** |

**多实例要做的**：`deployment.mode=multi`（启动时强制 5 项校验并拒绝不合规配置）；
位置/元数据/租约放 **PostgreSQL**；数据放**共享挂载**；GC 走**领导者选举 + 租约**；
租约与过期判定一律用 **PG 的 `now()`**（消除时钟偏移）。
**禁止**"DB 不可用降级到本地 SQLite"——PG 不可用一律 `503`。

**部署拓扑建议**

| 场景 | 推荐 |
| --- | --- |
| 大量频繁小文件 | **对象存储模式**：字节走客户端直连预签名 URL，服务只处理 JSON → 服务不在字节路径上，可水平扩展 |
| TB 级大文件分段读 | **对象存储模式最优**（客户端直接 Range GET，服务零参与）；若必须用集中存储，走 `sendfile` 独立数据面 |

---

## 9. 诚实的边界说明

| 项 | 说明 |
| --- | --- |
| 不追求 OSDU 官方认证 | 认证需要完整平台环境；目标是"接口兼容 + 可对接" |
| gRPC 面不属 OSDU 规范 | 不参与 OSDU 合规性认证；已用等价性测试保证与 REST 语义一致 |
| 集中存储是"能力增强" | OSDU 参考实现**不存在**任何本地文件系统驱动；`/v1/transfer` 数据面是本项目新增 |
| 默认内置 SQLite 元数据仓储 | 上游把元数据交给 Storage Service。使用内置仓储时**记录不会被 OSDU Search 检索到**；需要该能力时改 `metadata.repository: remote` |
| `Driver` 字段 | 上游把所有云硬编码为 `"GCS"`；本项目默认上报真实驱动，可用 `storage.driver_report_override: "GCS"` 复刻 |
| 未实现项 | `/v1` API（上游已于 v0.7.0 移除）、`PUT /v2/files/{id}/metadata`（上游不存在）、`/v2/files/{id}/versions`（上游不存在）、HTTP/2、TLS 终止、`multipart/form-data` |
| 未验证项 | 真实 MinIO/S3 的端到端（仅用 Python 标准库 mock 做独立验签）；真实 OSDU Storage Service 的端到端（仅用 mock）；上游"landing zone 24 小时自动删除"的实际部署配置 |

---

## 10. 调研来源

上游源码与文档（均为本项目实测访问）：

- 源仓库：[`osdu/platform/system/file`](https://community.opengroup.org/osdu/platform/system/file)（GitLab project id 90，`master`，取样 HEAD `d7c25c2d`）
- [File Service 文档](https://osdu.pages.opengroup.org/platform/system/file/File-Service/)
- [File Service OpenAPI（v2, community）](https://community.opengroup.org/osdu/platform/system/file/-/blob/master/docs/api/community/v2/openapi.yaml) —— 注意：**不完整**，遗漏所有 `@Hidden` 端点
- [Generic File Schema](https://community.opengroup.org/osdu/data/data-definitions/-/blob/master/Generated/dataset/File.Generic.1.0.0.json)
- [Core Services Overview](https://community.opengroup.org/groups/osdu/platform/-/wikis/Core-Services-Overview)
- [Storage OpenAPI（`AppError` / bearer / cursor 分页）](https://community.opengroup.org/osdu/platform/system/storage/-/blob/master/docs/api/community/v2/openapi.yaml)
- [ETP 1.1 规范（"ETP does not use the Avro RPC facility"）](https://docs.energistics.org/ETP/ETP_TOPICS/ETP-000-012-0-C-sv1100.html)
