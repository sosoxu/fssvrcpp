# ADR-009：多实例部署的一致性与共享状态设计

- 状态：**已采纳（Accepted）**，核心竞态已实测复现并验证修复
- 日期：2025
- 相关：`docs/adr/ADR-004`（持久化策略）、`docs/adr/ADR-008`（写入耐久性）、
  `docs/02-design.md` §9/§13、`docs/appendix/multi-instance-probe/`

---

## 1. 结论摘要

**当前设计不支持多实例部署，而且不只是"换成共享数据库"就够。**
实测复现了 **3 个会造成数据损坏或丢失的竞态**，另有 2 个设计缺陷：

| # | 问题 | 后果 | 实测 | 修复 |
| --- | --- | --- | --- | --- |
| **M1** | 临时文件名不含实例标识（`.tmp_<idx>`） | **静默内容错乱**：B 的最终文件里是 A 的内容 | **21/40** | 文件名加实例标识 |
| **M2** | 无幂等键唯一约束（check-then-insert） | **重复记录 + 重复复制对象** | **20/20** | 幂等键唯一 + `ON CONFLICT` 原子领取 |
| **M3** | GC 删除"无元数据记录"的 staging 对象 | **误删在途上传，数据丢失** | **20/20** | 租约 + 到期才回收 + 原子领取 |
| **M4** | SQLite 作为共享状态 | 状态发散（各实例独立 DB）；共用文件在 NFS 上不安全 | 已复现 | 改用 **PostgreSQL** |
| **M5** | 自签 token 的 `single_use_nonce` 用本地表 | 跨实例**无法拒绝重放**（或误拒合法 token） | 已复现 | nonce 入共享存储，或**默认关闭** |

**推荐架构**：控制面强一致（**PostgreSQL** 事务 + 唯一约束 + advisory lock）；
数据面放**共享 POSIX 存储**（NFS/SAN/CephFS）；在途状态用**租约**；单例后台任务用**领导者选举**。
实例本身**无状态**（除连接池与缓冲），任何实例可服务任何请求。

---

## 2. 原设计假设审计：多实例下哪些东西会坏

| 原设计内容 | 多实例下的问题 | 严重度 |
| --- | --- | --- |
| `location.repository: sqlite` / `metadata.repository: sqlite`（默认） | 每个实例一个本地 DB → **状态发散**（M4） | 🔴 致命 |
| `ObjectKeyPolicy` 生成 `file_id` 作为键，tmp 名由序号派生 | 跨实例 tmp 名冲突 → **静默内容错乱**（M1） | 🔴 致命 |
| `CreateFileMetadata` 的 12 步序列（check-then-copy-then-insert） | 两实例并发处理同一 `fileSource` → **重复记录 + 重复复制**（M2） | 🔴 致命 |
| GC 任务"删除无元数据记录的 staging 对象" | 与在途上传竞争 → **误删**（M3） | 🔴 致命 |
| `self_signed.single_use_nonce`（本地 nonce 表） | 跨实例重放无法拒绝（M5） | 🟠 高 |
| 进程内 `ManualClock`/`IClock` 驱动的过期与租约判定 | **时钟偏移**会导致租约误判 | 🟠 高 |
| 每实例独立的配额/限流计数 | 全局配额失效 | 🟡 中 |
| 只在本地做 `readiness` 检查 | 依赖不可用时仍报就绪 | 🟡 中 |

> 注：**ADR-008 的两阶段批提交（P4）本身是单实例内的批处理**，多实例下每个实例各自成批、
> 互不影响（见 §6.2 的 NFS 语义待验证项）。

---

## 3. 实测证据（5 个实验，两个独立进程充当两个实例）

原始输出：`docs/appendix/multi-instance-probe/RESULTS.txt`；探针源码同目录。

### M1 临时文件名跨实例冲突 —— **21/40 次静默内容错乱**

两实例向同一共享目录写入，最终名不同（`f_A_7` / `f_B_7`）但 tmp 名相同（`.tmp_7`）：

| 方案 | 结果 |
| --- | --- |
| `tmp = .tmp_<idx>` | **内容错乱 21/40** ❌ —— 例：`f_B_7` 的内容是 `idx=7;writer=A;...`；另一种表现是 `rename` 报 `ENOENT` |
| `tmp = .tmp_<inst>_<pid>_<counter>` | 0/40 ✅ |

**这是最危险的一类 bug**：不报错、不崩溃，只是"数据悄悄换成了别人的"。
在单实例下永远不会出现，一旦扩到 2 个实例就随机发作。

### M2 重复创建竞态 —— **20/20 次产生重复记录，且两个实例都做了复制**

两实例并发用同一 `fileSource` 登记元数据（经典 check-then-act，窗口 80 ms）：

| 方案 | 重复记录 | 执行复制的实例数 |
| --- | --- | --- |
| 无唯一约束，check-then-insert | **20/20** ❌ | **2**（重复复制 + 重复对象） |
| 幂等键唯一约束 + `ON CONFLICT` 原子领取 | **0/20** ✅ | **1** |

**过程中的一个自身错误（值得记录）**：我第一次把唯一约束建在了 `(partition_id, id, version)`
这个**随机 UUID 主键**上——冲突永远不会触发，所以"修复版"依然 20/20 重复。
**唯一约束必须建在幂等键上**（`file_source`），这不是实现细节，是协议的一部分。

### M3 GC 与在途写入竞态 —— **20/20 次误删在途数据**

实例 A：创建 staging 对象 → 花 500 ms 计算校验和/复制 → 才登记元数据。
实例 B：同时跑 GC，清理"没有元数据记录"的 staging 对象。

| 方案 | A 的数据被误删 |
| --- | --- |
| naive GC（无元数据记录就删） | **20/20** ❌ 数据丢失 |
| fixed GC（租约未过期则跳过；`DELETE ... RETURNING` 原子领取） | **0/20** ✅ |

naive GC 的逻辑看起来完全合理——"没有元数据记录的对象就是垃圾"。
但**在途上传恰恰就是"还没有元数据记录"**，于是 GC 与正常业务直接对撞。

### M4 SQLite 不能作为多实例共享状态

| 部署方式 | 结果 |
| --- | --- |
| 每实例独立 DB 文件（最容易误用） | A 写入的记录 **B 完全看不到**（B 的记录数 = 0）→ 状态发散 ❌ |
| 两进程共用同一 DB 文件（本地 ext4） | 可用（400/400 条，无冲突）；但 SQLite 官方明确**不支持在网络文件系统上并发写**，而集中存储多实例必然使用 NFS/SAN ❌ |

### M5 `single_use_nonce` 在多实例下不成立

| 场景 | 结果 |
| --- | --- |
| A 签发 token，带回 A | 接受 ✅ |
| A 签发 token，**带回 B** | **接受** ❌ —— B 的本地 nonce 表里没有该 nonce，无法拒绝重放 |
| 同一 token 再次回 A | 若 A 未记录则接受 |

⇒ "一次性"语义需要**共享 nonce 状态**；本项目的默认应当是 **`single_use_nonce: false`**
（ADR-003 已经把默认设为 false，这里明确**多实例下必须保持 false**，除非接入共享存储）。

---

## 4. 推荐架构

```
                    ┌──────────────────────── 客户端 ────────────────────────┐
                    │  可被任意实例服务（实例无状态）                          │
                    └───────┬───────────────────────────────┬─────────────────┘
                            ▼                               ▼
                  ┌──────────────────┐            ┌──────────────────┐
                  │   实例 #1        │            │   实例 #2   ...   │
                  │  控制面：无状态   │            │  控制面：无状态   │
                  │  数据面：无状态   │            │  数据面：无状态   │
                  └───┬──────────┬───┘            └───┬──────────┬───┘
                      │          │                    │          │
        ┌─────────────▼──┐   ┌───▼────────────────────▼───┐  ┌───▼──────────────┐
        │ PostgreSQL     │   │ 共享 POSIX 存储             │  │ 对象存储（S3 等）  │
        │ ·位置/元数据记录 │   │ (NFS/SAN/CephFS)           │  │ 客户端直连，      │
        │ ·幂等键唯一约束  │   │ ·staging / persistent       │  │ 实例不在字节路径上 │
        │ ·租约（在途状态）│   │ ·tmp 名含实例标识            │  │                  │
        │ ·advisory lock  │   │                            │  │                  │
        │   （领导者选举） │   └────────────────────────────┘  └──────────────────┘
        └────────────────┘
```

### 4.1 共享状态放哪里

| 状态 | 单实例方案 | **多实例方案** | 理由 |
| --- | --- | --- | --- |
| 位置记录 | SQLite | **PostgreSQL** | 需要跨实例事务与唯一约束 |
| 元数据记录 | SQLite | **PostgreSQL** | 同上；也与 OSDU 参考实现的 baremetal 部署（`osm.postgres`）一致 |
| 在途租约 | 无 | **PostgreSQL 表** | GC 必须能看到其他实例的在途状态 |
| nonce（可选） | 内存表 | **PostgreSQL 表** | 或直接关闭该特性 |
| 单例任务归属 | 无 | **PostgreSQL advisory lock** | 不引入 etcd/consul 等新依赖 |
| 文件数据 | 本地盘 | **共享 POSIX 挂载** 或 **对象存储** | 本地盘在多实例下根本无法共享 |
| 配置 | 本地文件 | 本地文件（各实例一致）+ 版本校验 | 见 §4.5 |

> **明确禁止**：任何"数据库不可用时降级为本地 SQLite"的设计。那正是状态发散的起点。

### 4.2 幂等（M2 的修复）

```sql
-- 位置记录：幂等键 = (partition_id, file_id)
CREATE UNIQUE INDEX IF NOT EXISTS ux_loc_id
  ON file_locations(partition_id, file_id);

-- 元数据记录：幂等键 = (partition_id, file_source)，且只约束"未删除"的记录
CREATE UNIQUE INDEX IF NOT EXISTS ux_mr_source
  ON file_metadata_records(partition_id, file_source)
  WHERE state <> 'deleted';
```

`POST /v2/files/metadata` 的原子领取：

```sql
INSERT INTO file_metadata_records(partition_id, id, version, file_source, object_key, state, data)
VALUES (:partition, :new_id, 1, :file_source, :object_key, 'claiming', :data)
ON CONFLICT (partition_id, file_source) WHERE state <> 'deleted'
DO NOTHING
RETURNING id;
```

| 返回 | 含义 | 动作 |
| --- | --- | --- |
| 有行 | 本实例**赢得领取权** | 执行 staging→persistent 复制 → `UPDATE ... SET state='ready'` → 201 |
| 无行 | 其他实例已登记（或正在登记） | **不重复复制**；读取既有记录并按状态返回（`ready` → 返回同一 id；`claiming` → `409`/`400` 或短暂重试） |

**约束**：`state='claiming'` 的记录若因领取者崩溃而残留，必须由 GC 按**租约到期**回收
（不能直接当成 `ready` 返回给客户端）。

### 4.3 在途状态的租约（M3 的修复）

```sql
CREATE TABLE IF NOT EXISTS staging_leases (
  partition_id  TEXT NOT NULL,
  file_source   TEXT NOT NULL,
  owner         TEXT NOT NULL,          -- instance_id
  acquired_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
  expires_at    TIMESTAMPTZ NOT NULL,   -- 续租：now() + lease_ttl
  PRIMARY KEY (partition_id, file_source)
);
```

- **写入者**：`GetUploadLocation` 时建租约；长耗时操作（复制/校验和）期间**定期续租**（TTL/3）。
- **GC**：只处置"**租约已过期** 且 **无元数据记录**"的对象，并用
  `DELETE ... RETURNING` **原子领取**后再删文件（避免两个 GC 同时删/重复删）：
  ```sql
  DELETE FROM staging_leases l
   WHERE l.expires_at < now()
     AND NOT EXISTS (SELECT 1 FROM file_metadata_records m
                      WHERE m.partition_id = l.partition_id AND m.file_source = l.file_source)
  RETURNING l.partition_id, l.file_source;
  ```
- **时间基准**：所有租约/过期判定一律用**数据库的 `now()`**，不用实例本地时钟 → 消除时钟偏移（M6）。

### 4.4 单例任务（GC、配额汇总、指标聚合）

```sql
-- 会话级 advisory lock：拿到锁的实例才是当前 leader
SELECT pg_try_advisory_lock(:gc_lock_key);   -- 拿不到就跳过本轮，下轮再试
```

- 优先用 **PostgreSQL advisory lock**（已在 PG 14.24 与目标环境 PG 12.6 上实测），不引入 etcd/consul。
- **同时**要求 GC 本身幂等且用原子领取（§4.3）——**纵深防御**：
  即使选举失效、两个实例同时 GC，也不会误删。
- leader 需要**续期健康检查**。★ 措辞按实测收窄（见 `docs/test-evidence/phase10.md` §15.9）：
  "持锁实例退出时连接断开，锁自动释放"**只在持锁会话空闲（或只跑短语句）时成立**。
  实测（kill -9 客户端进程）：

  | 持锁会话被杀时的状态 | PG 14.24 | PG **12.6**（目标） |
  | --- | --- | --- |
  | 空闲（阻塞在客户端 socket 读） | 释放 9 ms | 释放 **51 ms** |
  | 正在跑 15 s 单语句 | 释放 633 ms（14 能察觉客户端消失并取消） | **≥8 s 仍未释放**，语句结束后才释放 |

  12.6 **没有** `client_connection_check_interval`（14+ 才有），因此崩溃后的锁滞留窗口 =
  **当前语句的剩余时长**。实现约束：① leader 的**锁连接与数据连接分离**；
  ② 锁连接**只跑短语句**；③ 每条语句带 `statement_timeout`（把滞留窗口夹到
  `statement_timeout_ms` 以内）——空闲态崩溃 51 ms 即可接管，可接受。

### 4.5 实例本地必须唯一化的东西

| 项 | 要求 |
| --- | --- |
| **tmp 文件名** | 必须含 `instance_id` + `pid` + 进程内递增计数 + 随机后缀（M1 的修复）。**绝不能**只用业务序号 |
| `instance_id` | 启动时生成（UUID）或从编排层注入（如 K8s 的 `POD_NAME`），并在日志/`/v2/info` 中暴露 |
| 数据面缓冲 | 每实例独立，无需共享 |
| 连接池 | 每实例独立；需按实例数**下调**总连接预算（见 §6.3） |

### 4.6 自签 token（M5 的修复）

| 项 | 要求 |
| --- | --- |
| 签名密钥 | **所有实例共享**（同 `signing_key`/`key_id`）；密钥轮换需支持多 key 并存（`key_id` 已在设计里） |
| `single_use_nonce` | **默认 `false`**；多实例下若要开启，nonce 必须写**共享存储**（PG 表或 Redis）。开启前必须明确"这是重放防护，还是仅仅审计" |
| token 载荷 | 必须绑定 `partition` + `op`；不需要绑定实例（任何实例都应能验证） |
| 时钟 | 过期判定容忍偏移（见 §6.4） |

---

## 5. 一致性模型与失败语义

### 5.1 一致性模型

| 层面 | 模型 |
| --- | --- |
| 控制面（位置/元数据记录） | **强一致**（PostgreSQL 事务 + 唯一约束；单主写入） |
| 数据面（文件内容） | **读写后一致**（read-your-writes）由**共享存储**保证；跨实例可见性依赖存储的 close-to-open 语义（见 §6.2） |
| 在途状态（租约） | 强一致（同一 PG），租约到期即视为可回收 |
| 单例任务 | 领导者选举（弱保证）+ 幂等（强保证），两者叠加 |

### 5.2 客户端可见的失败语义

| 情形 | 行为 |
| --- | --- |
| 重试 `POST /metadata`（同一 `fileSource`） | **幂等**：返回同一记录 id（不重复复制） |
| 重试 `getLocation`/`uploadURL`（同一 `fileID`） | 幂等或明确 `400 Location for fileID = <id> already exists`（对齐上游） |
| 请求落到任意实例 | 必须成功——实例不保存会话状态 |
| PostgreSQL 不可用 | **`503` + `Retry-After`**（fail-closed）。**禁止**降级到本地存储 |
| 共享存储不可用 | `503`；`readiness` 失败 |
| 实例 A 崩溃，其领取的 `claiming` 记录 | 租约到期后由 GC 回收，客户端重试可重新领取 |
| 客户端 ack 过的写入 | 由 ADR-008 的耐久性协议保证；多实例下每个实例对**自己**的批负责 |

### 5.3 `readiness` 的语义变更

多实例下 `readiness_check` 必须检查**共享依赖**，而不是只看本地：

| 检查 | 内容 |
| --- | --- |
| PostgreSQL | 可连接 + 一个低开销读（如 `SELECT 1`）+ 迁移版本匹配 |
| 共享存储 | 能创建/fsync/删除一个探针文件（不能只做 `stat`） |
| 配置一致性 | 与 PG 中记录的配置版本一致（防止版本混杂的滚动升级事故） |
| 本地 | 线程池/内存预算未越界 |

---

## 6. 不能保证的、以及残余风险

### 6.1 未验证项（如实标注）

| # | 项 | 状态 |
| --- | --- | --- |
| 1 | **NFS/SAN 上 `rename` 的原子性** | **未测**（本机无可挂载的 NFS）。协议要求 NFSv4；部署前必须在目标存储上验证 |
| 2 | **NFS 上 `fsync`/`syncfs` 的耐久性语义** | **未测**。ADR-008 的 P4 依赖 `syncfs` 覆盖全批数据；在 NFS 上 `syncfs` 的实际语义需要在目标环境验证 |
| 3 | **网络分区（脑裂）行为** | **未测**。PG 单主避免了数据分歧，但实例可能同时操作同一对象（由租约 + 原子领取兜底） |
| 4 | 多实例下 PG 的容量（连接数/QPS） | **未测**（本轮只验证正确性） |
| 5 | 对象存储模式下的多实例 | 实例不在字节路径上，天然友好；但元数据仍需 PG |

### 6.2 共享 POSIX 存储的语义假设（必须在部署前验证）

| 假设 | 为什么重要 |
| --- | --- |
| `rename(2)` 跨客户端原子 | M1 的修复依赖"最终名一次性出现"；NFSv3 在某些配置下不够可靠 |
| close-to-open 一致性 | 实例 A 写完 → 实例 B 立刻读，必须能看到（否则 `CreateFileMetadata` 会报"源文件不存在"） |
| `O_NOFOLLOW`/权限语义 | 路径安全（ADR-003）依赖这些；NFS 上的符号链接与权限行为需验证 |
| 文件锁（`flock`/`fcntl`） | **不要依赖**：NFS 上的锁语义历来脆弱。本项目改用 **PG advisory lock**，正是为了避开它 |

### 6.3 必须重新评估的容量参数

多实例会**成倍放大**外部依赖的负载，原先的容量结论需要重新计算：

| 参数 | 单实例 | 多实例注意 |
| --- | --- | --- |
| PG 连接数 | — | 实例数 × 池大小 ≤ `max_connections`；建议每实例池上限 16–32，并做总预算校验 |
| `syncfs` 频率 | 每批 1 次本机 flush | 若共享同一文件系统，**N 个实例各自 syncfs 会互相干扰**（ADR-008 §3.2 的担忧被放大）→ 强烈建议**按 partition 分盘** |
| GC 频率 | 单例 | leader 单跑；若退化为多实例并行，需限速 |

### 6.4 时钟偏移（M6）

| 依赖本地时钟的地方 | 多实例风险 | 修正 |
| --- | --- | --- |
| 租约到期判定 | 快钟实例会误回收别人的在途对象 | **改用 PG 的 `now()`** |
| token 过期 | 快钟实例让 token 提前失效 | 容忍一个可配偏移（默认 ±60 s），并在启动时检查偏移量 |
| `CreatedAt`（`getFileList` 输出） | 同一次列表里的时间可能乱序 | 用 PG 时间或明确接受 |

启动时检查：与 PG 的 `now()` 差值 > `clock_skew_tolerance` 则**拒绝启动并告警**。

### 6.5 显式不做的事

| 不做 | 原因 |
| --- | --- |
| 分布式共识（Raft/Paxos）自研 | 用 PG 单主 + 唯一约束 + advisory lock 已足够；自研共识是巨大的风险来源 |
| 多主写入同一 partition 的记录 | 会把"强一致"降级为"最终一致"，且冲突解决会引入难以测试的分支 |
| 跨实例共享内存缓存 | 会引入缓存一致性问题；宁可每请求打 PG（可加短期本地缓存但**必须设 TTL 且不可用于幂等判定**） |
| 依赖 NFS 文件锁做互斥 | 语义脆弱；一律用 PG advisory lock |

---

## 7. 备选方案与取舍

| 方案 | 优点 | 缺点 | 结论 |
| --- | --- | --- | --- |
| **A. 共享 PG + 共享 POSIX 存储 + 租约 + 领导者选举** | 强一致；任意实例可服务；与上游 baremetal 部署（`osm.postgres`）一致 | 引入 PG 依赖；共享存储成为单点（需存储层 HA） | **采纳** |
| B. 按 partition 分片归属（sticky routing） | 可保留本地 SQLite；无需 PG | 单 partition 失去 HA；需要路由层与再平衡逻辑；扩缩容时要迁移状态 | 记录为**小规模替代**（见 §8.4） |
| C. 共享 PG + 本地盘 + 对象存储网关 | 无共享文件系统依赖 | 集中存储模式本身要求共享文件系统；改成对象存储就回到另一种模式 | 作为"集中存储受限时的推荐迁移方向" |
| D. 事件溯源 + 最终一致 | 高写入吞吐 | 对文件服务而言"重复对象/记录"的冲突解决复杂且难测；M2 的强一致方案更简单可靠 | 否决 |

---

## 8. 配置与实现变更清单

### 8.1 新增/修改的配置项

```yaml
deployment:
  mode: single            # single | multi     ← 决定默认值与启动校验
  instance_id: ""         # 空则自动生成；K8s 下建议注入 POD_NAME
  clock_skew_tolerance_seconds: 60

metadata:
  repository: sqlite      # single 默认；multi 下**必须**为 postgres
  postgres:
    dsn: "${ENV:FSS_PG_DSN}"
    max_connections: 16
    statement_timeout_ms: 5000
    schema_version_check: true    # readiness 校验迁移版本

location:
  repository: sqlite      # multi 下必须为 postgres
  postgres: { dsn: "${ENV:FSS_PG_DSN}", max_connections: 8 }

leases:
  enabled: false          # multi 下必须为 true
  ttl_seconds: 60
  renew_interval_seconds: 20

leader_election:
  enabled: false          # multi 下必须为 true
  backend: postgres_advisory_lock
  lock_key: 0x4653535F4743    # "FSS_GC"

self_signed:
  single_use_nonce: false # multi 下保持 false（除非 nonce 入共享存储）

gc:
  require_lease_expiry: true   # 必须 true；禁止"无记录即删"
```

**启动校验（`deployment.mode=multi` 时强制）**：

1. `metadata.repository` 与 `location.repository` 必须是 `postgres`；
2. `leases.enabled` 与 `leader_election.enabled` 必须为 `true`；
3. `gc.require_lease_expiry` 必须为 `true`；
4. `storage.posix.root` 必须位于**共享挂载**上（启动时通过与 PG 协作的探测确认：写一个带 `instance_id` 的探针文件，检查是否能看到其他实例的探针）；
5. 与 PG 的时钟偏移必须在容忍范围内。

任一条不满足 → **拒绝启动**并一次性列出全部问题。

### 8.2 实现变更（相对当前设计）

| 位置 | 变更 |
| --- | --- |
| `ObjectKeyPolicy` | tmp 名加 `instance_id`+`pid`+计数；最终键不变 |
| `CreateFileMetadata` | 增加"原子领取"步骤（§4.2），领取成功才复制；`claiming`→`ready` 状态机 |
| `IFileLocationRepository`/`IMetadataRepository` | 新增 `postgres` 实现（与 sqlite 共用契约测试） |
| 新增 `ILeaseRepository` | 建租约/续租/原子领取 |
| `GC 任务` | 改为 leader-only + 租约到期 + 原子领取；`require_lease_expiry=true` |
| `ISelfSignedUrlCodec` | 签名密钥共享；nonce 可插拔（默认关闭） |
| `readiness` | 增加 PG / 共享存储 / 配置版本检查 |
| 时钟相关判定 | 租约与过期一律用 PG `now()`，或与 PG 校准 |

### 8.3 部署形态

| 项 | 建议 |
| --- | --- |
| 滚动升级 | 必须兼容**新旧版本并行**；配置版本写入 PG，`readiness` 校验不一致则不上流量 |
| 存储分盘 | **强烈建议每个 partition（或每组 partition）独立文件系统**，避免多实例 `syncfs` 互相干扰（ADR-008 §3.2 + §6.3） |
| PG 高可用 | 主从 + 自动故障转移（由运维承担，不在本项目范围） |
| 会话亲和 | **不需要**。若为性能而使用，必须保证正确性不依赖它 |

### 8.4 小规模替代方案（方案 B）的适用边界

如果只有 2–3 个实例且能接受"单 partition 无 HA"，可以按 `partition_id` 做路由分片，
让每个实例只服务固定 partition 集合并继续用本地 SQLite。**但必须**：

- 路由层保证同一 partition 的请求只落到一个实例（否则所有竞态照旧）；
- 有明确的"分片归属表"与再平衡流程（再平衡期间该 partition 不可用）；
- 承认扩缩容时需要停机或状态迁移。

**不推荐作为默认**：它把"一致性"换成了"路由正确性"，而路由层的 bug 同样难以测试。

---

## 9. 门槛（新增，插入实现计划）

| 编号 | 判据 |
| --- | --- |
| **C2.10** | `postgres` 与 `sqlite` 两种仓储实现**共用同一套契约测试**（含唯一约束/冲突语义） |
| **C6.11** | **幂等性测试**：并发 2 实例提交同一 `fileSource` → 只产生 1 条记录、只发生 1 次复制（复现 M2 的对照测试） |
| **C6.12** | **GC 租约测试**：在途对象在租约有效期内**不被删除**；租约过期且无记录时被回收；两个 GC 并发时用原子领取保证不重复删（复现 M3 的对照测试） |
| **C6.13** | **tmp 名唯一性测试**：2 实例并发写同一业务序号 → 无内容错乱（复现 M1 的对照测试） |
| **C8.9** | `deployment.mode=multi` 的启动校验：7 条强制项各有一个"拒绝启动"的测试（B1 起校验通过后还会真的装配 PG 运行形态） |
| **C8.10** | 时钟偏移：模拟快/慢钟实例 → 租约不误判（使用 PG `now()`） |
| **C9.26** | 多实例端到端：2 个真实进程 + 共享 PG + 共享目录，跑完整上传→登记→下载→删除流程，并注入实例崩溃 |
| **C9.27** | **在目标存储上验证 NFS 语义**：`rename` 原子性、close-to-open 一致性、`syncfs` 耐久性（§6.1/§6.2 的未验证项） |
| **C9.28** | PG 连接预算校验：实例数 × 池上限 ≤ `max_connections`，超限拒绝启动 |

---

## 10. 待办

- [x] 实现 `PostgresLocationRepository` / `PostgresLeaseRepository`（L2 + libpq 薄封装；与内存/SQLite 共用 `tests/framework/port_contract.h` 的同一套契约测试；已在 PG 14.24 与 12.6 实测。证据：`docs/test-evidence/phase10.md` §15）
- [x] 实现 `PostgresMetadataRepository`（上一项里 metadata 那一半）：`file_metadata_records` 的 L2 实现，读路径过滤 `state <> 'deleted'`，`Create` 用 `ON CONFLICT (partition_id, file_source) WHERE state <> 'deleted' AND is_latest DO NOTHING` 做**单条 INSERT 的原子领取**；与内存/SQLite 共用 `tests/framework/port_contract.h` 的同一套 `CheckMetadataRepositoryContract`，并新增"并发 Create 同一 fileSource → 恰好 1 行 / 所有调用者同一个 id"的判据。已在 PG 14.24 与 12.6 实测。证据：`docs/test-evidence/phase10.md` §16
- [x] 实现 `CreateFileMetadata` 的**跨步骤**原子领取 + `claiming`→`ready` 状态机（C1：端口三原语 `ClaimForWrite`/`MarkReady`/`ReleaseClaim`，用例**先领取再复制**（并发同一 `fileSource` 只复制一次），`claiming` 期间第二个调用者有界等待 ~2s 后 **503 且不复制**；`state` 只做仓储列、读路径只返回 `ready`。证据：`docs/test-evidence/phase10.md` §18）
- [ ] **崩溃者留下的 `claiming` 行按租约到期回收**（C1 的剩余一半：需要上传路径的租约 `Acquire`/`Renew` + GC 侧的回收；在此之前一个崩溃的领取者会让该 `fileSource` 一直走"2s 后 503"路径，直到有人 `ReleaseClaim`/`Delete`）
- [x] 实现 leader election（PG advisory lock）+ GC 的租约与原子领取（B1：`src/infra/postgres/pg_leader_election.*` + 组合根门控 `GcScheduler`/`GcCallbacks`；PG 租约仓储见 §15。证据：`docs/test-evidence/phase10.md` §17）
- [x] 实现 `deployment.mode=multi` 的 7 条启动校验 + 组合根装配 PG 仓储/租约/leader election（B1）。⚠️ **PG-vs-本地时钟比较仍未实现**：`deployment.clock_skew_tolerance_seconds` 仍是非默认即 exit 78 的守卫
- [x] tmp 名唯一化（B1 补齐 ADR-009 §4.5 的**每进程随机后缀**：`.tmp.<instance_id>.<pid>.<counter>.<random>`；判据 `tests/integration/test_posix_tmp_names.cpp`）。⚠️ 最终键的 `ObjectKeyPolicy` 生成规则未变
- [ ] 在目标环境验证 NFS 语义（C9.27）——这是**上生产前的硬前提**
- [ ] 更新 `docs/operations.md`：多实例部署、PG 高可用、分盘建议、滚动升级与配置版本
