#!/usr/bin/env bash
# =============================================================================
#  db/tests/003_concurrent_claim.sh —— 真并发"原子领取"验证（幂等性的核心门槛）
# =============================================================================
#  这是 ADR-009 §3 M2 的**真实 schema 版**对照测试：
#    naive 模式：check-then-insert（无唯一约束保护）→ 应出现重复
#    fixed 模式：唯一索引 + ON CONFLICT ... DO NOTHING RETURNING → 应恰好 1 个赢家
#
#  与之前一次性探针的区别：这里用的是**迁移文件里的真实 schema/索引**，
#  不是临时表。因此本脚本同时验证了 schema 与协议。
#
#  验证内容：
#    C1  fixed：N 个并发会话领取同一 fileSource → 恰好 1 个拿到 id
#    C2  fixed：重试（已存在活跃记录时再领取）→ 拿到 0 行，且不产生新记录
#    C3  naive：去掉唯一索引后同样并发 → **必须**出现多个赢家（自证检测器有效）
#    C4  fixed：软删除后可以重新领取（部分索引 WHERE 谓词生效）
#
#  用法：db/tests/003_concurrent_claim.sh [轮数，默认 10] [并发数，默认 8]
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PGCTL="${REPO_ROOT}/scripts/dev_postgres.sh"
ROUNDS="${1:-10}"
WORKERS="${2:-8}"
PART='claimtest'

C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
pass() { printf '%s\n' "${C_GRN}  ✓${C_OFF} $*"; }
fail() { printf '%s\n' "${C_RED}  ✗${C_OFF} $*" >&2; exit 1; }
info() { printf '%s\n' "${C_DIM}  ·${C_OFF} $*"; }
warn() { printf '%s\n' "${C_YEL}  !${C_OFF} $*" >&2; }
q() { "${PGCTL}" psql -tAq -c "$1"; }

"${PGCTL}" status >/dev/null 2>&1 || fail "PostgreSQL 未运行：请先执行 scripts/dev_postgres.sh start"

# ---------------------------------------------------------------------------
#  前置条件：幂等键的唯一索引必须存在
#  若上一次运行中途失败（例如 naive 对照留下重复行导致索引重建失败），
#  这里给出**明确**的错误，而不是让后续 SQL 报 "no unique constraint matching"。
# ---------------------------------------------------------------------------
if [[ "$("${PGCTL}" psql -tAq -c "SELECT count(*) FROM pg_indexes WHERE schemaname='public' AND indexname='ux_mr_source';")" != "1" ]]; then
  fail "前置条件不满足：ux_mr_source 不存在。
       请先清理残留数据并重建索引：
         scripts/dev_postgres.sh psql -q -c \"DELETE FROM file_metadata_records WHERE partition_id='claimtest';\"
         scripts/dev_postgres.sh psql -q -c \"CREATE UNIQUE INDEX IF NOT EXISTS ux_mr_source
           ON file_metadata_records (partition_id, file_source)
           WHERE state <> 'deleted' AND is_latest;\""
fi

# 注意顺序：必须**先删掉测试数据**（naive 对照会留下重复行），
# 否则重建唯一索引会因 "is duplicated" 失败。
cleanup() {
  q "DELETE FROM file_metadata_records WHERE partition_id='${PART}';" >/dev/null 2>&1 || true
  q "DROP INDEX IF EXISTS ux_mr_source;" >/dev/null 2>&1 || true
  q "CREATE UNIQUE INDEX IF NOT EXISTS ux_mr_source
       ON file_metadata_records (partition_id, file_source)
       WHERE state <> 'deleted' AND is_latest;" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
#  单轮并发领取：启动 WORKERS 个独立 psql 进程，全部尝试领取同一 fileSource
#  返回：赢家数量
# ---------------------------------------------------------------------------
# 所有 worker 都等到同一个墙钟时刻再执行 INSERT —— 否则 psql 启动开销
# （几十毫秒）会把它们串行化，竞态窗口消失，测试变成"假通过"。
FUTURE_AT=""
prepare_common_start() { FUTURE_AT="$(q "SELECT to_char(clock_timestamp() + interval '1.5 second', 'YYYY-MM-DD HH24:MI:SS.US');")"; }

#  one_round <fileSource> <outdir> <fixed|naive>
#    fixed：唯一索引 + ON CONFLICT DO NOTHING RETURNING  → 原子领取
#    naive：check-then-insert（INSERT ... WHERE NOT EXISTS）→ 无约束保护
#  返回：成功写入（即"赢得领取权"）的会话数
one_round() {
  local fs="$1" outdir="$2" mode="${3:-fixed}"
  rm -rf "$outdir"; mkdir -p "$outdir"
  local i sql pids=()
  if [[ "$mode" == "fixed" ]]; then
    sql="INSERT INTO file_metadata_records
           (partition_id,id,version,kind,state,is_latest,created_by,file_source,data)
         VALUES ('${PART}','rec-IDX-RAND',1,'k','claiming',TRUE,'instIDX','${fs}','{}'::jsonb)
         ON CONFLICT (partition_id,file_source) WHERE state <> 'deleted' AND is_latest
         DO NOTHING
         RETURNING id;"
  else
    # 经典 check-then-act：单条语句里先查后插，但**没有唯一索引兜底**
    sql="INSERT INTO file_metadata_records
           (partition_id,id,version,kind,state,is_latest,created_by,file_source,data)
         SELECT '${PART}','rec-IDX-RAND',1,'k','claiming',TRUE,'instIDX','${fs}','{}'::jsonb
          WHERE NOT EXISTS (
                 SELECT 1 FROM file_metadata_records
                  WHERE partition_id='${PART}' AND file_source='${fs}'
                    AND state <> 'deleted' AND is_latest)
         RETURNING id;"
  fi
  for ((i=0; i<WORKERS; i++)); do
    (
      local s="${sql//IDX/$i}"; s="${s//RAND/$RANDOM}"
      # 先连上并等到共同起点，再执行插入 → 真正同时发生
      "${PGCTL}" psql -tAq -v ON_ERROR_STOP=0 \
        -c "SELECT pg_sleep(GREATEST(0, EXTRACT(EPOCH FROM TIMESTAMPTZ '${FUTURE_AT}') - EXTRACT(EPOCH FROM clock_timestamp())));" \
        -c "$s" > "$outdir/w$i.out" 2>"$outdir/w$i.err" || true
    ) &
    pids+=($!)
  done
  for p in "${pids[@]}"; do wait "$p" 2>/dev/null || true; done
  local winners=0 f
  for f in "$outdir"/w*.out; do
    # 只统计最后一行有内容（= 真的插入了）的会话
    [[ -n "$(tr -d '\n' < "$f")" ]] && winners=$((winners+1))
  done
  echo "$winners"
}

echo "原子领取并发验证：${ROUNDS} 轮 × ${WORKERS} 个并发会话（真实迁移 schema）"

# ---------------------------------------------------------------------------
# C1 fixed：恰好 1 个赢家
# ---------------------------------------------------------------------------
bad1=0
for ((r=0; r<ROUNDS; r++)); do
  q "DELETE FROM file_metadata_records WHERE partition_id='${PART}';" >/dev/null
  fs="/osdu-user/staging/claim-${r}"
  prepare_common_start
  w="$(one_round "$fs" "/tmp/fss_claim_${r}" fixed)"
  rows="$(q "SELECT count(*) FROM file_metadata_records WHERE partition_id='${PART}' AND file_source='${fs}';")"
  if [[ "$w" != "1" || "$rows" != "1" ]]; then
    bad1=$((bad1+1))
    warn "第 ${r} 轮：赢家=${w} 记录数=${rows}（期望各为 1）"
  fi
done
if [[ $bad1 -eq 0 ]]; then pass "C1 ${ROUNDS} 轮全部恰好 1 个赢家、1 条记录（幂等成立）"
else fail "C1 有 ${bad1}/${ROUNDS} 轮不满足（赢家或记录数 != 1）"; fi

# ---------------------------------------------------------------------------
# C2 fixed：已存在活跃记录时再领取 → 0 行，且记录数不变
# ---------------------------------------------------------------------------
fs="/osdu-user/staging/retry"
q "DELETE FROM file_metadata_records WHERE partition_id='${PART}';" >/dev/null
q "INSERT INTO file_metadata_records(partition_id,id,version,kind,state,is_latest,created_by,file_source,data)
   VALUES ('${PART}','rec-first',1,'k','ready',TRUE,'inst0','${fs}','{}'::jsonb);" >/dev/null
again="$(q "INSERT INTO file_metadata_records(partition_id,id,version,kind,state,is_latest,created_by,file_source,data)
            VALUES ('${PART}','rec-second',1,'k','claiming',TRUE,'inst1','${fs}','{}'::jsonb)
            ON CONFLICT (partition_id,file_source) WHERE state <> 'deleted' AND is_latest
            DO NOTHING RETURNING id;")"
cnt="$(q "SELECT count(*) FROM file_metadata_records WHERE partition_id='${PART}' AND file_source='${fs}';")"
if [[ -z "$again" && "$cnt" == "1" ]]; then pass "C2 重试领取返回空且未新增记录（幂等：客户端重试安全）"
else fail "C2 重试产生了副作用（again='$again', count='$cnt'）"; fi

# ---------------------------------------------------------------------------
# C3 naive：删掉唯一索引 → 必须出现多赢家（自证本测试能发现问题）
# ---------------------------------------------------------------------------
q "DROP INDEX IF EXISTS ux_mr_source;" >/dev/null
q "DELETE FROM file_metadata_records WHERE partition_id='${PART}';" >/dev/null
multi=0
for ((r=0; r<5; r++)); do
  fs="/osdu-user/staging/naive-${r}"
  q "DELETE FROM file_metadata_records WHERE partition_id='${PART}';" >/dev/null
  prepare_common_start
  w="$(one_round "$fs" "/tmp/fss_naive_${r}" naive)"
  rows="$(q "SELECT count(*) FROM file_metadata_records WHERE partition_id='${PART}' AND file_source='${fs}';")"
  [[ "$w" -gt 1 || "$rows" -gt 1 ]] && multi=$((multi+1))
done
# 恢复索引：★ 必须先清掉 naive 轮次留下的重复行，否则 CREATE UNIQUE INDEX 会失败
q "DELETE FROM file_metadata_records WHERE partition_id='${PART}';" >/dev/null
q "CREATE UNIQUE INDEX IF NOT EXISTS ux_mr_source
     ON file_metadata_records (partition_id, file_source)
     WHERE state <> 'deleted' AND is_latest;" >/dev/null
if [[ $multi -gt 0 ]]; then pass "C3 去掉唯一索引后 5 轮中有 ${multi} 轮出现多赢家  ✅ 自证有效"
else fail "C3 去掉唯一索引后仍未出现多赢家 —— 本测试无法发现问题，结论不可信"; fi

# ---------------------------------------------------------------------------
# C4 软删除后可以重新领取
# ---------------------------------------------------------------------------
fs="/osdu-user/staging/reclaim"
q "DELETE FROM file_metadata_records WHERE partition_id='${PART}';" >/dev/null
q "INSERT INTO file_metadata_records(partition_id,id,version,kind,state,is_latest,created_by,file_source,data)
   VALUES ('${PART}','rec-old',1,'k','ready',TRUE,'inst0','${fs}','{}'::jsonb);" >/dev/null
q "UPDATE file_metadata_records SET state='deleted' WHERE partition_id='${PART}' AND file_source='${fs}';" >/dev/null
got="$(q "INSERT INTO file_metadata_records(partition_id,id,version,kind,state,is_latest,created_by,file_source,data)
          VALUES ('${PART}','rec-new',1,'k','claiming',TRUE,'inst1','${fs}','{}'::jsonb)
          ON CONFLICT (partition_id,file_source) WHERE state <> 'deleted' AND is_latest
          DO NOTHING RETURNING id;")"
if [[ -n "$got" ]]; then pass "C4 软删除后可以重新领取（部分索引谓词生效）"
else fail "C4 软删除后仍无法重新领取 —— 部分索引谓词有误"; fi

echo
printf '%s\n' "${C_GRN}原子领取验证通过（C1~C4）${C_OFF}"
echo "  结论：幂等性由【数据库唯一约束】保证，而不是应用层的 check-then-insert。"
echo "        这直接支撑门槛 C6.11（重复创建）与 C9.26（多实例端到端）。"
