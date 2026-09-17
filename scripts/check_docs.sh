#!/usr/bin/env bash
# =============================================================================
#  scripts/check_docs.sh —— 文档一致性机械检查（AGENTS.md §2.3 / §5 的执行体）
# =============================================================================
#  为什么需要它：AGENTS.md 里的规则如果无法机械检查，就只是"建议"。
#  本脚本把下面四条变成会失败的检查：
#
#    D1 文档内部链接有效
#         （跳过代码块与行内代码 —— 文档里的正则表达式看起来很像 markdown 链接）
#    D2 被推翻的结论/数字不再被无标记引用
#         （例如已作废的 58,741；出现处必须带作废标记）
#    D3 ADR 索引完整：无孤儿 ADR；被引用的 ADR 文件存在（待定稿的需显式标注）
#    D4 阶段状态一致：实现计划里的完成标记 ⇄ run_all_gates.sh 的 IMPLEMENTED_PHASES
#
#  用法：scripts/check_docs.sh [--quiet] [--selftest]
#  退出码：0 = 全部通过；1 = 有问题（逐条列出，并给出修复提示）
#
#  ⚠️ D2 是**启发式**检查，不是证明：它只在"值出现的 ±3 行窗口内找不到任何作废标记词"
#     时报错。因此存在漏报（窗口外有标记）与误报（窗口内恰好出现标记词）的可能。
#     `--selftest` 通过注入一个**故意不合作**的临时文件来验证 D1/D2 确实能失败
#     —— 按 AGENTS.md 铁律 R1，检查器自身也必须自证。
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QUIET=0
SELFTEST=0
for arg in "$@"; do
  case "$arg" in
    --quiet) QUIET=1 ;;
    --selftest) SELFTEST=1 ;;
  esac
done

C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
say() { [[ ${QUIET} -eq 1 ]] || printf '%s\n' "$*"; }
ok()   { [[ ${QUIET} -eq 1 ]] || printf '%s\n' "${C_GRN}  ✓${C_OFF} $*"; }
bad()  { printf '%s\n' "${C_RED}  ✗${C_OFF} $*"; }
info() { [[ ${QUIET} -eq 1 ]] || printf '%s\n' "${C_DIM}  ·${C_OFF} $*"; }

FAILURES=0

# -----------------------------------------------------------------------------
#  自证（AGENTS.md 铁律 R1）：证明 D1/D2 确实能检出问题
#  注入的文件**不能包含任何作废标记词**，否则 D2 会被自己的文字骗过去
# -----------------------------------------------------------------------------
if [[ ${SELFTEST} -eq 1 ]]; then
  TMP_MD="${REPO_ROOT}/docs/_selftest_injected.md"
  # ★ 用 trap 清理：早前手工测试时因为 exit 在 rm 之前触发，残留了一个临时文件
  trap 'rm -f "${TMP_MD}"' EXIT
  cat > "${TMP_MD}" <<'MD'
# 临时自证文件（由 check_docs.sh --selftest 生成，检查后删除）
指向不存在的目标：[坏的链接](no-such-document.md)
这个性能数字现在没有依据：达到 58,741 files/s。
MD
  set +e
  out="$("${BASH_SOURCE[0]}" --quiet 2>&1)"; rc=$?
  set -e
  rm -f "${TMP_MD}"
  ok_d1=0; ok_d2=0
  printf '%s' "$out" | grep -q "D1 失效链接" && ok_d1=1
  printf '%s' "$out" | grep -q "D2 已作废的值" && ok_d2=1
  if [[ ${rc} -ne 0 && ${ok_d1} -eq 1 && ${ok_d2} -eq 1 ]]; then
    printf '%s\n' "${C_GRN}  ✓${C_OFF} 自证：D1 与 D2 都能检出注入的错误（检查器有效）"
  else
    printf '%s\n' "${C_RED}  ✗${C_OFF} 自证失败：检查器未能检出注入的错误（rc=${rc}, D1=${ok_d1}, D2=${ok_d2}）"
    printf '%s\n' "     → 检查器的"通过"结论不可信"
    exit 1
  fi
fi

say "文档一致性检查（AGENTS.md §2.3）"

python3 - "${REPO_ROOT}" <<'PY'
import os, re, sys

root = sys.argv[1]
failures = []

def md_files():
    for dp, dns, fns in os.walk(root):
        dns[:] = [d for d in dns if d not in
                  ('build', '.git', 'third_party', 'src', 'tests', '.devpg')]
        for fn in fns:
            if fn.endswith('.md'):
                yield os.path.join(dp, fn)

def read(p):
    return open(p, encoding='utf-8', errors='replace').read()

def strip_code(text):
    """去掉围栏代码块与行内代码，避免把文档里的正则当成链接"""
    text = re.sub(r'```.*?```', '', text, flags=re.S)
    text = re.sub(r'~~~.*?~~~', '', text, flags=re.S)
    return re.sub(r'`[^`]*`', '', text)

# ---------------------------------------------------------------- D1 链接
checked = 0
for p in md_files():
    body = strip_code(read(p))
    for m in re.finditer(r'\[([^\]]+)\]\(([^)]+)\)', body):
        url = m.group(2).strip()
        if url.startswith(('http://', 'https://', '#', 'mailto:')):
            continue
        frag = url.split('#')[0]
        if not frag:
            continue
        checked += 1
        target = os.path.normpath(os.path.join(os.path.dirname(p), frag))
        if not os.path.exists(target):
            failures.append(f"D1 失效链接: {os.path.relpath(p, root)} -> {url}")
print(f"  D1 检查了 {checked} 个本地链接")

# ------------------------------------------------- D2 已作废结论/数字
# 已作废的取值 -> 该值必须出现的"可接受的上下文关键词"
RETRACTED = {
    '58,741': ['作废', '无效', '不安全', '⚠️', '~~', '推翻', '错误', '旧', '更正', '修正',
               '不要用', '禁止', '而不是'],
    '59,248': ['作废', '无效', '不安全', '⚠️', '~~', '推翻', '错误'],
}
for p in md_files():
    lines = read(p).splitlines()
    for i, line in enumerate(lines):
        for val, markers in RETRACTED.items():
            if val not in line:
                continue
            window = "\n".join(lines[max(0, i - 3): i + 4])
            if not any(mk in window for mk in markers):
                failures.append(
                    f"D2 已作废的值 '{val}' 被无标记引用: "
                    f"{os.path.relpath(p, root)}:{i+1} —— "
                    f"请加作废标记，或把它移到 docs/00-final-design.md §5")

# ------------------------------------------------------- D3 ADR 索引完整性
adr_dir = os.path.join(root, 'docs', 'adr')
adr_files = sorted(f for f in os.listdir(adr_dir) if f.startswith('ADR-') and f.endswith('.md'))
adr_nums = {re.match(r'ADR-(\d+)', f).group(1): f for f in adr_files}

# 允许"尚未定稿"的 ADR 编号（在索引里显式标注）
PENDING = {'005', '006'}

all_text = "\n".join(read(p) for p in md_files())
referenced = set(re.findall(r'ADR-(\d{3})', all_text))

for num, fn in adr_nums.items():
    # 到少要在"ADR 索引"里出现一次；这里放宽为"全仓库被引用过"，并要求索引存在
    if num not in referenced:
        failures.append(f"D3 孤儿 ADR（从未被引用）: docs/adr/{fn}")
for num in sorted(referenced):
    if num not in adr_nums and num not in PENDING:
        failures.append(f"D3 引用了不存在的 ADR-{num}（若为待定稿，请加入 check_docs.sh 的 PENDING）")
print(f"  D3 ADR 文件 {len(adr_nums)} 个，被引用 {len(referenced)} 个编号")

# --------------------------------------------- D4 阶段状态一致性
plan = read(os.path.join(root, 'docs', '04-implementation-plan.md'))
gates = read(os.path.join(root, 'scripts', 'run_all_gates.sh'))

# 计划表：| **P0** | ... | ✅ **已完成** |
# 计划表有 6 列：| **P0** | 名称 | 产出 | 命令 | 状态 |
#   ✅ = 已完成；🚧 = 进行中（部分门槛已启用，但本阶段未收口）
done_in_plan = set()
partial_in_plan = set()
for line in plan.splitlines():
    m = re.match(r'\|\s*\*\*P(\d)\*\*', line)
    if not m:
        continue
    if '✅' in line:
        done_in_plan.add(m.group(1))
    elif '🚧' in line:
        partial_in_plan.add(m.group(1))
impl = re.search(r'IMPLEMENTED_PHASES=\(([^)]*)\)', gates)
impl_set = set(re.findall(r'\d', impl.group(1))) if impl else set()

# 规则：
#   ① 标 ✅ 的阶段必须已启用门槛（不允许"声称完成但没有门槛"）
#   ② 启用了门槛的阶段必须在计划里有标记（✅ 或 🚧）—— 不允许悄悄启用
not_gated = done_in_plan - impl_set
unmarked = impl_set - done_in_plan - partial_in_plan
if not_gated:
    failures.append(
        f"D4 标 ✅ 但未启用门槛的阶段：{sorted(not_gated)}"
        f"（请把编号加入 run_all_gates.sh 的 IMPLEMENTED_PHASES）")
if unmarked:
    failures.append(
        f"D4 已启用门槛但计划中无标记的阶段：{sorted(unmarked)}"
        f"（请在 docs/04-implementation-plan.md 的阶段表标 ✅ 或 🚧）")
print(f"  D4 已完成 {sorted(done_in_plan) or '无'}，进行中 {sorted(partial_in_plan) or '无'}，"
      f"门槛启用 {sorted(impl_set) or '无'}")

# ------------------------------------------- D5 门槛编号完整性
# 形如 C1.7 / C1.2b：同一阶段内数字必须连续无断号、无重复
# （本条检查就是被"补端口时顺手加了 C1.11+ 却漏了 C1.8~C1.10"这个真实缺陷驱动的）
# ★ 只认"定义行"：门槛编号必须出现在表格的【第一列】
#   （否则会把"支撑门槛"引用列里的 C2.10 当成第二次定义 —— 已踩过这个误报）
gate_defs = re.findall(r'^\|\s*\*{0,2}(C\d\.\d+[a-z]?)\*{0,2}\s*\|', plan, flags=re.M)
flat = gate_defs
by_phase = {}
for gid in flat:
    m = re.match(r'C(\d)\.(\d+)([a-z]?)', gid)
    by_phase.setdefault(m.group(1), []).append((int(m.group(2)), m.group(3), gid))
for ph, items in sorted(by_phase.items()):
    nums = [n for n, _, _ in items]
    maxn = max(nums)
    missing = [n for n in range(1, maxn + 1) if n not in nums]
    full = [g for _, _, g in items]
    dupes = sorted({g for g in full if full.count(g) > 1})
    if missing:
        failures.append(
            f"D5 C{ph} 门槛编号断号：缺 " + ", ".join(f"C{ph}.{n}" for n in missing)
            + "（要么补齐，要么确认是有意省略）")
    if dupes:
        failures.append(f"D5 C{ph} 门槛编号重复：{', '.join(dupes)}")
print(f"  D5 门槛编号检查：{len(by_phase)} 个阶段，共 {len(flat)} 条门槛")

# --------------------------------------------- 输出
if failures:
    print()
    for f in failures:
        print("  ✗ " + f)
    sys.exit(1)
print()
print("  全部检查通过（D1~D5）")
PY
RC=$?
if [[ ${RC} -ne 0 ]]; then
  printf '%s\n' "${C_RED}文档一致性检查未通过 —— 修复后重跑${C_OFF}"
  exit 1
fi
ok "文档一致性检查通过"
