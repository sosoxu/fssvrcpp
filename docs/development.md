# 开发指南

> 目标：让任何人（和 CI）在一台**没有 root、外网受限**的机器上，把本项目构建起来、
> 跑通阶段门槛、并且能验证需要真实外部依赖的行为。

---

## 1. 环境事实（本项目实测，写在这里以免反复踩坑）

| 项 | 值 | 影响 |
| --- | --- | --- |
| OS / 编译器 | Ubuntu 22.04.5 / g++ 11.4.0 | C++20 可用；`clang++` / `ninja` **未安装** |
| 构建 | CMake 3.22.1 + GNU Make 4.3 | 不使用需要更新 CMake 的特性 |
| **root 权限** | ❌（`sudo` 需密码） | 依赖一律 `apt-get download` + `dpkg-deb -x`；不能 `mount`、不能装系统包 |
| 外网 | `github.com` 直连**不可达**；apt 镜像（USTC）可用 | 第三方库只能从 apt 或镜像 pool 获取 |
| gRPC / protobuf | 1.30.2 / **3.12.4** | ★ **proto3 `optional` 不可用**（需 3.15+），`.proto` 中禁止使用 |
| 其他 | OpenSSL 3.0.2、SQLite3 3.37.2、libcurl 7.81、zlib | 可用 |
| 缺失 | boost、aws-sdk-cpp、nlohmann-json（已 vendored） | 设计上不依赖 |

---

## 2. 构建与测试

```bash
# 配置 + 构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"

# 阶段 0 门槛（环境与契约可行性）
ctest --test-dir build -L phase0 --output-on-failure

# 全部已启用门槛（按顺序，失败即停）
./scripts/run_all_gates.sh
```

`third_party/` 缺失时的恢复方式见 [`third_party/README.md`](../third_party/README.md)。
要点：`cpp-httplib` **不要**用 Ubuntu 的二进制包（jammy 的 0.10.3 有 `Range` 下溢缺陷），
要从镜像 pool 取上游 0.26.0 的源码单头文件。

---

## 3. 开发/测试用 PostgreSQL（`scripts/dev_postgres.sh`）

### 3.1 为什么需要它

多实例一致性相关的门槛（C2.10 / C6.11~C6.13 / C8.9~C8.10 / C9.26）**必须有真实 PostgreSQL**：
唯一约束的冲突语义、`ON CONFLICT ... RETURNING` 的原子性、`pg_try_advisory_lock` 的会话锁、
以及用 PG 侧 `now()` 做时间基准——这些用 SQLite 或 mock 都验证不了（见
[ADR-009](adr/ADR-009-multi-instance-consistency.md)）。

它**无需 root**：二进制通过 `apt-get download` + `dpkg-deb -x` 解包到 `.devpg/`，
以普通用户 `initdb` / `pg_ctl` 运行。

> ⚠️ **仅供开发与测试**：使用 `trust` 认证、监听 `127.0.0.1`、关闭 `fsync`。
> 生产部署请使用托管 PG 或正规集群。掉电耐久性由 ADR-008 在真实存储上单独验证。

### 3.2 常用命令

```bash
./scripts/dev_postgres.sh start      # 启动（必要时自动解包二进制 + initdb + 应用迁移）
./scripts/dev_postgres.sh status     # 状态 + 连接信息
./scripts/dev_postgres.sh stop       # 停止（保留数据）
./scripts/dev_postgres.sh restart    # 重启（会重写开发配置，配置改动总能生效）
./scripts/dev_postgres.sh reload     # 仅重载配置
./scripts/dev_postgres.sh reset      # 删除数据目录后重新初始化 + 迁移（回到干净状态）
./scripts/dev_postgres.sh migrate    # 只应用未应用的迁移
./scripts/dev_postgres.sh psql       # 交互式 psql
./scripts/dev_postgres.sh psql -c "SELECT 1;"
./scripts/dev_postgres.sh dsn        # 打印 DSN
./scripts/dev_postgres.sh env        # 打印可 eval 的 export（含 PATH / LD_LIBRARY_PATH）
./scripts/dev_postgres.sh logs       # 看服务日志
./scripts/dev_postgres.sh destroy    # 停止并删除 .devpg/（含解包的二进制）
```

在脚本里使用：

```bash
eval "$(./scripts/dev_postgres.sh env)"   # 之后可直接用 psql / pg_ctl，且对 set -u 安全
psql -c "SELECT version();"
```

默认参数（均可用环境变量覆盖）：

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `FSS_PG_ROOT` | `<repo>/.devpg` | 数据与二进制根目录 |
| `FSS_PG_PORT` | `15432` | 避开系统默认 5432 |
| `FSS_PG_USER` / `FSS_PG_DB` | `fss` / `fss` | 超级用户 / 应用库 |
| `FSS_PG_BIN` | 自动发现 | 直接指定 `pg_ctl`/`initdb`/`psql` 所在目录 |
| `FSS_PG_VERSION` | `14` | 要解包的 apt 包版本 |

### 3.3 迁移

迁移文件在 `db/migrations/*.sql`，按文件名数字顺序应用，记录在 `schema_migrations`。
每条迁移必须**幂等**（`IF NOT EXISTS` / `OR REPLACE`），因为：

- 开发时会反复 `migrate`；
- **多实例的 `schema_version_check` 依赖版本号**（配置里 `metadata.postgres.schema_version_check: true`）。

新增迁移：创建 `db/migrations/00N_描述.sql`（`N` 递增），里面写幂等语句，然后：

```bash
./scripts/dev_postgres.sh migrate
./scripts/dev_postgres.sh psql -f db/tests/001_verify_invariants.sql
```

### 3.4 已就绪的数据库层验证

| 文件 | 内容 | 支撑门槛 |
| --- | --- | --- |
| `db/tests/001_verify_invariants.sql` | **schema 不变量自检**（I1~I9b）：幂等键唯一、`ON CONFLICT` 可推断、原子领取、`state` CHECK、版本链唯一、租约 GC 查询、排障视图、**索引键必须在 `file_source` 上且谓词含 `is_latest`** | C2.10 |
| `db/tests/002_advisory_lock.sh` | **跨会话 advisory lock**（A1~A4）：互斥、自然结束释放、**`kill -9` 后释放**、会话级 vs 事务级 | C8.9 / C9.26 |
| `scripts/check_io_uring.sh` | io_uring 可用性三态探测（宿主机 / 容器默认 seccomp / unconfined）→ ADR-010 的 U1 结论 |
| `db/tests/003_concurrent_claim.sh` | **真并发原子领取**（C1~C4）：多会话并发领取恰好 1 个赢家、重试幂等、**去掉唯一索引后必须出现多赢家（自证）**、软删除后可重领 | C6.11 / C9.26 |

### 3.5 通过 CTest 运行（含 fixture 管理生命周期）

PG 相关测试默认**不注册**（默认构建必须能在没有任何外部服务的机器上通过）。
显式开启：

```bash
cmake -S . -B build -DFSS_WITH_PG=ON
ctest --test-dir build -L pg --output-on-failure
```

CTest 用 fixture 管理 PG：

```
pg_fixture_setup  (FIXTURES_SETUP pg)      → scripts/dev_postgres.sh start
pg_schema_invariants      ┐
pg_advisory_lock          ├ FIXTURES_REQUIRED pg
pg_concurrent_claim       ┘
pg_fixture_teardown (FIXTURES_CLEANUP pg)  → 默认只回显；FSS_PG_TEARDOWN=ON 时停止服务
```

即：从**停止状态**直接 `ctest -L pg` 也能自动拉起来（实测约 30 秒）。

### 3.6 新增一个依赖 PG 的测试

```cmake
add_test(NAME my_pg_test COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/my_pg_test.sh)
set_tests_properties(my_pg_test PROPERTIES
  FIXTURES_REQUIRED pg          # 复用同一个 PG 实例
  LABELS "pg"
  TIMEOUT 300)
```

测试脚本里的约定：

1. 先自检 PG 可用，不可用就**明确报错**（不要静默跳过）：
   `"${PGCTL}" status >/dev/null 2>&1 || fail "..."`
2. 用 `eval "$("${PGCTL}" env)"` 导入环境；
3. **测试数据要用独立的 `partition_id` 前缀**（例如 `claimtest`），并在 `trap ... EXIT` 里清理；
4. 需要"证伪"的断言要配**对照实验**（例如 `003` 的 C3：去掉约束后必须能复现问题），
   否则"没发现问题"可能只是测试无效。

---

## 3b. Sanitizer 构建（ASan + UBSan + LSan）

```bash
./scripts/run_sanitizers.sh                 # 独立目录 build-asan；首次数分钟，之后增量
FSS_ASAN_JOBS=4 ./scripts/run_sanitizers.sh # 并行度（默认 4；不要用 nproc）
FSS_GATES_SKIP_SANITIZERS=1 ./scripts/run_all_gates.sh   # 临时跳过
```

**三条必须知道的约束**（都是实测踩出来的）：

| 约束 | 原因 |
| --- | --- |
| **并行度不要用 `nproc`** | 插桩后的 Catch2/httplib 单 TU 编译可达 1 GB+，16 核机器上 `-j16` 会把 `cc1plus` 送进 OOM killer（`Killed signal terminated program cc1plus`） |
| **链接 gRPC 的目标必须带 `-DGRPC_ASAN_SUPPRESSED=1`** | gRPC 的 `port_platform.h` 在 `__SANITIZE_ADDRESS__` 下定义 `GRPC_ASAN_ENABLED`，**改变 C++ 类布局**（实测 `sizeof(grpc::ClientContext)`：504 → 512）；而系统 `libgrpc++.so` 不带 ASan → 布局不一致 → 垃圾值/巨型 malloc。该项目定义已挂在 `PkgConfig::GRPCPP` 的 INTERFACE 上，链接 gRPC 即自动获得 |
| **大文件测试要缩小规模** | ASan 的影子内存/redzone 会抬高 RSS 基线；用 `FSS_TEST_BIG_BYTES=67108864`（脚本已设），1 GiB 的完整用例留在普通构建的门槛里 |

**判据**：`-fno-sanitize-recover=all`（UBSan 命中即 abort）+ LeakSanitizer（退出时报泄漏并使进程非零退出）。
目前**没有任何"按目标关检查"的例外** —— 曾经为绕过 gRPC 症状加过 `-fno-sanitize=bool`，找到根因后已撤掉。

## 4. 写测试的三条纪律（本项目已因此抓到过多个真实缺陷）

| 纪律 | 为什么 |
| --- | --- |
| **关键断言要配"自证"对照** | 否则无法区分"实现正确"与"测试无效"。示例：C1.2b（去掉前置拦截必须复现 40ms 停顿）、ADR-008 的协议 9 对照、`003` 的 C3 |
| **测量必须用独立进程 + 绑核** | 进程内压测会得出方向性错误结论（见 `docs/appendix/capacity-probe/README.md`） |
| **性能数字必须标注协议与安全性** | 本项目已发生过一次"用不安全协议的 58,741 文件/秒 作为设计依据"的错误（ADR-008 §1） |

---

## 5. 证据归档

每个阶段的门槛通过后，把**命令、输出摘要、结论、以及本阶段发现并修复的缺陷**
写入 `docs/test-evidence/phaseN.md`。已归档：`docs/test-evidence/phase0.md`。

附录（`docs/appendix/`）保存可重跑的探针与其原始输出：

| 目录 | 内容 |
| --- | --- |
| `httplib-hardening-probe/` | cpp-httplib 的两个缺陷（H-1/H-2）复现与修法 |
| `capacity-probe/` | 并发/容量测量与**方法学教训**（含保留的错误方法探针） |
| `coroutine-probe/` | 协程 vs 线程：阻塞/offload/真异步三方对比 |
| `posix-io-probe/` | POSIX I/O 延迟分布、io_uring vs 线程、写入三路线 |
| `group-commit-durability/` | 耐久性协议的顺序不变量检查与崩溃注入 |
| `multi-instance-probe/` | 多实例 5 个竞态的复现脚本与结果 |

---

## 6. 静态检查与已知陷阱

| 陷阱 | 说明 |
| --- | --- |
| `proto3 optional` | protobuf 3.12 **不支持**，用 message 包裹表达可选性 |
| NFS | 本机**没有** NFS 设备，但**可以在容器里搭出 NFSv4.2（Linux knfsd）实验室**：`scripts/check_nfs_semantics.sh` 在这种挂载上已实测全绿（详见 [lab-nfs-multiinstance.md](lab-nfs-multiinstance.md)）。⚠️ 这只证明 **knfsd 的协议语义**，**不能**替代目标环境（真实 NFS 设备 / 跨主机 / 断电）的复核 —— C9.27 仍是**上生产硬前提**（ADR-009 §6.1） |
| `kill -9` 与 advisory lock | PG 默认 `client_connection_check_interval=0` 时，**正在跑长查询**的后端不会察觉客户端已死，会话锁不会被释放。`dev_postgres.sh` 已设为 `1s`；设计上还要求"持锁会话保持空闲/只做心跳"（ADR-009 §4.4） |
| `syncfs` 的全局影响 | 它是**文件系统级**操作；多实例共盘时会互相干扰 → 建议按 partition 分盘（ADR-008 §3.2 / ADR-009 §6.3） |
| `bash` 后台任务取 PID | 不能用 `PID="$(spawn)"`（命令替换会创建子 shell，任务成孤儿，`wait`/`kill` 都失效）。用全局变量 |
| **io_uring 被容器 seccomp 阻断** | Docker 自 2023 起默认 profile 屏蔽 `io_uring_*`（实测 `EPERM`；`seccomp=unconfined` 才通）。因此 I/O 引擎默认是**阻塞线程池**；启用 io_uring 前用 `./scripts/check_io_uring.sh` 在**目标环境**验证（ADR-010） |
| `liburing` 版本能力差异 | jammy 的 liburing 2.1 **没有** `io_uring_prep_sendfile`（只有 `splice`）→ 本版本无法把零拷贝与 io_uring 直接结合 |

---

## 7. NFS × 多实例实验室（用容器替代 NFS 设备）

本机既没有 NFS 设备、也没有 root，但 **Docker 足以搭出**"NFSv4.2 共享存储 + PostgreSQL + 两个真实
`fss_server`"的多实例环境，用来逐条验证 ADR-009 的判据（选举 / GC 门控 / 在途租约 / 崩溃回收 /
共享挂载探针 / 滚动升级护栏）。

**怎么搭、每一步为什么、踩过哪些坑 → 见 [lab-nfs-multiinstance.md](lab-nfs-multiinstance.md)**
（含：rpcbind / rpc_pipefs / rpc.mountd 三个必需组件，`--init` 与僵尸进程，LSan 与 C9.32 的冲突，
`http_proxy` 劫持容器内 HTTP 调用，迁移版本行缺失导致 readiness 503，等等）。
