# 架构设计文档

> 输入：`docs/01-osdu-research.md`（OSDU 一手调研）
> 相关：`docs/03-api-contract.md`（接口契约）、`docs/04-implementation-plan.md`（实现计划）
> 决策记录：`docs/adr/ADR-001` ~ `ADR-004`

---

## 1. 目标与非目标

### 1.1 目标

| # | 目标 | 可验证的验收标准 |
| --- | --- | --- |
| G1 | 提供 **OSDU File Service 兼容的 REST 接口** | `tests/conformance/` 中基于 OSDU 黄金样例的契约测试全部通过；路径/字段名/状态码与 `docs/03-api-contract.md` 逐条一致 |
| G2 | 同时支持 **集中存储（POSIX）** 与 **对象存储（S3 兼容）** | 同一套契约测试分别对两种驱动各跑一遍并全部通过；切换只改配置，不改代码 |
| G3 | 提供 **HTTP 与 RPC 两种协议接口** | 同一操作经两条链路产出的领域结果与错误分类等价（双协议等价性测试通过） |
| G4 | **分层设计，模块间低耦合** | 依赖方向由 CMake 目标图在**编译期强制**；存在自动化护栏测试，违反即构建失败 |
| G5 | **每一步完成先测试，通过再进入下一步** | 每个阶段有唯一的自动化门槛命令与通过判据；CI 中阶段门槛是前置依赖 |
| G6 | C++20 实现，依赖最小化 | OpenSSL + SQLite3 + libcurl + protobuf/gRPC + nlohmann/json + Catch2 + cpp-httplib（源码 vendored，仅作 HTTP 传输层，见 ADR-002） |

### 1.2 非目标（明确排除，避免范围蔓延）

| # | 非目标 | 原因 |
| --- | --- | --- |
| N1 | 不实现 Schema / Legal / Entitlements / Partition / Storage 服务本身 | 它们是独立 OSDU 服务；本项目通过**端口**对接（本地实现 + 远端实现） |
| N2 | 不追求 OSDU 官方认证/合规证书 | 认证需要完整平台环境；本项目目标是"接口兼容 + 可对接" |
| N3 | 不实现多部分上传（`multipart/form-data`）的完整协议协商 | OSDU File Service 不使用该模式；数据面走签名 URL 或流式 RPC |
| N4 | 不实现 HTTP/2 与 TLS 终止（首版） | 由前置反向代理/服务网格承担；RPC 面本身是 HTTP/2 |
| N5 | 不实现 `PUT /v2/files/{id}/metadata` | 上游实测**不存在**该端点（见调研报告 §4.2），实现它反而制造不兼容 |
| N6 | 不实现 Reserve DDMS 的 ETP / GraphQL | 领域不相关；记录为未来可选（ADR-001 方案 D） |
| N7 | 不实现文件内容解析/校验（如 CSV/WITSML 语义校验） | OSDU 文档明确：File Service 不检查文件内容 |
| N8 | 不做分布式部署的存储一致性协议（如跨节点副本） | 集中存储依赖底层 NFS/共享卷；对象存储依赖 S3 自身一致性 |

---

## 2. 需求到设计的映射

```
需求                                   设计承载
────────────────────────────────────────────────────────────────────────────
"基于文件的后端服务"              →  以 file_id 为聚合根的位置记录 + 元数据记录
"支持集中存储"                    →  PosixBlobStore + /v1/transfer 自签数据面
"支持对象存储"                    →  S3BlobStore (SigV4 预签名, libcurl 数据面)
"两种方式可切换"                  →  IBlobStore 端口 + BlobCapabilities + 配置(含按分区覆盖)
"C++ 开发"                        →  C++20, CMake, 目标图强制分层
"提供 http 和 rpc 两种协议"       →  adapters/http (OSDU REST) + adapters/grpc (扩展)
"整体分层、降低耦合"              →  L1..L5 五层 + 端口/适配器 + 编译期依赖护栏
"每步完成先测试"                  →  10 个阶段各带自动化门槛命令与判据
```

---

## 3. 总体架构

### 3.1 分层视图

```
┌───────────────────────────────────────────────────────────────────────────────┐
│ L5  协议适配层 (Protocol Adapters)                        —— 薄、可替换、无业务  │
│                                                                               │
│   adapters/http/                        adapters/grpc/                        │
│   ├─ OsduFileMetadataHandler            ├─ FileServiceGrpcAdapter             │
│   ├─ OsduLocationHandler                ├─ GrpcErrorMapper                    │
│   ├─ OsduDmsHandler                     ├─ GrpcStreamingIO                    │
│   ├─ OsduDeliveryHandler                └─ (proto <-> app DTO 转换)           │
│   ├─ OsduOpsHandler                                                           │
│   ├─ HttpErrorMapper                                                          │
│   ├─ DtoJson (PascalCase 序列化)                                              │
│   └─ (HTTP DTO <-> app DTO 转换)                                              │
│   ↑ 全部挂在 base_path（默认 /api/file）之下                                   │
│                         │                              │                      │
│                         └──────────┬───────────────────┘                      │
│                                    ▼  只依赖 L4 的用例接口 (纯 C++ 结构体)      │
├───────────────────────────────────────────────────────────────────────────────┤
│ L4  应用层 (Application / Use Cases)                       —— 编排、无 IO 细节  │
│                                                                               │
│   app/usecases/                                                               │
│   ├─ GetUploadLocation      ├─ CreateFileMetadata   ├─ GetFileMetadata        │
│   ├─ GetDownloadLocation    ├─ DeleteFileMetadata   ├─ GetFileList            │
│   ├─ GetStorageInstructions ├─ GetRetrievalInstructions ├─ CopyFiles          │
│   ├─ RevokeUrl              ├─ UploadFile(stream)   └─ DownloadFile(stream)   │
│   app/services/                                                               │
│   ├─ LocationIssuer         ★ 能力收敛点：按 BlobCapabilities 决定地址形态     │
│   ├─ ObjectKeyPolicy        ★ file_source <-> ObjectRef 映射 + 日期分层        │
│   ├─ ExpiryPolicy           ★ expiryTime 解析/默认/上限 (1h 默认, 7d 上限)     │
│   ├─ ChecksumPolicy         ★ 校验和算法与编码                                  │
│   └─ MetadataAssembler      ★ 默认 kind / id / acl / legal 组装                │
│                                    │                                          │
│                                    ▼  只依赖 L3 的领域类型与端口接口            │
├───────────────────────────────────────────────────────────────────────────────┤
│ L3  领域层 (Domain)                          —— 零外部依赖（无 protobuf/HTTP/DB）│
│                                                                               │
│   domain/model/    FileLocation, FileMetadataRecord, FileSourceInfo, Acl,     │
│                    Legal, Ancestry, StorageZone, SignedLocation, BlobRef      │
│   domain/ports/    IBlobStore(+BlobCapabilities)  IBlobStoreFactory           │
│                    IFileLocationRepository        IMetadataRepository         │
│                    IPartitionRegistry             IAuthorizer                 │
│                    ILegalValidator                ISchemaValidator            │
│                    IEventPublisher                IAuditLogger                │
│                    IClock                         IIdGenerator                │
│                    ISelfSignedUrlCodec            ILeaseRepository            │
│                    IIoEngine                      (共 15 个端口)              │
│   domain/error/    ErrorKind (规范错误分类) + Result<T>                        │
│                                    ▲                                          │
│                                    │ 实现（依赖倒置）                          │
├────────────────────────────────────┼──────────────────────────────────────────┤
│ L2  基础设施适配层 (Infrastructure) │                    —— 每个端口一个实现目录 │
│                                                                               │
│   infra/blob/posix/     PosixBlobStore            (集中存储)                   │
│   infra/blob/s3/        S3BlobStore + SigV4Signer (对象存储)                   │
│   infra/blob/memory/    InMemoryBlobStore         (测试用，契约测试基准)         │
│   infra/location/sqlite/ SqliteLocationRepository                              │
│   infra/metadata/sqlite/ SqliteMetadataRepository (含版本链)                   │
│   infra/metadata/remote/ RemoteStorageServiceRepository (对接 OSDU Storage)    │
│   infra/auth/local/      LocalJwtAuthorizer + StaticRoleTable                  │
│   infra/auth/remote/     RemoteEntitlementsAuthorizer                          │
│   infra/legal/           NoopLegalValidator / RemoteLegalValidator             │
│   infra/partition/       FilePartitionRegistry / RemotePartitionRegistry       │
│   infra/event/           LogEventPublisher / HttpWebhookEventPublisher          │
│   infra/transfer/        SelfSignedUrlIssuer + TransferTokenCodec ★ 集中存储数据面│
│                                    │                                          │
│                                    ▼  只依赖 L1                                 │
├───────────────────────────────────────────────────────────────────────────────┤
│ L1  通用库 (Common)                          —— 与业务无关、可独立复用的技术能力 │
│                                                                               │
│   common/result/    Result<T>, Error, ErrorKind                                │
│   common/json/      nlohmann/json 封装 + PascalCase/camelCase 序列化辅助        │
│   common/http/      ★ fss_http：httplib 之上的强化包装层(上限/Range/中间件/归一化)│
│   common/crypto/    HMAC-SHA256, SHA-256, base64url, 常量时间比较               │
│   common/ids/       UUIDv4 / ULID                                               │
│   common/time/      IClock 实现 + ISO-8601 / RFC-1123 解析与格式化              │
│   common/fs/        原子写（tmp+fsync+rename）、安全路径校验、目录遍历            │
│   common/config/    配置加载（文件 + 环境变量 + 命令行覆盖）、校验               │
│   common/logging/   结构化日志（JSON 行）、级别、correlation-id 透传             │
│   common/net/       Url 解析/构造、查询字符串编解码、百分号编码                   │
│   common/bytes/     ByteSource / ByteSink / Range 抽象、缓冲、限流              │
│   common/sys/       系统能力探测（io_uring 可用性 → 引擎选择依据）              │
│   common/metrics/   ★ fss_metrics：最小指标注册表（Prometheus 文本；P9/C9.6）  │
└───────────────────────────────────────────────────────────────────────────────┘
```

### 3.2 依赖规则（唯一允许的方向：向下）

```
L5 ──▶ L4 ──▶ L3 ◀── L2 ──▶ L1
              ▲      │
              └──────┘   (L2 实现 L3 定义的端口：依赖倒置)
L4 ──▶ L1 ✅        L3 ──▶ L1 ✅        L2 ──▶ L1 ✅
L3 ──▶ L2 ❌  L3 ──▶ L5 ❌  L4 ──▶ L2 ❌  L4 ──▶ L5 ❌  L1 ──▶ L2/L3/L4/L5 ❌
```

**禁止的依赖（会用自动化测试检查）**：

| 禁止项 | 为什么 | 检查方式 |
| --- | --- | --- |
| `domain/*` 依赖 `protobuf` | 领域层必须能在无 gRPC 环境下编译与单测 | 目标图（`fss_domain` 不链接 `fss_proto`）+ 源码检索护栏测试 |
| `domain/*`、`app/*` 依赖 `common/http` | 领域/应用逻辑不应知道 HTTP | 目标图 + 源码检索护栏测试 |
| `domain/*`、`app/*` 依赖 `sqlite3`、`libcurl`、`openssl` | 同上，保证可用内存实现单测 | 目标图 + 源码检索护栏测试 |
| `adapters/http` 直接依赖 `adapters/grpc` | 两个适配器必须彼此独立 | 目标图 |
| 任何层绕过端口直接 `new` 具体实现 | 保证可替换、可测试 | 组合根 `src/main/` 是唯一允许实例化具体实现的位置；护栏测试检查 `new XxxBlobStore` 只出现在 `main/` 与测试中 |

### 3.3 组合根（Composition Root）

所有具体实现的实例化集中在 `src/main/` 的**唯一**位置：

```cpp
// src/main/server_main.cpp （示意）
AppConfig cfg = LoadConfig(argc, argv);

// 1) L1
auto clock  = std::make_shared<SystemClock>();
auto ids    = std::make_shared<UuidGenerator>();

// 2) L2：按配置 + 分区解析存储驱动
auto blob_store_factory = MakeBlobStoreFactory(cfg);        // 返回按 partition 解析的工厂
auto location_repo = MakeLocationRepository(cfg, /*sqlite*/);
auto metadata_repo = MakeMetadataRepository(cfg);
auto authorizer    = MakeAuthorizer(cfg);
// ...

// 3) L3/L4：注入端口
auto location_service = std::make_shared<app::LocationService>(...);
// ...

// 4) L5：注册到两个协议适配器
http::Router router;
adapters::http::RegisterOsduRoutes(router, *location_service, *metadata_service, ...);
grpc::ServerBuilder grpc_builder;
grpc_builder.RegisterService(MakeGrpcService(...).release());

// 5) 启动两个监听器
```

**这条规则是"降低耦合"最实际的抓手**：只要"具体实现只能在组合根创建"，任何模块都无法偷偷耦合到另一个模块的实现细节上。

---

## 4. 编译期护栏：CMake 目标图

```
                    ┌─────────────────┐
                    │   fss_server    │  (可执行文件 = 组合根)
                    └────────┬────────┘
        ┌────────────────────┼────────────────────┬──────────────────┐
        ▼                    ▼                    ▼                  ▼
┌───────────────┐   ┌────────────────┐   ┌──────────────┐   ┌──────────────┐
│fss_grpc_adapter│  │fss_http_adapter│   │fss_infra_*   │   │  fss_proto   │
└───────┬───────┘   └───────┬────────┘   └──────┬───────┘   └──────┬───────┘
        │                   │                    │                  │
        ├───────────────────┴────────────────────┘                  │
        ▼                                                            │
┌───────────────┐   ┌───────────────┐   ┌──────────────┐            │
│   fss_app     │   │   fss_http    │   │fss_crypto/…  │◀───────────┘
└───────┬───────┘   └───────┬───────┘   └──────┬───────┘  ← fss_proto 只被
        ▼                   │                   │            fss_grpc_adapter 链接
┌───────────────┐           │                   │
│  fss_domain   │           │                   │
└───────┬───────┘           │                   │
        └─────────┬─────────┴───────────────────┘
                  ▼
          ┌───────────────┐
          │  fss_common   │  (INTERFACE，含 fss_warnings)
          └───────────────┘
```

**关键性质（编译器/链接器强制）**：

1. `fss_domain` 的 `target_link_libraries` **只有** `fss_common`。
   → 一旦有人在领域层 `#include <grpcpp/grpcpp.h>` 或 `<sqlite3.h>`，编译或链接立刻失败。
2. `fss_app` 只链接 `fss_domain` + `fss_common`。→ 应用层不可能碰到 HTTP/DB/protobuf。
3. `fss_proto` 只被 `fss_grpc_adapter`（和测试）链接。→ 领域层不可能依赖 proto。
4. `fss_http_adapter` 与 `fss_grpc_adapter` **互不链接**。→ 双协议无耦合。
5. `fss_infra_*` 只链接 `fss_domain` + `fss_common`（+ 各自的外部库）。
   → 基础设施不会反向依赖应用层。
6. `fss_server` 是唯一链接所有目标的 executable。

此外增加**源码检索护栏测试**（`tests/unit/test_layering_guard.cpp`，纯文本检索，秒级）：
- `src/domain/**` 中出现 `#include <grpc`、`<sqlite3.h>`、`<curl/`、`<openssl/` → 失败
- **`<httplib.h>` 只允许在 `src/common/http/` 内出现**，其他任何位置出现 → 失败（保证库可替换）
- `src/app/**` 同上 → 失败
- `src/domain/**`、`src/app/**` 中出现 `common/http` → 失败
- `src/adapters/http/**` 中出现 `osdu/file/v1`（proto 头）→ 失败
- 在 `src/` 中（除 `src/main/`）出现 `new ` + 具体实现类名白名单 → 警告/失败

> 为什么两道护栏都要：目标图防"能编译但设计违规"，源码检索防"通过头文件间接引入"。两者结合才能让"低耦合"成为**可执行的约束**而不是口号。

---

## 5. 领域模型与端口清单

### 5.1 领域模型（L3，`src/domain/model/`）

```cpp
namespace fss::domain {

enum class StorageZone { kStaging, kPersistent };
enum class StorageDriver { kPosix, kS3 };

// 物理对象引用（驱动无关）。container = POSIX 目录 / S3 bucket
struct ObjectRef {
  std::string container;
  std::string key;
};

// 对客户端可见的相对路径（OSDU 的 fileSource / FileSource）
// 不可与物理路径混淆：它由 ObjectKeyPolicy 映射到 ObjectRef
struct FileSource { std::string value; };

// 位置记录：fileID -> (fileSource, ObjectRef, zone, driver) 的登记项
struct FileLocation {
  std::string file_id;
  FileSource  file_source;
  ObjectRef   object;
  StorageZone zone = StorageZone::kStaging;
  StorageDriver driver = StorageDriver::kPosix;
  std::int64_t created_at_epoch = 0;
  std::string  created_by;
};

// 签名/传输地址
struct SignedLocation {
  std::string url;
  std::string method;                 // "PUT" / "GET"
  std::int64_t expires_at_epoch = 0;
  std::map<std::string, std::string> required_headers;
  bool is_self_signed = false;        // true = 由本服务数据面提供
};

// 元数据记录（File.Generic，信封 + data）
struct Acl   { std::vector<std::string> viewers, owners; };
struct Legal { std::vector<std::string> legaltags, other_relevant_data_countries;
               std::string status; };
struct Ancestry { std::vector<std::string> parents; };

struct FileSourceInfo {
  std::string file_source;           // ★ JSON 名 "FileSource"
  std::string preload_file_path;     // "PreloadFilePath"
  std::string preload_file_create_user, preload_file_create_date;
  std::string preload_file_modify_user, preload_file_modify_date;
  std::string name, file_size, encoding_format_type_id;
  std::string checksum, checksum_algorithm;
};

struct FileData {
  std::string name, description, total_size;       // "Name" "Description" "TotalSize"
  std::string encoding_format_type_id, schema_format_type_id;
  std::string resource_home_region_id;
  std::vector<std::string> resource_host_region_ids;
  std::string resource_curation_status, resource_lifecycle_status,
              resource_security_classification, source;
  std::optional<FileSourceInfo> file_source_info;
  std::string existence_kind;
  std::string endian;                              // "BIG" / "LITTLE" / ""
  std::string checksum;
  Json open_extension_properties;                  // 开放字段原样保留
  Json additional_dataset_properties;              // 未知字段不丢失
};

struct FileMetadataRecord {
  std::string id, kind;
  Acl acl;
  Legal legal;
  FileData data;
  Ancestry ancestry;
  Json meta{Json::array()};                        // List<Object>
  std::map<std::string, std::string> tags;
  std::optional<std::int64_t> version;             // 仅响应/持久化时有值
};
}
```

**设计要点**：
- 领域模型**不使用** `nlohmann::json` 作为主要表示（避免 JSON 细节渗透），仅在明确标注"开放字段"处使用 `Json` 类型承载原样内容（保证**前向兼容**：上游新增字段不会被丢弃）。
- 所有 JSON 名称映射集中在 `adapters/*/dto` 与 `infra/metadata/*/codec` 的编码器里，**集中一处**，避免"字段名散落各处"导致的契约漂移。
- 时间统一用 **epoch 秒（int64）** 在领域内传递，格式化只发生在适配层。

### 5.2 端口清单（L3，`src/domain/ports/`）

| 端口 | 职责 | 本项目提供实现 | 是否必需 |
| --- | --- | --- | --- |
| `IBlobStore` | 对象存储原语 + 能力声明 | `PosixBlobStore`、`S3BlobStore`、`InMemoryBlobStore` | ✅ |
| `IBlobStoreFactory` | 按 `partition` 解析 `IBlobStore` | `ConfigBlobStoreFactory` | ✅ |
| `IFileLocationRepository` | 位置记录 CRUD + 列表 | `SqliteLocationRepository`、`InMemoryLocationRepository` | ✅ |
| `IMetadataRepository` | 元数据记录 CRUD + 版本链 | `SqliteMetadataRepository`、`RemoteStorageServiceRepository`、`InMemoryMetadataRepository` | ✅ |
| `IPartitionRegistry` | 租户配置（存储区地址、限额、特性开关） | `FilePartitionRegistry`、`RemotePartitionRegistry` | ✅ |
| `IAuthorizer` | 鉴权（角色判定） | `LocalJwtAuthorizer`、`RemoteEntitlementsAuthorizer`、`AllowAllAuthorizer`（仅测试） | ✅ |
| `ILegalValidator` | legal tag 校验 | `NoopLegalValidator`、`RemoteLegalValidator` | 可选 |
| `ISchemaValidator` | kind/schema 校验 | `NoopSchemaValidator`、`RemoteSchemaValidator` | 可选 |
| `IEventPublisher` | 状态变更事件（`status-changed` 语义） | `LogEventPublisher`、`HttpWebhookEventPublisher` | 可选 |
| `IAuditLogger` | 审计事件（对齐参考实现的 `AuditLogger`） | `StructuredLogAuditLogger` | ✅ |
| `IClock` ⓛ | 可注入时间（可测试性关键） | `SystemClock`、`ManualClock` | ✅ ⓛ |
| `IIdGenerator` ⓛ | fileID / recordID 生成 | `UuidGenerator`、`SequentialIdGenerator` | ✅ ⓛ |
| `ISelfSignedUrlCodec` | 集中存储的自签 URL 编解码 | `HmacTransferTokenCodec` | ✅ |
| `ILeaseRepository` | 在途租约的建立 / 续租 / **原子领取**（GC 依赖） | `PostgresLeaseRepository`、`InMemoryLeaseRepository`（测试） | 多实例必需（ADR-009） |
| `IIoEngine` | 文件 I/O 引擎抽象（`read`/`write`/`sync`/`stat`，64 位偏移 + 区间读） | `BlockingIoEngine`（默认）、`UringIoEngine`（可选，探测 + 回退） | ✅（ADR-010） |

> ⓛ **声明位置说明（实现阶段细化）**：`IClock` 与 `IIdGenerator` **声明在 L1**
> （`src/common/time/clock.h`、`src/common/ids/id_generator.h`），其余 13 个声明在 L3
> `domain/ports/`。理由：这两个接口**零依赖且无领域语义**（"现在几点""生成一个 ID"
> 都不是业务规则），而 L1 自身的测试也需要它们；若放 L3，L2 的实现要为一个无领域
> 语义的接口反向依赖 L3，收益为负。它们的默认实现与测试替身也都在 L1，因此
> **不破坏依赖方向**（L3 → L1 是允许的）。
>
> **端口数量的取舍**：端口只在"存在多个实现"或"必须在测试中替换"时才引入。
> 例如"上传地址业务"没有多实现需求，因此不设 `ILocationService` 端口，直接是 `app::LocationService` 具体类
> （这与 OSDU 参考实现不同——它把 `ILocationService` 也做成了接口，代价是 5 个方法用 `default` 空实现兜底，见调研报告 §6）。

---

## 6. 存储抽象与"双模式"（摘要）

完整论证见 **[ADR-003](adr/ADR-003-storage-abstraction.md)**。此处只列结论性设计。

### 6.1 端口签名

```cpp
namespace fss::domain {

struct BlobCapabilities {
  bool native_presign = false;      // 有原生预签名 URL（S3: true / POSIX: false）
  bool server_side_copy = false;    // 支持服务端复制
  bool range_read = false;          // 支持字节区间读取
  bool streaming_put = false;       // 支持流式写入
  std::size_t recommended_part_size = 0;
  std::string driver_name;          // 回填到 OSDU 响应的 Driver 字段
};

struct PresignOptions {
  std::int64_t expires_in_seconds = 3600;
  std::string  content_type;
  std::string  file_name;           // 仅用于 Content-Disposition
  std::string  method;              // "PUT" / "GET"
};

struct PutOptions {
  std::string content_type;
  std::string expected_checksum;
  std::string checksum_algorithm;
  std::int64_t expected_size = -1;  // -1 = 未知
};
struct Range { std::uint64_t offset = 0; std::uint64_t length = 0; }; // length=0 => EOF

class IBlobStore {
 public:
  virtual ~IBlobStore() = default;

  virtual BlobCapabilities capabilities() const = 0;
  virtual Result<void> ensure_container(const std::string& container) = 0;

  virtual Result<SignedLocation> presign_put(const ObjectRef&, const PresignOptions&) = 0;
  virtual Result<SignedLocation> presign_get(const ObjectRef&, const PresignOptions&) = 0;

  virtual Result<void>       put(const ObjectRef&, ByteSource&, const PutOptions&) = 0;
  virtual Result<void>       get(const ObjectRef&, ByteSink&, const Range&) = 0;
  virtual Result<ObjectStat> stat(const ObjectRef&) = 0;
  virtual Result<void>       remove(const ObjectRef&) = 0;
  virtual Result<ObjectStat> copy(const ObjectRef& from, const ObjectRef& to) = 0;
  virtual Result<ListPage>   list(const std::string& container,
                                 const std::string& prefix,
                                 const std::string& continuation_token,
                                 int limit) = 0;

  //  ★ C9.25（P9）：临时文件**永远不是对象**，所以 `list()` 看不见它们
  //    （`IsInternalKey` 跳过 `.tmp.` 与侧车）。GC 清残留必须走这个专用入口。
  //    返回结构而不是一个计数：临时文件对 `list()` 不可见，于是"看到但太新所以保护"
  //    这件事只有驱动知道 —— 只回删除数会让"在途上传被保护"在 POSIX 路径上不可观测。
  virtual Result<TempSweepResult> remove_temp_files(const std::string& container,
                                                   std::int64_t older_than_epoch_seconds,
                                                   bool dry_run) = 0;
};
}
```

`ByteSource` / `ByteSink` 是回调式流抽象：

```cpp
class ByteSource {                     // 生产者视角：给我一块缓冲，我来填
 public:
  virtual ~ByteSource() = default;
  // 返回读取字节数；0 表示 EOF；错误通过 Result 表达
  virtual Result<std::size_t> read(char* buf, std::size_t capacity) = 0;
  virtual Result<std::uint64_t> size() const { return 0; }  // 0 = 未知
};

class ByteSink {                       // 消费者视角：这块数据给你，写下去
 public:
  virtual ~ByteSink() = default;
  virtual Result<void> write(const char* buf, std::size_t len) = 0;
  virtual Result<void> finish() { return Ok(); }
};
```

> 这样 `IBlobStore::put/get` 天然支持"从 HTTP 请求体直接流到磁盘"与"从磁盘直接流到 HTTP 响应"，
> 中间不产生整文件内存副本 —— 这是大文件场景的正确形态；
> HTTP 侧由 cpp-httplib 的 `set_content_provider` / `ContentReader` 提供流式能力，
> 由 `fss_http` 包装层补上硬上限与 Range 归一化（ADR-002 §4 的 H-1/H-2 防护）。

### 6.2 两种模式的最终形态对照

| 关注点 | 集中存储（POSIX） | 对象存储（S3 兼容） |
| --- | --- | --- |
| 驱动 | `PosixBlobStore` | `S3BlobStore` |
| `native_presign` | `false` | `true` |
| 上传地址 | `https://<self>/v1/transfer/{token}?exp=…&sig=…`（自签，服务代理字节） | `https://<endpoint>/<bucket>/<key>?X-Amz-…&X-Amz-Signature=…`（原生预签名，客户端直连） |
| 下载地址 | 同上（`op=get`） | 原生预签名 GET |
| 响应 `Driver` | `"posix"` | `"s3"` |
| 服务带宽消耗 | 与文件大小成正比 | 零（除元数据） |
| 适用场景 | 私有化小/中规模、无对象存储、内网 | 云上/大规模、已有 S3/MinIO/Ceph/SeaweedFS |
| 关键测试 | 路径穿越、符号链接逃逸、原子写、Range 边界、自签 URL 过期/越权/重放 | SigV4 由 mock-S3 **独立验签**、path-style/virtual-host、分页、Region 处理 |

### 6.3 `LocationIssuer`：唯一的能力分支点

```cpp
// src/app/services/location_issuer.cpp （示意）
Result<LocationResponse> LocationIssuer::IssueUploadLocation(
    const std::string& partition, std::optional<std::string> file_id,
    const ExpirySpec& expiry) {
  const auto file_id_final = file_id ? *file_id : ids_->NewFileId();
  const auto zone = StorageZone::kStaging;

  FSS_TRY(auto store, blobs_->ForPartition(partition, zone));
  FSS_TRY(auto ref, keys_->NewObjectRef(partition, zone, file_id_final));

  // ① 只需保证容器存在
  FSS_TRY(store->ensure_container(ref.container));

  // ② 记录位置（先登记，保证后续 metadata 能查到；失败可回滚）
  FileLocation loc{file_id_final, keys_->ToFileSource(ref, zone), ref, zone,
                   driver_of(store->capabilities()), clock_->NowEpoch(), caller};
  FSS_TRY(locations_->Save(loc));

  // ③ 生成地址 —— 全项目唯一的能力分支点
  const auto caps = store->capabilities();
  SignedLocation signed_loc;
  if (caps.native_presign) {
    FSS_TRY(signed_loc, store->presign_put(ref, opts));            // 客户端直连存储
  } else {
    FSS_TRY(signed_loc, self_signed_->IssuePut(ref, partition, opts)); // 服务数据面
  }
  return LocationResponse{file_id_final, signed_loc, loc.file_source, ...};
}
```

**约束（护栏测试检查）**：`capabilities()` 的调用点仅允许出现在
`src/app/services/location_issuer.cpp` 与 `src/app/services/storage_instruction_service.cpp`。
其他任何位置调用 `capabilities()` → 护栏失败。这从机制上防止"能力分支扩散"。

---

## 7. HTTP 传输栈与 REST 适配层

### 7.1 HTTP 传输栈：cpp-httplib（vendored 源码）+ `fss_http` 强化包装层

决策依据见 **[ADR-002](adr/ADR-002-http-framework.md)**（该 ADR 的初版结论"完全自研"已被推翻）。

```
┌──────────────────────────────────────────────────────────────────┐
│ adapters/http：OSDU 路由 / DTO / 错误映射                          │
├──────────────────────────────────────────────────────────────────┤
│ fss_http  ← 本项目拥有（src/common/http/）—— 安全与正确性边界       │
│   Server 门面 · 硬上限 · 中间件链 · Range 归一化 · 错误归一化        │
├──────────────────────────────────────────────────────────────────┤
│ cpp-httplib 0.26.0（third_party/httplib.h，MIT，vendored 源码）      │
│   仅作传输层：TCP 监听 / HTTP1.1 解析 / 路由分发 / 写回 / 线程池      │
│   ⚠️ 视为不可信组件，见下方 H-1 / H-2 缺陷                         │
└──────────────────────────────────────────────────────────────────┘
```

**约束**：只有 `src/common/http/` 允许 `#include <httplib.h>`；
其余任何位置出现该 include → 分层护栏测试失败。这保证库可替换（换库不必触碰领域层/应用层/适配层）。

**为什么不直接裸用 httplib**：实测复现了它在本项目最关键路径（流式 + 上限/Range）上的两个缺陷：

| ID | 版本 | 缺陷 | 后果 | 处置 |
| --- | --- | --- | --- | --- |
| **H-1** | 0.10.3 | 流式响应 + 越界 `Range` 无 416 分支，`size_t` 下溢 | `Content-Length: 18446744072717940225`、`Content-Range` 倒序（非法）→ 客户端挂死/错帧 | **不使用 0.10.3**；升到 0.26.0（已修复，实测 416 正确） |
| **H-2** | 0.26.0 | `ContentReader` + `set_payload_max_length` 超限时 handler 仍被调用、reader 交出 **0 字节**、最终返回 **201** | **静默数据丢失**：上传"成功"而对象为空 | 包装层三重防护：① 按 `Content-Length` 前置拦截 → 413；② 自封装计数 reader，超限立即中止；③ 读取字节数与 `Content-Length` 一致性断言 |

复现源码与原始输出：`docs/appendix/httplib-hardening-probe/`；回归测试是阶段 1 门槛的一部分。

**`fss_http` 的职责（本项目的自有代码）**

| 职责 | 说明 |
| --- | --- |
| 门面隔离 | 对外只暴露 `fss::http::Server`/`Router`/`Request`/`Response`，不泄漏 httplib 类型 |
| 硬上限强制 | 请求头 / URI / 请求体（按路由配置）/ 并发连接数 / 空闲超时；超限一律拒绝且**不产生副作用** |
| 中间件链 | `CorrelationId` → `AccessLog` → `Auth` → `BodyLimit` → Handler → `ErrorMapping` |
| Range 归一化 | `bytes=a-b` / `bytes=a-` / `bytes=-N` / 多段 / 越界 → 统一为 `206` / `416`，与驱动无关 |
| 错误归一化 | 不把库的默认 `400` 直接漏给客户端，统一经 `http_error_mapper` 产出 `AppError` |
| 可测试面 | 所有边界都是**我们的**断言，而不是对第三方行为的信任 |

**HTTP 层硬上限与拒绝语义（按实测行为固化，避免"想当然"）**

| 场景 | 行为 | 依据 |
| --- | --- | --- |
| `Content-Length` > 路由上限 | **413** | 包装层前置拦截（H-2 防护①） |
| chunked 且累计 > 路由上限 | **400**（不是 413） | httplib 读失败语义；这是**实测行为**，已写入契约并由测试锁定 |
| 单请求头 > 8192 字节 | 400 | httplib `CPPHTTPLIB_HEADER_MAX_LENGTH` |
| URI > 8192 字节 | 400 | httplib `CPPHTTPLIB_REQUEST_URI_MAX_LENGTH` |
| 重复 `Content-Length` | 400 | 0.26.0 实测 |
| `Content-Length` 与 `chunked` 并存 | 400 | 0.26.0 实测 |
| 未知方法（已知路径） | **404**（不是 405） | 0.26.0 实测。**决定接受并写入契约**：OSDU 只定义已注册方法，兼容性无影响；若日后需要 405，在包装层用 `set_pre_routing_handler` 自行实现 |
| 未实现的 `Transfer-Encoding` | 400/501 | 包装层归一化 |

> 本节取代了初版设计中的"自研 HTTP 内核 + 15 条拒绝优先规则"。
> 那些规则中，**凡是 httplib 0.26.0 已正确处理的（已实测）不再自己实现**；
> **凡是它处理不当或语义不符合契约的（H-1/H-2/上限/归一化）由包装层承担并测试**。

### 7.2 路由与中间件

```cpp
namespace fss::http {
class Router {
 public:
  void Get (std::string_view pattern, Handler);
  void Post(std::string_view pattern, Handler);
  void Put  (std::string_view pattern, Handler);
  void Delete(std::string_view pattern, Handler);
  void Head (std::string_view pattern, Handler);
  // pattern 支持 ":" 命名段，如 "/v2/files/:id/metadata"
};

// 中间件链（顺序固定，以便可推理）
// 1. CorrelationIdMiddleware   —— 生成/透传 correlation-id，注入日志上下文
// 2. AccessLogMiddleware       —— 结构化访问日志
// 3. AuthMiddleware            —— 解析 Bearer / data-partition-id，失败即 401
// 4. BodyLimitMiddleware       —— 按路由配置的请求体上限
// 5. Handler
// 6. ErrorMappingMiddleware    —— 把 Result 的 ErrorKind 映射为 AppError + 状态码
}
```

### 7.3 REST 适配层的组织

```
src/adapters/http/
├── server.h/.cpp               # 组合 HttpServer + Router + 中间件
├── middleware/                 # 见 7.2
├── routes/
│   ├── metadata_routes.cpp     # /v2/files/metadata, /v2/files/{id}/metadata
│   ├── location_routes.cpp     # /v2/files/uploadURL, /v2/getLocation, /v2/getFileLocation,
│   │                           #   /v2/files/{id}/downloadURL, /v2/getFileList
│   ├── dms_routes.cpp          # /v2/files/{storageInstructions,retrievalInstructions,copy}
│   │                           #   /v2/file-collections/{storageInstructions,retrievalInstructions,copy}
│   ├── delivery_routes.cpp     # /v2/delivery/GetFileSignedUrl
│   ├── admin_routes.cpp        # /v2/files/revokeURL
│   ├── ops_routes.cpp          # /v2/info, /v2/liveness_check, /v2/readiness_check
│   └── transfer_routes.cpp     # /v1/transfer/{token}  ★ 扩展数据面
├── dto/
│   ├── file_metadata_dto.h/.cpp   # PascalCase 序列化（"Name"/"FileSource"/…）
│   ├── location_dto.h/.cpp        # LocationResponse{FileID, Location{SignedURL, FileSource}}
│   ├── file_list_dto.h/.cpp       # Spring Page：{Content, Number, NumberOfElements, Size}
│   ├── dms_dto.h/.cpp             # {providerKey, storageLocation} / {datasets:[...]} / [{success, datasetBlobStoragePath}]
│   ├── delivery_dto.h/.cpp        # {srns} → {processed, unprocessed}
│   ├── version_info_dto.h/.cpp
│   └── error_dto.h/.cpp           # AppError{code,reason,message} / legacy / ApiError
└── http_error_mapper.h/.cpp       # ErrorKind -> (HTTP status, reason, message)
```

**Base path（易错点）**：上游所有端点都挂在部署 context path **`/api/file`** 之下
（`server.servlet.contextPath=/api/file/`）。本服务的 `server.http.base_path`
默认同为 `/api/file`，路由注册时统一加前缀。
唯一例外是**扩展**的数据面 `/api/file/v1/transfer/{token}` 与指标 `/metrics`
（后者在 base path 之外，便于被监控系统直接抓取）。

**19 个 OSDU 端点的完整清单与字段级契约见 [`docs/03-api-contract.md`](03-api-contract.md) §2。**

**适配层的三条纪律**：
1. **不含业务判断**（无 `if kind == ...`、无有效期计算、无路径构造）。
2. **不含直接 IO**（除 `/v1/transfer` 的数据面转发，且它也只调用 `IBlobStore`）。
3. DTO ↔ 领域模型的转换**只在本层**发生，且转换函数是纯函数（可单测，无 IO）。

---

## 8. gRPC 适配层

### 8.1 契约

`proto/osdu/file/v1/file_service.proto`（已随仓库提供）。要点：

- 消息字段用 `json_name` 精确对齐 OSDU JSON 名称（`data` 内 PascalCase、信封 camelCase），
  使 gRPC 客户端启用 proto3-JSON 映射时能直接生产/消费规范 JSON。
- 保留 `StorageDriver` / `StorageZone` / `StorageErrorKind` 枚举，用于把领域分类无损传递。
- 扩展了 3 个流式/代理方法：`UploadFile`（客户端流）、`DownloadFile`（服务端流）、`ServerSideCopy`。
- **不使用 proto3 `optional`**（本机 protobuf 3.12.4 不支持，见 `cmake/Dependencies.cmake` 约束）。

### 8.2 适配方式

```cpp
// src/adapters/grpc/file_service_adapter.cpp （示意）
class FileServiceAdapter final : public osdu::file::v1::FileService::Service {
  grpc::Status GetUploadLocation(grpc::ServerContext* ctx,
                                 const GetUploadLocationRequest* req,
                                 LocationResponse* resp) override {
    // 1) 提取元数据（data-partition-id / authorization / correlation-id）
    FSS_TRY_GRPC(auto caller, ExtractCaller(ctx));
    // 2) proto -> app DTO
    auto dto = dto::FromProto(*req);
    // 3) 调用应用层（与 REST 路径完全相同）
    FSS_TRY_GRPC(auto out, usecases_->GetUploadLocation(caller, dto));
    // 4) app DTO -> proto
    dto::ToProto(out, resp);
    return grpc::Status::OK;
  }
  // UploadFile / DownloadFile：把 gRPC 流适配成 ByteSource / ByteSink，
  // 直接驱动 IBlobStore，与 HTTP 数据面共用同一实现。
};
```

### 8.3 错误映射（双向一致）

**唯一权威表在契约 §5，数据在 `src/domain/contract/error_table.cpp`（L3，无协议类型）**；
HTTP 与 gRPC 两个适配器都从**同一张表**派生（`HttpStatusFor` / `GrpcStatusFor`），
`test_error_equivalence`（C7.2）用手抄的契约期望值同时钉住两边。

| 领域 `ErrorKind` | REST | gRPC |
| --- | --- | --- |
| `kOk`（不是错误；误当错误用 → 实现 bug） | `500` | `INTERNAL` |
| `kInvalidArgument` | `400` | `INVALID_ARGUMENT` |
| `kFileSourceEmpty` | `400` | `INVALID_ARGUMENT` |
| `kInvalidSourcePath` | `400` | `INVALID_ARGUMENT` |
| `kLocationAlreadyExists` | `400`（对齐上游 `LocationAlreadyExistsException` → 400） | `ALREADY_EXISTS` |
| `kChecksumMismatch` | `400` | `INVALID_ARGUMENT`（尾随元数据 `fss-error-details` 携带期望/实际） |
| `kUnauthenticated` | `401` | `UNAUTHENTICATED` |
| `kPermissionDenied` | `403` | `PERMISSION_DENIED` |
| `kStorageAccessDenied` | `403` | `PERMISSION_DENIED`（存储侧拒绝本服务，对客户端是依赖故障） |
| `kNotFound` | `404` | `NOT_FOUND` |
| `kUnimplemented` | `501` | `UNIMPLEMENTED` |
| `kInternal` | `500` | `INTERNAL` |
| `kBadGateway` | `502` | `UNAVAILABLE` |
| `kUnavailable` | `503` | `UNAVAILABLE` |

> 上表**不允许**在本节独立演化：新增 `ErrorKind` 必须改 `error_table.cpp` + 契约 §5 表，
> 否则 `test_error_equivalence`（"无空缺"判据）直接失败。
> 本节的旧版本曾列出 `kAlreadyExists` / `kFailedPrecondition` / `kResourceExhausted`
> 三个**实现中并不存在**的枚举值，已按真实枚举更正（P7 切片 2）。
>
> 注意 `kLocationAlreadyExists → 400` 而非 409：这是**实测的上游行为**（`LocationAlreadyExistsException` 在
> `RestExceptionHandler` 中被归入 `handleBadRequest` → `HttpStatus.BAD_REQUEST`）。契约测试会固化这一点。

---

## 9. 位置与元数据持久化

### 9.1 位置记录（`IFileLocationRepository`）

借鉴 OSDU baremetal 的 KV 模型（`id` 主键 + `data jsonb`，见调研报告 §7），但抽出可索引列以支持 `getFileList` 的查询：

```sql
CREATE TABLE IF NOT EXISTS file_locations (
  partition_id   TEXT    NOT NULL,              -- 租户 ★ 必须进主键
  file_id        TEXT    NOT NULL,              -- fileID
  file_source    TEXT    NOT NULL,              -- 对客户端可见的相对路径
  container      TEXT    NOT NULL,              -- 物理容器（目录/bucket）
  object_key     TEXT    NOT NULL,              -- 物理键
  zone           TEXT    NOT NULL,              -- 'STAGING' | 'PERSISTENT'
  driver         TEXT    NOT NULL,              -- 'POSIX' | 'S3'
  created_by     TEXT    NOT NULL,              -- 上传者（= FileLocation.user_id，响应的 CreatedBy）
  created_at     INTEGER NOT NULL,              -- epoch 秒
  updated_at     INTEGER NOT NULL,              -- 最近一次更新（signed URL / zone）
  signed_url     TEXT    NOT NULL DEFAULT '',   -- 最近一次签发的 URL（非权威）
  data           TEXT    NOT NULL,              -- 完整记录 JSON（前向兼容，未知字段不丢）
  PRIMARY KEY (partition_id, file_id)
);
CREATE INDEX IF NOT EXISTS idx_fl_partition_created
  ON file_locations(partition_id, created_at);
CREATE INDEX IF NOT EXISTS idx_fl_zone ON file_locations(partition_id, zone);
CREATE UNIQUE INDEX IF NOT EXISTS idx_fl_source ON file_locations(partition_id, file_source);
```

**重要：`partition_id` 必须进主键/索引。** 上游 `file_locations_osm` 只有 `id` 唯一约束，
在多租户下依赖 `fileID` 全局唯一。本项目显式加 `partition_id` 维度，避免跨租户 `fileID` 碰撞
（UUID 碰撞概率极低，但**契约上不应依赖概率**）。这是相对参考实现的一个安全加固。

> ⚠️ **修正（P3 切片 2 实现时发现）**：本表初版把主键写成 `file_id TEXT PRIMARY KEY`
> 并同时声称"显式加 `partition_id` 维度" —— 二者**矛盾**：单列主键会让两个租户无法拥有同名
> `fileID`，与"每个仓储方法都带 partition"的端口契约直接冲突（`tests/framework/port_contract.h`
> 的"partition 严格隔离"小节会在 SQLite 上失败）。现修正为 `PRIMARY KEY (partition_id, file_id)`，
> 并补齐端口需要的 `updated_at` / `signed_url` 两列。实现见 `src/infra/location/sqlite/`。

**端口方法（P2 切片 5b 定稿）**：`getFileList`（契约 §2.5）要求"按 partition + 时间区间 +
`UserID` 过滤 + 分页"查询位置记录，因此 `IFileLocationRepository` 增加

```cpp
struct LocationQuery { std::string user_id; std::int64_t created_after_epoch_seconds = -1;
                       std::int64_t created_before_epoch_seconds = -1; int limit = 10; int offset = 0; };
struct LocationPage  { std::vector<FileLocation> records; std::int64_t total = 0; };
Result<LocationPage> List(std::string_view partition, const LocationQuery& query);
```

语义（由 `tests/framework/port_contract.h` 钉住）：时间边界**含端点**、`-1` 表示无界；
`user_id` 为空串 = 不过滤；`total` 是**过滤后、分页前**的总数；排序为 `created_at` 升序、
同秒按 `file_id` 升序（否则 offset 分页会漏项/重项）。领域模型 `FileLocation` 相应增加
`user_id` 字段（映射到上面 schema 已有的 `created_by` 列与响应的 `CreatedBy`）。
"无匹配记录时返回 400"（上游 `File_GetList_NoRecordPayload.json` 的行为）是**用例层**的契约，
仓储只返回空页。

### 9.2 元数据记录（`IMetadataRepository`）与版本链

★ **重要事实（实测）**：上游 OSDU File Service **自己不存元数据** ——
`dataset--File.Generic` 记录由 core-common 的 `DataLakeStorageService` 转发给
**Storage Service**（`PUT {storage.api}/records`）；File Service 自己的数据库只存 `FileLocation` 行。
完整论证与取舍见 **[ADR-004](adr/ADR-004-persistence-strategy.md)**。

因此本项目提供两种实现：

| 实现 | 语义 | 适用 | 是否上游能力 |
| --- | --- | --- | --- |
| `SqliteMetadataRepository` | 自管版本链：`(partition_id, id, version)` 联合主键，`is_latest` 标记，`previous_version` 指针 | **默认**，独立部署 | ❌ **本项目新增** |
| `RemoteStorageServiceRepository` | 转发到 OSDU Storage Service 的 `/api/storage/v2/records`（`PUT`/`GET`/`POST /records/{id}:delete` 需 204） | 部署在完整 OSDU 平台中 | ✅ 与上游一致 |

配置项 `metadata.repository = sqlite | remote`，默认 `sqlite`。

> ⚠️ **必须在交付文档中明确的限制**：
> 使用内置 `sqlite` 时，记录**不会**进入 Storage Service，因此**不会被 OSDU Search 检索到**。
> 若需要该能力，必须改为 `remote`。该限制会在 `/v2/info` 的 `connectedOuterServices` 中体现。

```sql
CREATE TABLE IF NOT EXISTS file_metadata_records (
  partition_id   TEXT NOT NULL,
  id             TEXT NOT NULL,
  version        INTEGER NOT NULL,
  kind           TEXT NOT NULL,
  is_latest      INTEGER NOT NULL DEFAULT 1,   -- 0/1
  created_at     INTEGER NOT NULL,
  created_by     TEXT NOT NULL,
  acl_viewers    TEXT NOT NULL,                -- JSON 数组，便于后续鉴权过滤
  acl_owners     TEXT NOT NULL,
  legal_tags     TEXT NOT NULL,                -- JSON 数组
  file_source    TEXT,                         -- data.DatasetProperties.FileSourceInfo.FileSource
  data           TEXT NOT NULL,                -- 完整记录 JSON（信封 + data）
  PRIMARY KEY (partition_id, id, version)
);
CREATE INDEX IF NOT EXISTS idx_fmr_latest ON file_metadata_records(partition_id, id, is_latest);
CREATE INDEX IF NOT EXISTS idx_fmr_source ON file_metadata_records(partition_id, file_source);
```

**事务边界（关键设计）**：`CreateFileMetadata` 用例跨越"复制文件"与"写记录"两个副作用，
不能依赖单一 DB 事务。采用**补偿式两阶段**。该序列**逐条对齐上游
`FileMetadataService#saveMetadata` 的实测行为**（见调研报告 §2.1）：

```
1.  publish status(DATASET_SYNC, IN_PROGRESS)                  [非致命]
2.  validateKind: 4 段, [1]=="wks", [2]=="dataset--File.Generic"
3.  filePath = data.DatasetProperties.FileSourceInfo.FileSource [空 → 400 "FileSource can not be empty"]
4.  id = "<partition>:dataset--File.Generic:<uuid-no-dashes>"
5.  stagingLocation / persistentLocation 解析
6.  blob.copy(staging → persistent)                             [失败 → 502/500，不写记录]
7.  checksum = 流式计算(persistent 写入过程); 覆写 FileSourceInfo.Checksum + ChecksumAlgorithm
8.  record = 组装(id, acl, legal, kind, ancestry, data, meta, tags)
9.  metadataRepo.Upsert(record, version=1)                      [失败 → 回滚删除 persistent 对象]
10. publish status(SUCCESS) + datasetDetails                    [非致命]
11. remove(staging 对象)                                        [失败 → 忽略 + 审计告警，响应仍为 201]
12. 第 6/7/9 步失败 → remove(persistent) 回滚 + publish status(FAILED) → 重抛
```

每一步的失败语义、是否可重试、事件上报方式都在契约文档中固定，
并有对应的**故障注入测试**（阶段 6 门槛 C6.3 要求 6 个故障点各有一个测试）。

### 9.2.1 多实例部署（★ 单实例设计的假设在此失效）

> 完整实测与方案见 **[ADR-009](adr/ADR-009-multi-instance-consistency.md)**；
> 原始输出见 `docs/appendix/multi-instance-probe/RESULTS.txt`。

**单实例设计的默认值在多实例下会直接导致数据损坏或丢失**，实测复现了 5 个问题：

| # | 问题 | 实测结果 | 多实例下的修复 |
| --- | --- | --- | --- |
| M1 | tmp 名不含实例标识（`.tmp_<idx>`） | **21/40 次静默内容错乱**（B 的最终文件里是 A 的内容） | tmp 名加 `instance_id`+`pid`+计数 |
| M2 | 无幂等键唯一约束（check-then-insert） | **20/20 重复记录 + 2 次复制** | 幂等键唯一索引 + `ON CONFLICT` **原子领取** |
| M3 | GC 删除"无元数据记录"的 staging 对象 | **20/20 误删在途上传** | **租约（到期才回收）+ `DELETE...RETURNING` 原子领取** |
| M4 | SQLite 作为共享状态 | 各实例独立 DB → 状态发散；共用文件在 NFS 上不安全 | 改用 **PostgreSQL** |
| M5 | `single_use_nonce` 用本地表 | 跨实例无法拒绝重放 | nonce 入共享存储，或**默认关闭** |

**多实例架构（状态全部外置）**

```
客户端 → 任意实例（无状态：不保存会话/不缓存幂等判定）
            ├── PostgreSQL：位置记录 / 元数据记录 / 在途租约 / advisory lock（领导者选举）
            └── 共享 POSIX 存储（NFSv4/SAN/CephFS）：staging / persistent / tmp（名含实例标识）
```

| 状态 | 单实例 | 多实例 |
| --- | --- | --- |
| 位置记录、元数据记录 | SQLite | **PostgreSQL**（事务 + 唯一约束） |
| 在途状态 | 无 | **租约表**（时间基准用 PG 的 `now()`，消除时钟偏移） |
| 单例任务（GC 等） | 直接跑 | **PG advisory lock 领导者选举** + 幂等（纵深防御） |
| 文件数据 | 本地盘 | **共享挂载**或对象存储 |

**明确的禁令**：**禁止**"数据库不可用就降级到本地 SQLite"——那是状态发散的起点；
PG 不可用时一律 `503 + Retry-After`（fail-closed）。

**未验证的硬前提**（上生产前必须在目标存储上验证）：NFS 的 `rename` 原子性、
close-to-open 一致性、`fsync`/`syncfs` 耐久性语义；以及**不要依赖 NFS 文件锁**
（本项目改用 PG advisory lock）。

### 9.3 回收（GC）

| 对象 | 条件 | 默认 |
| --- | --- | --- |
| staging 区中未登记元数据的对象 | `created_at` 早于 N 小时且无对应 metadata 记录 | 24 小时（对齐 OSDU 文档口径） |
| persistent 区中无元数据记录的对象（孤儿） | 无对应记录，且超过宽限期 | 宽限 72 小时 |
| 过期 transfer token | 内存/表中过期记录 | 按过期时间清理 |
| **写入残留 `.tmp_*`** | 未完成批提交的临时文件（ADR-008） | 启动时 + 定期清理；**绝不能视为有效对象** |
| **在途租约过期的 staging 对象** | 租约到期 且 无元数据记录（ADR-009） | 多实例下必须走此路径；**禁止**"无记录即删" |

GC 是**独立后台任务**（可通过配置关闭），默认**只记录不删除**（`gc.dry_run=true`）以避免误删；
生产环境显式开启。GC 本身有独立测试（构造孤儿对象 → 验证 dry-run 不删、开启后删除）。

**实现进度（P6 完成时的现状，逐行对应上表）**

| 行 | 状态 |
| --- | --- |
| staging 超期（无记录） | ✅ `GcTask`：仅当 `gc.require_lease_expiry=false`（**单实例**）时按 TTL 扫描 |
| persistent 孤儿 | ✅ `GcTask`：无任何位置记录引用 + 超过 `gc.orphan_grace_hours` |
| 过期 transfer token | ⬜ 未做：自签 token 是**无状态**的（无表可清，靠签名校验拒绝过期）→ P9 复核 |
| `.tmp_*` 残留 | ✅ `GcTask` 第 ④ 步（P9/C9.25）：走 `IBlobStore::remove_temp_files` 专用入口（`list()` 按契约**跳过**内部键，所以不能用它）；判据是 mtime 早于 `gc.staging_ttl_hours` —— 在途上传因此被保护，且**保护数可见**（`TempSweepResult::skipped_too_young` → `fss_gc_skipped_total{reason="tmp_too_young"}`）。反向测试：即使某条位置记录**恰好指向** `.tmp_*` 键也照删（那种键不可能由 `ObjectKeyPolicy` 生成） |
| 租约过期的 staging 对象 | ✅ `GcTask`：`ClaimExpired` **原子领取** + 领取后**再看一次记录**（有记录永不删） |

实现位置：`src/app/tasks/gc_task.{h,cpp}`；测试：`tests/integration/test_gc_lease.cpp`（C6.12）。
GC 指标（P9/C9.6）：`/metrics` 暴露 `fss_gc_runs_total{mode,outcome}`、
`fss_gc_objects_deleted_total`、`fss_gc_tmp_removed_total`、
`fss_gc_skipped_total{reason}`（`has_record` / `too_young` / `no_location` / `tmp_too_young`）
与 `fss_gc_last_run_epoch_seconds`。

**未做**（如实登记，属 P9）：调度（`gc.interval_seconds`）、领导者选举、
PG 版 `ILeaseRepository`（`InMemoryLeaseRepository` 目前只是测试替身）、
多实例下 GC 的 TTL 判定应改用数据库时钟（ADR-009 §6.4）。

---

## 10. 认证、授权与多租户

```
请求 ──▶ AuthMiddleware
          │ 1) 取 data-partition-id  ── 缺失 → 401
          │ 2) 取 Authorization: Bearer <JWT> ── 缺失/格式错 → 401
          │ 3) 解析 JWT：签名校验(可选)、exp、aud/iss(可配)
          │ 4) 取 user id（x-user-id 或 JWT 的 sub/email）
          ▼
        IAuthorizer::Authorize(CallerContext, RequiredRole)
          │ 本地实现: 静态角色表（配置文件）或按 JWT 的 roles claim
          │ 远端实现: 调 Entitlements Service /api/entitlements/v2/groups/{group}/members
          ▼
        通过 → 用例；失败 → 403
```

- **`CallerContext`**（领域类型，L3）：`{partition_id, user_id, bearer_token, correlation_id, roles}`。
  所有应用层用例的**第一个参数**都是 `const CallerContext&`，使鉴权与租户隔离无法被"忘记"。
- **角色常量**逐字节对齐上游实测值（见 `docs/03-api-contract.md` §1.3）：

| 端点组 | 角色 |
| --- | --- |
| 上传位置（`uploadURL` / `getLocation`）、`getFileLocation`、`getFileList` | `service.file.editors` |
| 创建 / 删除元数据 | `service.file.editors`（删除另允许 `service.file.admin`） |
| 读取元数据（`GET .../metadata`） | **`service.file.viewers`**（★ 官方文档误写为 editors，以代码为准） |
| 下载 URL（`files/{id}/downloadURL`） | `service.file.viewers` |
| `revokeURL` | `service.file.admin`（**且不要求 `data-partition-id`**） |
| DMS `storageInstructions` | **`service.dataset.editors`** |
| DMS `retrievalInstructions` | **`service.dataset.viewers`** |
| DMS `copy` | `service.storage.creator` / `service.storage.admin` |
| Delivery `delivery/GetFileSignedUrl` | **`service.delivery.viewer`** |
| `info`、`liveness_check`、`readiness_check` | 免鉴权 |

  鉴权判定语义 = `authorizeAny`（拥有**任一**所列角色即通过）。
- **多租户隔离**：所有仓储方法的第一个参数都是 `partition_id`，无一例外；
  护栏测试检查"仓储实现中的 SQL 是否都带 `partition_id` 条件"（文本检索 `FROM <table>` 附近必须有 `partition_id`）。
  自签传输 token 同样绑定 `partition`，防止跨租户使用签名 URL。
- **开发/测试模式**：`auth.mode=disabled` 时使用 `AllowAllAuthorizer`，
  但**必须**要求显式配置，且启动时打印显著告警；在 `production` 部署模板中该值被强制为 `jwt`。

---

## 11. 配置设计

优先级：**命令行 > 环境变量 > 配置文件 > 内置默认值**。

```yaml
# config/fss.example.json（示例）
server:
  http:  { base_path: "/api/file",               # ★ 对齐上游 context path
           bind: "0.0.0.0", port: 8080, max_header_bytes: 16384, max_uri_bytes: 8192,
           max_body_bytes: 10485760,            # 10 MiB（数据面端点单独放宽）
           idle_timeout_seconds: 60, worker_threads: 8 }
  grpc:  { enabled: true, bind: "0.0.0.0", port: 50051, max_message_bytes: 4194304 }

storage:
  # 默认驱动；可被 partition 级覆盖
  driver: posix                  # posix | s3
  proxy_mode: auto               # auto | always   (always = 强制服务代理所有字节)
  # 兼容开关：上游把所有云的 Driver 硬编码为 "GCS"（见调研 §4.0 F8）
  driver_report_override: ""     # "" = 上报真实驱动；"GCS" = 复刻上游行为
  provider_key_override: ""      # "" = 使用 "POSIX"/"S3"；可覆盖 DMS 的 providerKey
  posix:
    root: "/var/lib/fss/data"    # 集中存储根目录
    fsync_on_write: true
    atomic_write: true
  s3:
    endpoint: "http://127.0.0.1:9000"
    region: "us-east-1"
    access_key: ""               # 支持 ${ENV:VAR} 引用
    secret_key: ""
    force_path_style: true       # MinIO/Ceph/SeaweedFS 需要
    verify_tls: true
    presign_max_seconds: 604800  # 7 天（对齐 OSDU 上限）

self_signed:                     # 集中存储数据面
  enabled: true
  public_base_url: "http://127.0.0.1:8080"   # 对外可达地址（生成自签 URL 用）
  signing_key: "${ENV:FSS_TRANSFER_SIGNING_KEY}"
  key_id: "k1"
  default_ttl_seconds: 3600
  max_ttl_seconds: 604800

expiry:
  default: "1H"                  # 对齐上游代码：缺省 1 小时
  max: "7D"                      # 对齐上游代码：超限静默截断到 7 天

http:
  error_format: apperror         # apperror（默认，OSDU 标准）| legacy | api_error

metadata:
  repository: sqlite             # sqlite（默认，独立部署）| remote（对接 OSDU Storage Service）
  sqlite: { path: "/var/lib/fss/meta.db", busy_timeout_ms: 5000, journal_mode: WAL }
  remote: { base_url: "", token_provider: "static", timeout_ms: 5000 }

location:
  repository: sqlite
  sqlite: { path: "/var/lib/fss/location.db" }

auth:
  mode: jwt                      # jwt | disabled | remote-entitlements
  jwt:  { jwks_url: "", issuer: "", audience: "", verify_signature: true,
          roles_claim: "roles", user_id_claim: "email" }
  local_roles:                   # auth.mode=jwt 时的静态角色表
    "user@example.com": ["service.file.editors", "service.file.viewers"]

partition:
  registry: file                 # file | remote
  file:
    opendes:
      staging_container: "opendes-staging"
      persistent_container: "opendes-persistent"
      storage_driver: posix      # 分区级覆盖
      max_file_bytes: 0          # 0 = 不限
      allowed_checksum_algorithms: ["SHA-256", "MD5", "SHA-1"]

gc:
  enabled: false
  dry_run: true
  staging_ttl_hours: 24
  orphan_grace_hours: 72
  interval_seconds: 3600

observability:
  log_level: info                # debug | info | warn | error
  log_format: json               # json | text
  audit_enabled: true
  metrics_enabled: true
  metrics_path: "/metrics"       # ★ 非 OSDU 端点，与 /api/file 隔离
```

**配置校验**：启动时对全部配置做一次性校验（类型、范围、互斥项、必填项），
任何不合法项 → **启动失败并打印全部问题**（不进入"半可用"状态）。这是可测试的（配置校验单测）。

---

## 12. 目录结构与 CMake 目标图

```
fssvrcpp/
├── CMakeLists.txt
├── cmake/
│   ├── Dependencies.cmake        # 依赖探测（无 root 环境的解包式获取）
│   ├── ProtoGen.cmake            # protoc/grpc_cpp_plugin 封装
│   └── CompilerWarnings.cmake
├── proto/osdu/file/v1/file_service.proto     ★ RPC 契约
├── config/fss.example.json
├── docs/
│   ├── 01-osdu-research.md  ★
│   ├── 02-design.md         ★（本文）
│   ├── 03-api-contract.md   ★
│   ├── 04-implementation-plan.md ★
│   ├── adr/ADR-001..004.md  ★
│   └── test-evidence/       ★ 每阶段测试证据
├── src/
│   ├── common/            → fss_common（INTERFACE）+ fss_http/fss_crypto/... （STATIC）
│   │   ├── result/ json/ http/ crypto/ ids/ time/ fs/ config/ logging/ net/ bytes/
│   ├── domain/            → fss_domain       （仅链接 fss_common）
│   │   ├── model/ ports/ error/
│   ├── app/               → fss_app          （链接 fss_domain）
│   │   ├── usecases/ services/ dto/
│   ├── infra/             → fss_infra_posix / fss_infra_s3 / fss_infra_sqlite /
│   │   │                     fss_infra_auth / fss_infra_remote / fss_infra_event
│   │   ├── blob/{posix,s3,memory}/ location/sqlite/ metadata/{sqlite,remote}/
│   │   ├── auth/{local,remote}/ legal/ partition/ event/ transfer/
│   ├── adapters/          → fss_http_adapter / fss_grpc_adapter
│   │   ├── http/{routes,middleware,dto}/
│   │   └── grpc/
│   └── main/              → fss_server（可执行文件，唯一组合根）
├── tests/
│   ├── framework/         # 测试基建：临时目录、进程内服务器、mock-S3、ManualClock…
│   ├── unit/              # 毫秒级，无 IO
│   ├── integration/       # 真实端口/文件系统/mock 服务
│   ├── conformance/       # OSDU 契约符合性（黄金样例）
│   └── tools/mock_s3.py   # Python stdlib 实现的 mock S3（无第三方依赖）
└── third_party/
    ├── nlohmann/json.hpp
    └── catch2/catch.hpp
```

**CMake 目标 → 层映射**

| 层 | CMake 目标 | 类型 | 链接（只允许这些） |
| --- | --- | --- | --- |
| L1 | `fss_common` | INTERFACE | `fss_warnings` |
| L1 | `fss_http` `fss_crypto` `fss_ids` `fss_time` `fss_fs` `fss_config` `fss_logging` `fss_net` `fss_bytes` `fss_sys` `fss_metrics` | STATIC | `fss_common` `OpenSSL::Crypto` `Threads` |
| L3 | `fss_domain` | STATIC | `fss_common` |
| L4 | `fss_app` | STATIC | `fss_domain` |
| L5 | `fss_proto` | STATIC | `fss_common` `gRPC++` `protobuf` |
| L2 | `fss_infra_posix` `fss_infra_s3` `fss_infra_sqlite` `fss_infra_local` `fss_blob_metered` | STATIC | `fss_domain` `fss_common` + 对应外部库 |
| L5 | `fss_http_adapter` | STATIC | `fss_app` `fss_http` |
| L5 | `fss_grpc_adapter` | STATIC | `fss_app` `fss_proto` |
| — | `fss_server` | EXECUTABLE | 以上全部 |

---

## 13. 并发模型与资源上限

> ★ **本节已按实测数据重写。** 初版把 `worker_threads` 设为 8 并称"阻塞模型简单"，
> 但对"大量频繁小文件 + TB 级大文件分段读"的负载**不达标**。
> 完整实测数据、缺陷清单与修正方案见 **[`docs/05-capacity-and-concurrency.md`](05-capacity-and-concurrency.md)**。

### 13.1 并发模型（含实测依据）

| 项 | 设计 | 实测依据 |
| --- | --- | --- |
| HTTP 传输 | cpp-httplib 线程池 + 阻塞 IO，**一条 keep-alive 连接占用一个池线程直到连接结束** | 因此**线程池大小 = 并发连接上限**；默认 `max(8, nproc-1)` = 15 是硬上限 |
| **`TCP_NODELAY`** | **强制开启**（服务端与客户端） | 关闭时 keep-alive 小请求 **23 req/s**，开启后 **21,062 req/s**（**950x**，40 ms delayed-ACK） |
| 线程池分离 | **控制面池**（JSON 短任务）+ **数据面池**（字节流长任务），互不饿死 | 单池时一批大文件传输会占满线程、饿死元数据请求 |
| 准入控制 | 超过并发上限 → **`503` + `Retry-After`**，不排队到超时 | 线程池满时排队只会把延迟推到超时，不如明确背压 |
| gRPC 并发 | gRPC 自带线程池；服务实现内部不共享可变状态（除仓储连接池） | — |
| 小文件写并发 | **有界写并发（默认 8）+ 写队列**，禁止所有请求线程直抢 SQLite 写锁 | 8 线程 25,471 tx/s 为峰值；**32 线程降到 13,475 tx/s**（写争用恶化） |
| 数据面缓冲 | 从**缓冲池**取（非每次分配）；`buffer_bytes × 并发上限 ≤ transfer_memory_budget`，超限**拒绝启动** | 避免并发 × 缓冲的内存放大 |
| 时间 | 全部通过 `IClock`，保证超时/过期逻辑可被 `ManualClock` 精确测试 | — |

### 13.2 大文件分段读（TB 级，只读）

| 要求 | 说明 |
| --- | --- |
| 64 位偏移 | 内部一律 `uint64_t`；编译期强制 `_FILE_OFFSET_BITS=64`；已实测在 200 GiB 文件的 150 GiB 偏移读取正确 |
| 定位读 | 用 **`pread`**（不改文件偏移、天然线程安全），**不用** `lseek+read` |
| 无读放大 | 已实测：读 1 MiB 段仅耗时 1.5 ms，不触发全文件扫描 |
| **禁止在读路径算校验和** | 否则范围读退化为 O(文件大小)。**硬性禁令**，有专门回归测试 |
| Range 处理 | 声明**完整** `content_length`，由 httplib 计算 `(offset,length)`；`fss_http` 只做越界/416 归一化 |
| I/O 引擎 | **默认 `blocking`**（`pread`/`pwrite` + 有界线程池，到处能跑）；`IIoEngine` 抽象保留 `uring` 可选实现（启动探测 + 回退）。**默认容器 seccomp 阻断 io_uring（实测 EPERM）** → [ADR-010](adr/ADR-010-io-engine-choice.md) |
| 零拷贝 | **ADR-006（已采纳方向，P9 定稿）**：大文件数据面走自持 socket + `sendfile`（循环处理 >2 GiB 上限）；**实现未交付**。受控 A/B（同一负载生成器）实测 `sendfile` 比 httplib 内容提供者快 **2.12x**（几何平均；~~旧值 ~5–7x 来自不同探针的拼装比较，方法学不成立，已作废~~）。`sendfile` 比 `pread+write` 快 **2.05x**、每 GiB CPU 少 **42%**（探针） |
| io_uring | ~~ADR-006 范围已扩展：数据面文件侧用 io_uring~~ ⚠️ **已由 [ADR-010](adr/ADR-010-io-engine-choice.md) 取代**：`io_engine` 默认 `blocking`，`uring` 为**可选加速**（默认容器 seccomp **阻断**，实测 `EPERM`）。探针数字仍有效（1 线程 132,934 IOPS vs 128 线程 118,148；写路径 1 线程 20,497 文件/s），但它证明的是"省线程"，不是"提高上限"；**ADR-006 定稿后不要求 io_uring** |
| 页缓存 | 随机读 `posix_fadvise(RANDOM)`；大段顺序读后可 `DONTNEED`（可配） |
| 超时 | 数据面**无整体超时**，只有"空闲无进展"超时，否则 TB 级传输会被打断 |

### 13.3 小文件读写（大量频繁）

| 要求 | 说明 |
| --- | --- |
| 推荐拓扑 | **对象存储模式**：字节走客户端直连预签名 URL，**服务不在字节路径上**，元数据可水平扩展 |
| 集中存储模式 | 字节经过服务 → 每并发传输占一个线程；小段读实测请求率上限约 **40,000 req/s**（4 KiB 段 35,206 req/s） |
| DB 固定成本 | 每小文件 2 次事务。WAL + `synchronous=NORMAL` 实测 **25,471 tx/s**；`synchronous=FULL` 仅 **1,219 tx/s**（差 21x） |
| `fsync` 策略 | 由全局开关改为分级：`fsync_policy: always \| by_size \| never` + `fsync_threshold_bytes`（默认 1 MiB） |
| 审计 / 事件 | 移出请求关键路径 → **异步有界队列**（满则丢弃并计数；审计可配 fail-closed） |
| 原子写 | 保留 tmp+rename；`fsync` 按上面的分级策略，避免逐文件 fsync |

### 13.4 硬上限（均可在配置中调整，均有"被拒绝"的测试）

| 项 | 默认 | 超限行为 |
| --- | --- | --- |
| 请求头 | 16 KiB | 400 |
| 请求 URI | 8 KiB | 400 |
| JSON 请求体 | 10 MiB | **413**（`Content-Length` 已知时前置拦截） |
| 数据面请求体 | 按租户配额 | 413 / 400（见契约 §1.7） |
| 并发连接 | 按公式计算（见下） | **503 + `Retry-After`** |
| 每租户并发 | 可配 | 503 |
| 传输内存预算 | 256 MiB | **拒绝启动**（配置校验） |

> **★ P9 实测（`docs/test-evidence/phase9.md` §9 / `docs/05-capacity-and-concurrency.md` §1.9）**：
> 上表"传输内存预算 → 拒绝启动"已有正/反两条测试（C9.3 ⑥ / C9.13）；
> `storage.posix.durability` 的三档在**产品进程**上的端到端差异为
> `per_file` **96.4** files/s vs `batch` **410.1** vs `never` **426.0**（c4，4 KiB 对象），
> 即 **4.3×** —— 收益来自"摊销 fsync"而不是"不做耐久性"（ADR-008）。
> ⚠️ 这些数字来自 WSL2 虚拟盘，只作**本机量级参考**（C9.14 未验证）。

**并发上限的取值公式**（取代初版硬编码的 8）：

```
控制面连接上限 ≈ 峰值并发 JSON 请求数
                 实测参考：纯内存 JSON 约 200,000 req/s（16 核）
数据面连接上限 ≈ 峰值并发传输数
                 受 buffer_bytes × 上限 ≤ transfer_memory_budget 约束
SQLite 写并发  = 8（实测峰值，超过反而下降）
```

---

## 14. 可观测性

| 维度 | 实现 |
| --- | --- |
| 日志 | 结构化 JSON 行；字段：`ts, level, logger, msg, correlation_id, partition_id, user_id, method, path, status, duration_ms, file_id, driver, zone, error_kind`；`log_format=text` 供本地开发。实现选型见 [ADR-011](adr/ADR-011-logging-library.md)；实测 ~1.1 µs/条（含脱敏），峰值负载下占单核 **2.3%** |
| 审计 | `IAuditLogger`，事件对齐上游 `AuditOperation`（`createLocationSuccess`、`readFileLocationSuccess` 等），包含操作者、对象、结果、时间 |
| 指标 | `/metrics`（非 OSDU 端点，独立路径）：请求计数/延迟直方图（按端点、状态码）、存储操作计数/延迟/字节数（按 driver、zone）、活跃连接数、GC 删除数、自签 URL 校验失败数 |
| 健康 | `/v2/liveness_check`（进程存活，纯文本 `File service is alive`）、`/v2/readiness_check`（依赖就绪：存储可达、DB 可写，纯文本 `File service is ready`）、`/v2/info`（版本信息，含 `connectedOuterServices`） |
| 追踪 | `correlation-id` 全链路透传（入站读取或生成；出站调用携带） |

---

## 15. 安全与威胁模型

| # | 威胁 | 缓解措施 | 验证方式 |
| --- | --- | --- | --- |
| T1 | 路径穿越（`..`、绝对路径）写入 POSIX 存储 | `ObjectKeyPolicy` 只生成规范化键；`PosixBlobStore` 逐段校验 + `openat`/`O_NOFOLLOW` + 解析后校验仍在 `root` 内 | 专项单元测试（≥10 个恶意键） |
| T2 | 符号链接逃逸 | 拒绝 `O_NOFOLLOW` 打开失败；容器目录创建时校验非符号链接 | 集成测试（预置符号链接） |
| T3 | 自签传输 URL 伪造/篡改 | HMAC-SHA256 + 常量时间比较；载荷含 `op`+`ref`+`exp`+`partition`+`nonce` | 单元测试：篡改任一字段必须失败 |
| T4 | 自签 URL 重放（跨操作/跨租户/过期） | 载荷绑定 `op` 与 `partition`；强制 `exp` 校验；`nonce` 支持一次性使用（可选，默认关闭以支持大文件续传） | 集成测试：GET token 用于 PUT、越租户、过期后使用 → 全部拒绝 |
| T5 | 密钥泄漏（S3 secret / 签名密钥） | 配置支持 `${ENV:VAR}` 引用；日志脱敏（`access_key`/`secret_key`/`token`/`sig` 一律打码）；`/v2/info` 不输出密钥 | 单元测试：日志与 info 输出中不出现密钥明文 |
| T6 | 越权访问他人记录 | 所有仓储方法强制 `partition_id`；路由级鉴权预检 + 用例入口 `IAuthorizer`；**租户绑定**：token 的 `data-partition-id` claim 必须等于请求头（ADR-012 §3） | `test_jwt_authorizer`（A 的 token + B 的头 → 403）+ `test_auth_matrix`（端点 × 角色矩阵）+ P3/P6 的分区隔离契约 |
| T7 | 认证绕过 | ADR-012：默认 `auth.mode=jwt`（HS256 本地校验）；`alg` 白名单（拒绝 `none`）；`exp` 必需；密钥/claim 缺失一律拒绝（fail-closed）；production 禁止 `disabled`/关验签/无密钥；`disabled` 打印显著告警且 `/v2/info` 暴露 `authMode`；`remote-entitlements`/JWKS 未实现时**拒绝启动** | `test_unit/test_jwt_authorizer`（9 类越权/畸形 token 全 401）+ `test_config`（production 三条拒绝 + 一条正例）+ `test_auth_matrix` |
| T8 | HTTP 解析歧义/走私 | 拒绝优先的解析规则（见 §7.1），`Content-Length` 与 `chunked` 互斥 | 单元测试（畸形样例 ≥15 例） |
| T9 | 资源耗尽（大文件/大量连接） | 硬上限 + 配额 + 磁盘水位检查（低于阈值时 readiness 失败） | 压力/边界测试 |
| T10 | 校验和绕过（数据损坏） | 若请求提供 `expected_checksum`，服务端在写入过程中增量计算并比对，不符则删除对象并返回 400 | 集成测试：故意给错校验和 |
| T11 | 审计缺失 | 审计日志与业务日志分离；审计写入失败要影响操作结果（可配置为 fail-closed） | 集成测试（注入审计失败） |

---

## 16. 风险登记

| ID | 风险 | 影响 | 概率 | 缓解 | 触发条件（何时升级） |
| --- | --- | --- | --- | --- | --- |
| R-01 | **第三方 HTTP 库（cpp-httplib）的缺陷或行为变化** | 中 | **已发生**（H-1/H-2） | 版本锁定 + 校验和 + `fss_http` 三重防护（前置拦截/计数 reader/长度一致性）+ H-1/H-2 回归测试；门面隔离使换库影响面限于一个目标 | 升级 httplib 后回归测试失败；或发现新的、包装层无法兜住的缺陷 → 转评估 Boost.Beast（ADR-002 候选 3） |
| R-02 | S3 SigV4 实现与真实存储不兼容 | 高 | 中 | mock-S3 **独立验签**（不用我们自己的签名器验自己）+ 一个真实 MinIO（若可用）端到端 | 阶段 5 中 mock 验签通过但真实 MinIO 失败 |
| R-03 | protobuf 3.12 限制（无 proto3 `optional`）导致契约表达力不足 | 低 | 已发生 | 已规避：用 message/wrapper 表达可选性；契约测试覆盖 | 需要真正的字段存在性语义时 |
| R-04 | 错误体格式（`AppError` vs 遗留 `ErrorResponse`）选择与上游部署不一致 | 中 | 中 | 提供 `http.error_format` 开关，默认 `apperror`；契约测试两个格式各测一遍 | 对接方反馈客户端不兼容 |
| R-05 | 无 root 权限导致无法安装依赖（如真实 MinIO/Postgres） | 中 | 高 | 全部依赖走"解包到 `third_party`"或"Python 标准库 mock"；集成测试不强依赖外部服务 | 阶段 5 需要真实 S3 才能验证某特性时 |
| R-06 | OSDU 上游接口变更（如新增/废弃端点）导致契约漂移 | 中 | 低 | 契约文档记录取样 commit；升级时对比 `docs/api/community/v2/openapi.yaml`；契约测试集中在一处便于更新 | 上游新增 breaking change |
| R-07 | 大文件流式路径的背压/超时处理不完善 | 中 | 中 | 阶段 4/6 的门槛测试包含大文件（≥1 GiB 虚拟）与慢客户端场景；固定缓冲 + 明确超时 | 压力测试出现内存增长或连接悬挂 |
| R-08 | 领域层被"便利地"污染（如直接 include json/http） | 中 | 中 | 编译期目标图 + 源码检索双护栏（§4） | 护栏测试被绕过或禁用 |
| **R-12** | **`TCP_NODELAY` 未开启导致小请求 40ms 停顿** | 高 | **已实测** | 强制 `set_tcp_nodelay(true)` + 阶段 1 门槛断言（含"关闭时必须复现 40ms"的自证测试） | 升级 httplib 后默认值变化；见 `docs/05-capacity-and-concurrency.md` §1.2 |
| **R-13** | **线程池过小 = 并发硬上限**（默认 15） | 高 | **已实测** | 显式配置 + 控制面/数据面池分离 + 503 背压 + 容量门槛 | 见 §1.3 |
| **R-14** | 集中存储模式下大文件传输占用线程且无法零拷贝 | 高 | **已实测** | [ADR-006](adr/ADR-006-large-file-data-plane.md)：受控复核 **2.12x ≥ 1.5x → 采纳 sendfile 方向**（实现未交付，落地必须与控制面校验同源、可关闭）；在此之前**显式接受** httplib 路径的带宽/CPU 上限 | 真实存储/网卡上复核 <1.5x；或"复用控制面校验"做不到；或 TLS 成为硬需求（ADR-006 §7） |
| **R-15** | 小文件逐文件 fsync 把上限压到 ~1.2k/s | 高 | **已实测** | `fsync_policy: by_size` + SQLite `synchronous=NORMAL` + 有界写并发 8 + 批提交 | 见 §1.8 |
| **R-16** | 容量测量方法本身不可信（进程内客户端压测） | 中 | **已发生** | 负载生成器必须独立进程 + 绑核；保留错误方法探针作对照；门槛 C9.11 | 见 `docs/appendix/capacity-probe/README.md` |
| **R-23** | **多实例下 tmp 名冲突导致静默内容错乱** | 高（数据悄悄被换成别人的） | ADR-009 M1；tmp 名含实例标识；门槛 C6.13 | 已实测复现 21/40 |
| **R-24** | 多实例重复创建（重复记录 + 重复复制） | 高 | ADR-009 M2；幂等键唯一约束 + 原子领取；门槛 C6.11 | 已实测复现 20/20 |
| **R-25** | 多实例 GC 误删在途上传 | 高（数据丢失） | ADR-009 M3；租约 + 原子领取；门槛 C6.12 | ✅ **已缓解并验证**：`GcTask` 只在"租约到期 + 原子领取 + 领取后仍无元数据记录"三条件同时满足时才删；`test_gc_lease` 6 用例（含"无租约不许删""有记录永不删""两个 GC 并发每对象恰好删一次"）。仍未做：多实例的数据库时钟（P6-D18）、领导者选举（依赖 PG；**周期调度已在阶段 10 切片 2 / C10.9 交付**） |
| **R-26** | 误用 SQLite 做多实例共享状态 | 高（状态发散） | ADR-009 M4；`deployment.mode=multi` 启动校验强制 PG；门槛 C8.9 | 已复现 |
| **R-27** | 共享 POSIX 存储的 NFS 语义未验证即上生产 | 高 | ADR-009 §6.2；门槛 C9.27 为**上生产硬前提** | 未验证 |
| **R-28** | 多实例 `syncfs` 互相干扰（ADR-008 P4 的放大效应） | 中 | 建议按 partition 分盘；门槛 C9.24 + ADR-009 §6.3 | — |
| **R-29** | 误把 io_uring 当必需依赖 → 在默认容器 seccomp 下启动即失败 | 高 | ADR-010：默认 `blocking` 引擎；`io_engine=uring` 探测失败则拒绝启动、`auto` 则回退；门槛 C1.15 | 已实测 EPERM |

---

### 16.1 风险收口（P9 / C9.10）

> 判据原文："全部 11 项风险（§16）都有明确的缓解措施落地证据或显式接受记录"。
> ⚠️ **登记纠正**：§16 实际有 **20 行**（R-01~R-08、R-12~R-16、R-23~R-29），计划里的"11 项"是
> 初版数字、已过时；本表覆盖**全部 20 行**（R13：数字随实现同步）。
> 状态口径：✅ 已落地（有可执行证据）· 🟡 部分（已落地部分 + 明确未做）· ⛔ 显式接受/未验证（附理由与再评估条件）。

| ID | 状态 | 落地证据（可执行） | 未做 / 接受理由 |
| --- | --- | --- | --- |
| R-01 httplib 缺陷 | ✅ | 版本锁定 + vendored 源码；`fss_http` 三重防护（前置拦截/计数 reader/长度一致性）；`test_httplib_hardening` + `scripts/verify_http_hardening.sh`（含"拆掉防护必须失败"的自证） | — |
| R-02 S3 SigV4 兼容 | 🟡 | AWS 官方向量 5 条逐字节 + `mock_s3.py` **独立验签** + 同一套端口契约跑第三遍 + `verify_driver_switch.sh` | **真实 MinIO/S3 未联调**（本环境无凭据/服务）；分片上传 >5 GiB、退避重试、STS 未测 |
| R-03 protobuf 3.12 限制 | ✅ | 用 message 包裹表达可选性；`dataset_properties.present` 段级存在性 + P7 契约/等价性矩阵测试 | — |
| R-04 错误体格式选择 | ✅ | `http.error_format` 开关（默认 `apperror`）+ `test_error_formats` 两格式各一遍 + C4.3 驱动上游样例逐字对齐 | — |
| R-05 无 root | ✅ | 依赖走 `apt-get download` + 解包；外部服务用 Python mock（`mock_s3.py`/`mock_entitlements.py`）；`scripts/dev_postgres.sh` 起 PG（无需 root） | 真实 MinIO **未装**（登记，见 R-02） |
| R-06 上游接口变更 | ✅ | 契约文档记录取样点 + `docs/api/community/v2/openapi.yaml` 对照 + 端点/DTO 契约测试集中（`test_ops_endpoints`/`test_error_equivalence`/`test_http_dto`） | 上游出现 breaking change 时按"触发条件"升级 |
| R-07 大文件背压/超时 | ✅ | 1 GiB 流式 RSS 断言（`test_posix_large_file`）+ 慢客户端 + 空闲 408/无整体超时（C4.11，`verify_transfer_no_timeout.sh` ≥5 min）+ 8 并发大文件逐路 SHA-256（C9.1） | — |
| R-08 领域层被污染 | ✅ | CMake 目标图（编译期）+ `test_layering_guard` 源码检索 + `scripts/verify_link_graph.sh`（含越层注入自证） | — |
| R-12 `TCP_NODELAY` | ✅ | 强制 `set_tcp_nodelay(true)`；H-2/C1.2 自证（关闭时复现停顿）；容量基线顶部标注协议 | — |
| R-13 线程池=并发上限 | ✅ | `max_connections ≤ worker_threads` 启动校验 + 503 背压（C9.3 ⑤）+ 容量基线（C9.11：c4 之后吞吐不再提升，延迟显著上升） | 生产线程/连接取值需按目标硬件重算（C9.14 未验证） |
| R-14 大文件零拷贝 | 🟡 | ADR-006 受控复核 **2.12x** → 采纳方向（`docs/test-evidence/phase9-adr006.md`）；落地边界与 4 条重开条件已定 | **sendfile 数据面未实现**（判据只要求定稿）：在此之前**显式接受** httplib 路径的上限 |
| R-15 逐文件 fsync | ✅ | 三档 `durability`（`FSS_POSIX_DURABILITY`）+ 实测 4.3x（`per_file` 110 → `batch` 469 files/s，C9.15）+ ADR-008 批提交与不变量测试（`db/tests/001`） | 组合根仍用 env 而非 JSON 配置（见 §16.1 末尾"配置接通"） |
| R-16 容量测量方法 | ✅ | 独立进程 + 互不重叠绑核 + 每点位 3 次取中位数（`scripts/bench_baseline.sh`）+ 保留错误方法探针（`docs/appendix/capacity-probe/`）+ 门槛 C9.11；本轮新增"批量删除不得与测量并发""报告型点位"两条方法学约束 | 跨会话机器漂移 ≈40%（共享 WSL2 主机）⇒ 20% 判据的判定力受限，已如实登记 |
| R-23 tmp 名冲突 | ✅ | tmp 名含 `instance_id`+`pid`+计数；`test_posix_tmp_names`（C6.13） | — |
| R-24 重复创建 | ✅ | 幂等键唯一约束 + `ON CONFLICT` 原子领取（C6.11；`db/tests/003_concurrent_claim.sh` 含自证） | — |
| R-25 GC 误删在途 | ✅ | 租约到期 + 原子领取 + 领取后复查记录（`test_gc_lease` 6 用例）；**P9 又抓到一条会破坏它的缺陷**：POSIX 时间戳时基错（P9-D04）→ 已修复并把时间戳语义写进端口契约 | 多实例的数据库时钟（P6-D18）、调度与领导者选举（C9.26 未验证） |
| R-26 误用 SQLite 做多实例 | ✅ | `deployment.mode=multi` 的 5 条启动校验（含正例，C8.9）+ 组合根**拒绝启动**（`server_main.cpp`） | 多实例**运行形态未交付**（缺 PG 仓储/租约）→ C9.26 未验证 |
| R-27 NFS 语义未验证 | ⛔ | 上生产硬前提（C9.27）；实现**不依赖** NFS 文件锁、用 PG advisory lock（ADR-009） | 本环境无 NFS/多客户端共享挂载 ⇒ **未验证**，登记为**生产门禁**：未在目标存储上验证前不得上生产 |
| R-28 多实例 `syncfs` 干扰 | 🟡 | 部署建议"按 partition 分盘"；配置键 `shared_mount_required` / `one_filesystem_per_partition` 已在 schema 与示例中 | 干扰量级**未测**（C9.24 未验证）；组合根**未接**这两个键 → 目前只能靠部署纪律 |
| R-29 io_uring 当必需依赖 | 🟡 | 默认 `blocking`（ADR-010）+ `scripts/check_io_uring.sh`（0/1/**2=无结论**）+ 探测失败按配置回退 | `/v2/info` 暴露 `ioEngine`/`ioUringAvailable` 未做（C9.30 未验证）；seccomp 阻断已实测 `EPERM` |

**P9 遗留（如实登记，不阻塞 C9.1~C9.10）**：① 组合根当时仍**只读环境变量**、未接
`config/fss.example.json` —— **阶段 10 切片 1 已修**（`--config`/`--set` + 优先级 +
exit 78 失败语义）；**阶段 10 切片 2 进一步**把 GC 周期调度、`expiry.*` 接进组合根，
并把 16 个未实现键改为"非默认值 → 拒绝启动"（后续切片与 C10.16 / C10.16 续 继续收敛，当前三态为
**生效 93 / 拒绝启动 21 / 已读但无效果 42**；逐键登记在 `docs/operations.md` §1.2/§1.3）；
② 多实例相关的 `shared_mount_required`/`one_filesystem_per_partition` 仍不可配（在 §1.3 的
42 个"已读但无效果"键里）；③ PG 仓储/租约与 `deployment.mode=multi` 运行形态；④ sendfile 数据面
实现（ADR-006 §6 的门槛）；⑤ 真实硬件/多进程/容器类判据（C9.14、C9.17–C9.22、C9.26–C9.30）。

---

## 17. 设计决策索引

| ADR | 标题 | 状态 |
| --- | --- | --- |
| [ADR-001](adr/ADR-001-rpc-as-extension.md) | 以 gRPC/RPC 作为**平台外扩展**协议，而非 OSDU 合规接口 | 已采纳 |
| [ADR-002](adr/ADR-002-http-framework.md) | 使用上游 cpp-httplib（源码 vendored）作传输层 + 自建 `fss_http` 强化包装层 | 已采纳（**修订版，取代初版"完全自研"结论**） |
| [ADR-003](adr/ADR-003-storage-abstraction.md) | 以单一 `IBlobStore` 端口统一"集中存储"与"对象存储" | 已采纳 |
| [ADR-004](adr/ADR-004-persistence-strategy.md) | 位置/元数据持久化：内置 SQLite + 可选远端 Storage Service | 已采纳（阶段 6 复核） |
| [ADR-005](adr/ADR-005-s3-driver.md) | S3 驱动：**自研 SigV4**（OpenSSL）+ libcurl 数据面 + 原生预签名；编码/签名/寻址/错误映射约定与兼容矩阵 | 已采纳（P5 定稿；与 libcurl/Go SDK 的 4 处实测差异见其 §4） |
| [ADR-006](adr/ADR-006-large-file-data-plane.md) | 大文件数据面：**采纳 `sendfile` 方向**（P9 受控复核几何平均 **2.12x ≥ 1.5x**），但**实现未交付**；落地必须与控制面校验**同源**、可关闭、默认仍走 httplib 内容提供者 | 已采纳（P9/C9.12 定稿；证据 `docs/test-evidence/phase9-adr006.md`） |
| [ADR-007](adr/ADR-007-async-and-coroutines.md) | 异步/协程采纳策略：分层决策 + 量化触发条件（T1–T3），现在不整体重写 | 已采纳 |
| [ADR-008](adr/ADR-008-write-durability-protocol.md) | 小文件写入耐久性协议：两阶段批提交（同步先行、改名后置），更正了 ADR-007 的错误数字 | 已采纳（含机器可检查的不变量验证） |
| [ADR-009](adr/ADR-009-multi-instance-consistency.md) | 多实例一致性与共享状态设计：强一致控制面（PG）+ 租约 + 领导者选举 | 已采纳（5 个竞态已实测复现并验证修复） |
| [ADR-010](adr/ADR-010-io-engine-choice.md) | I/O 引擎：阻塞线程池为默认；io_uring 为可选引擎（默认容器 seccomp 阻断） | 已采纳 |
| [ADR-011](adr/ADR-011-logging-library.md) | 日志实现：自研最小实现（spdlog 评估后不采用，附重开触发条件） | 已采纳 |
| [ADR-012](adr/ADR-012-auth-and-tenant-binding.md) | 认证与租户绑定：本地 JWT（HS256）+ `partition` claim 绑定 + fail-closed；远端 Entitlements 与 RS256/JWKS 登记为未实现 | 已采纳（P8 切片 1；未实现项见其 §5.3） |

---

## 18. 与"低耦合"要求的逐条对应

| 需求表述 | 设计机制 | 验证 |
| --- | --- | --- |
| "整体采用分层设计" | L1–L5 五层，职责与依赖方向在 §3 明确 | 目标图（§4）+ 护栏测试 |
| "降低模块间的耦合度" | 端口/适配器（依赖倒置）；具体实现只在组合根创建；DTO 转换隔离在适配层 | 护栏测试"`new 具体实现` 只出现在 `main/` 与测试" |
| "支持两种存储方式" | `IBlobStore` + `BlobCapabilities`；能力分支只在 `LocationIssuer` 一处 | 同一套契约测试对两驱动各跑一遍；"`capabilities()` 调用点唯一"护栏 |
| "提供两种协议接口" | 两个互不链接的适配器目标共享同一应用层 | `fss_http_adapter` 与 `fss_grpc_adapter` 无相互依赖 + 双协议等价性测试 |
| "每一步完成后先做测试" | 10 个阶段，每阶段单一门槛命令与通过判据 | `docs/04-implementation-plan.md`；证据存 `docs/test-evidence/` |
