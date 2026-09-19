# 阶段 10 测试证据（✅ 切片 1/2/3 全部完成：配置面接线 + GC/expiry + 审计 fail-closed/SQLite 调优/鉴权与 gRPC 面 + C10.16）

| 项 | 值 |
| --- | --- |
| 阶段 | P10（配置面接线） |
| 状态 | ✅ **切片 1/2/3/4/5 全部完成 + C10.16 + C10.16 续**：C10.1~C10.17 满足；**切片 4** 接通数据面 PUT 上限与 SQLite PRAGMA（见 §9）；**切片 5（C10.17）** 接通 `self_signed` 三键（见 §10） |
| 门槛命令 | `ctest -L phase10 && scripts/verify_config_wiring.sh` |
| 退出码 | `0` |
| 新测试 | `tests/integration/test_config_wiring.cpp`：**31 个 TEST_CASE / 692 断言**（真实 `build/bin/fss_server` + 驱动层；切片 5 新增 2 用例 / 81 断言，父代理 §10.6 再 +3 断言）+ `tests/unit/test_transfer_token_key_id.cpp`：**4 个 TEST_CASE / 29 断言**（C10.17 codec 边界 + **切片 6a**：`tests/integration/test_remote_validators.cpp`（真实进程 + mock；见 §11）+ `tests/unit/test_composition_root_guard.cpp` 的清单纯增加 2 个类型；`test_config_wiring.cpp` 新增 C10.11 的 `fail_closed` 三 SECTION（+159 断言，见 §11.3） |
| 脚本 | `scripts/verify_config_wiring.sh`：**54 条断言**（切片 1 的 22 条 + 切片 2：GC 调度 4 + `--once` 3 + 样例配置启动/拒绝 6 + C10.11 拒绝 15 + expiry 3 + 就绪 1） |
| 全阶段门槛 | `./scripts/run_all_gates.sh` → **P0~P10 全绿，总耗时 348 s（5 分 48 秒，11 个阶段）**（含 ASan+UBSan 全量）；`ctest` **77/77** 通过 |
| sanitizer | `run_sanitizers.sh` 已自动纳入 `phase10`（`✓ phase10 在 sanitizer 下通过`） |
| 配置键三态 | `config/fss.example.json` **156** 个叶子键：**生效 113 / 拒绝启动（触发条件）18 / 已读但无效果 25**（C10.20 后；切片 6b 时为 107/18/31）（切片 3 新接通 9 键；**C10.16** 接通 1 键 + 2 键改为拒绝启动；**C10.16 续**接通 7 键 + 3 键改为拒绝启动；**切片 4** 接通 4 键；**切片 5** 接通 3 键；**切片 6a（C10.18）** 接通 6 键 + `auth.remote_entitlements.fail_closed` 更正为拒绝启动；**切片 6b（C10.19）** 接通 `events.publisher` 与 `events.webhook.{url,timeout_ms,topic}` 4 键）（逐键见 `docs/operations.md` §1.2 的"接通状态"列与 §1.3 的三个清单；由 §1.2 的 156 行程序化核对得出，`test_operations_doc` 机械断言） |
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
最终三态为生效 **96** 个 / 拒绝启动（触发条件）**21** 个 / 已读但无效果 **39** 个（**96** + **21** + **39** = 156）。
> 本数字是切片 5 收尾时的**历史快照**，**已被 §11 的切片 6a 取代**（+6 键生效、`auth.remote_entitlements.fail_closed` 更正为拒绝启动）。

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
  `tests/unit/test_operations_doc.cpp`（期望值 96 / 21 / 39 —— **切片 6a 已改为 102 / 20 / 34**，见 §11）。

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

---

## 11. 切片 6a（本轮）：接通「远端法务校验器」与「远端 schema 校验器」（C10.18 / ADR-013）

**背景**：`docs/operations.md` §1.3.3 的三态里，`legal.validator` / `schema.validator` 是
「拒绝启动（`remote` → exit 78）」，四个 `*.remote.{base_url,timeout_ms}` 是「已读但无效果」。
本轮把这 **6 个键**接通成**生效**：新增两个 L2 适配器（`RemoteLegalValidator` /
`RemoteSchemaValidator`），组合根按 `*.validator` 选择 `noop`（默认，行为逐字不变、**不发请求**）
或 `remote`。

### 11.1 逐键结论（6 个键）

| 键 | 最终状态 | 判据 / 证据 |
| --- | --- | --- |
| `legal.validator` | **生效** | `noop`（默认）只保留本地空值防线（`legaltags` 为空 → 400）且**不发起任何请求**（mock `requests == 0`，C10.18 ⑤）；`remote` → 组合根装配 `RemoteLegalValidator`；非法取值由 schema 的 `Enum({"noop","remote"})` 拒绝 → exit 78（C10.11 的 `bogus` 反向用例）。横幅打印 `validators : legal=remote（端点 …，timeout=…ms …）` |
| `legal.remote.base_url` | **生效** | → `RemoteLegalValidatorOptions.base_url`；**POST 到该 URL，不追加任何路径**（与 `auth.remote_entitlements` 有 `authorize_path` 不同，见 ADR-013 §2）。真实进程证据：mock 收到的请求体恰好是 `{"partition","legaltags"}` 且**不含** `record`（C10.18 ①）；空值 + `remote` → **exit 78** + 可读原因（C10.18 ⑦） |
| `legal.remote.timeout_ms` | **生效** | → `CURLOPT_TIMEOUT_MS`。C10.18 ④：**同一个 mock**（delay 800ms）在 `timeout_ms=300` → **503**、`timeout_ms=3000` → **201**（证明配置真的在起作用，而不是"永远 503"） |
| `schema.validator` | **生效** | 同 `legal.validator` 的对称语义（`noop` 不发请求；`remote` 装配 `RemoteSchemaValidator`；非法值 → exit 78） |
| `schema.remote.base_url` | **生效** | → `RemoteSchemaValidatorOptions.base_url`；请求体为 `{"kind","record"（完整记录）}` 且**不含** `partition`/`legaltags`（C10.18 ⑥）；空值 + `remote` → exit 78 |
| `schema.remote.timeout_ms` | **生效** | → `CURLOPT_TIMEOUT_MS`；超时 → 503（与 legal 同一套 fail-closed 矩阵） |

### 11.2 实现点（可点击）

* 新增 `src/infra/legal/remote_legal_validator.{h,cpp}` 与 `src/infra/schema/remote_schema_validator.{h,cpp}`：
  与 `RemoteEntitlementsAuthorizer` **同一套写法**（`CURLOPT_NOSIGNAL` / `FOLLOWLOCATION=0` /
  `CONNECTTIMEOUT_MS` + `TIMEOUT_MS` / `WriteToString` / 状态码判定 / **所有依赖故障 → `kUnavailable`** /
  `Ready()` + `NotReadyReason()`）。文件头写明"这是**本项目的约定**、未与真实服务联调"，
  以及"端口签名不带 bearer token → 端点必须允许无 per-request 认证访问"。
* `src/CMakeLists.txt`：新增 L2 目标 `fss_legal_remote` / `fss_schema_remote`（`fss_domain` + `fss_json` +
  `CURL::libcurl`），并链进 `fss_server`。
* `src/main/server_main.cpp`：读 6 个键 + 按选择器装配（**R12：只在组合根创建具体实现**）；
  `base_url` 为空 → `拒绝启动：…（fail-closed）` + `kExitConfigError`；横幅新增
  `validators : legal=… | schema=…`（**不打印任何密钥**）。
* `src/common/config/core_schema.cpp`：**无需改动** —— `legal.validator` / `schema.validator` 早就有
  `.Enum({"noop","remote"})`（本轮确认，并把它写进注释与文档）。
* `src/app/usecases/usecases.cpp`：第 3c 步的注释「失败即 400」**过时**（远端依赖故障是 503）→
  改为"失败方向由 `ErrorKind` 决定"（代码路径不变，`FSS_TRY` 已透传 ErrorKind）。
* `tests/unit/test_composition_root_guard.cpp`：具体实现清单加 `RemoteLegalValidator` / `RemoteSchemaValidator`
  （否则"组合根装配了几个"的非空洞性断言看不到它们）。
* 测试：新增 `tests/tools/mock_validators.py` + `tests/framework/mock_validators.h`（照
  `mock_entitlements` 的做法：`--port 0` 打印 `LISTENING <port>`、`popen` 取 pid、析构 kill、轮询就绪；
  额外 `--observe-file` 原子落盘请求计数与请求体）+ `tests/integration/test_remote_validators.cpp`
  （真实 `build/bin/fss_server`）。
* 文档：`docs/adr/ADR-013-file-service-extension-endpoints.md`（新）、`docs/03-api-contract.md` §7/§7.1、
  `docs/operations.md` §1.2.11 + §1.3、`docs/02-design.md`（L2 模块表 + §17 索引）、
  `docs/00-final-design.md` §4、`docs/04-implementation-plan.md`、`AGENTS.md`、`config/fss.example.json` 注释。

### 11.3 实测命令与输出摘要

```
$ cmake --build build -j4                 # ★ 用 -j4（AGENTS §4.3 的 OOM 陷阱）
[100%] Built target test_remote_validators
$ ./build/bin/test_remote_validators
All tests passed (464 assertions in 8 test cases)
$ ./build/bin/test_config_wiring "★ C10.11*"
All tests passed (244 assertions in 1 test case)     # 该 TEST_CASE 原有 85 断言 + 新增 fail_closed 三个 SECTION
$ ./build/bin/test_operations_doc
All tests passed (24 assertions in 2 test cases)     # 三态 102 / 20 / 34（§11 当时值；已由 §12 更新为 106 / 19 / 31）
$ ./build/bin/test_composition_root_guard
All tests passed (17 assertions in 2 test cases)
$ ctest --test-dir build -L phase10 --output-on-failure
100% tests passed, 0 tests failed out of 3            # test_config_wiring / test_operations_doc / test_remote_validators
$ ctest --test-dir build -j4
100% tests passed, 0 tests failed out of 78
$ ./scripts/check_docs.sh --selftest
  ✓ 自证：D1/D2/D4/D5 都能检出注入的错误（检查器有效）
  D3 ADR 文件 13 个，被引用 13 个编号
  D5 门槛编号检查：11 个阶段，共 144 条门槛
  全部检查通过（D1~D5）
$ ./scripts/verify_config_wiring.sh
配置面接线：全部通过（54 条断言）
```

**fail-closed 矩阵逐条实测**（C10.18 ③ 的五态 + 400 两侧；每条都断言**无残留**）
`RequireNoPersistentSideEffect()` 的判据：`uploadURL` 返回的 `Location` 仍指向 **staging**
容器，且对应的 **persistent** 文件不存在（校验在用例第 3c 步、复制/落库之前）。

| 依赖侧事实 | 期望 | 实测 |
| --- | --- | --- |
| `200 + {"valid":true}` | 201 | **201**（C10.18 ①，legal / ⑥ schema） |
| `200 + {"valid":false,"message":"bad tag"}` | 400 + 消息含 `bad tag` | **400**，body `"message":"bad tag"`；staging 仍在、persistent 无文件（C10.18 ②） |
| 超时（mock delay 2000ms > timeout 300ms） | 503 | **503**（C10.18 ③-①），无残留 |
| 非 200（`--status 500`） | 503 | **503**（③-②），无残留 |
| 200 但非 JSON（`--malformed`） | 503 | **503**（③-③），无残留 |
| 200 但缺 `valid`（`--no-valid-field` → `{}`） | 503 | **503**（③-④），无残留 |
| 连不上（指向未监听端口） | 503 | **503**（③-⑤），无残留 |
| `base_url` 为空 + `validator=remote` | exit 78 + 可读原因 | **exit 78**，输出含 `legal.remote.base_url` / `schema.remote.base_url`（C10.18 ⑦） |
| `noop` + `*.remote.*` 配了地址 | 不发起请求、行为不变 | **201** + mock `requests == 0`（400ms 窗口）+ schema 侧指向未监听端口也 201（C10.18 ⑤） |
| `timeout_ms` 真的来自配置 | 300ms → 503；3000ms → 201 | **503 / 201**（同一个 mock，C10.18 ④） |

### 11.4 R1 自证（注入 → 用例失败 → 还原 → 实测输出）

三个"错误实现"各注入一次，都让对应用例**失败**；随后**完整还原**，
`grep -rn "R1-INJECT" src/` 无输出（rc=1）。

**① 把 fail-closed 分支改成 fail-open**（在 `curl_easy_perform` 之后插入：传输失败 / 非 200 /
缺 valid → `return fss::Ok();`）→ 四态用例必须失败：

```
$ ./build/bin/test_remote_validators
tests/integration/test_remote_validators.cpp:408: FAILED:
  REQUIRE( result.create_status == 503 )
with expansion:
  201 == 503 (0x1f7)
with messages:
  test_case.name := "超时"
  result.create_status := 201
tests/integration/test_remote_validators.cpp:447: FAILED:   REQUIRE( result.create_status == 503 )
tests/integration/test_remote_validators.cpp:569: FAILED:   REQUIRE( result.create_status == 503 )
test cases:   8 |   5 passed | 3 failed
assertions: 265 | 262 passed | 3 failed
```

**② 把"缺 `valid` 字段"读成 `true`**（`if (!root.contains("valid")) return fss::Ok();`）
→ `--no-valid-field` 用例必须失败：

```
$ ./build/bin/test_remote_validators "★ C10.18 ③*"
tests/integration/test_remote_validators.cpp:408: FAILED:
  REQUIRE( result.create_status == 503 )
with expansion:
  201 == 503 (0x1f7)
with messages:
  fail-closed 形态：缺 valid 字段
  test_case.name := "缺 valid 字段"
  result.create_status := 201
test cases:  1 |  0 passed | 1 failed
assertions: 142 | 141 passed | 1 failed
```

**③ 让适配器忽略 `timeout_ms`（硬编码 30000）** → timeout 用例必须失败：

```
$ ./build/bin/test_remote_validators "★ C10.18 ④*"
tests/integration/test_remote_validators.cpp:447: FAILED:
  REQUIRE( result.create_status == 503 )
with expansion:
  201 == 503 (0x1f7)
  timeout_ms := 300 (0x12c)
  result.create_status := 201
test cases:  1 |  0 passed | 1 failed
assertions: 26 | 25 passed | 1 failed
```

还原后的实测：

```
$ grep -rn "R1-INJECT" src/                          # 无输出（rc=1）
$ ./build/bin/test_remote_validators                  → 9 用例（含父代理补的 ⑧）；见 §11.4.1
$ ./scripts/run_all_gates.sh                          # 父代理在最终工作树上重跑
  汇总
    失败: 无
  ⏱  总耗时: 5 分 15 秒（315 s，阶段数 11）
```

**父代理独立复核**：`cmake --build build -j4`、`ctest` **78/78**、`check_docs.sh --selftest`
（13 ADR / 144 门槛）、`test_operations_doc` 24 断言、`run_all_gates.sh` 全绿（315 s / 11 阶段）。

#### 11.4.1 父代理复核：注入自证**第一次没失败**，抓到一个判据缺口（已修）

复核时我按 R1 自己重做了一遍注入，**第一次全绿**（用例没失败）—— 这本身就是"判据无效"的信号，
追下去发现两处**真实缺口**：

**① `--status 500` 的响应体让"状态码检查"无法被单独证明。** mock 当时返回
`{"error":"injected"}`（没有 `valid`），于是"非 200 必须 fail-closed"与"缺 `valid` 必须
fail-closed"**两条防线同时触发**：把状态码检查改成 `if (false)`，用例照样 503 通过。
判据无法区分"状态码真的被检查了"与"只是恰好缺 `valid`"。

修法：mock 的失败状态码改为回一个**伪装成通过**的体 `{"valid":true,"note":"injected status"}`
（`--fail-file` 同理），并在 mock 里写明理由。修后再做同一个注入：

```
# 注入：remote_legal_validator.cpp 的 if (status != 200) → if (false)
$ cmake --build build -j4 && ./build/bin/test_remote_validators "★ C10.18 ③*"
  result.create_status := 201        # 依赖回了 500 却说"通过"，而我们没检查状态码 → 记录被建出来
tests/integration/test_remote_validators.cpp:408: FAILED: REQUIRE( result.create_status == 503 )
test cases:  1 |  0 passed | 1 failed      assertions: 66 | 65 passed | 1 failed
# 还原后：All tests passed (185 assertions in 1 test case)
```
即：**现在这条判据真的能失败**。（顺便修正了一个更早的版本：只 `--target test_remote_validators`
重建是**不够的** —— 用例拉起的是 `build/bin/fss_server`，必须全量重建，否则注入根本没进被测二进制。）

**② `--fail-file` 是"声明了但没人用"的开关。** 规格要求"删掉控制文件 = 依赖恢复"，
但没有任何用例使用它。补 `C10.18 ⑧`：控制文件存在 → **503 且无残留**；**同一个服务进程**、
不重启、只删掉文件 → 下一次请求立刻 **201**。这条同时锁住"不缓存依赖结论"这个真实性质
（若实现第一次失败就记住，或成功一次就不再问，它必然失败）。反向验证：

```
# 注入：忽略依赖的回答（200 时直接 return Unavailable）
$ ./build/bin/test_remote_validators "★ C10.18 ⑧*"
  up.create_body := {"code":503,"message":"R1-INJECT: 忽略依赖的回答",...}
tests/integration/test_remote_validators.cpp:...: FAILED: REQUIRE( up.create_status == 201 )
test cases:  1 |  0 passed | 1 failed      assertions: 53 | 52 passed | 1 failed
# 还原后：All tests passed (54 assertions in 1 test case)
```

**③（撤回的错误做法）**用 `--status 500` 那条用例去证明"状态码被检查"的**原始**尝试不成立 ——
上面 ① 已给出真正的证明方式。这两条都记在这里，因为它们正是 R1 存在的意义：
"用例通过了"与"用例能失败"是两件事。

#### 11.4.2 父代理**事后更正**：本切片的"无残留"断言曾经是**恒真**的（切片 6b 复核时发现）

切片 6b 的实现者在写它自己的"记录真的建出来"断言时发现：`getFileList` 的 `Location` 是
**容器相对路径**（`opendes-staging/...`），而 POSIX 驱动的物理根是
`<storage.posix.root>/blobs`（组合根拼的）。本切片的 `RequireNoPersistentSideEffect` 当时是：

```cpp
const std::filesystem::path persistent_path(persistent);   // 相对路径！
REQUIRE_FALSE(std::filesystem::exists(persistent_path));    // 测试进程 CWD 下恒为 false → 恒真
```

也就是说 **"persistent 侧没有对应文件"这一条从未被真正验证**（它只证明了"这个相对路径不存在"）。
修法（父代理改，并配自证）：

```cpp
const std::filesystem::path staging_path    = std::filesystem::path(blob_root) / location;
const std::filesystem::path persistent_path = std::filesystem::path(blob_root) / persistent;
REQUIRE(std::filesystem::exists(staging_path));            // ★ 正控：证明路径解析是真的
REQUIRE_FALSE(std::filesystem::exists(persistent_path));    // 目标判据（现在有牙齿）
```

调用点 6 处统一改为传 `data_dir.child("store") + "/blobs"`。自证（把目标判据**反转**必须失败）：

```
$ ./build/bin/test_remote_validators          # 反转 REQUIRE_FALSE → REQUIRE
tests/integration/test_remote_validators.cpp:207: FAILED:
  persistent 侧不应存在的文件：/tmp/fss_.../store/blobs/opendes-persistent/...
# 还原后：All tests passed (528 assertions in 9 test cases)   ← 464 → 528（+6 条正控）
```

**这条比切片本身更值得记**：凡是**否定式**判据（`REQUIRE_FALSE`）都必须配一条**正控**
（同一条路径解析上断言"该存在的东西**存在**"），否则路径写错、对象不存在、名字拼错都会让
判据静默恒真（与 §11.4.1 的"两条防线重合"同族：都是**判据无区分力**）。已同步写进
`AGENTS.md` §4.3。

### 11.5 规格勘误与一处**独立理由的更正**（都必须写清楚）

**① 规格勘误（父代理确认）**：本切片的规格原写"把 **7** 个键变成生效，最终三态
生效 103、拒绝启动 19、其余 34 键已读但无效果"。这是**父代理规格里的算术错误** —— 实际只有
**6 个键**（`legal.validator`、`schema.validator` 来自「拒绝启动」= 2 个；
四个 `*.remote.{base_url,timeout_ms}` 来自「已读但无效果」= 4 个），正确落点是
**102 / 19 / 35**。实测机械核对：HEAD 为 `EFF 96 / REJ 21 / INE 39`，接线后
`EFF 102 / REJ 19 / INE 35`，逐键 diff **恰好只有这 6 个键改变**。

**② 独立理由的更正**：`auth.remote_entitlements.fail_closed` 从「已读但无效果」更正为
「拒绝启动（触发条件）」。理由**不是**凑数字，而是原登记**自相矛盾**：`operations.md` 那一行
自己写着"`remote` 模式下 schema 会拒绝 `false`"，却把该键标成"无效果"。
`core_schema.cpp` 的跨字段校验确实有**真实、可观测**的效果：
`auth.mode=remote-entitlements` 且该键非 `true` → `problems` 带
"必须为 true：依赖不可用不可降级为放行" → **exit 78**。
**触发条件是模式相关的**：`auth.mode=jwt`/`disabled` 时 `false` 被接受且无任何影响
（实现内恒为 fail-closed，不存在"失败即放行"的分支）。
真实进程用例：`tests/integration/test_config_wiring.cpp` 的 C10.11 新增三个 SECTION ——
反例（`remote-entitlements` + `false` → exit 78 + 可读原因）、R16 正例（同模式 + `true` + 有地址
→ **不因该键**被拒）、模式无关性（`disabled` + `false` → 不因该键被拒）。
**它不做也不影响切片 6a 的正确性**（落点不同：一个进「生效」+6，一个进「拒绝启动」+1）。

### 11.6 三态计数（切片 6a 收尾；**最终**）

`config/fss.example.json` 的 **156** 个叶子键：生效 **102** / 拒绝启动（触发条件）**20** /
已读但无效果 **34**（**102** + **20** + **34** = 156），由 `tests/unit/test_operations_doc.cpp`
的 C10.11 用例从 §1.2 的 156 行程序化提取并机械断言（表格计数 + 正文两处字符串同时断言）。
净变化：6 个键移入「生效」（+6）；`auth.remote_entitlements.fail_closed` 移入「拒绝启动」（+1）。

### 11.7 未做 / 降级 / 未验证（如实登记）

* **未与真实 Legal / Schema 服务联调**（本环境没有该服务、外网受限）：协议形状见 ADR-013 §2，
  是本项目与运维方的约定；`base_url` 由运维给出正是为了适配真实路由。上游**没有**这条调用链
  （`docs/01-osdu-research.md:110`：legal tag 由 Storage Service 的 PUT /records 内部校验），
  所以**不存在**"与上游对齐"这回事 —— 这是本服务的扩展。
* **不透传调用方身份**：端口签名 `Validate(partition, tags)` / `Validate(kind, record)` 没有
  bearer token 参数 → 端点必须允许**无 per-request 认证**访问（集群内网 / mTLS 终结 /
  网络策略白名单）。若真实服务要求鉴权，必须**改端口契约**（新增参数 + 同步契约 §5/§6 与
  两个适配器）——本切片不做，也不假装做了。
* **不做校验结果缓存**（每请求一次远端 RTT）；**不做重试/退避**（失败即 503，重试会把"依赖降级"
  伪装成"只是慢"）。
* **`*.remote.connect_timeout_ms` 未暴露为配置键**（固定 1000ms；与 Entitlements 的键集不同）。
* **webhook（`events.publisher=webhook`）仍拒绝启动**：ADR-013 §5.3-⑥ 已为它立好"完整 URL +
  fail-closed + 不透传身份"三条规矩，但**方向不同**（出站通知），落地时要单独定"通知失败是否致命"。
* 本轮**未**跑 `run_all_gates.sh` 全量（由父代理收尾跑）；上面与本切片相关的命令全绿。
* **一处测试自身的假判据（本轮实测抓到并修掉）**：最初用"`getFileList` 必须 400（无记录）"
  证明"503 后没有记录被建出来"，但 `getFileList` 列的是**位置仓储**而不是元数据 ——
  `uploadURL` 一旦签发就已经写了一条 staging 位置记录，所以该断言拿到 **200**。
  改成两条**可证**的副作用：`Location` 仍指向 **staging** 容器 且 persistent 侧**没有**对应文件。

---

## 12. 切片 6b（本轮）：接通事件发布器 `events.publisher` 与 `events.webhook.*`（C10.19 / ADR-013 §9）

**背景**：`docs/operations.md` §1.3 的三态里，`events.publisher` 是「拒绝启动
（`webhook`/`none` → exit 78）」，`events.webhook.{url,timeout_ms,topic}` 是「已读但无效果」。
本轮把这 **4 个键**接通成**生效**：新增 L2 适配器 `WebhookEventPublisher`，组合根按
`events.publisher` 选 `log`（默认，行为逐字不变）/ `webhook` / `none`（显式关闭）。

**与切片 6a 的关键差异（必须同时被两条测试各自钉住）**：6a 的远端校验是 **fail-closed**
（依赖故障 → 503、**无残留**）；事件发布是**非致命**的（上游只
`log.warning("Failed to publish ...")`）——依赖故障**绝不能让请求失败**：建记录请求照常
**201**，且记录**真的建出来**。两条语义方向相反，`test_remote_validators.cpp`（503 + 无残留）
与 `test_webhook_publisher.cpp`（201 + 有文件）是**镜像**判据。

### 12.1 逐键结论（4 个键）

| 键 | 最终状态 | 判据 / 证据 |
| --- | --- | --- |
| `events.publisher` | **生效** | `log`（默认）= 既有 `LogEventPublisher`（写日志、**不发请求**，行为逐字不变）→ C10.19 ④（mock `requests == 0` + 日志里仍有 `"msg":"status-changed"`）；`webhook` = 装配 `WebhookEventPublisher` → C10.19 ①（201 + 两个事件都到）；`none` = 内联 `NoopEventPublisher`（**显式关闭**）→ C10.19 ③（mock `requests == 0` 且**连日志都不写**）。非法取值由 schema 的 enum 拒绝 → exit 78（`test_config_wiring.cpp` 的 `events.publisher=bogus`）。横幅打印 `events : publisher=…`（**不打印密钥**） |
| `events.webhook.url` | **生效** | → `WebhookEventPublisherOptions.url`；**POST 到该 URL，不追加任何路径**（与 6a 的 `*.remote.base_url` 同一约定，ADR-013 §2/§9）。真实进程证据：mock 收到的每个 body 都是 `{"topic","kind","body"}` 形状（C10.19 ①）；空值 + `publisher=webhook` → **exit 78** + 可读原因（C10.19 ⑤） |
| `events.webhook.timeout_ms` | **生效** | → `CURLOPT_TIMEOUT_MS`（连接超时 = `min(1000, timeout_ms)`）。C10.19 ⑥：**同一个 mock**（`--delay-ms 800`）在 `timeout_ms=300` → 日志有超时告警、`timeout_ms=3000` → **无告警**且 mock 收到带**本轮记录 id** 的两个 kind。三次形态的实测见 §12.4 |
| `events.webhook.topic` | **生效** | → 两个事件载荷里的 `topic`（`statusChanged` 与 `datasetDetails` **都用配置值**）。C10.19 ① 用非默认值 `fss-events-test`：mock 收到的所有 body 的 `topic` 都是它；R1 自证②把实现硬编码成 `status-changed` → 该断言失败（§12.5） |

### 12.2 实现点（可点击）

* 新增 `src/infra/event/webhook_event_publisher.{h,cpp}`（L2）：照 `remote_legal_validator.*`
  的写法（`CURLOPT_NOSIGNAL` / `FOLLOWLOCATION=0` / `CONNECTTIMEOUT_MS` + `TIMEOUT_MS` /
  `WriteToString` / 2xx 判定 / `Ready()` + `NotReadyReason()`），但失败方向相反：非 2xx /
  连不上 / 超时 → 返回 `Err(kUnavailable)` **并记一条 `Warn`**（用例层丢弃 Err，请求不受影响）。
  载荷用 `json::Value` 构造（不拼字符串）：`statusChanged` 的 `body` 是对象；
  `datasetDetails` 的 `body` 是**长度 1 的数组**、元素 `{"properties":{...}}`
  （对齐上游 `FileDatasetDetailsPublisher.java`，见 `ports.h` 注释）。
* `src/CMakeLists.txt`：新增 L2 目标 `fss_event_webhook`（`fss_domain` + `fss_json` +
  `fss_logging` + `CURL::libcurl`）并链进 `fss_server`。**链接 `fss_logging` 的理由**：
  "发布失败要留一条可读告警"是适配器自身的职责（用例层按上游语义丢弃 `Result`，
  没有别的层能记这条告警）。
* `src/main/server_main.cpp`：删掉 `if (events_publisher != "log") return reject_startup(...)`；
  新增三分支（`log` / `webhook` / `none`）+ 内联 `NoopEventPublisher`（R12：具体实现只在组合根
  创建）；`webhook` + 空 `url` → `Ready()`/`NotReadyReason()` **exit 78**；横幅新增
  `events : publisher=…`（含端点/timeout/topic；**不打印密钥**）。
* `src/app/usecases/usecases.cpp`：`PublishStatus` 增加 `record_id` 参数（第 10 步 / 幂等命中
  路径带真实 id；第 1 步 IN_PROGRESS 在建记录之前 → 空），使 `statusChanged.body.recordId`
  与上游形状一致。**调用点仍是 `(void)ports.events.Publish...`（失败被丢弃）—— 这是本切片
  的核心语义，不得改成 `FSS_TRY`。**
* `tests/tools/mock_validators.py` + `tests/framework/mock_validators.h`：新增
  `--mode webhook`（默认回 200 `{"ok":true}`）与观测字段 `bodies`（**全部**请求体，按到达顺序；
  保留 `last_body` 兼容 6a）；请求体改在故障短路**之前**读取/观测，因此失败形态下也能断言
  "mock 真的收到了事件体"。`Observation` 新增 `BodiesOfKind(kind)` 与 `last_headers`。
* `tests/integration/test_webhook_publisher.cpp`（新，6 用例）+ `tests/CMakeLists.txt`；
  `tests/unit/test_composition_root_guard.cpp` 清单加 `WebhookEventPublisher`。

### 12.3 实测命令与输出摘要

```
$ cmake --build build -j4                 # ★ 用 -j4（AGENTS §4.3 的 OOM 陷阱）
[100%] Built target test_config_wiring
$ ./build/bin/test_webhook_publisher
All tests passed (331 assertions in 6 test cases)
$ ./build/bin/test_remote_validators
All tests passed (518 assertions in 9 test cases)
$ ./build/bin/test_config_wiring
All tests passed (851 assertions in 31 test cases)
$ ./build/bin/test_operations_doc
All tests passed (24 assertions in 2 test cases)     # 三态 106/19/31
$ ./build/bin/test_composition_root_guard
All tests passed (17 assertions in 2 test cases)
$ ctest --test-dir build -L phase10 --output-on-failure
100% tests passed, 0 tests failed out of 4            # + test_webhook_publisher
$ ctest --test-dir build -j4
100% tests passed, 0 tests failed out of 79
$ ./scripts/check_docs.sh --selftest
  ✓ 自证：D1/D2/D4/D5 都能检出注入的错误（检查器有效）
  D5 门槛编号检查：11 个阶段，共 145 条门槛          # +C10.19（无断号）
  全部检查通过（D1~D5）
$ ./scripts/verify_config_wiring.sh
  ✓ events.publisher=webhook + 空 url（exit 78）（=78）
  配置面接线：全部通过（54 条断言）
```

**§7.2 / ADR-013 §9 的载荷形状逐条实测**（C10.19 ①，判据来自 mock 的 `bodies`）：

| 断言 | 实测 |
| --- | --- |
| 一次 `createMetadata` 发 3 个事件（`IN_PROGRESS`、`SUCCESS`、`datasetDetails`） | mock `requests == 3`；`BodiesOfKind("statusChanged").size() == 2`、`datasetDetails == 1` |
| `topic` 来自**配置**（非默认 `fss-events-test`） | 3 个 body 的 `topic` 全部 == `fss-events-test` |
| `statusChanged.body` | `{recordId,partition:"opendes",status,datasetSync:"DATASET_SYNC",version}`；`recordId` 与 201 响应体的 `id` **逐字相等**；`IN_PROGRESS` 的 `version==0` 且 `recordId==""`；`SUCCESS` 的 `version>=1` |
| `datasetDetails.body` | **长度 1 的数组**；`properties.datasetId`==记录 id、`datasetType=="FILE"`、`recordCount==1`、`datasetVersionId=="1"`、`timestamp>0`、含 `correlationId` |
| 表头 | `Content-Type: application/json` |

### 12.4 **非致命**三种故障形态的实测（本切片核心）

判据（每种形态都必须**同时**满足）：① 请求仍 **201**；② 记录**真的建出来**（`getFileList`
恰好 1 条、`Location` 指向 **persistent** 容器、且该文件在 `<storage.posix.root>/blobs/` 下
**真实存在**）；③ 日志里有一条**可读**告警。三种形态在同一台机器上依次实测（独立配置 +
真实 `build/bin/fss_server`；前两种用 mock 注入，第三种用未监听端口）：

| 形态 | `createMetadata` | `getFileList` | persistent 文件存在 | 日志告警（截断） |
| --- | --- | --- | --- | --- |
| **连不上**（未监听端口，timeout=1000ms） | **201** | 1 条，`opendes-persistent/osdu-user/…` | **是**（`/tmp/wh_closed/store/blobs/opendes-persistent/…`） | `Failed to publish event: 事件发布失败（传输层，kind=statusChanged）：Couldn't connect to server（端点 http://127.0.0.1:36565，timeout=1000ms）` |
| **非 2xx**（mock `--status 500`，timeout=3000ms） | **201** | 1 条，persistent | **是**（`/tmp/wh_http500/store/blobs/opendes-persistent/…`） | `Failed to publish event: 事件发布失败（非 2xx，kind=statusChanged）：HTTP 500（端点 http://127.0.0.1:35153）` |
| **超时**（mock `--delay-ms 800`，timeout=300ms） | **201** | 1 条，persistent | **是**（`/tmp/wh_timeout/store/blobs/opendes-persistent/…`） | `Failed to publish event: 事件发布失败（传输层，kind=statusChanged）：Timeout was reached（端点 http://127.0.0.1:44703，timeout=300ms）` |

同一组断言也在 `test_webhook_publisher.cpp` 的 **C10.19 ②** 里被机械重放
（`All tests passed (106 assertions in 1 test case)`）。

★ 一处**判据自身的坑**（本轮实测抓到）：`getFileList` 的 `Location` 是**容器相对路径**
（`opendes-persistent/…`），而 POSIX 驱动的物理根是 `<storage.posix.root>/blobs`（组合根
`storage_root + "/blobs"`）。最初直接 `std::filesystem::exists(location)` → **恒为 false**
（恒真的反面：恒假）。已改成用 `blobs` 根拼出**真实物理路径**再断言 —— 该断言现在能失败
（若 persistent 复制没发生就会 false），是本切片"记录真的建出来"的可证判据。
（顺带记录：6a 的 `RequireNoPersistentSideEffect` 用的是**相对** `exists`，那里只因为断言的是
`REQUIRE_FALSE` 才没有暴露；本切片**没有**把它当成"文件不存在"的正面证据。）

### 12.5 R1 自证（注入 → 用例失败 → 还原 → 实测输出）

三个"错误实现"各注入一次，都让对应用例**失败**；随后**完整还原**
（`diff` 三个备份文件全部一致），`grep -rn "R1-INJECT" src/` 无输出（rc=1）。
**每次注入后都 `cmake --build build -j4` 全量重建**（用例拉起的是 `build/bin/fss_server`，
只重建测试目标注入不会进被测二进制 —— 切片 6a 的教训）。

**① 把发布失败改成致命**（`PublishStatus` 改为返回 `fss::Result<void>` 并在内部
`FSS_TRY(ports.events.PublishStatusChanged(...))`，第 1/10 步与幂等命中路径都改 `FSS_TRY`）
→ C10.19 ② 必须失败：

```
$ ./build/bin/test_webhook_publisher "★ C10.19 ②*"
tests/integration/test_webhook_publisher.cpp:376: FAILED:
  REQUIRE( result.create_status == 201 )
with expansion:
  503 == 201 (0xc9)
with messages:
  test_case.name := "连不上（未监听端口）"
  result.create_status := 503 (0x1f7)
  result.create_body := "{"code":503,"message":"事件发布失败（传输层，kind=statusChanged）：
                        Couldn't connect to server（端点 http://127.0.0.1:55455，timeout=1000ms）",
                        "reason":"Service Unavailable"}"
test cases:  1 |  0 passed | 1 failed
assertions: 22 | 21 passed | 1 failed
```

**② 让发布器忽略 `events.webhook.topic`**（`EffectiveTopic` 硬编码 `"status-changed"`）
→ C10.19 ① 的 topic 断言必须失败：

```
$ ./build/bin/test_webhook_publisher "★ C10.19 ①*"
tests/integration/test_webhook_publisher.cpp:286: FAILED:
  REQUIRE( message["topic"].get<std::string>() == "fss-events-test" )
with expansion:
  "status-changed" == "fss-events-test"
test cases:  1 |  0 passed | 1 failed
assertions: 38 | 37 passed | 1 failed
```

**③ 让 `none` 仍然"发布"**（组合根 `none` 分支错接 `log_events`）→ C10.19 ③ 必须失败：

```
$ ./build/bin/test_webhook_publisher "★ C10.19 ③*"
tests/integration/test_webhook_publisher.cpp:419: FAILED:
  REQUIRE( banner.find("\"msg\":\"status-changed\"") == std::string::npos )
with expansion:
  ...
  "msg":"events.publisher=none：已**显式关闭**  ...（找到内容）
test cases:  1 |  0 passed | 1 failed
assertions: 40 | 39 passed | 1 failed
```

**③ 的判据缺口（值得单独记，与 §11.4.1 同类）**：按本切片最初的规格，C10.19 ③ 只有
"201 / 记录建出来 / mock `requests == 0` / 无失败告警 / 横幅写 `none`" 这些断言 ——
这些**无法区分** `None` 与 `Log`，因为 `LogEventPublisher` **也不发 HTTP 请求**、也不会产生
失败告警。也就是说：把 `none` 错接成 `LogEventPublisher` 时，**原始判据会全绿**。
在跑注入 ③ 之前就预判到这一点，因此给用例 3 补了第 5 条断言 ——
**"显式关闭"= 什么都不做（连事件日志都不写）**：`DumpLog()` 里不得出现
`"msg":"status-changed"`。补上后再注入③，用例**如期失败**（上面的输出）。
教训与 §11.4.1 一致：**"用例通过了"与"用例能失败"是两件事**；这一次是**先**补判据再注入。

**还原后的实测**：

```
$ grep -rn "R1-INJECT" src/                          # 无输出（rc=1）
$ diff /tmp/r1_usecases_backup.cpp src/app/usecases/usecases.cpp      # 一致
$ diff /tmp/r1_webhook_h_backup.h  src/infra/event/webhook_event_publisher.h   # 一致
$ diff /tmp/r1_server_main_backup.cpp src/main/server_main.cpp        # 一致
$ ./build/bin/test_webhook_publisher "★ C10.19 ①*"   → All tests passed (67 assertions in 1 test case)
$ ./build/bin/test_webhook_publisher "★ C10.19 ②*"   → All tests passed (106 assertions in 1 test case)
$ ./build/bin/test_webhook_publisher "★ C10.19 ③*"   → All tests passed (40 assertions in 1 test case)
$ ./build/bin/test_webhook_publisher                 → All tests passed (331 assertions in 6 test cases)
```

### 12.6 三态计数（切片 6b 收尾；**最终**）

`config/fss.example.json` 的 **156** 个叶子键：生效 **106** / 拒绝启动（触发条件）**19** /
已读但无效果 **31**（**106** + **19** + **31** = 156），由 `tests/unit/test_operations_doc.cpp`
的 C10.11 用例从 §1.2 的 156 行程序化提取并机械断言（表格计数 + 正文两处字符串同时断言）。
净变化：`events.publisher` 移出「拒绝启动」、`events.webhook.{url,timeout_ms,topic}` 移出
「已读但无效果」→ **4 个键全部移入「生效」**（净：生效 +4 / 拒绝启动 −1 / 已读但无效果 −3）。

### 12.7 未做 / 降级 / 未验证（如实登记）

* **异步有界发布队列未交付**：本实现是**内联同步** POST。一次 `createMetadata` 发 2~3 个事件，
  一个慢 webhook 最多给请求路径增加 **事件数 × `events.webhook.timeout_ms`**（ADR-013 §9.4-1）。
  要交付必须先定义「队列上界 + 丢弃策略 + 退避参数」并新增配置键（同步 example / operations /
  自动比对测试）。
* **无重试/退避、无投递保证**：单次尝试，失败即丢弃（只留一条告警）。上游消息总线的
  at-least-once 语义**没有**被复现（ADR-013 §9.4-2）。
* **未与真实消息总线 / 中间件联调**（本环境没有）：协议形状是本项目与运维方的约定；
  上游 File Service 通过消息总线（Kafka 类）发布这两个事件，**没有** webhook 传输形态。
* **不透传调用方身份**（与 6a 同源）：载荷里没有 bearer/tenant 凭证；端点须允许
  **无 per-request 认证**访问。这是端口签名决定的，不代表身份问题已解决。
* `events.webhook.connect_timeout_ms` **未暴露为配置键**（固定 `min(1000, timeout_ms)`）。
* 本轮**未**跑 `run_all_gates.sh` 全量（由父代理在最终工作树上跑）；上面与本切片相关的
  五条命令全绿。

### 12.8 父代理复核（含对切片 6a 的一处**事后更正**）

**① 独立复核结果**：`cmake --build build -j4` 通过、`ctest` **79/79**、
`check_docs.sh --selftest` 通过（13 ADR / **145** 门槛）、`test_operations_doc` 24 断言、
`run_all_gates.sh` 全绿（**350 s / 11 阶段，失败 无**）、`grep -rn "R1-INJECT" src/ tests/` 无残留。

**② 我自己重做了一次注入**（不转述实现者）：把 `WebhookEventPublisher::EffectiveTopic`
硬编码成 `"status-changed"`（即"忽略配置的 topic"）→

```
tests/integration/test_webhook_publisher.cpp:286: FAILED:
  REQUIRE( message["topic"].get<std::string>() == "fss-events-test" )
with expansion:  "status-changed" == "fss-events-test"
assertions: 302 | 301 passed | 1 failed
# 还原后：All tests passed (331 assertions in 6 test cases)
```
即：**`events.webhook.topic` 确实来自配置**（这条判据有区分力）。非致命语义的实现机制也已核对：
`usecases.cpp` 的两个发布助手用 `(void)` 丢弃结果（`PublishStatus` / `PublishDatasetDetails`），
因此"发布失败不影响请求"是**用例层**的性质，不依赖适配器返回什么。

**③ 对切片 6a 的事后更正（由 6b 的实现者发现线索，父代理修复）**：6a 的
`RequireNoPersistentSideEffect` 用**容器相对路径**做 `REQUIRE_FALSE(exists(...))` →
在测试进程 CWD 下恒为 false → **该判据恒真，"persistent 侧无文件"从未被验证**。
已改为拼真实物理根（`<storage.posix.root>/blobs`）并**加正控**
（`REQUIRE(exists(staging_path))` 证明路径解析是真的），6 处调用点同步；
反转目标判据跑一次确认会失败（详见 **§11.4.2**）。断言数 464 → **528**（+6 条正控）。
教训已写进 `AGENTS.md` §4.3（"否定式判据必须配正控"）。

---

## 13. P9 补交（C9.31）：GC 的按需 HTTP 端点 —— 证据在 `phase9.md` §12

本文件（P10 切片 1~6b）之外，本轮**同时补交了 P9 登记未交付的一项**：GC 的 HTTP 端点
（`POST {base_path}/v2/gc:run` + `GcTask` 单飞护栏，判据编号 **C9.31**，见
`docs/04-implementation-plan.md` 的 P9 段）。因为它属于 **P9** 门槛，证据按阶段归档在
**`docs/test-evidence/phase9.md` §12**（结论 / 实现点 / 命令输出 / R1 自证 / 未做项）。

对 P10 的影响：**零**——本切片**不新增任何配置键**，`docs/operations.md` §1.3 的三态计数
当时保持 **生效 106 / 拒绝启动 19 / 已读但无效果 31 = 156** 不变（`test_operations_doc` 继续通过）；
新增两个测试二进制（`test_gc_endpoint` / `test_gc_task_single_flight`，标签 `phase9`）。

> ⚠️ **当前值已变**：ADR-008 的 P4 后为 **107/18/31**，C10.20（本文件 §14）后为 **113/18/25**。

---

## 14. C10.20（本轮）：SQLite **数据库层组提交** —— 让 6 个「已读但无效果」的键真的生效

> 判据编号 **C10.20**（`docs/04-implementation-plan.md` 的 P10 段）；逐键说明在
> `docs/operations.md` §1.2.7 / §1.2.8 与 §1.3.1；设计取舍在 `docs/02-design.md` §13.5；
> 踩到的两个陷阱登记在 `AGENTS.md` §4.3。

### 14.1 结论（先说答案）

1. **6 个键真的生效**：`metadata.sqlite.{group_commit,group_commit_max_wait_ms,group_commit_max_batch}`
   与 `location.sqlite.{同三键}` —— 之前登记为「已读但无效果」（理由："两个仓储是单连接 +
   互斥、没有组提交实现"）。本切片新增 L2 共用小工具 `src/infra/sqlite/sqlite_group_commit.h`
   并把它接到两个仓储的写路径。
2. **摊销是确定性的**（不靠 sleep）：N=8 并发 `Save`、批上限 B=4 → 提交次数 **2 == ceil(8/4)**；
   B=2 → 4；B=8 → 1；**B=1 → 8**；`group_commit=false` → **8 且每操作一个事务**（逐字回归）。
   元数据仓储同一套判据：N=6、B=3 → 2。
3. **每操作原子性可机器检查且有牙齿**：批里一个操作失败（**真实约束冲突**）→ 只有它回滚、
   同批其它操作照常提交且可读、失败操作**不留半行**、观察者看到 **1 次**保存点回滚。
   去掉 `SAVEPOINT` 的注入（整批回滚）让两条用例都红（§14.4 ③）。
4. **`max_wait_ms` 真的生效**：批永远不满时窗口是唯一的 flush 依据（`0` → 立即提交；
   `400` → 实测等满 ≥ 350ms 且有界）。忽略它的注入会**挂死**（`timeout` rc=124，§14.4 ④）。
5. **读不脏**：批事务开着时 `GetById` 在连接互斥上排队（150ms 内必须**不能**返回），
   提交后返回的才是提交后的值。
6. **真实进程可观测**：启动横幅新增 `sqlite commit :` 行；`/metrics` 新增
   `fss_sqlite_{group_commits,ops}_total{repo=...}`；一次 `createMetadata` 在
   `group_commit_max_wait_ms=400` 下耗时 **416ms**、同窗口 `group_commit=false` 下 **14ms**。
7. **诚实边界**：`*.sqlite.max_write_concurrency` **仍未生效**（单连接 ⇒ 实际并发 1）；
   组提交**不改变耐久性等级**（仍每事务一次 `COMMIT`）；**没有**任何吞吐数字（未做 R2 合规测量）。

### 14.2 实现点（可点击）

| 文件 | 内容 |
| --- | --- |
| `src/infra/sqlite/sqlite_group_commit.h` **（新增）** | 共用协调器 `SqliteGroupCommitter` + 三个窄接缝：`ISqliteBatchObserver`（每批 `(ops, committed)` + 保存点回滚次数）、`ISqliteBatchGate`（`ShouldFlush` 决定领队何时 flush；`BeforeCommit` 把"事务开着"变成确定性时点）、`ISqliteCommitFault`（注入整批 `COMMIT` 失败）。协议：领队等「批满」或「窗口到期」→ **一个** `BEGIN IMMEDIATE` → 每操作 `SAVEPOINT`/`RELEASE`/`ROLLBACK TO` → 一次 `COMMIT`；整批失败 → 批内所有操作返回该错误；批事务期间持有**连接互斥** |
| `src/infra/location/sqlite/sqlite_location_repository.{h,cpp}` | Options 新增 `group_commit`/`group_commit_max_wait_ms`/`group_commit_max_batch` + 三个接缝 + `metrics`；写路径拆成 `...Locked`（逐操作，与接线前逐字一致）与 `...InTransaction`（批内，无事务控制）；`StepUpsert` 抽出语句级 upsert（两条路径共用） |
| `src/infra/metadata/sqlite/sqlite_metadata_repository.{h,cpp}` | 同上；`InsertVersionRow`（语句级插入 + 扩展码按主码比较）、`CreateLocked/InTransaction`、`UpdateLocked/InTransaction`、`DeleteLocked/InTransaction` |
| `src/main/server_main.cpp` | 6 个键逐键读入（默认 = schema 默认 `true/5/64`）→ 两个 Options；注册 `fss_sqlite_{group_commits,ops}_total`；横幅新增 `sqlite commit :` 行（与 `journal_mode`/`synchronous` 同处） |
| `tests/unit/test_sqlite_group_commit.cpp` **（新增）** | 协调器级：每操作原子性（真实主键冲突 + 半成品回滚）、整批 COMMIT 失败（故障注入）、门控是 flush 时机的确定性来源 |
| `tests/integration/test_sqlite_group_commit.cpp` **（新增）** | 仓储级：摊销 == `ceil(N/B)`（位置 + 元数据）、`group_commit=false` 逐字回归、每操作原子性（真实 `(partition, file_source)` 唯一索引冲突）、`max_wait_ms` 两档、**读不脏** |
| `tests/integration/test_sqlite_{location,metadata}_repository.cpp` | 各自新增一条 `group_commit=false` 的**契约回归**（同一套 `port_contract.h` 在逐操作档再跑一遍） |
| `tests/integration/test_config_wiring.cpp` | C10.20 真实进程用例（横幅 + 指标 + 时延 416ms vs 14ms） |

### 14.3 实测命令与输出摘要

```
$ cmake --build build -j4
  → 0 error

$ ./build/bin/test_sqlite_group_commit_unit
  → All tests passed (34 assertions in 3 test cases)

$ ./build/bin/test_sqlite_group_commit
  → All tests passed (62 assertions in 6 test cases)

$ ./build/bin/test_config_wiring "★ C10.20*" -s        # 关键数字
  createMetadata latency(ms) = 416          # group_commit_max_wait_ms=400（默认 group_commit=true）
  fss_sqlite_ops_total{metadata} = 1；commits = 1
  createMetadata latency(ms) = 14           # 同窗口 + metadata.sqlite.group_commit=false
  fss_sqlite_ops_total{metadata} = 1；commits = 1
  → All tests passed (57 assertions in 1 test case)

$ ctest --test-dir build -j4
  → 100% tests passed, 0 tests failed out of 86        （84 → 86：+2 个测试二进制）

$ ctest --test-dir build -L phase3 --output-on-failure
  → 100% tests passed, 0 tests failed out of 10
$ ctest --test-dir build -L phase6 --output-on-failure
  → 100% tests passed, 0 tests failed out of 9
$ ctest --test-dir build -L phase10 --output-on-failure
  → 100% tests passed, 0 tests failed out of 6

$ ./scripts/verify_config_wiring.sh
  → 配置面接线：全部通过（54 条断言）
$ ./scripts/check_docs.sh --selftest
  → 自证：D1/D2/D4/D5 都能检出注入的错误（检查器有效）；D5 = 11 个阶段 / 148 条门槛
```

真实进程启动横幅（`--set` 默认值 + 非默认值两处都断言过）：

```
  sqlite tuning  : location busy_timeout=5000ms journal_mode=WAL（wal=true，max_write_concurrency=8，synchronous=NORMAL）| metadata busy_timeout=5000ms journal_mode=WAL（wal=true，synchronous=NORMAL）
  sqlite commit  : location group_commit=true max_wait_ms=5 max_batch=64（组提交：并发写一批一次 COMMIT；读可能多等 ≤ max_wait_ms） | metadata group_commit=true max_wait_ms=5 max_batch=64
```

### 14.4 R1 自证（4 个注入 → 对应用例失败 → 完整还原）

每个注入都**全量重建**（`cmake --build build -j4`）后再跑；还原后
`grep -rn "R1-INJECT" src/ tests/` **无输出（rc=1）**。

**① 让 `group_commit` 被忽略（恒 false）** —— 在两个仓储的 `Open()` 里强制
`options.group_commit = false;` → 判据 1（摊销）必须失败：

```
$ ./build/bin/test_sqlite_group_commit "★ C10.20 摊销：并发 N=8*"
tests/integration/test_sqlite_group_commit.cpp:206: FAILED:
  REQUIRE( harness.observer.BatchCount() == 2 )
with expansion:  8 == 2
tests/integration/test_sqlite_group_commit.cpp:217: FAILED:
  REQUIRE( harness2.observer.CommitCount() == 4 )
with expansion:  8 == 4
test cases:  1 | 1 failed   assertions: 10 | 8 passed | 2 failed
```

**② 忽略 `max_batch`（选「恒 1」）** —— 在协调器构造里强制
`options_.group_commit_max_batch = 1;`（说明：恒 1 表示"每个操作自己一批"，等价于
"上限被忽略、退化到无摊销"；恒 ∞ 会挂在门控上，见 ④，故选恒 1）→ 判据 1 必须失败：

```
$ ./build/bin/test_sqlite_group_commit "★ C10.20 摊销：并发 N=8*"
REQUIRE( harness2.observer.CommitCount() == 4 )   with expansion:  8 == 4
test cases:  1 | 1 failed   assertions: 10 | 8 passed | 2 failed
```

**③ 去掉每操作 `SAVEPOINT`（一个失败就整批回滚）** —— 这是**最重要**的一条：
`ExecuteBatch` 里不再 `SAVEPOINT`/`ROLLBACK TO`，改为"任一操作失败 → `ROLLBACK` 整批 +
poison 全部结果" → 判据 2 必须失败：

```
$ ./build/bin/test_sqlite_group_commit_unit
tests/unit/test_sqlite_group_commit.cpp:180: FAILED:
  REQUIRE( r1.ok() )        with expansion:  false
test cases:  3 |  2 passed | 1 failed   assertions: 25 | 24 passed | 1 failed

$ ./build/bin/test_sqlite_group_commit "★ C10.20 每操作原子性*"
tests/integration/test_sqlite_group_commit.cpp:313: FAILED:
  REQUIRE( ok_count.load() == 2 )   with expansion:  0 == 2
test cases:  1 | 1 failed   assertions: 3 | 2 passed | 1 failed
```

**④ 忽略 `max_wait_ms`（一直等到批满）** —— 把领队的 `wait_until(deadline)` 换成
无超时的 `wait(sealed)`。门控是 `NeverFlushGate`（批永远不满）⇒ **挂死**（如实记录）：

```
$ timeout 20 ./build/bin/test_sqlite_group_commit "★ C10.20 max_wait_ms*"
  → SIGTERM - Termination request signal
  test cases: 1 | 1 failed   assertions: 2 | 1 passed | 1 failed
$ echo $?
  → 124        # timeout 杀掉的证据（不是用例自己失败，是永远等不到窗口）
```

**还原后的实测**：

```
$ grep -rn "R1-INJECT" src/ tests/          # 无输出（rc=1）
$ cmake --build build -j4                   # 0 error
$ ./build/bin/test_sqlite_group_commit      # All tests passed (62 assertions in 6 test cases)
$ ./build/bin/test_sqlite_group_commit_unit # All tests passed (34 assertions in 3 test cases)
```

### 14.5 本切片自己踩到并修掉的两个问题（如实记录）

**① 实现缺陷：领队等窗口的循环没有 `break`（第一次跑直接挂死）。**
第一版写成

```cpp
while (!batch->sealed) {
  if (gate && gate->ShouldFlush(...)) break;
  batch_cv_.wait_until(lock, batch->deadline);   // ← 返回值没看，到期也不 break
}
```

`wait_until` 到期返回 `timeout` 后循环条件仍为真、deadline 已过 ⇒ **忙循环**。
症状：第一次跑 `test_sqlite_location_repository` 时 **600s 超时被 SIGTERM 杀掉**（不是断言失败，
是根本不返回）。修法：`if (batch_cv_.wait_until(...) == std::cv_status::timeout) break;`。
★ 教训：`wait_until` 的**返回值**是这类循环的唯一出口，必须显式处理（`AGENTS.md` §4.3）。

**② 测试缺陷：并发批里"操作之间的相互影响"让判据随调度翻转。**
第一版单测让操作 ① 插 `(1,'a')`、操作 ② 先插 `(2,'b')` 再插 `(1,'x')`（想制造"半成品 + 主键
冲突"）。但批内执行顺序 = **到达顺序**：② 先到时它的第二次插入**反而成功**、① 才失败。
压测 **200 次里 27 次** `REQUIRE(r1.ok())` 假失败：

```
$ for i in $(seq 1 200); do ./build/bin/test_sqlite_group_commit_unit; done
  → fails=27/200     # r1 意外失败：UNIQUE constraint failed: t.id
```

修法：让 ② **自己内部**必然失败（插 `(2,'b')` 后再插 `(2,'x')`，与谁先到无关）→ **200/200 通过**。
教训写进 `AGENTS.md` §4.3（"并发测试的失败必须由被测对象自己决定，不能依赖线程到达顺序"）。

### 14.6 未做 / 未验证（如实登记）

1. **`*.sqlite.max_write_concurrency` 仍未生效**：两个仓储都是"单连接 + 互斥"，
   实际写并发**恒为 1 ≤ 上限**，改它不改变行为。**本切片刻意不把它标成生效**（它仍留在
   `operations.md` §1.3.3）。下一步：连接池交付后才接通。
2. **无吞吐数字**：组提交只保证"多个操作的语句合进一个事务（一次 `COMMIT`）"，
   本切片**没有**做 R2 合规的独立进程 + 绑核吞吐测量 ⇒ **不给任何倍数或 req/s**。
3. **不改变耐久性等级**：仍是"每个事务一次 `COMMIT`"，`synchronous` 语义不变；
   **没有**新增崩溃风险，也没有做"断电下批提交是否安全"的实验（本环境无 root/不能 mount，
   与 ADR-008 §6 / P9 §13.5 的未验证项相同，维持登记）。
4. **真实进程侧只做到"窗口时延 + 指标"**：真实进程**无法**直接数"事务次数"
   （SQLite 不暴露），因此 amortization 的确定性判据在 **L2 用例**里（观察者/门控），
   真实进程侧只断言①横幅打印实际取值、②`/metrics` 两族存在且动过、③窗口时延差异。
   **不声称真实进程验证了 `== ceil(N/B)`**。
5. **`max_wait_ms` 与批大小的调参未做**：默认 `5ms/64` 直接取 schema 默认值；
   没有测"顺序单写"下窗口带来的额外时延是否可接受（只知其**上界** = 每操作一个窗口）。
6. **读的等待窗口只做了定性 + 有界断言**（`REQUIRE_FALSE(reader_done)` + 提交后取到新值）；
   没有测"读在窗口下的 p99 时延分布"（需 R2 合规测量）。

### 14.7 三态计数（本切片收尾）

6 个键从「已读但无效果」移入「生效」：**生效 107 → 113**、**已读但无效果 31 → 25**
（**113 + 18 + 25 = 156**，`test_operations_doc` 机械断言）。
**未新增/删除任何配置键**（`git status --porcelain config/` 为空）。

### 14.8 父代理独立复核：两条**数据正确性**语义我自己做了消融

本切片的摊销判据（提交次数 == ⌈N/B⌉）只是性能；**真正要紧的是两条语义**——「每操作原子性」与
「读不到未提交数据」。我按 R1 各自做了一次注入（全量重建后运行，随后完整还原）：

**① 去掉每操作的 `ROLLBACK TO SAVEPOINT`**（失败操作不再单独回滚）：

```
$ ./build/bin/test_sqlite_group_commit          # 集成
tests/integration/test_sqlite_group_commit.cpp:334: FAILED
test cases: 6 | 5 passed | 1 failed      assertions: 62 | 61 passed | 1 failed
$ ./build/bin/test_sqlite_group_commit_unit     # 单测
tests/unit/test_sqlite_group_commit.cpp:199: FAILED
test cases: 3 | 2 passed | 1 failed      assertions: 26 | 25 passed | 1 failed
# 还原后：62 断言 / 6 用例 与 31 断言 / 3 用例 全绿
```
⇒ **"失败操作只回滚自己、同批其它照常提交"是被真正检查的性质**，不是靠"整批一起提交"顺带成立。

**② 批事务期间不持连接互斥**（`defer_lock`，读不再排队）：

```
tests/integration/test_sqlite_group_commit.cpp:415: FAILED:
  REQUIRE_FALSE( reader_done.load() )
（另在 192 行有 3 条同族失败）
test cases: 6 | 4 passed | 2 failed      assertions: 47 | 42 passed | 5 failed
# 还原后：All tests passed (62 assertions in 6 test cases)
```
⇒ **"读会等到批结束、因此读不到未提交数据"是被真正检查的**；一旦去掉那把锁，读者立刻拿到
批内未提交（随后可能被回滚）的数据 —— 用例立刻报错。这正是我在规格里要求"读与批互斥 + 把代价
写进文档"的原因。

**③ 独立复跑**：`cmake --build build -j4` 0 error；`ctest` **86/86**（84 → 86）；
`./build/bin/test_sqlite_group_commit`（62/6）与 `test_sqlite_group_commit_unit`（31/3）全绿；
`config/` 零改动；`grep -rn R1-INJECT src/ tests/` 无输出。

**④ 说明**：`== ceil(N/B)` 的确定性判据在 L2（观察者 + 门控），真实进程侧只断言横幅取值、
两族指标存在且动过、以及窗口时延差异（416ms vs 14ms）—— 这一点实现者已如实登记，我认可：
SQLite 不对外暴露"事务次数"，真实进程侧无法直接数。**没有**任何吞吐数字被声明。

---

## 15. A1（本轮）：ADR-009 §10 的 PostgreSQL 仓储落地 —— L2 位置仓储 + 在途租约

> 依据：`docs/adr/ADR-009-multi-instance-consistency.md` §4.1/§4.2/§4.3/§6.4 与 §10 待办第 1 项；
> schema 是 `db/migrations/001_init.sql`（**已应用**，本切片**不改** schema）。
> 本切片**只做 L2**：新增两个 PG 仓储 + libpq 薄封装 + 契约测试；**组合根未接线**
> （R12：具体实现只能在 `src/main/` 创建，那个改动留给后续切片）。
> 与 ADR-009 有关的既有证据：端口与契约基座在 §（`phase2.md`），GC 租约的单实例侧在
> `phase6.md` §7；本文件记录**共享 PG 侧**的落地。

### 15.1 结论（先说答案）

1. **两个 PG 仓储真的存在且跑同一套契约**：`PostgresLocationRepository`
   （`file_locations`）与 `PostgresLeaseRepository`（`staging_leases`）
   在**真实 PG 14.24 与 12.6** 上都跑 `tests/framework/port_contract.h` 的
   `CheckLocationRepositoryContract` / `CheckLeaseContract` —— 不是"各写一套测试"。
2. **实测计数**：`test_postgres_repositories` = **8 用例 / 276 断言**，两个引擎**完全一致**；
   `ctest -L pg` = **6/6**（原来 5 个基建测试 + 本切片 1 个）；默认 `ctest` **86/86 无回归**。
3. **SQL 只用 ≤12 语法**：没有扩展/`gen_random_uuid`、没有 `INCLUDE` 索引、没有
   `NULLS NOT DISTINCT`、没有 `MERGE`、没有 PG-14 专属 GUC；同一份二进制直接指向
   PG 12.6（`FSS_PG_DSN=postgresql://fss@172.17.64.1:5555/fss`）全绿。
4. **`ClaimExpired` 的原子性有"与线程顺序无关"的判据**：另一条**独立连接**把过期行
   `FOR UPDATE` 锁住后再调用 → 正确实现（CTE + `SKIP LOCKED`）**立即返回空**；
   ROLLBACK 后同一条路径必须能把 3 条全领到（正控）。去掉 `SKIP LOCKED` 的注入 →
   用例失败在 `statement_timeout`（57014，§15.4 ①）。
5. **fail-closed**：DSN 连不上 → `kUnavailable` + libpq 消息；**不存在**任何到
   SQLite/内存的静默回退（ADR-009 §4.1 的明确禁止）。测试连不上时**大声失败**，不 skip。
6. **数据库时钟**：`PgConnection::NowEpochMillis()` 执行
   `SELECT (extract(epoch from now()) * 1000)::bigint`（ADR-009 §6.4：租约/过期判定
   只能用数据库 `now()`）；`expires_at` / `renewed_at` 全部由 SQL 侧 `now()` 计算。
7. **诚实边界**：`PostgresMetadataRepository` **未交付**；组合根**未接线**（因此
   `location.postgres.*`、`metadata.postgres.*`、`leases.*` 仍是「已读但无效果」，
   `deployment.mode=multi` 仍**拒绝启动**）；leader election、多实例 E2E、NFS 语义
   验证均**未做**（§15.6 逐条列出）。

### 15.2 实现点（可点击）

| 文件 | 内容 |
| --- | --- |
| `src/infra/postgres/pg_connection.{h,cpp}` **（新增）** | libpq 薄封装：RAII `PgResult`（行数/列名→下标/NULL/按名按位取值/受影响行数/错误消息+SQLSTATE）、RAII `PgConnection`（`PQconnectdb` + 新连接即 `SET statement_timeout`）、**有界连接池** `PgPool`（活跃数 ≤ `max_connections`；坏连接丢弃不借出；借空则条件变量等待）、`NowEpochMillis()`（数据库时钟）、`MapPgError`（`23505`→`kLocationAlreadyExists`、`57014`/`08xxx`→`kUnavailable`、其余→`kInternal`；Error 带 `sqlstate` detail） |
| `src/infra/location/postgres/postgres_location_repository.{h,cpp}` **（新增）** | `IFileLocationRepository` 的 PG 实现（`file_locations`）：`Save` 用 `ON CONFLICT (partition_id, file_id) DO UPDATE` 保留 upsert 语义；`UpdateSignedUrl` 用一条 `jsonb_set(jsonb_set(data,...))` 原子改 `signed_url`/`updated_at_epoch_seconds`（未知字段与 `created_at` 不动）；`List` 的过滤/排序/分页/`total` 与 SQLite 逐条对齐；`data` JSONB 无损往返未知字段 |
| `src/infra/location/postgres/postgres_lease_repository.{h,cpp}` **（新增）** | `ILeaseRepository` 的 PG 实现（`staging_leases`）：`Acquire` 的 `ON CONFLICT ... WHERE expires_at <= now()` 原子接管/占用；`Renew`/`Release` 用 `owner = $3` 做归属校验，未命中再区分 `kNotFound`/`kPermissionDenied`；`ClaimExpired` 一条 CTE：`FOR UPDATE SKIP LOCKED` 挑选 → 改 owner、`expires_at = now() + 60s` → `LEFT JOIN file_locations` 反解 `Lease::file_id`（无位置记录留空） |
| `src/infra/location/memory/memory_lease_repository.cpp` | `Renew` 的"非本人"从 `kNotFound` 改为 `kPermissionDenied`（与 `Release` 一致；端口语义要求"不存在"与"不是你的"可区分，R16） |
| `src/CMakeLists.txt` | `find_package(PostgreSQL QUIET)` + `FSS_HAVE_LIBPQ`（**`CACHE INTERNAL`**，子目录顺序无关）；命中时建 `fss_pg`（`PUBLIC FSS_HAVE_LIBPQ=1`）与 `fss_location_postgres`；未命中**什么都不建**（默认构建在无 PG 机器上依旧成立） |
| `tests/CMakeLists.txt` | `FSS_WITH_PG=ON` **additionally require** `FSS_HAVE_LIBPQ`（配置期 fail-closed + 修复指令）；新增 `test_postgres_repositories`（`LABELS "pg;infra"` + `FIXTURES_REQUIRED pg`） |
| `tests/framework/port_contract.h` | 位置契约新增**可选** `partition_prefix`（默认值不变，既有调用点零改动）；新增租约契约 `CheckLeaseContract`（Acquire/Renew/Release/ClaimExpired 两侧 + 并发领取者不重不漏），带 `LeaseContractKey()` 处理内存/PG 的字段差异 |
| `tests/unit/test_port_contract_memory.cpp` | 用同一套租约契约跑 `InMemoryLeaseRepository`（+ 编译期 `is_base_of`/非抽象断言） |
| `tests/integration/test_postgres_repositories.cpp` **（新增）** | PG 侧：两套共享契约 + 显式跨租户隔离 + `file_id` 反解 + **锁住行再领**的确定性并发判据 + options 校验 / 连不上 fail-closed / 有界池与等待 / 数据库时钟 |

### 15.3 实测命令与输出摘要

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
#  0 error / 0 warning（新增目标单独重编亦无告警）
ctest --test-dir build --output-on-failure
#  100% tests passed, 0 tests failed out of 86     ← 无回归
./build/bin/test_port_contract_memory
#  All tests passed (472 assertions in 6 test cases)   ← 新增租约契约（原 5 用例）
./build/bin/test_sqlite_location_repository
#  All tests passed (275 assertions in 7 test cases)

cmake -S . -B build-pg -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFSS_WITH_PG=ON
cmake --build build-pg -j4 --target test_postgres_repositories
ctest --test-dir build-pg -L pg --output-on-failure
#  100% tests passed, 0 tests failed out of 6
#    pg_fixture_setup / pg_schema_invariants / pg_advisory_lock / pg_concurrent_claim
#    / test_postgres_repositories / pg_fixture_teardown
./build-pg/bin/test_postgres_repositories
#  All tests passed (276 assertions in 8 test cases)        [PG 14.24]
FSS_PG_DSN=postgresql://fss@172.17.64.1:5555/fss ./build-pg/bin/test_postgres_repositories
#  All tests passed (276 assertions in 8 test cases)        [PG 12.6]
FSS_PG_DSN=postgresql://fss@172.17.64.1:5555/fss ctest --test-dir build-pg -L pg
#  100% tests passed, 0 tests failed out of 6               [远端 12.6，本地 fixture 仍指向本机 14]
```

> `pg_fixture_setup` 在本机启动/复用 dev PG 14.24 并应用迁移；`FSS_PG_DSN` 只影响
> **新测试**连哪个库，**不会**对远端 12.6 执行迁移（远端 schema 早已就绪）。

### 15.4 R1 注入（每条都在完整重编后跑；末尾已全部还原）

**① `ClaimExpired` 去掉 `FOR UPDATE SKIP LOCKED`（领取退化为"先读后写"）**

```
tests/integration/test_postgres_repositories.cpp:365: FAILED:
  REQUIRE( claimed.ok() )
with expansion:
  false
with message:
  claimed.ok() ? std::string("ok") : claimed.error().message() :=
  "领取过期租约失败：ERROR:  canceling statement due to statement
  timeout
  CONTEXT:  while updating tuple (0,4) in relation "staging_leases""
test cases: 1 | 1 failed      assertions: 9 | 8 passed | 1 failed
```

⇒ 非原子实现在"行已被别人锁住"时**阻塞到 `statement_timeout`（57014）**，而不是跳过；
判据与线程到达顺序无关。还原后：`All tests passed (13 assertions in 1 test case)`。

**② 位置仓储 `Find` 去掉 `partition_id` 过滤**

```
tests/integration/test_postgres_repositories.cpp:254: FAILED:
  REQUIRE_FALSE( by_id.ok() )
with expansion:
  !true
test cases: 1 | 1 failed      assertions: 8 | 7 passed | 1 failed
```

⇒ 第三个租户用同样的 `file_id` **命中**了 A 的记录；同一用例里的正控
（A/B 各自 `Find` 成功）说明这条负断言不是恒真。还原后：
`All tests passed (14 assertions in 1 test case)`。

**③ `Renew` 去掉 owner 校验（恒真谓词 `AND (owner = $3 OR owner <> $3)`）**

```
tests/framework/port_contract.h:103: FAILED:
  REQUIRE_FALSE( r.ok() )
with expansion:
  !true
with messages:
  契约点（期望失败 kPermissionDenied）：非本人 Renew 必须 kPermissionDenied
test cases: 1 | 0 passed | 1 failed    assertions: 94 | 93 passed | 1 failed
```

⇒ 非本人续租**成功了**，契约点正确报红。还原后：
`All tests passed (98 assertions in 1 test case)`。

**④（追加）位置仓储 `Save` 去掉 `ON CONFLICT DO UPDATE`（幂等 upsert 失效）**

```
tests/framework/port_contract.h:84: FAILED:
  REQUIRE( r.ok() )
with messages:
  契约点：Save persistent（同 file_id 必须覆盖而不是报冲突）
  契约点：Save 更新
test cases: 1 | 0 passed | 1 failed    assertions: 109 | 107 passed | 2 failed
```

⇒ 同 `file_id` 的第二次 `Save` 变成唯一键冲突（不再覆盖）。还原后：
`All tests passed (276 assertions in 8 test cases)`。

**还原自证**：`grep -rn "R1-INJECT" src/ tests/` → 无输出；
`git status --short` 只列出本切片新增/修改的文件（见 §15.2）。

### 15.5 关于契约语义的三处判断（附理由）

1. **端口的 `file_id` 参数 = 表身份列 `file_source`（"租约键"）**：`staging_leases` 的
   主键是 `(partition_id, file_source)`（ADR-009 §4.3），表里没有 `file_id` 列。
   裁决：端口参数是"被写入的 staging 对象的 `file_source`"；`ClaimExpired` 再用
   `LEFT JOIN file_locations` 反解真正的 `file_id` 回填给 GC。**已写进仓储头文件**。
   当前**没有**任何生产调用方 `Acquire`（上传路径接租约是后续切片），因此落地不改变现网行为。
2. **内存实现的 `Renew` 非本人动作改为 `kPermissionDenied`**：旧版把"不存在"与"不是你的"
   都折叠成 `kNotFound`，与 `Release` 不一致、也让调用方无法排障（R16）。端口契约测试
   现在同时钉住两种情形。
3. **共享租约契约不直接断言 `Lease::file_id` / `file_source` 的取值**：内存实现把端口参数
   当 `file_id`（`file_source` 留空），PG 实现反之（`file_source` = 键、`file_id` 反解）。
   套件用 `LeaseContractKey()` 以"两个字段里非空的那个"识别租约，并**另有一条 PG 专属用例**
   验证 `file_id` 反解与"无位置记录 → 留空"。这是把差异显式登记，而不是让断言变弱到恒真。

另有一处**未引入**的新语义：`migrated_at`（PG 有、SQLite 没有）在 `zone=persistent` 时写
`updated_at_epoch_seconds`，读回**忽略**（端口没有对应领域字段；写进 `extra` 会破坏
未知字段的无损往返）。

### 15.6 仍未交付 / 未验证（如实登记）

| 项 | 状态 |
| --- | --- |
| `PostgresMetadataRepository` | **未交付**（ADR-009 §10 第 1 项的 metadata 部分仍未勾） |
| 组合根接线 `location.postgres.*` / `metadata.postgres.*` / `leases.*` | **未交付**：这些键仍逐字登记为**「已读但无效果」**（`docs/operations.md` §1.3.3）；`deployment.mode=multi` 仍**拒绝启动** |
| `CreateFileMetadata` 的原子领取 + `claiming`→`ready` 状态机 | **未交付** |
| leader election（PG advisory lock）+ GC 的 leader-only 调度 | **未交付** |
| 上传路径 `Acquire`/定期 `Renew` 租约 | **未交付**（当前无生产调用方） |
| 多实例端到端（C9.26：2 进程 + 共享 PG + 共享目录 + 崩溃注入） | **未验证** |
| PG 连接预算校验（C9.28：实例数 × 池上限 ≤ `max_connections`） | **未交付** |
| readiness 的 PG `SELECT 1` + 迁移版本校验 | **未交付** |
| 与 PG 的时钟偏移启动校验（ADR-009 §6.4 第二半） | **未交付** |
| NFS/SAN 的 `rename` 原子性、close-to-open、`syncfs` 语义（C9.27） | **未验证**（无目标存储） |
| `single_use_nonce` 入共享存储（M5） | **未交付**（默认 `false` 不变） |
| 真实远端 PG 的网络故障/断连恢复（重试/退避） | **未验证**：连接池只做"坏连接丢弃 + 重建"，**没有**重试策略 |

### 15.7 门槛结果

```
JOBS=4 ./scripts/run_all_gates.sh
#  失败: 无；⏱ 总耗时: 7 分 46 秒（466 s，阶段数 11）；✅ 全部已启用阶段门槛通过
#  （build 目录为默认配置：FSS_WITH_PG=OFF；ctest 86/86）

JOBS=4 FSS_GATES_WITH_PG=1 ./scripts/run_all_gates.sh
#  [infra] ctest -L pg → 100% tests passed, 0 tests failed out of 6
#  失败: 无；⏱ 总耗时: 7 分 51 秒（471 s，阶段数 11）；✅ 全部已启用阶段门槛通过

./scripts/check_docs.sh
#  全部检查通过（D1~D5）
grep -rn "R1-INJECT" src/ tests/    # 无输出
```

### 15.8 父代理独立复核（不采信子代理的自述，全部自己重跑）

| 复核项 | 我的命令 | 实测结果 |
| --- | --- | --- |
| PG 测试本体（本地 14.24） | `FSS_PG_DSN=postgresql://fss@127.0.0.1:15432/fss ./build-pg/bin/test_postgres_repositories` | **276 断言 / 8 用例**，全通过 |
| PG 测试本体（局域网 **12.6**） | `FSS_PG_DSN=postgresql://fss@172.17.64.1:5555/fss ./build-pg/bin/test_postgres_repositories` | **276 断言 / 8 用例**，两个引擎**逐项一致** |
| CTest 集成（fixture 启库 + 迁移 + 标签） | `ctest --test-dir build-pg -L pg --output-on-failure` | **6/6** |
| **fail-closed 不是 skip** | DSN 指向不可达端口 `127.0.0.1:15999` | `test cases: 8 \| 1 passed \| 7 failed`、`assertions: 18 \| 11 passed \| 7 failed`、**进程退出码 7** —— 连不上就大声失败；唯一通过的 1 个是不需要连库的 `PgOptions` 校验用例 |
| 数据卫生（可重跑、不污染共享库） | 两库各查 `pgtest-%` 在 `file_locations`/`staging_leases`/`file_metadata_records` 的残留 | 两库都是 **0/0/0** |
| 有界连接池（安全属性） | `./build-pg/bin/test_postgres_repositories "PgPool*"` | 14 断言通过；读代码确认负断言 `REQUIRE_FALSE(acquired)` **带正控**（归还后等待者必须 5 s 内拿到），且用 `started` 标志避免到达顺序影响判据 |
| 跨租户隔离 | `"...*partition 严格隔离*"` | 14 断言通过 |
| 门槛（我自己跑一遍） | `JOBS=4 FSS_GATES_WITH_PG=1 ./scripts/run_all_gates.sh` | `失败: 无`、**376 s / 11 阶段**、退出码 0 |
| 门槛（加入下面的护栏补丁后**重跑**） | 同上 | `失败: 无`、**436 s / 11 阶段**、退出码 0；`test_sql_guardrail` 断言数 9/2 → **17/3** |
| **无 libpq 的默认构建路径**（R11：可选依赖必须有无依赖的默认路径） | `cmake -S . -B build-nolibpq -DCMAKE_DISABLE_FIND_PACKAGE_PostgreSQL=ON`（默认选项）；再追加 `-DFSS_WITH_PG=ON` | 前者**配置成功（退出码 0）**且不编译任何 PG 源；后者**配置期 fail-closed（退出码 1）** 并打印可执行修复指令（"安装 libpq 开发文件，或去掉 `-DFSS_WITH_PG=ON`"） |
| **静态 SQL 护栏是否覆盖新 PG 文件**（我自己的消融） | 把 `postgres_lease_repository.cpp` 的 `kRelease` 去掉 `partition_id = $1 AND` → 跑 `test_sql_guardrail` | **失败**并点名 `tests/unit/test_sql_guardrail.cpp:138`；`md5` 逐字还原（`a4e35e94…`）后恢复全绿 |

#### 15.8.1 复核中发现并**修掉**的一个真问题：SQL 护栏有"换定界符即失明"的盲区

新代码里第一次出现 `R"pgsql(...)pgsql"`（数据库时钟那条语句，理由已写在
`src/infra/postgres/pg_connection.h` 文件头）。复核时意识到：C3.9 的扫描器
**只认 `R"sql(...)sql"`**，所以只要换个定界符，里面的 DML 就从检查范围里消失，
而"每条 DML 都必须带 `partition_id`"这条规则**不会因此失败** —— 检查静默退化成
"只覆盖一部分 SQL"。留一条"想绕过就绕过"的通道比不检查更危险（它给的是虚假的安全感）。

修法（`tests/unit/test_sql_guardrail.cpp`）：新增用例
**「★ C3.9 仓储实现文件不得用非 sql 定界符藏 SQL（护栏盲区补丁）」** ——
`src/**/*_repository.{h,cpp}` 里出现的每个原始字符串字面量都必须是 `R"sql(`；
并带**非空洞性断言**（真的扫到了仓储文件、也真的扫到了原始字符串）与**提取器自证**
（合成样本 `R"pgsql(SELECT 1)pgsql"` 必须被识别为 `pgsql` 定界符且行号正确）。

**注入自证（证明这条新规则不是恒真的，且盲区原先真实存在）**：往
`src/infra/location/postgres/postgres_location_repository.cpp` 追加一行

```cpp
constexpr const char* kBlindInjection = R"pgsql(DELETE FROM file_locations WHERE file_id = $1)pgsql";
```

| 跑的用例 | 结果 |
| --- | --- |
| 旧规则 `*不存在缺少*`（partition_id 扫描） | **仍然全绿**（2 断言 / 1 用例）⇒ 这条 `DELETE` 确实**逃过了**原有检查 |
| 新规则 `*非 sql 定界符*` | **失败**，并点名 `src/infra/location/postgres/postgres_location_repository.cpp:281` 与定界符 `pgsql`（退出码 1） |
| 还原后 | `md5` 逐字一致、`test_sql_guardrail` 全绿（17 断言 / 3 用例）、`grep -rn "R1-INJECT\|kBlindInjection" src/ tests/` 无输出 |

★ 该用例现在为**仓储实现文件**钉死了范围；`pg_connection.*` 里那条不访问任何表的
数据库时钟语句**不在**仓储文件里，因此不受此约束（它的定界符与理由仍然逐字写在
文件头，属于"显式登记"而非"悄悄绕过"）。

### 15.9 目标引擎 PG 12.6 的前置实测（父代理，本轮为 PG 落地新做）

> 生产 PG 是局域网 **12.6**（Windows，`server_encoding=UTF8`，
> `lc_collate=Chinese (Simplified)_China.936`）；开发用本地 **14.24**。
> 本轮在其上新建**专用角色 `fss` + 专用库 `fss`**（owner=`fss`，`trust` 免密），
> **未触碰**既有的 `fsserver`/`ndp*`/`permsvr` 等库与角色。

| 项 | 命令 | 结果 |
| --- | --- | --- |
| 迁移可用性 | `psql -f db/migrations/001_init.sql` | 12.6 上 **0 错误**（7 表 + 1 视图） |
| schema 不变量 | `psql -f db/tests/001_verify_invariants.sql` | 12.6 与 14.24 **各 9 条全通过**（I1~I9b，ROLLBACK 不污染） |
| 原子领取（真实 schema） | `db/tests/003_concurrent_claim.sh 10 8`（经临时 shim 指向 12.6） | **C1~C4 全绿**，含 C3 自证（去掉唯一索引后 **5/5** 轮出现多赢家） |
| 适配层依赖的 SQL 语义（8 项探针） | `build/pg_probe.sql`（`ON CONFLICT` 部分索引推断、版本链、`now()` 时钟、`ctid` + `FOR UPDATE SKIP LOCKED` 租约领取 + `LEFT JOIN` 反解 `file_id`、跨分区隔离、视图） | 两引擎**逐项一致**，全部符合预期（探针 P8 的"期望 0 行"是我自己注解写错：它插入了 1 条 staging 位置记录，**1 行才是对的**） |
| SQLSTATE（错误映射的依据） | `statement_timeout` / 唯一冲突 | 两引擎都是 **57014** / **23505**。★ 12.6 的报错是**本地化中文**（合法 UTF-8，如 `错误` = `e9 94 99 e8 af af`）⇒ 任何"匹配英文子串"的判定在目标库上会**静默失效**，必须按 SQLSTATE 映射（`MapPgError` 正是这么做的） |
| 会话级 advisory lock 的崩溃释放语义 | `build/pg_probe2.sh`（A 例空闲持锁、B 例持锁跑长语句，各 kill -9 客户端） | 见下表 |

| 持锁会话被杀时的状态 | PG 14.24 | PG **12.6** |
| --- | --- | --- |
| **空闲**（阻塞在客户端 socket 读） | 释放 **9 ms** | 释放 **51 ms** |
| **正在跑 15 s 单语句** | 释放 **633 ms**（14 能察觉客户端消失并取消） | **≥8 s 仍未释放**；语句结束后才释放（滞留窗口 = 当前语句剩余时长） |

★ **仓库自带的 `db/tests/002_advisory_lock.sh` 在 PG 12.6 上 A3 失败**（`✗ 持锁会话崩溃后锁仍被持有`）。
根因不是"12.6 不可用"，而是该脚本 `spawn_holder()` 的**注释与实现不符**：注释说
"循环短查询（leader 持锁的会话基本空闲，只做心跳）"，实现却是**一条 120 秒的
`DO $$ FOR i IN 1..600 LOOP PERFORM pg_sleep(0.2)` 单语句** —— PG 14 的客户端消失
侦测掩盖了这个不一致，12.6 如实暴露。

⇒ **对切片 B（leader election）的硬约束**（实测得出，不是推测）：
① leader 的**锁连接必须与数据连接分离**；② 锁连接**只跑短语句**（空闲态崩溃实测
51 ms 即可接管）；③ 每条语句带 `statement_timeout`，把"崩溃后锁滞留窗口"夹到
`statement_timeout_ms` 以内；④ ADR-009 §4.4 的"连接断开即释放"只在**空闲/短语句**
会话下成立，这句话需要按上面的实测**收窄措辞**。切片 B 同时会把
`002_advisory_lock.sh` 的 holder 改成真的"短语句心跳"（让它在两个引擎上都有判定力），
并保留 A3b 作为"为什么锁会话不能跑长语句"的对照。

---

## 16. A2（本轮）：ADR-009 §10 第 1 项的 metadata 一半 —— `PostgresMetadataRepository` + 原子领取判据

> 依据：`docs/adr/ADR-009-multi-instance-consistency.md` §4.2/§6.4 与 §10 待办第 1 项；
> schema 是 `db/migrations/001_init.sql` 的 `file_metadata_records`（**已应用**，本切片**不改** schema）。
> 本切片**只做 L2 + 测试**：新增 PG 元数据仓储与 PG 侧契约/并发判据；**组合根未接线**。
> 这是 §15（A1）的续作：A1 落地了位置仓储 + 租约，本切片落地 metadata 那一半。

### 16.1 结论（先说答案）

1. **`PostgresMetadataRepository` 真的存在且跑同一套契约**：`file_metadata_records`
   的 L2 实现在真实 PG **14.24 与 12.6** 上都跑 `tests/framework/port_contract.h` 的
   `CheckMetadataRepositoryContract`（与内存/SQLite 实现**逐字同一份断言**）——
   不是"各写一套测试"（C2.10 的元数据侧 / C6.11）。
2. **实测计数（两个引擎完全一致）**：
   `test_postgres_repositories` = **508 断言 / 14 用例**（A1 时是 276/8）；
   其中元数据 6 个用例 = **112 + 16 + 17 + 13 + 21 + 53 = 232 断言**
   （共享契约 / 跨租户隔离 / 墓碑不可见 / JSONB 无损往返 / `created_at` 精确往返 + 含端点区间 / 原子领取）。
   默认树 `ctest` **86/86 无回归**；`ctest -L pg` **6/6**（两个引擎）。
3. **原子领取（ADR-009 M2）有"与线程到达顺序无关"的判据**：8 个并发 `Create` 用
   **不同 id、同一 `(partition, file_source)`** → 库里**恰好 1 行**、**所有**调用者拿到
   同一条（同 id、`version == 1`）；正控是 8 个**不同 `file_source`** → 恰好 8 行、
   每个调用者拿到自己那条（证明"恰好 1 行"不是"并发调用全被丢掉"）。
   去掉 `ON CONFLICT` 的部分索引谓词 → 用例失败（§16.5 ①）。
4. **`created_at` 由注入的 `IClock` 产生**（与 SQLite 逐字一致），SQL 写
   `to_timestamp($n::bigint)`、读/过滤用 `floor(extract(epoch from created_at))::bigint`。
   实测往返**精确无漂移**（写 `1700000000` → 读回 `1700000000`，两引擎一致，§16.3）。
   ⚠️ 本条是对本轮早些时候一处**错误规格的纠正**：ADR-009 §6.4 的"一律用数据库 `now()`"
   适用于**租约/过期判定**（`staging_leases`，§15 的租约仓储），**不**适用于元数据记录的
   `created_at`；后者的契约用 `ManualClock` 钉住，组合根将来注入真实时钟。
5. **SQL 只用 ≤12 语法**：无扩展、无 `INCLUDE`、无 `MERGE`、无 `NULLS NOT DISTINCT`、
   无 PG-14 专属 GUC；同一份二进制直接指向 PG 12.6 全绿。
6. **fail-closed**：DSN 连不上 → `kUnavailable` + libpq 消息；**不存在**到 SQLite/内存的
   静默回退。测试连不上时**大声失败**，不 skip。
7. **诚实边界**：`state='deleted'` 的**软删除**语义与 `claiming`→`ready` 状态机
   **未实现**（§16.6）；组合根**未接线**（因此 `metadata.postgres.*` 仍是
   **「已读但无效果」**，`deployment.mode=multi` 仍**拒绝启动**）。

### 16.2 实现点（可点击）

| 文件 | 内容 |
| --- | --- |
| `src/infra/metadata/postgres/postgres_metadata_repository.{h,cpp}` **（新增）** | `IMetadataRepository` 的 PG 实现：`Create` 用单条 `INSERT … ON CONFLICT (partition_id, file_source) WHERE state <> 'deleted' AND is_latest DO NOTHING RETURNING id, version` 做**原子领取**（0 行 = 别人赢了竞态 → 按 `file_source` 回读返回那条；绝不把唯一冲突当 500）；`Update` 在**同一借出连接**上 `BEGIN` → 清旧 latest → 插 `version+1` → `COMMIT`（任何一步失败都 `ROLLBACK`，不留"半个操作"）；`GetById`/`GetLatestByFileSource`/`List` 一律 `is_latest AND state <> 'deleted'`；`Delete` 硬删全部版本；`List` 下推 `kind` / `name_prefix` / 含端点时间区间 + 稳定全序 + `total`；`VersionCount` 诊断访问器；`pool()` 诊断访问器 |
| `src/CMakeLists.txt` | `if(FSS_HAVE_LIBPQ)` 内新增 `fss_metadata_postgres`（链接 `fss_domain fss_json fss_pg PostgreSQL::PostgreSQL`，与 `fss_metadata_sqlite` 同构）；无 libpq 的机器仍然**什么都不建** |
| `tests/CMakeLists.txt` | `test_postgres_repositories` 链接 `fss_metadata_postgres` |
| `tests/framework/port_contract.h` | `CheckMetadataRepositoryContract` 新增**可选** `partition_prefix`（默认 `"contract-part"`，既有调用点零改动）；内部唯一的硬编码 `"contract-part-x"` 改成 `partition_prefix + "-x"`（默认值下**逐字相同**） |
| `tests/integration/test_postgres_repositories.cpp` | 新增 6 个元数据用例（共享契约 + 5 个 PG 专属）；`CleanupPrefix` 增加 `file_metadata_records` 清理；新增 `ScalarQuery` / `RawInsertMetadata` 辅助 |
| `docs/adr/ADR-009-multi-instance-consistency.md` §10 | 勾选 `PostgresMetadataRepository`（`CreateFileMetadata` 原子领取 / `claiming`→`ready` / leader election 等**仍未勾**） |

### 16.3 实测命令与输出摘要

```bash
cmake --build build -j4            # 默认树（FSS_WITH_PG=OFF）：rc=0
ctest --test-dir build --output-on-failure
#  100% tests passed, 0 tests failed out of 86          ← 无回归
./build/bin/test_sql_guardrail
#  All tests passed (17 assertions in 3 test cases)     ← 新仓储文件通过 C3.9 两条规则

cmake -S . -B build-pg -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFSS_WITH_PG=ON
cmake --build build-pg -j4         # rc=0
ctest --test-dir build-pg -L pg --output-on-failure
#  100% tests passed, 0 tests failed out of 6            [PG 14.24]
FSS_PG_DSN=postgresql://fss@172.17.64.1:5555/fss ctest --test-dir build-pg -L pg
#  100% tests passed, 0 tests failed out of 6            [PG 12.6]

./build-pg/bin/test_postgres_repositories
#  All tests passed (508 assertions in 14 test cases)    [PG 14.24]
FSS_PG_DSN=postgresql://fss@172.17.64.1:5555/fss ./build-pg/bin/test_postgres_repositories
#  All tests passed (508 assertions in 14 test cases)    [PG 12.6，逐项一致]
```

元数据 6 个用例的逐条断言数（**两个引擎完全相同**）：

```text
*PostgresMetadataRepository*  -> 112 assertions in 1 test case   （共享契约，含 List 时间区间）
*PG 元数据仓储：partition*    ->  16 assertions in 1 test case   （跨租户严格隔离）
*墓碑*                        ->  17 assertions in 1 test case   （state='deleted' 不可见 + 正控）
*JSONB*                       ->  13 assertions in 1 test case   （未知字段无损往返）
*created_at 由注入时钟*       ->  21 assertions in 1 test case   （精确往返 + 含端点区间）
*并发 Create*                 ->  53 assertions in 1 test case   （原子领取 + 正控）
```

**`created_at` 往返（两引擎逐字一致）**：写 `1700000001 / 1700000002 / 1700000003`
→ 独立连接回读 `floor(extract(epoch from created_at))::bigint` 得到 `1700000001 / 1700000002 / 1700000003`；
区间过滤 `after = 1700000002` → 2 条、`before = 1700000002` → 2 条、
单点窗口 `[1700000002,1700000002]` → 恰好 1 条（`alphabet`）；窗口外 `[1700000010,1700000020]` → 0 条（正控）。

**数据卫生（`pgtest-%` 残留，两引擎都是 0）**：

```text
PG 14.24: loc=0  lease=0  meta=0
PG 12.6 : loc=0  lease=0  meta=0
```

> 注：开始本轮时 `build/CMakeCache.txt` 里 `FSS_WITH_PG=ON`（于是 `ctest --test-dir build`
> 是 92 个测试）。这与本文件 §15 与 AGENTS §0 记录的"默认树 = `FSS_WITH_PG=OFF`、86/86"
> 不一致，因此已用 `cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFSS_WITH_PG=OFF`
> 恢复默认，并在此状态下报告 86/86。**没有**削弱任何测试。

### 16.4 Schema 映射裁决（SQLite ↔ PG；写进仓储头文件，不重新设计）

| SQLite `metadata` | PG `file_metadata_records` | 本实现 |
| --- | --- | --- |
| `version` | `version` | `Create` → `1`；`Update` → 现有 latest `+1`；读回时**列覆盖** JSON 里的 `version` |
| `is_latest` | `is_latest` | `Update` 先清旧 latest，再插新版本为 `TRUE`（`ux_mr_latest` 保证每 `(partition,id)` 一个 latest） |
| `previous_version` | **无列** | 不建列、不加迁移：领域侧 `ancestry` 在记录 JSON 里（`ToJson` 输出 `"ancestry"`），靠 `data` 列无损往返 |
| `name` | **无列** | 不建列：`name_prefix` 过滤 `data -> 'data' ->> 'Name'` |
| `created_at INTEGER` | `created_at TIMESTAMPTZ` | 写注入时钟 epoch 秒（`to_timestamp($n::bigint)`）；读/过滤 `floor(extract(epoch from created_at))::bigint`（**floor 截断**而非四舍五入，否则 ±1 s 漂移会破坏含端点区间） |
| `created_by` | `created_by` | 两边都写 `""`（PG 列 NOT NULL 且无默认值） |
| `data TEXT` | `data JSONB` | 存 `json::Dump(domain::ToJson(record))` —— 与 SQLite 是**同一份完整信封** |
| （无） | `state` | 所有读取过滤 `state <> 'deleted'`；`Create`/`Update` 写 `'ready'`；`Delete` **硬删全部版本**（含外部 tombstone，否则同 id 重新 `Create` 会撞主键） |
| （无） | `acl_viewers`/`acl_owners`/`legal_tags` | 写入时从 `record.acl.viewers` / `record.acl.owners` / `record.legal.legaltags` 填（**忠实的反规范化镜像**，不再永久为空）；**读取只用 `data` JSON**（单一事实来源） |

### 16.5 R1 注入（每条都在完整重编 `cmake --build build-pg -j4 --target test_postgres_repositories` 之后跑；末尾已全部还原）

**① 去掉 `ON CONFLICT` 的部分索引谓词（原子领取退化为"无仲裁者"）**

```
tests/integration/test_postgres_repositories.cpp:713: FAILED:
  REQUIRE( results.errors[i].empty() )
with expansion:
  false
with message:
  caller 0 : 插入元数据失败：ERROR:  there is no unique or exclusion
  constraint matching the ON CONFLICT specification
test cases: 1 | 1 failed      assertions: 2 | 1 passed | 1 failed
```

⇒ 部分唯一索引 `ux_mr_source` 的谓词必须与 `db/migrations/001_init.sql` **逐字一致**
（PostgreSQL 才能推断出该索引；两引擎实测都报 42P10，12.6 上是本地化中文）。
还原后：`All tests passed (508 assertions in 14 test cases)`。

**② 停止清旧 `is_latest`（版本链断裂；`kClearLatest` 的 `SET` 改成 `is_latest = TRUE`）**

```
tests/framework/port_contract.h:93: FAILED:
  REQUIRE( r.ok() )
with expansion:
  false
with messages:
  ...
  契约点：Update → v2
test cases:  1 |  0 passed | 1 failed
assertions: 87 | 85 passed | 2 failed
```

⇒ `Update` 命中 `ux_mr_latest` 唯一冲突，`is_latest` 没有从旧版移到新版。还原后：
`All tests passed (508 assertions in 14 test cases)`。

**③ `GetById` 丢掉 `state <> 'deleted'`（墓碑重新可见）**

```
tests/integration/test_postgres_repositories.cpp:509: FAILED:
  REQUIRE_FALSE( by_id.ok() )
with expansion:
  !true
test cases:  1 | 1 failed      assertions: 10 | 9 passed | 1 failed
```

⇒ 同用例里的**正控**（同一 `GetById` 路径对 live 行命中、`List` 里 live 行可见）说明这条
负断言不是恒真。还原后：`All tests passed (508 assertions in 14 test cases)`。

**还原自证**：`md5sum -c`（三个文件）全部 `OK`；
`grep -rn "R1-INJECT" src/ tests/` → 无输出；
还原后完整重编 + 全量 PG 测试 = `All tests passed (508 assertions in 14 test cases)`。

### 16.6 仍未交付 / 未验证（如实登记）

| 项 | 状态 |
| --- | --- |
| 组合根接线 `metadata.postgres.*` | **未交付**：`metadata.postgres.*` 仍逐字登记为**「已读但无效果」**（`docs/operations.md` §1.3.3）；组合根仍注入 SQLite/内存元数据仓储 |
| `CreateFileMetadata` 的**跨步骤**原子领取 | **未交付**：仓储只提供"单条 INSERT 的原子领取" |
| `state='deleted'` 软删除 + `claiming`→`ready` 状态机 + 崩溃回收 | **未交付**（ADR-009 §10 第 2 项）；本仓储只写 `'ready'`、只按 `state <> 'deleted'` 过滤 |
| `deployment.mode=multi` 的 5 条启动校验 | **未交付**：`multi` 仍**拒绝启动** |
| leader election（PG advisory lock）+ GC leader-only | **未交付** |
| 多实例端到端（C9.26：2 进程 + 共享 PG + 共享目录 + 崩溃注入） | **未验证** |
| PG 连接预算校验（C9.28） | **未交付**（仓储的 `PgPool` 有 `max_connections` 上限，但没有"实例数 × 池上限 ≤ 服务端上限"的启动校验） |
| readiness 的 PG `SELECT 1` + 迁移版本校验 / 时钟偏移校验 | **未交付** |
| NFS/SAN 的 `rename` 原子性、close-to-open、`syncfs` 语义（C9.27） | **未验证**（无目标存储） |
| 并发 `Update` 同一 id | **未验证/未序列化**：没有跨实例互斥，第二个 `Update` 会因 `ux_mr_latest` 冲突报错（不重试、不合并）；契约只要求串行语义 |
| `List` 的 `name_prefix` 下推 | 用 `left(data -> 'data' ->> 'Name', length($n)) = $n`（**字面前缀**，与 SQLite 的 C++ `rfind(prefix,0)==0` 等价；`%`/`_` 不是通配符）；**没有**为它建表达式索引（分区内记录数本阶段可控） |
| 完整构建日志中的告警 | 新增/改动的 3 个文件 **0 警告**；完整 `build-pg` 重建日志里仍有 **24 条既有告警**（`tests/framework/app_fixture.h`、`tests/framework/raw_http.h`、`tests/framework/mock_s3.h`、`tests/integration/test_metadata_lifecycle.cpp`、`tests/integration/test_upload_flow_s3.cpp` 的 `-Wmissing-field-initializers`/`-Wunused-parameter`/`-Wunused-result`），**不是**本切片引入 |

### 16.7 门槛结果

```
./scripts/check_docs.sh
#  全部检查通过（D1~D5）；D5：11 个阶段、148 条门槛

JOBS=4 FSS_GATES_WITH_PG=1 ./scripts/run_all_gates.sh
#  ==> [infra] ctest -L pg → 100% tests passed, 0 tests failed out of 6
#  失败: 无
#  ⏱  总耗时: 7 分 5 秒（425 s，阶段数 11）
#  ✅ 全部已启用阶段门槛通过。（退出码 0）

md5sum -c /tmp/r1_baseline.md5    # 三个文件全部 OK（R1 注入逐字还原）
grep -rn "R1-INJECT" src/ tests/  # 无输出
```

> 门槛脚本（`FSS_GATES_WITH_PG=1`）会把 `build` 缓存改成 `FSS_WITH_PG=ON`；跑完后已再次
> `cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFSS_WITH_PG=OFF` 并重跑
> `ctest --test-dir build` = **86/86**，把默认树恢复到本文件 §15 / AGENTS §0 记录的形态。

### 16.8 父代理独立复核（不采信子代理自述，全部自己重跑）

#### 16.8.1 派发前：先在两台引擎上验证映射裁决（这一次让一处**错误规格**在写代码前被打掉）

本轮 spec 初稿把元数据 `created_at` 定为"用数据库 `now()`"。派发后复核契约发现这是**错的**：
`tests/framework/port_contract.h` 的签名是
`CheckMetadataRepositoryContract(domain::IMetadataRepository& repo, fss::ManualClock& clock)`，
测试用 `ManualClock clock{1700000000}` + `clock.AdvanceSeconds(1)` 驱动时间，并断言
`created_after = t0 + 1` **恰好 2 条**、单点窗口 `[t0+1, t0+1]` **恰好 1 条**。
若写 `now()`，存进去的是真实墙钟（≈1789820936），契约在 1700000000 附近过滤 ⇒
**所有时间区间断言必然失败**。已即时把纠正发给子代理：注入 `IClock`、写
`to_timestamp($n::bigint)`；并撤掉我原先要求的"epoch 接近数据库时钟"断言。

纠正后的机制**由我在两台引擎上独立预验证**（`build/pg_epoch_probe.sql`，全部 ROLLBACK）：

| 项 | PG 14.24 | PG 12.6 |
| --- | --- | --- |
| `to_timestamp` 往返 `1700000001 / 1700000010 / 0` | 逐值相等，**drift = 0** | 同左 |
| 真实写入 → 独立读回 `floor(extract(epoch ...))::bigint` | `1700000001`，drift = 0 | 同左 |
| 下界**含端点**（`>=` 1 条 / `>` 0 条） | ✅ | ✅ |

同一次预验证还覆盖了本切片其余全部映射裁决（`build/pg_meta_probe.sql`，两引擎**逐项一致**）：
`name_prefix` 走 JSONB `data -> 'data' ->> 'Name'`（命中 1 / 未命中 0）、
`floor(extract(epoch ...))` 截断、时间区间含端点、`state='deleted'` 行不可见**且同查询正控可见**、
软删除后同一 `file_source` 可重新领取（返回 1 行新记录）、版本链"清旧 latest → 插 v2"后
同 id 只有 1 条 latest、`total` = 过滤后分页前 + 稳定排序分页。两库 `probe-m%` / `probe-e%`
残留均为 **0**。

#### 16.8.2 落地后的复核

| 复核项 | 我的命令 | 实测 |
| --- | --- | --- |
| 默认树无回归 | `ctest --test-dir build`（缓存确认 `FSS_WITH_PG:BOOL=OFF`） | **86/86**、无失败 |
| PG 测试本体（本地 14.24） | `FSS_PG_DSN=postgresql://fss@127.0.0.1:15432/fss ./build-pg/bin/test_postgres_repositories` | **508 断言 / 14 用例** |
| PG 测试本体（目标 **12.6**） | 同上，DSN 指 `172.17.64.1:5555` | **508 断言 / 14 用例**（与 14.24 逐项一致） |
| fail-closed 不是 skip | DSN 指不可达端口 `127.0.0.1:15999` | `test cases: 14 \| 1 passed \| 13 failed`、**退出码 13** |
| 数据卫生 | 两库 `pgtest-%` 在 `file_locations`/`staging_leases`/`file_metadata_records` 的残留 | 两库 **0/0/0** |
| 静态护栏覆盖新文件 | `./build/bin/test_sql_guardrail` | 17 断言 / 3 用例通过（新仓储用 `R"sql(` 且 DML 都带 `partition_id`） |
| 代码复核 | 读 `postgres_metadata_repository.cpp` 全部 SQL 与 `Create`/`Update`/`Delete`/`List` | 读路径一律 `is_latest AND state <> 'deleted'`；`Update` 用**同一借出连接** `BEGIN…COMMIT`、统一失败出口 `ROLLBACK`、且"清 latest 命中 0 行"显式报 `kInternal`（R9）；`Delete` 硬删全部版本并查 `AffectedRows`；`RowToRecord` 以**列**覆盖 JSON 里的 `version` |
| 测试是否有区分力（读用例本体） | 墓碑用例、并发领取用例 | 墓碑：三条负断言各自**同路径正控**（`GetById(live)` 在前后各断言一次）、并在 `List` 上先证明 live 行 `total == 1`；并发领取：`std::atomic` 起跑栅栏最大化真实竞态、结果按线程分开收集、另有"8 个不同 `file_source` → 恰好 8 行"的正控 |
| 门槛（我自己跑） | `JOBS=4 FSS_GATES_WITH_PG=1 ./scripts/run_all_gates.sh` | `失败: 无`、**461 s / 11 阶段**、退出码 0；随后把 `build` 缓存恢复 `FSS_WITH_PG=OFF` 并重跑 `ctest --test-dir build` = **86/86** |

#### 16.8.3 我自己的消融（子代理没做过的那一条）

把 `created_at` 的写入整体前移 100000 秒（`to_timestamp($4::bigint)` →
`to_timestamp($4::bigint - 100000)`；SQL 仍合法、参数个数不变，因此失败只能来自**语义**），
完整重编后跑元数据用例：

```
tests/framework/port_contract.h:870: FAILED:  REQUIRE( after_page.total == 2 )
tests/integration/test_postgres_repositories.cpp:611: FAILED:
  REQUIRE( raw.value().Value(0, 1) == "1700000001" )
test cases: 5 | 3 passed | 2 failed ; assertions: 150 | 148 passed | 2 failed   [退出码 2]
```

⇒ 两条结论都被钉住：① `created_at` 确实来自**注入时钟**（我纠正后的规格）；② 契约的
时间窗口断言**有区分力**，不是恒真。还原后 `md5` 逐字一致、重编后 **508/14 全绿**。

> 另外复核确认：`grep -rn "R1-INJECT" src/ tests/` 无输出；`AGENTS.md`、
> `config/fss.example.json`、`docs/operations.md`、`src/main/server_main.cpp`、
> `db/migrations/001_init.sql` **均未被本切片触碰**（三态计数因此逐字不变）。


