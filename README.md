# fssvrcpp

**OSDU 兼容的文件服务后端（C++20）** —— 单个二进制同时提供 **REST/JSON 合规面**与 **gRPC 扩展面**，
并同时支持**集中存储（POSIX / NFS 共享卷）**与 **S3 兼容对象存储**。

> 面向需要在私有环境自托管、可审计、可多实例横向扩展的部署场景：一个进程、一份带注释的
> JSON 配置、一套会失败的门槛与可追溯的证据，覆盖从元数据到数据面的完整文件服务。

`19 个 REST 端点` · `17 个 gRPC RPC（含流式上传 / 下载 / 服务端拷贝）` · `2 种存储后端` ·
`多实例：PG 原子领取 + 在途租约 + 领导者选举` · `1 GiB 级流式传输，内存占用与文件大小无关`

---

## 目录

- [1. 这是什么](#1-这是什么)
- [2. 能力总览](#2-能力总览)
- [3. 架构一览](#3-架构一览)
- [4. 快速开始](#4-快速开始)
- [5. 质量与验证](#5-质量与验证)
- [6. 项目状态与已知边界](#6-项目状态与已知边界)
- [7. 运维要点](#7-运维要点)
- [8. 深入阅读](#8-深入阅读)
- [9. 目录结构](#9-目录结构)
- [10. 第三方依赖与许可](#10-第三方依赖与许可)
- [11. 参与开发](#11-参与开发)

---

## 1. 这是什么

OSDU（开放地下数据宇宙标准）的 **File Service** 负责文件记录的**元数据**与
"**上传地址 / 下载地址**"的两段式数据面：客户端先向服务索取签名上传地址、直传存储，
再提交元数据记录；下载同理（索取签名下载地址 → 直连存储读取）。

**fssvrcpp** 是这套接口的一个 C++20 实现，关注点是"**合规面严格对齐 + 数据面可控 + 多实例可扩展**"：

| 关注点 | 做法 |
| --- | --- |
| 合规面 | REST 路径、PascalCase 字段名、状态码、错误体、请求头严格对齐上游；逐条写成**接口契约**，并驱动上游样例做逐字比对 |
| 扩展面 | gRPC 作为**平台外扩展**（独立端口 / 独立命名空间），与 REST 共享同一应用层，语义一致性由**双协议等价性矩阵**强制 |
| 存储 | 集中存储与对象存储走**同一套端口契约**；切换只改配置，不改代码 |
| 交付 | 单二进制 + 一份带注释的 JSON 配置 + 容器镜像；不引入 YAML 解析器、不依赖运行时框架 |
| 一致性 | 多实例不是"能起来就行"：幂等、原子领取、在途租约、单例 GC、崩溃回收都被测试与证据钉住 |

> **关于"OSDU 是否支持 RPC"**：上游规范**没有** RPC 面 —— 实测上游仓库 0 个 `.proto`，
> 全部是 Spring `@RestController`。本项目的 gRPC 面是**平台外扩展**，为流式上传/下载与
> 服务间调用提供一条不破坏 OSDU 合规性的通道。取证与取舍见
> [ADR-001](docs/adr/ADR-001-rpc-as-extension.md) 与 [docs/01-osdu-research.md](docs/01-osdu-research.md)。

---

## 2. 能力总览

| 能力 | 说明 | 入口 |
| --- | --- | --- |
| **OSDU 兼容 REST 面** | 19 个端点：上传/下载地址、位置查询、元数据 CRUD、DMS storage/retrieval/copy、Delivery、revokeURL 等；另有 `/metrics` 与探针端点 | [接口契约](docs/03-api-contract.md) |
| **gRPC 扩展面** | 17 个 RPC = 14 个一元 + 3 个流式（`UploadFile` 客户端流、`DownloadFile` 服务端流、`ServerSideCopy`） | [proto](proto/osdu/file/v1/file_service.proto) · [契约 §6](docs/03-api-contract.md) |
| **双存储后端** | POSIX（本地 / NFS 共享卷）与 S3 兼容对象存储（AWS S3 / MinIO / Ceph RGW / SeaweedFS / 华为 OBS…）；自研 SigV4 签名器**以 AWS 官方测试向量对拍**（差异项逐条附理由，并用独立的 mock 验签） | [ADR-003](docs/adr/ADR-003-storage-abstraction.md) · [ADR-005](docs/adr/ADR-005-s3-driver.md) |
| **大文件数据面** | 1 GiB 级流式上传/下载/搬迁，**内存占用与文件大小无关**（实测见容量文档）；`Range` 断点续传；自签传输 token；`sendfile` 下载面；两阶段批提交的耐久性协议 | [ADR-006](docs/adr/ADR-006-large-file-data-plane.md) · [ADR-008](docs/adr/ADR-008-write-durability-protocol.md) |
| **多实例一致性** | PostgreSQL 原子领取（幂等键唯一约束）+ 在途租约 + 会话级 advisory lock **领导者选举** + GC 单例门控 + 共享挂载探针 + 崩溃者回收 | [ADR-009](docs/adr/ADR-009-multi-instance-consistency.md) |
| **认证与多租户** | 本地 JWT（HS256，租户绑定 + 角色 claim）+ 远端 Entitlements（fail-closed）+ 审计（actor / 对象 / 结果 / correlation-id） | [ADR-012](docs/adr/ADR-012-auth-and-tenant-binding.md) |
| **可观测与运维** | 结构化日志、`/metrics`、`/v2/info`（版本 / 驱动 / 实例 id / 能力）、readiness 与 liveness 语义、配置全表（157 键，含"生效 / 拒绝启动 / 已读但无效果"三态）、故障处置手册 | [运维手册](docs/operations.md) · [故障处置](docs/runbook.md) |
| **质量工程** | 11 个阶段门槛（每阶段有判据、命令与证据）、ASan/UBSan/LSan、性能方法学（独立进程 + 绑核）、关键断言的"自证对照" | [阶段证据](docs/test-evidence/phase0.md) |

---

## 3. 架构一览

五层结构，**依赖方向由 CMake 目标图在编译期强制**（领域层看不见 gRPC / protobuf / SQLite 头文件，违反即编译失败）：

```
L5 适配层     REST（cpp-httplib + fss_http 强化包装层）      gRPC（14 一元 + 3 流式）
                 ▲                                          ▲
L4 应用层     用例（创建 / 搬迁 / 删除 / 查询 / DMS / GC…） + 纯策略（键 / 过期 / 能力护栏）
                 ▲
L3 领域层     领域模型 · 契约（错误投影表） · 15 个端口        ← 不认识任何基础设施
                 ▲
L2 基础设施   存储驱动（POSIX / S3）· 仓储（SQLite / PostgreSQL）· 认证 · 传输 · IO 引擎
                 ▲
L1 通用库     result / json / logging / bytes / fs / time / ids / config / crypto / net / sys / http
```

- **端口与实现分离**：L3 只声明 15 个端口，L2 用不同后端实现同一套端口契约 —— 内存、SQLite、
  PostgreSQL、POSIX、S3 各自跑**同一份契约测试**。
- **单一装配点**：所有具体实现只允许在组合根 `src/main/` 创建；适配层只拿端口（这条纪律有护栏自证）。
- **两条协议同源**：REST 与 gRPC 的错误映射与线上形状来自**同一张权威表**，由等价性矩阵钉住。

细节见 [架构设计](docs/02-design.md) 与 [最终方案](docs/00-final-design.md)。

---

## 4. 快速开始

### 4.1 依赖（源码构建）

| 依赖 | 说明 |
| --- | --- |
| g++ ≥ 11 / CMake ≥ 3.16 / GNU Make | C++20；无需 clang / ninja |
| gRPC + protobuf（含 `protoc`、`grpc_cpp_plugin`） | gRPC 扩展面（可用 `-DFSS_ENABLE_GRPC=OFF` 关闭） |
| OpenSSL / SQLite3 / libcurl | 签名、位置与元数据仓储、S3 数据面与出站 HTTP |
| libpq（可选） | 多实例（PostgreSQL 仓储 / 租约 / 领导者选举）；不装则这些目标不构建 |
| nlohmann/json、Catch2、cpp-httplib 0.26.0 | **已 vendored** 在 `third_party/`；⚠️ 不要用发行版的 cpp-httplib（老版本有 `Range` 下溢缺陷） |

### 4.2 构建与自检

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"

ctest --test-dir build -L phase0      # 环境与工具链
./scripts/check_docs.sh               # 文档一致性（链接 / ADR 索引 / 阶段表）
./scripts/run_all_gates.sh            # 全部已启用阶段门槛（P0~P10，失败即停）
```

`third_party/` 缺失时的恢复方式见 [third_party/README.md](third_party/README.md)；
开发者细节（依赖版本实测、PG 测试基建、sanitizer、多实例实验手册）见 [docs/development.md](docs/development.md)。

### 4.3 容器镜像

```bash
scripts/verify_image.sh     # 自动：准备最小构建上下文 → docker build → 启动 → 断言 readiness / HEALTHCHECK
```

> 镜像需要**最小构建上下文**（仓库根还有 `build/`，直接以根为上下文会把数 GB 产物算进去）；
> `scripts/verify_image.sh` 会自动准备。镜像内 production 配置**强制** `auth.mode=jwt`。

### 4.4 起一个实例

```jsonc
// fss.json —— 配置是"带注释的 JSON"（不支持 YAML，也不支持尾随逗号）
{
  "server":   {"http": {"bind": "0.0.0.0", "port": 8080}},
  "storage":  {"driver": "posix", "posix": {"root": "/var/lib/fss"}},
  "location": {"sqlite": {"path": "/var/lib/fss/location.db"}},
  "metadata": {"sqlite": {"path": "/var/lib/fss/metadata.db"}},
  "self_signed": {"signing_key": "change-me"},
  "auth": {"mode": "jwt", "jwt": {"hmac_secret": "change-me"}}
}
```

```bash
./build/bin/fss_server --config fss.json
# 全部配置键（157 个）与三态说明：docs/operations.md；完整样例：config/fss.example.json
```

> **配置来源与优先级**（高 → 低）：`--set` 命令行覆盖 → 环境变量（通用名，如 `server.http.port`
> 对应 `FSS_SERVER_HTTP_PORT`）→ 环境变量旧别名（`FSS_HTTP_PORT` 等，deprecated）→ `--config`
> 指定的配置文件 → 内置默认值。逐条说明见 [docs/operations.md](docs/operations.md) §1.1；
> `--print-config` 可打印每个键最终取值与来源。

### 4.5 端到端跑一遍

```bash
# ① 索取上传地址
curl -s -H 'authorization: Bearer <token>' -H 'data-partition-id: opendes' \
  http://localhost:8080/api/file/v2/files/uploadURL
#   → {"FileID":"…","Location":{"SignedURL":"http://…/v1/transfer/…","FileSource":"/osdu-user/…"}}

# ② 直传存储（PUT 到 ① 返回的 Location.SignedURL）
curl -T ./sample.bin '<SignedURL>'

# ③ 提交元数据（把 ① 的 FileSource 填进记录；完整记录样例见
#    tests/conformance/fixtures/File_CorrectPayload.json）
curl -s -X POST -H 'content-type: application/json' \
  -H 'authorization: Bearer <token>' -H 'data-partition-id: opendes' \
  --data @record.json http://localhost:8080/api/file/v2/files/metadata

# ④ 索取下载地址并下载
curl -s -H 'authorization: Bearer <token>' -H 'data-partition-id: opendes' \
  http://localhost:8080/api/file/v2/files/<record-id>/downloadURL
```

> `auth.mode=disabled` 仅供开发与测试；生产启动会强制校验鉴权配置。

---

## 5. 质量与验证

本项目的判据不是"看起来能用"，而是**会失败的门槛 + 可追溯的证据**：

| 做法 | 说明 |
| --- | --- |
| 每阶段一道门槛 | P0~P10，任一阶段失败即停；每阶段的命令、输出摘要、结论与**发现并修复的缺陷**都写入 [docs/test-evidence/](docs/test-evidence/phase0.md) |
| 自证对照 | 关键断言配"注入错误实现后必须失败"的对照（护栏、防护、门槛脚本自身都有），避免"通过"是因为判据恒真 |
| 内存与未定义行为 | `scripts/run_sanitizers.sh`：ASan + UBSan + LeakSanitizer 全量构建并跑全部阶段 |
| 性能方法学 | 负载生成器**独立进程 + 绑核**；容量模型与实测见 [docs/05-capacity-and-concurrency.md](docs/05-capacity-and-concurrency.md) |
| 多实例实验 | 容器里搭 NFSv4.2 + PostgreSQL + 双实例的完整步骤与踩坑清单：[docs/lab-nfs-multiinstance.md](docs/lab-nfs-multiinstance.md) |
| 一键复现 | `./scripts/check_docs.sh && ./scripts/run_all_gates.sh`（PG 基建门槛加 `FSS_GATES_WITH_PG=1`） |

---

## 6. 项目状态与已知边界

**阶段进度**（判据、命令与证据逐条见 [docs/phase-status.md](docs/phase-status.md) 与 [实现计划](docs/04-implementation-plan.md)）：

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| P0 | 环境与契约可行性 | ✅ |
| P1 | 分层骨架 + 12 个通用库 + HTTP 传输层 | ✅ |
| P2 | 领域模型 + 15 个端口 + 应用层纯逻辑 | ✅ |
| P3 | 集中存储驱动 + 位置仓储 + 数据面 | ✅ |
| P4 | REST 适配层 + 端到端垂直切片（POSIX） | ✅ |
| P5 | 对象存储驱动（S3 SigV4） | ✅ |
| P6 | 元数据语义完整化（版本链 / 校验和 / 12 步序列 / DMS） | ✅ |
| P7 | gRPC 适配层 + 双协议等价性 | ✅ |
| P8 | 认证授权与多租户 | ✅ |
| P9 | 硬化与交付（故障注入、指标、GC、镜像、运维手册） | ✅ |
| P10 | 配置面接线（157 键全量接通与三态核对） | ✅ |

**已知边界（诚实登记，不外推）**：

- 没有与**真实 S3 / MinIO** 做过端到端（签名与线协议由官方测试向量 + 自建 mock 钉住）；
- NFS 语义（`rename` 原子性 / close-to-open 一致性 / `syncfs` 范围）已在**容器中的 Linux knfsd**
  上实测，但**真实 NFS 设备与跨主机**仍须在目标环境复核；
- **断电耐久性**未验证（需要断电控制）；
- 认证面目前只交付本地 HS256 JWT：RS256/JWKS、多密钥轮换、真实 Entitlements 联调未做；
- `metadata.repository=remote`（可选实现）只交付 `static` token，且**不支持多实例**；
- 尚未测量：`syncfs` 的跨实例干扰量级、多租户并发流控调优、真实硬件上的容量基线。

---

## 7. 运维要点

- **配置**：157 个叶子键，逐个标注"生效 / 拒绝启动（fail-closed）/ 已读但无效果"；`--print-config`
  打印每个键的取值与来源（文件 / 默认）。见 [docs/operations.md](docs/operations.md)。
- **探针语义**：liveness = 进程活着；readiness = 依赖（存储、PostgreSQL、迁移版本）与**跨实例一致性**
  （共享挂载探针、对端服务版本 / 配置哈希）都正常 —— 多实例下不一致会 fail-closed。
- **单例任务**：GC 由**领导者选举**门控（PostgreSQL 会话级 advisory lock），只有 leader 真跑；
  另有按需端点 `POST /v2/gc:run`（同样受门控）。
- **故障处置**：[docs/runbook.md](docs/runbook.md) —— 症状 → 诊断 → 处置，含明确禁令。
- **多实例前置**：共享存储（NFS 或等价共享卷）+ PostgreSQL；启动会校验"每 partition 独占文件系统"
  等部署断言，不满足即拒绝启动（exit 78）。

---

## 8. 深入阅读

| 想了解 | 看这里 |
| --- | --- |
| 最终方案（只看一份） | [docs/00-final-design.md](docs/00-final-design.md) |
| OSDU 上游真实接口（一手调研） | [docs/01-osdu-research.md](docs/01-osdu-research.md) |
| 架构分层、端口、威胁模型、风险登记 | [docs/02-design.md](docs/02-design.md) |
| **接口契约**（实现与测试的唯一基准） | [docs/03-api-contract.md](docs/03-api-contract.md) |
| 阶段划分与逐条门槛判据 | [docs/04-implementation-plan.md](docs/04-implementation-plan.md) |
| 容量 / 并发实测与方法学教训 | [docs/05-capacity-and-concurrency.md](docs/05-capacity-and-concurrency.md) |
| 各阶段状态明细与未交付 / 未验证登记 | [docs/phase-status.md](docs/phase-status.md) |
| 配置全表、可观测性、安全 | [docs/operations.md](docs/operations.md) |
| 故障处置（症状 → 诊断 → 处置） | [docs/runbook.md](docs/runbook.md) |
| 开发者：环境事实、构建、PG 基建、陷阱 | [docs/development.md](docs/development.md) |
| 容器里搭 NFS × 多实例实验室 | [docs/lab-nfs-multiinstance.md](docs/lab-nfs-multiinstance.md) |
| 关键决策记录（含被推翻的结论） | [docs/adr/](docs/adr/ADR-001-rpc-as-extension.md) |
| **协作者硬约束**（工作流、铁律、已知陷阱） | [AGENTS.md](AGENTS.md) |

---

## 9. 目录结构

```
src/
  common/      L1 通用库（result / json / logging / bytes / fs / time / ids / config / crypto / net / sys / http）
  domain/      L3 领域层：模型、契约（错误投影表）、15 个端口
  app/         L4 应用层：用例与纯策略
  infra/       L2 基础设施：blob（posix / s3）、location、metadata、auth、transfer、io、postgres…
  adapters/    L5 协议适配：http、grpc
  main/        组合根（唯一允许创建具体实现的地方）
proto/         gRPC 契约（平台外扩展）
tests/         单元 / 集成 / 门槛测试 + 契约测试基类 + 上游 conformance 样例
db/            迁移脚本与数据库层不变量测试
config/        带注释的 JSON 配置样例（与运维文档一一对应）
scripts/       门槛、自证、运维、实验脚本
docs/          设计、契约、计划、运维、证据、ADR、实验室手册
third_party/   vendored 依赖（nlohmann/json、Catch2、cpp-httplib 0.26.0）
```

### 数据来源（测试数据与样例）

- **仓库内不含任何真实业务数据**。单元/集成测试的输入都在运行时临时生成，用完即删。
- `tests/conformance/fixtures/upstream/` 里的 JSON 样例来自 **OSDU 上游开源仓库**（Apache-2.0），
  只用于"逐字对齐上游消息"的契约测试；来源与 commit 在 `docs/test-evidence/phase4.md` 里登记。
- 大文件 / 容量类测试的输入由脚本在本地生成（稀疏文件或随机数据），不随仓库分发。
- 如需共享真实样本：**放共享盘**（内部约定 `/data4`），仓库里只写路径与清单，不入库原始数据。

---

## 10. 第三方依赖与许可

| 组件 | 形态 | 许可 |
| --- | --- | --- |
| nlohmann/json 3.10.5 | vendored（`third_party/nlohmann/`） | MIT |
| Catch2 2.13.8 | vendored（`third_party/catch2/`） | BSL-1.0 |
| cpp-httplib 0.26.0 | vendored 源码单头文件（`third_party/httplib.h`） | MIT |
| gRPC / protobuf / OpenSSL / SQLite / libcurl / libpq | 系统依赖（见 §4.1） | 各自上游许可 |

版本与校验和见 `third_party/CHECKSUMS.txt` 与 [third_party/README.md](third_party/README.md)。

> ⚠️ 本仓库**尚未添加自身的 LICENSE 文件** —— 若要对外发布或接受外部贡献，请先补上并在此声明。

---

## 11. 参与开发

1. **先读 [AGENTS.md](AGENTS.md)**：它是本仓库对协作者的硬约束（环境约束、工作流契约、铁律、
   已知陷阱、会被机械检查的规则）。
2. 改动遵循"**先让测试失败，再实现**"；新增判据要能自证（把实现改错后必须失败）。
3. 收工前必跑 `./scripts/check_docs.sh` 与 `./scripts/run_all_gates.sh`，并按约定把命令、输出摘要、
   结论与缺陷写入 `docs/test-evidence/`。
4. 涉及接口 / 协议 / 配置的改动必须同步更新契约、`config/fss.example.json` 与运维文档（有自动比对测试）。
