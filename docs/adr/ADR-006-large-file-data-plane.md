# ADR-006：大文件数据面 —— **下载面已交付**（方案 ③：进程内自持 socket + `sendfile`，复用控制面校验）

- 状态：**已交付（Implemented，仅下载面）**；§6 第 5 条（真实存储/网卡上的受控基线复核）**未完成**
- 日期：2025（方向定稿于 P9 受控复核）；**下载面落地**：本轮
- 相关：`docs/adr/ADR-002-http-framework.md`（`fss_http` 包装层）、
  `docs/adr/ADR-007-async-and-coroutines.md`、`docs/adr/ADR-010-io-engine-choice.md`（io_uring）、
  `docs/05-capacity-and-concurrency.md` §1.7/§1.9、
  证据：`docs/test-evidence/phase9-adr006.md`、`docs/test-evidence/phase10.md` §25、
  `docs/appendix/capacity-baseline/BASELINE.tsv`
- 门槛：**C9.12**（受控复核得倍数）、**C9.16**（本文件）
- 落地实现：`src/common/http/large_file_plane.{h,cpp}`（第二个监听 socket + `sendfile`）、
  `src/common/bytes/bytes.h` 的 `NativeFd()`、`src/infra/transfer/posix_file_byte_source.{h,cpp}`、
  `domain::IBlobStore::OpenNativeRead`、`Router::BuildTransferGetHandler()`、
  `src/common/http/http.h` 的 `ErrorBody`/`LogAccess`（同源）、
  `MeteredBlobStore::{OpenNativeRead,RecordNativeRead}`、组合根装配
- 用例：`tests/integration/test_large_file_plane.cpp`（P1~P12；`ctest -L adr006`）

---

## 1. 结论

**采纳**"为集中存储实现 `sendfile(2)` 大文件数据面"这一方向 —— 依据是 C9.12 规定的判据：
用**同一个负载生成器**、**独立进程 + 互不重叠绑核**做受控 A/B，得到的倍数为

> **run 1：2.30x / 2.02x / 2.04x，几何平均 2.12x；run 2（独立重跑）：2.27x / 1.80x / 2.02x，
> 几何平均 2.02x —— 两次都 ≥ 1.5 → 采纳。**（逐点散布 ±10%，原因见证据 §3）

但**采纳不等于照原样实现**。本 ADR 同时定下四条硬边界（§4）与实现门槛（§6）。
**本轮已按方案 ③ 落地"下载面"**：进程内第二个监听 socket（`GET/HEAD /<base>/v1/transfer/{token}`），
字节用 `sendfile(2)` 送出（无原生 fd 或 `use_sendfile=false` 时回退用户态 pump），
**校验复用控制面同一个已包装 handler**（§4 第 1 条按构造满足）。
**尚未落地**：§6 第 5 条（真实存储/网卡上的受控基线复核）—— 见 §6 与 §5。

## 2. 备选方案（必须真有备选；含未实测项）

| # | 方案 | 现状/评估 | 结论 |
| --- | --- | --- | --- |
| ① | **httplib 内容提供者**（`fss::http::Server` + `Response::Stream`） | 已交付、已硬化（Range/416、限额、超时、连接上限、审计、访问日志、`/metrics`）；受控 A/B 的**基线**（2387/7320/6990 MiB/s @c1/c4/c16） | **保留为默认路径**；不是"错的"，只是每次传输多一次用户态拷贝 |
| ② | **独立进程 + sendfile 数据面**（原容量文档 §3.1 的建议） | 未实现；把字节面从 HTTP 适配层里剥离，需要新的进程/端口/生命周期与运维面 | **暂不选**：控制面校验会被复制一份，重复实现是 P1/P4 一系列缺陷的根因（见 §4） |
| ③ | **进程内自持 socket + sendfile**（本次复核原型采用的形态） | 已实测：**5488/14785/14274 MiB/s**，即 2.02–2.30x；**已按 §4 的边界条件落地（本轮）**：`src/common/http/large_file_plane.*`，复用 `Router::BuildTransferGetHandler()` 的**同一个**已包装 handler，可关闭、默认关闭 | **已交付（下载面）**：只 `GET/HEAD`；多段 Range 降级为 200 全量；`sendfile` 绕过 `get()` ⇒ 计数点分裂（见 §5 与 `docs/00-final-design.md` §5.bb） |
| ④ | **对象存储模式（客户端直连预签名 URL）** | 已交付（ADR-005，C5.8/C5.9 验证"S3 模式下 SignedURL 指向存储端点"）；字节**完全不经过服务** | **大文件的生产首选**：它绕开了这个问题的本体（服务不再搬字节）。集中存储场景仍需要 ① 或 ③ |
| ⑤ | `io_uring` + `sendfile` | ADR-010 已定：默认容器 seccomp 阻断（实测 `EPERM`），`io_engine=blocking` 为默认 | **不引入**（除非 ADR-010 的 U1–U4 触发条件满足） |

## 3. 实测证据（受控 A/B，C9.12）

方法：`bench/sendfile_ab.cpp`（一个二进制两种模式）+ 仓库内独立负载生成器
`bench/capacity_bench.cpp load`（keep-alive、显式 `Content-Length`、无 chunked、双端 `TCP_NODELAY`）；
服务端 `taskset -c 4-7`、客户端 `taskset -c 8-15`（**不重叠**）；两侧并发上限相同
（httplib `worker_threads=4`/`max_connections=4` ↔ sendfile 4 个连接线程）；
负载为**非稀疏** 256 MiB 文件；每点位 4 s（预热 0.8 s 不计入）；**所有点位 `errs=0`**。

| 模式 | c1 | c4 | c16 |
| --- | --- | --- | --- |
| httplib 内容提供者（run 1 / run 2） | 2387 / 2667 MiB/s | 7320 / 7867 MiB/s | 6990 / 7133 MiB/s |
| **裸 socket + `sendfile`**（run 1 / run 2） | **5488 / 6059** | **14785 / 14156** | **14274 / 14428** |
| 比值（sendfile / httplib） | **2.30x / 2.27x** | **2.02x / 1.80x** | **2.04x / 2.02x** |
| 客户端 CPU（httplib / sendfile，run 1） | 38.9% / 108.7% | 348.8% / 433.5% | 355.8% / 431.8% |

> **主表的 c16 不是"16 并发"**：两侧并发上限都是 4 线程 / 4 连接（对称），c16 只有 4 条连接在跑
> （`reqs` 与 c4 几乎相同）。把两侧上限同时提到 16 的补充点位（证据 §3.3）得到
> httplib 10.2 GiB/s vs sendfile 21.0 GiB/s（**2.05x**），但该点位 `client_cpu_pct=864.9%`
> ≥ 客户端 8 核上限的 90% → 按 R4 **绝对值无结论**（比值仍是偏保守的下界）。

- **几何平均 2.12x**（脚本 `scripts/bench_sendfile_ab.sh` 直接给出）；
- 客户端 CPU 最高 4.3 核（可用 8 核）⇒ **两侧都未被客户端饱和**；
- **所有点位 `errs=0`**（含 keep-alive 复用与连接上限），否则数字不予采信。

**与早期探针的关系**：`docs/05` §1.7 曾给出"sendfile vs httplib 约 5–7x"，那是**不同探针、
不同客户端**的拼装比较，方法学上不可比。本次受控复核得到 **2.12x**，**方向一致、幅度更小**；
旧结论的"5–7x"已按 P9 证据更新（见 `docs/00-final-design.md` §5 与本文件 §5）。

## 4. 落地的硬边界（采纳的条件，不是建议）

1. **数据面必须复用控制面校验，不得出现"第二套 HTTP 语义"**：
   `Range` 归一化与越界 416、请求头/URI/体限额、空闲与整体超时、在途连接上限（503 + `Retry-After`）、
   走私检查（重复 `Content-Length`、`CL`+`chunked`）、访问日志与审计、`/metrics` 记账，
   全部必须与 `fss_http` 包装层**同源**（同一份实现或同一份共享代码），并有**等价性测试**
   （参照 P7 的双协议等价性矩阵的做法：同一批请求打到两条路径，逐字段比对）。
2. **数据面的安全姿态不得低于现状**：自签 token 校验、`partition` 绑定、跨租户隔离一条都不能少；
   传输仍是明文 HTTP/1.1（现状即如此），**TLS 一旦成为硬需求，零拷贝收益需要重新评估**
   （用户态 TLS 会重新引入拷贝）。
3. **单实例与多实例行为一致**：`instance_id`/临时文件/租约/GC 的约束不变（ADR-009）。
4. **可回退**：数据面必须是**可关闭**的（配置项 + `/v2/info` 可见），默认路径仍是 ①，
   关闭后行为与今天完全一致（R11 的"可选加速能力必须先有无依赖的默认路径"）。

## 5. 诚实声明：未验证 / 无结论

| 项 | 说明 |
| --- | --- |
| **本机是 WSL2 + 虚拟盘 + loopback** | 绝对数字（14.8 GiB/s 已是**内存带宽量级**）**不能**当磁盘或网卡吞吐；真实 NVMe/HDD/NFS 与真实网卡上的比值**未测**（C9.14 登记为未验证） |
| **文件全程在页缓存里** | 每点位前预热，256 MiB < 可用内存 ⇒ 测的是**"页缓存已热"的路径成本（拷贝/系统调用数）**，**不含磁盘 I/O**。冷缓存或真实存储下磁盘会成为主导项、差距可能被抹平 → 本实验对"真实磁盘上的 sendfile 收益"**无结论** |
| **绝对值是否已被内存带宽饱和：无法区分** | sendfile 侧 4 并发后停在 14–15 GiB/s；本环境无 root（不能 `drop_caches`）⇒ "路径成本触顶"与"内存/loopback 带宽触顶"**无法分开**（R4：绝对值无结论）。**可复现的是比值**（同一条件紧邻测得） |
| **两侧都不含 token 校验与 Router/用例层** | 对比的是**纯数据面路径**；产品路径（`large_stream_*`，含自签 token 校验）绝对吞吐更低（64 MiB、c4 = 3394.8 MiB/s），但本实验**不能**把差值归给数据面路径 |
| **一个未测的廉价备选** | 调大 `StreamPump` 的 64 KiB 块（或按大块 `writev`）可能拿到"一部分收益"，属改产品代码、**未实测**，本 ADR 不据此决策（登记为后续可评估项） |
| **原型不是可上线数据面**（历史陈述） | `bench/sendfile_ab.cpp` 只支持 `GET /blob`：**没有** Range、鉴权、限额、超时、连接上限、审计、日志、`/metrics`、`keep_alive_max_count` 语义。★ **本轮交付的数据面已补齐这些**（`src/common/http/large_file_plane.*`）：Range/416/多段降级、鉴权（复用控制面**同一个** handler）、URI/头限额、空闲超时 408、在途连接上限 503+`Retry-After`、访问日志（同源 `LogAccess`）、`/metrics`（含数据面诊断计数）、`keep_alive_max_count`。**审计**不由数据面单独写（控制面的 `transfer.get` 本身也不写审计，两条路径一致） |
| **§6 第 5 条（真实存储/网卡复核）未完成** | 本环境是 WSL2 + 虚拟盘 + loopback：**没有**真实 NVMe/NFS、**没有**真实网卡。§6 要求在其上重测并更新 `BASELINE.tsv`/`docs/05` —— 本切片**不新增任何绝对数字**，只把 `scripts/bench_sendfile_ab.sh` 与执行前置条件移交生产环境（与 C9.27 的 NFS 探针同一处置）。本机只报告比值与自用吞吐（页缓存/loopback），**不迁移** |
| **TLS / HTTP2 未评估（本切片仍未评估）** | 现状不支持 TLS；§7.3 的重开条件仍然有效：一旦 TLS/mTLS 成为硬需求，`sendfile` 的收益需要**重新评估**（内核 TLS 或用户态 TLS 都会改变结论）。本轮**未做**任何 TLS 相关工作 |
| **真实跨主机 / 多实例数据面未测** | 本切片的数据面只在**单进程 + loopback**上验证（P1~P12）。多实例下数据面与控制面的租约/`instance_id`/GC 约束（ADR-009）**未在数据面上单独复测**；跨主机 LB 粘性同样未测 |
| **> 2 GiB 对象未实测** | 代码按 `sendfile_chunk_bytes` 循环（单次上限可配，默认 1 GiB），但本切片**实测最大对象 = 1 GiB**（P7）。>2 GiB / >5 GiB 分片未测 |
| **`splice` / 上传加速刻意未做** | 上传（`PUT`）留在控制面；`splice(2)`/`receive-file` 不在本 ADR 的实现门槛内，**本轮未做**。
| **对象存储模式的对照未包含在同一 A/B 中** | 方案 ④ 的收益是"服务不搬字节"，其数字取决于对象存储，**不在本 ADR 的判据范围内** |
| **TLS/HTTP2 未评估** | 现状不支持 TLS；若将来引入，sendfile 路径需要重新设计（内核 TLS 或用户态 TLS 都会改变结论） |

## 6. 实现门槛（**已交付状态**；本 ADR 的方向 + 落地记录）

落地时必须同时满足（缺一条即视为未完成）。**本轮交付状态**：

1. ✅ **等价性矩阵**：Range（含后缀/多段降级/416）、限额（413/400）、超时（408）、连接上限（503+`Retry-After`）、
   走私检查、`401/403/404`、`X-FSS-Error-Kind` 与错误体格式 —— 与现有 REST 数据面**逐条等价**。
   ★ **实测的两处显式差异（已登记，非静默）**：① **多段 Range 降级为 200 全量**（控制面对可寻址来源是 multipart 206；§6.1 本条自己要求"多段降级"）；② **超长 URI** 数据面回 **414**，而控制面因 httplib 的 header reader 在 URI 检查之前失败而**不返回任何 HTTP 响应**（库行为，见 `docs/00-final-design.md` §5.x）。**`413` 条目是 PUT-only 的关注点**（数据面不服务上传 ⇒ 不适用）。判据：`tests/integration/test_large_file_plane.cpp` 的 P2/P4/P8（P8 是逐字段矩阵，唯一被文档化的差异是多段）；
2. ✅ **`/metrics` 与访问日志同字段**（含 `driver`、字节数、状态码、correlation-id）：访问日志的 emitter 抽成 L1 `fss::http::LogAccess`，数据面与 httplib 包装层**调同一份**；P11 逐字段比对两侧 `http_request` 的键集合。存储字节/操作数与 `get()` **同族同标签**（`MeteredBlobStore::RecordNativeRead`），P5 双向断言；
3. ✅ **RSS 断言**：1 GiB 传输的 RSS 增长不得超过既有门槛（`test_posix_large_file` 的 `RssLimitKib()`）。P7 实测服务进程 `VmHWM` 增长 **240 KiB**（上限 64 MiB，本机实测，页缓存/loopback 条件）；
4. ✅ **配置与可观测**：开关 + 默认关闭 + `/v2/info` 暴露**实际**数据面（新增 `largeFilePlane`，REST + proto 字段 13）+ 启动横幅 `plane bind :`（与 ADR-010 的 `ioEngine` 同一条纪律）；
5. ❌ **受控基线复核（未完成）**：用同一负载生成器在**真实存储**上重测，并更新
   `docs/appendix/capacity-baseline/BASELINE.tsv` 与 `docs/05`。
   ★ **本环境做不到**：WSL2 + 虚拟盘 + loopback，且 §5 自己声明这些绝对值**无结论**。
   因此**不新增任何绝对数字**；脚本 `scripts/bench_sendfile_ab.sh` 与执行前置条件**移交生产环境**
   （与 C9.27 的 NFS 探针同一处置）。本机只报告"页缓存/loopback"条件下的**比值**（不迁移）；
6. ✅ **不降低安全姿态**：跨租户隔离与 token 校验的用例在数据面路径上**再跑一遍**（P3：篡改 token → 401、缺 sig → 401、`data-partition-id` 不符 → 403、对象不存在 → 404；且与控制面**同体**）。

## 7. 重开（放弃）条件

出现任一条即回到方案 ①（httplib 内容提供者），并在本文件记录"旧结论 → 现状 → 影响"：

1. 在**真实存储 + 真实网卡**上的受控复核比值 **< 1.5x**；
2. §4 第 1 条做不到（只能复制一套 HTTP 校验逻辑）—— 重复实现带来的缺陷风险高于 2x 的收益；
3. TLS/mTLS 成为硬需求，且没有可接受的零拷贝 TLS 方案；
4. 运维面成本不可接受（第二个端口/进程/生命周期，且无法用同一份配置与探针管理）。
