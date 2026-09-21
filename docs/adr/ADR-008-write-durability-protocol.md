# ADR-008：小文件写入的耐久性协议 —— 两阶段批提交（同步先行，改名后置）

- 状态：**已采纳（Accepted）**，协议已通过顺序不变量验证；**P4 已实现**（并发驱动的组提交，
  见 §7；真实断电语义仍**未验证**，见 §7.3）
- 日期：2025
- 相关：`docs/05-capacity-and-concurrency.md`、`docs/adr/ADR-007-async-and-coroutines.md` §8、
  `docs/appendix/group-commit-durability/`
- **重要**：本 ADR **更正**了 ADR-007 §8.3 中一个错误的性能数字（见 §1）

---

## 1. ⚠️ 先更正一个错误结论

ADR-007 §8.3 曾给出"**组提交 = 58,741 文件/秒，比 io_uring 快 2.9x**"。

**该数字是错的**（或者说，它测的是一个**不安全**的协议）。当时的基准实现是：
写文件 → rename → 批末 `fsync(dir)`，**全程没有对文件数据做 `fsync`/`fdatasync`**。

用 strace 逐系统调用核对后确认，这个顺序是：

```
write(.tmp_N)  →  rename(.tmp_N → f_N)        ← ★ 数据还没落盘就改名了
... 批末 ...   →  fsync(dir)
```

**后果**：断电后可能出现"**最终文件名存在，但内容为空或残缺**"的文件。
对一个文件服务来说这是**数据完整性事故**：元数据可能已经登记，但文件是坏的。

修正后的真实数字见 §4：安全的协议是 **31,478 文件/秒**（而不是 58,741），
但仍然比"最朴素的严格做法"快 **82 倍** —— 结论方向不变，数字与协议都要改。

---

## 2. 问题：哪些不变量必须成立

对"客户端已收到成功响应"的文件，必须满足：

| 不变量 | 内容 | 为什么重要 |
| --- | --- | --- |
| **R1（可见即完整）** | 任何 rename 到最终名之前，该文件的**数据**必须已经 durable | 否则断电留下"存在但内容为空/残缺"的文件 → 数据完整性事故 |
| **R2（承诺即可靠）** | 任何对外确认（HTTP 201 / 元数据登记）之前，本批的**改名**必须已经 durable | 否则"成功"了的文件重启后消失 → 契约违约 |
| **R3（无部分写）** | 最终名下一个文件要么完整，要么不存在 | 由 `tmp + rename` 的原子性保证 |

> **R1 是经典的"rename without fsync"陷阱**。POSIX 不保证：`rename` 只保证*命名*的原子性，
> 不保证*数据*与*元数据*的持久顺序。Linux ext4 在 `data=ordered` 下**通常**安全，
> 但这不是 API 保证，不能依赖。

---

## 3. 候选协议与实测（每文件 4 KiB，5000 文件，批 500，本机 WSL2 ext4）

| 协议 | 顺序 | 文件/秒 | R1 | 判定 |
| --- | --- | --- | --- | --- |
| **P0** 无 sync | `write → rename` | **61,131** | ✗ | 不安全上界（仅参考） |
| **P1** 严格 | `write → fdatasync(文件) → rename → fsync(目录)`（每文件） | **383** | ✅ | 安全但不可用 |
| **P2** 天真的组提交 | `write → fdatasync(文件) → rename`，批末 `fsync(目录)` | **784** | ✅ | 安全，仅 **2.0x** |
| **P4 ★ 两阶段批提交** | ①写整批 `.tmp_i` → ②`syncfs()` → ③统一 `rename` → ④`fsync(目录)` → ⑤确认 | **31,478** | ✅ | **安全，82.3x** |
| **P3** 松散 | `write → rename`，批末 `syncfs()` | 35,389 | ✗ | **不安全**，且只比 P4 快 **11%** |

### 3.1 三个关键洞察

**① P2（把每文件 `fdatasync` 保留、只批量目录 `fsync`）只快 2 倍，没用。**

因为真正贵的不是目录 `fsync`，而是**每文件的数据 `fdatasync`**（实测 1.44 ms/次）。
批量化目录 `fsync` 只把 5000 次降到 11 次，省下的是零头。

**② 真正的杠杆是"把修改名的时机后移"（P4）。**

```
阶段 A: write .tmp_0, .tmp_1, ..., .tmp_N-1     （数据进页缓存，尚未 durable）
阶段 B: syncfs(dirfd)                            ← 一次系统调用让【全批数据】durable
阶段 C: rename .tmp_i → f_i  （N 次，纯元数据操作，廉价）
阶段 D: fsync(dirfd)                             ← 一次系统调用让【全批改名】durable
阶段 E: 对外确认（HTTP 201 / 元数据登记）
```

- R1 成立：所有 rename 都在 `syncfs` **之后**；
- R2 成立：确认在 `fsync(dir)` **之后**；
- 代价：每批 **1 次 `syncfs` + 1 次 `fsync(dir)`**，与批大小无关（实测：5000 文件 / 500 批 → `syncfs=10`, `fsync_dir=10`）。

**③ 不安全的做法只快 11%（35,389 vs 31,478）。**

这是个**没有取舍的取舍**：P3 用 11% 的吞吐换来了"可能产生损坏文件"。
任何"为了性能而跳过数据 fsync"的理由都不成立。

### 3.2 代价与必须声明的语义

| 项 | 说明 |
| --- | --- |
| **耐久性粒度** | P4 的确认粒度是**批**而不是单文件：崩溃时最多丢**一个在途批**（默认批大小可配）。已经确认（E 阶段）的批一定不会丢 |
| **`syncfs` 是全局操作** | 它会 flush **整个文件系统**，在多租户/多业务共盘时会牵连其他写入。缓解：按 partition 分盘，或改用"每文件 `fdatasync` 的 P2"（慢但精确） |
| **崩溃后的残留** | 会留下未改名的 `.tmp_*` 文件。它们**不是**最终文件，清理任务必须能识别并删除（GC 的一部分），且**绝不能**把它们当成有效对象 |
| **批大小的选择** | 批越大吞吐越高、崩溃丢失窗口越大。默认建议 **500–2000**，必须可配并在 `/v2/info` 暴露实际值 |

---

## 4. 验证方法与结果

因为**没有 root**，无法使用 `dm-log-writes` / `dm-flakey`，也**无法做真实断电实验**。
因此采用三条**互补且可复现**的验证手段，并如实标注各自能证明什么。

### 4.1 检测器自证（对照实验）—— 证明"没发现问题"不是因为检测器失灵

用一个**故意不安全**的协议做对照：`协议 9` 直接写最终路径（无 `tmp + rename`）。

| 结果 | 数值 |
| --- | --- |
| 12 次随机时刻 `SIGKILL` 中检出**残缺文件** | **11/12** ✅ |
| 检出的典型残缺 | `('f_3073', '大小2048')`、`('f_3413', '大小0')` |

**检测器有效**。因此后续"0 个残缺文件"是有意义的结论，而不是空转。

### 4.2 崩溃点模型枚举 + 顺序不变量（机器可检查，**这是安全性的主要依据**）

`docs/appendix/group-commit-durability/model_check2.py` 解析 strace 跟踪，
把 R1 建模为可判定条件：*某个 `rename(tmp→final)` 之前，该 tmp 的数据是否已 durable*
（`fdatasync` 该 fd，**或** write 之后发生过 `syncfs`）。

| 协议 | rename 次数 | **R1 违反** | 最坏崩溃分支下"存在但残缺"的文件数 | 判定 |
| --- | --- | --- | --- | --- |
| P1 严格 | 6 | **0** | 0 | ✅ 安全 |
| P2 组提交 | 6 | **0** | 0 | ✅ 安全 |
| **P4 ★两阶段** | 6 | **0** | **0** | ✅ **安全** |
| P3 松散 | 6 | **6** | **6** | ❌ **不安全** |

strace 人工交叉核对 P4（`t4.trace`）：

```
openat(.tmp_0) write   openat(.tmp_1) write   openat(.tmp_2) write
syncfs(3)                                        ← 全批数据落盘
rename(.tmp_0→f_0) rename(.tmp_1→f_1) rename(.tmp_2→f_2)
fdatasync(3)                                     ← 全批改名落盘
```

### 4.3 崩溃注入（`SIGKILL`）—— 证明"可见性原子性"

每协议 16 次随机时刻 `SIGKILL`（总 6000 文件，批 500）：

| 协议 | I1 残缺文件 | I2 残留 tmp > 批大小 | I3 最终文件少于承诺 | 最大残留 tmp |
| --- | --- | --- | --- | --- |
| P1 | 0/16 | 0/16 | 0/16 | 1 |
| P2 | 0/16 | 0/16 | 0/16 | 1 |
| **P4** | **0/16** | **0/16** | **0/16** | 0 |
| P3 | 0/16 | 0/16 | 0/16 | 0 |

> ⚠️ **这个测试不能证明断电安全**：`SIGKILL` 只杀进程，**页缓存不会丢**，
> 所以未 `fsync` 的数据依然"存在"。它只能证明 **`tmp+rename` 的可见性原子性**（R3）。
> **断电丢数据的结论来自 §4.2 的顺序不变量，不是来自这里。**

### 4.4 机制性证据：`/proc/meminfo` 的 Dirty 页

写入 4000 个文件（约 16 MiB）期间采样 `Dirty` 峰值：

| 协议 | Dirty 峰值 | 含义 |
| --- | --- | --- |
| **P1** 每文件 `fdatasync` | **1,344 kB** | 内存里几乎不积压 → 数据确实在设备上 |
| **P3** 批末 `syncfs` | 3,296 kB | 积压一个批 |
| **P0** 无 sync | **15,128 kB** | **几乎全部 16 MiB 都只在内存里** |

直接证明 P0/P3 在 rename 时刻数据**尚未落盘**。

### 4.5 失败/无结论的实验（如实保留）

| 实验 | 结果 |
| --- | --- |
| `O_DIRECT` 读回（绕过页缓存）区分"已落盘/仅在内存" | ❌ **无法区分**：`fdatasync` 后与不 sync 的 O_DIRECT 读回都返回正确内容（ext4 对 O_DIRECT 与缓冲写有连贯性处理）。该实验**不构成证据** |
| 真实断电实验 | ❌ **未做**：需 root（`dm-log-writes`/`dm-flakey`）、VM 快照或硬件。列为残余风险 |

---

## 5. 决策

1. **采用 P4（两阶段批提交）作为 POSIX 集中存储的小文件写入协议**：
   `write all tmp → syncfs → rename all → fsync(dir) → 确认`。
2. **禁止使用 P3 式顺序**（`rename` 早于数据 durable）。在实现中这是一条**不变量**，
   由 `model_check2.py` 这样的顺序检查在 CI 中强制执行（见门槛 C9.23）。
3. **保留 P2 作为可选的精确模式**：`durability: per_file`（每文件 `fdatasync`，383 文件/秒）
   供"不允许批级丢失窗口"或"`syncfs` 全局 flush 影响他人"的场景使用。
   配置项：`fsync_policy: batch | per_file`，默认 `batch`，批大小 `group_commit_max_batch`（默认 500）。
4. **必须在文档与 `/v2/info` 中显式声明当前耐久性粒度**（批级 / 单文件级），
   不允许"默认值不说清"。
5. **GC 必须能识别并清理残留 `.tmp_*`**，且**绝不能**把它们视为有效对象。
6. **不为追求吞吐而放弃 R1**。实测代价只有 11%，没有取舍空间。

### 与既有设计文档的衔接

| 既有条目 | 需要的修改 |
| --- | --- |
| `config/fss.example.json` 的 `fsync_policy: by_size` | 改为 `durability: batch \| per_file`，并保留按大小的例外；新增 `group_commit_max_batch` |
| `docs/05-capacity-and-concurrency.md` §5.5 与 README 的"58,741 文件/秒"（该数字与容量小节**只在旧版 README** 里；README 改版为对外介绍后，容量内容以 `docs/05` 为准） | **更正为 31,478（P4，安全）**，并注明原数字来自不安全协议 |
| 门槛 C9.21（fsync 摊销收益） | 数值更正；并增加"必须为 P4 顺序"的断言 |
| ADR-007 §8.3 | 加更正批注（已在原处标注） |

---

## 6. 待办

- [ ] 门槛 **C9.20**（本 ADR 的验证已部分完成）：把 `model_check2.py` 的顺序不变量检查
      **固化为 CI 测试**（对新实现产出的 strace 跟踪做机器检查），并要求 P4 顺序
- [x] 门槛 **C9.23**：实现"顺序不变量"回归测试 —— 故意把 `syncfs` 移到 `rename` 之后，
      测试**必须**失败（自证手法，参照 C1.2b）
      —— **已交付（本切片）**。判据在 `tests/integration/test_posix_batch_commit.cpp`：
      ① 摊销（并发 N=8 / C=4 → `syncfs == 2`；C=1 → == 8；`per_file` → 0）；
      ② 顺序不变量（任何 `rename` 都不在第一次 `syncfs` 之前；`fsync_dir` 在每批所有
      `rename` 之后）；③ 数据正确性 + 无 `.tmp_*` 残留（配正控）；④ 阈值例外；
      ⑤ `atomic_write=false` 不入批；⑥ 失败路径。R1 自证 3 个注入都让对应用例失败（证据：
      [`docs/test-evidence/phase9.md`](../test-evidence/phase9.md) §13）。实现状态见 §7。
- [ ] 门槛 **C9.24**：`syncfs` 全局 flush 的影响评估 —— 在多租户共盘场景下测量它对其他写入的
      干扰；必要时默认改为 `per_file` 或要求按 partition 分盘。⚠️ **本环境无法测**（见 §7.3）
- [ ] 门槛 **C9.25**：GC 对残留 `.tmp_*` 的清理测试（含"绝不视为有效对象"的反向测试）
      —— **已交付（P9）**：`IBlobStore::remove_temp_files` + `tests/hardening/test_metrics_and_gc.cpp`
- [ ] 批大小与耐久性窗口的敏感性测试，确定推荐默认值与上限。⚠️ 本实现的批形成是
      **并发驱动**的组提交，窗口是具名常量 `kBatchWaitWindow = 2 ms`（不是配置键），
      敏感性测试只在"批大小上限 `group_commit_max_batch`"这一维可做（见 §7）
- [ ] 在目标部署环境尝试真实断电/VM 快照测试（若可获得 root 或虚拟化能力）；否则在文档中
      维持"**真实断电未验证**"的标注

---

## 7. 实现状态（P4 **已实现**：并发驱动的组提交）

### 7.1 结论

- `storage.posix.durability=batch` 现在是**真两阶段批提交**：`write all tmp → syncfs →
  rename all → fsync(dir)`。此前组合根把它**近似**成 `FsyncPolicy::kBySize`，而该策略的语义
  是"小于阈值的文件**不 fsync**、靠批提交摊销"——**批提交根本不存在**，于是默认配置下
  小文件**从不落盘**（崩溃可能丢已确认的写入）。这是"默认路径的耐久性承诺与实现不符"，
  已按 §5 的 R1/R2 修正（推翻记录见 `docs/00-final-design.md` §5）。
- 实现位置：`src/infra/blob/posix/posix_blob_store.cpp`（`JoinBatch` / `CommitBatch` /
  `WriteTmpFile`）与 `src/infra/io/file_sync.cpp`（`RealFileSync::SyncFilesystem` →
  `syncfs(2)`）；接缝声明在 `posix_blob_store.h`（`IBatchCommitObserver` /
  `IBatchArrivalGate`）与 `file_sync.h`（`IFileSync::SyncFilesystem`）。
- 配置映射（`src/main/server_main.cpp`）：`durability=batch → batch_commit=true` +
  `group_commit_max_batch` + `sync_dir_after_batch`；`per_file`/`never` 逐字保持既有语义；
  `sync_dir_after_batch=false` **拒绝启动**（R2 是不变量，schema 也写明必须 true）。
- 可观测：`fss_posix_{syncfs,group_commits,batch_objects}_total`（真实进程 `/metrics`）。

### 7.2 批如何形成 / 领队做什么 / 失败规则

| 项 | 实现 |
| --- | --- |
| 批的形成 | **并发驱动的组提交**（没有独立的批量写 API，也没有调用方写过批）：每个 `put` 先写自己的 `.tmp_*`（+ sidecar 的 tmp），再入批；把批填满 `group_commit_max_batch` 的**那个入批者**成为提交者（因此批不会超过上限）；本批第 1 个入批者（领队）等"批满"或"有界窗口到期"（`kBatchWaitWindow = 2 ms`，具名常量，理由见源码注释）或"没有别的写入者" |
| 领队/提交者做什么 | ② 一次 `syncfs(dirfd)` 让全批数据 durable → ③ 统一 `rename`（对象本体 + sidecar）→ ④ `fsync(目录)`。提交串行化（`batch_commit_mutex_`），保证逐批事件序列可切分 |
| `syncfs` 失败 | **整批失败、一个 rename 都不做**（R1：宁可没有对象，也不留"可见但可能残缺"的对象），并清理全部 tmp |
| 某个 rename 失败 | **只有那个对象**失败（`put` 返回错误给它的调用者），同批其它对象照常提交；失败对象的所有 tmp 被清理 |
| `atomic_write=false` | **不参与批**（没有 rename 阶段）：逐字保持"直接写目标、失败删目标"的旧直写语义 |
| `>= fsync_threshold_bytes` | **强制单独 `fdatasync`**（不靠批摊销）：单文件 `fdatasync → rename → fsync(dir)`，与 schema 描述一致 |
| `copy` 路径 | **不参与批**（不走 put 的入批逻辑）；`batch_commit=true` 时按"单文件落盘"处理（安全方向），因此 copy 拿不到摊销 |

### 7.3 本实现的**未验证项**（不许含糊）

| 项 | 状态 | 为什么本环境测不了 |
| --- | --- | --- |
| **power-loss durability（R1/R2 在真实断电下成立）** | **未验证（R4/R8）** | 无 root、不能 `mount`、无电源故障注入（`dm-log-writes`/`dm-flakey`/VM 快照都不可用）。已验证的是**顺序不变量**（机器可检查）——它是安全性的**主要依据**（ADR-008 §4.2），但**不是**断电实验 |
| §4.3 的 `SIGKILL` 可见性证据 | **属于协议研究，不是对本实现的验证** | `SIGKILL` 不丢页缓存；它只证明 `tmp+rename` 的可见性原子性（R3） |
| **C9.24（`syncfs` 全局 flush 对他人的影响）** | **未验证（干扰量级）** | 无多租户共盘场景可测；缓解方向（按 partition 分盘）的配置键 `storage.posix.one_filesystem_per_partition` 已在 **E1b** 接通（启动期校验规则 A/B，违规 → exit 78），但该键只保证"分盘这件事成立"，**不测量**干扰量级 |
| **吞吐数字** | **不迁移** | §3/§4 的 31,478 文件/秒、82.3×、383 文件/秒来自**显式批量写** 5000 文件 / 批 500 场景；本实现是并发驱动的组提交，**顺序单文件写仍是一文件一次提交**（拿不到摊销），只有**并发**写才有摊销。不得把 ADR 数字当成本实现成绩；本切片未重测基线 |
| **批窗口敏感性（推荐默认值与上限）** | **未做** | 窗口是具名常量而非配置键；只在"批大小上限"一维上可由 `tests/integration/test_posix_batch_commit.cpp` 覆盖（N=8/C=4 → 2；C=1 → 8） |
