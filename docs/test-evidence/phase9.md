# 阶段 9 测试证据（🚧 进行中）

| 项 | 值 |
| --- | --- |
| 阶段 | P9（硬化与交付） |
| 状态 | ✅ **P9 完成 —— 切片 1/2/3 全部完成**（C9.1~C9.13、C9.15、C9.16、C9.25 满足；C9.8 见 `docs/test-evidence/phase9-image.md`；C9.12 见 `docs/test-evidence/phase9-adr006.md`） |
| 门槛命令 | `ctest -L phase9` |
| 退出码 | `0`（**6 测试 / 466 断言**：`test_concurrency` 92、`test_fault_injection` 79、`test_resource_limits` 44、`test_metrics_and_gc` 196、`test_transfer_error_semantics` 41、`test_operations_doc` 14） |
| 全阶段门槛 | `./scripts/run_all_gates.sh` → **P0~P9 全绿，总耗时 225 s（3 分 45 秒，10 个阶段）**；`ctest` **75/75** 通过 |

> **切片 3（交付面）**：C9.7 门槛纳入 P9 并记录耗时（**225 s / 10 阶段**）、C9.8 容器镜像
> （构建/启动/readiness 200/强制 jwt/非 root/HEALTHCHECK **全部实测**）、
> C9.9 运维文档（`operations.md` + `runbook.md` + 自动比对测试覆盖 **156 个配置键**）、
> C9.10 风险收口（§16.1 覆盖全部 **20 行**）。
>
> ★ 本切片顺带修掉了 6 个**同族**缺陷（P9-D04~D09，见 §6）：全部是"**元数据/状态语义错误**"
> —— 时间戳时基、临时文件计数、响应状态码被覆盖、守卫白名单。它们的共同特征是
> **"看起来通过"**：类型正确、编译通过、原有测试全绿，但判据实际恒真/恒假。
> 抓到它们的手段都是**同一招**：把"此前没人断言过的语义"写进**契约测试**（memory/POSIX/S3 共用）。
> 需要真实硬件/多进程/容器的判据（C9.14/C9.17–C9.22/C9.26–C9.30）**本环境不具备条件**，将如实登记为"未验证"。

---

## 1. 切片 1 交付物

| 路径 | 内容 |
| --- | --- |
| `tests/hardening/test_concurrency.cpp` | C9.1：**100 并发 JSON（真实端口 + 真实线程，零 5xx）** + **8 并发大文件（真实 POSIX，逐路 SHA-256 必须属于自己）** |
| `tests/hardening/test_fault_injection.cpp` | C9.2：存储不可用 / 元数据写入失败 / **磁盘满（ENOSPC，含回滚）** / **远端鉴权依赖不可用** 四类故障 + 非致命依赖两类 + **恢复时间断言（≤30 秒）** |
| `tests/hardening/test_resource_limits.cpp` | C9.3：**6 类上限各一个"被拒绝"的测试**（头/URI/体×2 档/连接数/传输内存预算）+ 每类都有"上限之内必须成功"的对照 |
| `tests/tools/mock_entitlements.py` + `tests/framework/mock_entitlements.h` | 新增 `--fail-file`（文件存在 = 依赖不可用；删除 = 恢复）→ 让"**解除故障后的恢复时间**"可以在**真实远端依赖**上测 |
| `tests/framework/repo_path.h` | `RepoRelative` 抽成共享头（两个 mock 各定义一份会 ODR 重定义） |
| `tests/framework/fake_ports.h`、`src/common/ids/id_generator.h` | **并发安全修复**（P9-D01：见 §4） |

## 2. 判据进展

| 判据 | 状态 | 证据 |
| --- | --- | --- |
| **C9.1** 并发 | ✅ | ① 100 线程 × 5 轮 × 3 类端点（免鉴权探活 / 写位置记录 / 读仓储）在真实端口上：`server_errors == 0`（**零 5xx**）且 `unexpected == 0`；② 8 并发大文件（默认 8 MiB/个，真实 POSIX）：逐路 PUT/GET + **逐路 SHA-256 与存储侧 `stat().checksum` 三方一致**，任何"串数据"都会让某一路不匹配 |
| **C9.2** 故障注入 + 恢复 | ✅ | 四类故障 + 两类非致命依赖各一条断言；**每类都在解除后轮询探测并断言恢复 ≤ 30 秒**（实测毫秒级）。错误映射按**路径**区分并如实断言：取上传地址时存储不可用 → **503**；12 步序列第 6 步复制失败 → **502**（`kBadGateway`，契约 §5 对"依赖服务失败"的归类） |
| **C9.3** 6 类资源上限 | ✅ | ① 头超限 → 400；② URI 超限 → 连接关闭无响应（8191 字节必须被正常处理作对照）；③ JSON 端点体 11 MiB → 413；④ 小体端点 300 KiB → 413；⑤ 在途连接数超限 → 503 + `Retry-After`（只允许 200/503，且恢复后能正常服务）；⑥ `并发 × 缓冲 > budget` → **拒绝启动**（`ValidateOptions`） |
| **C9.5** GC 判据 | ✅ | `test_gc_lease`（P6）已覆盖"有记录永不删/无租约不许删/在途不删/dry-run 不动/dry-run 计数"；本切片补 `test_metrics_and_gc`：① dry-run 只报候选（`tmp_removed == 1`）而**文件仍在**；② 真删只删够旧的残留；③ 反向：即使某条位置记录**恰好指向** `.tmp_*` 键也照删（那种键不可能由 `ObjectKeyPolicy` 生成） |
| **C9.6** `/metrics` | ✅ | `test_metrics_and_gc`：① HTTP（请求/延迟直方图/在途/被拒）+ 自签校验失败 + **存储操作/字节（新）** + **GC（新）** 全部出现；② 值真的动过（不是"指标存在但恒 0"）；③ 逐行格式校验（每个 family 都有 HELP/TYPE，样本行匹配 `name{labels} value`）；④ **不含 secret**（把夹具里的敏感串逐个扫渲染结果）；⑤ `/metrics` 本身是唯一免鉴权端点且 `Content-Type: text/plain` |
| **C9.25** `.tmp_*` 清理 | ✅ | `test_metrics_and_gc`：够旧的清掉（含"被位置记录引用"的那个）、**太新（5 秒前）的保护**、`tmp_skipped_too_young == 1` 且指标 `fss_gc_skipped_total{reason="tmp_too_young"}` 同步；另有 `report.tmp_skipped_unknown_mtime`（取不到 mtime → 保守不动） |
| **C9.4** p99 阈值 | ✅（本机阈值） | `scripts/bench_baseline.sh` 内置 p99 阈值表（控制面读 ≤50 ms、数据面小对象 ≤20 ms、PUT ≤60 ms、大流 ≤400 ms、上传链 ≤200 ms；取基线最差点位的 ~2.5 倍余量），超限**直接失败**。实测 p99：控制面读 **253 µs（c1）/ 539（c4）/ 4346（c16）/ 19123（c64）**，数据面小对象 381 / 3109，PUT 7292 / 17276，大流 42606 / 77289，上传链 12526~49810 —— 全部在阈值内。⚠️ 这些是 **WSL2 + 虚拟盘**上的阈值，生产阈值必须在目标硬件上重新推导（C9.14） |
| **C9.11** 容量基线（方法学） | ✅（本机数字，非生产结论） | 独立进程 + **互不重叠**绑核（服务端 `4-7`、客户端 `8-15`）+ 同一负载生成器；四类都测了：① 小段读请求率上限（控制面 **14.3k req/s @c4**；数据面小对象 **13.7k req/s @c16**）② 大文件单流吞吐（**1272 MiB/s @c1**，c4 合计 3395 MiB/s）③ **4→64 并发扩展曲线**（见下：**c4 之后吞吐回落**）④ SQLite 写吞吐（**1482 / 2409 writes/s** @1/8 线程）。全部点位 `errs=0`；客户端 CPU 占比同时记录（R4） |
| **C9.13** 内存预算 | ✅ | `test_resource_limits` ⑥：`并发上限 × 缓冲 > budget` → `ValidateOptions` 拒绝（组合根在 `Server` 构造时校验 → `Bind()` 失败即**拒绝启动**），并有一条**正例**（满足预算 → 通过），见 §2 的 C9.3 行 |
| **C9.15** 小文件上传端到端 + fsync 档位 | ✅（本机数字） | `chain` 模式实测**完整链路**（uploadURL → PUT → POST metadata，含 token 校验、POSIX 原子写、位置/元数据两处 SQLite 写）：**34.9 files/s @c1 → 174.2 @c8**；**fsync 档位差异**（c4）：`per_file` **96.4 files/s** vs `batch` **410.1** vs `never` **426.0** ≈ **4.3×**，与 ADR-008 的"fsync 是主成本"一致 |
| **C9.9** 运维文档 | ✅ | `docs/operations.md`（68 KB）+ `docs/runbook.md`；**自动比对测试** `tests/unit/test_operations_doc.cpp`（14 断言）：`config/fss.example.json` 的 **156 个叶子键全部**以 `` `路径` `` 形式出现在文档中（非空洞下限 40 + 文档 >5000 字节）、示例⇄schema 一致、**反向断言**（文档里出现的每个 `a.b.c` 都必须 `CoreSchema().IsAllowedPath`）。自证：注入假键 → 反向断言失败；删掉 `gc.interval_seconds` 的文档 → 正向断言失败 |
| **C9.10** 风险收口 | ✅ | `docs/02-design.md` §16.1：**实际 20 行风险**（R-01~R-08/R-12~R-16/R-23~R-29）逐条给出"✅ 已落地 / 🟡 部分 / ⛔ 显式接受"与**可执行证据**（测试名/脚本/ADR）；并登记纠正"计划里的 11 项"为过时数字。已登记未做：多实例运行形态（PG 仓储/租约）、NFS 语义（生产门禁）、sendfile 实现、组合根未接 JSON 配置 |
| **C9.7** 门槛纳入 P9 + 耗时 | ✅ | `scripts/run_all_gates.sh` 的 `IMPLEMENTED_PHASES` 已含 9；P0~P9 **顺序全绿**，脚本末尾打印 **`⏱ 总耗时: 3 分 45 秒（225 s，阶段数 10）`**（本轮实测）；`ctest` 75/75 |
| **C9.8** 容器镜像 | ✅ | `Dockerfile`（多阶段、非 root `fss` uid 10001、`HEALTHCHECK`、`VOLUME /data`、**强制 `FSS_AUTH_MODE=jwt`**）+ `scripts/verify_image.sh`：镜像 **107 MB**、冷构建 452 s、`readiness_check` **200**、`/v2/info.authMode=jwt`、无 token → **401**、伪造 token → **401**、`HEALTHCHECK=healthy`、`FSS_AUTH_MODE=disabled`/空密钥 → **exit 64 拒绝启动**。证据 `docs/test-evidence/phase9-image.md`（含代码冻结后的最终复验） |
| C9.12/C9.16（ADR-006 受控复核） | 🚧 进行中 | `docs/test-evidence/phase9-adr006.md`（同一负载生成器对比 httplib 内容提供者 vs 裸 `sendfile`） |
| C9.14/C9.17–C9.30（需真实硬件/多进程/容器） | ⬜ 未验证 | 环境不具备（详见 §9） |
| C9.17–C9.30（需真实硬件/多进程/容器） | ⬜ 未验证 | 环境不具备（详见 §5） |

## 3. 数据面并发用例的设计（为什么"逐路比对"是必需的）

`8 并发大文件 + 校验和一致`有几种**看起来**够、实际不够的写法：

- ❌ 只断言"8 个请求都 200" —— 只能证明没崩，不能证明**内容没串**；
- ❌ 只比对"总字节数" —— 长度相同但内容互换依然通过；
- ❌ 8 路用**同一份**载荷 —— 串数据永远发现不了。

本用例的做法：**第 i 路的载荷 = `slot-<i>:` + 重复模式**。于是

1. 客户端对**自己那一份**算 SHA-256（流式，不额外驻留对象）并与响应比对
   —— 任何跨路串数据都会让**长度相同但摘要不同**；
2. 收尾再用 `PosixBlobStore::stat().checksum` 做**存储侧的独立对账**（该摘要是驱动
   在写入过程中增量算出来的）——两条独立路径都指向同一份内容才算通过。

## 4. 切片 1 发现的实现陷阱（P9-D01~D03）

| 编号 | 症状 | 根因 | 规避 |
| --- | --- | --- | --- |
| **P9-D01** | 100 并发用例直接 **`double free or corruption (out)` + SIGABRT**（一次都没跑到断言） | **测试替身不是线程安全的**：`AllowAllAuthorizer`（`calls`/`last_role`/`last_roles`）、`RecordingAuditLogger`（`events`/`calls`）、`RecordingEventPublisher`（`events`/`details`）、`FakeBlobStoreFactory`（`calls`/`last_partition`）在多个服务线程里被并发写 → 数据竞争 + 堆损坏。此前的"并发"用例只有 4~8 个线程、且审计是 P8 才全面铺开的，所以一直没暴露 | 给这些替身加 `std::mutex`（与 P6 修 `InMemory*Repository` 同一条纪律）；`SequentialIdGenerator` 的计数器改为 `std::atomic`（并发取 id 会拿到同一个 id → 唯一键冲突）；`test_multi_instance.cpp` 里对它的**赋值**改为 `Reset()`（原子成员不可赋值）。**生产代码**的 `HttpMetrics`/日志器/仓储本来就都有锁，本切片未发现生产侧竞争 |
| **P9-D02** | "磁盘满"用例第一次跑返回 **201**（故障没注入进去） | 两个 zone 指向**同一个** store 时，12 步序列走的是 `IBlobStore::copy()` 而**不是** `put()`；只在 `put()` 上注入故障就完全没生效 | 故障装饰器**同时**覆盖 `put()` 与 `copy()`（并把这个"同 store 走 copy"的路径写进注释）；用例的"注入必须真的生效"通过状态码断言钉住 |
| **P9-D03** | `RecordMetadata` 故障用例返回 201 | 替身的 `fail_create` 开关**默认 false**，用例忘了打开 | 注入类用例必须显式打开开关；"故障后的状态码"断言本身就是"注入生效"的证据（这也是为什么不能只断言"最终成功"） |

---

## 5. 切片 2（指标 + GC 清理）交付物

| 路径 | 内容 |
| --- | --- |
| `src/common/metrics/metrics.{h,cpp}`（新 L1 目标 `fss_metrics`） | 最小指标注册表：`Register/Increment/SetGauge/Value/RenderPrometheus/Reset`，`Labels` 有序（`std::map`）→ 渲染稳定；标签值转义；每个 family 固定一对 HELP/TYPE |
| `src/infra/blob/metered/metered_blob_store.{h,cpp}`（新 L2 目标 `fss_blob_metered`） | 计量装饰器：`fss_storage_operations_total{driver,op,outcome}` + `fss_storage_bytes_total{direction}`；**按调用私有**的计数器（并发写不互相干扰）；只装饰不改语义 |
| `src/adapters/http/router.{h,cpp}` | `RouterOptions.metrics_registry`；`/metrics` 渲染 HTTP 指标 + 注册表文本（唯一免鉴权端点） |
| `src/domain/ports/ports.h` | 新增 `IBlobStore::remove_temp_files(...)`（**临时文件不是对象**：`list()` 永远看不见它们）+ `domain::TempSweepResult`（删除数 / 太新保护数 / mtime 未知数） |
| `src/infra/blob/{posix,memory,s3}` | 三个驱动各实现 `remove_temp_files`：POSIX 走目录扫描比对 **mtime**；内存按语义删 `.tmp.` 键；S3 无服务端临时文件（原生预签名） |
| `src/app/tasks/gc_task.{h,cpp}` | 第 ④ 步：清理 staging/persistent 两个容器里**够旧**的 `.tmp_*`；新增 GC 指标族；`tmp_skipped_too_young`/`tmp_skipped_unknown_mtime` 进入 `GcReport` 与指标 |
| `tests/hardening/test_metrics_and_gc.cpp` | C9.6 + C9.25 的用例（真实 POSIX 驱动 + 真实 `/metrics` 端点 + 真实 `utime` 造陈旧文件） |
| `tests/hardening/test_transfer_error_semantics.cpp` | P9-D05/D07 的回归（裸 `Server` + 自定流式路由 + 真实数据面 401 各一条） |
| `src/common/http/server.cpp` | 收尾读体时**不再无条件**覆盖 handler 的响应；新增"流式 handler 必须把请求体读干"的守卫（P9-D05/D07） |
| `tests/framework/port_contract.h` | 补上**此前没人断言过的语义**：`list()`/`stat()` 的 `last_modified_epoch_seconds` 必须是 Unix 纪元秒（P9-D04/D08 就是从这个缺口进去的） |
| `tests/tools/mock_s3.py` | `GET`/`HEAD` 补 `Last-Modified`（真实 S3 一定有；缺了它 S3 驱动的 `stat()` 时间恒为 0） |

## 6. 切片 2 发现的实现陷阱（P9-D04~D09，全部为"看起来通过"型）

| 编号 | 症状 | 根因 | 规避 / 证据 |
| --- | --- | --- | --- |
| **P9-D04** | GC 在 POSIX 上把**刚写入/在途**的对象当成"够旧"回收（`tmp_removed` 比预期多 1，`tmp_skipped_too_young` 恒为 0） | `PosixBlobStore` 用 `std::filesystem::last_write_time().time_since_epoch().count()/1e9` 当 Unix 秒。它在 libstdc++ 上的时基是 **file_clock**，实测同一文件（`st_mtime=1699999995`）得到 **-4737664005**。于是 `GcTask` 的"太新 → 保护"比较（`> cutoff`）**恒假** | 改用与 `stat()` 端口操作**同源**的 `::stat().st_mtime`（新增 `MtimeEpochSeconds()`，失败返回 `nullopt` → 删除路径一律不动）；并把"Unix 纪元秒"写进**端口契约测试**（此前该字段没有任何断言）。自证：修复前该断言实测 `-4737664005 → FAILED` |
| **P9-D05** | 数据面 handler 提前报错（如 token 签名不合法 → 401）时，客户端收到 **`400 failed to read request body`**，真正原因被抹掉 | 流式路由收尾时 `pump->Stop()` 会让 httplib 的 reader 返回 false（"消费者不再取了"），第一版**无条件**用读体错误覆盖 handler 的响应 | 只允许两种情况覆盖：① handler 报 2xx（否则是**静默截断**）；② 读体错误是**确定性**的（超时/超限/声明长度不符，那是更精确的诊断）；其余保留 handler 响应并把事实写进访问日志（`handler_error_kept_over_body_read_failure`）。自证：把条件改回恒真 → 用例立刻变 400 而失败 |
| **P9-D06** | `.tmp_*` 的"太新保护"计数**翻倍**（1 → 2） | 第 ④ 步按"存储 × 容器"**叉乘**扫描；两个 zone 解析到同一个物理存储时（单实例/测试装配）同一目录被扫两遍。删除数因幂等看不出来，**保护数会翻倍** | 只扫"每个 zone 的存储 × 它自己的容器"两个组合；并让驱动回报"看到但保护了几个"（`TempSweepResult`）——POSIX 的 `list()` 看不见临时文件，这个数字只能由驱动给 |
| **P9-D07** | 流式 handler **没读请求体却回了 201** → 客户端以为存好了，字节被丢弃（**静默截断**） | `BodyPump::Push` 只要队列没满就返回 true，所以"handler 没读"时 httplib 的 `reader_ok` 仍是 true —— P9-D05 那条检查根本不会触发 | `BodyPump` 记 `consumed()`（真正交给 handler 的字节）；2xx 且 `consumed != read` → **500**（服务端缺陷）+ 访问日志 `handler_did_not_consume_body`。自证：关掉该守卫 → 客户端真的收到 `201 created` 而用例失败。正例对照：读干的 handler 仍 201 且字节数一致 |
| **P9-D08** | S3 驱动 `list()` 返回的每个条目时间恒为 **0**（`stat()` 同样为 0） | `ListObjectsV2` 的 `<LastModified>` 是 **ISO-8601**，`HEAD` 的 `Last-Modified` 是 **RFC 1123**；第一版对两者都套 `ParseHttpDate` → 前者解析失败返回 0。mock S3 也没回 `Last-Modified` | 按格式分派（`ParseObjectTimestamp`：ISO 优先、HTTP 日期兜底）+ mock 补头；契约测试同时断言 `stat()` 的时间戳。自证：`test_s3_blob_store` 在补断言后**立刻**抓到 `0 >= 1600000000 FAILED` |
| **P9-D09** | `capabilities()` 护栏（C2.7）把计量装饰器的**转发**调用判成越权 | 护栏只认"文件白名单"，而装饰器**必须**实现并转发 `capabilities()`——那不是"按能力分支" | 新增**更强的**白名单类别：`PassThroughCallSites()`，要求该文件里每一次 `capabilities()` 出现都必须是 `return inner_...capabilities()` 形态，并加"非空洞"断言。这样"加进白名单"等价于"声明它是纯转发器" |

**共同教训**：这五个缺陷全都**编译通过、原有测试全绿**。能抓到它们的原因是同一件事 ——
把此前**没人断言过**的语义写进**多实现共用的契约测试**（`list/stat` 的时间戳、"看到但保护"的计数、
"handler 报错时状态码是谁的"）。这也是 C9.6"指标必须真的动过"、C9.25"保护也要可观测"的由来。

## 7. 切片 2 的两条"反向约束"（为什么不是简单地放开/收紧）

1. **P9-D05 的放开**必须配一条**收紧**：`handler 报 2xx 但请求体没读完 → 必须改成读体错误`。
   否则"保留 handler 响应"就会变成"读体检查失效"。这条由 `test_transfer_error_semantics.cpp`
   的第三、四条用例钉住（分块体 + 半关连接 ⇒ 访问日志里必须是 400 `body_read_failed` 而不是 201）。
   附带实测发现：这种情形下 httplib 会**中止连接**，所以 400 发不出去 —— 客户端看到的是连接被关，
   **不会**把这次上传当成功；因此判据落在**访问日志**上（运维读到的真相）。
2. **P9-D07 的守卫**必须有**正例对照**：把请求体读干的 handler 仍然 201 且字节数一致，
   否则"必须读干"就可能退化成"凡是流式上传都报错"（R16）。

## 8. 自证对照（R1：每条关键判据都要能失败）

本切片的所有"防线"都做过**注入自证**（改坏实现 → 用例/护栏必须失败 → 还原）：

| 判据 | 注入方式 | 期望的失败 | 实测 |
| --- | --- | --- | --- |
| 契约：`list()` 的时间戳是 Unix 纪元秒（P9-D04） | 用回 `last_write_time().time_since_epoch().count()/1e9` | 契约用例失败 | ✅ 实测 `-4737664005 >= 1600000000 → FAILED`（POSIX 契约） |
| 契约：`stat()` 的时间戳是 Unix 纪元秒（P9-D08） | 移除 mock 的 `Last-Modified` | 契约用例失败 | ✅ 实测 `0 >= 1600000000 → FAILED`（S3 契约） |
| P9-D05：handler 的错误码不得被覆盖 | `if (handler_succeeded \|\| conclusive)` → `if (true)`（还原第一版行为） | 分块体 409 的用例变 400 而失败 | ✅ 实测 `400 failed to read request body`，用例失败 |
| P9-D05 另一半：2xx + 读体失败必须纠正 | 去掉 `handler_succeeded` 这一项 | 访问日志出现 `status: 201` | ✅ 实测访问日志留下骗人的 `status: 201`，用例失败 |
| P9-D07：流式 handler 必须读干请求体 | `false &&` 关掉该守卫 | 客户端收到 `201 created` | ✅ 实测响应正是 `201 created`，用例失败 |
| 能力护栏的"装饰器只能转发"（P9-D09） | 把 `metered_blob_store.h` 的 `capabilities()` 改成"改语义"的实现 | 护栏报出该文件 | ✅ 实测 `装饰器白名单项必须原样转发：src/infra/blob/metered/...`，护栏失败 |

> 注入**全部已还原**：`grep -rn "INJECT" src/` 为空；`run_all_gates.sh` 的前置检查（P6-D04）也会挡住残留。

## 9. 切片 2（性能基线部分）交付物与实测

| 路径 | 内容 |
| --- | --- |
| `bench/capacity_bench.cpp`（新） | **独立进程**负载生成器：`seed`（走真实 API 造对象）/ `load`（GET/PUT，keep-alive，p50/p99）/ `chain`（uploadURL → PUT → POST metadata 的**完整上传链**）；输出 `RESULT ... client_cpu_pct=...` |
| `bench/sqlite_write_bench.cpp`（新） | SQLite 写吞吐（位置仓储 + 元数据仓储，1/8 线程） |
| `bench/CMakeLists.txt`（新） | 两个基准目标，均 `EXCLUDE_FROM_ALL`（基准**不**进入日常构建与门槛） |
| `scripts/bench_baseline.sh`（新） | 编排：起服务（绑核）→ 播种 → 跑 14 个点位 → **p99 阈值检查** → 与基线对比（**退化 >20% 失败**）→ `--save` 写基线 |
| `docs/appendix/capacity-baseline/BASELINE.tsv`（新） | 基线文件（label/metric/value；含环境、绑核、协议、时长与"仅本机参考"的警示头） |
| `src/main/server_main.cpp` | 组合根**此前没读** `storage.posix.durability` / `fsync_threshold_bytes`（于是"档位差异"在真实进程上不可配置）→ 新增 `FSS_POSIX_DURABILITY`（`per_file｜batch｜never`，与 `FsyncPolicy` 的映射写在注释里）与 `FSS_POSIX_FSYNC_THRESHOLD_BYTES`；同时把组合根的元数据仓储从 `InMemoryMetadataRepository` 换成 **`SqliteMetadataRepository`**（P6 的登记项：单实例 = 内置 SQLite，ADR-004） |

**实测（`scripts/bench_baseline.sh --save`，4 s/点位，全部 `errs=0`）**

> 每个点位重复 **3 次取中位数**，原始逐次数据在 `build/bench-run/results_raw.tsv`。

| 点位 | 吞吐（3 次中位数） | p50 | p99 | 客户端 CPU |
| --- | --- | --- | --- | --- |
| 控制面读 `GET .../metadata` c1 / c4 / c16 / c64 | **6809 / 13849 / 12636 / 11820** req/s | 105 / 212 / 938 / 4065 µs | 275 / 552 / 3646 / 16480 µs | 16% / 50% / 50% / 52% |
| 数据面小对象 GET c1 / c16 | 4642 / **16678** req/s | 155 / 677 µs | 356 / 2641 µs | 13% / 89% |
| 数据面 PUT（4 KiB，`per_file`）c1 / c16 | 153 / **1355** req/s | 5161 / 9222 µs | 7037 / 12185 µs | 1% / 8% |
| 大文件单流 c1 / c4 | **1343 / 3997** MiB/s | 37.5 / 50.4 ms | 40.9 / 59.4 ms | 20% / 203% |
| 上传链 `per_file` c1 / c8 | 43.3 / **189.1** files/s | 20.8 / 34.3 ms | 24.9 / 44.6 ms | 1% / 4% |
| 上传链 `per_file` / `batch` / `never` @c4 | **109.9 / 468.6 / 452.0** files/s | 29.0 / 6.9 / 7.2 ms | 38.8 / 11.2 / 12.4 ms | 2% / 9% / 9% |
| SQLite 写 1 / 8 线程 | **1446 / 2430** writes/s（1 线程仅报告，见下） | — | — | — |

**从数字里能读出的三条（都如实标注适用范围）**

1. **并发：c1→c4 提升明显（6.8k → 13.8k req/s），c4 之后不再提升（12.6k / 11.8k），
   而 p99 从 0.28 ms 涨到 3.6 ms / 16.5 ms。**
   ⚠️ **"c4 是否峰值"在本机无结论**：四次独立运行测到的 c4 分别是
   **14308 / 14140 / 9969 / 13849 req/s**（本次中位数 13849）。同一次运行内的 3 次重复
   散布 <5%，而**跨会话漂移 ≈40%**（本机是共享的 WSL2 主机）⇒ 只能下"c4 之后不再随
   并发提升、延迟随并发显著上升"这个**方向性**结论；"峰值点位/负扩展"需独占硬件复核（C9.14）。
   客户端 CPU 全程 ≤52%（8 核可用）⇒ 不是客户端瓶颈。
2. **fsync 是小文件上传的主成本**：`per_file` 110 files/s → `batch` 469 files/s（**4.3×**），
   而 `batch` 与 `never` 几乎相同（469 vs 452）⇒ 收益来自"摊销 fsync"，不是"完全不做耐久性"。
   这与 ADR-008 的结论一致，也是 `storage.posix.durability` 默认值的依据。
3. **大文件吞吐在本机是内存带宽量级**（1343 MiB/s @c1 —— 与页缓存/loopback 同量级），
   **不能**当作磁盘吞吐；真实存储上的数字见 C9.14（未验证）。
4. **SQLite 单线程写在本机不可复现**（手动 7 次：2014~2488；脚本内多轮：359~1503）
   ⇒ 该点位降级为**报告型**（只打印、不参与判定），可判定的是 8 线程（2130~2488）。
   根因：虚拟盘上"每次写都 fsync 的单线程路径"对后台回写/删除活动极其敏感。

**测量方式（决定这些数字怎么用）**

- **每点位 3 次取中位数**（`FSS_BENCH_REPEATS`，默认 3；`--quick` 时为 1），逐次原始数据保留在
  `results_raw.tsv`，脚本会把"逐次散布 >20%"的点位显式打印（实测 SQLite 8 线程出现过
  654 writes/s 的离群点，根因是**播种/前序点位留下的脏页回写**抢盘 → 已在播种后与 SQLite 前
  各加一次 `sync`，复测 7 次为 2014~2488）。
- 因此"回归 >20% 判失败"这个判据**在本机只对"同一次会话 / 同一机器状态"有意义**；
  跨会话的机器漂移（实测 40%）会淹没 20% 的阈值。这一点与 fsync 类点位的告警机制一起，
  写进了 `scripts/bench_baseline.sh` 的注释与 §11 的未验证清单。

**方法学自证（为什么这些数字可信）**

| 检查 | 做法 | 结果 |
| --- | --- | --- |
| R2 独立进程 + 绑核 | 服务端 `taskset -c 4-7`、客户端 `taskset -c 8-15`（**不重叠**），脚本打印两者 | ✅ |
| R4 客户端不是瓶颈 | 生成器自报 `client_cpu_pct`（`CLOCK_PROCESS_CPUTIME_ID`）；除 `data_small_get_c16`（95%，仍远低于 8 核上限）外全部 <55% | ✅（该点位在报告里单独标注） |
| 前置条件（R9） | 每个点位都断言 `errs=0`，且"请求数 >0"（避免"0 请求也报成功"）；播种失败直接中止 | ✅ |
| 基线的"能失败" | `--check` 模式对吞吐退化 >20% 判失败；对 **fsync/磁盘类**点位（`data_put_*`/`upload_chain_*`/`sqlite_write_*`）**只告警**，因为 WSL2 上这些点位的逐次散布本身就 >20%（实测 `per_file c4` = 78.6 / 96.4 files/s）—— 这是 R4 的"无结论"处理，而不是放宽所有阈值 | ✅ |
| 基线不腐烂 | 基线文件头部记录环境/绑核/协议/时长与"仅本机参考"警示；`--save` 才写，日常跑只对比 | ✅ |

## 10. 切片 3（交付面）补充

### 10.1 组合根补齐：指标与日志脱敏（P9-D10）

`docs/operations.md` 的逐键核对（C9.9）顺带查出**我自己在切片 2 留下的同类缺陷**：

| 编号 | 症状 | 根因 | 修法 / 证据 |
| --- | --- | --- | --- |
| **P9-D10** | **真实进程**的 `/metrics` 只有 HTTP 族（没有 `fss_storage_*`）；且**日志完全不打码** | 组合根从不创建 `fss::metrics::Registry`/`MeteredBlobStore`（`RouterOptions::metrics_registry` 恒为 `nullptr`），并用 `LogOptions{}` 构造 logger（`redact_keys` 为空）。**测试夹具里都接上了** ⇒ "测试里通过、产品里不存在"（R15 那类陷阱的第三次应验） | 组合根创建 `metrics::Registry` + `MeteredBlobStore`（纯转发）并接进 `RouterOptions`；日志改用 `logging::OptionsFromConfig`，默认脱敏键与 `config/fss.example.json` 的 `observability.redact_keys` **逐项一致**（11 个键，`FSS_LOG_REDACT_KEYS` 可覆盖）；启动横幅新增 `log redact : 11 个键` 与 `metrics : 已接入（含存储计量）` 两行，使"接没接上"**在真实进程上可见** |

**真实进程实测**（`taskset -c 0 build/bin/fss_server` + 一次 `uploadURL` + 一次带 body 的 `PUT`）：

```
# TYPE fss_storage_bytes_total counter
fss_storage_bytes_total{direction="in"} 13
# TYPE fss_storage_operations_total counter
fss_storage_operations_total{driver="posix",op="ensure_container",outcome="ok"} 2
fss_storage_operations_total{driver="posix",op="put",outcome="ok"} 2
```

启动横幅：`log redact : 11 个键` / `metrics : 已接入（含存储计量）`。

### 10.2 C9.9 顺带记录的配置面事实（如实登记，非缺陷申报）

`docs/operations.md` §1/§8 逐键核对了 `config/fss.example.json`：**156 个叶子键中 31 个已接通**
（组合根读 `FSS_*`），**125 个未接通**（改配置文件不生效）。三条最需要注意：

1. **默认值分歧**：`auth.mode`（组合根 `disabled` vs schema `jwt`）、
   `storage.posix.durability`（`per_file` vs schema `batch`）⇒ **不设环境变量启动 = 无鉴权 + 每文件 fsync**。
   生产必须显式设 `FSS_AUTH_MODE=jwt`（容器镜像已强制）与耐久档位；三者差异已写进 `operations.md` §1.4。
2. `storage.posix.durability` 的 schema enum **不含 `never`**，而组合根接受三档（只能经环境变量打开）。
3. `observability.metrics_enabled` / `metrics_path` 未接通（`/metrics` 恒开、路径固定）。

这些属于"组合根未接 JSON 配置"这一个登记项（见 §11 与 `docs/02-design.md` §16.1 末尾）。

## 11. 未验证 / 环境限制（如实登记）

| 项 | 为什么未验证 | 已登记的替代证据 |
| --- | --- | --- |
| 指标口径的"生产级"校验（每秒采样、基数爆炸、远端抓取） | 需要真实 Prometheus/长跑流量 | 已断言"family 数有限 + 标签值受控 + 渲染稳定 + 不含 secret"；**未**做基数压测（登记） |
| **容量基线的"跨会话"可比性** | 本机是共享的 WSL2 主机：同一 3 次取中位数的点位在三次独立运行中给出 **14308 / 14140 / 9969 req/s**（control_read_c4，跨会话漂移 ≈40%），而同一次运行内的 3 次重复散布 <5% | 已把"每点位 3 次取中位数""逐次散布 >20% 显式告警""fsync 类点位只告警"写进 `scripts/bench_baseline.sh`；**"退化 >20% 即失败"在本机只对"同一会话/同一机器状态"有判定力**，独占硬件上才可作最终判据（C9.14） |
| "配置文件即真相"（`config/fss.example.json` 生效） | **组合根只读环境变量**：156 个叶子键里 125 个未接通（逐键见 `docs/operations.md` §1.3）；改配置文件不生效 | 已如实登记（`docs/02-design.md` §16.1 末尾、`operations.md` §1.1）并把每个键的 `FSS_*` 映射/未接通状态写进文档；**未做** JSON 配置加载器接线 |
| 大文件吞吐的**磁盘**语义 | 64 MiB 对象全程在页缓存（1159 MiB/s 是内存/loopback 量级）；无 root 无法 `drop_caches` | 只作"路径开销"参考；C9.14（真实 NVMe/HDD/NFS + 真实网卡）登记为未验证 |
| `fss_gc_skipped_total{reason=tmp_too_young}` 在真实生产流量下的量级 | 需要真实在途上传并发 | 已有确定性用例（5 秒前的临时文件必须被保护且计数为 1） |
| 真实 ENOSPC（写满一个小文件系统） | 需要一个可写的小文件系统（`mount` 需要 root；tmpfs 也不例外） | 用注入的 ENOSPC 语义（`kUnavailable`）覆盖**行为**（含回滚），并断言状态码 |
| 真实 NVMe/HDD/NFS 上的延迟分布与容量数字（C9.14/C9.22） | 本机是虚拟盘（WSL2 语义），`docs/00-final-design.md` 已明确**不得作为判定依据** | 现有量级参考仍标注为"量级参考" |
| io_uring 在目标内核上的可用性（C9.18/C9.29/C9.30） | 需目标部署内核；本机默认 seccomp 阻断（ADR-010 已实测 `EPERM`） | `scripts/check_io_uring.sh`（退出码 2 = 无结论）+ `UringIoEngine` 骨架 |
| 多实例 + 共享 PG 端到端（C9.26）与 PG 租约 | PG 版仓储/租约未交付（P9 登记项） | `db/tests/00{1,2,3}` 的 PG 基建门槛（`FSS_GATES_WITH_PG=1`）+ `test_multi_instance`（内存/单进程） |
| 容器镜像（C9.8）与真实网卡吞吐（C9.14） | 本环境无 docker/真实网卡 | 待切片 3 评估（Dockerfile 至少静态可检查） |

---

## 12. P9 补交（P10 期间完成）：GC 的按需 HTTP 端点（C9.31）

> **指针**：本轮（P10 切片 1~6b 期间）补交了 P9 登记未交付的一项 —— "GC 的 HTTP 端点"。
> 判据编号 **C9.31**（`docs/04-implementation-plan.md` 的 P9 段）；契约 **§7.3**；决策 **ADR-013 §10**。
> 因为它是 **P9** 门槛，证据按阶段归档在本文件；P10 侧只留指针（`docs/test-evidence/phase10.md` §13）。

### 12.1 结论（先说答案）

* **端点**：`POST {base_path}/v2/gc:run`（本实现选 `gc:run` 这一形态；路由名 `ops.gc_run`）——
  上游 OSDU **没有** GC 端点，这是本服务的**运维扩展**。
* **授权**：`service.file.admin`，**不要求** `data-partition-id`；`401`（无 token）/ `403`（非 admin）/ `200`。
* **报告**：`GcReport` 的全部字段（**snake_case**）+ 运行态 `partition` / `scheduled`。
* **dry-run 只能更保守**：有效值 = `配置 gc.dry_run || 请求 ?dryRun=true`；**没有**"强制真删"的参数。
* **单飞护栏是 `GcTask` 的通用性质**：周期调度与端点共享同一实例；已在跑 → `kUnavailable` → **503**，
  **不排队、不并行**。
* **`gc.enabled=false` 时端点仍可用**（按需 GC 与周期调度是两件事），报告 `scheduled=false`。
* **审计**：`operation=gcRun`（actor / partition / result / correlationId / epochMillis），成功与失败两侧。
* **不加任何配置键**：`docs/operations.md` §1.3 三态仍是 **106 / 19 / 31 = 156**（`test_operations_doc` 通过）。

### 12.2 实现点（可点击）

| 位置 | 内容 |
| --- | --- |
| `src/app/tasks/gc_task.h:75` / `:115`、`src/app/tasks/gc_task.cpp:245` | ★ **单飞护栏**：`std::mutex run_mutex_` + `std::unique_lock(..., std::try_to_lock)`；失败即 `kUnavailable` + 可读消息（"GC 已在运行（上一轮尚未结束），请稍后重试"）。用 RAII 而不是裸原子标志：任何提前 return/异常都释放，不留假锁 |
| `src/adapters/http/router.h:61` | `GcCallbacks`（`run` 回调 + `partition` + `scheduled`）——适配层不认识 L4/L2（R12） |
| `src/adapters/http/router.cpp:69` | `RouteAuthTable` 的 `{"ops.gc_run", kAdminNoPartition}`（`{roles, false}`，与 `revokeURL` 同型） |
| `src/adapters/http/router.cpp:525` | 路由注册：`server.Post(base + "/v2/gc:run", MakeRoute("ops.gc_run", kSmallBodyLimit), ...)`；`?dryRun` 只认 `true`/`1`，其余值不改变配置 |
| `src/adapters/http/dto/dto.h:193` / `dto.cpp:131` | `GcRunResponse` + `ToJson`（snake_case；`dry_run` 是**有效值**） |
| `src/main/server_main.cpp:1844` | 组合根：`gc_schedule` 提前判定 → `gc_callbacks.{partition,scheduled,run}`；`run` 里 `options.dry_run = gc_options.dry_run \|\| force_dry_run`，并写审计 `gcRun`（失败/`errors>0` → `result=failure`）；`:1906` 把回调交给 Router；`:1955` 复用同一个 `gc_schedule` 起调度 |
| `tests/integration/test_gc_endpoint.cpp` | 真实 `build/bin/fss_server`：6 用例 / **208 断言** |
| `tests/unit/test_gc_task_single_flight.cpp` | L4 单测：2 用例 / **12 断言** |
| `tests/framework/fake_ports.h` | ★ 新增 `BlockingTempSweepBlobStore`：把 `remove_temp_files` 变成一扇门（`WaitEntered()`/`Release()`），让"确实还在跑"成为**确定性**判据，而不是 sleep 猜时序 |

### 12.3 实测命令与输出摘要

```
$ cmake --build build -j4
[100%] Built target test_config_wiring

$ ctest --test-dir build -j4
100% tests passed, 0 tests failed out of 81
  phase9         =   3.52 sec*proc (8 tests)      # C9.31 前是 6 tests
  phase10        =  15.35 sec*proc (4 tests)

$ ctest --test-dir build -L phase10 --output-on-failure
100% tests passed, 0 tests failed out of 4
  (test_config_wiring / test_remote_validators / test_webhook_publisher / test_operations_doc)

$ ./scripts/check_docs.sh --selftest
  ✓ 自证：D1/D2/D4/D5 都能检出注入的错误（检查器有效）
  D5 门槛编号检查：11 个阶段，共 146 条门槛           # C9.31 前是 145
  全部检查通过（D1~D5）

$ ./scripts/verify_config_wiring.sh
  配置面接线：全部通过（54 条断言）

$ ./build/bin/test_operations_doc
All tests passed (24 assertions in 2 test cases)     # 156 键三态 106/19/31 未变

$ ./build/bin/test_gc_endpoint
All tests passed (208 assertions in 6 test cases)

$ ./build/bin/test_gc_task_single_flight
All tests passed (12 assertions in 2 test cases)
```

**逐条判据的实测**（`test_gc_endpoint.cpp`）：

| 判据 | 断言要点（**真实物理路径**） |
| --- | --- |
| ① 正例（R16） | `gc.enabled=false` + `gc.dry_run=false` + 够旧 `.tmp.*`（`<root>/blobs/opendes-staging/residue...`，`utime` 改到 3 天前）→ `200`；`dry_run=false`、`tmp_removed>=1`、`tmp_skipped_too_young>=1`、`errors=0`、`partition=opendes`、`scheduled=false`；**`REQUIRE_FALSE(exists(residue))`**（真的消失），同一条路径上 **正控** `REQUIRE(exists(fresh))`（太新的仍在）+ 调用前 `REQUIRE(exists(residue))` |
| ② 干跑安全 | `gc.dry_run=true` → `200` + `dry_run=true` + `tmp_removed>=1`（报候选）+ **文件仍在**；`gc.dry_run=false` + `?dryRun=true` → 有效 `dry_run=true` + **文件仍在**（同一实例不带参数时确实删掉，作为对照） |
| ③ 授权矩阵 | 无 token → `401`；只有 `service.file.editors` → `403`；`service.file.admin`（**不带** `data-partition-id`）→ `200`；无 token 但带 partition → 仍 `401` |
| ④ 审计 | 成功：日志含 `"operation":"gcRun"` + `"user":"gc-operator@example.com"` + `"result":"success"`；失败（删掉 persistent 容器目录 → `errors>=1`）：`"result":"failure"` + 同一 actor |
| ⑤ scheduled | `gc.enabled=false` → `scheduled=false` 且端点可用；`gc.enabled=true` → `scheduled=true`（横幅 `gc : 已启动`） |
| ⑥ 指标 | 调用前 `/metrics` **没有** `fss_gc_runs_total`（`gc.enabled=false`，调度不跑 → 计数只能来自端点）；调用后**轮询**到 `fss_gc_runs_total{...}` 非 0 |

### 12.4 R1 自证（注入 → 用例失败 → 还原）

**注入 ①：去掉单飞护栏**（`gc_task.cpp` 的 `try_lock` 分支改成无条件 `lock()`）→ 单飞用例必须失败：

```
$ cmake --build build -j4 && ./build/bin/test_gc_task_single_flight
tests/unit/test_gc_task_single_flight.cpp:98: FAILED:
  REQUIRE( returned_while_first_round_running )
with expansion:  false
with messages: returned_while_first_round_running := false
               b_ok := true
               a_done.load() := false
tests/unit/test_gc_task_single_flight.cpp:152: FAILED:
  REQUIRE( returned )
with expansion:  false
test cases: 2 | 2 failed
assertions: 3 | 1 passed | 2 failed
```

即：没有护栏时第二次调用会**排队**（窗口内没返回，`b_ok=true` 说明它最终成功了）——判据有区分力。

**注入 ②：忽略请求的 `?dryRun=true`**（组合根 `options.dry_run = gc_options.dry_run;`）→ 干跑用例必须失败：

```
$ cmake --build build -j4 && ./build/bin/test_gc_endpoint "★ C9.31：dry-run*"
tests/integration/test_gc_endpoint.cpp:301: FAILED:
  REQUIRE( forced_json.value()["dry_run"] == true )
with expansion:  false == true
with messages: forced.status := 200
  forced.body := {..."dry_run":false,..."tmp_removed":1,...}
test cases:  1 | 0 passed | 1 failed
assertions: 51 | 50 passed | 1 failed
```

（**注意**：同一用例的第一个 SECTION「配置 `dry_run=true`」在注入下**仍然通过** —— 它不是这条判据的
区分点；真正有区分力的是第二个 SECTION「配置 `false` + `?dryRun=true`」。已如实记录。）

**注入 ③：端点不做授权**（`RouteAuthTable` 里 `{"ops.gc_run", kAdminNoPartition}` → `{"ops.gc_run", {}}`，
即免鉴权）→ 授权矩阵必须失败：

```
$ cmake --build build -j4 && ./build/bin/test_gc_endpoint "★ C9.31：授权矩阵*"
tests/integration/test_gc_endpoint.cpp:325: FAILED:
  REQUIRE( reply.status == 401 )
with expansion:  200 == 401 (0x191)
test cases:  1 | 0 passed | 1 failed
assertions: 15 | 14 passed | 1 failed
```

**还原证据**：

```
$ grep -rn "R1-INJECT" src/ tests/        # 无输出（rc=1）
$ ./build/bin/test_gc_task_single_flight  → All tests passed (12 assertions in 2 test cases)
$ ./build/bin/test_gc_endpoint            → All tests passed (208 assertions in 6 test cases)
```

⚠️ **每次都做了全量重建**（`cmake --build build -j4`）：用例拉起的是 `build/bin/fss_server`，
只重建测试目标注入不进被测二进制（phase10 §11.4.1 的教训）。三次注入**第一次就都失败**，
没有出现"注入后仍全绿"的判据缺口。

**本轮抓到的一个**真实**缺陷（判据之外的收获）**：写运维文档时在 `operations.md` §1.3 里
顺手举了一个**不存在**的配置键做例子（`gc.endpoint_enabled`）→ `test_operations_doc` 的
反向检查（"文档里出现的配置路径必须能被 `CoreSchema().IsAllowedPath()` 接受"）**立刻失败**：

```
$ ./build/bin/test_operations_doc
tests/unit/test_operations_doc.cpp:187: FAILED: REQUIRE( unknown.empty() )
with messages: operations.md 写了但 example/schema 中不存在的配置路径（共 1）
```

改成"不引入任何『端点开关』之类的新键"（不含点分路径）后 24 断言全过。这正是
phase10 §12.5 想说的：**判据有效时，它会在你写错的那一刻说话**。

### 12.5 未做 / 未验证（如实登记）

1. **无异步 / 进度查询**：端点同步等待整轮结束才返回；**没有** job id、进度、取消。
   大目录（百万级 `.tmp.*`）下这一轮可能很久（ADR-013 §10.7）。
2. **无 rate limit / 并发配额**：单飞护栏只保证"同时最多一轮"，不限制"每分钟点几次"。
3. **无 per-partition 授权细化**：`service.file.admin` 是全局角色；分区由组合根单租户决定
   （`opendes`），**不是**调用方通过 `data-partition-id` 指定。多租户下"只允许自己租户"需要
   分区级角色 + `data-partition-id`，本切片**不做也不假装做了**。
4. **`errors > 0` 仍回 200**：报告携带 `errors`，审计记 `failure`；未定义"部分失败"的 HTTP 语义。
5. **`deleted_file_ids` 未进响应**（避免响应体随删除规模线性膨胀）；需要逐条清单查审计/日志。
6. **未与真实大目录联调**：全部用例都在临时目录（个位数文件）上跑；未测百万级目录的耗时/内存，
   也未测"上游网关超时切断后服务端仍把这一轮跑完"的行为。
7. **未做 ASan 第二遍**：本切片是"路由 + 回调 + 互斥量"的常规路径，未改内存/生命周期敏感代码；
   `scripts/run_sanitizers.sh` 由父代理在收尾时按常规门槛跑（phase0~7）。
8. **发现的一处既有不一致（不在本切片范围，如实登记）**：`auth.mode=disabled` 下组合根的
   allow-all **占位**授权器仍要求非空 `partition`，因此 `revokeURL` 与 `gc:run` 在
   `disabled` 模式下不带 `data-partition-id` 会得到 `401 Missing partitionID`
   （`LocalJwtAuthorizer` 在 `jwt` 模式下**不会**这样判）。契约 §1.2 说 `revokeURL` 不需要
   partition，这条不一致在 P8 引入占位实现时就存在，**本切片不改它**（改了会动 P8 的语义），
   只在 runbook §4.1 与契约 §7.3 注明"生产必须是 `jwt`；`disabled` 仅开发/测试"。

### 12.6 父代理独立复核（不是转述实现者）

```
cmake --build build -j4                    → 0 error
ctest --test-dir build -j4                 → 100% tests passed, 0 failed out of 81
./scripts/check_docs.sh --selftest         → 自证 ✓；13 ADR / 146 门槛 / D1~D5 全过
grep -rn "R1-INJECT" src/ tests/           → 无输出
git status --porcelain config/             → 空（**确认未新增任何配置键**，三态 106/19/31 不变）
./scripts/run_all_gates.sh                 → 失败 无（P0~P10）
```

**我自己重做了一次注入**（单飞护栏是本切片的实质新增性质，最值得独立验证）：把
`GcTask::Run` 的 `std::unique_lock(run_mutex_, std::try_to_lock)` + `if (!owns_lock())` 改成
**排队**（`std::unique_lock(run_mutex_)` + `if (false)`）→

```
$ ./build/bin/test_gc_task_single_flight
tests/unit/test_gc_task_single_flight.cpp:152: FAILED:
  REQUIRE( returned )
with expansion:  false
test cases: 2 | 2 failed      assertions: 3 | 1 passed | 2 failed
# 还原后：All tests passed (12 assertions in 2 test cases)
```

即：该判据真的依赖"同时只允许一轮"这一性质（不是恒真）；且注入成"排队"时用例**会结束**而不是
挂死（说明它断言的是"第二次调用必须立刻被拒"，不是靠超时）。

**两处既有不一致的核对**（实现者如实登记、未擅自更改，我已读代码确认描述准确）：

1. `auth.mode=disabled` 用的 `AllowAllAuthorizer`（`src/main/server_main.cpp:705~715`）在
   `partition` 为空时返回 `kUnauthenticated "Missing partitionID"` → `disabled` 模式下不带
   `data-partition-id` 会 **401**（`jwt` 模式不会）。本切片**不改**它（改 `disabled` 的语义
   超出范围），已在契约 §7.3 与 `runbook.md` §4.1 注明"生产必须 jwt"。
2. `errors>0` 仍返回 **200**（报告携带错误计数、审计记 `result=failure`）——这是"运维端点"
   的合理选择（部分失败要能看到报告），已写进契约与 §12.5。

**P10 收口**：本切片使计划里的 P10 判据 C10.1~C10.19 **全部满足**（`docs/04-implementation-plan.md`
的 P10 行与 §「阶段 10」状态行均标 ✅；`check_docs.sh` 的 D4 确认"计划 ✅ 集合 ⇄
`IMPLEMENTED_PHASES`"一致：`已完成 ['0'..'10']`、`进行中 无`）。**156 键里仍有 31 个
「已读但无效果」**——它们不是接线遗漏，而是**依赖尚未交付的能力**（PG/multi、远端 Storage
Service 仓储、ADR-006 数据面、ADR-008 P4），逐键理由与下一步登记在 `docs/operations.md` §1.3.3；
按 C10.11 的判据原文，这属于允许状态（必须给出理由与下一步）。

---

## 13. P9 补交（ADR-008 的 P4 / C9.20 / C9.23）：两阶段批提交的**真实实现**

> 本节的命令、输出与注入都是本轮实测，不是转述。被测二进制：`build/bin/test_posix_batch_commit`
> 与 `build/bin/test_config_wiring`（后者拉起真实 `build/bin/fss_server`）。

### 13.1 结论（先说答案）

1. **`storage.posix.durability=batch` 从"近似"改成真话**：此前组合根把它映射成
   `FsyncPolicy::kBySize`（语义 = "小于阈值的文件**不 fsync**，靠批提交摊销"），而**批提交不存在**
   ⇒ 默认配置（`config/fss.example.json` 的 `durability: "batch"`）下小文件**从不落盘**。
   现在它是 ADR-008 的 P4：`write all tmp → syncfs → rename all → fsync(dir)`。
2. **摊销判据是确定性的**（不靠 sleep）：N=8 并发 `put`、批上限 C=4 → `syncfs == 2`（`ceil(8/4)`）；
   C=1 → `syncfs == 8`（退化即"每文件一次"）；`durability=per_file` → 0 次 `syncfs`（走 `data_sync`）。
3. **顺序不变量（C9.23）可机器检查且能失败**：任何 `rename` 都不在第一次 `syncfs` 之前；
   每批 `fsync_dir` 在所有 `rename` 之后。三个 R1 注入都让对应用例失败（§13.4）。
4. **诚实偏差**：本实现是**并发驱动**的组提交，ADR-008 的 31,478 文件/秒 / 82.3x 是**显式批量写**
   场景的协议研究数字，**不迁移**——顺序单文件写仍是一文件一次提交（`fss_posix_syncfs_total`
   与 `fss_posix_group_commits_total` 之比 ≈ 1 可观测）。

### 13.2 实现点（可点击）

| 文件 | 内容 |
| --- | --- |
| `src/infra/io/file_sync.h` / `file_sync.cpp` | `IFileSync::SyncFilesystem(dir)`（生产 = `RealFileSync` → `syncfs(2)`），与既有 `DataSync`/`SyncDirectory` 同族的**窄注入接缝** |
| `src/infra/blob/posix/posix_blob_store.h` | `IBatchCommitObserver`（记录 `write_tmp`/`data_sync`/`syncfs`/`rename`/`fsync_dir`）、`IBatchArrivalGate`（测试门控）；`PosixBlobStoreOptions::{batch_commit,group_commit_max_batch,sync_dir_after_batch,batch_observer,batch_gate,metrics}` |
| `src/infra/blob/posix/posix_blob_store.cpp` | `JoinBatch`（并发组提交：入批 → 第 1 个入批者为领队等 `kBatchWaitWindow=2ms` 或批满；**填满者**成为提交者，保证不超过上限）、`CommitBatch`（② `syncfs` → ③ 统一 `rename` → ④ `fsync(dir)`；`syncfs` 失败 → 整批不 rename；单条 rename 失败 → 只该对象失败）、`WriteTmpFile`（把 sidecar 也纳入同批） |
| `src/main/server_main.cpp` | `durability=batch → batch_commit=true`；`group_commit_max_batch` 生效；`sync_dir_after_batch=false` **仍拒绝启动**（R2 不变量）；启动横幅新增 `durability` 行（粒度 + 批上限）；注册 `fss_posix_{syncfs,group_commits,batch_objects}_total` |
| `tests/integration/test_posix_batch_commit.cpp` | 6 个用例：摊销 == ceil(N/C) / 顺序不变量 / 数据正确性 + 无 `.tmp_` 残留（配**正控**）/ 阈值例外 / `atomic_write=false` / 失败路径 |
| `tests/integration/test_config_wiring.cpp` | 更新 C10.16 续的守卫用例（`group_commit_max_batch=7` 现在 `--print-config` 退出 0；`sync_dir_after_batch=false` → exit 78 且消息含 `R2`）+ 新增 C9.23 真实进程用例（默认 batch 上传 200 且 `/metrics` 的 `fss_posix_syncfs_total >= 1`） |

### 13.3 实测命令与输出摘要

```
$ cmake --build build -j4
  → 0 error（全量重建；测的是 `build/bin/fss_server` 与 test 二进制，不是只重建某个 target）

$ ./build/bin/test_posix_batch_commit
  → All tests passed (128 assertions in 6 test cases)

$ ./build/bin/test_config_wiring "★ C9.23*"
  → All tests passed (25 assertions in 1 test case)
    （其中真实进程 `/metrics` 三段：fss_posix_syncfs_total / group_commits_total /
      batch_objects_total；runbook 报告里可见上传后 syncfs=2、commits=2、objects=2 ——
      顺序单文件写下"平均批大小 = 1"正是 §13.1 的诚实偏差）

$ ctest --test-dir build -L phase3
  → 100% tests passed, 0 tests failed out of 10（含新 test_posix_batch_commit）
```

`/metrics`（真实进程）片段：

```
# HELP fss_posix_batch_objects_total 通过两阶段批提交提交的对象数（用于计算平均批大小）
# TYPE fss_posix_batch_objects_total counter
fss_posix_batch_objects_total 2
# HELP fss_posix_group_commits_total POSIX 批提交的批次数（每批 1 次 syncfs + 1 次 fsync(dir)）
# TYPE fss_posix_group_commits_total counter
fss_posix_group_commits_total 2
# HELP fss_posix_syncfs_total POSIX 批提交中 syncfs(2) 的调用次数（ADR-008 的 P4）
# TYPE fss_posix_syncfs_total counter
fss_posix_syncfs_total 2
```

> ⚠️ **踩到并修掉的判据陷阱**：第一次写真实进程断言时用 `find("fss_posix_syncfs_total ")`
> 解析数值，先命中的是 `# HELP fss_posix_syncfs_total POSIX ...`（帮助文本），`strtol` 返回 **0**
> → 判据误报失败。改成解析**样本行**（`"\nfss_posix_syncfs_total "`）后通过。这类"解析到了
> HELP 行而不是样本行"与 phase10 §11.4.1 的"两条防线重合"同族，记在这里备用。

### 13.4 R1 自证（3 个注入 → 对应用例失败 → 完整还原）

每个注入都**全量重建**后再跑；还原后 `grep -rn "R1-INJECT" src/ tests/` **无输出（rc=1）**。

**① 把 `syncfs` 挪到 `rename` 之后**（违反 R1 顺序不变量）→ 顺序判据必须失败：

```
$ ./build/bin/test_posix_batch_commit "★ C9.23 顺序不变量*"
/home/ll/fssvrcpp/tests/integration/test_posix_batch_commit.cpp:284: FAILED:
  REQUIRE( events[i].op != "rename" )
with expansion:
  "rename" != "rename"
with messages:
  事件序列:
  i := 16
  events[i].op := "rename"
  events[i].path := ".../o_4.bin.tmp.local.1100754.5 -> .../o_4.bin"
test cases:  1 |  0 passed | 1 failed
assertions: 28 | 27 passed | 1 failed
```

**② 让批大小上限被忽略**（`max_batch = 1`，退化成每文件一次提交）→ 摊销判据必须失败：

```
$ ./build/bin/test_posix_batch_commit "★ C9.23 摊销*"
/home/ll/fssvrcpp/tests/integration/test_posix_batch_commit.cpp:204: FAILED:
  REQUIRE( sink.Count("syncfs") == 2 )
with expansion:
  8 == 2
test cases:  1 |  0 passed | 1 failed
assertions: 28 | 27 passed | 1 failed
```

**③ 让阈值例外被忽略**（batch 档下大对象也走批、不单独 `fdatasync`）→ 阈值判据必须失败：

```
$ ./build/bin/test_posix_batch_commit "★ C9.23 阈值例外*"
/home/ll/fssvrcpp/tests/integration/test_posix_batch_commit.cpp:412: FAILED:
  REQUIRE( sink.Count("data_sync") == 1 )
with expansion:
  0 == 1
test cases:  1 | 1 failed
assertions: 6 | 5 passed | 1 failed
```

**还原后的实测**：

```
$ grep -rn "R1-INJECT" src/ tests/         # 无输出（rc=1）
$ ./build/bin/test_posix_batch_commit       # All tests passed (128 assertions in 6 test cases)
```

> **第一次"全绿"记录**：三个注入都是**第一次就红**（分别命中 §13.4 的三条断言），没有出现
> phase10 §11.4.1 那种"注入后仍全绿"的判据缺口。顺序判据在实现前特意加了两条互相独立的编码
> （"所有 `write_tmp` 早于第一次 `syncfs`" + "第一次 `syncfs` 前不得有任何 `rename`"），
> 因此注入 ① 无法从任何一条缝里溜过去。

### 13.5 未做 / 未验证（如实登记）

1. **真实断电 / 崩溃的耐久性（R1/R2 在断电下成立）：未验证（R4/R8）**。本环境**无 root、
   不能 `mount`、无电源故障注入**（`dm-log-writes`/`dm-flakey`/VM 快照均不可用）。已验证的是
   **顺序不变量**（机器可检查，§13.4）——它是 ADR-008 §4.2 的**主要依据**，但**不是**断电实验。
2. **C9.24（`syncfs` 全局 flush 对他人的影响）：未验证**。无多租户共盘场景；缓解方向（按
   partition 分盘）依赖的 `storage.posix.one_filesystem_per_partition` **仍未接通**。
3. **吞吐数字不迁移 / 未重测**：ADR-008 的 31,478 文件/秒、82.3x、383 文件/秒来自**显式批量写**
   场景；本实现是并发驱动组提交。P9 的 `batch` 基线值（410.1 / 468.6）是在 P4 交付**之前**用
   `kBySize` 近似测的 → 已在 `docs/operations.md` §5.2、`docs/02-design.md` §13.4、
   `docs/05-capacity-and-concurrency.md` §1.9 就地标**作废**；本切片**未**跑 `bench_baseline.sh --save`
   （会覆盖基线；环境漂移已知）。
4. **`/v2/info` 未暴露耐久性粒度**：C9.20 原文要求"在 `/v2/info` 与运维文档中显式声明"。本轮把
   粒度与批上限写进了**启动横幅**与 `docs/operations.md` §5.1（真实进程可见），但**没有**给
   `/v2/info` 加字段——加字段会动契约 §7 的响应形状与等价性断言，超出"把默认耐久性语义修成真话"
   的定案范围。**登记为未做**。
5. **批窗口敏感性（推荐默认值与上限）未做**：`kBatchWaitWindow=2ms` 是具名常量（`storage.posix`
   没有 wait 键，不新增配置键）；只在"批大小上限"一维有确定性判据（N=8/C=4 → 2；C=1 → 8）。
6. **`copy` 路径不参与批**：`batch_commit=true` 时 `copy` 按"单文件落盘"处理（安全方向），
   因此 copy 拿不到摊销；已在 ADR-008 §7.2 登记。

### 13.6 三态计数（ADR-008 的 P4 收尾）

`storage.posix.group_commit_max_batch` 从「拒绝启动」移入「生效」（+1）；
`storage.posix.sync_dir_after_batch` **保持拒绝启动**（`false` → exit 78，理由 = R2 **不变量**，
不是"未实现"）。三态：**生效 107 / 拒绝启动 18 / 已读但无效果 31 = 156**。
**未新增/删除任何配置键**（`git status --porcelain config/` 为空）。

### 13.7 父代理独立复核（自己重做顺序不变量注入）

```
cmake --build build -j4        → 0 error
ctest --test-dir build -j4     → 100% tests passed, 0 failed out of 82（81 → 82）
check_docs.sh --selftest       → 13 ADR / 146 门槛 / D1~D5 全过
git status --porcelain config/ → 空（**未新增/删除任何配置键**；三态 107/18/31）
grep -rn R1-INJECT src/ tests/ → 无输出
```

**第一次注入不干净，我如实记录并重做**：先把 `CommitBatch` 里的 `syncfs` 挪到 rename 之后，但**忘了同时挪走观察者事件**，于是每批记录到 **两个** `syncfs` 事件 → 失败的是**摊销计数**断言
（`REQUIRE(sink.Count("syncfs") == 2)` 等 5 条），**不是**顺序断言。那样的注入只能证明"计数判据有牙齿"。

**干净注入**（唯一一个 `syncfs` 事件，但发生在 `rename` 之后）：

```
$ ./build/bin/test_posix_batch_commit
tests/integration/test_posix_batch_commit.cpp:284: FAILED
test cases:   6 |   5 passed | 1 failed
assertions: 122 | 121 passed | 1 failed
# 还原后：All tests passed (128 assertions in 6 test cases)
```

失败的正是**顺序不变量**那一条：判据按 `syncfs` 位置切批，然后要求"每批 syncfs 之后
`renames > 0`" —— `syncfs` 后移后该窗口内 **0 个 rename**，`REQUIRE(renames > 0)` 失败
（`test_posix_batch_commit.cpp:284`）。即：**ADR-008 §5 的 R1（所有 rename 在 syncfs 之后）
是被真正机器检查的**，而且它与摊销计数是**两条互相独立**的判据（一个坏了不会掩盖另一个）。

**R2 不变量未被放宽**（已核对代码）：`src/main/server_main.cpp:1331` 仍对
`storage.posix.sync_dir_after_batch=false` 拒绝启动（ADR-008 §5 的 R2：rename 之后必须
fsync 目录），**没有**为了多一个"生效"键去放宽它 —— 这是本切片刻意保留的拒绝项。

---

## 14. P9 补交（P10 期间完成）：未预期异常必须以**干净退出码**结束（C9.32）

> **指针**：判据编号 **C9.32**（`docs/04-implementation-plan.md` 的 P9 段）；故障接缝的运维登记
> 在 `docs/runbook.md` §10；容器侧真实回归见 `docs/test-evidence/phase9-image.md` §10.12；
> HTTP 包装层的新增偏离见 `docs/adr/ADR-002-http-framework.md` §4.1 的 **H-7**。

### 14.1 结论（先说答案）

* **顶层兜底**：`main()` 变成**薄包装**（函数体搬进 `static int RunServer`），外面套
  `catch (const std::exception&)` + `catch (...)`：
  * 退出码 **70（EX_SOFTWARE）**，消息 `未预期异常（exit 70）：<what()>` + 一行静态提示；
  * **不复用 78**（那是"配置/部署形态非法"，会把运维引向改配置）与 **2**（命令行用法错误）；
  * 既有退出码逐字不变：配置非法 **78**、未知 CLI 参数 **2**。
  这一层**不做清理** —— 所有资源在 `RunServer` 的局部对象里，栈展开时 RAII 完成
  （`GcScheduler::~GcScheduler` → `Stop()+join`、`http::Server::~Server` → `Stop()`、
  gRPC 句柄 → `Shutdown()`），实测 `throw_after_start` 的墙钟退出 < 0.1 s。
* **故障注入接缝**：环境变量 `FSS_STARTUP_FAULT_INJECT`（**不是配置键**，三态 107/18/31 不变），
  取值 `throw_system_error` / `throw_bad_alloc` / `throw_unknown`（早注入点：CLI 解析之后、装配之前）
  与 `throw_after_start`（晚注入点：服务器已 `Start()`、GC 调度线程已在跑之后）。
* **★ 根因（本轮最重要的发现）**：**只加顶层 catch 修不了这条真实缺陷**。
  容器 `--pids-limit=64` 的实测结果是 **仍然是 `ExitCode=139` + `terminate`**（第一次 H4a 复跑
  如实记录），因为 64 个工作线程是 `httplib` 在 **runner 线程**里（`listen_internal()` →
  `new_task_queue()`）创建的：
  1. `httplib::ThreadPool` 用 `threads_.emplace_back(...)` 逐个建线程；某次 `pthread_create`
     返回 `EAGAIN` 时，**已建好的 joinable `std::thread` 向量在栈展开中被析构** → `terminate`；
  2. 该 terminate 发生在**非主线程**，`main()` 的顶层 catch 看不到。
  修法（`fss_http` 包装层，ADR-002 H-7）：`SafeThreadPool`（失败先 `join` 已建线程再重抛）
  + `Server::Start()` 把 runner 线程的异常**在主线程重抛**；并且只等 `pool_ready`
  （`new_task_queue()` 成功返回）才算启动成功，消除与 `is_running_` 的竞态挂死。
* **实测**：容器 `--pids-limit=64` → **`ExitCode=70` + 可读「未预期异常」、无 `terminate`、
  `OOMKilled=false`**；`--pids-limit=66` 仍正常启动（下界未变）。

### 14.2 实现点（可点击）

| 位置 | 内容 |
| --- | --- |
| `src/main/server_main.cpp:94` | `constexpr int kExitInternalError = 70;`（注释写明为什么不复用 78/2） |
| `src/main/server_main.cpp:139` | `struct StartupFaultUnknown {}`（非 std 类型，唯一用途是驱动顶层 `catch (...)`） |
| `src/main/server_main.cpp:163` / `:172` | 早/晚注入点：`MaybeInjectStartupFaultEarly()` / `MaybeInjectStartupFaultAfterStart()` |
| `src/main/server_main.cpp:1055` | 早注入点调用（CLI 解析之后、配置加载/装配之前） |
| `src/main/server_main.cpp:2220` | 晚注入点调用（HTTP 已 Start、GC 调度已 Start 之后） |
| `src/main/server_main.cpp:1032` | `static int RunServer(...)`（原 `main()` 全文，函数体逐字不变） |
| `src/main/server_main.cpp:2253` | `main()` 薄包装（`catch (const std::exception&)` + `catch (...)`） |
| `src/common/http/server.cpp:568` | `SafeThreadPool`：任一 `emplace_back` 失败 → `StopAndJoin()`（join 已建线程）→ 重抛 |
| `src/common/http/server.cpp:641`/`:642` | `Impl::start_mutex` / `start_error`（runner 线程的异常回到主线程） |
| `src/common/http/server.cpp:648` | `Impl::pool_ready`（`new_task_queue()` 成功返回后才置位） |
| `src/common/http/server.cpp:1247` | `new_task_queue` 工厂改用 `SafeThreadPool` + 置位 `pool_ready` |
| `src/common/http/server.cpp:1336` | `Server::Start()`：捕获 runner 异常并在主线程重抛；等 `pool_ready` 而非 `is_running` |
| `tests/integration/test_startup_faults.cpp` | 真实 `build/bin/fss_server`：**7 用例 / 42 断言** |
| `tests/CMakeLists.txt`（`test_startup_faults`） | `LABELS "phase9;integration"` + `add_dependencies(... fss_server)` |
| `tests/framework/server_process.h` | `RunServerForExit` 的 `timeout` 增加 `--kill-after=5`（挂死时强制收尸，不再拖到 CTest 300 s） |
| `scripts/verify_image.sh`（H4a / H4a2） | H4a 从"记录真实结果"升级为**断言**（`ExitCode==70` + stderr 含「未预期异常」+ 无 `terminate called` + `OOMKilled=false` + 未进服务状态）；H4a2 = pids=66 **正控** |

### 14.3 实测命令与输出摘要

```
$ cmake --build build -j4
[100%] Built target test_startup_faults

$ ctest --test-dir build -j4
100% tests passed, 0 tests failed out of 83
  phase9         =   3.65 sec*proc (9 tests)      # C9.32 前是 8 tests

$ ./build/bin/test_startup_faults
All tests passed (42 assertions in 7 test cases)

$ ./scripts/check_docs.sh --selftest
  ✓ 自证：D1/D2/D4/D5 都能检出注入的错误（检查器有效）
  D5 门槛编号检查：11 个阶段，共 147 条门槛           # C9.32 前是 146
  全部检查通过（D1~D5）

$ ./build/bin/test_operations_doc
All tests passed (24 assertions in 2 test cases)     # 156 键三态 107/18/31 未变

$ ./scripts/verify_image.sh
-- [H4a] 资源上限（任务原文形态）：--memory=128m --pids-limit=64 → 期望 exit 70 + 可读原因
   readiness=000  ExitCode=70  OOMKilled=false
   日志: 未预期异常（exit 70）：Resource temporarily unavailable
   ✅ ExitCode=70（EX_SOFTWARE；不再是 terminate/139）
   ✅ stderr 含「未预期异常」（异常被顶层接住，原因可读）
   ✅ stderr **不含** terminate called
   ✅ OOMKilled=false（确实不是内存不足）
   ✅ readiness=000（未进入服务状态，符合预期）
-- [H4a2] pids 下界正控：--pids-limit=66 必须正常启动（下界=66 未变）
   pids=66 → readiness=200  ExitCode=0  terminate=0
   ✅ pids=66 readiness=200（下界=66 未变）
...
C9.8 + 容器硬化 ✅ 全部断言通过
$ echo $?
0
```

**逐条判据的实测**（`test_startup_faults.cpp`；每条都能因注入而失败）：

| # | 判据 | 断言要点 |
| --- | --- | --- |
| ① | 注入 `throw_system_error`（复刻 pids EAGAIN） | `exit_code == 70`；输出含「未预期异常」+「Resource temporarily unavailable」+「pids-limit」；**不含** `terminate` / `what():` / `config sources` / `secret`；**不含**「已启动」（未进入服务状态） |
| ② | 注入 `throw_bad_alloc` | `exit_code == 70` +「未预期异常」+「bad_alloc」，无 `terminate`，无「已启动」 |
| ③ | 注入**非 std** 类型 | `exit_code == 70` +「未知类型」+「未预期异常」，无 `terminate`（证明 `catch (...)` 生效） |
| ④ | **不注入**（R16 正例） | readiness **200** + body `File service is ready`；日志含「fss_server 已启动」（**正控**）且**不含**「未预期异常」 |
| ⑤ | `throw_after_start`（服务器已起 + GC 调度已跑后抛） | `exit_code == 70`；**墙钟 < 5 s**（实测 < 0.1 s，证明 RAII stop/join 不卡）；无 `terminate`；横幅含「fss_server 已启动」与「gc : 已启动」（证明晚注入点确实在启动之后） |
| ⑥ | **真实**线程创建 EAGAIN（部分创建） | `ulimit -u` 压到"当前数 + 500"，`worker_threads=20000` ⇒ 线程池建到一半才 EAGAIN；`exit_code == 70` +「未预期异常」+「Resource temporarily unavailable」，无 `terminate`，无「已启动」 |
| ⑦ | 注入点在 CLI 解析之后 | `FSS_STARTUP_FAULT_INJECT=throw_system_error --help` → `exit_code == 0` + 用法，且**不含**「未预期异常」 |

### 14.4 R1 自证（5 个注入 → 对应用例失败 → 完整还原）

> 与 §11.4.1 的教训一致：**第一次实现"全绿"不代表判据有区分力**。本切片的"第一次"就是
> 一个真实反例 —— **只加顶层 catch 时容器 H4a 仍是 139**（见 §14.1），因此我追到了 runner
> 线程的根因，并补了用例⑥（真 EAGAIN）与注入④⑤。所有注入后都执行了**全量重建**
> （`cmake --build build -j4`），还原后 `grep -rn "R1-INJECT" src/ tests/` **无输出**。

**注入 ①：去掉顶层 catch**（`main` 直接 `return RunServer(...)`）→ `test_startup_faults`：

```
test cases:  7 |  2 passed |  5 failed
  test_startup_faults.cpp:137: FAILED  outcome.exit_code := 134
    outcome.output := "terminate called after throwing an instance of 'std::system_error'"
  test_startup_faults.cpp:253: FAILED  outcome.exit_code := 134
    ("真实线程创建 EAGAIN" 用例也是 134 + std::system_error terminate)
```
即：用例①②③⑤⑥全部失败 —— **异常逃出 main 就是 134/139**，判据有区分力。

**注入 ②：`kExitInternalError` 改成 0** → 用例①②③⑤⑥在退出码断言上失败：

```
test_startup_faults.cpp:137 / :160 / :175 / :227 / :253: FAILED  outcome.exit_code := 0
test cases:  7 |  2 passed |  5 failed
```
只有"不注入（正例）"和"--help"仍然通过 —— 正例的作用正在于此（证明 0 不是"恰好也满足"）。

**注入 ③：删掉 `catch (...)`** → 用例③⑤失败：

```
test_startup_faults.cpp:175: FAILED  outcome.exit_code := 134
  outcome.output := "terminate called after throwing an instance of '(anonymous namespace)::StartupFaultUnknown'"
test cases:  7 |  5 passed |  2 failed
```
`catch (const std::exception&)` 接不住非 std 类型 —— 这条证明第二层 catch 是**必需**的。

**注入 ④：把 `SafeThreadPool` 换回 `httplib::ThreadPool`**（保留顶层 catch 与 runner 捕获）→ 用例⑥失败：

```
test_startup_faults.cpp:267: FAILED
  outcome.exit_code := 124          # timeout 收尸（进程打印「监听失败」后挂死）
test cases:  7 |  6 passed |  1 failed
```
顶层 catch 全部就位、其余 6 条用例全过，**只有"真 EAGAIN"那条失败** —— 这正是"第一次
全绿会漏掉根因"的证据：合成注入（①~③）无法覆盖 runner 线程的 terminate。

**注入 ⑤：`Server::Start()` 退回"只等 `is_running()`"**（去掉 `pool_ready`）→ 用例⑥失败（3/3）：

```
test_startup_faults.cpp:269: FAILED  outcome.exit_code := 124
# 直连复现：headroom=4000 时主线程在 runner 建池期间看到 is_running()==true →
# 打印「fss_server 已启动」并进入 serve 循环 → 永久挂死（rc=124）
```
这条把"竞态导致偶发挂死"钉成确定性判据（headroom=500 时 3/3 复现）。

**还原证据**：

```
$ grep -rn "R1-INJECT" src/ tests/
（无输出；grep rc=1）
$ ./build/bin/test_startup_faults
All tests passed (42 assertions in 7 test cases)
```

### 14.5 未做 / 未验证（如实登记）

| 项 | 状态 | 说明 |
| --- | --- | --- |
| **其它线程里抛出的异常** | ⬜ **不在本切片范围** | 本切片只处理"顶层能接住的异常"与"runner 线程经由 `Server::Start()` 转交的异常"。任何**其它**工作线程（GC 调度、HTTP worker、gRPC）里逃出的异常仍会 `terminate` —— 修法要在每个线程入口各自 `try/catch`，属另一个话题（未做） |
| **`std::terminate` 仍可能出现在 `SafeThreadPool` 之外** | ⚠ 部分覆盖 | 已覆盖"线程池构造失败"这一条真实路径；`std::thread` 析构 joinable 的**其它**位置未逐一排查（本仓库"先 stop 再 join"的纪律在 §4.3 已登记） |
| **`--pids-limit` 的真实下界在新代码下重测** | ⚠ 部分 | 默认路径只断言 pids=64（→70）与 pids=66（→200）；**65 未在默认路径断言**（`FSS_VERIFY_IMAGE_FULL=1` 的 H4b2 仍会探 65/66）。下界"65 失败/66 成功"的**历史记载保留**，未在 C9.32 下逐点重测 |
| **K8s / 其它容器运行时的 pids 限制** | ⬜ 未验证 | 与 `docs/test-evidence/phase9-image.md` §10.8 同口径：只测了 Docker（cgroup v2 + pids controller） |
| **ASan/UBSan 下的这条路径** | ✅ **已覆盖**（父代理复核时更正） | 上一版这里写"未在 sanitizer 构建里跑（标签范围是 phase0~7）"——**理由与结论都不对**：`run_sanitizers.sh` 的标签是从 `run_all_gates.sh` 的 `IMPLEMENTED_PHASES` **推导**的，`IMPLEMENTED_PHASES` 早已含 10 ⇒ 实际覆盖 **phase0~phase10**。父代理在最终门槛里实测：覆盖标签 `phase0|phase1|…|phase10`，其中 **phase1 = 18 个测试**（`fss_http` / `SafeThreadPool` 所在层）、phase9 = 9 个测试（含 `test_startup_faults`），全部在 ASan+UBSan+LSan 下通过。（旧文"phase0~7 全绿"是 AGENTS 里的陈旧描述，已一并修正。） |

### 14.6 三态计数（未改配置面）

**未新增/删除/修改任何配置键**：`FSS_STARTUP_FAULT_INJECT` 是**环境变量接缝**（与
`FSS_AUDIT_FAULT_INJECT` 同族），不进入 schema/`config/fss.example.json`。三态仍为
**生效 107 / 拒绝启动 18 / 已读但无效果 31 = 156**（`test_operations_doc` 通过）。

### 14.7 父代理独立复核（含对规格与两处文档表述的更正）

**① 独立复跑**：`cmake --build build -j4` 0 error；`ctest` **83/83**；
`./scripts/verify_image.sh` **rc=0**，其中
```
-- [H4a] --memory=128m --pids-limit=64   readiness=000  ExitCode=70  OOMKilled=false
   ✅ ExitCode=70（EX_SOFTWARE；不再是 terminate/139）
   ✅ stderr 不含 terminate called
-- [H4a2] --pids-limit=66               readiness=200  ExitCode=0（下界=66 未变）
```
与我自己的日志 `build/parent-verify-image-c932.log` 逐项一致。

**② 我自己重做了"根因"那条注入**（本切片最关键的主张：**只加顶层 catch 不够**）：
把 `src/common/http/server.cpp` 的 `new SafeThreadPool(...)` 换回 `new httplib::ThreadPool(...)`
（顶层 catch 全就位）→ 真实 EAGAIN 用例**失败**：

```
$ ./build/bin/test_startup_faults "★ C9.32：真实线程创建 EAGAIN*"
  监听失败: 服务端未能在 1 秒内进入运行状态
test cases: 1 | 1 failed      assertions: 1 | 1 failed
# 还原后：All tests passed (5 assertions in 1 test case)
```
即：**包装层（`SafeThreadPool` + 把 runner 线程异常在主线程重抛）确实是修好这条缺陷的必要条件**，
不是"顺手改的"。这条独立复现写在这里，替代"实现者说它必要"的转述。

**③ 我的规格漏了根因（如实记录）**：我只要求"给 `main` 加顶层兜底"，没有意识到失败发生在
httplib 的 **runner 线程**里（`listen_internal()` → `new_task_queue()`），而 `httplib::ThreadPool`
在部分创建失败时会因析构 joinable `std::thread` 而 `terminate`——那是**线程内**的 terminate，
`main` 的 catch 看不到。实现者追到了这一层，并覆盖了 `is_running()` 与建池的竞态（只等 `pool_ready`）。
这正是"照规格做不够、必须追根因"的实例。

**④ 两处文档表述更正（父代理改）**：
- `AGENTS.md` 的"`run_sanitizers.sh`（ASan+UBSan+LSan，**phase0~7** 全绿）"是**陈旧**的：脚本的标签是从
  `run_all_gates.sh` 的 `IMPLEMENTED_PHASES` **推导**的，早已含 10 ⇒ 实际覆盖 **phase0~phase10**。
  已在门槛里实测：`覆盖标签： phase0|phase1|…|phase10`，其中 **phase1 = 18 个测试**（`fss_http` /
  `SafeThreadPool` 所在层）、phase9 = 9 个测试（含 `test_startup_faults`），ASan+UBSan+LSan 全绿。
  ⇒ §14.5 里"`SafeThreadPool` 未在 sanitizer 下跑"的登记**作废并已更正**（详见本轮 §14.5 的表格行）。
- `docs/04-implementation-plan.md` 的 P10 行与"当前为…"句、`docs/test-evidence/phase10.md` 的配置键三态
  表头，仍写着**当前值** 106/19/31 —— ADR-008 的 P4 交付后应为 **107/18/31**。已改（历史切片里的
  106/19/31 保留为当时事实，属上下文正确的历史记录）。

**⑤ 未做/未验证（本轮维持）**：其它工作线程（GC 调度线程、HTTP worker、gRPC 线程）里逃出的异常
仍会 `terminate`（不在本切片范围）；`--pids-limit=65` 未进默认断言（仅 FULL 的 H4b2）；
K8s/containerd 未验证。
