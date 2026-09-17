# ADR-010：I/O 引擎选择 —— 阻塞线程池为默认，io_uring 为**可选加速引擎**

- 状态：**已采纳（Accepted）**
- 日期：2025
- 相关：`docs/adr/ADR-006`（大文件数据面，拟定）、`docs/adr/ADR-007`（异步/协程）、
  `docs/05-capacity-and-concurrency.md`、`docs/appendix/posix-io-probe/`

---

## 1. 结论

**不把 io_uring 作为必需依赖。** 做法是：

```
IIoEngine（抽象）
 ├── BlockingIoEngine   ← 默认。pread/pwrite + 有界线程池。到处都能跑，无额外依赖
 └── UringIoEngine      ← 可选。启动时探测内核 + seccomp；不可用则回退（或按配置拒绝启动）
```

`io_engine: blocking | uring | auto`，**默认 `blocking`**；`auto` 表示"探测后择优"。
探测结果必须在 `/v2/info` 与指标中可见。

**理由的优先级**：不是"性能不够"，而是 **io_uring 在默认容器运行时里根本跑不起来**（实测 `EPERM`），
且其收益天花板受"只能用于文件侧"限制。详见 §2、§3。

---

## 2. 实测：收益是多少（本机，量级参考）

原始输出：`docs/appendix/posix-io-probe/RESULTS.txt`、`docs/appendix/coroutine-probe/`。

### 2.1 读路径（O_DIRECT 4 KiB 随机读，绕开页缓存）

| 方案 | 并发 | IOPS |
| --- | --- | --- |
| 阻塞线程池 | 1 线程 | 8,221 |
| 阻塞线程池 | 32 线程 | 108,096 |
| 阻塞线程池 | **128 线程** | **118,148** |
| **io_uring** | **1 线程，深度 64** | **132,934** |
| io_uring | 1 线程，深度 256 | 136,491（**设备饱和**） |

**结论**：设备本身是天花板（约 136k IOPS / 533 MiB/s），
**io_uring 只是用 1 个线程替代 32–128 个线程，并不提高上限**。
32 线程已拿到设备的 79%。

### 2.2 写路径（4 KiB 写 + `fsync`）

| 方案 | 文件/秒 |
| --- | --- |
| 阻塞线程池，64 线程，每文件 `fsync` | 16,104 |
| io_uring，深度 16（单线程），`write`+`fsync` 链式 | **20,497** |
| **两阶段批提交（ADR-008）** | **31,478** ← 比 io_uring 快 **1.5x** |

**结论**：写路径的真正杠杆是**摊销 `fsync`**，不是异步 I/O。
io_uring 拿不到"用异步换掉 1.44 ms 设备 flush"这种便宜。

### 2.3 页缓存命中路径

4 KiB 缓存命中读 **0.7 µs**（单线程 150 万 ops/s）→ **任何异步化都是净开销**。
io_uring 只对"绕过缓存/慢存储"的路径有意义（可用 `min_latency` 场景收益另算）。

---

## 3. ★ 关键约束：默认容器运行时**禁用** io_uring（实测）

这是本 ADR 的决定性证据。

| 环境 | `io_uring_setup` |
| --- | --- |
| 宿主机 | ✅ 成功（fd=3） |
| **容器 + 默认 seccomp profile** | ❌ **`EPERM` (Operation not permitted)** |
| 容器 + `--security-opt seccomp=unconfined` | ✅ 成功（fd=3） |
| 内核开关 `kernel.io_uring_disabled` | `0`（内核层面允许） |

即：**问题不在内核，而在容器运行时**。Docker 自 2023 年起在默认 seccomp profile 中屏蔽
`io_uring_setup`/`io_uring_enter`/`io_uring_register`（因为 io_uring 历史上的 CVE 密度较高）。
Kubernetes 集群普遍沿用该默认 profile。

**这意味着**：若把 io_uring 设为必需依赖，部署方必须做到其中之一：

| 选项 | 代价 |
| --- | --- |
| `seccomp=unconfined` | **大幅削弱容器隔离**，多数安全策略不允许 |
| 自定义 seccomp profile 放行 io_uring 三个系统调用 | 可行但需与平台/安全团队协商；集群级策略可能直接拒绝 |
| 不用容器 / 特权容器 | 与常规部署方式冲突 |

**这属于"部署环境依赖"，不是应用能自己解决的问题** → 因此不能作为默认路径。

---

## 4. 收益上限：本项目的 io_uring 只能用在**文件侧**

io_uring 的最大价值之一是"网络 + 文件合并提交、共享一个 ring"。但本项目的 HTTP 栈是
**cpp-httplib 的阻塞线程模型**（ADR-002），socket 由它持有，**handler 拿不到 fd**
（这也是 ADR-006 要用自持 socket 的原因）。因此：

| 侧 | 能否用 io_uring | 说明 |
| --- | --- | --- |
| socket（accept/recv/send） | ❌（现状） | httplib 持有；除非整个控制面迁到 Asio —— 已被 ADR-007 否决 |
| **文件（pread/pwrite/fsync）** | ✅ | 数据面可用 |
| **`sendfile` 零拷贝** | ⚠️ 部分 | 本机 liburing 2.1 **没有** `io_uring_prep_sendfile`（只有 `splice`）；且 `sendfile` 的主要收益（2.05x）**不依赖 io_uring** |

**结论**：io_uring 的收益被限制在"文件侧并发"这一块；
而文件侧最大的那个优化（`sendfile` 零拷贝，2.05x、每 GiB CPU 少 42%）**用阻塞 `sendfile` 就能拿到**。

---

## 5. 决策

### 5.1 分层策略

| 层面 | 决策 |
| --- | --- |
| 默认引擎 | **`blocking`**（`pread`/`pwrite` + 有界线程池） |
| io_uring | **可选引擎**，默认关闭；代码路径与探测必须先于启用而存在 |
| 抽象 | 新增 `IIoEngine` 端口（L3），数据面只依赖该端口，不直接调 `pread`/io_uring |
| 探测 | 启动时做**能力探测**：内核版本 + `io_uring_setup` 是否 `EPERM`/`ENOSYS` + `IORING_REGISTER` 可用性 |
| 配置语义 | `blocking`：不探测；`uring`：探测失败则**拒绝启动**（避免"以为开了其实没开"）；`auto`：探测失败则回退并在日志/指标中标注 |
| 可观测 | `/v2/info` 暴露 `ioEngine` 与 `ioUringAvailable`；指标 `fss_io_engine_engaged`、`fss_io_engine_probe_failures_total` |
| 回退 | `blocking` 路径**始终可用且被测试**（它不是"降级"，而是一等公民） |

### 5.2 启用 io_uring 的前置条件（全部满足才考虑）

| # | 条件 | 验证方式 |
| --- | --- | --- |
| U1 | 目标部署环境**允许** io_uring（seccomp 已放行或未使用默认 profile） | 部署前在**目标容器**里跑探测（门槛 C10.2） |
| U2 | 目标存储上实测收益 **≥ 1.5x**（吞吐）或线程/内存显著下降 | 在真实存储（NVMe/HDD/NFS）上复测（门槛 C10.3） |
| U3 | 触发条件 T1（需 >2,000 并发 I/O）或 T3（socket 侧成瓶颈）之一成立 | ADR-007 §4 |
| U4 | 存储延迟**高**（HDD/NFS 的 ms 级）或需要极高队列深度 | 本机 NVMe 级别（120 µs）时 32 线程已够 → **收益不足以抵消复杂度** |

**特别说明 U4**：io_uring 的价值与**I/O 延迟**成正比。
本机 NVMe 级延迟下 32–64 线程已接近设备饱和；
但在 **HDD / NFS**（延迟高 1–2 个数量级）上，要拿到同样的 IOPS 需要数千个在途请求 ——
**线程模型做不到，io_uring 才有决定性价值**。
因此这个决策**必须在真实存储上复核**（本机无 NFS 可挂载，见 ADR-009 §6.1）。

### 5.3 与 ADR-006 的关系

ADR-006 的**独立数据面**是 io_uring 的**正确归宿**，因为它自持 socket ——
只有在那里才可能拿到"网络 + 文件同 ring"的完整收益。
因此实现顺序为：

```
P3/P4：数据面用 BlockingIoEngine（pread + sendfile）           ← 先交付，到处能跑
P9   ：C9.12 受控复核 ADR-006 是否值得做独立数据面
        └─ 若值得 → 在独立数据面内引入 UringIoEngine（可选，探测 + 回退）
```

**不**把 io_uring 引入 httplib 控制面（那里只能用在文件侧，收益被 §4 限制，还要多背一个依赖）。

---

## 6. 后果

**正面**

- 部署零额外要求：默认路径在任何内核/容器/权限组合下都能跑（已实测容器内可用）；
- 不引入 `liburing` 依赖（本机需 apt 解包；且平台版本与内核特性存在耦合）；
- 把"是否值得"变成**可测量的门槛**（U1–U4 + C10.2/C10.3），而不是先验判断；
- 保留了未来收益：抽象边界已就位，启用不需要改动业务逻辑。

**负面 / 成本**

- 需要维护 `IIoEngine` 抽象与**两条都被测试的路径**（成本可控：接口只有 read/write/fsync/stat 几个方法）；
- 若将来确实需要 io_uring，部署方必须改 seccomp profile —— 这是**外部依赖**，可能成为阻塞项；
- 在本机这种低延迟存储上，io_uring 的收益不足以抵消复杂度，可能永远不启用（这是可接受的结论）。

**明确不做**

- 不把 io_uring 作为默认或必需依赖；
- 不为 io_uring 而把控制面迁到 Asio（ADR-007 已否决整体重写）；
- 不基于"本机 WSL2 上可用"就假定生产可用（WSL2 的 seccomp/内核配置与生产不同）。

---

## 7. 门槛（新增）

| 编号 | 判据 |
| --- | --- |
| **C1.14** | `IIoEngine` 抽象存在，且**阻塞引擎**在阶段 3 交付并通过全部数据面测试（io_uring 不是前置条件） |
| **C1.15** | 能力探测正确：在**默认 seccomp 容器**内探测必须报告 `ioUringAvailable=false` 且 `io_engine=auto` 时**成功回退**、`io_engine=uring` 时**拒绝启动**（用本仓库的容器测试脚本复现） |
| **C9.29** | 在**目标存储**（NVMe/HDD/NFS）上复测 io_uring vs 阻塞的收益，据此决定是否启用（U2）；HDD/NFS 场景下若差异 ≥1.5x 则建议启用 |
| **C9.30** | `/v2/info` 与指标正确暴露 `ioEngine` / `ioUringAvailable`；且在**不允许 io_uring 的部署**里不影响任何 OSDU 端点行为 |

**验证工具**：本 ADR 的容器实验可脚本化为 `scripts/check_io_uring.sh`
（宿主机探测 + 容器内探测 + 回退行为验证），作为 C1.15/C9.29 的执行体。

---

## 8. 待办

- [ ] `IIoEngine` 端口签名定稿（`read`/`write`/`sync`/`stat`，含 64 位偏移与区间读）
- [ ] `BlockingIoEngine` 实现（P3）+ 与 POSIX 驱动共用契约测试
- [ ] `UringIoEngine` 骨架 + 能力探测（**可在不启用的情况下先交付探测与回退**）
- [ ] `scripts/check_io_uring.sh`：宿主机/容器/回退三态验证
- [ ] 在 `docs/operations.md` 写明：**启用 io_uring 需要修改容器 seccomp profile**，以及为什么默认不开
- [ ] 把本 ADR 的实测数据补进 `docs/appendix/posix-io-probe/RESULTS.txt`
