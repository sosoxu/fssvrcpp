# NFS × 多实例 实验室手册（本机实测，可照做）

> **这份文档解决什么问题**：本机（Deepin 25）没有 NFS 设备、没有 root、`/usr` 只读，
> 但我们需要验证 [ADR-009](adr/ADR-009-multi-instance-consistency.md) 的多实例假设。
> 这里记录**怎么用 Docker 搭出**"NFSv4.2 共享存储 + PostgreSQL + 两个真实 `fss_server`"
> 的实验室，以及**每一步踩过的坑与处置**。
>
> 全部命令与结论都在本机实测过（2026-09-20 ~ 09-21）。带 ⚠️ 的是"不看就会重踩"的坑。

---

## 0. TL;DR

四条命令搭起来（细节见 §4/§5）：

```bash
# ① 宿主内核加载 NFS 模块（需要一次特权容器，之后宿主与所有容器都能用 NFS）
docker run --rm --privileged -v /lib/modules:/lib/modules:ro ubuntu:22.04 \
  bash -lc 'modprobe nfs && modprobe nfsd'

# ② NFSv4.2 服务端容器（导出宿主机某个目录）
docker run -d --name fss-nfs --privileged --network fss-net \
  -v /home/aaa/code/fssvrcpp/build/nfs-export:/export ubuntu:22.04 sleep infinity

# ③ PostgreSQL 16（多实例的共享状态）
docker run -d --name fss-pg --network fss-net \
  -e POSTGRES_USER=fss -e POSTGRES_PASSWORD=fsspw -e POSTGRES_DB=fss postgres:16-alpine

# ④ 两个"实例容器"（各自挂同一 NFS，跑同一个二进制）
docker run -d --name fss-nfs-client  --privileged --network fss-net -v <repo>:/src ubuntu:22.04 sleep infinity
docker run -d --name fss-nfs-client2 --privileged --network fss-net -v <repo>:/src ubuntu:22.04 sleep infinity
```

验证清单一句话版：`pg_locks` 里该 key **恰好一行**；非 leader 的 `fss_gc_runs_total` 不涨；
`uploadURL` 在 A → `downloadURL` 在 B 能读回同样字节；`kill -9` leader 后
`fss_gc_reclaimed_claiming_total` ≥ 1。

---

## 1. 本机环境事实（先认清再动手）

| 项 | 事实 | 后果 |
| --- | --- | --- |
| OS | Deepin 25（crimson），内核 `6.18.36-amd64-desktop-rolling` | 与项目文档里的 Ubuntu 22.04 不同 |
| `/usr` | **只读 overlay**（不可变系统） | `apt-get install` 不生效；要么走管理员路径，要么解包到前缀/容器 |
| root | 无 | 依赖用 `apt-get download` + `dpkg-deb -x`，或放进容器 |
| 网络 | **不能直连外网**（需走内网镜像/代理） | 见 §3 |
| Docker | 26.1.5，已配置内网镜像站 | 拉镜像走镜像站 |
| 内核模块 | `nfs`/`nfsd` 的 `.ko.zst` 在，但**默认未加载** | 必须先 `modprobe`，否则连 `/proc/filesystems` 里都没有 `nfs4` |

⚠️ **容器里的日志时间是 UTC，宿主是 CST（+8）**。曾把"22:56 的日志"误判成"8 小时没动静、
后台线程卡死"——其实那一刻就是 `06:56 CST`，进程一直健康。看时间戳先对时区。

---

## 2. 构建环境：一个装了工具链的容器

宿主既没有 cmake 也没有 gRPC/protobuf 的开发包，`/usr` 还只读。最省事的做法是**在容器里构建**，
仓库以 bind mount 挂进去，产物落在 `build/`（`uid 1000` 所有，宿主可直接用）。

```bash
docker run -d --init --name fssbuild \
  -v /home/aaa/code/fssvrcpp:/src -w /src ubuntu:22.04 sleep infinity

# 若容器内需要经内网代理/镜像访问 apt，按环境再加 -e http_proxy=... -e https_proxy=...
docker exec -u 0 fssbuild bash -lc 'export DEBIAN_FRONTEND=noninteractive; apt-get update -qq && \
  apt-get install -y --no-install-recommends build-essential cmake pkg-config \
    libgrpc++-dev protobuf-compiler protobuf-compiler-grpc libprotobuf-dev \
    libcurl4-openssl-dev libsqlite3-dev libssl-dev zlib1g-dev libpq-dev \
    python3 curl git iproute2 netcat-openbsd'

# 构建 / 跑门槛
docker exec -u 1000:1000 -e HOME=/tmp fssbuild bash -lc 'cd /src && cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j12'
docker exec -u 1000:1000 -e HOME=/tmp fssbuild bash -lc 'cd /src && ./scripts/run_all_gates.sh'
```

### 2.1 ⚠️ `--init` 不是可选项

不加 `--init` 时容器 PID 1 是 `sleep infinity`，**不回收孤儿进程**。后果不只是脏：
`tests/integration/test_config_wiring.cpp` 的 C10.17 用例要"等旧 `fss_server` 真正退出"
（轮询 `kill -0`），僵尸进程会让 `kill -0` 永远返回成功 → 用例假失败。实测旧容器里堆了 365 个僵尸。

### 2.2 ⚠️ 启动常驻进程：用 `docker exec -d`，不要 `nohup ... &`

`docker exec -i` 里 `nohup cmd &` 会随该 exec 会话结束被杀。可靠写法：

```bash
docker exec -d <容器> bash -lc 'cd /src && exec build/bin/fss_server --config ... > /tmp/x.log 2>&1'
```

### 2.3 ⚠️ `pkill -f` 会杀掉承载命令的 shell 自己

`docker exec ... bash -lc 'pkill -f "fss_server --config"'` —— 该 shell 自己的命令行里也含这个串，
于是自杀、后面的命令全不执行。用 `pkill -x fss_server`（按进程名精确匹配）。

### 2.4 ⚠️ ASan/LSan 与 C9.32 的"真 EAGAIN"用例天然冲突

`scripts/run_sanitizers.sh` 打开 LeakSanitizer；而 C9.32 的用例**故意把 `RLIMIT_NPROC` 压到耗尽**
来制造线程创建失败。LSan 退出时也要再起一个 tracer 线程，于是它自己撞上同一个 `EAGAIN`：

```
未预期异常（exit 70）：Resource temporarily unavailable      ← 产品代码行为正确
==NNNN==Failed spawning a tracer thread (errno 11).
==NNNN==LeakSanitizer has encountered a fatal error.          → 退出码被改成 1，而不是 70
```

**判定**：这是"测试与 sanitizer 的交互"，不是实现缺陷（关掉 `detect_leaks` 后同一用例立刻通过）。
在本环境跑门槛时用 `FSS_GATES_SKIP_SANITIZERS=1 ./scripts/run_all_gates.sh`，
其余 phase0~10 全绿；sanitizer 段的这条要用目标环境复核。

★ **另一条覆盖边界**：sanitizer 配置是 `FSS_WITH_PG=OFF`，因此 **PG 目标（`fss_pg` / PG 仓储 /
leader 选举）不在仓库 sanitizer 门槛的覆盖范围内**。本次改动的做法：单独建 `build-pg-asan`
（`-DFSS_WITH_PG=ON` + ASan/UBSan 标志）跑 `test_leader_election_reconnect`，全绿。

### 2.5 其它构建注意事项

- sanitizer 构建并行度用 `FSS_ASAN_JOBS=8`（本机 15 GB 内存）；`scripts/run_sanitizers.sh` 默认 4。
- 想启用 PG 目标：装 `libpq-dev` 后重新 `cmake` 即可（**不需要** `-DFSS_WITH_PG=ON`）。
- `storage.posix.one_filesystem_per_partition`：**E1b 起已真的生效**（启动期强制校验，`true`
  且不满足 → exit 78）；此处原先写"仍未接线"已过时。

### 2.6 ⚠️ 门槛容器需要 `/run/lock` 是**独立文件系统**（否则 e1b 的 U5 正例必失败）

`tests/integration/test_partition_filesystem_check.cpp` 的 U5 用一个**正例**证明"没有偷偷加
`staging == persistent` 规则"：它要求 `/dev/shm`（tmpfs）、仓库/`/tmp` 文件系统、`/run/lock`
三者的 `st_dev` **互不相同**（开发机上 `/run/lock` 是独立 tmpfs）。容器默认把 `/tmp` 与
`/run/lock` 都放在自己的 overlay 上（实测同一 `st_dev`），于是前置断言**明确失败**（该用例
刻意不跳过）。修法：起容器时加 `--tmpfs /run/lock`：

```bash
docker run -d --init --tmpfs /run/lock --name fssbuild -v <repo>:/src -w /src ubuntu:22.04 sleep infinity
# 验证：三个设备号互不相同
docker exec fssbuild sh -c 'stat -c "shm=%d" /dev/shm; stat -c "tmp=%d" /tmp; stat -c "runlock=%d" /run/lock'
```

---

## 3. 依赖与代码获取（受限网络环境）

本手册假定机器**不能直连外网**：系统依赖走内网 apt 镜像，容器镜像走内网 registry 镜像；
代码仓库用内网 Git 服务（本文以 `dp/inexus` 为例，见 `dp-git-mbr-workflow.pdf` 的流程要求）。
第三方库已 vendored 在 `third_party/`，缺失时的恢复方式见 `third_party/README.md`。

⚠️ 不要把任何凭据（账号口令、令牌、私钥）写进仓库文件或远端 URL 里；需要凭据的场合用环境变量
或凭据管理器，并只授予最小范围。

---

## 4. NFS 实验室（NFSv4.2 服务端 + 客户端）

### 4.1 让宿主内核认识 NFS

```bash
docker run --rm --privileged -v /lib/modules:/lib/modules:ro \
  -e http_proxy=... -e https_proxy=... ubuntu:22.04 bash -lc \
  'apt-get update -qq && apt-get install -y --no-install-recommends kmod >/dev/null && modprobe nfs && modprobe nfsd && lsmod | head'
```

验证：`/proc/filesystems` 出现 `nfs`/`nfs4`。**重启宿主后要重来**。

### 4.2 服务端容器（三个必需组件，缺一个都起不来）

```bash
docker run -d --name fss-nfs --privileged --network fss-net \
  -v /home/aaa/code/fssvrcpp/build/nfs-export:/export ubuntu:22.04 sleep infinity
docker exec -u 0 -e http_proxy=... fss-nfs bash -lc \
  'export DEBIAN_FRONTEND=noninteractive; apt-get update -qq && \
   apt-get install -y --no-install-recommends nfs-kernel-server rpcbind'
```

容器内启动顺序（**每一步都是坑，见 §7**）：

```bash
docker exec -u 0 fss-nfs bash -lc '
  echo "/export *(rw,sync,no_subtree_check,no_root_squash,fsid=0,insecure)" > /etc/exports
  rpcbind                       # ① 不启动它 → nfsd 注册 v3 服务失败（errno 111）
  mkdir -p /run/rpc_pipefs && mount -t rpc_pipefs rpc_pipefs /run/rpc_pipefs   # ② 不挂 → exportfs 推不进内核
  rpc.nfsd 8                    # ③ 起内核线程
  exportfs -ra                  # ④ 推导出表
  rpc.mountd                    # ⑤ 不起 → NFSv4 挂载会**无限挂死**
'
```

验证：`cat /proc/fs/nfsd/versions` 有 `+4.2`；`rpcinfo -p 127.0.0.1 | grep nfs` 有 2049。

### 4.3 客户端容器

```bash
docker run -d --name fss-nfs-client --privileged --network fss-net -v <repo>:/src ubuntu:22.04 sleep infinity
docker exec -u 0 -e http_proxy=... fss-nfs-client bash -lc \
  'apt-get update -qq && apt-get install -y --no-install-recommends nfs-common libc6-dev'
docker exec -u 0 fss-nfs-client bash -lc 'mkdir -p /mnt/nfs && mount -t nfs4 -o vers=4.2 fss-nfs:/ /mnt/nfs'
```

- 挂载需要 `--privileged`（或 `SYS_ADMIN` + `apparmor=unconfined`）。
- **NFSv4 只占 2049**，不需要 rpcbind；NFSv3 才要额外处理 rpcbind/mountd/statd 的端口。
- 导出用 `no_root_squash`（或 `anonuid=1000,anongid=1000`），否则容器里以 uid 1000 写入会被压成 `nobody`。
- 探针的 N3（`syncfs`）要编译一个小助手 → 客户端需要 **`libc6-dev`**（只装 `gcc` 会失败，那一项变"无结论"）。

### 4.4 跑项目的语义探针（C9.27）

```bash
docker exec -u 0 fss-nfs-client bash -lc 'cd /src && bash scripts/check_nfs_semantics.sh /mnt/nfs'
```

退出码约定：`0=全部通过 / 1=有 FAIL / 2=无结论`（与 `scripts/check_io_uring.sh` 一致）。
本机在 NFSv4.2 上实测：N1 rename 原子性 1902 次采样 0 异常；N2 close-to-open 8~10 ms；
N3 `syncfs` 覆盖整文件系统；N4 flock 仅报告。

---

## 5. 多实例实验室（PG + 两个真实实例）

### 5.1 PostgreSQL 与**迁移版本行**

```bash
docker exec -i fss-pg psql -U fss -d fss -v ON_ERROR_STOP=1 -q < db/migrations/001_init.sql
# ★ 关键：裸跑 SQL 只建表，不写版本行 → readiness 会 503（这正是 B2a 该有的行为）
docker exec fss-pg psql -U fss -d fss -c "INSERT INTO schema_migrations(version,name) VALUES(1,'init') ON CONFLICT DO NOTHING"
```

原来那条 503 的原文值得记住：
`schema_migrations 为空：该数据库存在迁移表，但没有任何已记录的迁移版本（迁移应用流程没有把版本写进表）`
—— **readiness 探针确实能抓到真实的部署缺陷**；生产上用 `scripts/dev_postgres.sh` 或正规迁移工具。

### 5.2 实例配置（multi 模式的 7 条强制校验）

`deployment.mode=multi` 要求同时满足：`metadata.repository=postgres`、`location.repository=postgres`、
`leases.enabled=true`、`leader_election.enabled=true`、`storage.posix.shared_mount_required=true`、
`gc.require_lease_expiry=true`、`0 < deployment.max_clock_skew_seconds <= 60`；任一不满足 → **exit 78**。
两个实例的差异只允许在"每实例键"（`instance_id`、端口、`public_base_url`）上——
`config_hash` 把每实例键剔除后计算，不一致会让 readiness 变 not ready（滚动升级护栏）。

最小可用配置（本实验室用的两份只差实例名/端口/公网地址，见仓库 `build/multi-e2e/config-{a,b}.json`）：

```json
{
  "deployment": {"mode": "multi", "instance_id": "fss-a", "expected_instances": 2},
  "server": {"http": {"bind": "0.0.0.0", "port": 18401}, "grpc": {"enabled": false}},
  "storage": {"driver": "posix", "posix": {"root": "/mnt/nfs/fss-root", "shared_mount_required": true}},
  "metadata": {"repository": "postgres", "postgres": {"dsn": "postgresql://fss:fsspw@fss-pg:5432/fss"}},
  "location": {"repository": "postgres", "postgres": {"dsn": "postgresql://fss:fsspw@fss-pg:5432/fss"}},
  "leases": {"enabled": true, "ttl_seconds": 10, "renew_interval_seconds": 3, "time_source": "database"},
  "leader_election": {"enabled": true, "backend": "postgres_advisory_lock", "lock_key": 1179865927},
  "gc": {"enabled": true, "dry_run": false, "require_lease_expiry": true, "interval_seconds": 1},
  "self_signed": {"enabled": true, "signing_key": "multi-e2e-secret", "key_id": "k1",
                  "public_base_url": "http://fss-nfs-client:18401/api/file"},
  "auth": {"mode": "disabled"}
}
```

⚠️ **给实例的 env 里不要带 `http_proxy`**：Python/curl 会遵从它，请求被代理劫走，
报 `ERR_DNS_FAIL` 而不是连到服务。调用服务时用 `env -u http_proxy -u https_proxy ...`。

### 5.3 启动与验证

见 §0 的第 ④ 步与仓库里的 `build/multi-e2e/` 脚本（下表）。判据与实测结果：

| 判据 | 命令/观察点 | 本机实测 |
| --- | --- | --- |
| 恰好一个 leader | `SELECT objid,pid FROM pg_locks WHERE locktype='advisory'` | 恰好 1 行；`kill -9` 后 pid 换成对端 |
| 选举门控 GC | 非 leader 日志 `gc_skipped_not_leader`；leader `gc_run` | `fss_gc_runs_total` 62 → 148 |
| 共享挂载**跨实例**可见 | 横幅 `shared mount: 已验证…`；root 下有两个 `.fss_probe.<id>` | ✅ |
| 跨实例数据面 | A `uploadURL`+PUT+metadata → B 读 metadata/downloadURL | 逐字节一致（含崩溃后） |
| 崩溃回收（C9.26） | A 带 `FSS_CLAIM_HOLD_MS=8000` → 轮询到 `state='claiming'` → `kill -9` A | claiming 行先留存、租约到期后被回收；`fss_gc_reclaimed_claiming_total 1`；暂存孤儿对象被删 |
| 陈旧实例行清理 | 插一条 600s 前心跳的行 | 一个 tick（≤10s）内被删 |
| 滚动升级护栏 | 给 B 设 `FSS_SERVICE_VERSION_OVERRIDE=9.9.9` 重启 | 双方 readiness **503**，原因逐字给出对端版本 |
| 大对象传输中途崩溃 | 1 GiB PUT 到 523 MiB 时 `kill -9` 上传方 | 暂存区留下 `<fileid>.tmp.<实例id>.<pid>.<计数>.<随机>`（ADR-009 M1 的命名）；**没有**被 rename 成正式对象；无位置记录 |
| 暂存 tmp 的回收 | 同上 + 把 tmp 的 mtime 调老越过 `gc.staging_ttl_hours`(24h) | `fss_gc_tmp_removed_total` +1，文件消失；未越过门槛时报告 `tmp_skipped_too_young`（**保护在途**） |
| 按需 GC 的 dry-run | `POST /v2/gc:run?dryRun=true` | 报告 `dry_run:true`，`deleted_*` 全 0（"只能更保守"） |
| PG 停摆（运行中实例） | `docker stop fss-pg` | readiness **503**（原因："连接 PostgreSQL 失败：…"）；PG 回来后**自动**回到 200 |
| PG 停摆（新实例启动） | 停 PG 期间启动第三个实例 | **拒绝启动（exit 78）** + 可直接照做的修复指令（检查 DSN / `max_connections` / 迁移是否执行） |
| 选举连接被切断后重连 | 仓库用例 `tests/integration/test_leader_election_reconnect.cpp`（用 `pg_terminate_backend` 切断持锁会话） | 修复前必然失败（`recovered_ms = -1`）；修复后 ≤15 s 内重新当选，且同键第二实例始终自称非 leader |

### 5.4 共享挂载探针的三条行为（容易误判，务必分清）

| 你做的动作 | 实际会发生什么 |
| --- | --- |
| 删掉某个实例的 `.fss_probe.<id>` 文件 | **自愈**：该实例每 10s（心跳 tick）重写自己的探针 |
| 让某实例的挂载坏掉（写不出探针） | 它 readiness **503**，并且**连心跳也不再更新** → 30s 后退出 live peer 集合（"写不出探针的实例不算活着"） |
| 让**健康**实例看不到对端探针（本实验室做法：把本实例 root 换成不含对端探针的副本） | 本实例 readiness **503**：「实例 X 是 live peer（心跳在 30s 内），但其探针文件 … 不可见」；对端仍 200（各自判定自己的视图） |

`InstanceConsistencyMonitor::Evaluate()` 的顺序是 ①写自己的探针 → ②写心跳 → ③检查每个 live peer，
所以"写不出探针"会**连带**停止心跳 —— 这是设计，不是 bug。

---

## 6. 仓库里的实验室脚本（`build/` 下，gitignored）

| 路径 | 用途 |
| --- | --- |
| `build/multi-e2e/config-a.json` / `config-b.json` | 两个实例的 multi 配置 |
| `build/multi-e2e/e2e_upload.py` | A 上传 → B 下载 + 读元数据的端到端 |
| `build/multi-e2e/e2e_delete.py` | A 上传 → B 删除 → A 侧确认 404（跨实例一致性变更） |
| `build/multi-e2e/prepare_upload.py` / `post_metadata.py` | 崩溃恢复用：拆开 uploadURL/PUT 与 createMetadata |
| `build/multi-e2e/check_readiness.py` | 打印两个实例 readiness 的状态码与原因 |
| `build/multi-e2e/call.py` | 通用 HTTP 调用（method/url/body），便于临时探接口 |
| `build/nfs-test/xcto_*.sh` / `xrename_*.sh` | 跨容器 close-to-open 与 rename 原子性（两个实例各挂一次 NFS） |

接口备忘（本实验室用到的）：`GET /api/file/v2/files/uploadURL`、`POST /api/file/v2/files/metadata`、
`GET /api/file/v2/files/{id}/metadata`（**注意有 `/metadata` 后缀**）、`GET /api/file/v2/files/{id}/downloadURL`、
`DELETE …/metadata`、`POST /api/file/v2/gc:run`（leader 专属）、`GET /api/file/v2/{liveness,readiness}_check`、
`GET /api/file/v2/info`、`GET /metrics`。

**跑仓库自带的 PG 测试（不走 `dev_postgres.sh` 的 fixture，直接指向实验室 PG）**：

```bash
docker network connect fss-net fssbuild        # 一次性：让构建容器能解析 fss-pg
docker exec -u 999:999 fssbuild bash -lc \
  'cd /src && cmake -S . -B build-pg -DFSS_WITH_PG=ON && cmake --build build-pg -j12'
docker exec -u 1000:1000 -e FSS_PG_DSN=postgresql://fss:fsspw@fss-pg:5432/fss \
  fssbuild bash -lc 'cd /src && ./build-pg/bin/test_leader_election_reconnect'
```

（`FSS_PG_DSN` 是这些用例的既定入口：不设时它们找本机 `scripts/dev_postgres.sh` 的 dev PG；
设了就指向任何 PG —— 包括本实验室的 `fss-pg`。`ctest -L pg` 那条路会先跑 `pg_fixture_setup`，
需要联网下载 dev PG 二进制。）

---

## 7. 坑清单（症状 → 根因 → 处置）

| 症状 | 根因 | 处置 |
| --- | --- | --- |
| `rpc.nfsd: writing fd to kernel failed: errno 111`；内核日志 `svc: failed to register nfsdv3 RPC service (errno 111)` | 没有 rpcbind，nfsd 注册 v3 服务失败 | 容器内先启动 `rpcbind` |
| `exportfs -v` 有导出、但 `/proc/fs/nfsd/exports` 为空 | 没有挂 `rpc_pipefs`，导出推不进内核 | `mount -t rpc_pipefs rpc_pipefs /run/rpc_pipefs` |
| NFSv4 挂载**无限挂死**（`mount.nfs4` 卡住） | 没有 `rpc.mountd` | 启动 `rpc.mountd` 后再挂 |
| `mount.nfs4: Failed to find 'tcp' protocol` | 客户端缺包/参数写法 | 用 `mount -t nfs4 -o vers=4.2 host:/ /mnt`，并装 `nfs-common` |
| 容器里"看不到监听端口" | 镜像里根本没装 `ss` | 用 `rpcinfo -p` / `cat /proc/fs/nfsd/portlist` 判断 |
| 读者一直看不到刚创建的文件（卡死） | NFS 客户端缓存**负查找**：读者先启动、文件还不存在 | 先创建文件/标记再启动读者（脚本里已这么做） |
| 容器内日志时间比宿主"早 8 小时" | 容器是 UTC，宿主是 CST | 看时间先换算时区；别误判成卡死 |
| readiness 503 但示例里一切正常 | 迁移只建表、没写 `schema_migrations` 版本行 | 补版本行（或用 `scripts/dev_postgres.sh`） |
| 用 python/curl 调服务报 `ERR_DNS_FAIL`（代理的错误页） | 容器 env 里有 `http_proxy` | `env -u http_proxy -u https_proxy` 调用 |
| `docker exec` 里的后台进程立刻没了 | exec 会话结束即收回 | 用 `docker exec -d` + `exec` |
| 命令只执行了一半就没了 | `pkill -f "<自己命令行的子串>"` 自杀 | `pkill -x` 或改匹配串 |
| C10.17 假失败（`wait_gone` 永远不成立） | 容器 PID 1 不回收孤儿进程（无 `--init`） | 容器加 `--init` |
| sanitizer 门槛在 C9.32 用例上失败 | LSan 的 tracer 线程也撞上被刻意耗尽的 `RLIMIT_NPROC` | 见 §2.4；用 `FSS_GATES_SKIP_SANITIZERS=1` 并在目标环境复核 |
| `git fetch` / 拉镜像报 `CONNECT tunnel failed`、`503` | 受限网络挡了外网域名 | 改用内网 Git / registry 镜像；**不要**把凭据交给第三方镜像（§3） |
| 把文件 mtime 调老后 GC 仍报 `tmp_skipped_too_young` | GC 用**存储列出的 mtime** 判定年龄，而它跑在另一个容器里，NFS 客户端的**属性缓存**（`acregmax` 默认 60s）还拿着旧值 | 等过缓存窗口（≤60s）再跑；这不是"清理失效" |
| PG 不可用时 `uploadURL` 返回 **500** 而不是 503 | 位置记录写入失败被投影成 500 | 契约允许 500/502/503；调用方要按"依赖不可用"处理 5xx，不要只看 503 |

---

## 8. 已发现的问题（含已修，登记避免重复发现）

1. **横幅文案过时**：`src/main/server_main.cpp` 里打印
   `★ 上传路径尚未 Acquire/Renew，这三个键当前无效果`。实测 `staging_leases` 在上传时**确实**被创建
   （TTL≈9.6s）并被崩溃回收流程使用（C2 已接通）——属于 R13 那类"文档说没生效、实际已生效"的漂移。
2. **被领取的租约行会留在 `staging_leases`**：`ClaimExpired` 把 `expires_at` 往后推（防重复领取），
   本实验室窗口内没看到过期行被清理；归属 C9.25 的清理范围待确认。
3. **回收后空目录不清理**：孤儿对象被删了，`blobs/<container>/<path>/` 空目录还在（无害）。
4. **"在途租约互斥 503"无法从公开 HTTP 面触发**：`uploadURL` 的 `fileSource` 每次由服务端生成，
   `getFileLocation` 只按 `FileID` 查地址、不取租约 —— 这条判据只能在仓储层用测试替身驱动
   （见 `tests/integration/test_postgres_lease_lifecycle.cpp`）。

### ✅ 8.1 **PG 重启后集群永久失去 leader（GC 静默停摆）** —— 已修（ADR-009 §4.4.1）

> **修复状态**：已在 `src/infra/postgres/pg_leader_election.cpp` 实现"锁连接可重建 + 1s 冷却"，
> 并新增回归用例 `tests/integration/test_leader_election_reconnect.cpp`
> （修复前必然失败、修复后通过）。缺陷原文、根因与不变量见
> [ADR-009 §4.4.1](adr/ADR-009-multi-instance-consistency.md)。
> **修复后在同一注入下复测**：`docker restart fss-pg` → **≤2 s** advisory lock 恢复到 1 行，
> 且持有者是**新后端**；新 leader 立刻继续 `gc_run`，对端打的是**正常的**
> `gc_skipped_not_leader`；两个实例各记 **1 次** `gc_skipped_leader_election_unavailable`
> （新加的"选举不可用"事件，与"别人是 leader"区分开）。下面保留原始复现与根因，便于日后回归。

**症状**：`pg_locks` 里 advisory lock **0 行**，两个实例的日志每秒都在打
`gc_skipped_not_leader`，而两者的 **readiness 仍然是 200**（数据面完好）。GC 从此不再运行，
按需 GC 端点对所有实例都返回 503（"我不是 leader"），运维连手动兜底都做不到。

**复现**（本机实测，可重复）：

```bash
# 前置：两个 multi 实例就绪、恰好一个持锁
docker restart fss-pg                      # 或任何让"锁连接"断开的事件（主备切换/网络抖动/pg_terminate_backend）
# 观察（等 90s 以上也不会自愈）：
docker exec fss-pg psql -U fss -d fss -tAc "SELECT count(*) FROM pg_locks WHERE locktype='advisory'"   # → 0
docker exec -u 0 fss-nfs-client2 bash -lc 'grep -c gc_skipped_not_leader /tmp/fss-b.log'              # 持续增长
# 正控：重启任一实例，锁立刻回来
docker exec -u 0 fss-nfs-client bash -lc 'pkill -9 -x fss_server && exec …/fss_server --config …'
docker exec fss-pg psql -U fss -d fss -tAc "SELECT count(*) FROM pg_locks WHERE locktype='advisory'"   # → 1
```

**根因**（代码位置：`src/infra/postgres/pg_leader_election.cpp`）：

- `IsLeader()` 在"已是 leader 但连接不健康 / ping 失败"时 `connection_.reset(); leader_ = false;`
  —— 安全方向正确；
- 但紧接着的 `!leader_` 分支调 `TryAcquire()`，而 `TryAcquire()` 在 `connection_ == nullptr` 时
  **直接返回 `kUnavailable`**，**没有任何重连**；非 leader 实例的锁连接被 PG 重启打断后
  也不会被重建（`ExecParams` 持续失败）→ 所有实例**永远**返回"我不是 leader"。
- 头文件里写的运行期语义（"已是 leader → `SELECT 1` 验证；不是 leader → 每轮 `TryAcquire()` 以支持 failover"）
  只在**连接健康**时成立；连接被重置后这条链断掉了。

**影响**：GC（暂存 tmp / 孤儿对象 / crashing 的 claiming 行）全集群停摆且**无告警**；
生产上 PG 重启/切换是很常见的事件，因此这是一条会静默累积垃圾、且在出事前不可见的缺陷。

**建议修法**：`IsLeader()` 在 `connection_ == nullptr || !connection_->Healthy()` 时**重建锁连接**
（带退避，避免风暴），重连成功后立即 `TryAcquire()`；同时把"选举不可用/长时间无法取锁"
反映到 readiness（或至少一条显式指标 + 告警），不要让它停在"绿着但没有 leader"。

**测试空白**：现有用例覆盖了启动期选举、SIGTERM 释放、`kill -9` 后**对端接管**，
但**没有**覆盖"PG 重启（锁连接整体断开）后重新选举"。建议补一条：重启 PG → 轮询
`pg_locks` 在有限时间内重新出现一行、且新 leader 的 `fss_gc_runs_total` 继续增长。

---

## 9. 边界（不要外推）

- 本实验室的 NFS 是**同一台宿主、同一个内核**上的 Linux knfsd；它验证的是**协议语义**，
  **不能**替代真实 NFS 设备（NetApp/Isilon/ONTAP 等实现差异）。
- **没有**断电耐久性验证（需 root/断电控制），探针自身也把该项标为"无结论"。
- 多实例实验室里 PG 也是本机容器；跨主机时钟偏移只在上文那条 `max_clock_skew_seconds` 校验里涉及。
- `storage.posix.one_filesystem_per_partition`（每 partition 独占文件系统 / `syncfs` 隔离）**仍未接线**，
  多实例共盘时 `syncfs` 的跨实例干扰仍是登记在册的风险。

---

## 10. 拆掉/重建

```bash
docker rm -f fss-nfs-client fss-nfs-client2 fss-nfs fss-pg     # 实例 / NFS / PG
docker network rm fss-net
# fssbuild 是构建工具箱，通常留着
# 宿主内核模块可留着（重启自然消失）；要立刻卸载：docker run --rm --privileged -v /lib/modules:/lib/modules:ro ubuntu:22.04 bash -lc 'rmmod nfsd nfs'
```

导出目录在 `build/nfs-export/`（gitignored），删掉即可；它里面只剩实验用的临时文件。
