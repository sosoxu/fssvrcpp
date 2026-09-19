# 阶段 9 测试证据 —— 容器镜像（判据 **C9.8**）

| 项 | 值 |
| --- | --- |
| 判据 | `docs/04-implementation-plan.md` **C9.8**：容器镜像可构建、可启动、`readiness_check` 通过；镜像包含 production 配置（`auth.mode=jwt` 强制） |
| 状态 | ✅ **满足**（在本机**真实** docker 上构建、启动、断言、清理；非"未验证"）。**§10 = 容器硬化运行形态实测（本轮新增）** |
| 验证脚本 | `scripts/verify_image.sh`（可执行；`set -euo pipefail` + `trap` 清理；`[0/7]`~`[7/7]` = C9.8 基础断言，`[H*]` = 本轮新增硬化场景） |
| 镜像 | `Dockerfile`（仓库根，多阶段：`builder` / `runtime`，均基于 `ubuntu:22.04`） |
| 环境 | Docker client/server **27.3.1**；Ubuntu 22.04 / g++ 11.4；16 核 / 7 GiB RAM / 无 root（宿主） |
| 源码快照 | git HEAD `4a07a877d601ac1da224a694d6b4a45d3bc2ff9d` + **并发未提交改动**（构建时 `src/CMakeLists.txt` 08:37、`src/main/server_main.cpp` 08:38 被同工作区的另一个代理修改；见 §6 说明） |
| 关键结果 | 冷构建 **452 s** → 镜像 **107 MB**；`readiness_check` **HTTP 200**；`/v2/info` → `authMode=jwt`；无 token 的 `uploadURL` → **401**；容器 uid **10001**；HEALTHCHECK **healthy**；`auth.mode=disabled` / 空密钥 → **exit 64 拒绝启动** |
| 容器硬化（本轮新增，§10） | `--read-only`（**不需要**可写 `/tmp`，Dockerfile **未改动**）、`--cap-drop=ALL` + `no-new-privileges`（**Docker 默认 seccomp**）、`--memory=128m` 下 **1 GiB 上传+读回**（SHA-256 一致；**进程峰值 RSS 20.9 MiB**）、`--restart=on-failure` + HEALTHCHECK + SIGTERM 优雅退出（`ExitCode=0` / **0.30 s** < 10 s grace / 无 terminate 迹象）、健康检查**负控** `unhealthy`、两条反向对照 **全部实测**；**K8s / restricted PodSecurity / CVE 扫描 / 多架构仍未验证**（§10.8） |

---

## 1. 结论

**C9.8 ✅ 满足。** 四条子要求逐条有可执行证据：

| 子要求 | 实测结果 |
| --- | --- |
| 镜像可构建 | `docker build` 成功（多阶段，Release，`fss_server` 可执行文件 `test -x` 通过）；冷构建 452 s，镜像 107 MB |
| 镜像可启动 | `docker run` 成功；启动横幅显示 `bind 0.0.0.0:8080`、`auth jwt（HS256 本地校验）`、`storage driver posix`、`storage root /data` |
| `readiness_check` 通过 | `GET /api/file/v2/readiness_check` → **HTTP 200**，body `File service is ready`（轮询，第一次探测即 200，未额外等待） |
| production 配置强制 `auth.mode=jwt` | `ENV FSS_AUTH_MODE=jwt` + entrypoint 拒绝降级；`/v2/info` 的 `authMode` 实测为 `jwt`；无 `Authorization` 的 `GET /v2/files/uploadURL` → **401**；伪造 Bearer token → **401**；`-e FSS_AUTH_MODE=disabled` → **exit 64**；空 `FSS_JWT_HMAC_SECRET` → **exit 64** |

---

## 2. 交付物

| 路径 | 内容 |
| --- | --- |
| `Dockerfile` | 多阶段镜像：`builder`（apt 装 cmake/g++/make/pkg-config/ca-certificates/libssl-dev/libsqlite3-dev/libcurl4-openssl-dev/libgrpc++-dev/libgrpc-dev/libprotobuf-dev/protobuf-compiler/protobuf-compiler-grpc → `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` → `cmake --build build --target fss_server`）；`runtime`（仅运行时库 + 非 root 用户 `fss` uid 10001 + entrypoint/healthcheck + `EXPOSE 8080` + `VOLUME /data` + `HEALTHCHECK` + `USER fss`） |
| `scripts/verify_image.sh` | C9.8 的可复现验证：stage 最小构建上下文 → `docker build` → `docker run`（随机宿主端口、数据目录在 `build/image-verify/data`）→ 轮询 readiness → 断言 `authMode=jwt` 与 401 → 断言非 root → 等 `healthy` → 断言拒绝降级 → `trap` 停容器并删除 |

> 只新建了以上两个文件与本证据文件；**未修改** `src/`、`docs/` 下其它文档、`AGENTS.md`、`scripts/run_all_gates.sh`、`tests/CMakeLists.txt`。

---

## 3. Dockerfile 关键决策

### 3.1 基础镜像与两个阶段都选 `ubuntu:22.04`

`builder` 与 `runtime` 用**同一个** `ubuntu:22.04`（本机已有 `bf7f4568d957`，78.1 MB），
使编译期与运行期的 ABI 完全一致。CMake 配置输出的版本与本项目阶段 0 的受支持环境逐项吻合：

```
--  fssvrcpp 0.1.0 (Release)
--    C++ standard : 20
--    compiler     : GNU 11.4.0
--    grpc         : 1.30.2  (enabled=ON)
--    protobuf     : 3.12.4
--    openssl      : 3.0.2
--    sqlite3      : 3.37.2
```

这也是**没有退到 runtime-only 镜像**的原因：`third_party/` 完整（`nlohmann/json.hpp`、
`catch2/catch.hpp`、`httplib.h` 0.26.0 均在位），gRPC/protobuf 与 AGENTS.md §1 一致，
容器内构建一次成功。

### 3.2 apt 镜像源：换成 USTC，并给出实测理由

`ubuntu:22.04` 默认用 `archive.ubuntu.com`。在**同一个容器 base** 上实测 `apt-get update`：

| 源 | `apt-get update` 实测 |
| --- | --- |
| `http://archive.ubuntu.com/ubuntu`（默认） | **90.3 s** |
| `http://mirrors.ustc.edu.cn/ubuntu`（本机 `/etc/apt/sources.list` 同款） | **58.2 s** |

因此在两个阶段都用 `sed` 把 http 主机名替换为 USTC（只改主机名，不动发行版与组件），
可用 `--build-arg UBUNTU_MIRROR=...` 覆盖。理由写在 Dockerfile 注释里。

> 说明：下载速度在本网络下波动很大（builder 阶段 111 MB 的包实测平均约 1.8 MB/s，
> 但索引文件有若干次 5 分钟级的停滞）。452 s 里 **412.8 s 是 apt 阶段**，
> 真正的编译只有 **37.3 s**。

### 3.3 并行度 `min(nproc, 8)`（对原始命令的**有意偏离**，已登记）

要求的命令是 `-j"$(nproc)"`。本构建机是 **16 核 / 7 GiB RAM（可用 ≈5 GiB）**，AGENTS.md §4.3
已登记"高 `-j` 会 OOM 杀掉 `cc1plus`"（sanitizer 构建实测）。Dockerfile 改为
`jobs=min(nproc, 8)`，并保留 `--build-arg FSS_BUILD_JOBS=16` 以复现原始命令。
构建日志确认：`build jobs = 8 (nproc=16)`，编译 37.3 s 成功、无 OOM。
（编译不是瓶颈；apt 才是。）

### 3.4 运行时依赖以 `ldd` 为准

`builder` 里显式跑 `ldd build/bin/fss_server` 并打印到构建日志（证据），直接依赖与包映射：

| `.so` | 包 |
| --- | --- |
| `libgrpc++.so.1` | `libgrpc++1`（传递拉入 `libgrpc10` / `libgpr10` / `libabsl20210324` …） |
| `libprotobuf.so.23` | `libprotobuf23` |
| `libcurl.so.4` | `libcurl4` |
| `libsqlite3.so.0` | `libsqlite3-0` |
| `libssl.so.3` / `libcrypto.so.3` | `libssl3`（基础镜像已带） |

`runtime` 阶段只 `apt-get install -y --no-install-recommends ca-certificates libgrpc++1
libprotobuf23 libcurl4 libsqlite3-0 libssl3`（apt 自动补齐传递依赖），实测装出 17 个新包、
21.3 MB。`ca-certificates` 是真实运行时依赖：S3 SigV4 与远端 Entitlements 走 TLS 时要校验对端。

### 3.5 非 root + 数据卷

* `groupadd --gid 10001 fss` + `useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin fss`；
* `install -d -o fss -g fss -m 0750 /data /data/blobs /etc/fss`；
* `VOLUME ["/data"]`，`USER fss`；
* 固定 uid 10001 便于编排层用 `runAsUser` / `fsGroup` 对齐。

容器内文件属主实测：`/data`、`/data/blobs` 为 `fss:fss`；`/etc/fss/fss.example.json` 为 `fss:fss 0600`。

### 3.6 HEALTHCHECK：用镜像内已有的 bash `/dev/tcp`，不额外装 curl

```dockerfile
HEALTHCHECK --interval=5s --timeout=3s --start-period=10s --retries=12 \
  CMD ["/usr/local/bin/healthcheck.sh"]
```

`healthcheck.sh` 用 bash 的 `/dev/tcp` 打开连接，发
`GET /api/file/v2/readiness_check HTTP/1.1`，仅当响应首行含 `" 200 "` 时 `exit 0`。
选它而不是 `curl` 的原因：**保持运行时依赖最小**（不必为了探针多装一个 ~200 KB 的二进制及其依赖），
且 `ubuntu:22.04` 自带 bash。`docker build` 会对含 `SECRET` 的 ENV 名报
`SecretsUsedInArgOrEnv` 警告——那是对空占位符的**误报**（见 §6）。

### 3.7 `auth.mode=jwt` 强制的机制

组合根 `src/main/server_main.cpp` 当前**只读环境变量**、不解析 JSON 配置文件，因此"production 配置"
落在 ENV + entrypoint 双层：

```dockerfile
ENV FSS_AUTH_MODE=jwt \
    FSS_JWT_HMAC_SECRET="" \
    FSS_TRANSFER_SECRET="" \
    FSS_JWT_ISSUER="" \
    FSS_JWT_AUDIENCE="" \
    FSS_JWT_VERIFY_SIGNATURE=true \
    FSS_JWT_REQUIRE_PARTITION_CLAIM=true \
    FSS_DEPLOYMENT_MODE=single \
    FSS_STORAGE_DRIVER=posix FSS_STORAGE_ROOT=/data \
    FSS_HTTP_PORT=8080 FSS_BIND_ADDRESS=0.0.0.0 FSS_GRPC_PORT=0 \
    FSS_POSIX_DURABILITY=per_file
```

`/usr/local/bin/entrypoint.sh`（生成自 Dockerfile heredoc，**已实测镜像内没有发生构建期变量展开**）
再做两件 fail-closed 的事：

1. `FSS_AUTH_MODE != jwt` → `exit 64`（所以 `docker run -e FSS_AUTH_MODE=disabled` 不能把生产镜像
   变成 allow-all）；
2. `FSS_JWT_HMAC_SECRET` 或 `FSS_TRANSFER_SECRET` 为空 → `exit 64`（镜像内**没有任何真实密钥**，
   必须由运行时注入）。

**密钥/取值说明（写进镜像注释与本节，便于运维照抄）**：

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `FSS_JWT_HMAC_SECRET` | `""`（空占位） | **必须**由运行时注入（`docker run -e` / compose secrets / K8s Secret）。留空则进程/entrypoint 拒绝启动——这是 `LocalJwtAuthorizer::Ready()` 的既有行为 |
| `FSS_TRANSFER_SECRET` | `""`（空占位） | 自签数据面 token 的 HMAC 密钥；组合根默认值是 `dev-secret-change-me`，镜像**不**沿用该默认（否则生产可伪造数据面 URL），改为强制注入 |
| `FSS_JWT_ISSUER` | `""` | 非空才校验 `iss`（`local_jwt_authorizer.cpp:200-204`）。生产按平台实际值设置；非敏感，可直接放部署清单 |
| `FSS_JWT_AUDIENCE` | `""` | 非空才校验 `aud` 包含关系。同上 |
| `FSS_JWT_VERIFY_SIGNATURE` | `true` | 设为 `false` 会退化为不验签，生产**不得**关闭 |
| `FSS_DEPLOYMENT_MODE` | `single` | `multi` 会被组合根拒绝启动（ADR-009 的 PG 形态尚未交付） |

`config/fss.example.json` 被复制到 `/etc/fss/fss.example.json`（fss:fss 0600）作为**带注释的参考文档**；
**它不是生效来源**——组合根当前不读 JSON 配置（AGENTS.md §0「阶段 4 后续」）。这一点在镜像注释里写明了。

---

## 4. 实测输出（原始）

### 4.1 冷构建（第一次完整运行）

```
== [0/7] 准备最小构建上下文 /home/ll/fssvrcpp/build/docker-context
4.8M	/home/ll/fssvrcpp/build/docker-context
== [1/7] docker build -f Dockerfile -t fssvrcpp:verify (context=.../build/docker-context)
#8 [internal] load build context
#8 transferring context: 4.04MB 0.1s done
#9  [builder 2/12] ... apt-get update; apt-get install ...    -> #9 DONE 412.8s
#10 [runtime 2/8]  ... apt-get update; apt-get install ...    -> #10 DONE 347.5s
#21 [builder 12/12] build jobs = 8 (nproc=16)
#21 37.34 [100%] Built target fss_server
#21 DONE 37.5s
#27 naming to docker.io/library/fssvrcpp:verify done
构建耗时: 452s
镜像大小: 107MB (106576561 bytes)
```

构建日志里 `ldd build/bin/fss_server` 的直接依赖（节选，完整输出见构建日志）：

```
libgrpc++.so.1 => /lib/x86_64-linux-gnu/libgrpc++.so.1
libprotobuf.so.23 => /lib/x86_64-linux-gnu/libprotobuf.so.23
libcurl.so.4 => /lib/x86_64-linux-gnu/libcurl.so.4
libsqlite3.so.0 => /lib/x86_64-linux-gnu/libsqlite3.so.0
libcrypto.so.3 => /lib/x86_64-linux-gnu/libcrypto.so.3
```

### 4.2 启动 / readiness / production 配置 / 非 root / health（第一次运行）

```
== [2/7] docker run（随机宿主端口 → 容器 8080；数据目录 .../build/image-verify/data）
宿主端口: 32768  base url: http://127.0.0.1:32768/api/file

== [3/7] 轮询 GET http://127.0.0.1:32768/api/file/v2/readiness_check（最多 60s）
readiness: HTTP 200  body='File service is ready'

== [4/7] 断言 production 配置
GET /v2/info -> {"authMode":"jwt","buildVersion":"0.1.0","connectedOuterServices":["storage"],"version":"v2"}
authMode = jwt  ✅
GET /v2/files/uploadURL（无 Authorization）-> HTTP 401
     body='{"code":401,"message":"Missing partitionID","reason":"Unauthorized"}'
强制 jwt（未授权 401）✅

== [5/7] 断言容器以非 root 运行
docker exec id -u = 10001 (fss)
非 root ✅

== [6/7] 等待 HEALTHCHECK 变为 healthy（最多 90s）
Health.Status = healthy ✅

== [7/7] 断言镜像拒绝 production 配置降级
auth.mode=disabled -> exit 64: 拒绝启动：本镜像是 production 镜像，FSS_AUTH_MODE 必须为 jwt（当前='disabled'）。
空 FSS_JWT_HMAC_SECRET -> exit 64: 拒绝启动：以下密钥为空，必须由运行时注入（-e / compose secrets / K8s Secret）： FSS_JWT_HMAC_SECRET
fail-closed ✅

C9.8 ✅ 全部断言通过
```

### 4.3 第二次运行（加强 401 的**反向对照**，R1；apt 层命中缓存，编译因并发改码重跑）

第一次的 401 是 `Missing partitionID`——因为 `partition` 来自 `data-partition-id` **请求头**
（`src/app/usecases/caller_context.cpp:9`），而不是查询参数。这样"401"只证明"缺了某个头"。
于是脚本改为带上 `data-partition-id: opendes`，让 401 精确落在**缺 Bearer token**上，
并再加一条"伪造 token 也必须 401"的对照：

```
== [1/7] docker build ...                    （apt/COPY 多数 CACHED；#19 编译重跑）
构建耗时: 40s
镜像大小: 107MB (106609714 bytes)

== [3/7] readiness: HTTP 200  body='File service is ready'
== [4/7]
GET /v2/info -> {"authMode":"jwt","buildVersion":"0.1.0","connectedOuterServices":["storage"],"version":"v2"}
authMode = jwt  ✅
GET /v2/files/uploadURL（有 partition 头、**无** Authorization）-> HTTP 401
     body='{"code":401,"message":"Missing authorization token","reason":"Unauthorized"}'
GET /v2/files/uploadURL（伪造 Bearer token）-> HTTP 401
     body='{"code":401,"message":"JWT 格式非法（期望 header.payload.signature 三段）","reason":"Unauthorized"}'
强制 jwt（无 token → 401，伪 token → 401）✅

== [5/7] docker exec id -u = 10001 (fss)
== [6/7] Health.Status = healthy ✅
== [7/7] auth.mode=disabled -> exit 64；空 FSS_JWT_HMAC_SECRET -> exit 64

C9.8 ✅ 全部断言通过
```

> 第二次的 40 s 是"apt 层缓存 + 编译重跑"（#19 编译 36.1 s）。之所以重跑：两次运行之间
> 同工作区的另一个代理修改了 `src/CMakeLists.txt`（08:37，接上新的 `fss_blob_metered` 目标）
> 与 `src/main/server_main.cpp`（08:38），第二次构建如实编译了**新代码**。
>
> **第三次运行**（改完 Dockerfile 里一处**注释**后，对最终文件做确认）：全部层命中缓存，
> `docker build` **1 s**，镜像 digest 与第二次**相同**（106,609,714 B），全部断言再次通过。
> 即：两次冷/温构建 + 一次全缓存构建的结果一致，验证脚本可重复。

### 4.4 容器启动横幅与进程身份（独立探针运行）

```
$ docker logs <cid>
fss_server 已启动
  bind           : 0.0.0.0:8080
  grpc bind      : disabled（FSS_GRPC_PORT=0）
  base path      : /api/file
  storage driver : posix
  storage root   : /data
  sqlite path    : /data/location.db
  error format   : apperror
  log redact     : 11 个键（FSS_LOG_REDACT_KEYS 可覆盖）
  metrics        : 已接入（含存储计量）
  auth           : jwt（HS256 本地校验）

$ docker exec <cid> grep -E '^(Uid|Gid):' /proc/1/status
Uid:	10001	10001	10001	10001
Gid:	10001	10001	10001	10001
$ docker exec <cid> ps -o user,pid,comm
fss   1 fss_server
```

### 4.5 镜像元数据（`docker image inspect`）

```
User=fss
Entrypoint=["/usr/local/bin/entrypoint.sh"]
ExposedPorts={"8080/tcp":{}}
Volumes={"/data":{}}
Healthcheck={"Test":["CMD","/usr/local/bin/healthcheck.sh"],"Interval":5000000000,
             "Timeout":3000000000,"StartPeriod":10000000000,"Retries":12}
Env=[...,"FSS_AUTH_MODE=jwt","FSS_JWT_HMAC_SECRET=","FSS_TRANSFER_SECRET=",
     "FSS_JWT_ISSUER=","FSS_JWT_AUDIENCE=","FSS_JWT_VERIFY_SIGNATURE=true",
     "FSS_JWT_REQUIRE_PARTITION_CLAIM=true","FSS_DEPLOYMENT_MODE=single",
     "FSS_STORAGE_DRIVER=posix","FSS_STORAGE_ROOT=/data","FSS_HTTP_PORT=8080",
     "FSS_BIND_ADDRESS=0.0.0.0","FSS_GRPC_PORT=0","FSS_POSIX_DURABILITY=per_file"]
Size=106609714
```

> `EXPOSE 8080` 取自 `config/fss.example.json` 的 `server.port = 8080`（仓库当前实际默认端口）。

### 4.6 数据卷真的被写入

宿主侧 `build/image-verify/data/`（`-v ...:/data`）在两次运行后包含容器以 uid 10001 创建的：

```
build/image-verify/data/location.db  location.db-wal  location.db-shm
build/image-verify/data/metadata.db  metadata.db-wal  metadata.db-shm
build/image-verify/data/blobs/            （共 160K）
```

即位置仓储 + 元数据仓储（SQLite）+ blobs 都落在声明的 `VOLUME /data` 下，非 root 用户可写。

---

## 5. 复现命令

```bash
# 一条命令跑完 C9.8 的全部断言（构建 → 启动 → 轮询 readiness → 断言 → 清理）
./scripts/verify_image.sh

# 复用已构建镜像（跳过 build，只跑启动/断言；适合快速回归）
FSS_VERIFY_SKIP_BUILD=1 ./scripts/verify_image.sh

# 换 tag / 换超时 / 自定义 DOCKER_CONFIG
FSS_IMAGE_TAG=fssvrcpp:verify2 FSS_VERIFY_WAIT_SECONDS=120 ./scripts/verify_image.sh
```

手工等价步骤（脚本内部做的事）：

```bash
# 1) 最小上下文（不用 /tmp；仓库 build/ 下的 scratch 目录）
rm -rf build/docker-context && mkdir -p build/docker-context
for p in CMakeLists.txt cmake src proto third_party bench tests config; do
  cp -a "$p" "build/docker-context/$p"
done

# 2) 构建（本沙箱里 ~/.docker 只读，故 DOCKER_CONFIG 指到仓库内）
export DOCKER_CONFIG="$PWD/build/docker-cli"
DOCKER_BUILDKIT=1 docker build -f Dockerfile -t fssvrcpp:verify build/docker-context

# 3) 启动
install -d -m 0777 build/image-verify/data
CID=$(docker run -d -p 127.0.0.1::8080 -v "$PWD/build/image-verify/data:/data" \
        -e FSS_JWT_HMAC_SECRET=<运行期密钥> -e FSS_TRANSFER_SECRET=<运行期密钥> \
        fssvrcpp:verify)
PORT=$(docker port "$CID" 8080/tcp | awk -F: '{print $NF}')

# 4) 断言
curl -s -o /dev/null -w 'readiness=%{http_code}\n' "http://127.0.0.1:$PORT/api/file/v2/readiness_check"
curl -s "http://127.0.0.1:$PORT/api/file/v2/info"                       # authMode=jwt
curl -s -o /dev/null -w 'no-token=%{http_code}\n' \
     -H 'data-partition-id: opendes' "http://127.0.0.1:$PORT/api/file/v2/files/uploadURL"   # 401
docker exec "$CID" id -u                                                # 10001
docker inspect -f '{{.State.Health.Status}}' "$CID"                     # healthy

# 5) 清理
docker stop "$CID" && docker rm "$CID"
```

### 5.1 本环境特有的两点（脚本已内建）

1. **`DOCKER_CONFIG` 必须在工作区内**：buildx 默认把"builder 活动时间"写到
   `~/.docker/buildx/activity/`，本沙箱中该路径只读，`docker build` 会直接报
   `failed to update builder last activity time: ... read-only file system`。
   脚本 `export DOCKER_CONFIG="${REPO_ROOT}/build/docker-cli"` 绕开。
2. **不把仓库根当构建上下文**：仓库根还有 `build/`（≈4.9 GB）与 `build-asan/`（≈7.5 GB）。
   脚本先把**最小上下文**（4.8 MB）复制到 `build/docker-context/`，再 `docker build -f Dockerfile <ctx>`。
   没有新增 `.dockerignore`（不在本次允许新建的文件清单内）；如需直接
   `docker build -t x .`，请预期 daemon 会收到 ~12 GB 上下文。

---

## 6. 诚实声明：哪些是实测、哪些没有

### 6.1 实测（本机真实 docker，非模拟）

* `docker build`（两次，完整日志保留）、镜像大小、构建耗时；
* `docker run` 真实启动、真实进程（`ps` 可见 `fss_server` 为 PID 1）、真实监听 8080；
* `GET /api/file/v2/readiness_check` → 200（轮询得到）；
* `/v2/info` 的 `authMode=jwt`；
* 无 token / 伪造 token 的 `uploadURL` → 401（HTTP 状态 + body 逐字）；
* `docker exec id -u` = 10001、`/proc/1/status` 的 `Uid: 10001`；
* `docker inspect -f '{{.State.Health.Status}}'` = `healthy`；
* entrypoint 对 `auth.mode=disabled` 与空密钥的 fail-closed（exit 64，含拒绝消息）；
* 镜像内 `/usr/local/bin/entrypoint.sh`、`healthcheck.sh` 的 `${...}` **没有被构建期展开**
  （`docker run --entrypoint cat` 逐字比对）；
* 数据卷被非 root 用户真实写入（`location.db` / `metadata.db` / `blobs/`）。

### 6.2 未验证（明确不声称）

* **编排平台**：未在真实 Kubernetes / containerd / Nomad 上调度；未验证 `runAsUser`/`fsGroup`/
  SecurityContext。**部分已补测（§10）**：Docker 上的 `--read-only`、`--cap-drop=ALL` +
  `no-new-privileges`、Docker **默认 seccomp** 已实测；但 K8s 的 `readOnlyRootFilesystem` /
  restricted PodSecurity 是**等价但非同一实现**（K8s restricted 的 seccomp 是 `RuntimeDefault`，
  ≈ 但**不等价于** Docker 默认 profile，更不等价于自建/更严 profile）。AGENTS.md 铁律 R8 仍然成立：
  **目标集群上必须再验一次**。
* **存储卷**：只用宿主目录 bind mount（mode 0777）与镜像内 `/data`，**未在 NFS/CSI PVC、
  只读卷、配额卷**上验证属主/权限行为。
* **多架构**：只构建 `linux/amd64`（本机 x86_64），**未做** `--platform linux/arm64` 或
  manifest list（无 qemu/binfmt 验证）。**仍未验证**。
* **JWT 正向路径**：✅ **已在容器内跑通（§10）**：脚本用 python3 标准库签发 HS256 token
  （与被测代码无关），在容器里完成 `uploadURL → PUT → POST metadata → downloadURL → GET`
  并比对 SHA-256（每个硬化场景都跑一遍）。**仍未测**：`data-partition-id` 与 token claim
  **不一致**时的 403（宿主 `ctest -L phase8` 已覆盖，容器内未重复）。
* **HEALTHCHECK 的负向对照**：✅ **已补（§10 H6）**：真实服务、探针指向错误地址
  （`FSS_BIND_ADDRESS=127.0.0.2`）→ `Health.Status=unhealthy`、`FailingStreak=2`；
  与 H5 的 `healthy` 构成区分。**仍未测**：进程活着但**存储不可用**时的 readiness
  （本机实测：`/data` 挂成只读时 readiness **仍返回 200** —— readiness 只探元数据仓储，
  见 `operations.md` §8「readiness 细化 未实现」）。
* **镜像安全扫描**：未跑 trivy/grype（本机未安装），无 CVE 报告；且 **Docker registry 不可达**
  （拉不到扫描器镜像）→ **仍未验证**。
* **TLS / 反向代理**：镜像直接暴露明文 8080，未验证与 ingress/TLS 终止的配合。
* **其它运行时形态**：容器内未启用 `FSS_GRPC_PORT`（保持默认 0），未跑 S3 驱动
  （`FSS_STORAGE_DRIVER=s3`）、未接 `remote-entitlements`。
* **容器内完整业务链路**：✅ **已补（§10）**：容器内 upload → 登记（metadata）→ 下载读回
  （SHA-256 比对）已跑通；**仍未测**：容器内的 delete / list / 多租户越权（宿主 `ctest` 已覆盖）。
* **`deployment.mode=multi`**：组合根仍**拒绝启动**（ADR-009 的 PG 形态未交付），镜像沿用该行为。

### 6.3 已知的良性噪声 / 有意偏离

* `docker build` 的 linter 警告 `SecretsUsedInArgOrEnv`（`FSS_AUTH_MODE` /
  `FSS_JWT_HMAC_SECRET` / `FSS_TRANSFER_SECRET`）：对**空占位符**与"模式开关"的误报。
  我们刻意保留空的 `ENV FSS_JWT_HMAC_SECRET=""` 以声明"密钥必须外部注入"，
  因此不改成 `ARG`/`RUN --mount=type=secret`（那会让 `docker run` 无法注入）。
* 并行度 `min(nproc, 8)` 而非 `-j$(nproc)`：见 §3.3，本机 7 GiB RAM 的 OOM 风险；
  编译仅 37 s，不是瓶颈。
* 未新增 `.dockerignore`（不在允许新建的文件清单内）；构建上下文由脚本 stage，见 §5.1。
* 源码快照是**带未提交改动的工作树**：构建期间另一代理正在改 `src/CMakeLists.txt` 与
  `src/main/server_main.cpp`。两次构建都成功，但镜像内容对应的是构建时刻的工作树，
  不是纯 `4a07a877` 提交；若需可复现的字节级镜像，应在 `src/` 稳定后重跑。

---

## 7. 登记项（不阻塞 C9.8，交给后续）

| 项 | 说明 |
| --- | --- |
| 组合根接 JSON 配置 | 镜像里的 `/etc/fss/fss.example.json` 目前只是文档；等组合根改为读 `config/fss.example.json` 后，production 配置应改为"配置文件 + 少量密钥 env"，届时 ENV 强制的机制需要同步更新 |
| `.dockerignore` | 若希望支持仓库根直接 `docker build .`，应新增 `.dockerignore` 排除 `build*/`、`.devpg/`、`.git/` |
| 多架构 | 如需 arm64，需加 `--platform` 与交叉构建（当前工具链为宿主同构） |
| 镜像扫描 | 建议把 trivy/grype 纳入 CI，产出 CVE 基线 |
| 容器内端到端 | ✅ **已交付（本轮 §10）**：`scripts/verify_image.sh` 的 `[H*]` 段在每个硬化场景里跑 "签发 HS256 token → uploadURL → PUT（SignedURL）→ POST metadata → downloadURL → GET → SHA-256 比对"。运行注意：自签数据面 URL 由 `FSS_SELF_BASE_URL` 决定，必须指向**客户端可达**的地址（脚本用固定宿主端口覆盖），否则宿主拿到的是 `http://127.0.0.1:8080/...`（容器内网视图）而不可达 |
| NFS/PVC 属主 | 需在目标环境验证 `fsGroup` 对 10001 的映射 |

---

## 8. 镜像清理（按交付要求）

证据采集完成后已清理本次留下的镜像（`ubuntu:22.04` 保留）：

```
$ docker images --filter reference='fssvrcpp*'
REPOSITORY   TAG       IMAGE ID   CREATED   SIZE        # 空

$ docker rmi fssvrcpp:verify        # 第二/三次构建（digest 相同：28aed588723b）
$ docker rmi 0d720d621eb0           # 第一次构建变为 dangling 的镜像

$ docker images ubuntu:22.04
REPOSITORY   TAG       IMAGE ID       CREATED       SIZE
ubuntu       22.04     bf7f4568d957   2 weeks ago   78.1MB     # 保留

$ docker ps -a --filter name=fssvrcpp      # 无残留容器
```

`build/docker-context/`（4.8 MB 的最小构建上下文）与 `build/image-verify/`（被容器写入的
SQLite/数据目录）留在仓库 `build/` 下作为可复查的现场；`build/` 已被 `.gitignore` 排除。
重跑 `./scripts/verify_image.sh` 会重建镜像，再验证一次约 40 s（apt 层缓存）。

---

## 9. 最终复验（代码冻结后重跑，2026-09-18）

首次验证期间工作区被并发修改（`src/CMakeLists.txt` 接入 `fss_blob_metered`、
`src/main/server_main.cpp` 接入指标注册表与日志脱敏）。**代码冻结后重跑一次** `scripts/verify_image.sh`，
本次镜像对应最终树：

| 检查 | 输出 |
| --- | --- |
| `readiness_check` | `HTTP 200 body='File service is ready'` |
| `/v2/info` | `{"authMode":"jwt","buildVersion":"0.1.0","connectedOuterServices":["storage"],"version":"v2"}` |
| 无 `Authorization` 的 `uploadURL` | `HTTP 401 {"code":401,"message":"Missing authorization token",...}` |
| 伪造 Bearer token | `HTTP 401 {"code":401,"message":"JWT 格式非法（期望 header.payload.signature 三段）",...}` |
| `HEALTHCHECK` | `Health.Status = healthy` |
| `FSS_AUTH_MODE=disabled` | `exit 64: 拒绝启动：本镜像为 production 镜像，FSS_AUTH_MODE 必须为 jwt` |
| 镜像 | `fssvrcpp:verify`，107 MB（106,609,714 B），容器用户 `fss (uid=10001)` |

原始日志：`build/image-verify-final.log`（`EXIT=0`）。

---

## 10. 容器硬化实测（2026-09-19，本轮新增）

> **本节回答的是"容器/部署硬化在本环境到底能测到什么"**，全部数字来自真机 Docker 运行。
>
> ⚠️ **口径说明（重要）**：`AGENTS.md` §0 原先把 `C9.14 / C9.17~C9.22 / C9.24 / C9.26~C9.30`
> 笼统概括为"真实硬件/多进程/**容器类**判据"。逐条核对
> [`docs/04-implementation-plan.md`](../04-implementation-plan.md) 原文后确认：
> **C9.26~C9.30 不是容器硬化判据**（分别是多实例 E2E / NFS 语义 / PG 连接预算 / io_uring 收益复核 /
> `/v2/info` 与指标暴露），**Docker 硬化实测不能替代它们**（逐条状态见 §10.9）。
> 容器硬化对应的判据是 **C9.8**；本节是它的扩展证据。该概括已在本轮更正。

### 10.1 命令、镜像与可复现性

| 项 | 值 |
| --- | --- |
| 命令 | `./scripts/verify_image.sh`（`[0/7]`~`[7/7]` = 原 C9.8 断言 + `[H*]` = 本轮硬化场景） |
| 默认/全量 | 默认约 **53 s**（含缓存构建）；`FSS_VERIFY_IMAGE_FULL=1` 为 **81 s** |
| 本次（本节数字的来源） | `FSS_VERIFY_IMAGE_FULL=1 ./scripts/verify_image.sh` → **rc=0**；全程 **81 s**（`docker build` **1 s**：层全命中缓存，源码未改动）、镜像 **107 MB / 107,174,213 B**、`sha256:2cc45ec3aee7901c415ec97178c9c8b7e5e45ea776c131f00e2db3d8ac88a54c`。同一脚本本轮跑过多次 FULL（含跳构建的调试运行），结论一致；本节只引用**最终一次**（日志 `build/image-verify-hardening-final.log`）的数字，因为它对应**最终脚本** |
| 原始日志 | `build/image-verify-hardening-final.log`（本节所有引文都能在该文件里逐字找到） |
| 耗时的显式开关 | **默认**不跑 `H4b`（1 GiB 流式）与 `H4b2`（pids 下界）→ 约 **50 s**，供 `scripts/run_all_gates.sh` 使用；`FSS_VERIFY_IMAGE_FULL=1` 时追加这两段（+约 35 s） |
| 依赖 | 宿主 `python3`（签发测试 JWT / 解析 JSON）、`curl`、`docker`；**不需要**联网（不拉镜像） |

```bash
# 本节数字的复现命令（会 docker build + run；约 2.5 分钟）
FSS_VERIFY_IMAGE_FULL=1 ./scripts/verify_image.sh
# 只想快速回归（跳过 1 GiB）：约 50 s
./scripts/verify_image.sh
```

### 10.2 汇总表（脚本末尾原样打印）

```
形态                         | readiness | uid    | 上传读回           | 进程峰值RSS | 结论
-------------------------------+-----------+--------+------------------------+-------------+--------
H1 read-only（无 tmpfs）    | 200       | 10001  | PUT=200 GET=200 sha=一致 | 21.5 MiB    | 通过（**不需要**可写 rootfs，也不需要可写 /tmp）
H2 read-only + tmpfs /tmp      | 200       | 10001  | PUT=200 GET=200 sha=一致 | 21.3 MiB    | 通过
H3 cap-drop=ALL + NNP          | 200       | 10001  | PUT=200 GET=200 sha=一致 | 21.4 MiB    | 通过（CapBnd=0/NNP=1）
H4a 128m + pids=64             | 000       | NA     | 未做（容器未就绪） | -           | **失败（真实发现）**：pids=64<66 线程 → 启动即终止 exit 139（非 OOM）
H4b 1 GiB 流式（128m/256pids） | 200       | 10001  | PUT=200 GET=200 sha=一致 | 20.9 MiB    | 通过（5.9s；进程峰值 ≪1 GiB）
H4b2 pids=65 下界            | 000       | -      | -                      | -           | 预期失败（下界）
H4b2 pids=66 下界            | 200       | -      | -                      | -           | 预期成功
H5 healthcheck+SIGTERM（GC 在跑） | 200       | 10001  | PUT=200 GET=200 sha=一致 | -           | 优雅退出 exit=0，0.30s < 10s grace，无残留线程迹象
H6 健康检查负控（错地址） | unhealthy | -      | -                      | -           | unhealthy（FailingStreak=2）→ 与 H5 的 healthy 构成区分
H7a 对照①无 --read-only   | 200       | 10001  | PUT=200 GET=200 sha=一致 | 21.9 MiB    | /tmp 可写 → 差异确实来自 --read-only
H7b 对照②默认 caps       | 200       | 10001  | -                      | -           | CapBnd 非零 / NNP=0 → H3 的 CapBnd=0 / NNP=1 确实来自那两个开关
H8 默认 seccomp + 暴露面  | 200       | 10001  | PUT=200 GET=200 sha=一致 | -           | 端点行为不变 ✅；指标暴露 fss_io_engine ✅；/v2/info 字段=未交付
```

> 表头"进程峰值RSS"= 传输后的 `/proc/1/status` `VmHWM`（KiB/1024），**不是** cgroup `memory.peak`
> —— 两者含义不同，见 §10.5。

### 10.3 逐场景：命令 + 真实输出

公共部分（脚本 `start_hardened`）：每个场景单独起一个容器，
`docker run -d -p 127.0.0.1:<空闲端口>:8080 -v <每场景独立数据目录>:/data
-e FSS_SELF_BASE_URL=http://127.0.0.1:<端口>/api/file -e FSS_JWT_HMAC_SECRET=<测试密钥>
-e FSS_TRANSFER_SECRET=<测试密钥> <场景参数> fssvrcpp:verify`；
**测试 JWT 由 python3 标准库独立签发**（HS256、`roles=[service.file.editors, service.file.viewers]`、
`data-partition-id=opendes`、`exp=now+3600`），不经过被测代码。
每个场景断言完成后**立即** `docker rm -f`（脚本里的 `end_scenario`），脚本退出时 `trap` 再兜底；
本次运行结束后 `docker ps -a --filter name=fss-hard-` **为空**（已核对）。

#### H1 `--read-only`（**不带** `--tmpfs /tmp`）—— 任务要求"先记录真实结果"

```bash
docker run -d --read-only -v <data>:/data -e FSS_SELF_BASE_URL=http://127.0.0.1:<port>/api/file \
  -e FSS_JWT_HMAC_SECRET=<test> -e FSS_TRANSFER_SECRET=<test> -p 127.0.0.1:<port>:8080 fssvrcpp:verify
```

```
   readiness=200  uid=10001
   根文件系统写入探测: touch: cannot touch '/probe-write': Read-only file system
   /tmp 写入探测     : touch: cannot touch '/tmp/probe-write': Read-only file system
   ✅ uid=10001
   ✅ 根文件系统确实只读（touch /probe-write 被 EROFS 拒绝）
   ✅ 上传+读回通过（PUT=200 GET=200 sha=一致）
```

#### H2 `--read-only --tmpfs /tmp:rw,size=64m,mode=1777`

```
   readiness=200  uid=10001  /tmp 写入=OK
   根文件系统写入探测: touch: cannot touch '/probe-write': Read-only file system
   ✅ readiness=200
   ✅ uid=10001
   ✅ tmpfs /tmp 可写（挂上了）
   ✅ 根文件系统仍只读
   ✅ 上传+读回通过（PUT=200 GET=200 sha=一致）
```

#### H3 最小权限 `--cap-drop=ALL --security-opt no-new-privileges`（**Docker 默认 seccomp**）

```
   readiness=200  uid=10001
   CapEff: 0000000000000000;CapBnd: 0000000000000000;NoNewPrivs: 1;Seccomp: 2;Seccomp_filters: 1;
   HostConfig: ["no-new-privileges"] ["ALL"]
   ✅ readiness=200
   ✅ uid=10001
   ✅ CapBnd=0（cap-drop=ALL 生效）
   ✅ NoNewPrivs=1
   ✅ Seccomp=2（Docker **默认** profile 生效；未使用 seccomp=unconfined）
   ✅ SecurityOpt 中无 seccomp=unconfined：["no-new-privileges"] ["ALL"]
   ✅ 上传+读回通过（PUT=200 GET=200 sha=一致）
```

> 未使用 `--security-opt seccomp=unconfined`（脚本另有断言：`SecurityOpt` 里**不得**出现 `unconfined`）。

#### H4a `--memory=128m --pids-limit=64`（任务原文形态）→ **失败，但失败原因不是内存**

```
   readiness=000  ExitCode=139  OOMKilled=false
   日志: terminate called after throwing an instance of 'std::system_error'   what():  Resource temporarily unavailable
```

#### H4b `--memory=128m --pids-limit=256` 下的 **1 GiB** 上传 + 读回

```
   1 GiB 载荷（稀疏，内容全 0）预期 SHA-256 = 49bc20df15e412a64472421e13fe86ff1c5165e18b2afccf160d4dc19fe68a14
   readiness=200  uid=10001
   ✅ 1 GiB 上传+读回通过（PUT=200 GET=200 sha=一致）
   1 GiB 往返耗时=5.9s
   进程峰值 RSS(VmHWM)=20.9 MiB (21404 KiB；传输前 20812 KiB)  docker stats 当前=7.07MiB / 128MiB 5.52% pids=66
   cgroup memory.peak=128.1 MiB（含写 1 GiB 的页缓存；--memory 上限=128.0 MiB） 采样峰值=128.0 MiB
   OOMKilled=false ExitCode=0
   ✅ 进程峰值 RSS 20.9 MiB < 上限 128 MiB ≪ 载荷 1 GiB（流式）
   ✅ cgroup memory.peak 128.1 MiB 未超过上限 + 512 KiB 容差（页缓存被回收而非 OOM）
```

#### H4b2 pids 上限的**实测下界**

```
   --pids-limit=65 → readiness=000 ExitCode=139
   ✅ pids=65 不可用（与下界一致）
   --pids-limit=66 → readiness=200 ExitCode=0
   ✅ pids=66 可用（= 默认线程数下界）
```

#### H5 `--restart=on-failure` + HEALTHCHECK + SIGTERM（**GC 周期调度线程在跑**）

容器按生产 entrypoint 启动，另经 `FSS_CONFIG=/etc/fss/gc.json`（bind mount 的小配置
`{"gc":{"enabled":true,"interval_seconds":2}}`）打开 GC 周期调度：

```
   readiness=200
     gc             : 已启动（间隔 2s，dry_run=true，require_lease_expiry=true，staging_ttl=24h，orphan_grace=72h；每轮立即跑一次再按间隔重复）
   ✅ GC 周期调度确实在跑（退出路径必须 join 它）
   fss_gc_runs_total{mode="dry_run",outcome="ok"} 1
   ✅ 上传+读回通过（PUT=200 GET=200 sha=一致）
   Health.Status=healthy FailingStreak=0 RestartCount=0
   ✅ HEALTHCHECK healthy
   docker stop -t 10: 耗时=0.30s ExitCode=0 OOMKilled=false RestartCount=0 异常迹象行数=0
   ✅ ExitCode=0（SIGTERM 由处理器接管并正常退出，非 143/137）
   ✅ stop 耗时 0.30s < 10s grace（未被超时强杀）
   ✅ OOMKilled=false
   ✅ RestartCount=0（--restart=on-failure 下干净退出不重启）
   ✅ 退出路径无 terminate/core/abort 迹象（GC 调度线程已 join，无死锁）
```

#### H6 健康检查**负控**（真实服务，探针指向错误地址）

```
     bind           : 127.0.0.2:8080
   容器内直接执行探针 → /usr/local/bin/healthcheck.sh: connect: Connection refused
                        ...healthcheck: 无法连接 127.0.0.1:8080
   ✅ 服务**确实在跑**（只是绑定 127.0.0.2，不在探针地址上）
   ✅ 探针在容器内直接执行即失败（证明确实去连了业务端口）
   Health.Status=unhealthy FailingStreak=2 ...
   ✅ 探针变 unhealthy（**不是恒 healthy**）
   ✅ FailingStreak=2 ≥ 2
```

#### H7 反向对照①②、H8 默认 seccomp 与暴露面

```
-- [H7a] 反向对照①：去掉 --read-only ... → 镜像自带 /tmp（mode 1777）应恢复可写
   readiness=200 uid=10001
   touch /tmp/probe-write → OK
   touch /probe-write → touch: cannot touch '/probe-write': Permission denied（非 root 在 / 上本就 EACCES，不具区分力）
   ✅ 去掉 --read-only 后 /tmp 可写 → H1 的 EROFS 确实来自 --read-only
   上传+读回（对照）= PUT=200 GET=200 sha=一致

-- [H7b] 反向对照②：不带 --cap-drop=ALL / no-new-privileges
   readiness=200  CapEff: 0000000000000000;CapBnd: 00000000a80425fb;NoNewPrivs: 0;
   ✅ 默认形态 CapBnd 非零（与 H3 的 0 构成对照）
   ✅ 默认形态 NoNewPrivs=0（与 H3 的 1 构成对照）

-- [H8] 默认 seccomp（io_uring 不可用）下端点行为不变 + 暴露面实测
   readiness=200  Seccomp: 2 / Seccomp_filters: 1
   ✅ io_uring 不可用的部署里 OSDU 端点行为不变（PUT=200 GET=200 sha=一致）
   /metrics: fss_io_engine{engine="blocking",requested="blocking"} 1
   ✅ 指标确实暴露生效引擎
   /v2/info: {"authMode":"jwt","buildVersion":"0.1.0","connectedOuterServices":["storage"],"version":"v2"}
   ✅ 正控：/v2/info 含 version 字段（下面的『没有 ioEngine』不是空集恒真）
   ⚠ [真实发现] /v2/info **未**暴露 ioEngine/ioUringAvailable → **未交付**
```

### 10.4 只读根文件系统的**真实结论**

* **`--read-only` 在本镜像上直接可用，不需要 `--tmpfs /tmp`，也不需要任何其他可写路径。**
  H1（无 tmpfs）readiness **200**、uid **10001**、上传+读回 SHA-256 一致；`touch /probe-write`
  与 `touch /tmp/probe-write` 都返回 **`Read-only file system`（EROFS）** ⇒ 只读确实生效，且
  服务不依赖可写根文件系统。**因此 `Dockerfile` 未做任何改动**（既没有加 `VOLUME`，也没有在
  entrypoint 里准备可写目录）——任务里"若起不来再最小改动"的前提**没有发生**。
* 原因（与实现一致）：进程只写 `FSS_STORAGE_ROOT=/data`（bind mount；镜像里已声明 `VOLUME /data`）
  —— SQLite（`/data/location.db`、`/data/metadata.db`）与 blobs 都在其下；`/tmp` 没有被用到。
* 加了 `--tmpfs /tmp:rw,size=64m,mode=1777`（H2）同样通过，`/tmp` 变为可写、根文件系统仍只读。
  两种形态都断言过 `id -u == 10001`。
* ⚠ 一个**反例**（同一批实测顺带发现）：`--read-only` 且 `/data` 挂成**只读的、空**目录时，
  进程**拒绝启动**：`创建存储根失败: Read-only file system`，`exit 78`（fail-closed，符合预期）。
  而 `/data` 只读但**已有**库文件时 readiness 仍返回 200（readiness 只探元数据仓储；见 §10.8）。

### 10.5 资源上限下的 1 GiB 流式：三个数字必须分开看

| 指标 | 实测 | 含义 |
| --- | --- | --- |
| 1 GiB 往返耗时 | **5.9 s**（PUT + 登记 + GET，含 SHA-256 校验） | 端到端可用性 |
| **进程峰值 RSS**（`/proc/1/status` 的 `VmHWM`） | **20.9 MiB**（21404 KiB；传输前 20812 KiB） | **流式 vs 整体读入**的判据：1 GiB 全程只多了约 **0.6 MiB** |
| cgroup `memory.peak` | **128.1 MiB**（采样峰值 128.0 MiB） | **不是进程占用**：写 1 GiB 时生成的页缓存记在容器 memcg 上，被 `--memory=128m` 逼着回收/回写；`docker stats` 当时显示 `7.07MiB / 128MiB` |

* **结论：流式成立**，且判据本身有区分力 —— 若实现把 1 GiB 整体读入，`--memory=128m` 下必然
  OOM 或失败（H4a 已经证明该 cgroup 上限真的会拦住东西）；实测 `OOMKilled=false`、`ExitCode=0`。
* 载荷用**稀疏文件**（`truncate -s 1073741824`，内容全 0，不占宿主磁盘）：这是**功能/流式**验证，
  不是性能验证（AGENTS.md §4.2 的"稀疏文件不能代表真实磁盘"仍然成立，本节不报吞吐结论）。
* ⚠ **测量陷阱（本轮踩到）**：`curl --data-binary @file` 会把**整个文件读进内存**（1 GiB 时实测
  报 `curl: option --data-binary: out of memory`，rc=2）—— 那样测的是 curl 而不是服务端。
  大文件必须用 `-T`（流式上传）；脚本已改用 `-T` 并在注释里写明。

### 10.6 pids 上限的实测下界与**新登记的部署陷阱**

* 默认 `server.http.worker_threads` = `max(16, 4 × nproc)`（`src/common/http/server.cpp` 的
  `DefaultWorkerThreads`）⇒ 本机 16 核 = **64 个 HTTP 工作线程**，加主/监听线程 ⇒ `ls /proc/1/task | wc -l`
  = **66**（实测）。
* 因此 `--pids-limit=64`（任务原文形态）在**启动期线程创建**就失败：`pthread_create` 返回 `EAGAIN`
  → `std::thread` 抛 `std::system_error` → **未捕获** → `terminate` → 容器 `ExitCode=139`、
  `OOMKilled=false`。**这不是内存不足**（`--memory=128m` 单独跑完全正常，H4b 就是 128m 下跑完 1 GiB）。
* 实测下界：**65 失败 / 66 成功**（恰好等于默认线程数）。
* **建议**：`--pids-limit ≥ 128`（留余量），或显式调小 `server.http.worker_threads`（它是 HTTP
  并发上限；调小会同时降低并发能力）。
* **未修的最小改法（留给后续切片，登记在 `operations.md` §8）**：在 `main()` 顶层 catch
  `std::exception` → 打印一行并返回干净退出码（而不是 `terminate`/139）。本轮**只登记不修**：
  那属于产品代码的退出路径改动，需要独立的自证（R1）与回归。

### 10.7 EROFS 反向对照的**区分力**（R1 自证结论，如实写）

| 对照 | 带 `--read-only`（H1） | 不带（H7a） | 该断言有区分力吗 |
| --- | --- | --- | --- |
| `readiness_check` | 200 | 200 | ❌ **无区分力**（两者都成功） |
| 上传+读回 SHA-256 | 一致 | 一致 | ❌ **无区分力** |
| `touch /tmp/probe-write` | `Read-only file system`（EROFS） | **OK** | ✅ **有区分力**（这才是"只读"的证据） |
| `touch /probe-write` | `Read-only file system`（EROFS） | `Permission denied`（EACCES，因为 `/` 属主 root、进程是 uid 10001） | ⚠ 有区分力**但只体现在 errno/文案**：只看"被拒"会把 EROFS 与 EACCES 混为一谈 |

* `--cap-drop=ALL` 的对照同理：`CapBnd` 从 `00000000a80425fb`（默认）→ `0000000000000000`、
  `NoNewPrivs` 从 `0` → `1`，**这两个字段有区分力**；但 `CapEff` 在两种形态下**都是 0**
  （镜像以非 root 运行，切换用户时有效能力已被清空）⇒ **`--cap-drop=ALL` 不改变进程"当前"能做的事**，
  它缩小的是 **bounding set**（对未来通过 setuid/file-cap 获得能力的路径有意义）。
  因此不能声称"cap-drop 让服务更安全地跑起来了"——只能声称"服务在 **CapBnd=0 + NNP=1** 下正常"。
* 健康检查负控（H6）与正控（H5：`healthy`）构成区分：同一探针在"业务端口不可达"时变 `unhealthy`
  （`FailingStreak=2`），**不是恒 healthy**。

### 10.8 仍未验证（以及各自需要什么环境）

| 项 | 状态 | 需要什么 |
| --- | --- | --- |
| **真实 K8s / containerd / Nomad 调度** | ⬜ **未验证** | 一个真实集群。`readOnlyRootFilesystem: true`、`runAsNonRoot`、`fsGroup`、restricted PodSecurity 与 Docker 的对应项**等价但非同一实现**；K8s restricted 的 seccomp 是 `RuntimeDefault`（≈ Docker 默认 profile），**不等价于**自建/更严 profile（R8：必须在目标环境验） |
| **镜像 CVE 扫描** | ⬜ **未验证** | 本机无 trivy/grype/syft/clair，且 **Docker registry 不可达**（连 4 个镜像站都不通）→ 需要"能拉 registry 的环境 + 扫描器" |
| **多架构（linux/arm64）** | ⬜ **未验证** | 无 qemu/binfmt、无原生 arm64 构建器；本机只构建 `linux/amd64` |
| **NFS / CSI PVC / 只读卷 / 配额卷上的属主与权限** | ⬜ **未验证** | 可挂载的 NFS/CSI；本机只有宿主 bind mount |
| **readiness 的"业务深度"** | ⚠ **已实测其边界**：`/data` 只读但库已存在时 readiness 仍 200 | 需要实现"存储可达性/磁盘水位"细化（`operations.md` §8 登记为未实现） |
| **真实进程但在探针里做写探测** | ⬜ 未做 | 属 readiness 实现改动 |
| **容器内 delete / list / 越权** | ⬜ 未做（本轮只做 upload→登记→下载读回） | 宿主 `ctest -L phase4/8` 已覆盖，容器层未重复 |

### 10.9 与 C9.26~C9.30 的口径关系（逐条，**不混淆"未验证"与"未交付"**）

| 判据 | 原文要求 | 本轮 Docker 硬化实测能否覆盖 | 状态 |
| --- | --- | --- | --- |
| C9.26 多实例端到端 | 2 个真实进程 + 共享 PG + 共享目录 + 崩溃注入 | ❌ 不能（要 PG 运行形态；组合根对 `mode=multi` **exit 78**） | ⬜ **仍未验证**（且 PG 仓储/租约**未交付**） |
| C9.27 NFS 语义 | 目标存储上的 `rename` 原子性 / close-to-open / `fsync`·`syncfs` 耐久性 | ❌ 不能（本机无 root、不能 `mount`，无 NFS 服务端） | ⬜ **仍未验证**（**上生产硬前提**） |
| C9.28 PG 连接预算 | 实例数 × 池上限 ≤ `max_connections`，超限拒绝启动 | ❌ 不能（**无 PG 客户端/连接池实现**；`server.http.max_connections` 是 HTTP 并发上限，不是 PG 预算） | ⬜ **仍未验证 + 未交付** |
| C9.29 io_uring 收益复核 | 目标存储（NVMe/HDD/NFS）上 uring vs 阻塞 | ❌ 不能（`UringIoEngine::enabled()=false`，`storage.io_engine=uring` 一律 exit 78；默认 seccomp 实测 `EPERM`） | ⬜ **仍未验证 + 未交付** |
| C9.30 `/v2/info` 与指标暴露；不允许 io_uring 的部署里端点行为不变 | ①端点行为不变 ②指标暴露 `ioEngine`/`ioUringAvailable` ③`/v2/info` 字段 | **部分能**：① ✅ 已实测（H8：默认 seccomp + 上传读回）；② ⚠ **部分**实测（`fss_io_engine{engine,requested}` 有，**没有** `ioUringAvailable` 指标）；③ ❌ 实测 `/v2/info` **没有** `ioEngine`/`ioUringAvailable` | ① **已实测**；② **部分实测**；③ **未交付** ⇒ 判据**未满足** |

> C9.26~C9.30 的逐条标注同步写进了 [`docs/04-implementation-plan.md`](../04-implementation-plan.md)
> 的对应判据行；运维侧清单同步写进 [`docs/operations.md`](../operations.md) §8。

### 10.10 本轮对既有脚本/文档的附带修正（都已在本次运行中生效）

| 项 | 说明 |
| --- | --- |
| `scripts/verify_image.sh` 的**重复运行** | 原脚本 `rm -rf "${RUN_DIR}"` 在第二次运行时失败：容器以 uid 10001 在 bind mount 里建的 `blobs/<container>` 宿主删不掉 → `Permission denied` → `set -e` 直接把脚本打死（本轮第一次 `FSS_VERIFY_SKIP_BUILD=1` 复跑就撞上）。新增 `wipe_data_dir()`：先 `rm -rf`，若仍有残留就用一个 `--user 0:0` 的**一次性 root 容器**清掉（不联网、不装东西），基础段与硬化段共用 |
| `scripts/verify_image.sh` 的耗时开关 | 新增 `FSS_VERIFY_IMAGE_FULL=1`（1 GiB 流式 + pids 下界），默认跳过，保证 `run_all_gates.sh` 的总时长可控 |
| `docs/operations.md` §8 两处陈旧行 | ①"GC 的 HTTP 端点 → **未实现**"与 C9.31 已交付矛盾（`POST /v2/gc:run` 在 `src/adapters/http/router.cpp` 已实现）→ 改为"已实现（C9.31）"；②"本仓库当前没有 `docs/test-evidence/phase9-image.md`"——该文件早已存在 → 改为指向本文件 |
| `AGENTS.md` §0 P9 行 | 把"真实硬件/多进程/**容器类**判据（C9.14/C9.17~C9.22/C9.24/C9.26~C9.30）"改成按原文分组，并严格区分"未验证"与"未交付"——该概括曾把下一轮 agent 引向"去测容器的 C9.26~C9.30"（本轮即由此触发） |

**没有改动**：`Dockerfile`（只读 rootfs 不需要可写路径）、`config/`（配置键三态保持
**107 / 18 / 31 = 156**，本轮不新增/不修改任何配置键）、`src/`（产品代码）。

### 10.11 父代理独立复跑（不转述子代理）

我用**同一条命令**重新跑了一遍全量场景，核对每个"头条数字"是否可复现：

```
$ FSS_VERIFY_IMAGE_FULL=1 ./scripts/verify_image.sh     # rc=0，原始日志 build/parent-verify-image.log
H1  read-only（无 tmpfs）      readiness=200 uid=10001 PUT/GET 200 sha=一致  进程峰值 21.7 MiB
H4b 1 GiB @ --memory=128m      进程峰值 RSS(VmHWM)=20.7 MiB（子代理实测 20.9）cgroup peak 128.1 MiB
H4b2 --pids-limit=65 → 000 / 66 → 200（下界=默认线程数 66，可复现）
H5  docker stop -t 10          ExitCode=0 / 0.26 s（子代理 0.30 s）/ RestartCount=0
H6  负控                        Health.Status=unhealthy FailingStreak=2（探针输出 Connection refused）
H7a 去掉 --read-only            /tmp touch → OK（证明 H1 的 EROFS 确实来自 --read-only）
H8  /v2/info                    {"authMode":"jwt","buildVersion":"0.1.0","connectedOuterServices":["storage"],"version":"v2"}
                                → **确认没有** ioEngine/ioUringAvailable（正控：version 字段在）
C9.8 + 容器硬化                 ✅ 全部断言通过
```

两次独立运行的差异只在统计噪声内（RSS 20.7/20.9 MiB、stop 0.26/0.30 s），**结论一致**。
另外我核实了子代理对我两处前提的更正，**它是对的**：① `scripts/run_all_gates.sh`
**不调用** `verify_image.sh`/docker（只跑 `ctest -L phaseN` 与少数 `verify_*.sh`），因此本切片
不改变门槛时长；② `C9.26~C9.30` 的原文与"容器硬化"无关（我的规格写错，已按原文改正标注）。
