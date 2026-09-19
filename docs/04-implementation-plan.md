# 实现计划（分阶段 + 每阶段测试门槛）

> 铁律：**每一步完成后先做测试，测试通过后才进入下一步。**
> 本文档的每个阶段都有一个**唯一的、可自动执行的**门槛命令与明确的通过判据。
> 门槛未通过时，禁止开始下一阶段的任务（包括"顺便先写一点"）。

---

## 1. 阶段总览

| 阶段 | 名称 | 核心产出 | 门槛命令 | 状态 |
| --- | --- | --- | --- | --- |
| **P0** | 环境与契约可行性 | 工具链验证、proto 可生成可调用、双协议语义等价性证明、依赖选型 ADR | `ctest -L phase0` | ✅ **已完成**（见 `docs/test-evidence/phase0.md`） |
| **P1** | 分层骨架 + 通用库 + HTTP 传输层 | CMake 目标图、`Result`/`Error`、JSON 编解码、日志、配置、UUID/时间、`fss_http` 强化包装层（httplib 之上） | `ctest -L phase1` | ✅ **已完成**（12 个 L1 模块 + `fss_http`；C1.1~C1.15 全部满足，含 ASan/UBSan。见 `docs/test-evidence/phase1.md`） |
| **P2** | 领域模型 + 端口 + 应用层纯逻辑 | 领域模型、**15 个端口**、`LocationIssuer`/`ExpiryPolicy`/`ObjectKeyPolicy`/`KindValidator` 等策略、内存适配器、**13 个用例 + `ErrorKind` 覆盖率矩阵** | `ctest -L phase2` | ✅ **已完成**（C2.1～C2.9 满足；10 测试 / 952 断言。C2.10 的跨实现复用待 P3/P6 的实现到位；见 `docs/test-evidence/phase2.md`） |
| **P3** | 集中存储驱动 + 位置仓储 + 数据面 | `PosixBlobStore`、`SqliteLocationRepository`、自签传输 token 与 `/v1/transfer` 内核 | `ctest -L phase3` | ✅ **已完成**（C3.1~C3.12；9 测试 / 580 断言；见 `docs/test-evidence/phase3.md`） |
| **P4** | REST 适配层 + 端到端垂直切片（POSIX） | OSDU 全部端点、错误映射、DTO、三大错误体、`fss_server` 可启动 | `ctest -L phase4` | ✅ **已完成：19/19 路由 + `/metrics` + 上游样例逐字对齐（C4.3）+ 护栏 + 超时语义；C4.1~C4.11 全部满足** |
| **P5** | 对象存储驱动（S3 SigV4） | `S3BlobStore`、SigV4 签名/验签、mock-S3、同一套契约测试跑 S3 | `ctest -L phase5` | ✅ **已完成：C5.1~C5.10 全部满足**（AWS 官方向量 5 条逐字节匹配、独立验签 mock、同一套契约跑第三遍、分页、错误映射、S3 端到端、按配置切驱动、ADR-005） |
| **P6** | 元数据记录语义完整化 | `File.Generic` 全字段、版本链、staging→persistent 搬迁与回滚、`getFileList`、DMS、Delivery | `ctest -L phase6` | ✅ **已完成（C6.1~C6.13；9 测试 / 2578 断言）** |
| **P7** | gRPC 适配层 + 双协议等价性 | RPC 面（14 一元 + 3 流式/代理）+ 双协议等价性矩阵 | `ctest -L phase7` | ✅ **已完成（C7.1~C7.10 全部满足；17/17 RPC；6 测试 / 5378 断言）** |
| **P8** | 认证授权与多租户 | `IAuthorizer`、JWT 解析、角色映射、分区隔离、跨租户拒绝 | `ctest -L phase8` | ✅ **已完成（C8.1~C8.8 全部满足；C8.9 配置级 + C8.10 机制级完成；6 测试 / 1323 断言）** |
| **P9** | 硬化与交付 | 并发/容量基线（独立负载进程）、故障注入、指标、GC、打包、部署模板、运维手册；**定稿 ADR-006** | `ctest -L phase9 && scripts/run_all_gates.sh` | ✅ **已完成（C9.1~C9.13、C9.15、C9.16、C9.25、C9.31 满足；C9.8/C9.12 有独立证据文件；环境不具备的判据登记为未验证）**。**C9.31 为 P9 补交（P10 期间完成）**：GC 的 HTTP 按需端点 `POST /v2/gc:run` + `GcTask` 单飞护栏（证据 `docs/test-evidence/phase9.md` §12） |
| **P10** | 配置面接线 | `--config`/`--set`/`--print-config` + `fss::config::Load` 接入组合根、旧环境变量别名兼容、生产强校验、`docs/operations.md` 接通状态逐键更新 | `ctest -L phase10 && scripts/verify_config_wiring.sh` | ✅ **切片 1/2/3/4/5/6a/6b 全部完成（C10.1~C10.19）**：三态 156 键 **生效 106 / 拒绝启动 19 / 已读但无效果 31**（`docs/operations.md` §1.3）；真实二进制证据见 `docs/test-evidence/phase10.md`。**期间补交 P9 的 C9.31**（GC 按需端点；不改任何配置键，三态计数不变） |

**全阶段门槛（回归保证）**：`scripts/run_all_gates.sh` 必须按顺序跑 P0→P10 并全绿。
任何阶段的门槛脚本一旦被加入，后续阶段不得使其退化。

---

## 2. 门槛规则（必须遵守）

| 规则 | 内容 |
| --- | --- |
| R1 | 阶段门槛 = **单一命令** + **明确判据**（见每阶段的"通过判据"） |
| R2 | 门槛未通过 → **不得**开始下一阶段 |
| R3 | 门槛通过后 → 把命令、输出摘要、结论写入 `docs/test-evidence/phaseN.md`，然后才进入下一阶段 |
| R4 | 门槛测试**不得**被跳过/禁用/加 `[!mayfail]`。确需临时跳过时，必须在证据文件中记录理由、影响面与恢复计划 |
| R5 | 每个阶段结束时，`scripts/run_all_gates.sh` 中已启用的阶段必须**全部通过**（防止新阶段破坏旧阶段） |
| R6 | 契约相关改动（`docs/03-api-contract.md`）必须同时更新对应契约测试，并在证据文件里说明"契约变更原因" |

---

## 3. 阶段详述

### 阶段 0：环境与契约可行性 ✅ 已完成

**目标**：在写任何业务代码之前，用可执行证据回答"这套技术栈与架构前提是否成立"。

**交付物**

- `CMakeLists.txt`、`cmake/Dependencies.cmake`、`cmake/ProtoGen.cmake`
- `proto/osdu/file/v1/file_service.proto`（RPC 契约 v0.2）
- `tests/integration/test_phase0_toolchain.cpp`（7 个测试用例）
- `third_party/nlohmann/json.hpp`、`third_party/catch2/catch.hpp`（离线 vendored）
- ADR-001 / ADR-002 / ADR-003

**门槛命令**

```bash
cmake -S . -B build && cmake --build build -j"$(nproc)" && ctest --test-dir build -L phase0 --output-on-failure
```

**通过判据**

| # | 判据 |
| --- | --- |
| C0.1 | 构建成功，无编译错误 |
| C0.2 | 7 个测试用例全部通过，共 55 个断言 |
| C0.3 | 其中必须包含：C++20 特性、nlohmann/json 的 PascalCase 精确性、**HMAC-SHA256 链式派生对齐 AWS SigV4 官方向量**、SQLite3 持久化、libcurl 真实 HTTP 往返、proto 生成+真实 gRPC 回环调用、**REST/RPC 语义等价** |

**结果**：✅ 通过（55 assertions in 7 test cases）。过程中该门槛**发现并修复了一个真实缺陷**：
HMAC 链式派生误用十六进制字符串而非原始摘要字节（SigV4 签名器的经典错误），
现已固化为回归测试。详见 `docs/test-evidence/phase0.md`。

**附带决策产出**：candidate `cpp-httplib` 因"预编译 `.so` 的宏耦合 ABI 风险"被实测否决 → ADR-002。

---

### 阶段 1：分层骨架 + 通用库 + HTTP/1.1 内核

**目标**：建立**可强制执行的**分层目标图与 L1 通用能力，其中 HTTP 部分是
**cpp-httplib 0.26.0（vendored 源码）作传输层 + `fss_http` 强化包装层**（ADR-002；后续所有协议的基石）。

**交付物**

| 路径 | 内容 |
| --- | --- |
| `CMakeLists.txt` + `cmake/CompilerWarnings.cmake` | 完整目标图（`fss_common`/`fss_http`/`fss_crypto`/`fss_ids`/`fss_time`/`fss_fs`/`fss_config`/`fss_logging`/`fss_net`/`fss_bytes`/`fss_domain`/`fss_app`/…） |
| `src/common/result/` | `Result<T>`（`Ok`/`Err`）、`Error`、`ErrorKind`、`FSS_TRY` 宏 |
| `src/common/json/` | JSON 读写封装、PascalCase/camelCase 辅助、未知字段保留、解析错误定位 |
| `src/common/crypto/` | `hmac_sha256`（**原始字节链式**）、`sha256`、`md5`、`base64url`、**常量时间比较** |
| `src/common/ids/` | UUIDv4、确定性序列 ID（测试用） |
| `src/common/time/` | `IClock`、`SystemClock`、`ManualClock`、ISO-8601 / RFC-1123 / `yyyy-MM-dd'T'HH:mm:ss.SSSZ` 解析格式化 |
| `src/common/fs/` | 原子写（tmp + `fsync` + `rename`）、安全路径校验（逐段 + `O_NOFOLLOW` + 真实路径复核）、目录递归 |
| `src/common/config/` | 配置加载（文件 + 环境变量 + 命令行）、`${ENV:VAR}` 引用、**一次性全量校验** |
| `src/common/logging/` | 结构化 JSON 行日志、级别、`correlation_id` 上下文、**敏感字段脱敏** |
| `src/common/net/` | URL 解析/构造、查询串编解码、百分号编码 |
| `src/common/bytes/` | `ByteSource`/`ByteSink`/`Range`（RFC 7233 归一化）、定长/对齐缓冲、计数 reader（上限）、令牌桶限流 |
| `src/common/sys/` | 系统能力探测：`ProbeIoUring()`（区分 可用 / EPERM / ENOSYS）+ `DecideIoEngine()`（纯函数决策） |
| `src/common/http/` | **`fss_http` 强化包装层**：`Server` 门面（隐藏 httplib 类型）+ 硬上限强制 + 中间件链 + Range 归一化 + 错误归一化。**唯一**允许 `#include <httplib.h>` 的位置。cpp-httplib 0.26.0 源码已 vendored 到 `third_party/httplib.h`（ADR-002） |
| `tests/unit/test_layering_guard.cpp` | **分层护栏测试**（源码检索） |
| `tests/integration/test_httplib_hardening.cpp` | ★ **H-1/H-2 回归 + 边界**：越界 Range→416、超限 CL→413、chunked 超限→400、长度不符→400、重复 CL→400、64 KiB 头→400、并发/keep-alive、大文件 RSS 上限 |
| `tests/integration/test_http_server.cpp` | 真实端口上的路由/keep-alive/Range/chunked/超限 |

**任务清单**

1. 建立 CMake 目标图，**先写护栏测试再写实现**（护栏必须先能失败，再让实现通过）。
2. `Result<T>`/`Error`/`ErrorKind`：`ErrorKind` 直接采用契约文档 §5 的枚举（作为端口的通用错误语言）。
3. `crypto`：HMAC 链式派生必须复用 P0 已固化的正确实现与向量测试。
4. `fss_http` 门面：包住 httplib，对外不暴露其类型；建立"只有此处可 include httplib.h"的护栏。
5. **H-2 三重防护**：`set_pre_routing_handler` 按 `Content-Length` 前置拒绝（413）；自封装计数 reader 超限立即中止；读取字节数与 `Content-Length` 一致性断言。
6. **H-1 回归**：流式响应 + 越界 Range 必须得到 `416`（不得出现下溢的 `Content-Length`）。
7. 中间件链：`CorrelationId` → `AccessLog` → `BodyLimit` → Handler → `ErrorMapping`；不把 httplib 的默认 400 直接漏给客户端。
8. 数据面原语：`Content-Length`/chunked 流式读写、`Range`（`a-b`/`a-`/`-N`/多段/越界）归一化、`HEAD`。
9. 路由：`:` 命名段；**方法不匹配按实测行为返回 404**并写入契约（不强行改成 405）。
8. 配置：加载 + 校验 + 打印生效配置（脱敏）。
9. 日志：结构化输出 + 脱敏 + `correlation_id` 透传。

**门槛命令**

```bash
cmake --build build -j"$(nproc)" && ctest --test-dir build -L phase1 --output-on-failure
```

**通过判据**

| # | 判据 |
| --- | --- |
| C1.1 | **护栏测试生效性自证**：临时向 `src/domain/` 插入 `#include <grpcpp/grpcpp.h>` 后构建**必须失败**；移除后恢复通过（此验证脚本化为 `scripts/verify_guard.sh`，其输出记入证据） |
| C1.2 | **✅ 已实现（切片 7）。** `test_httplib_hardening.cpp`：**H-1 回归**（越界 Range → 416，且响应头无下溢 `Content-Length`）、**H-2 回归**（超限 CL → 413 且 handler **无副作用**；chunked 超限 → 400 且 handler **无副作用**；长度不符 → 400）、重复 `Content-Length` → 400、`CL`+`chunked` 并存 → 400、9000 字节头 → 400、**target ≥ 8192 → 连接被关闭且无响应**（实测更正） |
| C1.2b | **✅ 已实现（切片 7）。** 护栏生效性自证：`scripts/verify_http_hardening.sh` 临时拆掉 H-2 的两道防护，测试**必须失败且症状是"超限却 201/413 退化"**（已纳入 `run_all_gates.sh`） |
| C1.3 | **✅ 已实现（切片 7）。** HTTP Server 集成（真实回环端口）：keep-alive 复用、并发 ≥ 50、`chunked` 请求体、`Range` 的 `bytes=a-b`/`bytes=a-`/`bytes=-N`/多段/越界；**大文件（≥ 1 GiB 虚拟）流式收发期间 RSS 峰值增长 < 64 MiB**（证明流式，不整文件驻留） |
| C1.4 | `Result<T>`：`FSS_TRY` 在错误路径上**不吞错、不泄漏**（有专门测试），错误携带 `ErrorKind` |
| C1.5 | 配置：非法配置 → 启动失败并**一次性列出全部问题**（不是只报第一个） |
| **C1.6** | **✅ 已实现（切片 4）。** 日志脱敏：`secret_key`/`access_key`/`token`/`sig` 在日志中不出现明文；JSON Lines 一条一行、级别过滤、并发写不交错、换行注入被转义 |
| **C1.7** | **✅ 已实现（切片 8）。** 无内存/资源泄漏：`scripts/run_sanitizers.sh`（ASan + UBSan + LeakSanitizer，`-fno-sanitize-recover=all`）下 phase0+phase1 共 15 个测试全绿；**无任何按目标关检查的例外** |
| **C1.8** | **✅ 已实现（切片 2）。** `common/json`：往返一致、未知字段原样保留、**PascalCase 精确性**（camelCase 访问必须失败），解析错误带位置信息 |
| **C1.9** | **✅ 已实现（切片 2）。** `common/fs`：安全路径校验（≥12 个恶意输入：`..`、绝对路径、NUL、超长、符号链接形态）全部被拒；原子写（tmp+fsync+rename）不产生半截文件 |
| **C1.10** | **✅ 已实现（切片 2）。** `common/crypto` + `common/time` + `common/ids`：HMAC-SHA256 对齐 AWS SigV4 官方向量（含**原始字节链式派生**）、SHA-256/base64url 正确、常量时间比较生效、`ManualClock` 可精确控制时间、ID 生成器可确定性注入 |
| **C1.11** | **`TCP_NODELAY` 生效性自证**：默认开启；并把"关闭 `TCP_NODELAY`"作为对照用例——此时 keep-alive 小请求**必须**复现 ~40 ms 停顿特征（否则说明该测试无效）。依据：实测 23 req/s vs 21,062 req/s（`docs/05-capacity-and-concurrency.md` §1.2） |
| **C1.12** | **并发上限可配置且可观测**：线程池不再硬编码为 8，按公式推导；超限返回 **503 + `Retry-After`**（不排队到超时）；测试断言并发连接数达到配置值（服务端 in-flight 计数） |
| **C1.13** | **传输内存预算**：`并发上限 × transfer_buffer_bytes ≤ transfer_memory_budget`，违反时**拒绝启动**并一次性列出全部问题 |
| **C1.14** | **✅ 已实现（切片 6）。** **io_uring 能力探测能力（L1 工具）**在 P1 交付：能区分"可用 / `EPERM`（seccomp）/ `ENOSYS`（内核不支持）"，并返回结构化结果供 P2 的 `IIoEngine` 与 P3 的引擎选择使用。**注意**：`IIoEngine` **端口**定义属于 P2（端口在 L3），引擎**实现**属于 P3 —— 本判据只要求探测能力 |
| **C1.15** | **✅ 已实现（切片 6）。** **io_uring 能力探测与回退语义**：在**默认 seccomp 容器**内探测必须报告不可用；`io_engine=auto` 时**成功回退**并记录，`io_engine=uring` 时**拒绝启动**（用 `scripts/check_io_uring.sh` 复现）。依据：ADR-010 §3 实测 `EPERM` |

**退出条件**：C1.1–C1.7 全部满足，证据写入 `docs/test-evidence/phase1.md`。

**风险**：R-01（cpp-httplib 缺陷/行为变化）—— 本阶段是其集中处置点。
H-1/H-2 两个缺陷是**已实测复现**的（不是假设），C1.2/C1.2b/C1.3 是关键防线。

---

### 阶段 2：领域模型 + 端口 + 应用层纯逻辑  ✅ 已完成

> **收口状态（切片 1~5 全部完成）**：领域模型 + 三个纯策略 + 15 个端口 + 端口契约测试基类与
> 三个内存适配器 + `LocationIssuer` 与 `capabilities()` 调用点护栏 + **13 个用例与 `ErrorKind`
> 覆盖率矩阵**。门槛：`ctest -L phase2` → **10 测试 / 952 断言 / 45 用例**，C2.1～C2.9 全部满足；
> 证据见 `docs/test-evidence/phase2.md`。
>
> ✅ **一处接口扩充（切片 5b 开局定案）**：`getFileList` 需要"按 partition + 时间区间 + 分页 +
> `UserID` 查询位置记录"，因此给 `IFileLocationRepository` 增加 `List(partition, LocationQuery)`
> （即本计划阶段 3 任务 5 的 `FindAll`），并给 `FileLocation` 加 `user_id`（映射 schema 的
> `created_by`）。设计同步见 `docs/02-design.md` §9.1。
>
> **交接给后续阶段**：C2.10 的"`sqlite` 与 `postgres` 共用同一套契约测试"要等实现到位
> （P3/P6/P9），届时**只加实例化**、不得复制断言；`IIoEngine` 选择逻辑护栏属 P3 组合根。

**目标**：把全部业务规则以**零 IO、零外部依赖**的形式实现完，并用内存适配器证明可测试性。

**交付物**

| 路径 | 内容 |
| --- | --- |
| `src/domain/model/` | `FileLocation`、`FileMetadataRecord`（含全部 `File.Generic` 字段）、`Acl`、`Legal`、`Ancestry`、`FileSourceInfo`、`FileData`、`ObjectRef`、`SignedLocation`、`StorageZone`、`StorageDriver` |
| `src/domain/error/` | `ErrorKind` 全部取值 + 与 `common::Error` 的桥接 |
| `src/domain/ports/` | `IBlobStore`（含 `BlobCapabilities`）、`IBlobStoreFactory`、`IFileLocationRepository`、`IMetadataRepository`、`IPartitionRegistry`、`IAuthorizer`、`ILegalValidator`、`ISchemaValidator`、`IEventPublisher`、`IAuditLogger`、`IClock`、`IIdGenerator`、`ISelfSignedUrlCodec` |
| `src/app/services/expiry_policy.cpp` | `expiryTime` 解析：合法 / 缺省 1H / 超限截断 7D / 非法 → `kInvalidArgument` + **固定消息** |
| `src/app/services/object_key_policy.cpp` | `FileSource` ↔ `ObjectRef` 双向映射、日期分层、前导斜杠、1024 上限、`FileID` 正则与长度 |
| `src/app/services/kind_validator.cpp` | kind 正则 + 4 段 + `wks` + `dataset--File.Generic` + 三条固定消息 |
| `src/app/services/location_issuer.cpp` | **唯一**的 `capabilities()` 分支点（ADR-003 §6.3） |
| `src/app/usecases/*.cpp` | 13 个用例：`GetUploadLocation`、`GetFileLocation`、`GetDownloadLocation`、`GetFileList`、`CreateFileMetadata`、`GetFileMetadata`、`DeleteFileMetadata`、`GetStorageInstructions`、`GetRetrievalInstructions`、`CopyFiles`、`GetFileSignedUrl`、`RevokeUrl`、`GetInfo` |
| `src/infra/blob/memory/` | `InMemoryBlobStore`（含可控故障注入：延迟/错误/字节截断） |
| `src/infra/location/memory/`、`src/infra/metadata/memory/` | 内存仓储（`partition_id` 严格隔离） |
| `tests/framework/` | `ManualClock`、`SequentialIdGenerator`、`RecordingEventPublisher`、`FakePartitionRegistry`、`AllowAllAuthorizer`、blob/仓储契约测试基类 |

**任务清单**

1. 领域模型：**只允许**依赖 `fss_common`。开放字段（`ExtensionProperties` 等）用 `Json` 承载原样内容。
2. 端口签名定稿（ADR-003 §待办第 1 项）。
3. `ExpiryPolicy`：把"缺省 1H / 截断 7D / 非法 400 + 固定消息"逐条实现并测试。
4. `ObjectKeyPolicy`：实现 `<userId>/<epochMillis>-<ts>/<fileID>` 布局与安全校验；**路径安全测试 ≥ 10 个恶意输入**。
5. `KindValidator`：4 段 + 固定消息。
6. `LocationIssuer`：能力分支只此一处；`capabilities()` 调用点护栏。
   ✅ **切片 5a 已完成**：`src/app/services/location_issuer.{h,cpp}` + `test_location_issuer.cpp`
   （C2.6，80 断言）+ `test_capability_guard.cpp` + `scripts/verify_capability_guard.sh`（C2.7，自证 ①~④）。
7. **仓储与 BlobStore 契约测试基类**：一套测试同时约束内存/POSIX/S3 三实现
   （`tests/framework/port_contract.h`）。
   ✅ **切片 4 已完成基类 + 三个内存实现**（blob/location/metadata，331 断言）；
   后续 POSIX/S3/SQLite/PostgreSQL 到位后**只加实例化**，不得复制断言（C2.10）。
8. 13 个用例：**全部只依赖端口**，用内存实现驱动；覆盖成功路径 + 每个 `ErrorKind`。
   ✅ **切片 5b 已完成**：`src/app/usecases/usecases.{h,cpp}`（13 个用例）+ `test_usecases.cpp`
   （9 用例 / 140 断言）+ `test_error_kind_coverage.cpp`（**12/12 无空缺**）。

**门槛命令**

```bash
cmake --build build -j"$(nproc)" && ctest --test-dir build -L phase2 --output-on-failure
```

**通过判据**

| # | 判据 |
| --- | --- |
| C2.1 | `fss_app` 与 `fss_domain` 的链接行**只有** `fss_domain`/`fss_common`（`scripts/verify_link_graph.sh` 输出证明） |
| C2.2 | 13 个用例全部有单元测试；每个 `ErrorKind` **至少一个**触发用例（覆盖率矩阵打印出来，无空缺）。✅ 切片 5b：矩阵 12/12，见 `docs/test-evidence/phase2.md` §1e |
| C2.3 | `ExpiryPolicy`：合法（`5M`/`2H`/`1D`）、缺省 → 3600s、`8D` → 截断为 604800s、非法（`5X`/`abc`/空/负数）→ `kInvalidArgument` + 逐字节匹配固定消息 |
| C2.4 | `ObjectKeyPolicy`：≥ 10 个恶意键（`..`、`/etc/passwd`、`a/../../b`、NUL 字节、超长、符号链接形态）全部被拒 |
| C2.5 | `KindValidator`：合法/非法各 ≥ 6 例；三种固定消息逐字节匹配 |
| C2.6 | `LocationIssuer` 在两个能力组合（`native_presign=true/false`）下分别产出原生预签名与自签 URL；**断言领域结果中不含驱动类型分支的痕迹**。✅ 切片 5a：`test_location_issuer`（行为+数据+源码+调用计数四层断言，见 `docs/test-evidence/phase2.md` §1d） |
| C2.7 | `capabilities()` 调用点护栏：除 `location_issuer.cpp`、`storage_instruction_service.cpp` 外出现即失败。✅ 切片 5a：`test_capability_guard`（含非空洞性断言）+ `scripts/verify_capability_guard.sh` 自证 |
| C2.8 | 全部用例使用 `ManualClock`（无真实睡眠），测试套件运行 < 2 秒。✅ 实测 0.03 s |
| C2.9 | ASan/UBSan 干净。✅ `run_sanitizers.sh` 覆盖 phase0/1/2（1+14+10 测试）全绿 |
| **C2.10** | `sqlite` 与 `postgres` 两种仓储实现**共用同一套契约测试**（含唯一约束/冲突/幂等语义）。🚧 切片 4 已交付契约基类 `tests/framework/port_contract.h` 与三个内存实现（C2.10 的内存侧）；`sqlite`/`postgres` 到位后只加实例化 |
| **C2.11** | 端口清单为 **15 个**，其中 ADR-009 的 `ILeaseRepository` 与 ADR-010 的 `IIoEngine` 已定义（含 64 位偏移与区间读签名）；`capabilities()` 与 `IIoEngine` 的选择逻辑**只出现在允许的位置**（护栏断言） |

**退出条件**：C2.1–C2.9 满足，证据写入 `docs/test-evidence/phase2.md`。

---

### 阶段 3：集中存储驱动 + 位置仓储 + 数据面  ✅ 已完成

> **收口状态（切片 1~5 全部完成；C3.1~C3.12 满足）**：
> · 切片 1：`PosixBlobStore` 通过**与内存实现共用的同一套契约测试**（C3.1），路径安全 C3.2、
>   原子写 C3.3、Range 边界 C3.5、并发 C3.6 满足；另补 L1 增量 SHA-256（流式校验和）。
> · 切片 2：`SqliteLocationRepository` 同样跑通共享契约（C3.8），SQL 护栏 C3.9 落地，
>   并**闭合 C2.10 的 SQLite 一侧**；同时修正设计文档 §9.1 的主键矛盾（P3-D05）。
> · 切片 3：`BlockingIoEngine`（`pread` 定位读并发，C3.10）+ `IFileSync` 落盘接缝与
>   `fsync` 分级（C3.11）；`src/infra/io/` 交付。
> · 切片 4：`HmacTransferTokenCodec` + `/v1/transfer` 内核（C3.7）；`src/infra/transfer/` 交付。
> · 切片 5：1 GiB 流式写读（RSS 增长 2.6 MiB，C3.4）、32 线程零 busy 的有界写并发（C3.12）、
>   `UringIoEngine` 探测骨架；并修正 `scripts/check_io_uring.sh` 的"空证据结论"（P3-D09）。
> **交接**：U1–U4 未满足前不得启用 io_uring；P4 组合根在 `chosen==kUring` 时须再查
> `UringIoEngine::enabled()`（P3-D08）。证据见 `docs/test-evidence/phase3.md`。

**目标**：落地**集中存储（POSIX）**这一条完整技术路径，包括自签传输 URL 与字节通道内核。

**交付物**

| 路径 | 内容 |
| --- | --- |
| `src/infra/blob/posix/posix_blob_store.cpp` | `container`→目录、`key`→路径；`put`（流式 + 原子写 + `fsync`）、`get`（支持 Range）、`stat`、`remove`、`copy`（`copy_file_range` 优先，回退到流式）、`list`、`ensure_container` |
| `src/infra/io/` | **`IIoEngine` 实现（ADR-010）**：`BlockingIoEngine`（默认，`pread`/`pwrite`/`fsync` + 有界线程池）与 `UringIoEngine` 探测骨架（可先不启用） |
| `src/infra/location/sqlite/` | `SqliteLocationRepository`（WAL、`busy_timeout`、`partition_id` 索引、`data` JSON 列保留未知字段） |
| `src/infra/transfer/transfer_token.cpp` | `HmacTransferTokenCodec`：载荷 = `{op, container, key, partition, exp, nonce}`，HMAC-SHA256 + base64url + 常量时间校验 |
| `tests/tools/mock_s3.py` | （本阶段仅落地骨架，P5 完善）Python 标准库实现的 mock 存储 |
| `tests/integration/test_posix_blob_store.cpp` | 契约测试基类实例化 + 路径安全 + 大文件 + 并发 |
| `tests/integration/test_transfer_endpoint.cpp` | 自签 URL 的正常/篡改/过期/越权/跨操作/重放 |

**任务清单**

1. `PosixBlobStore` 实现 8 个原语；**容器与键必须经过 `SafeJoin`**。
2. 原子写：写临时文件 → `fsync` → `rename`；配置 `fsync_policy`。
   ✅ 切片 1/3：`PosixBlobStore` 原子写 + `IFileSync` 接缝 + `FsyncPolicy{Always,BySize,Never}`（C3.11）。
3. `copy`：优先 `copy_file_range`（同文件系统、零拷贝），失败回退流式；**跨文件系统必须可用**。
4. `list`：按 `prefix` 分页（continuation token = 最后一个 key）。
5. `SqliteLocationRepository`：`Save`/`Find`/`FindByFileSource`/`List`（时间区间 + 分页 + `UserID` 过滤，P2 已定名，原计划的 `FindAll`）/`UpdateSignedUrl`/`Delete`；**所有 SQL 都必须带 `partition_id`**（护栏测试检查）。
6. 传输 token：签发 + 校验 + 过期 + 操作类型绑定 + 租户绑定。
   ✅ **切片 4 已完成**（C3.7）：`HmacTransferTokenCodec`（独立密钥域 + 常量时间验签 + 注入时钟判过期）。
7. `/v1/transfer/{token}` 内核（只做 token 校验 + 字节转发，不注册到 REST 路由表——P4 才接 HTTP 适配层）。
   ✅ **切片 4 已完成**：`TransferEndpoint`（操作绑定 / 租户绑定 / 载荷取键 / 经 `IBlobStore` 转发）。

**门槛命令**

```bash
cmake --build build -j"$(nproc)" && ctest --test-dir build -L phase3 --output-on-failure
```

**通过判据**

| # | 判据 |
| --- | --- |
| C3.1 | `PosixBlobStore` 通过**与内存实现共用的同一套契约测试**（证明行为一致） |
| C3.2 | 路径安全：≥ 12 个恶意键全部被拒（含预置符号链接逃逸场景） |
| C3.3 | 原子写：注入"写入中途崩溃"（进程内模拟：写入后不 rename）→ 目标路径**不出现半截文件** |
| C3.4 | 大文件：写入并读回 **≥ 1 GiB**（稀疏/虚拟数据），进程 RSS 峰值增长 **< 64 MiB**（证明流式，不整文件驻留）。✅ 实测增长 **2652 KiB** |
| C3.5 | `Range` 读取边界：`bytes=0-0`、`bytes=-1`、`bytes=len-1-`、`bytes=len-`、越界 → 各自正确 |
| C3.6 | 并发：8 线程并发写不同键、读同一键，全部正确（无数据竞争，ASan/TSan 干净） |
| C3.7 | 传输 token：正常用法通过；篡改 `op`/`container`/`key`/`partition`/`exp` 各一例**全部拒绝**；过期拒绝；`get` token 用于 `put` 拒绝；跨 partition 拒绝 |
| C3.8 | 位置仓储：所有查询在双租户数据集上验证**不串租户**；未知 JSON 字段往返不丢失 |
| C3.9 | 「无 `partition_id` 条件的 SQL」护栏测试通过 |
| **C3.10** | **`pread` 定位读的并发安全**：8 线程并发读同一大文件的不同区间，结果与单线程逐一读取完全一致（证明不使用共享文件偏移） |
| **C3.11** | **`fsync` 分级策略**：`fsync_policy=by_size` 时，小于阈值不 fsync、大于阈值必须 fsync（用系统调用计数或注入的 `IFileSync` 端口断言，不靠猜测）。依据：实测 FULL 比 NORMAL 差 **21 倍** |
| **C3.12** | **有界写并发**：默认 8；并发 32 写入时**不得**出现 SQLite `busy`/`SQLITE_BUSY` 失败（超出部分排队）。依据：实测 8 线程 25,471 tx/s 为峰值，32 线程降到 13,475 tx/s。✅ 32 线程零失败/零 busy |

**退出条件**：C3.1–C3.9 满足，证据写入 `docs/test-evidence/phase3.md`。

---

### 阶段 4：REST 适配层 + 端到端垂直切片（POSIX）  🚧 进行中

> **当前进度（切片 1~5，阶段已完成）**：`http_error_mapper` + REST DTO（切片 1）；`Router`（`Wrap()` 横切：
> 上下文/错误映射/异常兜底）+ `src/main/server_main.cpp`（组合根，**服务可真实启动**）+ 13 条路由
> （运维 3 / 位置 5 / 元数据 3 / 数据面 2）（切片 2~3）；**自签数据面 `/v1/transfer` 打通真实字节**
> —— 100 MiB 上行/下行 SHA-256 一致、0 字节边界、负例 404/403/401（切片 3）。
> C4.4/C4.5/C4.6/**C4.8** 已满足，C4.2/C4.7 部分满足。
> 切片 4 补齐：DMS 6 + Delivery 1 + revoke 1（19/19）+ `/metrics` + 负向矩阵 12/13 +
> 全字段黄金样例 + `expiryTime` 数值 + `verify_composition_root.sh` + 数据面无整体超时/空闲 408。
> 切片 5 用**本机归档的上游源码**（`/home/ll/osdu-file-upstream`，commit `d7c25c2`）复核了验收样例：
> 期望消息逐字对齐、并把"§3.4 有 13 条负向样例"更正为"10 条上游在跑 + 2 条上游注释掉但我们照实现"。
> **无留白**。
> 证据见 `docs/test-evidence/phase4.md`。

**目标**：让服务**真正可被 OSDU 客户端使用**（POSIX 模式），并证明契约逐条符合。

**交付物**

| 路径 | 内容 |
| --- | --- |
| `src/adapters/http/dto/*` | `FileMetadata`（PascalCase 编解码）、`LocationRequest/Response`、`FileLocationResponse`、`FileListRequest/Response`、DMS、Delivery、`VersionInfo`、三种错误体 |
| `src/adapters/http/routes/*` | 全部 OSDU 端点（契约 §2）+ `/v1/transfer` + `/metrics` |
| `src/adapters/http/middleware/*` | `CorrelationId`、`AccessLog`、`Auth`（P8 前为 allow-all + 显式告警）、`BodyLimit`、`ErrorMapping` |
| `src/adapters/http/http_error_mapper.cpp` | `ErrorKind` → (HTTP status, `reason`, `message`)；三种错误体形态 |
| `src/main/server_main.cpp` | **组合根**：配置 → 依赖构造 → 路由注册 → 启动 HTTP 监听 |
| `config/fss.example.json` | 完整配置样例 |
| `tests/conformance/test_rest_contract.cpp` | 契约 §2 全部端点 |
| `tests/conformance/test_error_formats.cpp` | 契约 §5 + 三种错误体 |
| `tests/conformance/test_ops_endpoints.cpp` | 纯文本健康检查、`/v2/info`、免鉴权 |
| `tests/integration/test_upload_flow_posix.cpp` | 端到端：`uploadURL` → `PUT /v1/transfer/{token}` → `metadata` → `downloadURL` → `GET` → `DELETE` |

**任务清单**

1. DTO 与编解码：**先写黄金样例测试**（契约 §3.3/§3.4），再实现编解码。
2. 路由注册：路径含 base path `/api/file`；`@Hidden` 端点也实现（契约明确它们可用）。
3. 错误映射：三种形态 + 固定消息 + `reason` 表。
4. 运维端点：纯文本 body 与 `Content-Type` 必须精确。
5. 组合根：唯一实例化具体实现的位置；启动时打印生效配置（脱敏）与显著告警（若 `auth.mode=disabled`）。
6. 端到端测试：真实进程 + 真实端口 + 真实文件系统（临时目录）。
7. `/metrics`（最小可用：请求计数 + 延迟直方图）。

**门槛命令**

```bash
cmake --build build -j"$(nproc)" && ctest --test-dir build -L phase4 --output-on-failure
```

**通过判据**

| # | 判据 |
| --- | --- |
| C4.1 | 契约 §2 全部 **19 个** OSDU 端点的路径/方法/状态码断言全部通过（对真实 HTTP 端口） |
| C4.2 | 契约 §3.3 黄金样例：`POST metadata` → `201` + `id` 格式 `"<partition>:dataset--File.Generic:<uuid>"`；`GET metadata` 返回全字段 + `version` |
| C4.3 | 契约 §3.4 的**全部样例**逐条通过：10 条上游在跑的负向样例返回期望状态码 + **逐字**期望消息；2 条上游自己注释掉的行（`File_invalid_ScalarIndicator` / `File_Datatype_Mismatch`）**照实现并登记**（前者逐字对齐、后者状态码对齐而消息更具体）；`File_Calculate_Checksum` 正向样例校验和被覆写 |
| C4.4 | 字段大小写逐字符断言：响应中必须出现 `FileID`/`Location`/`SignedURL`/`FileSource`/`SignedUrl`/`Content`/`NumberOfElements`，且**不得**出现 `fileId`/`fileSource`/`results` |
| C4.5 | 三种错误体形态各跑一遍，`code` 与 `message` 一致、只有外层包装不同 |
| C4.6 | 健康检查：`Content-Type: text/plain` 且 body 逐字节等于 `File service is alive` / `File service is ready`；`/v2/info` 免鉴权返回 200 |
| C4.7 | `expiryTime`：缺省 → 有效期末尾 ≈ now+3600s（±5s）；`8D` → ≈ now+604800s；`5X` → `400` + 固定消息 |
| C4.8 | 端到端切片通过：上传字节（含 **≥ 100 MiB** 与 **0 字节**两个边界）→ 元数据 → 下载 → 字节完全一致（SHA-256 比对）→ 删除后 `404` |
| C4.9 | 组合根纪律：`scripts/verify_composition_root.sh` 证明 `new *BlobStore`/`new *Repository` 仅出现在 `src/main/` 与 `tests/` |
| C4.10 | ASan/UBSan 干净；`ctest -L "phase0|phase1|phase2|phase3"` 仍全绿（R5） |
| **C4.11** | **数据面无整体超时**：用刻意放慢的客户端把单次传输拉长到 > 5 分钟仍不被中断；而"空闲无进展"超过 `transfer_idle_timeout_seconds` 时被主动断开的测试 |

**退出条件**：C4.1–C4.10 满足，证据写入 `docs/test-evidence/phase4.md`。
**此时服务已可被 OSDU 客户端在 POSIX 模式下真实使用。**

---

### 阶段 5：对象存储驱动（S3 SigV4）  🚧 进行中

> **当前进度（切片 1~5/5，阶段已完成）**：切片 1 = `sigv4_signer.{h,cpp}`（编码/canonical request/头部签名/
> 预签名/path-style+virtual-host）+ 两条**独立实现对拍**（libcurl `--aws-sigv4`、Python 参照实现）；
> 切片 2 = `tests/tools/mock_s3.py`（**Python 独立验签**的假 S3：CRUD/Range/ListObjectsV2/复制/错误触发）
> + `S3BlobStore::presign_put/get` + `test_s3_presign_verify`（篡改矩阵 7 条 + 反向自证）。
> C5.2、C5.5（编码）已满足；**C5.1 的 AWS 官方文档向量本机无来源 → 如实标注未验证**
> （见 `docs/test-evidence/phase5.md` §5 的补齐方式）。
> 切片 3 = `S3BlobStore` 数据面（libcurl 流式）+ **同一套端口契约跑第三遍**（C5.3）+ 分页 >1000 键（C5.6）
> + S3 错误码映射（C5.7，新增 `kStorageAccessDenied`：存储侧拒绝 ≠ 调用方没权限）。
> 切片 4 = S3 模式端到端（C5.8：`SignedURL` 指向存储端点、服务不代理字节）+ **按配置切驱动**（C5.9：
> `FSS_STORAGE_DRIVER=posix|s3`，同一二进制 + 同一段端到端脚本各跑一遍）。
> 切片 5 = **AWS 官方已知答案向量**（5 条完整请求逐字节匹配 + 2 条已知有意不同）+ virtual-host 数据面
> （`curl --connect-to`，无生产代码钩子）+ `docs/adr/ADR-005-s3-driver.md` 定稿（含与 libcurl/Go SDK 的
> 4 处实测差异与兼容矩阵）。
> **未实测**：真实 S3/MinIO 端到端、分片上传 >5 GiB、退避重试、STS 刷新（ADR-005 §7）。

**目标**：加入**对象存储**路径，用**独立验签**证明 SigV4 实现与真实 S3 兼容。

**交付物**

| 路径 | 内容 |
| --- | --- |
| `src/infra/blob/s3/sigv4_signer.cpp` | AWS SigV4：canonical request、string-to-sign、**原始字节链式派生**、`X-Amz-Signature`、path-style/virtual-host、URL 编码规则（`~`、`/`、`+`） |
| `src/infra/blob/s3/s3_blob_store.cpp` | `presign_put/get`（本地计算，无 SDK）、`put/get`（libcurl 流式）、`stat`（HEAD）、`remove`（DELETE）、`copy`（`x-amz-copy-source`）、`list`（ListObjectsV2 + continuation token）、`ensure_container`（bucket 存在性/创建） |
| `tests/tools/mock_s3.py` | Python 标准库实现的 S3：path-style `PUT/GET/HEAD/DELETE/ListObjectsV2` + **独立 SigV4 验签**（用 Python `hmac`/`hashlib` 重算，**不复用我们自己的签名器**） |
| `tests/integration/test_s3_blob_store.cpp` | 契约测试基类实例化 + SigV4 专项 |
| `tests/integration/test_upload_flow_s3.cpp` | 端到端切片（S3 模式） |
| `docs/adr/ADR-005-s3-driver.md` | S3 驱动设计、编码/签名细节、与 MinIO/Ceph/SeaweedFS 的兼容矩阵 |

**任务清单**

1. `SigV4Signer`：先写**官方向量测试**（P0 已固化派生，本阶段补全完整签名向量），再实现。
2. **独立验签**：mock-S3 用 Python 独立实现验签；我们签、它验 → 这是防止"自签自验"自欺的关键。
3. 查询串构造与百分号编码：`~` 不编码、`/` 编码为 `%2F`（在 query 中）、空格 `%20`（不是 `+`）。
4. `force_path_style` 与 virtual-host 两种模式都测。
5. `list` 的 `continuation-token` 与 `max-keys` 分页；`IsTruncated`。
6. `copy`：`x-amz-copy-source` 的 URL 编码；`<CopyObjectResult>` 解析。
7. 错误映射：`NoSuchKey`→`kNotFound`、`AccessDenied`→`kStorageAccessDenied`→403、`SlowDown`→`kUnavailable`→503。
8. **同一套契约测试跑第三遍**（内存/POSIX/S3），证明三实现行为一致。

**门槛命令**

```bash
cmake --build build -j"$(nproc)" && ctest --test-dir build -L phase5 --output-on-failure
```

**通过判据**

| # | 判据 |
| --- | --- |
| C5.1 | SigV4 已知答案向量（AWS 官方文档 3 个以上完整请求）逐字节匹配 |
| C5.2 | **mock-S3 独立验签**：我们生成的预签名 URL 被 Python 侧验签通过；**篡改任一签名输入（key/expires/header）必须验签失败** |
| C5.3 | `S3BlobStore` 通过**与 POSIX/内存共用的同一套契约测试**，三者结果一致 |
| C5.4 | path-style 与 virtual-host 两种模式下的预签名与数据面都通过 |
| C5.5 | path-style 与 virtual-host 两种模式的 URL 编码边界：key 含 `~`、空格、`+`、`%`、非 ASCII、`/` 全部正确 |
| C5.6 | 分页：写入 > 1000 个键后 `list` 能通过 continuation token 遍历全集（无重复、无遗漏） |
| C5.7 | 错误映射：`NoSuchKey`/`AccessDenied`/`SlowDown` 分别映射到 `kNotFound`/403/`kUnavailable` 并有测试 |
| C5.8 | 端到端（S3 模式）切片通过，且响应 `SignedURL` 指向存储端点（**服务不代理字节**） |
| C5.9 | 切换驱动**只改配置**：同一二进制、同一测试代码，`storage.driver: posix|s3` 两值各跑一遍全绿 |
| C5.10 | 回归：`ctest -L "phase0|...|phase4"` 全绿 |

**退出条件**：C5.1–C5.10 满足，证据写入 `docs/test-evidence/phase5.md`。
**此时"两种存储方式"目标（G2）达成。**

---

### 阶段 6：元数据记录语义完整化  ✅ 已完成（C6.1~C6.13）

> **✅ 已收口（7 个切片）**：退出条件"C6.1–C6.10 满足"已达成，C6.11~C6.13 亦完成。
> 证据：`docs/test-evidence/phase6.md`；门槛：`ctest -L phase6`（9 测试 / 2578 断言）+ `./scripts/run_all_gates.sh`。
> **切片 7 追加**：`src/app/tasks/gc_task.{h,cpp}`（GC 租约：原子领取 + 按 TTL/宽限期 + dry-run 默认）
> 与 `tests/integration/test_gc_lease.cpp`（6 用例 / 131 断言，含 5 条反向/正例对照）。
> **未做（已登记）**：`.tmp_*` 残留清理与 transfer token 清理（P9 C9.25）、GC 调度/领导者选举/指标（P9）、
> PG 版 `ILeaseRepository`、远端 Storage Service 仓储（ADR-004 **可选**）、组合根改接 SQLite 元数据仓储、
> GC 的 TTL 判定改用数据库时钟（P6-D18）。
>
> **进度明细（切片 1~7）**：
> ① `src/infra/metadata/sqlite/sqlite_metadata_repository.{h,cpp}`
> （版本链 + `is_latest` 部分唯一索引 + partition 隔离）+ 元数据契约在 SQLite 上跑第二遍
> （闭合 C2.10 的**元数据侧**）→ C6.5 已满足；C6.1 部分（仓储侧无损，REST 侧见 C4.2）。
> ② **校验和（C6.4）**：增量 `crypto::Hasher`（SHA-256/SHA-1/MD5，已用 RFC/FIPS 公开向量兜底）
> + 算法规范化 + `IsHexDigestOf` 结构校验；第 7 步按上游语义**无条件覆写**客户端传入的两处
> （"原生可用则采用、不可用则**流式回算** SHA-256"）。
> 并证 64 MiB 跨 store 搬迁 + 回算的 RSS 增长 **80 KiB**（同测试内"整块读回"对照 65664 KiB）。
> ⚠️ 本切片推翻了计划里"客户端提供但不符 → `400` + 删除对象"那句臆断（**P6-D05**，见 §5）。
> ③ **12 步序列（C6.3）**：统一的第 12 步回滚（删 persistent + `FAILED` + 审计）供第 6/7/9 步共用；
> 补发第 10 步的第二个事件 `datasetDetails`（`correlationId` 由 `x-correlation-id` 透传）；
> 第 11 步清理失败记审计告警；正常路径的顺序/副作用 + **7 个故障注入点**（计划要求 6 个，追加
> `datasetDetails` 非致命点）。
> ④ **`getFileList`（C6.6）+ 角色（C6.8）**：上游三条验收 fixture 逐字驱动；`Items` 缺省 0、
> `Driver` 小写、无记录消息对齐上游；9 个角色常量集中为唯一真相 + `AuthorizeAny`（任一角色）+
> 端点↔角色映射；顺带修 `ParseIso8601` 的越界时间静默归一化（P6-D10）。
> ⑤ **DMS 6 端点 + Delivery（C6.7）**：上传/下载位置的**两套键集合**（files `fileSource`；
> file-collections `fileCollectionSource` + `fileCount`/`fileNames`）、`retrievalProperties` 补齐
> 4 键、上游 DMS 端到端（上传→登记→取回字节一致）、copy/delivery 形状与状态码。
> ⑥ **大文件（C6.9）+ 幂等并发（C6.11）+ tmp 名（C6.13）**：1 GiB 搬迁 RSS < 64 MiB；
> 两个实例（含 SQLite 两个连接）并发提交同一 FileSource 都"恰 1 条记录、1 份对象"，
> 含"朴素主键 check-then-insert 必现重复"与"朴素临时名必错乱"两条对照；顺带修掉三个
> 内存适配器的线程安全与两处自锁（P6-D14~D16）。
> **剩余**：**GC 租约（C6.12）** + C6.10 回归、远端仓储。

**目标**：把 `File.Generic` 记录语义、版本链、staging→persistent 搬迁与回滚、列表/DMS/Delivery 做完整。

**交付物**

| 路径 | 内容 |
| --- | --- |
| `src/infra/metadata/sqlite/sqlite_metadata_repository.cpp` | `(partition_id, id, version)` 主键、`is_latest` 标记、`previous_version`、ACL/Legal 抽出列 |
| `src/infra/metadata/remote/remote_storage_repository.cpp` | 对接 OSDU Storage Service（`PUT/GET /records`、`POST /records/{id}:delete` 必须 204） |
| `src/app/usecases/create_file_metadata.cpp` | 契约 §2.6 的 **12 步序列**，含补偿回滚与事件发布 |
| `src/app/usecases/delete_file_metadata.cpp` | 契约 §2.8 序列 |
| `src/app/usecases/get_file_list.cpp` | Spring Page 语义 + 时间区间 + `UserID` + `PageNum`(0 起) |
| `src/app/usecases/dms_*.cpp`、`delivery_*.cpp` | 6 个 DMS 端点 + Delivery |
| `src/infra/checksum/` | SHA-256 / MD5 / SHA-1 流式计算（**边写边算**，不二次读盘） |
| `tests/conformance/test_metadata_payloads.cpp` | 契约 §3.3 + §3.4 全部样例 |
| `tests/integration/test_metadata_lifecycle.cpp` | 搬迁/回滚/版本/删除/GC 的故障注入 |

**任务清单**

1. `SqliteMetadataRepository`：版本链 + `is_latest` 唯一性约束；并发写同一 `id` 的串行化。
2. 校验和：流式计算，**在复制过程中同时计算**以避免二次读盘；与客户端传入值比对（若提供）。
3. 12 步序列：**每一步的失败路径都要有测试**（故障注入：复制失败、校验和失败、写记录失败、删 staging 失败）。
4. 版本语义：`GET metadata` 返回最新；重复 `POST` 同 `FileSource` 的行为（上游会新建记录）。
5. `getFileList`：Spring Page 字段名 + `PageNum` 0 起 + 无记录 → 400（对齐上游）。
6. DMS 6 个端点 + Delivery；`providerKey` 可配。
7. DMS `storageInstructions` 的 `storageLocation` 键集合对齐上游（`signedUrl`/`fileSource`/`createdBy`/`expiryTime`）。
8. GC 任务（默认 `dry_run=true`）：staging 超期、persistent 孤儿。

**门槛命令**

```bash
cmake --build build -j"$(nproc)" && ctest --test-dir build -L phase6 --output-on-failure
```

**通过判据**

| # | 判据 |
| --- | --- |
| C6.1 | 契约 §3.3 黄金样例经 REST 与（内存）仓储往返后**字段级完全一致**（含 `meta` 数组、`tags` 字典、未知字段） |
| C6.2 | 契约 §3.4 全部负向样例通过；三种 kind 消息逐字节匹配 |
| C6.3 | 12 步序列：正常路径 + **6 个故障注入点**各有测试。特别是：第 9 步失败 → persistent 对象被**回滚删除**；第 11 步失败 → 响应**仍为 201**（不是 500） |
| C6.4 | 校验和：客户端未提供 → 服务端计算并写入；~~提供但**不符** → `400` + 对象被删除~~（**已推翻：与上游冲突，见 `00-final-design.md` §5 与 P6-D05** → 客户端提供的一律**被覆写**，不比对、不回滚）；算法覆盖 SHA-256 / MD5 / SHA-1（**跟随驱动**；驱动原生值不可用时流式回算 SHA-256） |
| C6.5 | 版本链：同 `id` 两次写入 → `version` 1、2，`GET` 返回 2，历史可查 |
| C6.6 | `getFileList`：分页字段名精确；`PageNum=0` 与 `PageNum=1` 结果不重叠；时间区间过滤正确；无记录 → `400` |
| C6.7 | DMS 6 端点 + Delivery：状态码、角色、响应键集合全部断言通过 |
| C6.8 | 角色常量逐字节断言：`service.file.viewers/editors/admin`、`service.dataset.viewers/editors`、`service.storage.viewer/creator/admin`、`service.delivery.viewer` |
| C6.9 | 大文件搬迁：≥ 1 GiB staging→persistent 复制的 RSS 峰值增长 < 64 MiB |
| **C6.11** | **幂等性（复现 M2）**：2 实例并发提交同一 `fileSource` → 只产生 1 条记录、只发生 1 次复制。对照测试：去掉唯一约束时**必须**出现重复（自证） |
| **C6.12** | **GC 租约（复现 M3）**：在途对象在租约有效期内不被删；租约过期且无记录时被回收；两个 GC 并发时靠原子领取不重复删 —— ✅ 切片 7（`test_gc_lease` 6 用例 / 131 断言，含"无租约不许删"与"有记录永不删"两条反向测试） |
| **C6.13** | **tmp 名唯一性（复现 M1）**：2 实例并发写同一业务序号 → 无内容错乱。对照测试：用不含实例标识的 tmp 名时**必须**能复现错乱（自证） |
| C6.10 | 回归：P0–P5 全绿 |

**退出条件**：C6.1–C6.10 满足，证据写入 `docs/test-evidence/phase6.md`。→ ✅ **已满足（C6.11~C6.13 亦完成）**。

---

### 阶段 7：gRPC 适配层 + 双协议等价性  ✅ 已完成

> **收口（切片 3/3）**：`ctest -L phase7` **6 测试 / 5378 断言**；
> **C7.1~C7.10 全部满足**（C7.9 = 回归：`run_all_gates.sh` 全绿）。
> 切片 1：① 契约 §5 的**唯一权威表**下沉到 L3（`domain/contract/error_table.*`）→ HTTP/gRPC 两个适配器
> 从同一张表派生；`test_error_equivalence`（C7.2）用手抄的契约期望值同时钉住实现表与两个适配器。
> ② `app::CallerFromHeaders` 抽出"REST 头 / gRPC metadata → `CallerContext`"的共用规则。
> ③ C7.7 护栏：proto 头只能出现在 `adapters/grpc/`（含 4 条自证用例）。
> 切片 2：④ **14 个一元 RPC 全部在真实端口上实现**；⑤ `app::wire_shapes` 把"两条协议共用的线上形状"
> 抽到 L4，REST 的 `dto.cpp` 与 gRPC 适配器**都**从这里取，消除"两处各写一份 JSON"的漂移面；
> ⑥ `test_protocol_equivalence`（C7.3 矩阵 12 行 + C7.4 自签 URL 的 token 解码比对，含 4 条反向测试）
> 与 `test_proto_json_mapping`（C7.5 黄金样例逐字段互操作）。
> 切片 3：⑦ 3 个扩展 RPC（`UploadFile` 客户端流 / `DownloadFile` 服务端流 / `ServerSideCopy`）
> 的字节通道（`grpc_streaming_io.cpp`）+ L4 用例（`UploadFile`/`DownloadFile`/`ServerSideCopy`）；
> ⑧ **组合根同时开两个端口**（`FSS_GRPC_PORT`），`test_dual_protocol_concurrency` 在**真实二进制**
> 上用并发混合协议读写同一份状态（C7.8）；⑨ `test_grpc_streaming`：1 GiB/0 字节/中途取消/慢客户端
> （C7.6）+ 与 HTTP `Range` 的字节一致与"不整文件读"（C7.10）。
> **未做（已登记，不影响判据）**：真实 S3/MinIO 上的流式 RPC 端到端、多租户并发下的流控调优。

**目标**：交付 RPC 面，并**机械地证明**它与 REST 面语义等价（ADR-001 的核心约束）。

**交付物**

| 路径 | 内容 |
| --- | --- |
| `src/adapters/grpc/file_service_adapter.cpp` | 全部 14 个一元 RPC + 3 个流式/代理 RPC（契约 §4.1，共 17 个） |
| `src/adapters/grpc/grpc_error_mapper.cpp` | 契约 §5 映射表实现；`details` 携带期望/实际等附加信息 |
| `src/adapters/grpc/grpc_dto.cpp` | proto ↔ 领域模型双向转换（`json_name` 对齐 OSDU JSON） |
| `src/adapters/grpc/grpc_streaming_io.cpp` | `UploadFile`/`DownloadFile`：gRPC 流 ↔ `ByteSource`/`ByteSink`，**与 HTTP 数据面共用 `IBlobStore` 驱动** |
| `tests/conformance/test_error_equivalence.cpp` | 契约 §5 每个 `ErrorKind` 双协议一致 |
| `tests/conformance/test_protocol_equivalence.cpp` | 契约 §6 等价性矩阵 + `SignedUrlEquivalent`（含反向测试） |
| `tests/integration/test_grpc_streaming.cpp` | 流式上传/下载（含 1 GiB、0 字节、中途取消、慢客户端） |
| `tests/unit/test_proto_json_mapping.cpp` | proto 的 `json_name` 与 OSDU JSON 逐字段对齐（含 PascalCase 断言） |

**任务清单**

1. 转换层：proto ↔ 领域模型；**只允许在 `src/adapters/grpc/` 内出现 protobuf 类型**（护栏）。
2. `GrpcErrorMapper`：与 REST 的 `HttpErrorMapper` **共用同一张表**（同一份 C++ 数据结构，避免两处维护）。
3. 鉴权元数据：`authorization` / `data-partition-id` / `correlation-id` → 与 REST 复用同一 `AuthMiddleware` 逻辑。
4. 流式：`UploadFile` 首片必须是 `info`；后续 `chunk`；校验和边收边算；`register_metadata=true` 时调用同一 `CreateFileMetadata` 用例。
5. `DownloadFile`：支持 `offset`/`length`，与 HTTP `Range` 语义一致。
6. 等价性测试：用 `tests/framework/` 的同一份输入驱动两条链路，比较领域结果（不是比较 JSON 字符串）。

**门槛命令**

```bash
cmake --build build -j"$(nproc)" && ctest --test-dir build -L phase7 --output-on-failure
```

**通过判据**

| # | 判据 |
| --- | --- |
| C7.1 | 契约 §4.1 全部 **17 个 RPC**（14 个一元 + 3 个流式/代理）可调用并返回正确结果（对真实 gRPC 端口） |
| C7.2 | 契约 §5 映射表：**每个 `ErrorKind`** 的 REST `(status, reason)` 与 gRPC `StatusCode` 落在同一行；无空缺 |
| C7.3 | 契约 §6 等价性矩阵：**12 个操作**逐行通过（领域结果相等 + 错误分类相等 + 副作用相等） |
| C7.4 | `SignedUrlEquivalent`：REST 与 RPC 生成的 URL 结构等价（scheme/host/path/query 键集合/过期 ±5s）；**反向测试**（改一个 query 名 → 不等）通过 |
| C7.5 | proto `json_name` 对齐：`data` 内字段为 PascalCase、信封为 camelCase；用 proto3-JSON 序列化后能与契约 §3.3 的 JSON **逐字段互操作** |
| C7.6 | 流式：1 GiB 上传/下载成功、CRC/SHA 一致、RSS 峰值 < 64 MiB；0 字节成功；**中途取消**不遗留临时文件/不泄漏句柄 |
| C7.7 | 护栏：`src/domain/`、`src/app/`、`src/adapters/http/` 中不出现 `osdu/file/v1` proto 头（编译期目标图 + 源码检索双验） |
| C7.8 | gRPC 与 REST **同时**运行、互不干扰（两个端口的并发测试） |
| C7.9 | 回归：P0–P6 全绿 |
| **C7.10** | gRPC 流式下载与 HTTP `Range` 在**相同 offset/length** 下返回的字节完全一致（SHA-256 比对），且两者都不触发整文件读取 |

**退出条件**：C7.1–C7.9 满足（另加 C7.10），证据写入 `docs/test-evidence/phase7.md`。
**此时"双协议"目标（G3）达成。** ✅ **已达成**（2026-09-17，切片 3/3 收口）：
- C7.1 17/17 RPC 在真实端口上可调用；C7.2 契约 §5 逐行；C7.3 矩阵 12 行 + 错误分类；
- C7.4 `SignedUrlEquivalent`（含 4 条反向测试）；C7.5 `json_name` 逐字段互操作；
- C7.6 1 GiB/0 字节/中途取消/慢客户端（真实 POSIX，RSS 上限 + 整块读回对照）；
- C7.7 proto 隔离护栏；C7.8 **真实二进制**两个端口并发混合协议；C7.9 `run_all_gates.sh` 全绿；
- C7.10 gRPC 区间读 == HTTP `Range`（端口级记账证明没有整文件读）。

---

### 阶段 8：认证授权与多租户  ✅ 已完成

> **收口（切片 3/3）**：`ctest -L phase8` **6 测试 / 1323 断言**；**C8.1~C8.8 全部满足**
> （退出条件达成），C8.9/C8.10 的**配置/机制**部分完成、端到端形态依赖 PG（P9）。
> ⑧ **C8.7 审计覆盖**（`test_audit_coverage`，2 用例 / 355 断言）：15 个受保护用例**逐个**在
> 成功侧与失败侧（授权失败 + 领域失败）都留下审计，字段含 actor / partition / object_id /
> result / epoch_millis / **correlation_id**；实现上统一为 RAII 守卫（`AuditGuard`），
> "任何提前 return 都会记账"（手写调用漏掉失败出口是**静默**的）。
> ⑨ **C8.6 事件**：由 P6 的既有用例机械覆盖（`IN_PROGRESS→SUCCESS` 各恰好一次 + 
> `datasetDetails` 含 record id/version/correlationId/timestamp；失败路径 `IN_PROGRESS→FAILED`），
> 本次把操作名表写进契约 §4.6 并逐条对照。
> ⑩ **C8.9 multi 5 条启动校验**：仓储必须 PG / 租约与选举开启 / GC 必须要求租约到期 /
> 存储根共享挂载 / 时钟偏差容忍范围（0 < 值 ≤ 60）—— 五条各有一条"拒绝"测试 + 一条
> "全满足必须通过"的正例；组合根对 `FSS_DEPLOYMENT_MODE=multi` **拒绝启动**（PG 运行形态属 P9）。
> ⑪ **C8.10 时钟偏移**：`app::ClockSkewGuard`（参考时钟是 TTL/过期判定的**唯一**时间源；
> 快钟/慢钟超容忍 → fail-closed）+ JWT `exp` 的 ±skew 边界用例。
>
> **切片 2（已收口）**：跨租户隔离与远端 Entitlements。
> ⑤ **C8.2 跨租户隔离**（`test_tenant_isolation`，2 用例 / 107 断言）：两个租户 + 真实 HTTP
> + 真实 JWT；A 的 token 配 B 的头 → **403**（鉴权层拦，不去 B 的分区查）；B 读/删 A 的记录
> → **404** 且响应里没有 A 的任何数据；B 的列表看不到 A 的记录（A 自己能列到 → 正例）；
> 签名 URL 三道：跨租户头 → 403、篡改 token/sig → 401、改路径形状 → 404（如实区分）。
> ⑥ **C8.4 远端 Entitlements**（`src/infra/auth/remote/`，L2；`test_remote_entitlements_authorizer`
> 3 用例 / 62 断言）：`authorizeAny` 语义；超时 / 5xx / 坏 JSON / 缺 `allowed` / 连不上 /
> 未配地址 → **一律 503（fail-closed）**，`fail_closed=false` 被配置校验拒绝；故障形态由
> `tests/tools/mock_entitlements.py` **独立进程**注入（含超时）。
> ⑦ 组合根：`FSS_AUTH_MODE=remote-entitlements` 现在走真实实现（未配地址则拒绝启动）。
> `ctest -L phase8` **4 测试 / 954 断言**。
>
> **切片 1（已收口）**：本地 JWT 授权器落地（ADR-012）+ 路由级鉴权预检 + 配置强校验。
> ① `src/infra/auth/local/local_jwt_authorizer.{h,cpp}`（L2）：HS256 验签 + `exp`/`nbf`/`iss`/`aud`
> + 角色 claim ∪ 静态角色表 + **租户绑定**（token 的 `data-partition-id` claim 必须等于请求头）；
> 一律 fail-closed（密钥缺失/算法非 HS256/无 `exp`/畸形 → 401）。`ctest -L phase8` **单元 6 用例 / 68 断言**。
> ② HTTP 适配层的**路由级预检**（`RouteAuthTable()`）：鉴权**先于** DTO 解析（上游"过滤器先于
> controller"），未登记路由 fail-closed；`test_auth_matrix` **3 用例 / 717 断言** 覆盖
> 16 个端点 × 8 个单角色 token 的 401/403/放行矩阵 + 正向垂直切片。
> ③ 配置：`deployment.environment` + `auth.jwt.{hmac_secret,partition_claim,require_partition_claim}`，
> production 禁止 `disabled`/关验签/无密钥，`jwks_url` 非空直接拒绝（未实现不许"看起来配了"）。
> ④ `/v2/info` 的 `authMode`（REST + gRPC 同源）。
> **剩余**：切片 3 = 事件（C8.6）/审计（C8.7）+ `deployment.mode=multi` 的 5 条启动校验
> （C8.9）+ 时钟偏移（C8.10）。

**目标**：从"allow-all"切换到真实鉴权，并证明租户隔离不可绕过。

**交付物**

| 路径 | 内容 |
| --- | --- |
| `src/infra/auth/local/local_jwt_authorizer.cpp` | JWT 解析（`exp`/`iss`/`aud`/签名可选校验）、角色 claim 提取、静态角色表 |
| `src/infra/auth/remote/remote_entitlements_authorizer.cpp` | 调用 Entitlements `authorizeAny` 语义（HTTP + libcurl + 超时/重试） |
| `src/infra/partition/file_partition_registry.cpp` | 分区注册表（容器名、驱动覆盖、配额） |
| `src/infra/legal/`、`src/infra/events/`、`src/infra/audit/` | 可选校验器 + 事件发布（`status`/`datasetDetails`）+ 审计日志 |
| `tests/integration/test_auth_matrix.cpp` | 每个端点 × 每种角色的 401/403/200 矩阵 |
| `tests/integration/test_tenant_isolation.cpp` | 跨租户读写/列表/签名 URL 全部被拒 |
| `docs/operations.md` | 配置、部署、排障、密钥轮换 |

**任务清单**

1. `CallerContext` 作为所有用例第一个参数（编译期强制"不会忘记鉴权"）。
2. 角色矩阵：按契约 §2 每个端点的角色实现。
3. 401 与 403 的区分：缺 token/partition → 401；有 token 但角色不足 → 403；消息用上游固定文案。
4. Entitlements 远端调用：超时、重试、降级策略（**默认 fail-closed**）。
5. 租户隔离：仓储与签名 URL 双重校验（token 绑定 `partition`）。
6. 事件发布：`status`(`DATASET_SYNC`/`IN_PROGRESS|SUCCESS|FAILED`) 与 `datasetDetails`，对齐上游语义。
7. 审计：对齐上游 `AuditOperation` 语义（`createLocationSuccess`、`readFileLocationSuccess` 等）。
8. `auth.mode=disabled` 仅在非 production 允许，启动打印显著告警，且 `/v2/info` 中标记。

**门槛命令**

```bash
cmake --build build -j"$(nproc)" && ctest --test-dir build -L phase8 --output-on-failure
```

**通过判据**

| # | 判据 |
| --- | --- |
| C8.1 | 端点 × 角色矩阵：19 个 OSDU 端点 × 必要角色，正向全通过、负向全 403；缺 token/partition → 401（消息逐字节） |
| C8.2 | 跨租户：A 租户 token 读取/删除/列表 B 租户资源 → 全部 404/403（**不得**返回 B 的数据）；A 的签名 URL 不能访问 B 的对象 |
| C8.3 | JWT 边界：过期 token、`nbf` 未到、错 `aud`、错 `iss`、签名错误（若启用）、畸形 token → 全部 401 |
| C8.4 | Entitlements 远端不可达 → **fail-closed**（403/503），**绝不**降级为 allow；有测试注入超时 |
| C8.5 | `auth.mode=disabled` 时启动日志包含显著告警，且 `/v2/info` 含标记；production 配置文件不允许该值（配置校验测试） |
| C8.6 | 事件：`CreateFileMetadata` 触发 `IN_PROGRESS`→`SUCCESS`；失败路径触发 `FAILED`；事件内容含 record id 与 version |
| C8.7 | 审计：每个受保护端点在成功与失败两侧都产生审计记录（含 actor、对象、结果、时间、correlation-id） |
| C8.8 | 回归：P0–P7 全绿（注意 P4 的 allow-all 测试需改为显式注入 `AllowAllAuthorizer`，不得依赖配置默认值） |
| **C8.9** | `deployment.mode=multi` 的 **5 条强制启动校验**各有一个"拒绝启动"的测试：仓储必须为 PG、租约与选举必须开启、GC 必须要求租约到期、存储根必须在共享挂载上、时钟偏移在容忍范围内 |
| **C8.10** | 时钟偏移：模拟快钟/慢钟实例 → 租约与 token 过期**不误判**（租约判定一律用 PG 的 `now()`） |

**退出条件**：C8.1–C8.8 满足，证据写入 `docs/test-evidence/phase8.md`。✅ **已达成**
（2026-09；C8.9/C8.10 为表中**加粗**的追加判据：配置级与机制级已完成并测试，
端到端形态依赖 PG 版仓储/租约/数据库时钟 —— 属 P9，见其"未做"清单）。

---

### 阶段 9：硬化与交付  ✅ 已完成

> **当前进度（切片 1/2/3 全部完成）**：`ctest -L phase9` **6 测试 / 466 断言**；
> `run_all_gates.sh` **P0~P9 全绿（225 s / 10 阶段）**；`ctest` 75/75。
> C9.1~C9.13、C9.15、C9.16、C9.25、**C9.31**（P9 补交）满足（C9.14/C9.17~C9.22/C9.24/C9.26~C9.30 为环境不具备的
> 判据，已如实登记为未验证，见证据 §11）。
> ① **C9.1 并发**：100 并发 JSON（真实端口 + 真实线程）**零 5xx**；8 并发大文件（真实 POSIX）
> **逐路 SHA-256 + 存储侧 `stat().checksum` 三方一致**（载荷带路标识，串数据必然被抓）。
> ② **C9.2 故障注入 + 恢复**：存储不可用（503）/ 元数据写入失败（500）/ **磁盘满含回滚**（502，
> 按路径与契约 §5 对齐）/ 远端鉴权依赖不可用（503）+ 事件与审计两类非致命依赖；
> 每类都在解除后轮询探测并断言**恢复 ≤ 30 秒**。远端依赖用 `mock_entitlements --fail-file`
> 做"注入/恢复"开关（真实进程，不是替身翻标志位）。
> ③ **C9.3 六类资源上限**：头/URI/体（两档）/连接数/传输内存预算，各一条"被拒绝" + 一条对照。
> ④ 顺带修掉 **P9-D01**：测试替身不是线程安全的，100 并发直接把仓库的**堆写坏**（见证据 §4）。
> ⑤ **C9.6 指标**：`/metrics` 暴露 HTTP + 存储（操作/字节）+ GC 全部指标，逐行格式校验、
> 值真的动过、**不含任何 secret**；⑥ **C9.5/C9.25 GC 清理**：驱动新增"临时文件清理"专用入口
> （`list()` 看不见 `.tmp_*`——它们不是对象），够旧的删、**太新（在途）的保护**，且**绝不**因为
> "某条位置记录恰好指向它"而放过；⑦ 顺带修掉 **6 个"看起来通过"型缺陷**（P9-D04~D09：
> 时间戳时基/格式（POSIX 与 S3 各一处）、临时文件保护计数翻倍、数据面错误码被"读体失败 400"覆盖、
> 流式 handler 不读体却回 2xx、能力护栏把"装饰器纯转发"判成越权）。
> ⑧ **C9.4/C9.11/C9.13/C9.15 容量基线**：独立进程 + 互不重叠绑核、每点位 **3 次取中位数**，
> 产品进程上实测四类点位（小段读请求率上限、大文件单流吞吐、4→64 并发扩展曲线、SQLite 写吞吐）
> + **p99 阈值** + **回归 >20% 判失败**；文件落 `docs/appendix/capacity-baseline/BASELINE.tsv`，
> 方法学与"哪些点位在本机无判定条件"（fsync 类散布 >20%；**跨会话机器漂移实测 ≈40%**）
> 写在 `scripts/bench_baseline.sh` 与证据 §9；
> ⑨ **C9.12/C9.16 ADR-006 受控复核**：同一负载生成器对比 httplib 内容提供者 vs 裸 `sendfile`，
> **几何平均 2.12x / 2.02x（两次独立运行）≥ 1.5x → 采纳 sendfile 方向**（实现未交付，
> 边界条件见 ADR-006 §4）。
> ⑩ **C9.7 门槛纳入 P9**：`IMPLEMENTED_PHASES` 含 9，顺序全绿并记录总耗时（**225 s / 10 阶段**）；
> ⑪ **C9.8 容器镜像**：多阶段 `Dockerfile` + `scripts/verify_image.sh`，**实测** readiness 200、
> 强制 jwt（无/伪 token 均 401、`disabled` 拒绝启动）、非 root uid 10001、HEALTHCHECK healthy；
> ⑫ **C9.9 运维文档**：`docs/operations.md`（156 个配置键逐条 + 未接通清单）+ `docs/runbook.md`
> （症状→诊断→处置）+ **自动比对测试** `test_operations_doc`；⑬ **C9.10 风险收口**：
> `docs/02-design.md` §16.1 覆盖全部 **20 行**风险（含初版"11 项"的登记纠正）；
> ⑭ 顺带修掉 **P9-D10**：组合根未装配指标注册表/计量装饰器、日志不打码
> （"测试里通过、产品里不存在"的第三次应验）—— 现在**真实进程**的 `/metrics` 有存储族、
> 启动横幅显示 `log redact : 11 个键` 与 `metrics : 已接入（含存储计量）`。
> **P9 遗留（登记，不阻塞 C9.1~C9.10 的通过）**：组合根未接 `config/fss.example.json`
> （156 键中 125 个未接通，逐键见 `operations.md` §1.3）、PG 仓储/租约与 `mode=multi` 运行形态、
> sendfile 数据面实现、GC 调度与端点、真实硬件/多进程/容器类判据。

**目标**：从"功能正确"提升到"生产可用"，并交付完整运维资料。

**交付物**

| 路径 | 内容 |
| --- | --- |
| `tests/hardening/test_concurrency.cpp` | 并发/竞争/连接风暴 |
| `tests/hardening/test_fault_injection.cpp` | 存储/DB/网络故障注入下的行为与恢复 |
| `tests/hardening/test_resource_limits.cpp` | 上限拒绝行为（头/URI/体/连接数/配额） |
| `tests/hardening/test_perf_baseline.cpp` | 基准：JSON 端点 p99、数据面吞吐 |
| `src/common/metrics/` + `/metrics` | 完整指标集 |
| `src/app/tasks/gc_task.cpp` | GC（staging 超期、孤儿对象）+ 指标 |
| `Dockerfile`、`deploy/` | 容器镜像 + 部署模板（含 production 强制 `auth.mode=jwt`） |
| `scripts/run_all_gates.sh` | 顺序执行 P0–P9 全部门槛 |
| `docs/operations.md`、`docs/runbook.md` | 运维手册、故障处置手册 |
| `README.md` | 构建/运行/测试/配置入门 |

**任务清单**

1. 并发：HTTP 100 并发 × JSON 端点；数据面 8 并发大文件；SQLite 写并发。
2. 故障注入：存储不可用、DB 只读/锁、磁盘满、远端超时；验证错误映射正确 + 恢复后自愈。
3. 资源上限：全部上限都有"被拒绝"的测试（不是"能接受"）。
4. 指标：请求/延迟/存储操作/字节/GC/自签校验失败。
5. GC：dry-run 与真实删除都测；误删防护（有元数据记录的 persistent 对象**永不**删除）。
6. 性能基线：记录 p50/p95/p99 与吞吐，作为回归基线（超过 20% 退化即失败）。
7. 打包：静态/动态链接、依赖清单、SBOM、镜像大小。
8. 文档：`operations.md`（配置全表 + 密钥轮换 + 容量规划）、`runbook.md`（症状→诊断→处置）。

**门槛命令**

```bash
cmake --build build -j"$(nproc)" && ctest --test-dir build -L phase9 --output-on-failure && scripts/run_all_gates.sh
```

**通过判据**

| # | 判据 |
| --- | --- |
| C9.1 | 并发测试：100 并发 JSON 请求零 5xx（除注入的故障）、数据面 8 并发大文件校验和全部一致 |
| C9.2 | 故障注入：每类故障的行为符合契约 §5；解除故障后服务在 ≤ 30 秒内恢复（有测试） |
| C9.3 | 资源上限：头/URI/体/连接数/配额 6 类上限各有一个"被拒绝"的测试 |
| C9.4 | 性能基线：p99 在设定阈值内，且相对基线退化 < 20% |
| C9.5 | GC：`dry_run=true` 不删除任何对象；`dry_run=false` 只删除"无元数据记录"的对象；**有记录的对象永不被删**（有反向测试） |
| C9.6 | `/metrics` 暴露全部指标，格式合法，且**不含**任何 secret |
| C9.7 | `scripts/run_all_gates.sh` 顺序跑 P0→P9 **全绿**，总耗时记录在案 |
| C9.8 | 容器镜像可构建、可启动、`readiness_check` 通过；镜像包含 production 配置（`auth.mode=jwt` 强制） |
| C9.9 | 文档完整：`operations.md` 覆盖全部配置项（与 `config/fss.example.json` 一一对应，有自动比对测试） |
| C9.10 | 全部风险（`docs/02-design.md` §16；**实际 20 行** R-01~R-08/R-12~R-16/R-23~R-29 —— 初版"11 项"是过时数字，已在 §16.1 登记纠正）都有明确的缓解措施落地证据或显式接受记录 |
| **C9.11** | **容量基线（方法学强制）**：负载生成器为**独立进程并绑核**，测出并记录：小段读请求率上限、大文件单流吞吐、4→64 并发扩展曲线、SQLite 写吞吐。相对基线退化 > 20% 即失败。**禁止**用进程内库客户端压测（曾因此得出方向性错误结论，见 `docs/appendix/capacity-probe/README.md`） |
| **C9.12** | **ADR-006 受控复核**：用**同一负载生成器**对比 `sendfile` 数据面与 httplib 内容提供者，得出可复现的倍数；≥ 1.5x 则定稿 ADR-006，< 1.5x 则放弃并统一用 httplib（避免无谓的重复实现） |
| **C9.13** | 内存预算断言：`并发上限 × 缓冲 ≤ budget`；超限**拒绝启动** |
| **C9.14** | 在**真实存储（非稀疏文件）与真实网卡**上复核容量数字，并据此更新 `docs/05-capacity-and-concurrency.md`（该文档的绝对数字目前仅是量级参考） |
| **C9.15** | 小文件**上传**端到端吞吐（含 POSIX 驱动 + token + DB 全链路）实测并记录；验证 `fsync_policy` 各档位的吞吐差异 |
| **C9.16** | ADR-006 定稿（或明确记录为"放弃"及理由） |
| **C9.17** | **异步化基线**：测量并记录"线程模型 vs 协程模型"的关键指标（吞吐 / RSS / 可达并发），作为 ADR-007 触发条件 T1–T3 的基线；在**真实存储**（非定时器、非稀疏文件）上复核 ADR-007 §2.3 的结论 |
| **C9.18** | **io_uring 可用性验证**：在**目标部署内核**上验证（本机为 WSL2，**不得作为判定依据**）；输出"可用/不可用"两种情形下的数据面异步方案 |
| **C9.19** | ADR-007 触发条件评估：明确记录 T1–T3 是否达成；若达成则启动数据面异步化（第 2 步），否则记录为"未触发"及复核时间点 |
| **C9.20** | **组提交耐久性语义**：实现 ADR-008 的**两阶段批提交**（`write all tmp → syncfs → rename all → fsync(dir)`）；在 `/v2/info` 与运维文档中显式声明耐久性粒度（批级/单文件级）与批大小 |
| **C9.21** | **`fsync` 摊销收益的端到端验证**：同一负载下对比 `durability` 各档位。**实测参考（ADR-008，安全协议）**：每文件 `fdatasync`+每文件 `fsync(dir)` = 383 文件/s；每文件 `fdatasync`+每批 `fsync(dir)` = 784；**两阶段批提交 = 31,478**；不安全方案 35,389（**禁止使用**） |
| **C9.23** | **写入顺序不变量回归测试（自证）**：把 `syncfs` 故意移到 `rename` 之后 → 顺序检查**必须失败**；恢复 → 必须通过。依据：ADR-008 §4.2 的 R1 不变量 |
| **C9.24** | **`syncfs` 全局 flush 的影响评估**：多租户共盘场景下测量它对其他写入的干扰；必要时默认改为 `per_file` 或要求按 partition 分盘 |
| **C9.25** | **GC 对残留 `.tmp_*` 的清理**：必须能识别并删除；且有"绝不把 `.tmp_*` 视为有效对象"的反向测试 |
| **C9.26** | **多实例端到端**：2 个真实进程 + 共享 PG + 共享目录，跑完整 上传→登记→下载→删除 流程，并注入实例崩溃（验证 `claiming` 记录的租约回收） |
| **C9.27** | **★ 在目标存储上验证 NFS 语义**（上生产硬前提）：`rename` 跨客户端原子性、close-to-open 一致性、`fsync`/`syncfs` 耐久性；并确认实现**不依赖** NFS 文件锁 |
| **C9.28** | PG 连接预算校验：实例数 × 池上限 ≤ `max_connections`；超限**拒绝启动** |
| **C9.29** | **io_uring 收益复核（U2/U4）**：在**目标存储**（NVMe/HDD/NFS）上复测 io_uring vs 阻塞线程池。HDD/NFS（高延迟）场景若差异 ≥1.5x 则建议启用；NVMe 级别不足则维持默认 |
| **C9.30** | `/v2/info` 与指标正确暴露 `ioEngine` / `ioUringAvailable`；在**不允许 io_uring 的部署**里所有 OSDU 端点行为不变 |
| **C9.22** | **真实存储上重测 I/O 延迟分布**（NVMe/HDD/NFS，非 WSL2 虚拟盘），替换 `docs/appendix/posix-io-probe/RESULTS.txt` 的量级参考；HDD/NFS 延迟高 1–2 个数量级，并发需求完全不同 |
| **C9.31** | **（P9 补交，P10 期间完成）GC 的 HTTP 按需端点**：`POST {base_path}/v2/gc:run`，授权 **`service.file.admin`** 且**不需要** `data-partition-id`（401/403/200）；响应 = `GcReport` 的字段（snake_case）+ 运行态 `partition` / `scheduled`；**有效 dry-run = 配置 `gc.dry_run` ‖ 请求 `?dryRun=true`**（请求只能更保守，**没有**"强制真删"的参数）；`GcTask::Run` 自带**单飞护栏**（已在跑 → `kUnavailable` → **503**，**不排队、不并行**；是 `GcTask` 的**通用**性质，周期调度与端点共享）；`gc.enabled=false` 时端点**仍可用**且报告 `scheduled=false`；会删数据的动作写**审计**（`operation=gcRun`，成功/失败两侧）；`/metrics` 的 `fss_gc_runs_total` 真的涨；**不新增任何配置键**（三态保持 106/19/31）。R1 自证：①去掉单飞护栏、②忽略请求的 `?dryRun=true`、③让端点免鉴权 —— 三种错误实现都必须让对应用例**失败** |

**退出条件**：C9.1–C9.10 满足（含 P9 补交的 **C9.31**），证据写入 `docs/test-evidence/phase9.md`。

---

## 4. 工作量估算（相对量级，非承诺）

| 阶段 | 主要内容 | 规模 | 依赖 |
| --- | --- | --- | --- |
| P0 | 环境/契约验证 | 小 | — |
| P1 | 分层骨架 + 通用库 + HTTP 内核 | **大** | P0 |
| P2 | 领域 + 端口 + 应用逻辑 | 中 | P1 |
| P3 | POSIX 驱动 + 位置仓储 + 数据面 | 中 | P2 |
| P4 | REST 适配 + 端到端切片 | 中 | P3 |
| P5 | S3 驱动 + SigV4 + mock-S3 | 中 | P4 |
| P6 | 元数据语义完整化 | **大** | P5 |
| P7 | gRPC + 等价性 | 中 | P6 |
| P8 | 认证授权/多租户 | 中 | P4（可并行） |
| P9 | 硬化与交付 | 中 | P7、P8 |

**可并行点**：P5（S3 驱动）与 P8（鉴权）互相独立；
P1 内的 `crypto`/`ids`/`time`/`fs`/`config`/`logging` 可并行开发，但**HTTP 内核与分层护栏必须先完成**才能真正开始 P2。

---

## 5. 环境与依赖准备

**已确认的环境事实（P0 实测）**

| 项 | 值 |
| --- | --- |
| OS / 编译器 | Ubuntu 22.04.5 / g++ 11.4.0（clang++ 未安装） |
| 构建 | CMake 3.22.1 + GNU Make 4.3（ninja 未安装） |
| 权限 | **无 root**（`sudo` 需密码）→ 依赖一律 `apt-get download` + `dpkg-deb -x` 解包 |
| 网络 | `community.opengroup.org` 可达；`github.com` 直连**不可达**；apt 走 USTC 镜像可用 |
| gRPC / protobuf | `grpc++` 1.30.2、`protobuf` 3.12.4、`protoc` 3.12.4、`grpc_cpp_plugin` ✅ |
| OpenSSL | 3.0.2（`libcrypto`，用于 HMAC/SHA）✅ |
| SQLite3 | 3.37.2 ✅ |
| libcurl | 7.81（头文件在 `/usr/include/x86_64-linux-gnu/curl`）✅ |
| 其他 | zlib、`uuid/uuid.h` ✅；**无** boost / aws-sdk-cpp |
| vendored 源码依赖 | `third_party/`：nlohmann/json 3.10.5、Catch2 2.13.8、**cpp-httplib 0.26.0**（MIT，单头文件，取自 Ubuntu 镜像 pool 的上游源码；**不要**用 Ubuntu 的二进制包，见 ADR-002） |
| docker | 27.3.1，daemon 可用（镜像拉取能力 **UNVERIFIED**） |

**依赖补装命令（无需 root，可重复执行）**

```bash
# 一次性补装/更新 vendored 依赖
apt-get download nlohmann-json3-dev catch2 && \
  for f in *.deb; do dpkg-deb -x "$f" ext/; done && \
  cp ext/usr/include/nlohmann/json.hpp third_party/nlohmann/ 2>/dev/null || \
  cp -r ext/usr/include/nlohmann third_party/ ; \
  cp ext/usr/include/catch2/catch.hpp third_party/catch2/
```

**新增的硬约束（来自容量实测，见 `docs/05-capacity-and-concurrency.md`）**

1. **`TCP_NODELAY` 必须开启**（服务端与客户端），否则小请求有 40 ms 停顿；
2. **`worker_threads` 语义是"并发连接上限"**，不能按 CPU 核数配；
3. **数据面与控制面线程池必须分离**，否则大文件传输会饿死元数据请求；
4. **读路径绝不算校验和**，`pread` 定位读，偏移一律 64 位；
5. **小文件写路径**：SQLite `synchronous=NORMAL` + `fsync_policy=by_size` + 有界写并发 8；
6. **容量测量必须用独立进程负载生成器**（进程内压测会得出错误结论）。
7. **异步化不是默认选项**：协程只在"真异步等待"时有效；阻塞 API（`pread`/`sendfile`/SQLite/libcurl-easy/fsync）
   在协程内会让出失败，**并发度退化为 io 线程数，比线程模型更差**。启用条件见 ADR-007 的 T1–T3。
8. **POSIX 写入路径优先摊销 fsync，而不是异步**：实测组提交（58,741 文件/s）> io_uring（20,497/s）
   > 64 线程（16,104/s）> 单线程（760/s）。`fsync` 的 1.44 ms 是设备物理成本，**异步无法消除，只有减少次数有效**。
9. **缓存命中路径禁止引入异步**：4 KiB 缓存命中读仅 **0.7 µs**，单线程可达 150 万 ops/s，异步是净损失。
10. **io_uring ≠ 协程**：io_uring 的全部收益可用"手写 submit/reap 事件循环 + 状态机"获得，
    协程只是语法糖。不要因为想用 io_uring 而被迫引入协程/Asio 重写控制面。
11. **写入必须遵守"数据先落盘再改名"**：采用 ADR-008 的两阶段批提交
    （`write all tmp → syncfs → rename all → fsync(dir)`）。**禁止** `rename` 早于数据 durable 的顺序。
12. **性能数字必须标注协议与安全性**：本项目已发生过一次"用不安全协议的 58,741 文件/秒 作为设计依据"
    的错误（ADR-008 §1）。基准结论必须写明协议、顺序与已验证的不变量。
13. **多实例下状态一律外置**：位置记录/元数据记录/在途租约放 PostgreSQL；文件数据放共享挂载。
    **禁止**"DB 不可用降级到本地 SQLite"。
14. **幂等键必须建唯一约束**：`(partition_id, file_id)` 与 `(partition_id, file_source)`。
    唯一约束建在随机主键上是无效的（本项目已犯过一次，见 ADR-009 §3 M2）。
15. **GC 禁止"无元数据记录即删"**：多实例下必须走"租约到期 + 原子领取"（ADR-009 §4.3）。
16. **tmp 文件名必须含实例标识**（instance_id + pid + 计数），否则共享存储上会静默串数据（ADR-009 M1）。
17. **租约/过期判定一律用数据库的 `now()`**，不用实例本地时钟（ADR-009 §6.4）。
18. **I/O 引擎默认用阻塞线程池**，io_uring 只能是可选引擎且必须带能力探测与回退
    （ADR-010：默认容器 seccomp 实测阻断 io_uring）。业务代码只依赖 `IIoEngine` 端口。
19. **任何"可选加速"能力都必须先有无依赖的默认路径**，并让探测结果在 `/v2/info` 可见。

**测试基建（已就绪，供 P2/P6/P8/P9 使用）**

| 资产 | 用途 | 支撑门槛 |
| --- | --- | --- |
| `scripts/dev_postgres.sh` | 无需 root 的真实 PostgreSQL（apt 解包 + initdb + 迁移 + 生命周期管理） | 全部 PG 相关门槛 |
| `db/migrations/*.sql` | 幂等迁移；`schema_migrations` 供 `schema_version_check` | C2.10 |
| `db/tests/001_verify_invariants.sql` | schema 不变量自检（含"索引键必须在 `file_source` 上且谓词含 `is_latest`"的回归断言） | C2.10 |
| `db/tests/002_advisory_lock.sh` | 跨会话 advisory lock：互斥、`kill -9` 后释放、会话级 vs 事务级 | C8.9 / C9.26 |
| `db/tests/003_concurrent_claim.sh` | 真并发原子领取：恰 1 赢家、重试幂等、**去掉约束必须复现多赢家（自证）** | C6.11 / C9.26 |
| CTest fixture `pg` | `cmake -DFSS_WITH_PG=ON` 后用 `ctest -L pg` 一键跑（实测约 30 秒） | — |

用法见 [`docs/development.md`](development.md) §3。

**关键约束（写代码时必须遵守）**

1. **protobuf 3.12 不支持 proto3 `optional`** → 用 message/wrapper 表达可选性。
2. **不使用需要 root 安装的依赖**，不引入 conan/vcpkg。
3. **不依赖外网**：集成测试所需的 mock 服务（S3）用 Python 3.10 标准库实现。

---

## 6. 风险与应对（与设计文档 §16 对应）

| ID | 风险 | 触发条件 | 本计划中的应对位置 |
| --- | --- | --- | --- |
| R-01 | **cpp-httplib 缺陷/行为变化** | 升级后 H-1/H-2 回归失败 | P1 C1.2/C1.2b/C1.3；版本锁定 + 校验和 + 包装层三重防护；备选：换 Boost.Beast（ADR-002 候选 3） |
| R-02 | SigV4 与真实存储不兼容 | mock 验签过但真实 MinIO 失败 | P5 C5.2（**独立验签**最关键）；如 docker 可拉取镜像，追加真实 MinIO 测试 |
| R-03 | protobuf 3.12 限制 | 需要真正字段存在性语义 | 已规避；P7 C7.5 验证映射足够 |
| R-04 | 错误体格式选择不兼容 | 对接方客户端不兼容 | P4 C4.5（三种形态都测）；`http.error_format` 开关 |
| R-05 | 无 root / 无外网 | P5 需要真实 S3 | mock-S3 自研（Python 标准库）；真实 S3 测试标记为可选 |
| R-06 | 上游契约漂移 | 上游新增 breaking change | §7 变更控制流程；契约集中在 `docs/03-api-contract.md` |
| R-07 | 大文件背压/超时 | 压力测试内存增长或连接悬挂 | P3 C3.4、P6 C6.9、P7 C7.6（RSS 上限断言） |
| R-08 | 领域层被污染 | 护栏被绕过/禁用 | P1 C1.1（护栏生效性自证）+ 每阶段回归 |
| R-09 | 租户隔离缺陷 | 跨租户能读到数据 | P8 C8.2（专用隔离测试，含签名 URL 越权） |
| R-10 | 上游"无 PUT / 无 versions"被误加 | 需求方要求不存在的端点 | `docs/03-api-contract.md` §2 明确标注；若需求方坚持，需先提供上游证据 |
| R-11 | `Driver` 硬编码 GCS 的兼容诉求 | 客户端依赖 `"GCS"` | P4 兼容开关 `storage.driver_report_override` + `/v2/info` 声明差异 |
| **R-12** | `TCP_NODELAY` 未开启 → 小请求 40ms 停顿 | 升级/换库后默认值变化 | P1 C1.11（含"关闭时必须复现 40ms"的自证） |
| **R-13** | 线程池过小 = 并发硬上限 | 配置错误 | P1 C1.12 + 容量基线 C9.11 |
| **R-14** | 集中存储下大文件传输无法零拷贝 | 带宽/CPU 上限 | ADR-006（C9.12 受控复核后定稿/放弃） |
| **R-15** | 逐文件 fsync 把上限压到 ~1.2k/s | 默认值不当 | P3 C3.11 + 配置默认 `by_size`/`NORMAL` |
| **R-16** | 容量测量方法本身不可信 | 决策依据失真 | C9.11 强制独立进程 + 保留错误方法探针作对照 |
| **R-17** | 误以为"上协程就能提高并发"而重写 I/O 栈 | 大成本、零收益（甚至负收益） | ADR-007 的 T1–T3 触发条件 + C9.17/C9.19；明确记录"协程+阻塞调用比线程模型更差"（实测 763 vs 127,026 req/s） |
| **R-18** | io_uring 在目标内核不可用导致异步化方案落空 | 数据面异步化受阻 | C9.18 在目标内核验证；ADR-007 给出两种情形下的方案 |
| **R-19** | 误把"写入慢"归因于并发模型，去做异步而无视 fsync 摊销 | 优化方向错误、收益为零 | ADR-007 §8.3 实测（组提交比 io_uring 快 2.9x）；C9.20/C9.21 |
| **R-20** | 组提交提高吞吐但扩大崩溃丢数据窗口 | 耐久性下降 | C9.20 显式声明粒度 + `/v2/info` 暴露 + 运维文档；默认档位可配 |
| **R-21** | **为性能而跳过数据 fsync（R1 违反）→ 断电后"文件存在但内容残缺"** | 高（数据完整性事故，且元数据可能已登记） | ADR-008 强制两阶段顺序；C9.23 顺序不变量自证测试。**实测不安全方案只快 11%，无取舍空间** |
| **R-22** | 文档/基准中的性能数字来自**不安全协议**而被误用 | 决策依据失真（**已发生一次**） | ADR-007 §8.3 已加更正批注；所有写入性能数字必须标注协议与安全性 |
| **R-23** | 多实例 tmp 名冲突 → 静默内容错乱 | 扩到 2 实例即随机发作 | ADR-009 M1；C6.13（含对照自证） |
| **R-24** | 多实例重复创建 | 重复记录 + 重复对象 | ADR-009 M2；C6.11 |
| **R-25** | 多实例 GC 误删在途上传 | 数据丢失 | ADR-009 M3；C6.12 |
| **R-26** | 误用 SQLite 做多实例共享状态 | 状态发散 | `deployment.mode=multi` 启动校验；C8.9 |
| **R-27** | NFS 语义未验证即上生产 | 高 | C9.27 为**上生产硬前提** |
| **R-28** | 多实例 `syncfs` 互相干扰 | 中 | 建议按 partition 分盘；C9.24 |
| **R-29** | **把 io_uring 当必需依赖 → 默认容器 seccomp 下启动即失败（EPERM）** | 高 | ADR-010：默认 `blocking` 引擎；`uring` 探测失败拒绝启动、`auto` 回退；门槛 C1.15 | 已实测 |

---

## 7. 变更控制（契约漂移处理）

当需要修改 `docs/03-api-contract.md` 时，必须**同时**完成：

| # | 动作 |
| --- | --- |
| 1 | 在证据文件中记录变更原因与上游依据（commit / 文件路径 / 行号） |
| 2 | 更新受影响的契约测试（新增/修改断言），**先让测试失败，再改实现** |
| 3 | 若变更影响 proto：更新 `proto/osdu/file/v1/file_service.proto` 与 §4 映射表 |
| 4 | 若变更引入新的 `ErrorKind`：更新 §5 映射表 + `test_error_equivalence.cpp`（否则该测试会因空缺而失败） |
| 5 | 若变更是**破坏性**的（字段改名/状态码变化）：在 `docs/CHANGELOG.md` 记录，并在 `/v2/info` 的版本信息中体现 |

**上游升级核对清单**（每次 OSDU 发布新 milestone 时执行）

```
[ ] 重新拉取 osdu/platform/system/file 的对应 tag
[ ] 重新提取 file-core/.../api/*.java 的 @*Mapping 与 @PreAuthorize
[ ] 与 docs/03-api-contract.md §2 逐条 diff
[ ] 检查 docs/api/community/v2/openapi.yaml 是否有新增内容
[ ] 检查 data-definitions 的 File.Generic 是否有新版本（当前 master 为 1.1.0，1.0.0 仍被使用）
[ ] 更新 docs/01-osdu-research.md 的取样 HEAD
```

---

## 8. CI 建议

```yaml
# 伪代码：阶段门槛串行，前序失败即阻断
stages:
  - phase0:  ctest -L phase0
  - phase1:  ctest -L phase1        # 含护栏自证脚本
  - phase2:  ctest -L phase2
  - phase3:  ctest -L phase3
  - phase4:  ctest -L phase4
  - phase5:  ctest -L phase5        # 需要 python3 启动 mock-S3
  - phase6:  ctest -L phase6
  - phase7:  ctest -L phase7
  - phase8:  ctest -L phase8
  - phase9:  ctest -L phase9 && scripts/run_all_gates.sh
sanitizers:                           # 与功能测试并行，任一失败即阻断
  - asan_ubsan: cmake -DFSS_SANITIZE=address,undefined && ctest -L "phase1|phase2|phase3"
  - tsan:       cmake -DFSS_SANITIZE=thread && ctest -L "phase3|phase9"
```

**证据归档要求**：每个阶段的 CI 运行必须把 `docs/test-evidence/phaseN.md` 作为产物归档，
内容包括：门槛命令、退出码、测试用例数/断言数、关键输出摘要、结论、以及"本阶段发现并修复的缺陷"。

---

## 9. 首个动作（下一步要做什么）

P0~P9 已完成并通过门槛；**阶段 10 的切片 1（配置面接线）、切片 2（GC/expiry/拒绝语义）、
切片 3（审计 fail-closed / SQLite 调优 / 鉴权与 gRPC 面）、切片 4（数据面 PUT 上限 + SQLite PRAGMA）
与切片 5（`self_signed` 三键：`key_id` + 自签 TTL 上界）与 **C10.16 / C10.16 续** 已完成**（见文末「阶段 10」）。
三态：**生效 106 / 拒绝启动 19 / 已读但无效果 31**（`docs/operations.md` §1.3）。
**下一步 = 阶段 10 的后续切片**，按 §1.3.3 的"已读但无效果"清单收敛：

1. ~~**C10.16**~~ ✅ 已完成（`partition.file.opendes.max_file_bytes` → 413；校验算法 → exit 78）；
   ~~`storage.posix.*` 细节键与容器名无字段可接~~ → **C10.16 续已完成**：`PosixBlobStoreOptions` /
   `PartitionConfig` 加上真实字段并接通（5 + 2 键生效、3 键拒绝启动，见文末）；
2. GC 的 **HTTP 端点**（周期调度与 `--once` 已在切片 2 交付；手动触发/查询未做）；
3. `storage.proxy_mode=always` / 远端 Storage Service（`metadata.repository=remote`）；
4. PG 仓储/租约 + `deployment.mode=multi` 运行形态（ADR-009）—— 同时解锁 `leases.*`/`leader_election.*`；
5. `server.http.large_file_plane.*` 与 sendfile 数据面（ADR-006 §6）；
6. `metadata.sqlite.{journal_mode,synchronous,max_write_concurrency,group_commit*}` /
   `location.sqlite.{synchronous,group_commit*}` —— 两个仓储 Options 里没有这些字段（不发明字段）。

---

## 阶段 10：配置面接线（把"文档里有的配置"变成"产品里生效的配置"）

> **为什么需要这个阶段**：P9 的 C9.9 逐键核对了 `config/fss.example.json` ——
> **156 个叶子键中只有 31 个接通**（组合根只读环境变量），**125 个未接通**。
> 这等于"文档写了、产品没有"，属于 R13/R15 明确禁止的状态；
> 同时它带来真实的运维/安全后果：不设环境变量启动 = **无鉴权** + **每文件 fsync**。
> P9 无法在本轮完成接线，故单列一个阶段，并按切片交付。

**目标**：让 `config/fss.example.json`（带注释的 JSON）成为**真配置源**，
优先级 **CLI > 环境变量 > 配置文件 > schema 默认值**（`fss::config::Load` 已实现该语义），
并把 P9 登记的"未接通"清单收敛到可证实的小集合。

**切片与判据**

| # | 判据 |
| --- | --- |
| **C10.1** | 组合根支持 `--config <path>`（等价 `FSS_CONFIG`）：加载带注释的 JSON，未知键/非法值/类型不符 → **拒绝启动**并逐条打印 `path + 原因 + 来源`；`--print-config` 打印**脱敏后**的有效配置与每项来源 |
| **C10.2** | 优先级与来源可见：同一键同时由 CLI/环境/文件给出时按 **CLI > env > file > default** 生效，且 `SourceOf(path)` 与启动横幅一致（有测试/脚本断言） |
| **C10.3** | **server.http.\*** 接通：`bind_address`、`port`、`worker_threads`、`max_connections`、`base_path`、`idle_timeout_seconds`、`json_request_timeout_seconds`、`transfer_idle_timeout_seconds`、`transfer_buffer_bytes`、`transfer_memory_budget_bytes`、`max_*_bytes` —— 生效且有"非法值拒绝启动"的反向测试 |
| **C10.4** | **storage.\*** 接通：`driver`、`posix.root`、`posix.durability`、`posix.fsync_threshold_bytes`、`posix.instance_id`（→ 临时文件命名）、`io_engine`（`blocking|uring|auto`，按 ADR-010 探测与回退/拒绝）、`s3.*` 超时与预签名时长 |
| **C10.5** | **auth.\*** 接通 + **生产强校验**：`deployment.environment=production` 时 `auth.mode` **必须** `jwt`、`jwt.hmac_secret` 必须非空、`verify_signature` 必须为真 —— 否则**拒绝启动**（把 P9 登记的"默认无鉴权"变成不可误配） |
| **C10.6** | **observability.\*** 接通：`log_level`、`log_format`、`log_service`、`redact_keys`、`audit_enabled`、`audit_fail_closed`、`metrics_enabled`、`metrics_path` |
| **C10.7** | **未接通清单收敛并机械化**：`docs/operations.md` 的"接通状态"列必须与实现一致；新增测试断言"**声明接通的键**在真实进程上确实生效"（覆盖 C10.3~C10.6 的键），未接通项必须显式列出（不得沉默） |
| **C10.8** | 自证（R1）：把配置加载**去掉**（退回只读环境变量）→ 新测试必须失败；恢复 → 通过。并保留一条"环境变量仍可覆盖文件"的正例 |

**门槛命令**：`ctest -L phase10 && scripts/verify_config_wiring.sh`（后者用**真实二进制** +
临时配置文件断言生效与拒绝启动两侧）。

**交付物**：`src/main/server_main.cpp`（接线）、`tests/*`（配置生效/拒绝的用例）、
`scripts/verify_config_wiring.sh`、`docs/operations.md`（接通状态列更新）、
`config/fss.example.json`（如有键需要与 schema 对齐）。

**状态：🚧 切片 1（配置面接线）完成**。C10.1~C10.8 全部满足：
`--config` / `--set` / `--print-config`（脱敏 + 来源）+ **exit 78（EX_CONFIG）** 失败语义；
组合根接入 `fss::config::Load`；旧环境变量别名（`FSS_HTTP_PORT` / `FSS_STORAGE_ROOT` /
`FSS_POSIX_DURABILITY` / `FSS_TRANSFER_SECRET` / `FSS_S3_ACCESS_KEY` …）逐条保留；
**156 键中 66 键已接通**（`docs/operations.md` §1.3/§1.4 逐键登记，90 键仍未接通）；
证据：`ctest -L phase10`（`tests/integration/test_config_wiring.cpp`，真实二进制）+
`scripts/verify_config_wiring.sh`；自证（R1）：去掉组合根的配置文件层后新用例失败。

**切片 2（判据）**

| # | 判据 |
| --- | --- |
| **C10.9** | **GC 真正跑起来**：组合根装配 `GcTask`（此前只在测试里被构造）+ **周期调度**（`gc.interval_seconds`；`gc.enabled=false` 或 `interval=0` → 不启动调度，横幅显式说明）；`gc.dry_run`/`require_lease_expiry`/`staging_ttl_hours`/`orphan_grace_hours` 生效；调度循环**可优雅停止**（与 `Stop`/join 同一退出路径，AGENTS 陷阱：不得留下 joinable thread）；GC 指标在**真实进程**的 `/metrics` 可见（`fss_gc_*`）；`--once`（或等价开关）支持"跑一轮就退出"便于 cron |
| **C10.10** | **样例配置真的能起来**：用 `config/fss.example.json` 作为 `--config`（仅覆盖路径/密钥等环境相关项）**启动成功**并 `readiness_check` 200 —— 这条把"156 键的文档"变成"可执行的事实"；同时在测试里断言"故意改坏一个键 → 拒绝启动" |
| **C10.11** | **不许"读了但静默无效"**：对每个键，`docs/operations.md` 必须标注三态之一 —— `生效` / `拒绝启动（列出触发条件）` / `已读但无效果（必须给出理由与下一步）`；对**未实现**的键（`large_file_plane.enabled=true`、`storage.io_engine=uring`、`metadata.repository=postgres|remote`、`leases.*`/`leader_election.*` 在 single 模式、`events.publisher=webhook`、`self_signed.single_use_nonce=true`）**非默认值必须拒绝启动**而不是被忽略 （`legal/schema.validator=remote` 已在**切片 6a（C10.18 / ADR-013）**接通为**生效**，不再属于本清单） |
| **C10.12** | `expiry.default`/`expiry.max` 接通（`app::ExpiryPolicy`），并有正/反用例（超上限拒绝、边界通过） |

**状态：🚧 切片 2（C10.9~C10.12）完成**。
* **C10.9**：组合根装配 `GcTask` + 后台周期调度（`gc.interval_seconds`，第一轮立即跑；
  `gc.enabled=false`/`interval<=0` → 不启动 + 横幅说明）；`gc.dry_run`/`require_lease_expiry`/
  `staging_ttl_hours`/`orphan_grace_hours` 读入 `GcOptions`；SIGINT/SIGTERM → 统一退出路径
  （先 `Stop()`+`join()`，绝不留 joinable thread）；`fss_gc_*` 在真实进程 `/metrics` 可见；
  新增 `--once`（跑一轮 GC 退出 0，打印 `GcReport` 摘要）；单实例用
  `src/infra/location/memory/memory_lease_repository.h`（PG 版未交付）。
* **C10.10**：`config/fss.example.json` 作为 `--config`（只覆盖路径/端口/密钥）**启动成功且
  readiness 200**；改坏 `server.http.port` → exit 78。
* **C10.11**：16 个未实现能力的非默认值 → **exit 78 +「未实现 + 下一步」**；
  `docs/operations.md` 逐键三态化（**当前为 生效 106 / 拒绝启动 19 / 已读但无效果 31 = 156**；各切片的历史计数与理由见 `docs/test-evidence/phase10.md`）。
* **C10.12**：`expiry.default`/`expiry.max` → `app::ExpiryPolicy`（作用于签发 URL 的 TTL；
  超上限**静默夹紧**、边界通过、非法仍 400 + 固定消息）。
* 证据：`ctest -L phase10`（`tests/integration/test_config_wiring.cpp`，16 用例 / 288 断言）+
  `scripts/verify_config_wiring.sh`（54 条断言）+ `docs/test-evidence/phase10.md`。
* 自证（R1）：把 GC 调度的 interval 强行设为 0 → C10.9 用例失败（指标不涨/tmp 不删），还原后全绿。

**切片 3（判据）** —— 把"已读但无效果"的键按**依赖就绪**逐组接通（每组都要正/反用例）

| # | 判据 |
| --- | --- |
| **C10.13** | **审计 fail-closed 真正实现**：`observability.audit_fail_closed=true` 时，审计写入失败 → **请求失败**（5xx，且不影响已提交的数据面语义），`false` 时保持现状（非致命）；两种取值各一条真实进程用例（正例：`false` 下审计后端坏掉仍正常服务） |
| **C10.14** | **SQLite 调优键接通**：`metadata.sqlite.*` / `location.sqlite.*`（`busy_timeout_ms`/`journal_mode`/`synchronous`/`max_write_concurrency`/`group_commit*`）作用到两个仓储（PRAGMA 与并发闸门要能在真实进程上被观测：例如 `journal_mode` 与 `busy_timeout` 用 `python3 sqlite3` 读回、`synchronous` 与写并发用基准/日志证明） |
| **C10.15** | **鉴权与 gRPC 面**：`auth.jwt.roles_claim` 与 `auth.local_roles.*` 接通（配置里的"用户 → 角色"表真的决定 403/200）；`server.grpc.enabled` 接通（false → 不开 gRPC 端口，true → 开且 `GetInfo` 可用） |
| **C10.16** | **分区与存储细节**：`partition.file.<partition>.*`（容器名/`max_file_bytes`/校验算法集合与默认算法）与 `storage.posix.{group_commit_max_batch,sync_dir_after_batch,atomic_write,dir_mode,file_mode,fadvise_random,fadvise_dontneed_after_large_read}` 接通；**超限必须被拒**（上传超过 `max_file_bytes` → 413 或契约规定的错误），非法的校验算法 → 400/拒绝启动（按真实语义断言） |

**切片 5（判据）** —— `self_signed` 的 3 个剩余键

| # | 判据 |
| --- | --- |
| **C10.17** | **`self_signed` 三键接通**：① `self_signed.key_id` 进**被签名的 token 载荷**，解码侧在验签通过后要求载荷里的 `key_id` 与当前配置**完全相等**（**缺失也拒绝** → `kUnauthenticated` / HTTP **401**）；真实进程正例（`k1` 签发的 URL PUT/GET 200）+ 反例（以 `k2` 重启后重放同一 URL → 401）；**多密钥轮换未交付**（ADR-009:227 的"多 key 并存"仍是待办，只做标识绑定）。② `self_signed.{default_ttl_seconds,max_ttl_seconds}` 是**自签分支（`!native_presign`）的 TTL 上界**（`ttl = min(ttl, max)`；请求未给 `expiryTime` 时再 `min(ttl, default)`），`expiry.*` 的既定语义（= `expiryTime` 参数的解析规则与缺省，C10.12）**不变**；`native_presign` 分支**完全不受影响**（进程内断言 `PresignOptions.expires_in_seconds` 仍等于 `expiry` 的结果）。若要把语义改成「`self_signed.*` 覆盖 `expiry.default` 作缺省」，必须先推翻 C10.12 并同步契约与测试 |

**切片 6a（判据）** —— 远端 legal / schema 校验器（ADR-013）

| # | 判据 |
| --- | --- |
| **C10.18** | **远端 legal / schema 校验器接通（6 个键）**：`legal.validator` / `legal.remote.{base_url,timeout_ms}` 与 `schema.validator` / `schema.remote.{base_url,timeout_ms}` → **生效**。线协议（ADR-013）：`*.remote.base_url` 就是**完整端点 URL**（POST 到它，**不追加路径**）；legal body `{"partition","legaltags"}`、schema body `{"kind","record"}`；`200+{"valid":true}` → 通过、`200+{"valid":false,"message":M}` → **400**（带上 M）。**fail-closed 矩阵逐条实现 + 逐条测试**：连接失败 / 超时 / 非 200（含 401/403/500）/ 非 JSON / 缺 `valid` / `valid` 非 bool → 一律 **503**（`kUnavailable`），**绝不**降级成"通过"或"不通过"；校验发生在用例第 **3c** 步（持久化之前）→ 503 后**不留残留**（staging 对象在原位、persistent 侧无文件）。`base_url` 为空且选择器为 `remote` → **exit 78** + 可读原因（`Ready()`/`NotReadyReason()`）；`noop`（默认）**不发起任何请求**。测试：真实 `build/bin/fss_server` + 独立进程 mock（`tests/tools/mock_validators.py` + `tests/framework/mock_validators.h`，含 `--observe-file` 断言请求体形状与"有没有发请求"）。⚠️ **未与真实 Legal/Schema 服务联调**；端口签名不带 bearer token → 端点须允许无 per-request 认证访问。 |
| **C10.18 伴随更正** | `auth.remote_entitlements.fail_closed` 从「已读但无效果」更正为「拒绝启动（触发条件）」：`auth.mode=remote-entitlements` 且该键非 `true` → **exit 78**（`core_schema.cpp` 跨字段校验，ADR-012 §5.1）；**模式相关**（`jwt`/`disabled` 下 `false` 被接受）。真实进程用例：C10.11 的两个 SECTION（反例 78 + R16 正例不被拒 + 模式无关性） |
| **C10.19** | **事件发布器 `events.publisher` 与 `events.webhook.*` 接通（4 个键）**：`events.publisher`（1 个来自「拒绝启动」）与 `events.webhook.{url,timeout_ms,topic}`（3 个来自「已读但无效果」）→ **生效**。线协议（ADR-013 §9）：`events.webhook.url` 就是**完整端点 URL**（POST 到它，**不追加路径**，与 C10.18 同一约定）；`Content-Type: application/json`；`topic` 取**配置值** `events.webhook.topic`。载荷镜像上游事件形状：`statusChanged` = `{"topic","kind":"statusChanged","body":{recordId,partition,status,datasetSync,version}}`；`datasetDetails` = `{"topic","kind":"datasetDetails","body":[{properties:{correlationId,datasetId,datasetType,datasetVersionId,recordCount,timestamp}}]}`（**长度为 1 的数组**，对齐 `FileDatasetDetailsPublisher.java`）。**2xx = 成功；其余一切（非 2xx / 连不上 / 超时 / 坏响应）→ 记一条可读告警并继续** —— **发布失败绝不能**让 HTTP 请求失败：请求照常 **201** 且记录**真的建出来**（这是 C10.18「无残留」的**镜像**语义，两条方向相反、必须各自被测；用例层保持 `(void)ports.events.Publish...`，**不得**改成 `FSS_TRY`）。`events.publisher=none` → 组合根内联 `NoopEventPublisher`（**显式关闭**，不发请求）；`log`（默认）→ 既有 `LogEventPublisher`（行为逐字不变、不发请求）；`webhook` + 空 `url` → **exit 78** + 可读原因。测试：`tests/integration/test_webhook_publisher.cpp`（真实 `build/bin/fss_server` + `tests/tools/mock_validators.py --mode webhook`，`--observe-file` 新增 `bodies` 列表以断言"两个 kind 都发了"）—— 正例 / **非致命三态**（连不上、非 2xx、超时）/ `none` 不发请求 / `log` 不受影响 / 空 url → 78 / `timeout_ms` 真生效。⚠️ **内联同步发布**（上游是异步消息总线）：慢 webhook 给请求路径增加 **事件数 × timeout_ms**；**异步有界队列 / 重试退避 / 投递保证未交付**（ADR-013 §9.4）；**未与真实消息总线/中间件联调** |

**状态：✅ 切片 1/2/3/4/5/6a/6b 完成（含 C10.16/C10.17/C10.18/C10.19）**。
* **C10.13**：`observability.audit_fail_closed` 真的决定"审计写入失败是否让请求失败"。
  用例层 `RecordAudit()` 现在返回 `Result<void>`，`AuditGuard::Success()` 在**返回前**记录成功审计
  并把结果交回（`FSS_TRY(audit.Success())`）——析构无法改状态码，那正是"审计失败却报 200"的静默缺陷。
  `audit_fail_closed=true` → 审计写入失败映射为 **500**（契约 §5 的 `kInternal`）；`false`（默认）保持非致命。
  可驱动接缝：`FSS_AUDIT_FAULT_INJECT=1`（故障注入，**不是**配置键）→ 真实进程上 `uploadURL` 在
  `true` 时 500、`false` 时 200（正例对照，R16）。
* **C10.14（部分）**：只接**真实存在**的 Options 字段 —— `metadata.sqlite.busy_timeout_ms`、
  `location.sqlite.busy_timeout_ms`（`sqlite3_busy_timeout`）、`location.sqlite.journal_mode`
  （映射到 `SqliteLocationRepositoryOptions.wal`；`WAL|DELETE` 两档，`TRUNCATE` → exit 78 不静默降级）。
  `synchronous`/`group_commit*`/`metadata.sqlite.journal_mode` 在两个 Options 结构体里**没有**字段 →
  按"不发明字段"留在"已读但无效果"（§1.3.3）。证据：`python3 sqlite3` 从库文件读回 `wal`/`delete`。
* **C10.15**：`auth.jwt.roles_claim`（默认 `roles`）与 `auth.local_roles.*` 接到 `LocalJwtOptions`
  （claim 名与"用户→角色表"真的决定 200/403）；`server.grpc.enabled` 接通
  （`false` → 不开端口；`true` 且 `port≠0` → 开且 `GetInfo` 可用；`FSS_GRPC_PORT=0` 的既有语义不变）。
* **C10.16**：只接**真实存在**的字段（先读了 `PartitionConfig` 与 `PosixBlobStoreOptions`）。
  `partition.file.opendes.max_file_bytes` → `PartitionConfig.max_object_bytes`（0 → -1 = 不限），
  并落到数据面 PUT 的 `RouteOptions::max_body_bytes`：带 `Content-Length` 超限 → **413**（读体前
  前置拒绝），chunked → 400。`allowed_checksum_algorithms` / `default_checksum_algorithm` →
  **启动期校验**（未知算法名 / 默认不在集合内 → **exit 78**）：这是"按真实语义"的断言 ——
  C6.4 有上游一手证据表明客户端声明的算法是**被覆写**的输入，请求期不做 400。
  **C10.16 续（本轮收尾）**：为 `PosixBlobStoreOptions` 与 `PartitionConfig` 加上真实字段并接通 ——
  `storage.posix.{atomic_write,dir_mode,file_mode,fadvise_random,fadvise_dontneed_after_large_read}`
  与 `partition.file.opendes.{staging,persistent}_container` → **生效**（+7）；
  `storage.posix.{group_commit_max_batch,sync_dir_after_batch}`（ADR-008 的 P4 两阶段批提交未实现，
  如实拒绝启动）与 `partition.file.opendes.storage_driver`（与顶层驱动冲突）→ **拒绝启动**（+3）。
  用例：`tests/integration/test_config_wiring.cpp` 的 `[c10.16]`（驱动层 + 真实进程）。
* **切片 4（数据面上限 + SQLite PRAGMA）**：`server.http.transfer_max_body_bytes` → 数据面 PUT
  上限（全局键 >0 时与 `partition.file.<p>.max_file_bytes` **取较小者**；两者都 0 → 仍不限）；
  `metadata.sqlite.{journal_mode,synchronous}` 与 `location.sqlite.synchronous` → 两个仓储的
  `wal` / `synchronous_level` 字段 + 真执行 PRAGMA。`synchronous` **不落盘**，用同连接访问器
  `AppliedPragma("synchronous")` 做进程内断言；`journal_mode` 用 `python3 sqlite3` 读回。
  用例：`tests/integration/test_config_wiring.cpp` 的切片 4 三用例 +
  `tests/integration/test_sqlite_{location,metadata}_repository.cpp` 的 `AppliedPragma` 用例。
* **切片 5（`self_signed` 三键，C10.17）**：`self_signed.key_id` → `HmacTransferTokenCodec`
  的第 3 个参数（非空时 `Encode` 把它写进**被签名的载荷**，`Decode` 要求完全相等、缺字段也拒绝
  → 401）；`self_signed.{default_ttl_seconds,max_ttl_seconds}` → `LocationIssuer` 的
  `SelfSignedTtlOptions`（**仅自签分支**的上界夹紧；`native_presign` 分支逐字不变；
  `expiry.*` 仍是 `expiryTime` 参数的解析规则与缺省）。用例：`tests/unit/test_transfer_token_key_id.cpp`
  （codec 边界 4 用例）、`tests/unit/test_location_issuer.cpp` 的 C10.17、`tests/integration/test_config_wiring.cpp`
  的 C10.17 两用例（真实进程正例 + 换 `key_id` 重启后 401 反例 + TTL 上界）。**未交付**：多密钥轮换
  （ADR-009:227 的"多 key 并存"）。
* **切片 6a（远端 legal / schema 校验器，C10.18 / ADR-013）**：新增 L2 适配器
  `src/infra/legal/remote_legal_validator.{h,cpp}` 与 `src/infra/schema/remote_schema_validator.{h,cpp}`
  （与 `RemoteEntitlementsAuthorizer` 同一套写法：`NOSIGNAL`/`FOLLOWLOCATION=0`/连接超时+整体超时/
  所有依赖故障 → `kUnavailable`/`Ready()`+`NotReadyReason()`）；组合根按
  `legal.validator`/`schema.validator` 选择 `noop`（默认）或 `remote` 并打印端点与超时（**不打印密钥**）；
  用例第 3c 步的注释更正（失败方向由 `ErrorKind` 决定：本地/远端"不通过" → 400、依赖故障 → 503）。
  用例：`tests/integration/test_remote_validators.cpp`（真实进程 + mock；正例 / 400 带 message /
  fail-closed 五态 / 无残留 / 启动拒绝 / noop 不发请求 / `timeout_ms` 真生效）+ `tests/tools/mock_validators.py`。
  **规格勘误（父代理已确认）**：本切片涉及 **6 个键**（2 个来自「拒绝启动」+ 4 个来自「已读但无效果」），
  不是最初写的 7 个。**未交付**：与真实 Legal/Schema 服务联调、调用方身份透传（需改端口契约）、
  校验结果缓存、重试/退避、`connect_timeout_ms` 配置键。
* **切片 6b（事件发布器，C10.19 / ADR-013 §9）**：新增 L2 适配器 `src/infra/event/webhook_event_publisher.{h,cpp}`（与 C10.18 的远端校验器同一套写法：
  `NOSIGNAL`/`FOLLOWLOCATION=0`/连接超时 `min(1000, timeout_ms)` + 整体超时/2xx 判定/  `Ready()`+`NotReadyReason()`），但失败**方向相反**：
  发布失败只记一条 `Warn` 并返回 `Err(kUnavailable)`，**用例层丢弃它**（保持 `(void)ports.events.Publish...`，  **不是** `FSS_TRY`），请求照常 201、记录照常落库。
  组合根三分支：`log`（默认，既有 `LogEventPublisher` 逐字不变）/ `webhook` / `none`（内联 `NoopEventPublisher`，  **显式关闭**）；横幅打印 publisher/端点/timeout/topic（**不打印密钥**）。
  `src/app/usecases/usecases.cpp` 的 `PublishStatus` 补上 `record_id`（第 10 步/幂等命中路径带真实 id；  第 1 步 IN_PROGRESS 发生在建记录前 → 空），使 `statusChanged.body.recordId` 与上游形状一致。
  用例：`tests/integration/test_webhook_publisher.cpp`（真实进程 + `mock_validators.py --mode webhook`；  `--observe-file` 新增 `bodies` 列表断言两个 kind 都发了）+ `tests/unit/test_composition_root_guard.cpp` 清单加   `WebhookEventPublisher`。**未交付**：异步有界发布队列、重试退避、投递保证、与真实消息总线/中间件联调。
* 三态计数：**生效 106 / 拒绝启动 19 / 已读但无效果 31 = 156**（`operations.md` §1.3 + `test_operations_doc` 机械断言）。
  其中「拒绝启动」+1 来自 C10.18 的**伴随更正**（`auth.remote_entitlements.fail_closed`），
  与"6 个键接通"是两件事（落点不同：一个进「生效」、一个进「拒绝启动」）。

**未做（本阶段不承诺）**：**异步有界事件发布队列 / 重试退避 / 投递保证**与真实消息总线联调（同步 webhook 发布器已在**切片 6b（C10.19）**交付；ADR-013 §9.4 登记了未交付项）、
`metadata/location.repository=postgres|remote`、`leader_election.*`/`leases.*` 的 PG 语义、
`storage.proxy_mode`/`driver_report_override`/`provider_key_override`（DMS 响应整形，需先定契约）、
`gc` 的 HTTP 端点。这些仍为"拒绝启动"或"已读但无效果（附理由与下一步）"，**不得**改成静默忽略。

**未做（本阶段不承诺）**：`gc.*` 的 HTTP 端点（只做周期调度与一次性运行）；
PG 仓储/租约与 `mode=multi` 运行形态、`storage.proxy_mode`/远端 Storage Service、
`leader_election.*`/`leases.*`（依赖 PG）、sendfile 数据面（ADR-006 §6）、
`observability.audit_fail_closed` 的"致命审计"行为**已在 C10.13 交付**（见 §8 的 `operations.md` 说明）。
