# 阶段 9 测试证据 —— 容器镜像（判据 **C9.8**）

| 项 | 值 |
| --- | --- |
| 判据 | `docs/04-implementation-plan.md` **C9.8**：容器镜像可构建、可启动、`readiness_check` 通过；镜像包含 production 配置（`auth.mode=jwt` 强制） |
| 状态 | ✅ **满足**（在本机**真实** docker 上构建、启动、断言、清理；非"未验证"） |
| 验证脚本 | `scripts/verify_image.sh`（可执行；`set -euo pipefail` + `trap` 清理） |
| 镜像 | `Dockerfile`（仓库根，多阶段：`builder` / `runtime`，均基于 `ubuntu:22.04`） |
| 环境 | Docker client/server **27.3.1**；Ubuntu 22.04 / g++ 11.4；16 核 / 7 GiB RAM / 无 root（宿主） |
| 源码快照 | git HEAD `4a07a877d601ac1da224a694d6b4a45d3bc2ff9d` + **并发未提交改动**（构建时 `src/CMakeLists.txt` 08:37、`src/main/server_main.cpp` 08:38 被同工作区的另一个代理修改；见 §6 说明） |
| 关键结果 | 冷构建 **452 s** → 镜像 **107 MB**；`readiness_check` **HTTP 200**；`/v2/info` → `authMode=jwt`；无 token 的 `uploadURL` → **401**；容器 uid **10001**；HEALTHCHECK **healthy**；`auth.mode=disabled` / 空密钥 → **exit 64 拒绝启动** |

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
  SecurityContext、未用受限 seccomp/AppArmor、未用 read-only rootfs。AGENTS.md 铁律 R8 要求
  部署层假设必须在目标环境验证——本条**只在"本机 docker 默认运行时"成立**。
* **存储卷**：只用宿主目录 bind mount（mode 0777）与镜像内 `/data`，**未在 NFS/CSI PVC、
  只读卷、配额卷**上验证属主/权限行为。
* **多架构**：只构建 `linux/amd64`（本机 x86_64），**未做** `--platform linux/arm64` 或
  manifest list（无 qemu/binfmt 验证）。
* **JWT 正向路径**：容器里只验证了**拒绝**（无 token / 伪 token）。带合法 HS256 token 的
  放行路径由宿主的 `ctest -L phase8`（`test_auth_matrix` 等）覆盖，**未在容器内**跑通一条
  "签发 token → 上传"的完整链路。
* **HEALTHCHECK 的负向对照**：只观察到 `healthy`；未构造"进程活着但依赖不可用"的场景来
  证明探针能变 `unhealthy`（`readiness_check` 依赖元数据仓储，容器内不易注入故障）。
* **镜像安全扫描**：未跑 trivy/grype（本机未安装），无 CVE 报告。
* **TLS / 反向代理**：镜像直接暴露明文 8080，未验证与 ingress/TLS 终止的配合。
* **其它运行时形态**：容器内未启用 `FSS_GRPC_PORT`（保持默认 0），未跑 S3 驱动
  （`FSS_STORAGE_DRIVER=s3`）、未接 `remote-entitlements`。
* **容器内完整业务链路**：只验证了运维端点 + 一个鉴权拒绝；未在容器内做 upload→metadata→
  download 的端到端 CRUD（宿主 `ctest` 已覆盖，容器层未重复）。
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
| 容器内端到端 | 可在 `scripts/verify_image.sh` 里追加一条"签发 token → uploadURL → 上传 → 下载"的容器内链路，作为 JWT 正向路径的容器侧证据 |
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

## 7. 最终复验（代码冻结后重跑，2026-09-18）

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
