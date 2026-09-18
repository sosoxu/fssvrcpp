# 阶段 10 测试证据（✅ 切片 1/2/3 全部完成：配置面接线 + GC/expiry + 审计 fail-closed/SQLite 调优/鉴权与 gRPC 面 + C10.16）

| 项 | 值 |
| --- | --- |
| 阶段 | P10（配置面接线） |
| 状态 | ✅ **切片 1/2/3/4/5 全部完成 + C10.16 + C10.16 续**：C10.1~C10.17 满足；**切片 4** 接通数据面 PUT 上限与 SQLite PRAGMA（见 §9）；**切片 5（C10.17）** 接通 `self_signed` 三键（见 §10） |
| 门槛命令 | `ctest -L phase10 && scripts/verify_config_wiring.sh` |
| 退出码 | `0` |
| 新测试 | `tests/integration/test_config_wiring.cpp`：**31 个 TEST_CASE / 692 断言**（真实 `build/bin/fss_server` + 驱动层；切片 5 新增 2 用例 / 81 断言，父代理 §10.6 再 +3 断言）+ `tests/unit/test_transfer_token_key_id.cpp`：**4 个 TEST_CASE / 29 断言**（C10.17 codec 边界） |
| 脚本 | `scripts/verify_config_wiring.sh`：**54 条断言**（切片 1 的 22 条 + 切片 2：GC 调度 4 + `--once` 3 + 样例配置启动/拒绝 6 + C10.11 拒绝 15 + expiry 3 + 就绪 1） |
| 全阶段门槛 | `./scripts/run_all_gates.sh` → **P0~P10 全绿，总耗时 348 s（5 分 48 秒，11 个阶段）**（含 ASan+UBSan 全量）；`ctest` **77/77** 通过 |
| sanitizer | `run_sanitizers.sh` 已自动纳入 `phase10`（`✓ phase10 在 sanitizer 下通过`） |
| 配置键三态 | `config/fss.example.json` **156** 个叶子键：**生效 96 / 拒绝启动（触发条件）21 / 已读但无效果 39**（切片 3 新接通 9 键；**C10.16** 接通 1 键 + 2 键改为拒绝启动；**C10.16 续**接通 7 键 + 3 键改为拒绝启动；**切片 4** 接通 4 键；**切片 5** 接通 3 键）（逐键见 `docs/operations.md` §1.2 的"接通状态"列与 §1.3 的三个清单；由 §1.2 的 156 行程序化核对得出，`test_operations_doc` 机械断言） |
| 切片 2 新增/修改 | `src/infra/location/memory/memory_lease_repository.{h,cpp}`（单实例内存租约）、`src/app/services/expiry_policy.{h,cpp}`（`ExpiryOptions` 重载 + `ParseExact`）、`src/app/services/location_issuer.{h,cpp}`、`src/main/server_main.cpp`、`src/CMakeLists.txt` |

---

## 1. 交付物

| 路径 | 内容 |
| --- | --- |
| `src/main/server_main.cpp` | 组合根接入 `fss::config::Load`（不重写加载器）：`--config`/`--set`/`--print-config`、优先级决议（cli > env 通用名 > 旧别名 env > file > 历史默认值）、`Resolver`（来源记录 + 旧别名 + 历史默认值）、C10.3~C10.6 的逐键接线、exit 78 失败语义、启动横幅来源打印 |
| `src/common/config/core_schema.cpp` | ① `auth.jwt.hmac_secret` 标为 **secret**（`--print-config`/诊断一律 `***`）；② production 要求 `auth.mode=jwt`（含 `remote-entitlements` 也拒绝）；③ 放宽 `storage.posix.fsync_threshold_bytes` 范围 `[0, 2^40]`（原先只允许 `0`，会让 `batch` 退化成 per_file） |
| `config/fss.example.json` | `storage.posix.fsync_threshold_bytes` 示例值 `0 → 1048576`（阈值 0 的语义是"所有对象都 fsync"，与 `batch` 的注释相矛盾；示例现在是"抄了就对"的） |
| `tests/framework/server_process.h` | 扩展（**不破坏既有使用者**，默认构造行为逐字不变）：`ServerProcessOptions`（追加 `--config`/`--set`/环境变量、可关默认 `FSS_HTTP_PORT=0`、可不等 gRPC 端口）、`RunServerForExit()`（退出码 + 合并输出，`timeout` 兜底） |
| `tests/integration/test_config_wiring.cpp` | 新增 phase10 用例（见 §2） |
| `tests/CMakeLists.txt` | 注册 `test_config_wiring`（`LABELS "phase10;integration"`） |
| `scripts/verify_config_wiring.sh` | 真实二进制 + 临时配置文件的 22 条断言；工作目录在 `build/` 下、`trap` 清理、不用 `\| head` |
| `scripts/run_all_gates.sh` | `IMPLEMENTED_PHASES` 加 `10`；循环加 `10`；phase10 挂 `verify_config_wiring.sh` |
| `docs/operations.md` | §0/§1.1（来源与优先级 + CLI 说明）、§1.2 逐键"读取方式"（66 键改为已接通）、§1.3 未接通清单（90 键）、§1.4 已接通清单 + 旧别名表 + 默认值分歧表、§2.1 启动/退出码、§3.1 `fss_io_engine`、§3.2、§5.1、§7、§8 |
| `docs/04-implementation-plan.md` | 阶段表新增 P10（🚧 切片 1 完成）；阶段 10 一节补状态；§9「首个动作」改为指向阶段 10 后续切片 |
| `README.md` | 快速开始补一句"配置来源与优先级见 `docs/operations.md` §1" |
| `docs/02-design.md` | §16 的"P9 遗留 ① 组合根只读环境变量"更新为"阶段 10 切片 1 已修"（R13） |
| `Dockerfile` | 注释更新：组合根现在**会**读 `--config`，但镜像 entrypoint 不传，因此生效的仍是 `FSS_*` |

---

## 2. 判据进展（C10.1~C10.8）

| 判据 | 状态 | 证据 |
| --- | --- | --- |
| **C10.1** `--config` / `--print-config` / 拒绝启动 | ✅ | `test_config_wiring`：配置文件端口 → 该端口 readiness 200；`--print-config` 退出 0 且 `auth.jwt.hmac_secret = ***`、输出**不含明文** `SUPERSECRET-VALUE`、逐键打印来源；未知键（文件里 `server.http.prot`、`--set nope.key=1`）→ **exit 78** 且逐条给路径；配置文件不存在 → 78；未知命令行参数 → 非 0（2）并打印用法。脚本 5 条对应断言 |
| **C10.2** 优先级与来源可见 | ✅ | `test_config_wiring` 的三个 SECTION：file vs `--set` → cli 赢；file vs `FSS_HTTP_PORT`（旧别名）→ env 赢；file + `FSS_HTTP_PORT` + `FSS_SERVER_HTTP_PORT` → **通用名赢**且日志给出歧义告警。横幅逐键打印 `cli/env/file/default`，`--print-config` 打印同样来源。脚本 3 条断言 |
| **C10.3** `server.http.*` 接通 + 非法值拒绝 | ✅ | port/worker_threads（`/metrics` 的 `fss_http_worker_threads 7`）/bind/base_path/三个超时/缓冲/预算/max_*_bytes/tcp_nodelay/max_connections 全部读到 `http::ServerOptions`/`RouterOptions`；构造 `Server` **前**调用 `http::ValidateOptions`；`max_connections=8 > worker_threads=4` → **exit 78**；`transfer_buffer × workers > budget` 由 schema 跨字段拒绝；`--set storage.posix.durability=never` → 78（schema 面更严，旧别名仍接受 `never`） |
| **C10.4** `storage.*` 接通 | ✅ | driver/posix.root/durability/fsync_threshold/instance_id/io_engine/io_uring.queue_depth + `storage.s3.*`（endpoint/region/access_key/secret_key/force_path_style/verify_tls/connect_timeout_ms/total_timeout_ms/presign_default_seconds/presign_max_seconds）+ `driver_report_override`/`provider_key_override` 全部接线；`io_engine=uring` → **exit 78** + `scripts/check_io_uring.sh` 提示；`io_engine=auto` → 回退 blocking，横幅含 `io engine ... 已回退`，`/metrics` 出现 `fss_io_engine{engine="blocking",requested="auto"} 1`（R11）；`metadata.repository`/`location.repository` 非 sqlite → 78 |
| **C10.5** `auth.*` 接通 + 生产强校验 | ✅ | `auth.mode=jwt`（配置文件）→ `/v2/info` 的 `"authMode":"jwt"`、无 token 与伪造 token 的 `uploadURL` = **401**；正例对照：`auth.mode=disabled` 时带 token 的同一请求 = 200（R16）。`production` + `disabled` → **exit 78** 且输出含 `production`；`production` + `remote-entitlements` → 78；`production` + 关验签 / 空密钥 → 78（schema 跨字段，`test_config` 已有用例） |
| **C10.6** `observability.*` 接通 | ✅ | `log_level`/`log_format`/`redact_keys` → `logging::OptionsFromConfig`；`log_service` 不在 schema → 用组合根专用 `FSS_LOG_SERVICE`（文档注明）；`audit_enabled=false` → no-op 审计器；`metrics_enabled=false` → `/metrics` 返回 **404**；`metrics_path=/internal/metrics` → 新路径 200、老路径 404；`audit_fail_closed` **已读但行为未实现**（见 §4） |
| **C10.7** 未接通清单收敛并机械化 | ✅ | `docs/operations.md` §1.3 = **90** 个未接通键、§1.4 = **66** 个已接通键（合计 156）；`test_operations_doc` 继续通过（156 键正向覆盖 + 反向 schema 校验）；`test_config_wiring` 对 C10.3~C10.6 声明接通的键在**真实进程**上断言生效 |
| **C10.8** 自证（R1） | ✅ | 见 §3：去掉配置文件层后 **8/10 用例、12 条断言失败**；恢复后全绿。并保留"env 覆盖 file"的正例（`FSS_HTTP_PORT` 覆盖文件端口） |

---

## 3. 自证（R1）：去掉配置加载 → 新测试必须失败

* **注入**：`src/main/server_main.cpp` 里 `load_request.file_path = config_path;`
  → `load_request.file_path.clear();`（等价"退回只读环境变量 + 只有 `--set`"）。
* **重建后运行**：`./build/bin/test_config_wiring` → **退出码 12**，
  `test cases: 10 | 2 passed | 8 failed`，`assertions: 89 | 77 passed | 12 failed`。
  失败的用例（节选）：
  * `C10.1/C10.3 配置文件端口`：`REQUIRE(server.http_port() == from_file)`（退回 8080）；
  * `C10.3 worker_threads`：`fss_http_worker_threads` 不等于 7（退回公式值）；
  * `C10.5 auth.mode=jwt`：`/v2/info` 不含 `authMode=jwt`（退回 disabled）；
  * `C10.1/C10.5 反向：production+disabled`：`outcome.output.find("production")` 找不到
    （配置没被读 → 进程不再拒绝启动）。
* **恢复**：备份还原该行 → 重建 → `./build/bin/test_config_wiring` **全绿（137 断言）**；
  `run_all_gates.sh` 的注入残留前置检查通过（`git diff -- src` 无注入标记）。

---

## 4. 未做到 / 降级处理（如实登记）

| 项 | 状态 |
| --- | --- |
| `observability.audit_fail_closed` | **已读但行为未实现**：`RecordAudit()` 丢弃 `IAuditLogger::Record` 的结果，没有"致命审计"路径。启动横幅会标注 `fail_closed=true（★该行为未实现，仅登记）`，`operations.md` §8 登记 |
| `observability.log_service` | **不在 schema 内**（`CoreSchema` 与示例都没有该键，加键会让 156 计数与自动比对失真）；用组合根专用环境变量 `FSS_LOG_SERVICE` 表达，已在 §1.4 注明 |
| `storage.posix.instance_id` | 任务书写作 `posix.instance_id`，但 schema 里唯一的实例标识键是 `deployment.instance_id`（临时名里的实例标识）；组合根接的是后者，已在 §1.2.1 注明 |
| `storage.io_engine=uring` | 内核探测通过时也**拒绝启动** —— 因为 `UringIoEngine::enabled()` 恒为 `false`（ADR-010 U1~U4 未满足）。"可用"的定义是"探测通过 **且** 引擎实现已启用"，避免"以为开了加速" |
| `storage.proxy_mode`、`gc.*`、`server.http.large_file_plane.*`、`expiry.*`、`legal.*`、`schema.*`、`events.*`、`partition.*`、`leases.*`、`leader_election.*`、`metadata/location` 的非 path 键 | **仍未接通**（90 键，逐项在 §1.3）—— 本阶段不承诺 |
| 旧别名的布尔语义 | `FSS_JWT_VERIFY_SIGNATURE` 等沿用接线前语义（**只有字面 `false` 才为假**），不改成 `ParseBool` 的全部别名（如 `0`/`no`）—— 为了"不破坏既有行为"；已在 §1.4 注明 |
| `storage.posix.durability=never` | schema enum 未含 `never`（配置面拒绝），只有旧别名 `FSS_POSIX_DURABILITY=never` 能表达；已在 §1.4/§5.1 注明 |
| `--config` 与更高优先级冲突时的"文件非法值" | 若文件里某键非法、但该键又被更高优先级（cli/通用 env/旧别名）覆盖，`Load` 仍会因文件值非法而拒绝启动（校验在优先级决议之前）。这是"非法值拒绝启动"的有意取舍，未做"按最终来源再校验一次" |
| GC 的 HTTP 端点 | **未做**：只交付周期调度与 `--once`；没有 `POST /gc` 之类的端点 |
| PG 版 `ILeaseRepository` | **未交付**：单实例用内存租约；产品里当前**没有用例 `Acquire` 租约**，因此 `leases.ttl_seconds`/`renew_interval_seconds`/`time_source` 归入"已读但无效果"（§1.3.3） |
| `events.publisher=none` | **拒绝启动**（比 C10.11 列出的 `webhook` 更严）：组合根固定装配 `LogEventPublisher`，`none` 无法真正关闭事件 → 宁可拒绝 |
| `expiry.default > expiry.max` | 组合根**拒绝启动**（可读原因）。这是配置一致性问题（不是请求级语义）：请求级仍是"超限静默夹紧"，与本仓库既有 `ExpiryPolicy` 语义一致 |
| `config/fss.example.json` 不能"零环境变量"直接启动 | 示例里 7 个密文键是 `${ENV:VAR}`；变量未注入时加载器把"引用未解析"计为问题 → **exit 78**（fail-closed）。这是加载器的既有语义，已在 `operations.md` §0 显式提示；启动示例配置前必须先注入这些 Secret |


---

## 5. 切片 2（C10.9~C10.12）证据

### 5.1 判据逐条

| 判据 | 状态 | 证据 |
| --- | --- | --- |
| **C10.9** GC 真正跑起来 | ✅ | `test_config_wiring` 的 C10.9 用例：`gc.enabled=true` + `interval_seconds=1` → 造一个 3 天前的 `residue.bin.tmp.local.7.1` 与一个 5 秒前的 `inflight.bin.tmp.local.7.2`；**轮询**（100 ms × 200）直到 `/metrics` 出现 `fss_gc_runs_total{mode="real",...}` 且 `fss_gc_tmp_removed_total 1`、旧文件消失；断言**在途文件仍在**（TTL 保护）。正例对照：`gc.enabled=false` → 横幅 `未启动（gc.enabled=false...）` 且 `/metrics` **不含** `fss_gc_runs_total`。`--once` 用例：exit 0 + `gc once : partition=opendes dry_run=false ... tmp_removed=1 errors=0`。脚本 4+3 条 |
| **C10.10** 样例配置真的能起来 | ✅ | `--config config/fss.example.json` + 注入 6 个 `${ENV:...}` + 覆盖路径/端口/`FSS_GRPC_PORT=0` → `readiness_check` 200 且 body `File service is ready`；反面：把样例的 `"port": 8080` 改成 `-1` → exit 78 且输出含 `server.http.port`、不含 `已启动` |
| **C10.11** 不许"读了但静默无效" | ✅ | 16 个守卫键：非默认值 → exit 78 + `拒绝启动` + 原因指向该键（用例里逐条 `REQUIRE`）；**正例对照（R16）**：同一批键全部取默认/合法值 → 真实进程 readiness 200。`operations.md` 逐键三态：新增 `test_operations_doc.cpp` 的 C10.11 用例**机械**提取 §1.2 的 156 行状态标记并断言 **生效 72 / 拒绝启动 16 / 已读但无效果 68（= 156）**，同时断言正文声明的数字一致 |
| **C10.12** expiry 接通 | ✅ | `expiry.default=5M` + `expiry.max=10M`：无 `expiryTime` → 自签 URL `exp=now+300`；`?expiryTime=10M`（边界）→ 200 且 `exp=now+600`；`?expiryTime=60M`（超限）→ **静默夹紧**到 `now+600`（不是拒绝）；`?expiryTime=5X` → 400 + 固定消息。横幅打印 `expiry : default=5M（300s）max=10M（600s）` |

### 5.2 命令与关键输出

```console
$ cmake -S . -B build && cmake --build build -j"$(nproc)"         # 全绿
$ ctest --test-dir build -j4                                       # 100% tests passed, 0 failed out of 76
$ ./build/bin/test_config_wiring                                   # All tests passed (288 assertions in 16 test cases)
$ ./build/bin/test_operations_doc                                  # All tests passed (24 assertions in 2 test cases)
$ ./scripts/verify_config_wiring.sh                                # 配置面接线：全部通过（54 条断言）
$ ctest --test-dir build -L phase10                                # 1/1 Test #.. test_config_wiring ... Passed
```

### 5.3 自证（R1）：把 GC 调度的 interval 强行设为 0 → C10.9 必须失败

* **注入**：`src/main/server_main.cpp` 第 1405 行
  `const bool gc_schedule = gc_enabled && gc_interval_seconds > 0;`
  → `const bool gc_schedule = false;`（等价"interval 强行设为 0"：**不启动调度**）。
* **重建后** `./build/bin/test_config_wiring "★ C10.9：GC 调度真的跑起来*"`：
  ```
  test cases:  1 |  0 passed | 1 failed
  assertions: 14 | 13 passed | 1 failed
  residue_gone := false
  metric_visible := false
  ```
  （`/metrics` 里始终没有 `fss_gc_runs_total`/`fss_gc_tmp_removed_total`，`.tmp.*` 也一直在。）
* **脚本** `./scripts/verify_config_wiring.sh`（注入下）→ 退出码 **1**：
  ```
  ✗ 周期调度清理了够旧的 .tmp.*，且 /metrics 的 fss_gc_tmp_removed_total 动过：期望 '1'，实际 '0'
  ✗ --once 真的清掉了旧的 .tmp.*：输出里找不到 'tmp_removed=1'
  配置面接线：2 条断言失败（通过 52 条）
  ```
  第二条是**连带**：第 ⑦ 节没清掉 residue，`--once` 一次性清掉了 2 个 tmp（`tmp_removed=2`），
  因此"=1"的断言也失败 —— 根因仍是"调度没跑"。
* **还原**：`cp build/server_main.cpp.bak src/main/server_main.cpp` → 重建 →
  `./build/bin/test_config_wiring "★ C10.9：GC 调度真的跑起来*"` → **All tests passed
  (16 assertions in 1 test case)**；`git diff -- src/main/server_main.cpp | grep -c "R1 自证注入"` = **0**
  （无注入残留，`run_all_gates.sh` 的前置检查通过）。

### 5.5 `scripts/bench_baseline.sh` 的 A/B 实测（**环境漂移，非切片 2 回归**）

本机当前状态下 `scripts/bench_baseline.sh` 的 `--check` 判据**失败**（吞吐退化 >20%），
但这是**环境/会话漂移**，不是切片 2 引入的回归 —— 用"同一台机器、同一会话、同一负载生成器"
对 **HEAD（切片 1，未含本切片改动）** 与**工作树（切片 2）**各跑了一遍：

| 点位 | 基线（BASELINE.tsv） | HEAD（切片 1） | 工作树（切片 2） |
| --- | --- | --- | --- |
| `control_read_c4/rps` | 13849 | 10598（**-23.5%**） | 10494（**-24.2%**） |
| `control_read_c16/rps` | 12636 | 通过（未进入失败列表） | 10082（**-20.2%**） |
| `data_small_get_c16/rps` | 16678 | 11810（**-29.2%**） | 12478（**-25.2%**） |
| `large_stream_c4/rps` | 62 | 41（**-34.9%**） | 47（**-24.8%**） |

* 结论：**HEAD 与切片 2 都以几乎相同的幅度失败**（甚至切片 2 在 `large_stream_c4` 上更好），
  因此失败**不可归因于本切片的代码**。`operations.md` §4.2 早已登记该判据的适用范围：
  "跨会话实测漂移可达 ≈40%；该判据只在**同一会话、独占硬件**上有判定力"（R4）。
* 未采取的动作：**没有**用 `--save` 覆写 `BASELINE.tsv`（那会把环境漂移洗成"新基线"，
  等于把判据变成恒真）。基线文件保持原样（`git status` 无改动）。
* 与切片 2 无关的旁证：`data_put_c1`（磁盘类，脚本只告警）在切片 2 上 **+0.4%**；
  `sqlite_write_t1` **+4.0%**；只有 CPU/网络密集的读点位整体下移 —— 典型的整机状态差异。

### 5.4 组合根生命周期（AGENTS §4.3 的"joinable thread"陷阱）

* HTTP 从 `Bind()`+阻塞 `Listen()` 改为 `Start()`（后台 runner，`Server::Stop()` 会 join），
  主线程**轮询** `volatile std::sig_atomic_t g_stop_requested`（`std::signal` 的处理器只置位）；
  退出路径统一为：`gc_scheduler->Stop()`（signal + join）→ `server.Stop()` → `grpc_server->Shutdown()`。
* `GcScheduler` 的析构函数也调用 `Stop()`，因此**任何提前 return 都不会留下 joinable 线程**。
* SIGTERM 实测：`kill -TERM` 后进程退出码 0（见 §5.2 的手工验证与脚本的 `stop_server`）。

---

## 6. 切片 3（C10.13~C10.16）

### 6.1 判据逐条

| 判据 | 状态 | 证据 |
| --- | --- | --- |
| **C10.13** 审计 fail-closed | ✅ | `src/app/usecases/usecases.cpp` 的 `RecordAudit()` 现在返回 `Result<void>` 并按 `UseCasePorts::audit_fail_closed` 判定；`AuditGuard::Success()` 在**返回前**记录成功审计并返回 `Result`（调用点 14 处改成 `FSS_TRY(audit.Success())`）—— 析构阶段改不了状态码，那正是"审计失败却报 200"的静默缺陷。可驱动接缝 `FSS_AUDIT_FAULT_INJECT=1`（组合根装配 `FailingAuditLogger`；**不是**配置键）。`test_config_wiring` 三条：`fail_closed=true` + 注入 → `uploadURL` **500** 且响应无成功载荷；**正例对照** `fail_closed=false` + 同一坏后端 → **200**；无注入 → 200 |
| **C10.14** SQLite 调优键 | ⚠️ **部分满足** | 只接**真实存在**的 Options 字段：`metadata.sqlite.busy_timeout_ms`、`location.sqlite.busy_timeout_ms`、`location.sqlite.journal_mode`（→ `SqliteLocationRepositoryOptions.wal`；`WAL|DELETE`，`TRUNCATE` → exit 78）。`test_config_wiring` 用 `python3 -c "import sqlite3…PRAGMA journal_mode"` 从库文件读回 `wal` / `delete`（配置真的改变了文件头，不是恒为 WAL）；`busy_timeout` 只作用于连接、**无法从文件读回** → 由启动横幅（`sqlite tuning : location busy_timeout=… journal_mode=…`）确认。`synchronous`/`group_commit*` 在两个 Options 结构体里**没有**字段 → 按"不发明字段"留在"已读但无效果"（§1.3.3） |
| **C10.15** 鉴权与 gRPC 面 | ✅ | `auth.jwt.roles_claim` + `auth.local_roles.*` → `LocalJwtOptions`；`test_config_wiring` 用真实 HS256 token 断言：claim 名 `myroles` 携带 editors → **200**；同一角色放在默认 `roles` 里 → **403**；无 claim 但 email 命中 `local_roles` → **200**；**正例对照**：没有 `local_roles` 的实例上同一 token → **403**。`server.grpc.enabled=false`（端口非 0）→ 横幅 `server.grpc.enabled=false` 且该端口**不监听**（TCP connect 失败）；`=true` → 端口在监听且 **`GetInfo` 返回 OK / version=v2**（真实 gRPC channel） |
| **C10.16** 分区与存储细节 | ✅ **已完成（1 键接通 + 2 键拒绝启动；其余无字段可接）** | 只接**真实存在**的字段：`partition.file.opendes.max_file_bytes` → `PartitionConfig.max_object_bytes`（0 → -1 = 不限），并落到数据面 PUT 的 `RouteOptions::max_body_bytes`（新增 `RouterOptions::transfer_put_max_body_bytes`，**默认 0 = 与接线前逐字一致**）→ 带 `Content-Length` 超限 **413**、恰好等于上限 **200**（R16 正例）；`allowed_checksum_algorithms` / `default_checksum_algorithm` → **启动期校验**（未知算法名 / 默认不在集合内 → **exit 78**；正例：合法集合 + 默认 `MD5` 真的 readiness 200）。证据：`tests/integration/test_config_wiring.cpp` 的 `[c10.16]`（**2 用例 / 39 断言**）、全量 `ctest` **76/76**。`partition.file.{staging_container,persistent_container,storage_driver}` 与 7 个 `storage.posix.*` 细节键在 `PartitionConfig`/`PosixBlobStoreOptions` 里**没有字段** → 按「不发明字段」留「已读但无效果」（§1.3.3，附下一步） |

### 6.2 命令与关键输出

```console
$ cmake --build build -j"$(nproc)"                      # 全绿
$ ctest --test-dir build -L phase10                     # 见最终回复
$ ./scripts/verify_config_wiring.sh                     # 54 条（切片 3 未改脚本）
```

### 6.3 自证（R1）—— 见最终回复的实测输出

注入方式：把 `src/main/server_main.cpp` 里 `location_sqlite_journal_mode != "DELETE"` 的
`journal_mode` 读出处强制改回常量 `"WAL"`（等价"配置读了但没接上"），重建后
`test_config_wiring "★ C10.14*"` 的 DELETE 分支必须失败（python3 读回 `wal` 而不是 `delete`）。

**C10.16 的自证（本轮实测）**：把组合根里
`router_options.transfer_put_max_body_bytes = partition_file.max_file_bytes;`
短路成 `= 0;  // R1 自证注入`（等价"配置读了但没接上"）→ 重建后
`./build/bin/test_config_wiring "[c10.16]"` 必须失败（17 字节 PUT 得到 **200** 而不是 413）：

```console
test cases:  2 |  1 passed | 1 failed
assertions: 31 | 30 passed | 1 failed
```

还原后：`All tests passed (39 assertions in 2 test cases)`；全量 `ctest` **76/76**；
`git diff -- src | grep -c '自证注入\|selftest\|injected'` = **0**。

### 6.4 未做 / 降级（切片 3）

| 项 | 状态 |
| --- | --- |
| C10.16（`partition.file.*` / `storage.posix.*` 7 键） | **部分完成**：`max_file_bytes` 已接通（413 + 200 正例）、`{allowed,default}_checksum_algorithm` 已改为拒绝启动；容器名（3 键）与 `storage.posix.*`（7 键）在结构体里**没有对应字段** → 仍为"已读但无效果"，下一步 = 先给 `PosixBlobStoreOptions`/`PartitionConfig` 加字段 |
| `metadata.sqlite.journal_mode`/`synchronous`/`max_write_concurrency`/`group_commit*`、`location.sqlite.synchronous`/`group_commit*` | **未接通**（Options 里无字段；不发明字段） |
| `location.sqlite.max_write_concurrency` | 值已传进 Options 并参与 `>0` 校验，但实现是"单连接 + 互斥"（实际并发 1）→ 仍为"已读但无效果" |
| `FSS_AUDIT_FAULT_INJECT` | **故障注入开关，不是配置键**（不进 156 键清单）；生产**不要**设置 |
| `busy_timeout` 的锁竞争 A/B 实测 | **未做**（只做了横幅 + PRAGMA 应用证据）——如实标注 |

### 6.5 C10.16 的接线范围，以及「3 个既有测试失败」的复核

**接线范围（哪条键 → 哪条路径 → 哪条契约）**
* `partition.file.opendes.max_file_bytes` → 组合根新增的 `RouterOptions::transfer_put_max_body_bytes`
  （默认 `0`）→ **只**作用在数据面 `PUT /v1/transfer/:token`（`MakeRoute("transfer.put", …)`）的
  `RouteOptions::max_body_bytes` → 契约 §1.7 的"超限拒绝"：带 `Content-Length` → **413**（读体前前置拒绝）。
* 默认值 `0` 与接线前 HEAD 的字面量 `MakeRoute("transfer.put", 0)` **逐字一致** → 未设置该键的进程
  （含所有测试夹具）行为不变。组合根另把该值填给 `domain::PartitionConfig::max_object_bytes`
  （端口语义 `-1` = 不限）。
* **JSON 路由不受影响**：`server.http.max_header_bytes` / `max_uri_bytes` / `max_body_bytes` 与
  `json_body_limit_bytes` / `small_body_limit_bytes` 全部未改；`src/common/http/*`（phase1 的契约所在层）
  **零改动**（`git diff -- src/common/http` 为空）。

**「3 个测试失败」的复核结论：本轮未能复现，判定不是该改动引入的回归**
* `test_config_wiring.cpp` 的 C10.16 新用例（曾报"期望 413、实际 400"）：干净重建后**通过**
  （`HttpDo` 对 PUT 显式补 `Content-Length` → 命中 HTTP 层读体前前置拒绝 → 413）。
* `tests/hardening/test_resource_limits.cpp`（phase9）与 `tests/integration/test_httplib_hardening.cpp`
  （phase1）都是**夹具级**测试：直接构造 `Router` / `fss::http::Server`，**从不启动**
  `src/main/server_main.cpp` → 组合根的 C10.16 改动**结构上不可达**。实测：
  `All tests passed (44 assertions in 5 test cases)` 与
  `All tests passed (270 assertions in 13 test cases)`。
* 全量 `ctest --test-dir build -j4` → **100% tests passed, 0 tests failed out of 76**。
* 复核了 stash 的第三个父提交：`git cat-file -p 4e3f701b` 有 `^3`，而
  `git ls-tree -r 4e3f701b^3` **为空** → 该 stash **不含未跟踪文件**，
  不存在"未跟踪改动丢失导致无法复现"的情形。
* 既有断言**一条未放宽**（phase1/phase9 用例的期望值原样保留；C10.16 正例 200 与反例 413 并存，R16）。

---

## 7. C10.16 续（本轮收尾）：`storage.posix.*` 细节键与 `partition.file.*` 容器名

**背景**：切片 1/2/3 与 C10.16 之后，`operations.md` §1.3.3 里还剩 10 个被标注为
「无对应结构体字段」的键。本轮把「缺的是**字段**而不是行为」这件事如实推翻并接通：
先读代码（`PosixBlobStore` 的真实读写路径、`ObjectKeyPolicy::ContainerFor` 的全部调用点、
ADR-008 的 P4 是否落地），再决定**生效**或**拒绝启动**。

### 7.1 逐键结论（10 个键）

| 键 | 最终状态 | 依据 / 证据 |
| --- | --- | --- |
| `storage.posix.dir_mode` | **生效** | → `PosixBlobStoreOptions::dir_mode`；`ensure_container` 与对象父目录都走 `fs::EnsureDir(dir, mode)`。真实进程：配置 `"0700"` → `stat` `opendes-staging` / `opendes-persistent` 均为 `0700`。非法八进制（`"8x"`）→ exit 78 |
| `storage.posix.file_mode` | **生效** | → `PosixBlobStoreOptions::file_mode`（`open(2)` 的 mode）。真实进程：配置 `"0600"` → `stat` staging 对象 = `0600`。⚠️ 接线前实现固定 `0644`；`PosixBlobStoreOptions` 的默认仍是 `0644`（既有驱动/契约测试不变），schema 默认 `0640` 由组合根传入 |
| `storage.posix.atomic_write` | **生效** | 默认 `true`（tmp + rename，与接线前一致）；`false` → 直接写目标文件。驱动层故障注入（读到一半返回错误）：**两种模式**下目标与 `.tmp.*` 都不残留；正例对照 = 同一段代码写成功时对象存在 |
| `storage.posix.fadvise_random` | **生效** | 写路径（`put`）/读路径（`get`）各下发一次 `POSIX_FADV_RANDOM`；`IFadviseSink` 计数接缝断言（默认关闭 → 0 次；打开 → 1/2 次） |
| `storage.posix.fadvise_dontneed_after_large_read` | **生效** | 读出 > 1 MiB 才下发 `POSIX_FADV_DONTNEED`；1 KiB 读 0 次、2 MiB 读 1 次、关掉开关后同一段大读 0 次（R1 对照） |
| `partition.file.opendes.staging_container` | **生效** | → `PartitionConfig::staging_container`；`ObjectKeyPolicy::ContainerFor(IPartitionRegistry&, …)` 统一解析（uploadURL 签发 / 用例 / GC / 启动建目录）。真实进程：`custom-stage-1` 目录出现且 uploadURL 后其下有对象，`opendes-staging` **不出现** |
| `partition.file.opendes.persistent_container` | **生效** | 同上；真实进程：`custom-persist-1` 目录出现 |
| `storage.posix.group_commit_max_batch` | **拒绝启动** | ADR-008 的 **P4（写整批 `.tmp` → `syncfs` → 统一 rename → `fsync(dir)`）实现里不存在**：只有 `FsyncPolicy::kBySize` 的"按大小决定是否 fdatasync + 改名后 fsync 目录"。非默认（`500`）→ exit 78 + 「批提交协议未实现（ADR-008 的 P4 待做）」 |
| `storage.posix.sync_dir_after_batch` | **拒绝启动** | 同上（没有"批末"这个时刻）。非默认（`false`）→ exit 78 |
| `partition.file.opendes.storage_driver` | **拒绝启动** | 与顶层 `storage.driver` 不一致 → exit 78 并说明（组合根只装配一个 `BlobStore`，`SingleStoreFactory` 的所有 partition/zone 共用）；**等于顶层值（或空）→ 正常启动**（R16 正例） |

### 7.2 实现点（可点击）

* `src/infra/blob/posix/posix_blob_store.h` / `.cpp`：`PosixBlobStoreOptions` 新增 5 字段 +
  `IFadviseSink` 可注入接缝；`put`/`copy` 支持非原子直写（失败删目标）、`get` 的 DONTNEED 阈值；
  `ensure_container` / `WritableObjectPath` 用 `dir_mode`。
* `src/domain/ports/ports.h`：`PartitionConfig` 新增 `staging_container` / `persistent_container` /
  `storage_driver`（空串 = 与接线前逐字一致的默认命名）。
* `src/app/services/object_key_policy.{h,cpp}`：新增
  `ContainerFor(const PartitionConfig&, zone)` 与 `ContainerFor(IPartitionRegistry&, partition, zone)`
  （注册表查不到 → 退回默认命名）；容器名白名单校验与 partition 名共用。
* `src/app/services/location_issuer.{h,cpp}`：可选的租户注册表（默认 `nullptr` → 既有调用点不变）。
* `src/app/usecases/usecases.cpp`（3 处）、`src/app/tasks/gc_task.cpp`（3 处）、
  `src/main/server_main.cpp`（启动建容器）：容器名统一走注册表。
* `src/main/server_main.cpp`：读取 7 个 `storage.posix.*` 键（八进制解析失败 → exit 78）、
  映射到 `PosixBlobStoreOptions`；批提交两键 → 拒绝启动；`partition.file.*` 的容器名/驱动
  启动期校验；`LocationIssuer` 传入 `&partitions`。
* `tests/integration/test_config_wiring.cpp`：新增 4 用例 / 75 断言（驱动层 + 真实进程两层；
  含关闭对照与失败注入的 R1 对照）；`tests/CMakeLists.txt` 给该测试补 `fss_blob_posix fss_io`。
* 文档：`docs/operations.md`（§1.2 逐键行、§1.3 三态表与三个清单、§1.3.3 理由表、§7 汇总行）、
  `docs/00-final-design.md` §5.w（推翻「无字段可接」）与 §6/§7、`docs/02-design.md` §16.1、
  `docs/04-implementation-plan.md`、`AGENTS.md`、`tests/unit/test_operations_doc.cpp`（期望值同步）。

### 7.3 实测命令与输出摘要

```
$ cmake -S . -B build && cmake --build build -j"$(nproc)"          # 构建通过（0 error）
$ ctest --test-dir build -j4
100% tests passed, 0 tests failed out of 76

$ ctest --test-dir build -L phase10
phase10 = 3.03 sec*proc (1 test)         # 1 个测试二进制 / 26 用例 / 499 断言

$ ./build/bin/test_config_wiring "★ C10.16 续*"
All tests passed (75 assertions in 4 test cases)

$ ./scripts/check_docs.sh
全部检查通过（D1~D5）                 # 53 链接 / 12 ADR / 阶段表一致 / 142 条门槛（11 个阶段）

$ ./scripts/verify_config_wiring.sh
配置面接线：全部通过（54 条断言）
```

### 7.4 R1 自证（注入 → 用例失败 → 还原 → 实测输出）

注入：把 `ObjectKeyPolicy::ContainerFor(const PartitionConfig&, …)` 的
`if (override_name.empty())` 改成 `if (true)`（等价于"容器名覆盖永远不生效"）：

```
$ ./build/bin/test_config_wiring "★ C10.16 续：partition.file*"
/home/ll/fssvrcpp/tests/integration/test_config_wiring.cpp:1545: FAILED:
  REQUIRE( std::filesystem::is_directory(custom_staging) )
with expansion:
  false
$ echo $?      # 1
```

还原（`git diff -- src` 无注入残留；`grep -rn R1-INJECT src/` → none）后：

```
$ ./build/bin/test_config_wiring "★ C10.16 续：partition.file*"
All tests passed (14 assertions in 1 test case)
```

即：该用例**真的**依赖配置值经过 `ContainerFor` 生效，而不是"恒真"。

### 7.5 未做 / 仍无效果（如实登记）

* `storage.posix.{shared_mount_required,one_filesystem_per_partition}` 仍「已读但无效果」
  （依赖 `deployment.mode=multi` 运行形态，而 multi 本身拒绝启动）。
* 其余 44 个「已读但无效果」键不变（逐键理由与下一步见 `operations.md` §1.3.3）。
* ADR-008 的 P4（两阶段批提交）**仍未实现** —— 本轮不假装接通，改为对
  `group_commit_max_batch` / `sync_dir_after_batch` 的非默认值**拒绝启动**。

### 7.6 三态计数（C10.16 续 收尾；**该数字已被 §9 的切片 4、§10 的切片 5 取代**）

`config/fss.example.json` 的 **156** 个叶子键：生效 **93** 个 / 拒绝启动（触发条件）**21** 个 /
已读但无效果 **42** 个（**93** + **21** + **42** = 156；由 `tests/unit/test_operations_doc.cpp` 的
C10.11 用例从 §1.2 的 156 行程序化提取并机械断言，正文声明的数字也一并断言）。
> 本节的数字是 C10.16 续 收尾时的**历史快照**，**已被 §9 的切片 4 与 §10 的切片 5 取代**（切片 4 把
> `server.http.transfer_max_body_bytes`、`metadata.sqlite.{journal_mode,synchronous}`、
> `location.sqlite.synchronous` 4 键从「已读但无效果」移入「生效」，见 §9；切片 5 把
> `self_signed` 3 键移入「生效」，见 §10）。

---

## 8. 门槛检查器自身的缺陷（本轮发现并修复）：D4/D5 只认一位数阶段号

> 这一条不是产品缺陷，而是**检查器缺陷**——它的"通过"曾经是空集合。
> 按 AGENTS.md 铁律 R1（关键断言必须配自证对照）与 P2-D07 的同类教训登记在此。

### 8.1 现象

`./scripts/check_docs.sh` 长期报"全部检查通过（D1~D5）"，但输出里：

```
  D4 已完成 ['0','1','2','3','4','5','6','7','8','9']，进行中 无，门槛启用 ['0',...,'9']
  D5 门槛编号检查：10 个阶段，共 126 条门槛
```

而 `scripts/run_all_gates.sh` 的 `IMPLEMENTED_PHASES` 里**明明有 10**，
`docs/04-implementation-plan.md` 里**明明有 P10 行与 C10.1~C10.16 共 16 条门槛**。

### 8.2 根因

```python
re.match(r'\|\s*\*\*P(\d)\*\*', line)          # ← 只匹配一位数
impl_set = set(re.findall(r'\d', ...))          # ← '10' 被拆成 '1' + '0'（已被 0/1 覆盖）
re.findall(r'^\|\s*\*{0,2}(C\d\.\d+[a-z]?)\*{0,2}\s*\|', ...)   # ← 同上
```

于是阶段 10 的**行**与 `C10.*` 的**全部 16 条门槛**对 D4/D5 **根本不存在**：
"D4 阶段表一致""D5 无断号"对阶段 10 恒为真。影响面是**两个方向**都失效：
① 计划里标 ✅ 却不加门槛 → 查不出；② 加了门槛却不写进计划 → 也查不出。

### 8.3 修复

`scripts/check_docs.sh`：三处正则改为 `\d+`（`P(\d+)` / `C(\d+)\.` / `findall(r'\d+')`），
并把 `sorted(...)` 改成 `key=int`（否则 `'10'` 会排在 `'2'` 前面）。
修复后同一份文档的输出：

```
  D4 已完成 ['0',...,'9']，进行中 ['10']，门槛启用 ['0',...,'10']
  D5 门槛编号检查：11 个阶段，共 142 条门槛
```

即：**C10.16 与 P10 从此真的在检查范围内**（126 → 142 条门槛，+16）。

### 8.4 R1 自证：`--selftest` 现在必须能检出 D4/D5 的注入

`--selftest` 原来只用临时 md 文件验证 D1/D2；本轮扩展为**同时注入 D4/D5 的错误**：

* D4：追加一行 `| **P99** | 自证注入阶段 | - | - | ✅ **已完成** |`（✅ 但未启用门槛；
  同时锁住"只认一位数"的回归——99 是两位数）；
* D5：把 `| **C10.16** |` 改成 `| **C10.17** |` → C10 出现断号。

注入后运行（计划文件用 `trap` 备份/恢复，检查完立即还原，不留残迹）：

```
$ ./scripts/check_docs.sh --selftest
  ✓ 自证：D1/D2/D4/D5 都能检出注入的错误（检查器有效）
  ...
  D4 已完成 ['0',...,'9']，进行中 ['10']，门槛启用 ['0',...,'10']
  D5 门槛编号检查：11 个阶段，共 142 条门槛
  全部检查通过（D1~D5）
$ git status --porcelain          # 只有 scripts/check_docs.sh 一个改动，计划文件已还原
 M scripts/check_docs.sh
```

自证脚本断言的是**注入版的四类报错同时出现**（`D1 失效链接` / `D2 已作废的值` /
`D4 标 ✅ 但未启用门槛的阶段：['99']` / `D5 C10 门槛编号断号…C10.16`）——少任一条即算自证失败。
`run_all_gates.sh` 第 98 行调用的是 `check_docs.sh --selftest`，所以这条自证**在门槛里**，不是手工程序。

### 8.5 修复后的全量门槛

```
$ ./scripts/run_all_gates.sh
  汇总
    失败: 无
  ⏱  总耗时: 4 分 12 秒（252 s，阶段数 11）
  ✅ 全部已启用阶段门槛通过。
$ ctest --test-dir build -j4
  100% tests passed, 0 tests failed out of 76
```

即：**P0~P10 全绿**（第一次修复后跑为 377 s / 11 阶段，第二次在最终提交树上为 252 s / 11 阶段，
两次都 `失败: 无`；D4/D5 的修复没有掩盖任何既有问题——修复前后"通过"的差异只体现在
**检查范围从 10 阶段/126 条门槛扩到 11 阶段/142 条门槛**）。

---

## 9. 切片 4（本轮）：数据面 PUT 上限 与 SQLite 的 journal_mode / synchronous

**背景**：`docs/operations.md` §1.3 的 156 键三态里还有 4 个键是「已读但无效果」：
`server.http.transfer_max_body_bytes`（schema 写成 `Int(0, 0)`，而 `FieldSpec` 在 `hi == lo`
时**只允许 0** → 这个键等于摆设）、`metadata.sqlite.journal_mode`（元数据仓储内硬编码 WAL）、
`metadata.sqlite.synchronous` / `location.sqlite.synchronous`（两个仓储都没有这个 PRAGMA）。
本轮先读代码确认"字段/通路是否真实存在"，再逐个接通。

### 9.1 逐键结论（4 个键）

| 键 | 最终状态 | 判据 / 证据 |
| --- | --- | --- |
| `server.http.transfer_max_body_bytes` | **生效** | schema 放宽为 `Int(0, 1L << 40)`（`0` = 不限）；落点 = 全局键与 `partition.file.<p>.max_file_bytes` 的**较小者**（都为 0 → 仍不限，逐字保持接线前行为）。真实进程：全局 `1048576` + 无 partition → 声明 2 MiB → **413**、恰好 1 MiB → **200**；未设该键（默认 0）→ 2 MiB → **200**（R16 对照）；全局 1 MiB + partition `4096` → 8192 → **413**、4096 → **200**（证明取较小者） |
| `metadata.sqlite.journal_mode` | **生效** | `SqliteMetadataRepositoryOptions.wal`（新增，默认 `true` = 与接线前逐字一致）；`WAL`→`PRAGMA journal_mode=WAL`；`DELETE`→不执行 PRAGMA（SQLite 默认 `delete`）。真实进程 `python3 sqlite3` 读回 metadata 库：WAL→`wal`、DELETE→`delete`；`TRUNCATE`→**exit 78** + 可读原因 |
| `metadata.sqlite.synchronous` | **生效** | `SqliteMetadataRepositoryOptions.synchronous_level`（新增）+ `PRAGMA synchronous=<n>`；同连接访问器 `AppliedPragma("synchronous")`：默认 `"1"`（NORMAL，R16 正例）、OFF `"0"`、FULL `"2"`；真实进程横幅打印 `synchronous=FULL/OFF/NORMAL` |
| `location.sqlite.synchronous` | **生效** | `SqliteLocationRepositoryOptions.synchronous_level`（新增，放在结构体**最后**，避免既有聚合初始化按位置静默错位）+ 同款 PRAGMA 与 `AppliedPragma` |

### 9.2 实现点（可点击）

* `src/common/config/core_schema.cpp`：`server.http.transfer_max_body_bytes` 由 `Int(0, 0)` 放宽为
  `Int(0, 1L << 40)`（上限与 `storage.posix.fsync_threshold_bytes` 同量级，description 写明语义）。
* `src/infra/metadata/sqlite/sqlite_metadata_repository.{h,cpp}`：Options 新增 `wal` /
  `synchronous_level`；`Open()` 按 `wal` 决定是否执行 WAL PRAGMA、按 `synchronous_level` 执行
  `PRAGMA synchronous`；新增 `AppliedPragma`（白名单只放行 `synchronous`，PRAGMA 名不能参数化）。
* `src/infra/location/sqlite/sqlite_location_repository.{h,cpp}`：同上（`synchronous_level` 加在最后）。
* `src/main/server_main.cpp`：读全局键 + 两个 `synchronous` 枚举（`ParseSynchronousLevel`，
  `NORMAL/FULL/OFF` → `1/2/0`）；metadata `journal_mode` 同样只接通 WAL|DELETE；两个仓储 Options
  改**具名赋值**；`router_options.transfer_put_max_body_bytes` = 全局与 partition 的较小者；
  横幅新增 `transfer limit` 行并把 `sqlite tuning` 扩展到 journal_mode/synchronous。
* `tests/integration/test_config_wiring.cpp`：+3 用例 / 108 断言（数据面 413/200/默认不限/较小者、
  metadata journal_mode python3 读回与 TRUNCATE→78、synchronous 横幅与非法值→78）。
* `tests/integration/test_sqlite_{location,metadata}_repository.cpp`：各 +1 用例 / 13 断言
  （`AppliedPragma` 的默认/OFF/FULL + 白名单护栏）。
* 文档：`docs/operations.md`（§1.2 逐键行、§1.3 三态与三个清单、§1.3.3 理由表、§8）、
  `docs/03-api-contract.md`（数据面默认上限行）、`docs/00-final-design.md` §P10、
  `docs/02-design.md` §16、`docs/04-implementation-plan.md` 末、`AGENTS.md`（§0 三态 + §4.3 新增
  synchronous 陷阱行）、`tests/unit/test_operations_doc.cpp`（期望值 **93**/**21**/**42**）。

### 9.3 实测命令与输出摘要

```
$ cmake --build build -j"$(nproc)"
[100%] Built target test_grpc_streaming                 # 0 error
#   ★ 说明：首次 -j$(nproc) 在 test_protocol_equivalence 上触发 AGENTS §4.3 的 OOM
#   （Killed signal terminated program cc1plus）；按该条用 -j4 续跑后全量构建通过。
$ ctest --test-dir build -j4
100% tests passed, 0 tests failed out of 76
$ ctest --test-dir build -L phase10 --output-on-failure
100% tests passed, 0 tests failed out of 1
$ ./build/bin/test_config_wiring
All tests passed (607 assertions in 29 test cases)
$ ./scripts/check_docs.sh --selftest
  ✓ 自证：D1/D2/D4/D5 都能检出注入的错误（检查器有效）
  D1 检查了 53 个本地链接 / D3 ADR 12 个 / D5 11 个阶段 142 条门槛
  全部检查通过（D1~D5）
$ ./scripts/verify_config_wiring.sh
配置面接线：全部通过（54 条断言）
$ ./scripts/run_all_gates.sh              # 父代理在最终工作树上重跑（含 check_docs --selftest）
  汇总
    失败: 无
  ⏱  总耗时: 5 分 27 秒（327 s，阶段数 11）
  ✅ 全部已启用阶段门槛通过。
```

**父代理独立复核**（不是转述切片实现者）：`cmake --build build -j4` 通过、`ctest` **76/76**、
`test_config_wiring` **607 断言 / 29 用例**、`test_operations_doc` **24 断言 / 2 用例**、
`check_docs.sh --selftest` 通过、`verify_config_wiring.sh` 54 条断言通过、
`grep -rn "R1-INJECT" src/ tests/` 无残留 —— 与 §9.3 的报告一致。

### 9.4 R1 自证（注入 → 用例失败 → 还原 → 实测输出）

4 个键**各**注入一次"错误实现"，都让对应用例失败；随后完整还原，
`grep -rn "R1-INJECT" src/` 无残留：

1. transfer 上限改成"直接取全局"（忽略 partition 的较小者）：
```
REQUIRE( server.DumpLog().find("max_body_bytes=4096") != std::string::npos )   FAILED
with expansion: transfer limit : ... max_body_bytes=1048576 ...
test cases: 1 | 0 passed | 1 failed      assertions: 46 | 45 passed | 1 failed
```
2. metadata `wal` 写死 `true`（DELETE 不生效）：
```
REQUIRE( mode == "delete" )   FAILED      with expansion: "wal" == "delete"
test cases: 1 | 0 passed | 1 failed      assertions: 22 | 21 passed | 1 failed
```
3. metadata 仓储注释掉 `PRAGMA synchronous`：
```
REQUIRE( applied.value() == "1" )   FAILED   with expansion: "2" == "1"
REQUIRE( applied.value() == "0" )   FAILED   with expansion: "2" == "0"
test cases: 1 | 0 passed | 1 failed      assertions: 13 | 11 passed | 2 failed
```
（`"2"` 正是"另开连接读回会拿到新连接默认值 FULL=2"的直接实证 —— 所以必须用同连接访问器）
4. location 仓储注释掉 `PRAGMA synchronous`：同上，`"2" == "1"` / `"2" == "0"`，2 条失败。

还原后的实测：
```
$ grep -rn "R1-INJECT" src/                              # 无输出
$ ./build/bin/test_config_wiring "★ 切片 4*"              → All tests passed (108 assertions in 3 test cases)
$ ./build/bin/test_sqlite_location_repository "★ 切片 4*"  → All tests passed (13 assertions in 1 test case)
$ ./build/bin/test_sqlite_metadata_repository "★ 切片 4*"  → All tests passed (13 assertions in 1 test case)
```

### 9.5 未做 / 降级（如实登记）

* `*.sqlite.{max_write_concurrency,group_commit*}` 仍「已读但无效果」：两个仓储都是
  "单连接 + 互斥"（`max_write_concurrency` 不改变行为），且实现里没有组提交。
* `metadata/location.sqlite.synchronous` 的**非枚举值**由 schema 的 `.Enum` 在 `config::Load`
  阶段就拒绝（exit 78 +「取值非法」），因此组合根里 `ParseSynchronousLevel() < 0` 那条分支在
  当前 schema 下不可达，保留为纵深防御；`journal_mode=TRUNCATE` 能到达组合根，因为 schema 的
  枚举**刻意**包含 TRUNCATE（组合根只接通 WAL|DELETE）。
* 数据面 PUT 超限的 413 用例**只发请求头**（用 `Content-Length` 声明 2 MiB），不真的灌 2 MiB：
  包装层按 Content-Length **读体前**拒绝（H-2①），真灌字节会因未读残余触发 RST、丢掉已收到的
  413（用例注释里写明了这条）。
* 未跑 `run_all_gates.sh` 全量（由父代理收尾跑）；上面 6 项与本切片相关的命令全绿。

### 9.6 三态计数（切片 4 收尾；**该数字已被 §10 的切片 5 取代**）

`config/fss.example.json` 的 **156** 个叶子键：生效 **93** 个 / 拒绝启动（触发条件）**21** 个 /
已读但无效果 **42** 个（**93** + **21** + **42** = 156），由 `tests/unit/test_operations_doc.cpp` 的 C10.11
用例从 §1.2 的 156 行程序化提取并机械断言（表格计数 + 正文两处字符串同时断言）。
> 本节的数字是切片 4 收尾时的**历史快照**，**已被 §10 的切片 5 取代**（切片 5 把
> `self_signed.{key_id,default_ttl_seconds,max_ttl_seconds}` 3 键移入「生效」，见 §10）。

---

## 10. 切片 5（本轮）：接通 `self_signed` 的 3 个键（C10.17）

**背景**：`docs/operations.md` §1.3.3 的 156 键三态里，`self_signed` 段仍有 3 个键是「已读但无效果」：
`key_id`（密钥标识）、`default_ttl_seconds`、`max_ttl_seconds`。本轮把这 3 个键接通成**生效**，
最终三态为 **生效 96 / 拒绝启动 21 / 已读但无效果 39 = 156**。

### 10.1 逐键结论（3 个键）

| 键 | 最终状态 | 判据 / 证据 |
| --- | --- | --- |
| `self_signed.key_id` | **生效** | `HmacTransferTokenCodec` 构造函数新增**带默认值**的第 3 个参数 `std::string key_id = {}`；非空时 `Encode` 把 `key_id` 写进**被签名的载荷**（`payload["key_id"]`），`Decode` 在验签通过后要求载荷里的值与配置**完全相等**（**缺字段也拒绝**）→ `kUnauthenticated` → HTTP **401**。配置为空（既有测试夹具）→ 不写也不校验（C7.4/C3.7 逐字不变）。真实进程证据：`key_id=k1` 签发的 URL PUT/GET → **200**（R16 正例）；以 `key_id=k2` 重启（同签名密钥）后重放同一 PUT/GET URL → **401**（实测状态码，不是猜的）。启动横幅打印 `key_id`（**不打印密钥**） |
| `self_signed.default_ttl_seconds` | **生效** | → `app::LocationIssuer` 的 `SelfSignedTtlOptions.default_seconds`（构造函数**末尾**可选参数，默认值 = 接线前行为）。**仅 `!native_presign`（自签）分支**且请求**未提供** `expiryTime` 时：`ttl = min(ttl, 值)`。真实进程：`default_ttl_seconds=60` 且不带 `expiryTime` → `exp=now+60`；给了 `expiryTime=1M`（60s）→ 仍 60s（证明缺省上界**只在未给 `expiryTime` 时叠加**） |
| `self_signed.max_ttl_seconds` | **生效** | → `SelfSignedTtlOptions.max_seconds`（**仅自签分支**）：`ttl = min(ttl, 值)`。真实进程：`max_ttl_seconds=120` + `expiryTime=9H`（32400s，远小于 `expiry.max=7D`）→ `exp=now+120`；`expiryTime=1M`（60s）→ 仍 60s（R16：上界不是"常量改写"） |

**语义与 `expiry.*` 的关系（必须一起读）**：`expiry.default`/`expiry.max` 仍是 **`expiryTime` 参数的
解析规则与缺省**（C10.12 定稿，两条分支共用）；本切片**不**改它，而是对**自签分支**再夹一次上界
（`native_presign` 分支**完全不受**这两个键影响）。**理由**：C10.12 的语义有测试锁定
（`expiry.default=5M` + 不带 `expiryTime` → `exp=now+300`），若让 `self_signed.*` 覆盖
`expiry.default`，就必须先推翻那条既定语义并同步契约与测试 —— 本切片刻意不这么做。
**若要改成"覆盖作缺省"，必须先推翻 C10.12 并同步契约与测试**（登记在 `docs/operations.md` §1.3.3）。

### 10.2 实现点（可点击）

* `src/domain/ports/ports.h`：`TransferToken` **末尾**新增 `std::string key_id;`（放最后，避免破坏
  既有聚合初始化）；注释里的 payload 字段列表补上 `key_id`。
* `src/infra/transfer/transfer_token.h` / `.cpp`：构造函数第 3 参数（带默认值）+ `key_id_` 成员；
  `Encode` 非空时写入被签名载荷；`Decode` 的 fail-closed 校验（不匹配 / 缺失 → `kUnauthenticated`）。
* `src/app/services/location_issuer.h` / `.cpp`：新增 `SelfSignedTtlOptions{default_seconds=3600,
  max_seconds=604800}` 与构造函数末尾可选参数；`ApplySelfSignedTtl()` 只在 `!native_presign` 时夹紧
  （请求未给 `expiryTime` 才叠加缺省上界）。
* `src/main/server_main.cpp`：读 `self_signed.{key_id,default_ttl_seconds,max_ttl_seconds}`
  （默认 `k1` / `3600` / `604800`）→ `HmacTransferTokenCodec(secret, clock, key_id)` +
  `LocationIssuer(..., self_signed_ttl_options)`；横幅新增 `self signed : key_id=... TTL 自签上界 ...`。
* 文档：`docs/operations.md`（§1.2.5 三行、§1.3 三态与三个清单、§1.3.3 的 `self_signed` 行与"下一步"）、
  `docs/02-design.md`（T3/T4 载荷字段 + 计数）、`docs/adr/ADR-003-storage-abstraction.md`（token 载荷字段）、
  `docs/00-final-design.md`、`docs/04-implementation-plan.md`（C10.17 门槛 + 三态计数）、`AGENTS.md`、
  `tests/unit/test_operations_doc.cpp`（期望值 96/21/39）。

### 10.3 实测命令与输出摘要

```
$ cmake --build build -j4
[100%] Built target test_dual_protocol_concurrency        # 0 error（★ 用 -j4，AGENTS §4.3 的 OOM 陷阱）
$ ctest --test-dir build -j4
100% tests passed, 0 tests failed out of 77                # 新增 test_transfer_token_key_id → 76 → 77
$ ctest --test-dir build -L phase10 --output-on-failure
100% tests passed, 0 tests failed out of 2
$ ./build/bin/test_config_wiring
All tests passed (689 assertions in 31 test cases)
$ ./build/bin/test_transfer_token_key_id
All tests passed (29 assertions in 4 test cases)
$ ./scripts/check_docs.sh --selftest
  ✓ 自证：D1/D2/D4/D5 都能检出注入的错误（检查器有效）
  全部检查通过（D1~D5）                                   # 含 C10.17（D5 连续无断号）
$ ./scripts/verify_config_wiring.sh
配置面接线：全部通过（54 条断言）
```

**父代理独立复核 + 两处加固**（在切片实现之上，见 §10.6）：最终
`test_config_wiring` = **692 断言 / 31 用例**（+3 断言）、`test_transfer_token_key_id` = 29/4、
`test_operations_doc` = 24/2、`ctest` **77/77**、`check_docs.sh --selftest` 通过、
`R1-INJECT` 无残留。

### 10.4 R1 自证（注入 → 用例失败 → 还原 → 实测输出）

3 个键**各**注入一次"错误实现"，都让对应用例失败；随后**完整还原**，
`grep -rn "R1-INJECT" src/` 无输出：

1. **把 `Decode` 的 `key_id` 校验去掉**（`if (!key_id_.empty())` → `if (false)`）→ 负例必须失败：
```
$ ./build/bin/test_config_wiring "★ C10.17*"
tests/integration/test_config_wiring.cpp:1989: FAILED:
  REQUIRE( replay_put.status == 401 )
with expansion:
  200 == 401 (0x191)
test cases:  1 |  0 passed | 1 failed      # 仅 .key_id 用例；另 1 个用例通过
assertions: 48 | 47 passed | 1 failed
$ ./build/bin/test_transfer_token_key_id
tests/unit/test_transfer_token_key_id.cpp:130: FAILED:  REQUIRE_FALSE( decoded.ok() )
tests/unit/test_transfer_token_key_id.cpp:147: FAILED:  REQUIRE_FALSE( decoded.ok() )
test cases:  4 |  2 passed | 2 failed
assertions: 25 | 23 passed | 2 failed
```
2. **把自签 TTL 上界整段变成"不生效"**（`ApplySelfSignedTtl` 里改成无条件 `return ttl_seconds;`）
   → 真实进程上界用例与进程内自签上界用例都必须失败：
```
$ ./build/bin/test_config_wiring "★ C10.17*"
tests/integration/test_config_wiring.cpp:2034: FAILED:  REQUIRE( exp <= now + 125 )
with expansion:
  1789773976 <= 1789741701            # = ExpiryPolicy 的 9H=32400s，未被 120s 夹紧
tests/integration/test_config_wiring.cpp:2054: FAILED:  REQUIRE( exp <= now + 65 )
with expansion:
  1789745176 <= 1789741641            # = expiry.default 1H=3600s，未被 60s 夹紧
test cases:  2 |  1 passed | 1 failed
assertions: 76 | 74 passed | 2 failed
$ ./build/bin/test_location_issuer "★ C10.17*"
tests/unit/test_location_issuer.cpp:297: FAILED:
  REQUIRE( defaulted.value().expires_at_epoch_seconds == now + 60 )
with expansion:
  1700003600 == 1700000060            # 3600（expiry.default）而不是 60（自签缺省上界）
test cases:  1 |  0 passed | 1 failed
assertions: 11 | 10 passed | 1 failed
```
3. **把 `default` 上界那一步删掉**（保留 `max` 的 `min`，去掉 `if (!expiry_time_provided) …`）→
   "请求不带 `expiryTime` → now+60"必须失败：
```
$ ./build/bin/test_config_wiring "★ C10.17*"
tests/integration/test_config_wiring.cpp:2054: FAILED:  REQUIRE( exp <= now + 65 )
with expansion:
  1789745191 <= 1789741656            # 缺省上界没叠加 → 仍是 expiry.default 1H
test cases:  2 |  1 passed | 1 failed
assertions: 79 | 78 passed | 1 failed
$ ./build/bin/test_location_issuer "★ C10.17*"
tests/unit/test_location_issuer.cpp:297: FAILED:
  REQUIRE( defaulted.value().expires_at_epoch_seconds == now + 60 )
with expansion:
  1700000120 == 1700000060            # 120（max 上界）而不是 60（default 上界）
test cases:  1 |  0 passed | 1 failed
assertions: 11 | 10 passed | 1 failed
```
还原后的实测：
```
$ grep -rn "R1-INJECT" src/                              # 无输出（rc=1）
$ ./build/bin/test_config_wiring "★ C10.17*"             → All tests passed (82 assertions in 2 test cases)
$ ./build/bin/test_transfer_token_key_id                 → All tests passed (29 assertions in 4 test cases)
$ ./build/bin/test_location_issuer "★ C10.17*"           → All tests passed (17 assertions in 1 test case)
```
**另记一处测试自身的缺陷（本轮抓到并修掉）**：`key_id` 真实进程用例最初让 k1/k2 两个顺序进程
**共用同一对 SQLite 库文件**，第二个进程偶发在启动期报 `PRAGMA synchronous 失败：database is locked`
→ 用例变成 flaky（实测 12 次里失败 1 次）。修法：两次启动用**各自独立**的库路径（负例判据在
`Decode` 阶段，不需要读位置记录），并**轮询**第一个进程真正退出（`kill -0` 失败）后才启动第二个 ——
"轮询实际条件"而非固定 sleep（AGENTS §4.3）。修后 15 次连跑全绿。

### 10.5 未做 / 降级（如实登记）

* **多密钥轮换未交付**（ADR-009:227 的"多 key 并存"）：`key_id` 只是"绑定进签名载荷 + 解码侧拒绝
  不匹配"，**没有**"按 id 选密钥"；换 `key_id` = 旧 URL 全量作废。要做轮换必须把 `key_id → 密钥`
  表引入 codec 并同步契约/文档/测试。
* `self_signed.*` **不覆盖** `expiry.default`（见 §10.1 的理由）；若要改，先推翻 C10.12。
* 组合根**未**加 `default_ttl_seconds > max_ttl_seconds` 的跨字段拒绝（父代理定的语义是纯 `min`
  合成，绝对上界胜出；加了会与 `max_ttl_seconds=120`（default 仍为 3600）的必测场景冲突）。
* 未跑 `run_all_gates.sh` 全量（由父代理收尾跑）；上面与本切片相关的命令全绿。

### 10.6 父代理复核时发现并自己修掉的两个问题

复核 diff 时发现两处"判据不够严 / 语义不一致"，在提交前修掉（都属于**收紧**，没有放宽任何检查）：

**① `?expiryTime=`（空串）能绕过 `self_signed.default_ttl_seconds`** —— 实现用
`expiry_time.has_value()` 判断"客户端给了参数"，但契约 §1.4 与 `ExpiryPolicy` 的既定语义是
**空串按"未提供"处理**。于是一个空参数就能跳过缺省上界：

```
修前：ttl_seconds = ApplySelfSignedTtl(ttl_seconds, caps, expiry_time.has_value());
修后：ttl_seconds = ApplySelfSignedTtl(ttl_seconds, caps,
                                      expiry_time.has_value() && !expiry_time->empty());
（两处调用点同改：IssueUploadLocation / IssueDownloadLocation；注释写明理由）
```
并补一条真实进程断言（`C10.17` 的 TTL 用例）：`self_signed.default_ttl_seconds=60` +
`?expiryTime=`（空）→ `exp=now+60`（**仍受缺省上界夹紧**）。同一路径上"空串"只允许一种含义。

**② "不叠加缺省上界"那条断言原本恒真** —— 原断言用 `?expiryTime=1M`（60s）而缺省上界也是
60s，"叠加"与"不叠加"都能过（R16 的反面：判据无法区分两种实现）。改成 `?expiryTime=2M`
（120s > 缺省上界 60s，且 < 绝对上界 604800s）→ 必须原样保留 120s，才真正证明"缺省上界只在
未给 `expiryTime` 时生效"。

修后实测：`./build/bin/test_config_wiring "★ C10.17*"` → **All tests passed (85 assertions in 2 test cases)**；
`"★ C10.12*"` → **All tests passed (19 assertions in 1 test case)**（C10.12 用例体仍未改）；
`ctest` **77/77**；`./build/bin/test_config_wiring` 全量 **692 断言 / 31 用例**。
