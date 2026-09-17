# ADR-003：以单一 `IBlobStore` 端口统一"集中存储"与"对象存储"

- 状态：已采纳（Accepted）
- 日期：2025（阶段 0）
- 相关文档：`docs/02-design.md` §4、§6，`docs/03-api-contract.md` §3

## 背景

需求：**同一套服务同时支持"集中存储"和"对象存储"两种后端**，且上层接口（REST/gRPC）完全一致。

难点来自 OSDU 规范本身的一个设计特征：**OSDU File Service 不代理文件字节**。它返回**签名 URL**，由客户端直连存储上传/下载（见 `docs/01-osdu-research.md` §6）。这一模型在对象存储上天然成立（S3/Azure Blob/GCS 都有原生预签名机制），但在"集中存储"（POSIX 文件系统 / NFS 共享卷）上**不存在原生签名 URL**。

## 术语与语义界定

| 术语 | 本文定义 | 典型实现 | 签名 URL |
| --- | --- | --- | --- |
| **集中存储**（centralized storage） | 服务可访问的 POSIX 文件系统（本地盘、NFS/SAN 挂载点、共享卷）。数据集中在存储节点/共享卷上，由**本服务**提供字节通道。 | 本地 `ext4`/`xfs` 目录、NFS 挂载点、CephFS | 无原生机制 → 由本服务自签 |
| **对象存储**（object storage） | S3 兼容 HTTP 对象存储。客户端可**直连**存储端点，服务不参与字节搬运。 | AWS S3、MinIO、Ceph RGW、SeaweedFS、华为 OBS、阿里云 OSS | 原生 SigV4 预签名 |

> 备注：OSDU 参考实现的 baremetal 部署（`file-core-plus/docs/baremetal/README.md`）实际使用 **S3 兼容对象存储（SeaweedFS）** + Postgres + RabbitMQ，即"on-premise"并不等于"集中存储"。本项目将两者作为**并列的两种驱动能力**，而不是把 on-premise 等同于 POSIX。

## 问题

若把"返回什么样的上传/下载地址"直接写进业务代码，会产生 `if (driver == posix) ... else ...` 式的分支扩散，破坏分层与可测试性，且新增第三种后端（如 Azure Blob）时需要改动业务逻辑。

## 决策

### 1. 定义单一端口 `IBlobStore`（L3 领域层定义，L2 基础设施层实现）

```cpp
namespace fss::domain {

struct ObjectRef { std::string container; std::string key; };

struct ObjectStat {
  std::uint64_t size = 0;
  std::string   etag;
  std::string   content_type;
  std::int64_t  last_modified_epoch = 0;
};

// 驱动能力声明：让上层"按能力编程"，而不是"按类型分支编程"
struct BlobCapabilities {
  bool native_presign        = false;  // 是否有原生预签名 URL
  bool server_side_copy      = false;  // 是否支持服务端复制
  bool range_read            = false;  // 是否支持字节区间读取
  bool streaming_put         = false;  // 是否支持流式写入
  std::size_t recommended_part_size = 0;  // 0 = 不支持分片
  std::string driver_name;             // 回填到响应中的 Driver 字段
};

class IBlobStore {
 public:
  virtual ~IBlobStore() = default;

  virtual BlobCapabilities capabilities() const = 0;

  // 容器（目录 / bucket）生命周期
  virtual Result<void> ensureContainer(const std::string& container) = 0;

  // 预签名（对象存储：原生；集中存储：由 LocationIssuer 兜底，此处返回 Unsupported）
  virtual Result<PresignedUrl> presign_put(const ObjectRef&, const PresignOptions&) = 0;
  virtual Result<PresignedUrl> presign_get(const ObjectRef&, const PresignOptions&) = 0;

  // 数据面原语
  virtual Result<void>       put(const ObjectRef&, ByteSource&, const PutOptions&) = 0;
  virtual Result<void>       get(const ObjectRef&, ByteSink&, const Range&) = 0;
  virtual Result<ObjectStat> stat(const ObjectRef&) = 0;
  virtual Result<void>       remove(const ObjectRef&) = 0;
  virtual Result<void>       copy(const ObjectRef& from, const ObjectRef& to) = 0;
  virtual Result<ListPage>   list(const std::string& container,
                                 const std::string& prefix,
                                 const std::string& token, int limit) = 0;
};
}
```

要点：
- `Result<T>` 是显式错误值类型（**不使用异常跨越层边界**），错误携带 `ErrorKind` 供上层映射为 OSDU `AppError`。
- `ByteSource`/`ByteSink` 是流式抽象（读/写回调），避免整文件驻留内存，也避免在接口中出现 `std::istream` 这类"实现细节泄漏"。
- `capabilities()` 是**唯一**允许上层感知驱动差异的入口，且只暴露能力布尔值，不暴露驱动类型。

### 2. 两个实现（L2）

| 目标 | 类 | 关键实现点 |
| --- | --- | --- |
| `fss_blob_posix` | `PosixBlobStore` | `container` → `root` 下的子目录；`key` → 相对路径。**路径安全**：拒绝 `..`、绝对路径、符号链接逃逸（`openat` + `O_NOFOLLOW` + 逐段校验），并校验解析后的真实路径仍在 `root` 之内。写入采用"临时文件 + `fsync` + `rename`"保证原子性。`native_presign = false`，`server_side_copy = true`（`copy_file_range`/`sendfile` 优先），`range_read = true`。 |
| `fss_blob_s3` | `S3BlobStore` | 用 OpenSSL 实现 **AWS SigV4**：`presign_put`/`presign_get` 生成带 `X-Amz-Signature` 的 URL（无需 AWS SDK）。签名器已由阶段 0 的 HMAC 链式派生测试固定（见 `docs/test-evidence/phase0.md`）。数据面用 libcurl。`copy` 走 `x-amz-copy-source`。`native_presign = true`。 |

`S3BlobStore` 覆盖 AWS S3 / MinIO / Ceph RGW / SeaweedFS；通过 `force_path_style` 配置项兼容不支持 virtual-host 风格的自建存储。

### 3. `LocationIssuer`：把能力差异收敛到一个策略点（L4 应用层）

```
Staging 上传地址请求
        │
        ▼
  ┌─────────────────────────────────────────────┐
  │ LocationIssuer                              │
  │  caps = blobStore.capabilities()            │
  │  if caps.native_presign:                    │
  │      → presign_put(...)          // 直连存储 │
  │  else:                                      │
  │      → SelfSignedTransferUrl     // 服务数据面│
  └─────────────────────────────────────────────┘
```

- 当 `native_presign == true`：返回对象存储的原生预签名 URL，`Driver = "s3"`。客户端直连，服务带宽零消耗。
- 当 `native_presign == false`：返回**本服务自签的传输 URL**，形如
  `https://<self>/v1/transfer/{token}?exp=...&sig=...`，`Driver = "posix"`。
  `token` 是一个自包含的、HMAC 签名的（`object_ref` + `op` + `exp` + `partition` + `nonce`）载荷，服务端在 `/v1/transfer` 校验签名与过期时间后代理字节。
  签名算法与 S3 SigV4 复用同一套 HMAC-SHA256 基础设施，但**使用独立的密钥域**（不同 key id / 不同签名字符串前缀），避免跨用途密钥复用。

因此**应用层与领域层不存在任何 `if driver == ...` 分支**；差异只体现在 `LocationIssuer` 内部的一次能力查询，以及响应中的 `Driver` 字段值。

### 4. 存储区（zone）模型与容器布局

OSDU 语义要求两个区：

| 区 | OSDU 名称 | 用途 | 生命周期 |
| --- | --- | --- | --- |
| landing / staging | staging area | `uploadURL` 生成后、元数据登记前 | 未在 N 小时内登记元数据则自动回收（默认 24h，OSDU 文档口径） |
| persistent | persistent area | 元数据登记成功后 | 长期保留，受 ACL/Legal 管控 |

容器（POSIX 目录 / S3 bucket）命名与映射由**分区（partition）配置**驱动，默认布局：

```
# 集中存储
<root>/<partition>/staging/<yyyy>/<mm>/<dd>/<file_id>
<root>/<partition>/persistent/<yyyy>/<mm>/<dd>/<file_id>

# 对象存储
container = <partition>-<zone>            # 例如 opendes-staging / opendes-persistent
key       = <yyyy>/<mm>/<dd>/<file_id>
```

`file_source`（对客户端可见的相对路径）与物理 `ObjectRef` 之间的映射由 `IFileLocationRepository` 保存，**不把物理路径暴露给客户端**，从而允许后端迁移（例如从 POSIX 迁到 S3）而不破坏已有记录。

## 备选方案与取舍

| 方案 | 结论 |
| --- | --- |
| 为两种后端分别实现两套 API（不同端点或不同字段语义） | 否决：违反"上层接口一致"的需求，客户端需要感知后端类型 |
| 统一由服务代理所有字节（集中存储和对象存储都一样） | 否决为默认：对象存储下会让服务成为带宽瓶颈，丧失 OSDU"客户端直连"的核心优势。但保留为**可选配置** `storage.proxy_mode=always`，用于内网无直连权限的场景 |
| 在业务层用 `if/else` 判断驱动 | 否决：分支扩散、难以单测、新增驱动要改业务 |
| **`IBlobStore` 端口 + `capabilities()` + `LocationIssuer` 策略点** | **采纳** |

## 后果

- 正面：新增驱动（Azure Blob / GCS / OBS）只需新增一个 `IBlobStore` 实现 + 配置枚举，应用层与 REST/gRPC 层零改动。可用**同一套契约测试**（`tests/conformance/blob_store_contract_test`）对全部驱动做一致性验证，该测试对 POSIX 与 S3 分别跑一遍。
- 负面 / 成本：`LocationIssuer` 与 `/v1/transfer` 数据面是本服务的**平台外扩展**（OSDU 无此端点），必须用独立路径前缀隔离并文档化。
- 安全要求（进入阶段 3/4 的门槛项）：
  - 自签 URL 必须包含过期时间，服务端强制校验，过期上限与 OSDU 一致（≤7 天，默认 1 小时）；
  - 自签 URL 必须绑定 `partition` 与操作类型（`put`/`get`），不可跨租户或跨操作重放；
  - `token` 必须是**不透明**的（base64url 编码的签名载荷），不得泄漏物理路径以外的信息；
  - POSIX 驱动必须抵御路径穿越与符号链接逃逸（专项测试用例）。

## 待办（进入阶段 2/3/4 时执行）

- [ ] 确定 `IBlobStore` 签名的最终形态（`Result`/`ByteSource`/`ByteSink`/`Range` 定义）
- [ ] 编写 `tests/conformance/blob_store_contract_test.cpp`，对内存/POSIX/S3 三实现共用
- [ ] 实现 `PosixBlobStore` 的路径安全测试（`..`、符号链接、绝对路径、超长路径）
- [ ] 实现 `S3BlobStore` 的 SigV4 预签名，并用 mock-S3 服务端**独立验签**（不能只测"能发出 URL"，必须测"服务端能验签通过"）
- [ ] 实现 `/v1/transfer` 的签名校验与重放/越权/过期拒绝测试
