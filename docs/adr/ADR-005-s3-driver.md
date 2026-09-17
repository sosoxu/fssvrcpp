# ADR-005：S3 驱动设计 —— 自研 SigV4 + libcurl 数据面 + 原生预签名

- 状态：**已采纳（Accepted）**
- 日期：2026（P5 定稿）
- 相关：`docs/adr/ADR-003`（存储抽象）、`docs/03-api-contract.md` §7、`docs/test-evidence/phase5.md`、
  `src/infra/blob/s3/`、`tests/tools/mock_s3.py`

---

## 1. 结论

**不引入 AWS SDK。** S3 驱动 = **自研 SigV4 签名器**（OpenSSL 的 SHA-256/HMAC）+ **libcurl 数据面**
+ **原生预签名地址**（客户端直连存储，服务不代理字节）。

```
IBlobStore
 └── S3BlobStore                       ← 只在组合根创建（R12）
      ├── SigV4Signer                  ← 纯函数：编码 / canonical request / 签名 / 预签名
      ├── libcurl（流式）               ← put/get/Range/HEAD/DELETE/copy/ListObjectsV2
      └── capabilities().native_presign = true   ← 地址由存储原生签名，服务不经手字节
```

**驱动选择只改配置**：`storage.driver: posix | s3`（同一二进制、同一套上层代码）。
证据：`scripts/verify_driver_switch.sh`（同一二进制 + 同一段端到端脚本，两模式各跑一遍全绿，
且 `SignedURL` 分别指向本服务与存储端点）。

---

## 2. 备选方案与取舍（为什么不是它们）

| # | 方案 | 优点 | 为什么没选 | 实测/依据 |
| --- | --- | --- | --- | --- |
| A | **aws-sdk-cpp** | 官方实现，覆盖面最全 | ① 镜像里**没有**该包（`apt-cache` 无、`github.com` 直连不可达，见 AGENTS §1）；② 依赖树庞大（其自身还依赖 aws-c-common/aws-c-event-stream 等）；③ 我们只需要 S3 的 6 个动词 + 预签名 | P0 环境探测（`docs/test-evidence/phase0.md`） |
| B | **libcurl 的 `--aws-sigv4` / `CURLOPT_AWS_SIGV4`**（让 curl 自己签名） | 零签名代码；libcurl 是第三方实现 | ① 实测**两处偏离 AWS 规范**：把 `host` 签成"URL 主机名（**去掉端口**）"、canonical query **不排序**；② 无法自定义要签的头集合（我们的 `x-amz-checksum-sha256`、`x-amz-copy-source` 需要参与签名）；③ 预签名 URL 的构造还是要自己拼 | 见 §4 的偏差实测（`docs/test-evidence/phase5.md` P5-M01/M02） |
| C | **服务代理全部字节**（不做原生预签名，一律走 `/v1/transfer`） | 实现最省事；POSIX 与 S3 语义完全一致 | 丢掉对象存储的核心价值：大文件吞吐不再受服务端带宽/内存约束，"两种存储方式"的目标（G2）退化成一种 | 需求 G2；`docs/00-final-design.md` §2 |
| D | **只支持预签名、不做服务端数据面** | 更少的 HTTP 代码 | `CreateFileMetadata` 必须在服务端做 staging→persistent 的**服务端复制**与校验和读取（契约 §2.6 的 12 步），没有数据面就无法完成 | 契约 §2.6；`test_s3_blob_store` 的契约用例 |

**选择**：自研签名器（A/B 的替代）+ libcurl 数据面（避免自己写 socket）+ 原生预签名（C 的反面）。

---

## 3. 实现约定（会被测试钉住的部分）

### 3.1 百分号编码（C5.5）

| 场景 | 规则 |
| --- | --- |
| 通用 | 只保留 RFC 3986 **unreserved**：`A-Za-z0-9-_.~`（★ `~` **不编码**） |
| canonical URI | 保留 `/`；其余逐字节 `%XX`（**大写**十六进制） |
| canonical query | 键与值都编码；`/` → `%2F`；空格 → `%20`（**不是 `+`**） |
| 同名 query 参数 | 按**值**升序（AWS 规范） |

### 3.2 头与 payload

| 项 | 取值 |
| --- | --- |
| SignedHeaders | 小写、按字典序、`;` 连接；`host` 与 `x-amz-date` 必须在内 |
| 同名头 | **用 `,` 连接**后再去多余空白（AWS 规范；曾被 AWS 官方向量抓出 P5-D08①） |
| 上传 payload 哈希 | 流式上传用 `UNSIGNED-PAYLOAD`（无法预知整包哈希）；给了 `expected_checksum` 时**同时**发 `x-amz-checksum-sha256`（base64）并本地边传边算 |
| 预签名 payload 哈希 | 由调用方决定：S3 用 `UNSIGNED-PAYLOAD`；其它服务可签真实体哈希（P5-D08②） |
| 没有整体超时 | 数据面只有"无进展"空闲断开（`transfer_idle_timeout_seconds`），与 C4.11 同一原则 |

### 3.3 寻址（C5.4）

`force_path_style` 决定**桶名放在哪里**，而它同时影响 canonical URI 与数据面路径：

| 形态 | Host | 路径 | 适用 |
| --- | --- | --- | --- |
| path-style（默认） | `<endpoint>` | `/<bucket>/<key>` | MinIO / Ceph RGW / SeaweedFS / 本机 mock |
| virtual-host | `<bucket>.<endpoint>` | `/<key>` | AWS S3 及支持子域的兼容实现 |

### 3.4 错误映射（C5.7）

| S3 `<Code>` / 状态 | ErrorKind | 对客户端 |
| --- | --- | --- |
| `NoSuchKey` / `NoSuchBucket` / 404 | `kNotFound` | 404 |
| `AccessDenied` / `SignatureDoesNotMatch` / 403 | **`kStorageAccessDenied`** | 403（★ 与"调用方角色不足"分开：这是**存储侧拒绝本服务**） |
| `SlowDown` / `ServiceUnavailable` / 503 | `kUnavailable` | 503 |
| `BadDigest` / `InvalidDigest` | `kChecksumMismatch` | 400（含 `details{expected,actual}`） |
| `InvalidRange` / 416 | `kInvalidArgument` | 400 |
| 其它 5xx | `kBadGateway` | 502 |

---

## 4. 与其它实现的**实测差异**（不跟随，但如实记录）

| 差异 | 对方行为 | 我们的选择 | 证据 |
| --- | --- | --- | --- |
| canonical host 的端口 | **libcurl 7.81** 去掉端口（请求里发的 `Host:` 却带端口） | 按收到的 `Host` 签（含非默认端口）—— 真实 S3/MinIO 按收到的 `Host` 重算 | phase5 P5-M01 |
| canonical query 排序 | **libcurl 7.81** 保留原顺序 | 按值排序（AWS 规范） | phase5 P5-M02 |
| 同名 query 参数的排序 | **Go SDK** 的 `url.Values.Encode()` 保留解析顺序（其 `TestPresignBodyWithArrayRequest` 的期望值据此产生） | 按值排序（AWS 规范） | `test_sigv4_aws_vectors` 的"有意不同"用例 |
| 路径里已含 `%XX` 时 | **Go SDK** 二次编码（`%2A` → `%252A`） | **单次**编码（canonical URI = 线上路径字节） | 同上 |

> 这四条差异都由**可执行断言**钉住（不是文档里的备注）：前两条在对拍测试里"只在对方也算对时才比对"，
> 后两条在官方向量测试里显式断言"我们不等于对方的值，并给出理由"。

---

## 5. 兼容矩阵（**已实测** vs 未实测）

| 实现 | path-style | virtual-host | 预签名 | 数据面 | 分页 | 状态 |
| --- | --- | --- | --- | --- | --- | --- |
| 本仓库 `mock_s3.py`（Python 独立验签） | ✅ | ✅ | ✅ | ✅ | ✅（>1000 键） | **已实测** |
| AWS S3 | — | — | — | — | — | ❌ **未实测**（无外网/无凭证） |
| MinIO | — | — | — | — | — | ❌ 未实测 |
| Ceph RGW | — | — | — | — | — | ❌ 未实测 |
| SeaweedFS | — | — | — | — | — | ❌ 未实测 |

**唯一的"真实性"保证**来自两侧独立实现：AWS 官方向量的**已知答案**（5 条完整请求逐字节匹配，
见 `test_sigv4_aws_vectors`）+ mock 侧用 Python `hmac`/`hashlib` **独立重算**验签。
**没有**在真实 S3/MinIO 上跑过——这条如实标注，不能当成已验证。

---

## 6. 后果

**收益**
- 依赖零增加（OpenSSL + libcurl 都已在环境里）；签名逻辑完全可测（纯函数）且能给出逐字节的
  canonical request/string-to-sign，排障不需要猜。
- 原生预签名让大文件吞吐不经过服务（G2）；`capabilities().native_presign` 是唯一的驱动分支点（R12）。

**代价 / 风险**
- 规范细节必须自己对齐：本阶段就踩了 3 处（同名头连接、预签名 payload 哈希、寻址形态影响路径），
  全靠"官方向量 + 两条独立实现对拍"抓出来。
- `UNSIGNED-PAYLOAD` 在 **HTTP（非 HTTPS）** 下会被某些实现拒绝（AWS 要求 HTTPS）；我们只在
  测试的 mock 上用 HTTP。生产必须 `verify_tls=true`（默认）。
- 大对象目前是**单次 PUT**（无分片上传）。超过 S3 单对象上限（5 GiB 的 PUT 限制）需要分片上传——
  **未实现**，见 §7。

---

## 7. 未验证 / 未实现（重开或补做的触发条件）

| 项 | 状态 | 触发条件 |
| --- | --- | --- |
| 真实 S3 / MinIO / Ceph / SeaweedFS 端到端 | ❌ 未实测 | 有可用端点与凭证时补 `FSS_STORAGE_S3_*` 端到端，并回填 §5 矩阵 |
| 分片上传（>5 GiB） | ❌ 未实现 | 出现 >5 GiB 对象的需求，或 S3 报 `EntityTooLarge` |
| `SlowDown` 的**退避重试** | ❌ 未实现（只映射错误） | 真实端点出现限流 |
| 会话令牌（STS）刷新 | ❌ 未实现（静态凭证） | 接入 STS/临时凭证 |
| AWS 官方文档里的 `sig-v4-test-suite` 全量用例 | ❌ 未取得 | 能访问官方仓库时导入（当前用 AWS Go SDK 自带的 5 条完整向量代替） |
