# syntax=docker/dockerfile:1
# =============================================================================
#  fssvrcpp 生产镜像（阶段 9 / 判据 C9.8）
# =============================================================================
#  目标：镜像可构建 → 可启动 → `/api/file/v2/readiness_check` 返回 200；
#        镜像内 production 配置**强制** `auth.mode=jwt`。
#
#  构建上下文（重要）
#  ---------------------------------------------------------------------------
#  需要的路径只有：CMakeLists.txt cmake/ src/ proto/ third_party/ bench/ tests/ config/
#  ⚠️ 仓库根还有 build/（≈4.9 GB）与 build-asan/（≈7.5 GB）。直接以仓库根为上下文
#     会把它们一起算进去。`scripts/verify_image.sh` 先把最小上下文复制到仓库内的
#     `build/docker-context/`（**不用 /tmp** —— 本环境 /tmp 不跨命令共享），再执行：
#         docker build -f Dockerfile build/docker-context
#
#  两个阶段都用**同一个** ubuntu:22.04，因此 ABI 与编译期完全一致：
#      g++ 11.4 / grpc 1.30.2 / protobuf 3.12.4 / openssl 3.0.2 / sqlite 3.37.2
#  （与本项目阶段 0 的受支持环境逐项相同，见 AGENTS.md §1）
# =============================================================================

# -----------------------------------------------------------------------------
#  builder：编译 fss_server（Release）
# -----------------------------------------------------------------------------
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

#  apt 镜像源：本环境实测默认的 archive.ubuntu.com 上 `apt-get update` ≈ 90 s，
#  换成本机 /etc/apt/sources.list 同款的 USTC 后 ≈ 58 s（约省 35%）。
#  只替换 http 主机名，不动发行版/组件；可用 --build-arg UBUNTU_MIRROR=... 覆盖。
ARG UBUNTU_MIRROR=http://mirrors.ustc.edu.cn/ubuntu
RUN set -eux; \
    sed -i \
      -e "s|http://archive.ubuntu.com/ubuntu|${UBUNTU_MIRROR}|g" \
      -e "s|http://security.ubuntu.com/ubuntu|${UBUNTU_MIRROR}|g" \
      /etc/apt/sources.list; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      cmake g++ make pkg-config ca-certificates \
      libssl-dev libsqlite3-dev libcurl4-openssl-dev \
      libgrpc++-dev libgrpc-dev libprotobuf-dev \
      protobuf-compiler protobuf-compiler-grpc \
    ; \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src

# 只复制构建所需路径（顺序：变化少的在前，便于层缓存）
COPY CMakeLists.txt ./
COPY cmake/ ./cmake/
COPY proto/ ./proto/
COPY third_party/ ./third_party/
COPY src/ ./src/
COPY bench/ ./bench/
COPY tests/ ./tests/
COPY config/ ./config/

# 构建 fss_server。命令与 AGENTS.md §8 一致（Release + 单目标）。
#  ★ 并行度默认取 min(nproc, 8) 而不是直接 -j"$(nproc)"：本构建机的宿主是 16 核
#    但只有 7 GiB RAM（可用 ≈5 GiB），AGENTS.md §4.3 已登记"高 -j 会 OOM 杀掉 cc1plus"
#    （sanitizer 构建实测），Release 构建里 server_main.cpp / *.pb.cc 同样是重 TU。
#    需要复现原始命令时：docker build --build-arg FSS_BUILD_JOBS=16 ...
ARG FSS_BUILD_JOBS=8
RUN set -eux; \
    jobs="${FSS_BUILD_JOBS}"; \
    if [ "$(nproc)" -lt "${jobs}" ]; then jobs="$(nproc)"; fi; \
    echo "build jobs = ${jobs} (nproc=$(nproc))"; \
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release; \
    cmake --build build --target fss_server -j"${jobs}"; \
    test -x build/bin/fss_server; \
    echo "--- ldd build/bin/fss_server ---"; \
    ldd build/bin/fss_server

# -----------------------------------------------------------------------------
#  runtime：只装运行时动态库 + 非 root 用户
# -----------------------------------------------------------------------------
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

#  运行时包清单来自 builder 里 `ldd build/bin/fss_server` 的**直接依赖**；
#  apt 会自动补齐传递依赖（libgrpc10 / libgpr10 / libabsl20210324 / libc-ares2 /
#  zlib1g / libstdc++6 / ...）。对应关系（dpkg -S 实测）：
#     libgrpc++.so.1 → libgrpc++1      libprotobuf.so.23 → libprotobuf23
#     libcurl.so.4   → libcurl4        libsqlite3.so.0   → libsqlite3-0
#     libssl.so.3 / libcrypto.so.3     → libssl3
#  ca-certificates 是运行时真实依赖：S3 SigV4 / 远端 Entitlements 走 TLS 时要校验对端。
ARG UBUNTU_MIRROR=http://mirrors.ustc.edu.cn/ubuntu
RUN set -eux; \
    sed -i \
      -e "s|http://archive.ubuntu.com/ubuntu|${UBUNTU_MIRROR}|g" \
      -e "s|http://security.ubuntu.com/ubuntu|${UBUNTU_MIRROR}|g" \
      /etc/apt/sources.list; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      ca-certificates \
      libgrpc++1 libprotobuf23 libcurl4 libsqlite3-0 libssl3 \
    ; \
    rm -rf /var/lib/apt/lists/*

#  非 root 运行：固定 uid/gid 10001（便于在编排里用 fsGroup / runAsUser 对齐）
RUN set -eux; \
    groupadd --gid 10001 fss; \
    useradd --uid 10001 --gid 10001 --no-create-home \
            --home-dir /nonexistent --shell /usr/sbin/nologin fss; \
    install -d -o fss -g fss -m 0750 /data /data/blobs /etc/fss

COPY --from=builder /src/build/bin/fss_server /usr/local/bin/fss_server
# 随镜像提供一份**带注释的**参考配置（人类可读的文档/排障参照）。
# ★ 组合根当前**只读环境变量**，不解析任何 JSON 配置文件（AGENTS.md §0「阶段 4 后续」），
#   因此这份文件不会改变进程行为；生效的是下面 ENTRYPOINT 里的 `FSS_*` 环境变量。
COPY --chown=fss:fss config/fss.example.json /etc/fss/fss.example.json

# -----------------------------------------------------------------------------
#  production 配置：进程内生效的是环境变量（组合根只读 env）
# -----------------------------------------------------------------------------
#  ★ auth.mode=jwt **强制**。这里用 ENV 设默认值，并由 ENTRYPOINT 拒绝任何降级尝试
#    （见 /usr/local/bin/entrypoint.sh 里的「production 形态：强制 auth.mode=jwt」校验）
#    ——否则 `docker run -e FSS_AUTH_MODE=disabled` 就能把生产镜像变成 allow-all。
#  ★ 镜像内**不写任何真实密钥/令牌**：
#      FSS_JWT_HMAC_SECRET / FSS_TRANSFER_SECRET 都留空占位，必须由运行时注入
#      （docker run -e ... / compose secrets / K8s Secret）。留空时进程**拒绝启动**，
#      这是 local_jwt_authorizer 的既有行为（hmac_secret 空 → NotReadyReason）。
#  ★ FSS_JWT_ISSUER / FSS_JWT_AUDIENCE 默认留空 = **不校验 iss/aud**
#      （src/infra/auth/local/local_jwt_authorizer.cpp:200-204 只在非空时比对）。
#      生产必须按平台实际值设置；二者都是非敏感标识，可直接放在部署清单里。
ENV FSS_AUTH_MODE=jwt \
    FSS_JWT_HMAC_SECRET="" \
    FSS_TRANSFER_SECRET="" \
    FSS_JWT_ISSUER="" \
    FSS_JWT_AUDIENCE="" \
    FSS_JWT_VERIFY_SIGNATURE=true \
    FSS_JWT_REQUIRE_PARTITION_CLAIM=true \
    FSS_DEPLOYMENT_MODE=single \
    FSS_STORAGE_DRIVER=posix \
    FSS_STORAGE_ROOT=/data \
    FSS_HTTP_PORT=8080 \
    FSS_BIND_ADDRESS=0.0.0.0 \
    FSS_GRPC_PORT=0 \
    FSS_POSIX_DURABILITY=per_file

# -----------------------------------------------------------------------------
#  entrypoint / healthcheck
# -----------------------------------------------------------------------------
#  healthcheck 用镜像内已有的 bash `/dev/tcp` 探测 readiness（**不额外装 curl**，
#  保持运行时依赖最小）；探测的是免鉴权的运维端点，200 才算 ready。
RUN set -eux; \
    cat > /usr/local/bin/healthcheck.sh <<'HEALTHCHECK_EOF'
#!/usr/bin/env bash
# C9.8 健康探针：GET /api/file/v2/readiness_check，仅当首行含 " 200 " 时退出 0。
set -euo pipefail
port="${FSS_HTTP_PORT:-8080}"
if ! exec 3<>"/dev/tcp/127.0.0.1/${port}"; then
  echo "healthcheck: 无法连接 127.0.0.1:${port}" >&2
  exit 1
fi
printf 'GET /api/file/v2/readiness_check HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' >&3
line=""
IFS= read -r line <&3 || true
printf 'healthcheck: %s\n' "${line}"
case "${line}" in
  *" 200 "*) exit 0 ;;
  *) exit 1 ;;
esac
HEALTHCHECK_EOF
RUN set -eux; \
    cat > /usr/local/bin/entrypoint.sh <<'ENTRYPOINT_EOF'
#!/usr/bin/env bash
# fssvrcpp 生产容器入口：把容器语义翻译成组合根读取的 FSS_* 环境变量，并做 fail-closed 校验。
set -euo pipefail

: "${FSS_STORAGE_ROOT:=/data}"
: "${FSS_HTTP_PORT:=8080}"
: "${FSS_BIND_ADDRESS:=0.0.0.0}"
export FSS_STORAGE_ROOT FSS_HTTP_PORT FSS_BIND_ADDRESS

# 自签数据面 URL 的 <base> 必须**带 base path**，且端口取实际监听端口
# （组合根的默认值按 8080 拼；这里显式覆盖以支持改端口）。
: "${FSS_SELF_BASE_URL:=http://127.0.0.1:${FSS_HTTP_PORT}/api/file}"
export FSS_SELF_BASE_URL

# 1) production 形态：强制 auth.mode=jwt，拒绝任何降级
if [ "${FSS_AUTH_MODE:-}" != "jwt" ]; then
  echo "拒绝启动：本镜像为 production 镜像，FSS_AUTH_MODE 必须为 jwt（当前='${FSS_AUTH_MODE:-}'）。" >&2
  exit 64
fi

# 2) 密钥必须由运行时注入（镜像内只有空占位，不含任何真实密钥）
missing=""
[ -n "${FSS_JWT_HMAC_SECRET:-}" ] || missing="${missing} FSS_JWT_HMAC_SECRET"
[ -n "${FSS_TRANSFER_SECRET:-}" ] || missing="${missing} FSS_TRANSFER_SECRET"
if [ -n "${missing}" ]; then
  echo "拒绝启动：以下密钥为空，必须由运行时注入（-e / compose secrets / K8s Secret）：${missing}" >&2
  exit 64
fi

mkdir -p "${FSS_STORAGE_ROOT}/blobs" 2>/dev/null || true
exec /usr/local/bin/fss_server
ENTRYPOINT_EOF
RUN set -eux; \
    chmod 0755 /usr/local/bin/healthcheck.sh /usr/local/bin/entrypoint.sh; \
    chown root:root /usr/local/bin/healthcheck.sh /usr/local/bin/entrypoint.sh

# 默认端口取自 config/fss.example.json 的 `server.port` = 8080
EXPOSE 8080

# 持久化数据：位置库 / 元数据库 / blobs 都在 FSS_STORAGE_ROOT=/data 下
VOLUME ["/data"]

HEALTHCHECK --interval=5s --timeout=3s --start-period=10s --retries=12 \
  CMD ["/usr/local/bin/healthcheck.sh"]

USER fss
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
