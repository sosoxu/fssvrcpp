# ADR-011：日志实现选型 —— 自研 vs spdlog

| 项 | 值 |
| --- | --- |
| 状态 | ✅ 已定稿（P1 切片 4 期间评估） |
| 日期 | 2026-02（P1） |
| 相关 | `docs/02-design.md` §14（可观测性）、§15 T5（密钥泄漏）；`docs/04-implementation-plan.md` C1.6；`src/common/logging/` |
| 取代 | —— |
| 被取代 | —— |

---

## 1. 问题

P1 需要一个结构化日志组件（JSON Lines、级别、敏感字段脱敏）。问题：
**为什么不直接用 spdlog 这类成熟库，而要自己写？**

> 这个问题必须认真回答，不能拿"拿不到包"当理由 —— 本仓库在
> `docs/adr/ADR-002-http-framework.md` 上刚因为过度外推挨过一次（R7）。

## 2. 事实（先测，再判）

### 2.1 spdlog **是**可获得的（所以"拿不到"不是理由）

| 事实 | 值 |
| --- | --- |
| apt 镜像（USTC jammy/universe） | `libspdlog-dev` **1.9.2**、`libfmt-dev` **8.1.1** |
| 镜像 pool 里的上游源码 | spdlog 0.16.3 / 1.5.0 / 1.6 / **1.12.0** / **1.15.1** / **1.15.3** |
| 分发形态 | Debian 用 `+ds` 重打包，**剥掉了自带的 bundled fmt** |
| 后果 | 用 apt 版必须**同时**引 `libfmt-dev` 并定义 `SPDLOG_FMT_EXTERNAL` —— 正是 `third_party/README.md` 明确要避免的"宏/ABI 耦合" |
| 体积 | spdlog 82 个头文件 484 KB + fmt 头文件 552 KB |

### 2.2 spdlog **不能**提供我们真正需要的东西

| 我们的需求 | spdlog 1.9.2 | 证据 |
| --- | --- | --- |
| **脱敏（C1.6）** | ❌ 完全没有 | `sinks/` 里没有任何脱敏设施；payload 是调用方自备的字符串 |
| **JSON Lines 输出** | ❌ 无 JSON sink / formatter | `sinks/` 目录 28 个 sink：file/rotating/daily/syslog/tcp/… 无 json |
| **换行注入防护** | ❌ 默认**不转义** | 实测：写 1 条含 `\n` 的记录 → 读出 **2 行**，可伪造 `{"level":"error",...}` |
| 级别过滤 / 线程安全 / 格式化 | ✅ | 实测 91–101 ns/条（含写 `/dev/null`） |

### 2.3 成本对比：spdlog 能替掉的是哪一部分

一次 `info` 记录（6 个字段）在我们的实现里各环节耗时（独立进程 + `taskset` 绑核，
`/dev/null` sink，取 3 轮最好值）：

| 环节 | ns/条 | 谁能替掉 |
| --- | ---: | --- |
| `json::Value` 组装 + `dump()` | **~1100** | 只能我们自己改（spdlog 无 JSON） |
| `Redactor::ScrubText`（7 条规则） | **~1100** | 只能我们自己改（spdlog 无脱敏） |
| `Matches` × 6 字段 | ~450 | 只能我们自己改 |
| `Apply` × 6 字段 | ~500 | 只能我们自己改 |
| **日志框架部分**（级别判断 + 互斥 + 写 sink + pattern） | **~95** | ← spdlog 只能替掉这一格 |
| 合计（实测） | **2838–3101** | |

**结论：spdlog 能替掉约 3% 的耗时，替不掉 97%。**

对照：同一台机器、同一条记录，只做格式化不做 I/O 的"地板"测量

| 实现 | ns/条 |
| --- | ---: |
| nlohmann 组装对象 + `dump()`（我们现在的 JSON 路径） | ~1100 |
| `std::ostringstream`（我们现在的 text 路径） | ~200 |
| `fmt::format` 生成同样的 JSON 行 | ~170 |
| 手写 `append` + `std::to_chars` | **~45** |

→ 真正的差距（2800 → ~150）**不在"用不用 spdlog"，而在我们自己的格式化与脱敏实现**。

## 3. 候选方案

| # | 候选 | 评价 | 实测？ |
| --- | --- | --- | --- |
| 1 | **自研最小实现（现状）+ 按 2.3 优化** | 需求 100% 覆盖；热路径可优化到 ~150 ns/条；代价是要自己保证转义/UTF-8/并发正确（用 nlohmann 作 oracle + 测试兜住） | ✅ 已实测 |
| 2 | **用 spdlog 做 sink/format** | 只替掉 ~95 ns/条；脱敏、JSON 形状、转义仍要自研；引入 1.0 MB 头文件 + 外部 fmt + `SPDLOG_FMT_EXTERNAL` 宏耦合 | ✅ 已实测（可用性、能力、性能） |
| 3 | spdlog（上游 1.15.3 源码）+ 自定义 JSON sink | 同 2，且要连 fmt 一起 vendored；上游源码仍需从 pool 取 `+ds` 包（fmt 仍被剥离） | ⚠️ 未实测（未验证 1.15.3 与 jammy g++ 11.4/C++20 组合） |
| 4 | glog | 镜像里是 `0.5.0+really0.4.0`（实为 0.4.0，2013 年代码）；无可配置脱敏、无 JSON、格式固定 | ⚠️ 未实测 |
| 5 | Boost.Log | 环境**无 boost**（`AGENTS.md` §1）；为日志引入 boost 与"不用 boost"的既定约束冲突 | ⚠️ 未实测 |
| 6 | 直接用 `fmt`（不用 spdlog） | 能拿到 2.3 里 `fmt::format` 的 170 ns，但仍需自己写 sink/级别/互斥，且 JSON 转义仍要自备 | ⚠️ 未实测 |

## 4. 决策

**采用候选 1：保留自研最小实现，并把热路径优化到地板附近。**

理由（按权重）：

1. **可替代比例太小**：spdlog 只覆盖 ~3% 的耗时与 0% 的核心需求（脱敏 / JSON 形状 / 注入防护）。
   引入依赖换来的收益，不足以抵偿"多一个必须锁版本的第三方 + 宏耦合"。
2. **需求的特殊部分无法外包**：T5 要求"日志里不出现密钥明文"，而"什么算密钥"是本仓库的
   配置语义（`observability.redact_keys`），任何通用库都不会提供。
3. **接口已经是端口**：`ILogger` 是 L1 的技术抽象，调用方只依赖它。**将来真需要
   spdlog 的旋转文件 / syslog / 异步 sink，可以把它作为 `ILogger` 的一个实现接进来，
   不必改任何调用点。** 所以这个决策是**低成本可逆**的。
4. 顺带满足一个本仓库特有的约束：`third_party/` 只放 header-only、无外部 `.so`、
   无宏耦合的依赖（`third_party/README.md`）。spdlog 的 apt 形态恰好违反这条。

## 5. 重新评估的触发条件（任一满足就回来重开）

| # | 触发条件 | 为什么 |
| --- | --- | --- |
| **L1** | 需要**文件轮转**（按大小/按天）或 **syslog / journald** sink | 这些是纯商品能力，自己写不划算 |
| **L2** | 日志写入成为请求路径瓶颈，且需要**异步/有界队列**（背压 + 丢弃策略） | spdlog 的 `async_logger` + 环形队列是成熟实现 |
| **L3** | 需要 `flush_on(level)` 之外的复杂 flush 策略 | 见 §6.3 |
| **L4** | 我们自己实现的转义/UTF-8/脱敏在**真实流量**下出现正确性缺陷 | 届时"自研"的账就要重算 |

## 6. 本次评估**顺带发现并修复**的自研缺陷（这才是关键收益）

评估过程暴露了自研实现的两个真实缺陷 —— 它们和"用不用 spdlog"无关：

### 6.1 P1-D11：热路径 ~28× 于地板

`Render()` 每条记录都新建 nlohmann 对象并 `dump()`（~1100 ns），text 路径用
`std::ostringstream`（~200 ns），`Matches`/`ScrubText` 每次调用都做一趟带堆分配的归一化。
修复：预留缓冲 + 手写追加 + `std::to_chars`；脱敏改成"一次归一化、扫描全部规则、
从后往前替换"。数字见 `docs/test-evidence/phase1.md`。

### 6.2 P1-D12：**非法 UTF-8 会让日志抛异常**（`json::Dump` 的 `strict` 语义）

nlohmann 的 `dump()` 默认是 `error_handler_t::strict`：**遇到非法 UTF-8 抛
`type_error.316`**。日志的输入来自文件名/路径/头部 —— 完全可能是非 UTF-8 字节。
即"用日志记录一个坏输入"反而把请求打挂。实测：

```
show("非法 UTF-8: \xff\xfe")   → THROW [json.exception.type_error.316] invalid UTF-8 byte at index 1: 0xFF
```

修复：自备转义函数，非法字节按 U+FFFD 替换（与 nlohmann `error_handler_t::replace` 同语义），
并给 `ILogger::Log` 加 `noexcept` 兜底：日志**任何情况下**都不得把异常抛给调用方。

### 6.3 flush 策略（记录，未改）

实测每记录 1 次 `write` 系统调用（20 万条 → 200001 次），而 spdlog 默认缓冲
（20 万条 → 2 次）。写 `/dev/null` 时该开销仅 ~230 ns/条，不是当前瓶颈；
但**写真实文件/管道的代价未实测**。暂不引入 `flush_on(level)` 策略，
在 P4（访问日志）再按实测决定 —— 这属于触发条件 L3。

## 7. 未验证项（如实标注）

- spdlog 1.15.3（pool 上游源码）与 jammy g++ 11.4 + C++20 的组合**未编译验证**；
  候选 3 的成本估计基于包元数据，不是实测。
- glog 0.4.0、Boost.Log、纯 `fmt`（候选 4/5/6）**未实测**，仅按可得性与能力范围评估。
- spdlog 的 `async_logger` 相对我们实现的吞吐优势**未实测**（那属于触发条件 L2 的范围）。
- 本文 2.3 的数字是**同机 `/dev/null` sink 的相对比较**，不是生产容量结论（R3）。
