# 阶段 9 测试证据（🚧 进行中）

| 项 | 值 |
| --- | --- |
| 阶段 | P9（硬化与交付） |
| 状态 | 🚧 **进行中 —— 切片 1/3 完成**（并发 C9.1 + 故障注入 C9.2 + 资源上限 C9.3） |
| 门槛命令 | `ctest -L phase9` |
| 退出码 | `0`（**3 测试 / 215 断言**：`test_concurrency` 92、`test_fault_injection` 79、`test_resource_limits` 44） |

> **剩余**：切片 2 = 指标完整性（C9.6）+ GC 清理 `.tmp_*`（C9.5/C9.25）+ 性能基线（C9.4/C9.11/C9.13/C9.15）
> + ADR-006 受控复核（C9.12/C9.16）；切片 3 = `run_all_gates.sh` 纳入 P9（C9.7）+ 容器镜像（C9.8）
> + `operations.md`/`runbook.md`/`README.md`（C9.9）+ 11 项风险收口（C9.10）。
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
| C9.4/C9.5/C9.6/C9.7/C9.8/C9.9/C9.10 | ⬜ 未开始 | 见开头"剩余" |
| C9.11–C9.30（需真实硬件/多进程/容器） | ⬜ 未验证 | 环境不具备（详见 §5） |

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

## 4. 本切片发现的实现陷阱

| 编号 | 症状 | 根因 | 规避 |
| --- | --- | --- | --- |
| **P9-D01** | 100 并发用例直接 **`double free or corruption (out)` + SIGABRT**（一次都没跑到断言） | **测试替身不是线程安全的**：`AllowAllAuthorizer`（`calls`/`last_role`/`last_roles`）、`RecordingAuditLogger`（`events`/`calls`）、`RecordingEventPublisher`（`events`/`details`）、`FakeBlobStoreFactory`（`calls`/`last_partition`）在多个服务线程里被并发写 → 数据竞争 + 堆损坏。此前的"并发"用例只有 4~8 个线程、且审计是 P8 才全面铺开的，所以一直没暴露 | 给这些替身加 `std::mutex`（与 P6 修 `InMemory*Repository` 同一条纪律）；`SequentialIdGenerator` 的计数器改为 `std::atomic`（并发取 id 会拿到同一个 id → 唯一键冲突）；`test_multi_instance.cpp` 里对它的**赋值**改为 `Reset()`（原子成员不可赋值）。**生产代码**的 `HttpMetrics`/日志器/仓储本来就都有锁，本切片未发现生产侧竞争 |
| **P9-D02** | "磁盘满"用例第一次跑返回 **201**（故障没注入进去） | 两个 zone 指向**同一个** store 时，12 步序列走的是 `IBlobStore::copy()` 而**不是** `put()`；只在 `put()` 上注入故障就完全没生效 | 故障装饰器**同时**覆盖 `put()` 与 `copy()`（并把这个"同 store 走 copy"的路径写进注释）；用例的"注入必须真的生效"通过状态码断言钉住 |
| **P9-D03** | `RecordMetadata` 故障用例返回 201 | 替身的 `fail_create` 开关**默认 false**，用例忘了打开 | 注入类用例必须显式打开开关；"故障后的状态码"断言本身就是"注入生效"的证据（这也是为什么不能只断言"最终成功"） |

## 5. 未验证 / 环境限制（如实登记）

| 项 | 为什么未验证 | 已登记的替代证据 |
| --- | --- | --- |
| 真实 ENOSPC（写满一个小文件系统） | 需要一个可写的小文件系统（`mount` 需要 root；tmpfs 也不例外） | 用注入的 ENOSPC 语义（`kUnavailable`）覆盖**行为**（含回滚），并断言状态码 |
| 真实 NVMe/HDD/NFS 上的延迟分布与容量数字（C9.14/C9.22） | 本机是虚拟盘（WSL2 语义），`docs/00-final-design.md` 已明确**不得作为判定依据** | 现有量级参考仍标注为"量级参考" |
| io_uring 在目标内核上的可用性（C9.18/C9.29/C9.30） | 需目标部署内核；本机默认 seccomp 阻断（ADR-010 已实测 `EPERM`） | `scripts/check_io_uring.sh`（退出码 2 = 无结论）+ `UringIoEngine` 骨架 |
| 多实例 + 共享 PG 端到端（C9.26）与 PG 租约 | PG 版仓储/租约未交付（P9 登记项） | `db/tests/00{1,2,3}` 的 PG 基建门槛（`FSS_GATES_WITH_PG=1`）+ `test_multi_instance`（内存/单进程） |
| 容器镜像（C9.8）与真实网卡吞吐（C9.14） | 本环境无 docker/真实网卡 | 待切片 3 评估（Dockerfile 至少静态可检查） |
