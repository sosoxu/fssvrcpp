#!/usr/bin/env bash
# =============================================================================
#  run_all_gates.sh —— 按顺序执行全部已启用的阶段门槛
# =============================================================================
#
#  规则（见 docs/04-implementation-plan.md §2）：
#    - 任一阶段门槛失败 → 立即退出（不继续后续阶段）
#    - 后续阶段不得使已启用阶段退化
#    - 每阶段结束后把证据写入 docs/test-evidence/phaseN.md
#
#  用法：
#     scripts/run_all_gates.sh              # 跑全部已启用阶段
#     scripts/run_all_gates.sh 0 1          # 只跑阶段 0 与 1
#     BUILD_DIR=build-asan scripts/run_all_gates.sh
#
#  说明：未实现的功能对应的阶段标记为 "not-implemented" 并**跳过**
#        （一旦该阶段的测试目标存在，就会自动被纳入门槛，无需改脚本）。
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
JOBS="${JOBS:-$(nproc)}"

cd "${REPO_ROOT}"

# 已实现阶段的前缀（新增阶段时把编号加进来；未列入的会被跳过并提示）
IMPLEMENTED_PHASES=(0 1 2 3 4 5 6 7 8)

WANT=("$@")

stage_enabled() {
  local p="$1"
  if [[ ${#WANT[@]} -eq 0 ]]; then return 0; fi
  for w in "${WANT[@]}"; do [[ "$w" == "$p" ]] && return 0; done
  return 1
}

phase_implemented() {
  local p="$1"
  for i in "${IMPLEMENTED_PHASES[@]}"; do [[ "$i" == "$p" ]] && return 0; done
  return 1
}

hr() { printf '%s\n' "------------------------------------------------------------------------"; }

echo "fssvrcpp 阶段门槛总执行"
echo "  仓库根目录 : ${REPO_ROOT}"
echo "  构建目录   : ${BUILD_DIR}"
echo "  并行度     : ${JOBS}"
hr

# ---------------------------------------------------------------------------
# 配置 + 构建（所有阶段共用；任何编译错误即整体失败）
# ---------------------------------------------------------------------------
# 可选：把 PostgreSQL 基建门槛（schema 不变量 / advisory lock / 并发领取）纳入本次执行。
#   FSS_GATES_WITH_PG=1 scripts/run_all_gates.sh
# 需要 scripts/dev_postgres.sh 能启动 PG（无需 root）。默认关闭，保证"无外部依赖也能跑门槛"。
PG_FLAG=()
if [[ "${FSS_GATES_WITH_PG:-0}" == "1" ]]; then
  PG_FLAG=(-DFSS_WITH_PG=ON)
  echo "==> 已启用 PostgreSQL 基建门槛（FSS_GATES_WITH_PG=1）"
fi

# ---------------------------------------------------------------------------
# 注入残留前置检查（P6-D04）
#   `verify_http_hardening.sh` 等自证脚本会**临时改源码**再靠 `trap` 恢复。若脚本被
#   SIGKILL 强杀（trap 对 SIGKILL 无效），注入就留在源码树里 —— 之后的门槛会在
#   「被拆掉防线」的代码上跑（最坏情况是静默通过）。这里是机械防线：
#     · `src/` 下不得存在注入用的临时文件（`_*selftest*` / `*_injected*`）；
#     · 受版本控制的源码 diff 里不得出现注入标记（`自证注入`）。
# ---------------------------------------------------------------------------
echo "==> [preflight] 注入残留检查"
LEFT1="$(cd "${REPO_ROOT}" && find src -type f \( -name '_*selftest*' -o -name '*_injected*' \) 2>/dev/null || true)"
LEFT2=""
if command -v git >/dev/null 2>&1 && [[ -d "${REPO_ROOT}/.git" ]]; then
  LEFT2="$(cd "${REPO_ROOT}" && git diff -U0 -- src 2>/dev/null | grep '自证注入' || true)"
fi
if [[ -n "${LEFT1}" || -n "${LEFT2}" ]]; then
  echo "==> [preflight] ❌ 检测到自证脚本的注入残留："
  [[ -n "${LEFT1}" ]] && printf '%s\n' "${LEFT1}" | sed 's/^/      /'
  [[ -n "${LEFT2}" ]] && printf '%s\n' "${LEFT2}" | sed 's/^/      /'
  echo "      修复：git checkout -- src  （或删掉上面列出的临时文件）后重跑。"
  echo "      原因：注入式自证脚本被强杀时 trap 不会执行 —— 门槛绝不能在「拆掉防线」的代码上跑。"
  exit 1
fi
echo "==> [preflight] ✅ 无注入残留"
hr

# ---------------------------------------------------------------------------
# 文档一致性（AGENTS.md §2.3：收工前必做）
# 放在最前：文档漂移会让后续所有工作的依据失真，且它只花不到 1 秒
# ---------------------------------------------------------------------------
echo "==> [docs] scripts/check_docs.sh --selftest"
if ./scripts/check_docs.sh --selftest; then
  echo "==> [docs] ✅ 通过"
else
  echo "==> [docs] ❌ 失败"
  echo
  echo "❌ 文档一致性检查未通过 —— 先修文档，再跑门槛。"
  exit 1
fi
hr

echo "==> [prepare] cmake 配置"
cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=RelWithDebInfo "${PG_FLAG[@]}"

echo "==> [prepare] 构建"
cmake --build "${BUILD_DIR}" -j"${JOBS}"
hr

FAILED=()
SKIPPED=()

# ---------------------------------------------------------------------------
# PostgreSQL 基建门槛（不隶属于某个阶段，但支撑 C2.10 / C6.11 / C8.9 / C9.26）
# ---------------------------------------------------------------------------
if [[ "${FSS_GATES_WITH_PG:-0}" == "1" ]]; then
  echo "==> [infra] ctest -L pg（真实 PostgreSQL：schema 不变量 / advisory lock / 并发领取）"
  if ctest --test-dir "${BUILD_DIR}" -L pg --output-on-failure; then
    echo "==> [infra] ✅ 通过"
  else
    echo "==> [infra] ❌ 失败"
    FAILED+=("pg")
  fi
  hr
fi

for p in 0 1 2 3 4 5 6 7 8 9; do
  stage_enabled "$p" || continue

  if ! phase_implemented "$p"; then
    SKIPPED+=("$p")
    echo "==> [phase${p}] 跳过：尚未实现（未列入 IMPLEMENTED_PHASES）"
    hr
    continue
  fi

  # 阶段 1 的前置：分层护栏必须能失败（C1.1 的自证部分）
  if [[ "$p" == "1" ]]; then
    echo "==> [phase1] scripts/verify_guard.sh（证明护栏能失败 —— AGENTS.md R1）"
    if ./scripts/verify_guard.sh "${BUILD_DIR}"; then
      echo "==> [phase1] ✅ 护栏自证通过"
    else
      echo "==> [phase1] ❌ 护栏自证失败 —— 护栏的'通过'不可信"
      FAILED+=("phase1-guard")
      break
    fi
    # H-2 防护的生效性自证（C1.2b）：拆掉防护后 test_httplib_hardening 必须失败
    echo "==> [phase1] scripts/verify_http_hardening.sh（证明 H-2 防护能失败 —— C1.2b）"
    if ./scripts/verify_http_hardening.sh "${BUILD_DIR}"; then
      echo "==> [phase1] ✅ H-2 防护自证通过"
    else
      echo "==> [phase1] ❌ H-2 防护自证失败 —— 超限用例防不住 H-2"
      FAILED+=("phase1-http-hardening")
      break
    fi

    # C1.7：ASan + UBSan 全量构建下测试全绿。
    #   复用 build-asan 目录 → 源没变时是增量构建，开销很小；首次约数分钟。
    #   跳过：FSS_GATES_SKIP_SANITIZERS=1（只在明确知道自己在做什么时用）。
    if [[ "${FSS_GATES_SKIP_SANITIZERS:-0}" == "1" ]]; then
      echo "==> [phase1] ! C1.7 sanitizer 检查被 FSS_GATES_SKIP_SANITIZERS=1 跳过"
    else
      echo "==> [phase1] scripts/run_sanitizers.sh（C1.7：ASan + UBSan 全量）"
      if ./scripts/run_sanitizers.sh; then
        echo "==> [phase1] ✅ sanitizer 全绿"
      else
        echo "==> [phase1] ❌ sanitizer 构建下有失败"
        FAILED+=("phase1-sanitizers")
        break
      fi
    fi
  fi

  # 阶段 2 的前置：链接图（C2.1）——依赖方向被破坏时立刻失败
  if [[ "$p" == "2" ]]; then
    echo "==> [phase2] scripts/verify_link_graph.sh（C2.1：分层链接图）"
    if ./scripts/verify_link_graph.sh "${BUILD_DIR}"; then
      echo "==> [phase2] ✅ 链接图通过"
    else
      echo "==> [phase2] ❌ 链接图出现越层依赖"
      FAILED+=("phase2-link-graph")
      break
    fi
    # 能力分支护栏的生效性自证（C2.7）：注入越权调用后必须失败
    echo "==> [phase2] scripts/verify_capability_guard.sh（证明能力护栏能失败 —— C2.7）"
    if ./scripts/verify_capability_guard.sh "${BUILD_DIR}"; then
      echo "==> [phase2] ✅ 能力护栏自证通过"
    else
      echo "==> [phase2] ❌ 能力护栏自证失败 —— 护栏的'通过'不可信"
      FAILED+=("phase2-capability-guard")
      break
    fi
  fi

  # C5.9：切换驱动只改配置（同一二进制 + 同一段端到端脚本，posix/s3 各跑一遍）
  if [[ "${p}" == "5" ]]; then
    echo "==> [phase5] scripts/verify_driver_switch.sh（C5.9：同一二进制，posix/s3 各跑一遍）"
    if ./scripts/verify_driver_switch.sh "${BUILD_DIR}"; then
      echo "==> [phase5] ✅ 驱动切换验证通过"
    else
      echo "==> [phase5] ❌ 驱动切换验证失败"
      FAILED+=("phase5-driver-switch")
      break
    fi
  fi

  # C4.9：组合根纪律护栏的生效性自证（注入越权装配后必须失败）
  if [[ "${p}" == "4" ]]; then
    echo "==> [phase4] scripts/verify_composition_root.sh（证明组合根护栏能失败 —— C4.9/R12）"
    if ./scripts/verify_composition_root.sh "${BUILD_DIR}"; then
      echo "==> [phase4] ✅ 组合根护栏自证通过"
    else
      echo "==> [phase4] ❌ 组合根护栏自证失败 —— 护栏的'通过'不可信"
      FAILED+=("phase4-composition-root")
      break
    fi
  fi

  echo "==> [phase${p}] ctest -L phase${p}"
  if ctest --test-dir "${BUILD_DIR}" -L "phase${p}" --output-on-failure; then
    echo "==> [phase${p}] ✅ 通过"
  else
    echo "==> [phase${p}] ❌ 失败"
    FAILED+=("$p")
    break    # 铁律：门槛失败即停止
  fi
  hr
done

echo "汇总"
if [[ ${#SKIPPED[@]} -gt 0 ]]; then
  echo "  跳过（未实现）: ${SKIPPED[*]}"
fi
if [[ ${#FAILED[@]} -gt 0 ]]; then
  echo "  失败: ${FAILED[*]}"
  echo
  echo "❌ 门槛未通过 —— 禁止进入下一阶段。"
  exit 1
fi

echo "  失败: 无"
echo
echo "✅ 全部已启用阶段门槛通过。"
echo "   提醒：通过后请把命令、输出摘要与结论写入 docs/test-evidence/phaseN.md。"
