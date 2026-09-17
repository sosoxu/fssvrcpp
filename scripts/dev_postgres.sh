#!/usr/bin/env bash
# =============================================================================
#  scripts/dev_postgres.sh —— 开发/测试用的 PostgreSQL 基建（无需 root）
# =============================================================================
#
#  为什么需要它
#  ---------------------------------------------------------------------------
#  多实例一致性相关的门槛测试（C2.10 / C6.11~C6.13 / C8.9~C8.10 / C9.26）
#  必须有**真实 PostgreSQL** 才能验证：唯一约束的冲突语义、`ON CONFLICT ... RETURNING`
#  的原子性、`pg_try_advisory_lock` 的会话锁、PG 侧 `now()` 的时间基准。
#  用 SQLite 或 mock 替代会让这些测试失去意义（详见 ADR-009）。
#
#  本机约束（实测）：无 root（sudo 需密码）、github 直连不可达、apt 镜像可用。
#  因此二进制通过 `apt-get download` + `dpkg-deb -x` 解包到工作目录，以普通用户运行。
#
#  设计原则
#  ---------------------------------------------------------------------------
#  * **幂等**：重复 `start` 不会失败；重复 `migrate` 不会重复迁移。
#  * **可清理**：`reset` 一键回到干净状态；`stop` 后数据仍在。
#  * **可发现**：`env` / `dsn` 输出供测试与 CI 使用的环境变量。
#  * **不做生产用途**：使用 trust 认证、监听 127.0.0.1、数据目录在工作区内。
#      → 生产部署请使用托管 PG 或正规集群；本脚本仅用于开发与测试。
#
#  用法
#  ---------------------------------------------------------------------------
#     scripts/dev_postgres.sh start      # 启动（必要时先初始化 + 应用迁移）
#     scripts/dev_postgres.sh stop       # 停止（保留数据）
#     scripts/dev_postgres.sh status     # 查看状态与连接信息
#     scripts/dev_postgres.sh restart
#     scripts/dev_postgres.sh reset      # 停止并删除数据目录，重新初始化 + 迁移
#     scripts/dev_postgres.sh migrate    # 仅应用未应用的迁移
#     scripts/dev_postgres.sh psql [args...]   # 进入 psql（可带 SQL）
#     scripts/dev_postgres.sh dsn        # 打印 DSN（供 FSS_PG_DSN 使用）
#     scripts/dev_postgres.sh env        # 打印可 eval 的 export（PG* 与 FSS_PG_DSN）
#     scripts/dev_postgres.sh logs       # 打印服务日志
#     scripts/dev_postgres.sh destroy    # 停止并删除所有数据（含解包的二进制缓存）
#
#  环境变量
#  ---------------------------------------------------------------------------
#    FSS_PG_ROOT        数据与二进制的根目录（默认 <repo>/.devpg）
#    FSS_PG_PORT        端口（默认 15432，避开系统默认 5432）
#    FSS_PG_USER        超级用户名（默认 fss）
#    FSS_PG_DB          应用数据库名（默认 fss）
#    FSS_PG_BIN         直接指定 bin 目录（含 pg_ctl/initdb/psql），跳过自动发现
#    FSS_PG_VERSION     要下载的 apt 包版本后缀（默认 14；即 postgresql-14）
#    FSS_PG_SOCKET_DIR  Unix socket 目录（默认 <FSS_PG_ROOT>/run）
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PGROOT="${FSS_PG_ROOT:-${REPO_ROOT}/.devpg}"
PGPORT="${FSS_PG_PORT:-15432}"
PGUSER="${FSS_PG_USER:-fss}"
PGDB="${FSS_PG_DB:-fss}"
PGVER="${FSS_PG_VERSION:-14}"
SOCKDIR="${FSS_PG_SOCKET_DIR:-${PGROOT}/run}"
DATADIR="${PGROOT}/data"
BINDIR="${FSS_PG_BIN:-${PGROOT}/pgbin/usr/lib/postgresql/${PGVER}/bin}"
LOGFILE="${PGROOT}/postgres.log"
CONFDIR="${PGROOT}/conf.d"
MIGRATIONS_DIR="${REPO_ROOT}/db/migrations"

C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
info() { printf '%s\n' "${C_DIM}·${C_OFF} $*"; }
ok()   { printf '%s\n' "${C_GRN}✓${C_OFF} $*"; }
warn() { printf '%s\n' "${C_YEL}!${C_OFF} $*" >&2; }
die()  { printf '%s\n' "${C_RED}✗${C_OFF} $*" >&2; exit 1; }

# -----------------------------------------------------------------------------
#  二进制发现：顺序为 ①显式指定 ②系统 PATH ③本地缓存 ④apt 解包
# -----------------------------------------------------------------------------
have_binaries() { [[ -x "$1/pg_ctl" && -x "$1/initdb" && -x "$1/psql" ]]; }

resolve_binaries() {
  if have_binaries "${BINDIR}"; then return 0; fi

  # ② 系统已安装
  for d in /usr/lib/postgresql/*/bin /usr/local/pgsql/bin /opt/homebrew/opt/postgresql*/bin; do
    if [[ -d "$d" ]] && have_binaries "$d"; then BINDIR="$d"; return 0; fi
  done
  if command -v pg_ctl >/dev/null 2>&1; then
    local d; d="$(dirname "$(command -v pg_ctl)")"
    if have_binaries "$d"; then BINDIR="$d"; return 0; fi
  fi

  # ④ apt 解包（无需 root）
  info "本地无 PostgreSQL 二进制，从 apt 镜像解包到 ${PGROOT}/pgbin（无需 root）"
  mkdir -p "${PGROOT}/pgbin" "${PGROOT}/debs"
  local tmpd; tmpd="$(mktemp -d)"
  (
    cd "$tmpd"
    local pkgs=( "postgresql-${PGVER}" "postgresql-client-${PGVER}" "libpq5" )
    for p in "${pkgs[@]}"; do
      apt-get download "$p" >/dev/null 2>&1 || die "apt-get download $p 失败（检查 apt 源是否可用）"
    done
    for f in *.deb; do dpkg-deb -x "$f" "${PGROOT}/pgbin/"; done
  )
  rm -rf "$tmpd"
  have_binaries "${BINDIR}" || die "解包后仍未找到 pg_ctl/initdb/psql（期望在 ${BINDIR}）"
  ok "二进制已就绪：${BINDIR}"
}

# 解包出来的 libpq 等共享库需要 LD_LIBRARY_PATH
pg_env() {
  export PATH="${BINDIR}:${PATH}"
  local libdir
  for libdir in "${PGROOT}/pgbin/usr/lib/x86_64-linux-gnu" "${PGROOT}/pgbin/usr/lib"; do
    [[ -d "$libdir" ]] && export LD_LIBRARY_PATH="${libdir}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
  done
  export PGHOST=127.0.0.1 PGPORT="${PGPORT}" PGUSER="${PGUSER}" PGDATABASE="${PGDB}"
  export PGCONNECT_TIMEOUT=5
}

dsn_string() { printf 'postgresql://%s@127.0.0.1:%s/%s' "${PGUSER}" "${PGPORT}" "${PGDB}"; }

is_running() {
  [[ -f "${DATADIR}/postmaster.pid" ]] || return 1
  pg_ctl -D "${DATADIR}" status >/dev/null 2>&1
}

wait_ready() {
  local i
  for i in $(seq 1 60); do
    if pg_isready -h 127.0.0.1 -p "${PGPORT}" -U "${PGUSER}" -q 2>/dev/null; then return 0; fi
    sleep 0.25
  done
  return 1
}

# -----------------------------------------------------------------------------
#  初始化 + 迁移
# -----------------------------------------------------------------------------
do_init() {
  [[ -f "${DATADIR}/PG_VERSION" ]] && return 0
  info "initdb → ${DATADIR}"
  mkdir -p "${DATADIR}" "${SOCKDIR}"
  # trust 认证仅用于本地开发；生产部署不得使用
  initdb -D "${DATADIR}" -U "${PGUSER}" --auth=trust -E UTF8 --no-locale >/dev/null
  # 让 postgresql.conf 包含我们的开发配置目录（只追加一次）
  if ! grep -q "fss dev include_dir" "${DATADIR}/postgresql.conf"; then
    {
      echo ""
      echo "# --- fss dev include_dir（由 scripts/dev_postgres.sh 维护）---"
      echo "include_dir = '${CONFDIR}'"
    } >> "${DATADIR}/postgresql.conf"
  fi
  ok "initdb 完成"
}

# -----------------------------------------------------------------------------
#  开发/测试专用配置：**每次 start 都重写**，保证改动总能生效
#  （早期版本只在 initdb 时追加一次，导致改配置后 restart 不生效）
# -----------------------------------------------------------------------------
write_dev_conf() {
  mkdir -p "${CONFDIR}"
  cat > "${CONFDIR}/10-fss-dev.conf" <<'CONF'
# 由 scripts/dev_postgres.sh 生成 —— 开发/测试专用，请勿用于生产
# 关闭 fsync 类开销以加速测试：测试关注的是**约束与并发语义**，
# 掉电耐久性由 ADR-008 在真实存储上单独验证。
fsync = off
synchronous_commit = off
full_page_writes = off
max_connections = 200
log_min_messages = warning

# ★ 关键：默认 client_connection_check_interval=0 时，客户端被 kill -9 后，
#   正在执行**长查询**的后端不会察觉连接已断，会话级 advisory lock 会一直被
#   持有到查询结束 —— 对领导者选举是致命的（实例崩溃后无人能接管）。
#   设为 1s 后，后端在长查询期间也会检测到客户端断开并释放锁。
#   生产部署若用 advisory lock 做选举，必须同样设置；并且设计上要求
#   【持锁会话保持空闲或只做心跳】，不要在同一会话里跑长查询（ADR-009 §4.4）。
client_connection_check_interval = 1s
CONF
}

do_migrate() {
  mkdir -p "${DATADIR}" "${SOCKDIR}"
  [[ -d "${MIGRATIONS_DIR}" ]] || die "找不到迁移目录 ${MIGRATIONS_DIR}"
  shopt -s nullglob
  local files=( "${MIGRATIONS_DIR}"/*.sql )
  shopt -u nullglob
  [[ ${#files[@]} -gt 0 ]] || die "${MIGRATIONS_DIR} 下没有 *.sql 迁移文件"
  local applied=0
  # 确保 schema_migrations 存在（第一条迁移里也会建，这里先建以便查询）
  psql -q -v ON_ERROR_STOP=1 -c "
    CREATE TABLE IF NOT EXISTS schema_migrations(
      version INTEGER PRIMARY KEY, name TEXT NOT NULL,
      applied_at TIMESTAMPTZ NOT NULL DEFAULT now());" >/dev/null

  local f base ver
  for f in $(printf '%s\n' "${files[@]}" | sort); do
    base="$(basename "$f")"
    ver=$((10#${base%%_*}))   # 001 -> 1；10# 前缀避免 08/09 被当作八进制
    if [[ -n "$(psql -tAq -c "SELECT 1 FROM schema_migrations WHERE version=${ver};")" ]]; then
      info "迁移 ${base} 已应用，跳过"
      continue
    fi
    info "应用迁移 ${base}"
    # 整个迁移文件在单个事务中执行；迁移文件本身也写幂等语句
    if ! psql -q -v ON_ERROR_STOP=1 --single-transaction -f "$f" >/dev/null; then
      die "迁移 ${base} 失败"
    fi
    psql -q -v ON_ERROR_STOP=1 -c \
      "INSERT INTO schema_migrations(version,name) VALUES(${ver},'${base}') ON CONFLICT DO NOTHING;" >/dev/null
    applied=$((applied+1))
  done
  ok "迁移完成（本次应用 ${applied} 个）"
}

do_start() {
  resolve_binaries; pg_env
  mkdir -p "${PGROOT}" "${CONFDIR}"
  write_dev_conf
  do_init
  if is_running; then ok "已在运行（port=${PGPORT}, datadir=${DATADIR}）"; return 0; fi
  # 清理陈旧 pid 文件（例如上次被 kill -9）
  if [[ -f "${DATADIR}/postmaster.pid" ]]; then
    warn "发现陈旧的 postmaster.pid，尝试清理"
    rm -f "${DATADIR}/postmaster.pid"
  fi
  info "启动 PostgreSQL（port=${PGPORT}, socket=${SOCKDIR}）"
  mkdir -p "${SOCKDIR}"
  if ! pg_ctl -D "${DATADIR}" -l "${LOGFILE}" \
       -o "-p ${PGPORT} -k ${SOCKDIR} -c listen_addresses=127.0.0.1" \
       -w -t 30 start >/dev/null 2>&1; then
    warn "启动失败，日志尾部："; tail -15 "${LOGFILE}" >&2 || true
    die "pg_ctl start 失败"
  fi
  wait_ready || { warn "等待就绪超时，日志尾部："; tail -15 "${LOGFILE}" >&2; die "服务未就绪"; }
  # 应用数据库：默认不存在则创建
  if [[ -z "$(psql -d postgres -tAq -c "SELECT 1 FROM pg_database WHERE datname='${PGDB}';")" ]]; then
    createdb -h 127.0.0.1 -p "${PGPORT}" -U "${PGUSER}" "${PGDB}"
    ok "已创建数据库 ${PGDB}"
  fi
  ok "PostgreSQL 已启动并就绪"
  do_migrate
  print_conn_info
}

do_stop() {
  resolve_binaries; pg_env
  if ! is_running; then ok "未在运行"; return 0; fi
  info "停止 PostgreSQL"
  if ! pg_ctl -D "${DATADIR}" -m fast -w -t 30 stop >/dev/null 2>&1; then
    warn "pg_ctl stop 失败，尝试强制终止"
    pg_ctl -D "${DATADIR}" -m immediate -w -t 15 stop >/dev/null 2>&1 || true
  fi
  ok "已停止（数据保留在 ${DATADIR}）"
}

do_reset() {
  do_stop
  info "删除数据目录 ${DATADIR}"
  rm -rf "${DATADIR}"
  do_start
}

do_destroy() {
  do_stop
  info "删除 ${PGROOT}（含数据与解包的二进制）"
  rm -rf "${PGROOT}"
  ok "已清理"
}

print_conn_info() {
  printf '  %-12s %s\n' "DSN"      "$(dsn_string)"
  printf '  %-12s %s\n' "psql"     "PGHOST=127.0.0.1 PGPORT=${PGPORT} PGUSER=${PGUSER} PGDATABASE=${PGDB} psql"
  printf '  %-12s %s\n' "socket"   "${SOCKDIR}/.s.PGSQL.${PGPORT}"
  printf '  %-12s %s\n' "日志"     "${LOGFILE}"
  printf '  %-12s %s\n' "数据目录" "${DATADIR}"
}

do_status() {
  resolve_binaries; pg_env
  if is_running; then
    local ver tables
    ver="$(psql -tAq -c 'SHOW server_version;' 2>/dev/null || echo '?')"
    tables="$(psql -tAq -c "SELECT count(*) FROM information_schema.tables WHERE table_schema='public';" 2>/dev/null || echo '?')"
    ok "运行中：PostgreSQL ${ver}，public 表数 ${tables}"
    print_conn_info
  else
    warn "未运行（datadir=${DATADIR}）"
    [[ -f "${DATADIR}/PG_VERSION" ]] && info "数据目录已初始化，可直接 start" || info "尚未初始化，start 会自动 initdb"
    return 1
  fi
}

do_env() {
  # 同时导出 PATH 与 LD_LIBRARY_PATH，使 `eval "$(scripts/dev_postgres.sh env)"`
  # 之后可以直接使用 psql/pg_ctl（解包出来的二进制不在系统 PATH 上）
  local libdirs=""
  local d
  for d in "${PGROOT}/pgbin/usr/lib/x86_64-linux-gnu" "${PGROOT}/pgbin/usr/lib"; do
    [[ -d "$d" ]] && libdirs="${libdirs}${libdirs:+:}${d}"
  done
  cat <<EOF
export FSS_PG_ROOT='${PGROOT}'
export FSS_PG_DSN='$(dsn_string)'
export FSS_PG_PORT='${PGPORT}'
export FSS_PG_BIN='${BINDIR}'
export PGHOST=127.0.0.1
export PGPORT='${PGPORT}'
export PGUSER='${PGUSER}'
export PGDATABASE='${PGDB}'
export PGCONNECT_TIMEOUT=5
export PATH='${BINDIR}':"\${PATH:-}"
${libdirs:+export LD_LIBRARY_PATH='${libdirs}':"\${LD_LIBRARY_PATH:-}"}
EOF
}

usage() {
  sed -n '2,50p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# -----------------------------------------------------------------------------
#  入口
# -----------------------------------------------------------------------------
cmd="${1:-help}"; shift || true
case "$cmd" in
  start)    do_start ;;
  stop)     do_stop ;;
  restart)  do_stop; do_start ;;
  reload)   resolve_binaries; pg_env; is_running || die "服务未运行"; write_dev_conf; pg_ctl -D "${DATADIR}" reload >/dev/null && ok "已 reload 配置" ;;
  status)   do_status ;;
  reset)    do_reset ;;
  destroy)  do_destroy ;;
  migrate)  resolve_binaries; pg_env; is_running || die "服务未运行，请先 start"; do_migrate ;;
  psql)     resolve_binaries; pg_env; if [[ $# -gt 0 ]]; then psql "$@"; else psql; fi ;;
  dsn)      dsn_string; echo ;;
  env)      do_env ;;
  logs)     [[ -f "${LOGFILE}" ]] && tail -n "${1:-50}" "${LOGFILE}" || die "无日志文件 ${LOGFILE}" ;;
  help|-h|--help) usage ;;
  *) die "未知子命令：${cmd}（用 help 查看用法）" ;;
esac
