# ADR-006 受控复核（C9.12）：httplib 内容提供者 vs 裸 socket + `sendfile(2)`

| 项 | 值 |
| --- | --- |
| 判据 | `docs/04-implementation-plan.md` **C9.12**：用同一负载生成器对比，倍数 ≥ 1.5x → 定稿 ADR-006（实现 sendfile 数据面）；< 1.5x → 放弃，统一用 httplib |
| 被测对象 | `--mode httplib`（产品路径 `fss::http::Server` + `Response::Stream`）vs `--mode sendfile`（裸 socket + `sendfile(2)` 原型） |
| 工具 | `bench/sendfile_ab.cpp`、`scripts/bench_sendfile_ab.sh`、客户端 `bench/capacity_bench.cpp`（`fss_bench_capacity load`） |
| 状态 | ✅ 已完成（2 次完整运行 + 1 个补充点位；全部点位 `errs=0`） |
| 结论 | **倍数 2.12x（run 1）/ 2.02x（run 2）→ ≥ 1.5x → 值得实现 sendfile 数据面**（带下方诚实声明的边界条件） |

---

## 1. 实验目的与判据

数据面（大文件下载）当前走 `src/common/http/http.h` 的 `fss::http::Server` +
`Response::Stream(...)`，底层是 cpp-httplib 的 `set_content_provider`：文件字节必须
`pread` 到用户态 64 KiB 缓冲再 `sink.write()` 进 socket。ADR-006 要决定是否值得为集中存储
**再实现一条 `sendfile(2)` 数据面**。

判据 **C9.12**：用**同一个负载生成器**对比两条路径，得到**可复现的倍数**；

* 倍数 **≥ 1.5x** → 结论：**值得实现 sendfile 数据面**；
* 倍数 **< 1.5x** → 结论：**放弃，统一用 httplib 内容提供者**。

本文件只记录实验与数字，不改产品代码、不改 ADR。ADR 的定稿由 P9 负责人据此完成。

---

## 2. 实验方法

### 2.1 环境与拓扑

| 项 | 值 |
| --- | --- |
| 环境 | `uname -sr` = `Linux 6.18.33.2-microsoft-standard-WSL2`；16 核；内存 7 GiB；**WSL2 + 虚拟盘**；无 root |
| 服务端绑核 | `taskset -c 4-7`（4 核），脚本启动时断言与客户端核集合**不重叠** |
| 客户端绑核 | `taskset -c 8-15`（8 核） |
| 铁律 R2 | 服务端与客户端是**两个进程**（`fss_bench_sendfile_ab` / `fss_bench_capacity load`），分别 `taskset` 绑核；**没有**进程内压测 |
| 网络 | `127.0.0.1` loopback |
| 负载文件 | 256 MiB（`268435456` 字节），**非稀疏**（伪随机内容真实写入 + `fsync`），落在 `build/ab-run/ab-blob-268435456.bin` |
| 页缓存 | 每个点位前用 `dd if=<file> of=/dev/null bs=1M` 把文件**预热进页缓存**，两种模式起跑状态相同（见 §5 诚实声明：本实验量的是"页缓存已热"的上界） |
| 负载生成器 | `build/bin/fss_bench_capacity load --mode get --url http://127.0.0.1:<port>/blob`（仓库现成的独立进程生成器） |
| 时长 | 每点位 `--duration-ms 4000`（计时 4 s），`--warmup-ms 800`（预热不计入） |
| 连接数 | `1`、`4`、`16` |

### 2.2 协议与安全性标注（铁律 R3）

* 两侧都是 **HTTP/1.1**、显式 **`Content-Length`**（= 268435456，精确）、**无 chunked**；
* 客户端 `Connection: keep-alive`；httplib 侧响应头 `Keep-Alive: timeout=5, max=1000000`
  （`keep_alive_max_count` 从默认 100 调大，避免服务端主动断连把协议行为计成 `errs`——
  AGENTS §4.2 陷阱清单）；
* **TCP_NODELAY 双端开启**（客户端 `Conn::Connect` 与两种服务端都 `setsockopt`）——
  httplib 默认 `false` 会造成 950x 级损失（AGENTS §4.2）；
* **没有**任何"跳过校验"的捷径：本实验两侧都**不包含**签名校验，因为对比的是**数据面路径本身**
  （httplib 的 `Server` + `Response::Stream` vs `sendfile`）；两侧用同一个入口
  `GET /blob` 服务同一个文件；
* **sendfile 侧是原型**：只支持 `GET /blob`，**没有** Range / 鉴权 / 限流 / 访问日志 / 超时 /
  连接上限 / HEAD / 压缩 / 流水线。**它测的是 sendfile 路径的上界（upper bound），
  不是一个可上线的数据面。**

### 2.3 两侧的装配（唯一差异就是数据面路径）

| | `--mode httplib`（产品路径） | `--mode sendfile`（原型） |
| --- | --- | --- |
| 监听/解析 | `fss::http::Server`（cpp-httplib 0.26 + `fss_http` 包装层） | 裸 socket：`accept` + 手写请求头解析（直到 `\r\n\r\n`） |
| 路由 | `server.Get("/blob", RouteOptions{.name="ab_blob"}, handler)` | 固定 `GET /blob`（其余方法/路径 → 405/404 并断连） |
| 响应头 | 库根据 `set_content_provider(total, ...)` 生成 `Content-Length` | 手写固定头（`HTTP/1.1 200 OK` + `Content-Type` + `Content-Length` + `Connection: keep-alive`） |
| 字节搬运 | `fss_http` 的 `StreamPump`：每 64 KiB 一次 `pread` → `sink.write()` → socket（2 次拷贝） | `sendfile(out_fd, in_fd, &offset, remaining)` 循环，**字节不进用户态** |
| 并发上限 | `worker_threads=4`、`max_connections=4` | **4** 个阻塞式 `accept`/连接处理线程（与 httplib 侧相同） |
| `TCP_NODELAY` | `ServerOptions.tcp_nodelay=true` | 每个 `accept` 的 fd 上 `setsockopt(TCP_NODELAY)` |
| 其它 | 4 线程；`bind 127.0.0.1:0`；`keep_alive_max_count=1e6` | backlog 512；`SO_RCVTIMEO` 1 s 让 accept 能退出 |

两侧都**不含** Router / 用例 / 自签 token 校验 / POSIX 存储查表 —— 本实验隔离的正是"数据面路径"。
（这也是本实验与 `scripts/bench_baseline.sh` 的 `large_stream_*` 的差别：后者是含 token 校验的完整产品路径。）

---

## 3. 原始数字

比值 = 同一连接数下 `sendfile.mibps / httplib.mibps`。
`cpu_pct` = 生成器进程自报的 `client_cpu_pct`（客户端 8 核 → 上限 800%）。

### 3.1 主实验 run 1（256 MiB / 4 s / 连接数 1,4,16）

| mode | conns | mibps | p99_us | errs | cpu_pct | rps | reqs | sf/hl |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| httplib | 1 | 2387.4 | 93896 | 0 | 38.9 | 9.3 | 38 | — |
| **sendfile** | 1 | **5487.7** | 39040 | 0 | 108.7 | 21.4 | 86 | **2.30x** |
| httplib | 4 | 7320.5 | 137789 | 0 | 348.8 | 28.6 | 117 | — |
| **sendfile** | 4 | **14784.5** | 89921 | 0 | 433.5 | 57.8 | 233 | **2.02x** |
| httplib | 16 | 6989.9 | 130520 | 0 | 355.8 | 27.3 | 122 | — |
| **sendfile** | 16 | **14273.9** | 67635 | 0 | 431.8 | 55.8 | 234 | **2.04x** |

**总倍数（3 个点位 mibps 比值的几何平均）= `2.116x`**（中位数 2.04x）

原始 RESULT 行（生成器 stdout，逐字）：

```text
label=httplib_c1  mode=get conns=1  dur_s=4.07 reqs=38  errs=0 rps=9.3  mibps=2387.4  p50_us=84863  p99_us=93896  client_cpu_pct=38.9
label=sendfile_c1 mode=get conns=1  dur_s=4.01 reqs=86  errs=0 rps=21.4 mibps=5487.7  p50_us=37175  p99_us=39040  client_cpu_pct=108.7
label=httplib_c4  mode=get conns=4  dur_s=4.09 reqs=117 errs=0 rps=28.6 mibps=7320.5  p50_us=107771 p99_us=137789 client_cpu_pct=348.8
label=sendfile_c4 mode=get conns=4  dur_s=4.03 reqs=233 errs=0 rps=57.8 mibps=14784.5 p50_us=53266  p99_us=89921  client_cpu_pct=433.5
label=httplib_c16 mode=get conns=16 dur_s=4.47 reqs=122 errs=0 rps=27.3 mibps=6989.9  p50_us=105327 p99_us=130520 client_cpu_pct=355.8
label=sendfile_c16 mode=get conns=16 dur_s=4.20 reqs=234 errs=0 rps=55.8 mibps=14273.9 p50_us=53560 p99_us=67635  client_cpu_pct=431.8
```

### 3.2 复现 run 2（同一命令重跑，独立新进程）

| mode | conns | mibps | p99_us | errs | cpu_pct | rps | reqs | sf/hl |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| httplib | 1 | 2666.8 | 80022 | 0 | 40.1 | 10.4 | 42 | — |
| **sendfile** | 1 | **6058.6** | 36283 | 0 | 108.7 | 23.7 | 95 | **2.27x** |
| httplib | 4 | 7867.2 | 158159 | 0 | 345.2 | 30.7 | 124 | — |
| **sendfile** | 4 | **14156.4** | 72011 | 0 | 431.7 | 55.3 | 224 | **1.80x** |
| httplib | 16 | 7133.4 | 138028 | 0 | 359.7 | 27.9 | 123 | — |
| **sendfile** | 16 | **14427.5** | 65262 | 0 | 418.4 | 56.4 | 238 | **2.02x** |

**总倍数 = `2.022x`**（中位数 2.02x）

两次运行的逐点比值：`2.30 / 2.02 / 2.04`（run 1）与 `2.27 / 1.80 / 2.02`（run 2）→
**可复现；点位间散布约 ±10%（c4 最大），但都 > 1.5x。**

### 3.3 补充点位：16 线程 × 16 连接（用于解释 c16 的 4 线程上限，非 C9.12 主表）

主表两侧都限制为 **4 线程 / 4 连接**，因此连接数 16 的点位只有 4 条连接真正在跑
（证据：c16 的 `reqs` 与 c4 几乎相同 —— `122 vs 117`、`234 vs 233`），其余 12 条 keep-alive
连接要么在 httplib 的任务队列里、要么在内核 accept 队列里排队。为了确认这不是客户端行为，
把**两侧并发上限同时提到 16** 另跑一个点位：

| mode | conns | 线程 | mibps | p99_us | errs | cpu_pct | rps | reqs | sf/hl |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| httplib | 16 | 16 | 10241.8 | 466222 | 0 | 354.0 | 40.0 | 170 | — |
| sendfile | 16 | 16 | 20980.6 | 273509 | 0 | **864.9** | 82.0 | 337 | 2.05x |

两点说明：

* 16 条连接真并发后两侧绝对吞吐都上去了（7.0→10.2 GiB/s、14.3→21.0 GiB/s），证实主表 c16
  的低绝对值来自**服务端 4 线程上限**，而不是客户端或路径本身；比值仍是 **2.05x**。
* sendfile 这一行 `client_cpu_pct=864.9` ≥ 8 核 × 100 的 90% → **该点位的绝对数字标注为
  "受客户端限制"，按 R4 属"无结论"**（比值 2.05x 因此是偏保守的下界：客户端被限住只会低估
  sendfile 侧）。这也是脚本 `[受客户端限制]` 标注的**自证**：该标注确实会触发，不是恒假。

---

## 4. 结论

1. **倍数：2.12x（run 1）/ 2.02x（run 2）**（3 个连接数点位的几何平均；逐点 1.80x ~ 2.30x）。
2. **≥ 1.5x → 判据 C9.12 给出的结论是「值得实现 sendfile 数据面」。**
3. 据此的**建议（分层）**：
   * ADR-006 按 C9.12 可以**定稿**（实现自持 socket + `sendfile` 数据面），但 ADR 里必须写明：
     受控实测的收益是 **~2x**，**不是**早先非受控探针的 5–7x（`docs/05-capacity-and-concurrency.md`
     §1.7 当时就标注了「这两行来自不同探针、不同客户端实现，**不是严格受控对比**」，
     本次受控复核给出了可复现的 2x）；
   * 这 2x 是**原型上界**：可上线的数据面还要付 Range / 鉴权 / 限流 / 超时 / 访问日志 /
     连接上限的成本（原型一个都没有），实际净收益必然低于 2x；
   * 若短期只想要"一部分收益"，还有一个**本实验未测**的廉价备选：把 `StreamPump` 的
     64 KiB 块调大（或改为按大块 `writev`/更大缓冲）—— 属于改产品代码，不在本次允许范围内，
     **未实测、不作结论**；
   * 若最终目标环境是**冷页缓存 / 真实网卡**，路径差异会被磁盘带宽或网络带宽盖住：
     本实验无法覆盖（见 §5），ADR 应把它列为"需在目标硬件复测"的前提。
4. 数字的**稳健部分**是"同一台机器、同一次会话、同一负载生成器、两侧紧邻测量"的**比值**；
   绝对 MiB/s 只是本机量级参考。

---

## 5. 诚实声明（务必与数字一起引用）

1. **本机是 WSL2 + 虚拟盘**：绝对吞吐（httplib 7.3 GiB/s、sendfile 14.8 GiB/s @4 并发）只作
   **本机量级参考**；生产容量必须在目标硬件/网卡上复测（C9.14，本环境不具备条件）。
2. **文件全程在页缓存里**：每个点位前显式 `dd` 预热，且 256 MiB < 可用内存。因此本实验测的是
   **"页缓存已热"的路径成本（CPU 拷贝/系统调用数）**，**不包含磁盘 I/O**。
   冷缓存或真实存储下磁盘会成为主导项、差距可能被抹平 → **本实验对"真实磁盘上的 sendfile 收益"无结论**。
3. **绝对数字是否已被内存带宽饱和：无法区分**（R4）。sendfile 侧在 4 并发下到 14–15 GiB/s 后不再
   上升，但在本环境下（无 root，不能 `drop_caches`、不能做 NUMA/绑内存）无法把
   "路径成本触顶"与"内存/loopback 带宽触顶"分开 → **绝对值无结论**；
   可复现的**比值**是两侧在同一条件下紧邻测得，不受此影响。
4. **sendfile 侧不是可上线的数据面**：原型只支持 `GET /blob`，**没有** Range / 鉴权 / 限流 /
   超时 / 访问日志 / 连接上限 / HEAD / 压缩 / 流水线；响应头是写死的。所以这是
   **"上界对照（upper bound）"，不是"实现对比"**。
5. **两侧都不含 token 校验与 Router/用例层**，对比的是纯数据面路径；真实产品路径
   （`scripts/bench_baseline.sh` 的 `large_stream_*`，含自签 token 校验）绝对吞吐更低
   （64 MiB 时 c4 = 3394.8 MiB/s），但本实验**不能**直接把这部分差值归给数据面路径。
6. **客户端受限点位已按 R4 标注**：补充点位的 sendfile/c16（`client_cpu_pct=864.9%`）标注为
   "受客户端限制 → 该点位无结论"。主表 6 个点位 `client_cpu_pct` 最高 433.5%/800%，
   **未被客户端限住**；且 6 个点位 `errs` 全为 0（且 `reqs>0`）。
7. **主表 c16 的吞吐不是"16 并发"**：两侧并发上限都是 4，c16 只有 4 条连接在跑（`reqs` 与 c4
   几乎相同），其余 12 条在排队；该点位反映的是"4 活跃流 + 12 排队"，两种模式**对称**。
   真正的 16 并发见 §3.3（那里 sendfile 侧被客户端限住 → 绝对数字无结论）。
8. 本文件**不写任何超出数据的断言**；ADR 的"是否实现"决策与 `docs/00-final-design.md` §5
   的旧结论更新由 P9 负责人按本报告完成。

---

## 6. 复现命令

```bash
cd /home/ll/fssvrcpp

# 1) 配置 + 构建 A/B 服务端与仓库自带的负载生成器（两个目标都是 EXCLUDE_FROM_ALL）
cmake -S . -B build
cmake --build build --target fss_bench_sendfile_ab fss_bench_capacity

# 2) 主实验（256 MiB / 4 s / 连接数 1,4,16；服务端 taskset 4-7、客户端 taskset 8-15）
./scripts/bench_sendfile_ab.sh

# 3) 重复一次（可复现性；默认会在结束时删掉 256 MiB 负载文件，加 KEEP_BLOB=1 可保留）
FSS_AB_KEEP_BLOB=1 ./scripts/bench_sendfile_ab.sh

# 4) 补充点位：16 线程 × 16 连接（解释 c16 的 4 线程上限；预期 sendfile 点位被标 [受客户端限制]）
FSS_AB_THREADS=16 FSS_AB_CONNS=16 FSS_AB_KEEP_BLOB=1 ./scripts/bench_sendfile_ab.sh

# 5) 冒烟（64 MiB / 2 s，**不作最终数字**）
./scripts/bench_sendfile_ab.sh --quick

# 6) 复核产物（服务器 stdout/stderr、逐点位客户端原始行、TSV）
cat build/ab-run/server-httplib.out  build/ab-run/server-httplib.err
cat build/ab-run/server-sendfile.out build/ab-run/server-sendfile.err
cat build/ab-run/load-*.log
cat build/ab-run/results.tsv
```

可覆盖的环境变量（默认值）：`FSS_AB_SERVER_CPUS=4-7`、`FSS_AB_CLIENT_CPUS=8-15`、
`FSS_AB_SIZE_BYTES=268435456`、`FSS_AB_DURATION_MS=4000`、`FSS_AB_WARMUP_MS=800`、
`FSS_AB_THREADS=4`、`FSS_AB_CONNS="1 4 16"`、`FSS_AB_KEEP_BLOB=0`。

本报告对应的产物（已复核，`errs=0`，两侧服务端日志只有 `FILE/READY/STOP` 三类行、无错误）：

* `build/ab-run/run1/`（run 1：`full-run.log`、`results.tsv`、两个服务器 `.err`）
* `build/ab-run/run2/`（run 2：同上）
* `build/ab-run/supp/`（补充 16×16：`supp-16x16.log`、`results.tsv`、两个服务器 `.err`）

> 注：`build/` 不纳入版本控制；上表目录是本次运行的留档，重跑脚本即可重新生成。
