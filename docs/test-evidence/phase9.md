# 阶段 9 测试证据（🚧 进行中）

| 项 | 值 |
| --- | --- |
| 阶段 | P9（硬化与交付） |
| 状态 | 🚧 **进行中 —— 切片 1/3 完成 + 切片 2（指标/GC）完成**（并发 C9.1 + 故障注入 C9.2 + 资源上限 C9.3 + 指标 C9.6 + GC 清理 C9.5/C9.25） |
| 门槛命令 | `ctest -L phase9` |
| 退出码 | `0`（**5 测试 / 451 断言**：`test_concurrency` 92、`test_fault_injection` 79、`test_resource_limits` 44、`test_metrics_and_gc` 195、`test_transfer_error_semantics` 41） |

> **剩余**：切片 2 的**性能基线部分**（C9.4/C9.11/C9.13/C9.15）+ ADR-006 受控复核（C9.12/C9.16）；
> 切片 3 = `run_all_gates.sh` 纳入 P9（C9.7）+ 容器镜像（C9.8）
> + `operations.md`/`runbook.md`/`README.md`（C9.9）+ 11 项风险收口（C9.10）。
>
> ★ 本切片顺带修掉了 5 个**同族**缺陷（P9-D04~D08，见 §4）：全部是"**元数据/状态语义错误**"
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
| C9.4/C9.7/C9.8/C9.9/C9.10 | ⬜ 未开始 | 见开头"剩余" |
| C9.11–C9.16（性能基线/ADR-006） | ⬜ 未开始 | 见开头"剩余"（需独立进程 + 绑核的负载生成器） |
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

## 9. 未验证 / 环境限制（如实登记）

| 项 | 为什么未验证 | 已登记的替代证据 |
| --- | --- | --- |
| 指标口径的"生产级"校验（每秒采样、基数爆炸、远端抓取） | 需要真实 Prometheus/长跑流量 | 已断言"family 数有限 + 标签值受控 + 渲染稳定 + 不含 secret"；**未**做基数压测（登记） |
| `fss_gc_skipped_total{reason=tmp_too_young}` 在真实生产流量下的量级 | 需要真实在途上传并发 | 已有确定性用例（5 秒前的临时文件必须被保护且计数为 1） |
| 真实 ENOSPC（写满一个小文件系统） | 需要一个可写的小文件系统（`mount` 需要 root；tmpfs 也不例外） | 用注入的 ENOSPC 语义（`kUnavailable`）覆盖**行为**（含回滚），并断言状态码 |
| 真实 NVMe/HDD/NFS 上的延迟分布与容量数字（C9.14/C9.22） | 本机是虚拟盘（WSL2 语义），`docs/00-final-design.md` 已明确**不得作为判定依据** | 现有量级参考仍标注为"量级参考" |
| io_uring 在目标内核上的可用性（C9.18/C9.29/C9.30） | 需目标部署内核；本机默认 seccomp 阻断（ADR-010 已实测 `EPERM`） | `scripts/check_io_uring.sh`（退出码 2 = 无结论）+ `UringIoEngine` 骨架 |
| 多实例 + 共享 PG 端到端（C9.26）与 PG 租约 | PG 版仓储/租约未交付（P9 登记项） | `db/tests/00{1,2,3}` 的 PG 基建门槛（`FSS_GATES_WITH_PG=1`）+ `test_multi_instance`（内存/单进程） |
| 容器镜像（C9.8）与真实网卡吞吐（C9.14） | 本环境无 docker/真实网卡 | 待切片 3 评估（Dockerfile 至少静态可检查） |
