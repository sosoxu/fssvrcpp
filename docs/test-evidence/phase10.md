# 阶段 10 测试证据（🚧 切片 1 完成：配置面接线）

| 项 | 值 |
| --- | --- |
| 阶段 | P10（配置面接线） |
| 状态 | 🚧 **切片 1（配置面接线）完成**：C10.1~C10.8 全部满足 |
| 门槛命令 | `ctest -L phase10 && scripts/verify_config_wiring.sh` |
| 退出码 | `0` |
| 新测试 | `tests/integration/test_config_wiring.cpp`：**10 个 TEST_CASE / 137 断言**（全部在真实 `build/bin/fss_server` 上） |
| 脚本 | `scripts/verify_config_wiring.sh`：**22 条断言**（配置生效 7 + 优先级 3 + print-config 5 + 生产拒绝 3 + 未知键 2 + io_uring 2） |
| 全阶段门槛 | `./scripts/run_all_gates.sh` → **P0~P10 全绿，总耗时 230 s（3 分 50 秒，11 个阶段）**；`ctest` **76/76** 通过 |
| sanitizer | `run_sanitizers.sh` 已自动纳入 `phase10`（`✓ phase10 在 sanitizer 下通过`） |
| 配置键计数 | `config/fss.example.json` **156** 个叶子键：**66 个已接通**、**90 个未接通**（逐键见 `docs/operations.md` §1.3/§1.4，计数由人工逐条核对：5+23+5+29+8+2+1+17+11+4+3+18+3+3+4+7+6+7 = 156） |

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
| `config/fss.example.json` 不能"零环境变量"直接启动 | 示例里 7 个密文键是 `${ENV:VAR}`；变量未注入时加载器把"引用未解析"计为问题 → **exit 78**（fail-closed）。这是加载器的既有语义，已在 `operations.md` §0 显式提示；启动示例配置前必须先注入这些 Secret |
