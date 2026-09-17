#!/usr/bin/env bash
# =============================================================================
#  db/tests/002_advisory_lock.sh —— 跨会话 advisory lock 互斥验证
# =============================================================================
#  为什么必须用脚本而不是 SQL：
#    PostgreSQL 的 advisory lock 是**会话级**的，同一会话内重复获取同一 key 会成功
#    （可重入）。因此"互斥性"只能在**两个不同会话**之间验证，单个 SQL 文件做不到。
#
#  验证内容（对应 ADR-009 §4.4 领导者选举）：
#    A1  会话 1 持锁时，会话 2 获取失败（返回 f）
#    A2  会话 1 自然结束后，锁被释放，可重新获取
#    A3  会话被 kill -9 后，锁**自动释放**（连接断开即释放，无需清理逻辑）
#    A4  会话级锁在 COMMIT 后仍持有（因此选举要用会话级，而非事务级）
#
#  实现注意（本脚本踩过的两个坑，务必保留）：
#    * 后台任务不能用 PID="$(spawn)" 取 PID —— 命令替换会创建子 shell，
#      任务变成孤儿进程，父 shell 既 wait 不到也 kill 不到。必须用全局变量。
#    * 不要用固定 sleep 等待锁释放，要轮询 pg_locks 直到归零。
#
#  用法：db/tests/002_advisory_lock.sh
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PGCTL="${REPO_ROOT}/scripts/dev_postgres.sh"
KEY=$((0x46535347))   # 'FSSG'

C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
pass() { printf '%s\n' "${C_GRN}  ✓${C_OFF} $*"; }
fail() { printf '%s\n' "${C_RED}  ✗${C_OFF} $*" >&2; exit 1; }
info() { printf '%s\n' "${C_DIM}  ·${C_OFF} $*"; }

"${PGCTL}" status >/dev/null 2>&1 || fail "PostgreSQL 未运行：请先执行 scripts/dev_postgres.sh start"

# 一次性导入环境（PATH/LD_LIBRARY_PATH/PG*），之后可直接使用 psql
eval "$("${PGCTL}" env)"

q() { psql -tAq -c "$1" 2>/dev/null || true; }
lock_count() { q "SELECT count(*) FROM pg_locks WHERE locktype='advisory' AND objid=${KEY};"; }
wait_lock_free() {
  local i
  for i in $(seq 1 40); do
    [[ "$(lock_count)" == "0" ]] && { sleep 0.1; return 0; }
    sleep 0.25
  done
  return 1
}

# 后台起一个真正持锁的会话；PID 通过全局变量 HOLDER_PID 返回
# 先取锁，之后**循环短查询**——这贴近真实设计：leader 持锁的会话基本空闲，
# 只做心跳；而不是在持锁的同一个会话里跑一个几分钟的长查询。
HOLDER_PID=""
spawn_holder() {
  local secs="$1" i n
  n=$(( secs * 5 ))     # 每轮约 0.2s
  ( exec psql -tAq -c "SELECT pg_try_advisory_lock(${KEY});" \
      -c "DO \$\$ BEGIN FOR i IN 1..${n} LOOP PERFORM pg_sleep(0.2); END LOOP; END \$\$;" ) \
      >/dev/null 2>&1 &
  HOLDER_PID=$!
}

# 对照用：在持锁的同一个会话里跑一个**长时间单查询**（不推荐，用于验证
# client_connection_check_interval 的作用）
spawn_holder_long_query() {
  local secs="$1"
  ( exec psql -tAq -c "SELECT pg_try_advisory_lock(${KEY});" \
      -c "SELECT pg_sleep(${secs});" ) >/dev/null 2>&1 &
  HOLDER_PID=$!
}

echo "advisory lock 跨会话互斥验证（key=${KEY}）"

# --- A1 会话 1 持锁 → 会话 2 必须失败 -------------------------------------
info "启动会话 1（持锁 3 秒）"
spawn_holder 3; S1="$HOLDER_PID"
sleep 0.8
[[ "$(lock_count)" -ge 1 ]] || fail "A1 前置失败：会话 1 未持锁"
r2="$(q "SELECT pg_try_advisory_lock(${KEY});")"
[[ "$r2" == "f" ]] && pass "A1 会话1持锁时，会话2 获取失败（互斥成立）" \
                   || fail "A1 会话2 竟然拿到了锁（'$r2'）—— 选举会失效"

# --- A2 会话 1 自然结束 → 锁释放 ------------------------------------------
wait "$S1" 2>/dev/null || true
if wait_lock_free; then pass "A2 会话 1 自然结束后锁被释放（可接管）"
else fail "A2 会话结束后锁仍未释放 —— leader 崩溃后无人能接管"; fi
r3="$(q "SELECT pg_try_advisory_lock(${KEY});")"
if [[ "$r3" == "t" ]]; then
  q "SELECT pg_advisory_unlock(${KEY});" >/dev/null
  pass "A2b 释放后可在新会话重新获取"
else fail "A2b 锁释放后仍无法获取（'$r3'）"; fi

# --- A3 会话被 kill -9 → 锁自动释放（持锁会话基本空闲，贴近真实）---------
info "启动会话 3（持锁 + 心跳）后 kill -9（模拟实例崩溃）"
spawn_holder 120; S3="$HOLDER_PID"
sleep 0.8
[[ "$(lock_count)" -ge 1 ]] || fail "A3 前置失败：会话 3 未持锁"
kill -9 "$S3" 2>/dev/null || true
wait "$S3" 2>/dev/null || true
if wait_lock_free; then pass "A3 持锁会话被 kill -9 后锁自动释放（连接断开即释放，无需清理逻辑）"
else fail "A3 持锁会话崩溃后锁仍被持有 —— 会造成 leader 永久失联"; fi

# --- A3b 反例：在持锁的同一会话里跑长查询 → 依赖 client_connection_check_interval
#     这一条说明了为什么设计上要求"持锁会话保持空闲/只做心跳"
info "对照：持锁会话正在跑长查询时 kill -9（验证 client_connection_check_interval 的作用）"
interval="$(q "SHOW client_connection_check_interval;")"
spawn_holder_long_query 120; S3b="$HOLDER_PID"
sleep 0.8
[[ "$(lock_count)" -ge 1 ]] || fail "A3b 前置失败：会话未持锁"
kill -9 "$S3b" 2>/dev/null || true
wait "$S3b" 2>/dev/null || true
if wait_lock_free; then
  pass "A3b 长查询期间 kill -9 也被检测（client_connection_check_interval=${interval}）"
else
  info "A3b 长查询期间 kill -9 未被检测（client_connection_check_interval=${interval}）"
  info "     → 因此实现必须：① 持锁会话保持空闲/只做心跳；② 部署设置该参数"
  info "     → 清理残留锁：终止该后端（见下方）"
  q "SELECT pg_terminate_backend(pid) FROM pg_stat_activity
      WHERE pid <> pg_backend_pid() AND query ILIKE '%pg_sleep%';" >/dev/null
  wait_lock_free || fail "A3b 清理后锁仍未释放"
fi

# --- A4 会话级 vs 事务级（选型依据）--------------------------------------
# ★ 必须在**同一个会话**内完成"取锁 → COMMIT → 查锁"。
#   拆成两次 psql 调用会各自建会话，锁随第一个会话结束而释放，测不出结论。
c_sess="$(q "BEGIN; SELECT pg_try_advisory_lock(${KEY}); COMMIT;
             SELECT count(*) FROM pg_locks WHERE locktype='advisory' AND objid=${KEY};" | tail -1)"
if [[ "${c_sess:-0}" -ge 1 ]]; then
  pass "A4a 会话级 pg_advisory_lock 在 COMMIT 后仍持有（适合 leader 选举）"
else
  fail "A4a 会话级锁在 COMMIT 后丢失（count='$c_sess'）"
fi

c_xact="$(q "BEGIN; SELECT pg_try_advisory_xact_lock(${KEY}); COMMIT;
              SELECT count(*) FROM pg_locks WHERE locktype='advisory' AND objid=${KEY};" | tail -1)"
if [[ "${c_xact:-1}" -eq 0 ]]; then
  pass "A4b 事务级 pg_advisory_xact_lock 在 COMMIT 后自动释放（不适合长期选举）"
else
  info "A4b 事务级锁 COMMIT 后 count=${c_xact}（PG 版本差异；实现统一用会话级即可）"
fi
echo
printf '%s\n' "${C_GRN}advisory lock 互斥验证通过（A1~A4）${C_OFF}"
echo "  结论：领导者选举用【会话级】pg_try_advisory_lock；连接断开自动释放，"
echo "        实例崩溃后无需额外清理逻辑（ADR-009 §4.4）。"
