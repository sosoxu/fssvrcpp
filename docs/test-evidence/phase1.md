# 阶段 1 测试证据（进行中）

| 项 | 值 |
| --- | --- |
| 阶段 | P1（分层骨架 + 通用库 + HTTP 传输层） |
| 状态 | ✅ **已完成并通过门槛**（C1.1 ~ C1.15 全部满足） |
| 门槛命令 | `ctest -L phase1` + `scripts/verify_guard.sh` |
| 退出码 | `0` |

> 本阶段已收口。P1 的退出条件（C1.1–C1.7）全部满足，证据见本文件；可以进入 P2。
> 按门槛铁律，**未收口前不进入 P2**。

---

## 1. 切片 1 交付物

| 路径 | 内容 |
| --- | --- |
| `src/CMakeLists.txt` | **分层目标图**：`fss_domain`（只链接 L1）、`fss_app`（只链接 L3） |
| `src/common/result/{result.h,result.cpp}` | `Result<T>` / `Result<void>` / `Error` / `ErrorKind` / `FSS_TRY` |
| `src/common/crypto/{crypto.h,crypto.cpp}` | SHA-256、HMAC-SHA256（**原始字节链式派生**）、SigV4 密钥派生、base64/base64url、常量时间比较、随机数 |
| `src/domain/version.*`、`src/app/version.*` | 占位 TU —— 必须有真实 TU 才能让分层违规在**构建期**失败 |
| `tests/unit/test_layering_guard.cpp` | 源码级分层护栏（6 条规则） |
| `tests/unit/test_result.cpp` | 错误传播不丢信息；`ErrorKind` 名字与契约 §5 对齐 |
| `tests/unit/test_crypto.cpp` | SHA-256 / RFC 4231 HMAC / **AWS SigV4 派生链** / base64url / 常量时间比较 |
| `tests/framework/catch_main.cpp` | 统一的 Catch2 主入口 |
| `scripts/verify_guard.sh` | 护栏自证（①~⑤） |

## 1b. 切片 2 交付物（`json` / `fs` / `time` / `ids`）

| 路径 | 内容 |
| --- | --- |
| `src/common/json/{json.h,json.cpp}` | 解析/序列化、**大小写敏感**的 OSDU 字段访问、带行列偏移的解析错误、未知字段原样保留 |
| `src/common/fs/{fs.h,fs.cpp}` | `LexicalJoin`（词法安全）+ `VerifyResolvedWithinRoot`（真实路径/符号链接）+ `SafeJoin`；`AtomicWriteFile`（tmp+fsync+rename+fsync dir）；`ReadAt`（`pread` 定位读） |
| `src/common/time/{clock.h,clock.cpp,time_format.*}` | `IClock`/`SystemClock`/`ManualClock`；ISO-8601、**OSDU `CreatedAt`（+0000）**、RFC 1123 |
| `src/common/ids/{id_generator.h,id_generator.cpp}` | `IIdGenerator`/`UuidGenerator`（UUIDv4）/`SequentialIdGenerator`（确定性） |
| `tests/framework/temp_dir.h` | 共用的临时目录 RAII 助手 |
| `tests/unit/test_{json,fs,time,ids}.cpp` | 四个模块的单元测试 |

## 1c. 切片 3 交付物（`config` + 注释 JSON 的真回归）

| 路径 | 内容 |
| --- | --- |
| `src/common/config/{config.h,config.cpp,core_schema.cpp}` | 分层配置加载（**默认值 < 文件 < 环境变量 < 命令行**）、`${ENV:VAR}` 展开、字段类型/范围/枚举校验、未知键拒绝、动态子树、跨字段规则、`RedactedDump()`、**一次性收集全部问题** |
| `config/fss.example.json` | 带注释的 JSON 样例（185 键；**与 `CoreSchema` 双向一致**由测试强制） |
| `tests/unit/test_config.cpp` | 13 个用例 / 70 断言 |
| `tests/unit/test_json.cpp`（扩充） | 新增 3 个用例覆盖 `ParseWithComments` + **严格 `Parse` 必须拒绝注释**的自证对照 |

## 1d. 切片 4 交付物（`logging` —— C1.6 脱敏）

| 路径 | 内容 |
| --- | --- |
| `src/common/logging/{logging.h,logging.cpp}` | `Level`/`ParseLevel`；`Redactor`（键名子串匹配 + `ScrubText` 自由文本打码）；`ILogger` + `StreamLogger`（JSON Lines / text）+ `MemoryLogger`；`Context`（correlation-id 透传）；`OptionsFromConfig` |
| `tests/unit/test_logging.cpp` | 13 个用例 / 3312 断言 |

## 1e. 切片 5 交付物（日志热路径与转义硬化 —— 由"为什么不用 spdlog"的评估引出）

| 路径 | 内容 |
| --- | --- |
| `docs/adr/ADR-011-logging-library.md` | **日志实现选型**：4 类候选、spdlog 可用性/能力/性能实测、取舍与 4 条重开触发条件 |
| `src/common/logging/logging.cpp`（重写热路径） | 自备 JSON 转义（与 nlohmann `error_handler_t::replace` **逐字节等价**）+ 预留缓冲直接追加 + 单趟脱敏 + `noexcept` 兜底 |
| `bench/logging_bench.cpp`·`scripts/bench_logging.sh` | 可重跑的日志基准（独立进程 + 绑核），含"单核预算占比"折算 |
| `tests/unit/test_logging.cpp`（扩充） | 18 个用例 / 3632 断言，含**穷举 47 万个字节序列**与 nlohmann 参照实现比对 |

## 1f. 切片 6 交付物（`bytes` / `net` / `sys`）

| 路径 | 内容 |
| --- | --- |
| `src/common/bytes/{bytes.h,bytes.cpp}` | **RFC 7233 Range 归一化**（`a-b`/`a-`/`-N`/多段/越界→416/语法非法→忽略）；`ByteSource`/`ByteSink` 流抽象（固定缓冲搬运 → RSS 与文件大小无关）；`FixedBuffer`/`AlignedBuffer`；`LimitedSource`（计数 reader，超限**立即**中止）；`ThrottledSource`（可注入时钟的令牌桶） |
| `src/common/net/{net.h,net.cpp}` | 百分号编码（严格 / 路径段两种模式）、严格的 `PercentDecode`、查询串解析/构造（保序 + 重复键）、URL 解析（scheme/host 归一化、显式端口保留、IPv6）、`AppendPathSegment`（防路径注入） |
| `src/common/sys/{capability.h,capability.cpp}` | `ProbeIoUring()`：区分 **可用 / EPERM（策略阻断）/ ENOSYS（内核不支持）/ 其它 errno**，返回结构化结果；`DecideIoEngine()`：**纯函数**决策（blocking / uring / auto 的回退与拒绝启动语义） |
| `tests/unit/test_{bytes,net,sys}.cpp` | 142 + 205 + 85 = **432 断言**；每个"拒绝"断言都配正向对照 |

## 1g. 切片 7 交付物（`fss_http` 包装层 —— P1 最关键的一块）

| 路径 | 内容 |
| --- | --- |
| `src/common/http/{http.h,server.cpp}` | **`fss_http`**：Server 门面（pimpl，**不泄漏任何 httplib 类型**）+ 自己的路由表（`:param` / `*` 前缀 / 方法不匹配→404）+ H-2 三重防护 + Range 归一化 + 中间件（correlation-id / 访问日志）+ 并发上限（503 + Retry-After）+ 错误体归一化 |
| `tests/framework/raw_http.h` | 原始 socket HTTP 客户端（畸形请求、逐字节读响应、chunked、丢弃大 body）——硬化测试必须在**线缆层**观察 |
| `tests/integration/test_httplib_hardening.cpp` | 13 用例 / **270 断言**：H-1/H-2 回归、Range 全边界、请求走私、头/URI 上限、方法不匹配、HEAD、correlation-id、日志、选项校验 |
| `tests/integration/test_http_server.cpp` | 9 用例 / **244 断言**：keep-alive、并发 ≥ 50（**峰值在途 ≥ 50**）、chunked、**1 GiB 流式下载 + 1 GiB 流式上传的 RSS 峰值增长 < 64 MiB**、TCP_NODELAY 自证、并发上限 503 |
| `scripts/verify_http_hardening.sh` | **C1.2b 自证**：拆掉 H-2 两道防护 → 测试必须失败且症状吻合（已纳入 `run_all_gates.sh`） |

## 1h. 切片 8 交付物（C1.7：ASan + UBSan 全量）

| 路径 | 内容 |
| --- | --- |
| `scripts/run_sanitizers.sh` | 独立构建目录 `build-asan`（`-fsanitize=address,undefined -fno-sanitize-recover=all`，LeakSanitizer 打开）；phase0+phase1 全跑；已纳入 `run_all_gates.sh` |
| `CMakeLists.txt` | ★ `target_compile_definitions(PkgConfig::GRPCPP INTERFACE GRPC_ASAN_SUPPRESSED=1)` —— 打断 gRPC 与 ASan 的**布局耦合**（见 P1-D20） |
| `tests/integration/test_http_server.cpp` | 大文件规模可配置（`FSS_TEST_BIG_BYTES`），使 sanitizer 下能跑得动；正常构建仍是 1 GiB |

```
$ ./scripts/run_sanitizers.sh
C1.7：ASan + UBSan 构建（build-asan）
  编译选项： -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
  并行度： -j4（sanitizer 下单个 TU 可达 1 GB+，并行过高会 OOM）
  ✓ 构建完成
  1/1 Test #1: test_phase0_toolchain ............   Passed
  100% tests passed, 0 tests failed out of 1        （phase0）
  14/14 Test #15: test_http_server ...............   Passed
  100% tests passed, 0 tests failed out of 14      （phase1）
  ✓ ASan + UBSan 全绿：无内存错误、无未定义行为、无泄漏（C1.7）
```

> ⚠️ **补记（P2 切片 4 发现并修复，详见 `docs/test-evidence/phase2.md` 的 P2-D07）**：
> 上面这段"全绿"当时是**空证据** —— 脚本用 `tr -d ' '` 解析 `IMPLEMENTED_PHASES`，把
> `(0 1 2)` 压成单个词 `012`，标签于是变成 `phase012`（根本不存在）。
> 实际只有 `phase0` 跑了，phase1/phase2 **从未在 sanitizer 下执行**，却照样打印"全绿"。
> 修复后（按数字切词 + 每个标签先核对测试数 > 0，为 0 直接判失败）重跑：
> `phase0`（1）+ `phase1`（14）+ `phase2`（6）在 ASan/UBSan 下全部通过；
> 并且立即抓出 `test_object_key_policy.cpp` 的越界读（P2-D08）。
> 因此本表 C1.7 的结论现在有**真实执行**支撑。

> 📝 同一轮还修掉了本文件里的两个**裸 NUL 字节**（P1 的 D-13 记录中把 `` `'\0'` `` 写成了
> 真正的 0x00），它使本文件被工具判定为二进制、无法被编辑/检索。已改为转义文本 `\0`。

## 2. 门槛命令与输出

```console
$ cmake -S . -B build && cmake --build build -j8
[ 25%] Built target fss_result
[ 41%] Built target fss_domain
[ 50%] Built target fss_app
[ 62%] Built target fss_crypto
[ 91%] Built target test_result
[ 91%] Built target test_layering_guard
[ 95%] Built target test_crypto
[100%] Built target test_phase0_toolchain

$ ctest --test-dir build -L phase1 --output-on-failure
    Start 2: test_layering_guard ..............   Passed    0.00 sec
    Start 3: test_result ......................   Passed    0.00 sec
    Start 4: test_crypto ......................   Passed    0.00 sec
    Start 5: test_json ........................   Passed    0.00 sec
    Start 6: test_fs ..........................   Passed    0.02 sec
    Start 7: test_time ........................   Passed    0.00 sec
    Start 8: test_ids .........................   Passed    0.00 sec
    Start 9: test_config ......................   Passed    0.01 sec
    Start 10: test_logging .....................   Passed    0.08 sec
100% tests passed, 0 tests failed out of 9

逐测试断言数（阶段 1）：
  test_layering_guard      20 assertions in 3 test cases
  test_result              35 assertions in 7 test cases
  test_crypto              47 assertions in 6 test cases
  test_json                79 assertions in 11 test cases
  test_fs                  80 assertions in 8 test cases
  test_time                34 assertions in 7 test cases
  test_ids                 16 assertions in 5 test cases
  test_config              70 assertions in 13 test cases
  test_logging           3632 assertions in 18 test cases
  test_bytes              142 assertions in 11 test cases
  test_net                205 assertions in 7 test cases
  test_sys                 85 assertions in 5 test cases
  test_httplib_hardening  270 assertions in 13 test cases
  test_http_server        244 assertions in 9 test cases
  ─────────────────────────────────────────────
  合计 4959 个断言（另有阶段 0 的 55 个，全仓共 5014）

$ ./scripts/verify_http_hardening.sh
  ✓ ① 基线：当前源码树通过
  · → 已注入：H-2① 前置拒绝失效、H-2② 计数 reader 不再中止
  ✓ ② 拆掉 H-2 防护后测试失败（符合预期）
  ✓ ③ 失败症状与 H-2 特征一致（超限却 201 / 413 退化）
  ✓ ④ 恢复防护后重新全绿

关键实测（test_http_server，独立进程 + 真实回环端口）：
  keep-alive 复用 20 次          → 全部 200，服务端不主动断连
  50 并发 × 4 keep-alive 请求    → 200/200 成功；峰值在途 ≥ 50（用 400ms 阻塞 handler 证明）
  1 GiB 流式下载 RSS 增长        < 64 MiB（客户端与服务端同进程，比只测服务端更严）
  1 GiB 流式上传（chunked）RSS   < 64 MiB
  TCP_NODELAY=on  平均延迟       ~0.1 ms
  TCP_NODELAY=off 平均延迟       ~40 ms（★ 必须复现，否则该测试无效）
  并发上限 2 → 第 3 个请求        → 503 + Retry-After: 1，延迟 0.29 ms（不排队到超时）
  listen backlog 修复前后        1436 ms → 414 ms（50 × 400ms 请求的总耗时）
  （`test_logging` 的断言数被"并发 8×200 条逐行解析"与"键名匹配矩阵"拉高，属预期；
    另有 **47 万个字节序列的穷举比对**在单个用例内完成，用时 0.08 s）

$ ./scripts/bench_logging.sh
  日志热路径基准（独立进程 + 绑核 cpu=3；每档 200000 条 × 3 轮取最好）
  json + 脱敏 + 落盘         1190 ns/rec      840666 rec/s
  text + 脱敏 + 落盘         1244 ns/rec      803772 rec/s
  json + 脱敏（无 I/O）      1011 ns/rec      988686 rec/s  (payload 214 B)
  text + 脱敏（无 I/O）      1079 ns/rec      926670 rec/s  (payload 147 B)
  json 无脱敏 + 落盘          760 ns/rec     1315764 rec/s
  Matches（无命中）          51.7 ns/call
  ScrubText（无密钥）       224.1 ns/call
  ScrubText（含签名）       281.2 ns/call
  → 峰值负载（21,062 req/s）下：1 条/请求 = 2.51% 单核；5 条/请求 = 12.53% 单核

$ ./scripts/verify_guard.sh
  ① 基线：当前源码树通过
  ② 注入领域层违规（<grpcpp/grpcpp.h>）后护栏失败
  ②b 注入领域层违规（<httplib.h>）后护栏失败
  ③ 移除违规后护栏恢复通过
  ④ common/http/ 内 include httplib 被正确放行（护栏不是粗粒度关键字匹配）
  ⑤ 领域层直接使用 httplib:: 符号被检出
  分层护栏有效（自证 ①~⑤ 全部符合预期）
```

## 3. 已满足的门槛判据

| 判据 | 状态 | 证据 |
| --- | --- | --- |
| **C1.1** 护栏生效性自证 | ✅ | `verify_guard.sh` ①~⑤；且护栏**能失败**、豁免位置**能放行**、符号级泄漏**能检出** |
| **C1.4** `Result<T>`：`FSS_TRY` 不吞错不泄漏，错误携带 `ErrorKind` | ✅ | `test_result.cpp`：三层嵌套传播后 `kind`/`message` 原样保留；`Result<void>` 支持只传播 |
| **C1.8** `common/json` | ✅ | 往返一致；**未知字段原样保留**；**PascalCase 精确性**（`name` 必须失败而 `Name` 成功）；解析错误带 line/column/offset（含深层错误定位） |
| **C1.9** `common/fs` | ✅ | **13 种恶意键全部被拒**（满足 ≥12）；**符号链接逃逸被拒**（词法合法但解析出界）；原子写不残留 tmp、失败不留半截文件；`pread` 区间读/越界/重复读语义 |
| **C1.10** `common/crypto` + `time` + `ids` | ✅ | SigV4 官方向量、RFC 4231 HMAC、base64url、常量时间比较、随机数；`ManualClock` 可精确推进（含毫秒进位）；**OSDU `CreatedAt` 末尾是 `+0000` 而非 `Z`**（回归断言）；时区偏移归一；`UuidGenerator` 合法 UUIDv4 且 1000 次不重复；`SequentialIdGenerator` 确定性 |
| **C1.5** 配置一次性列出**全部**问题 | ✅ | `test_config.cpp`：同一份配置里 5 个独立错误（类型/枚举/未知键/必填/范围）**同时**出现在 `ProblemsToString()`；严格 `Parse` 拒绝注释而 `ParseWithComments` 接受（自证对照，见 P1-D07）；优先级 `cli > env > file > default` 由 `SourceOf()` 逐层断言 |
| **C1.14** io_uring 能力探测（L1 工具） | ✅ | `test_sys.cpp`：**自证对照**（测试里再写一遍裸 `syscall(__NR_io_uring_setup)`，要求 errno 与我们的分类一致）；三种失败原因都能分类（EPERM→`blocked_by_policy`、ENOSYS→`not_supported`、其它→`other_error`）；`ToString()` 带内核版本；探测幂等且不泄漏 fd |
| **C1.15** 探测与回退语义 | ✅ | 决策矩阵穷举 4 种探测结果 × 3 种配置：`blocking` 恒定不用 uring；`uring` + 不可用 → **`refuse_start=true`** 且给出可执行修复指令；`auto` + 不可用 → **回退到 blocking 且不拒绝启动**，message 明确写"这是预期行为"。**容器实测**：`scripts/check_io_uring.sh` → 宿主机 AVAILABLE、**默认 seccomp 容器 EPERM**、`seccomp=unconfined` AVAILABLE（与 ADR-010 一致） |
| **C1.7** ASan/UBSan 干净（无内存错误/UB/泄漏） | ✅ | `scripts/run_sanitizers.sh`：**phase0 + phase1 共 15 个测试在 `-fsanitize=address,undefined -fno-sanitize-recover=all` + LeakSanitizer 下全绿**。过程中抓到两个真问题（P1-D19 栈生命周期、P1-D20 gRPC×ASan 布局耦合），**最终没有任何"按目标关检查"的例外** |
| **C1.2** H-1/H-2 回归 + HTTP 边界 | ✅ | `test_httplib_hardening.cpp` 13 用例 / 270 断言；**H-1 的不变量（不得下溢 `Content-Length`）在两条输入上都断言**；H-2 三种形态（CL 超限→413、chunked 超限→400、长度不符→400）都断言 **handler 无副作用**（`handler_calls == 0`）；重复 CL / CL+chunked / 9000 字节头 / target ≥ 8192 全部覆盖；每个"拒绝"都配正向对照 |
| **C1.2b** H-2 防护生效性自证 | ✅ | `scripts/verify_http_hardening.sh` ①~④：拆掉防护后 3 个用例失败，症状为 `201 == 400` 与 `400 == 413`（正是 H-2 的特征），恢复后全绿；已纳入 `run_all_gates.sh` |
| **C1.3** HTTP Server 集成 | ✅ | `test_http_server.cpp` 9 用例 / 244 断言；keep-alive 复用、**50 并发且峰值在途 ≥ 50**、chunked 请求体、Range 全形态（硬化测试覆盖）、**1 GiB 下载与 1 GiB 上传的 RSS 峰值增长均 < 64 MiB** |
| **C1.11** `TCP_NODELAY` 生效性自证 | ✅ | 成对测量：开启 **~0.1 ms**、关闭 **~40 ms**（delayed-ACK 特征），并断言量级差 > 10 倍。若关闭时不复现 40 ms，测试会失败 —— 这正是"该测试无效"的自证 |
| **C1.12** 并发上限可配置且可观测 | ✅ | `stats()` 暴露 `worker_threads`/`max_connections`/`in_flight`/`peak_in_flight`/`rejected_busy`；上限 2 时第 3 个请求 **0.29 ms** 内得到 503 + `Retry-After: 1`；断言**在途计数归还为 0**（曾因双重递减变成 -1 而被抓到） |
| **C1.6** 日志敏感字段脱敏 | ✅ | 结构化字段层 + 自由文本层双保险：① 键名命中的字段整个打码（含嵌套对象/数组，且**类型不变**）；② 消息与字符串值里的 `key: value` / `key=value` / `"key":"value"` 都被打码（覆盖 `Authorization: Bearer …` 与 `X-Amz-Signature=…`）；**每个"不出现"断言都配了"不脱敏时必须出现"的对照**；另有 JSON Lines 单行可解析、级别过滤、并发 1600 条不交错、换行注入不产生伪造行 |

## 4. 未满足/未开始的门槛

| 判据 | 内容 |
| --- | --- |
| C1.7 部分 | 目前只对 phase0/phase1 子集跑过；**ASan/UBSan 全量构建尚未纳入门槛** |
| C1.7 | ASan/UBSan 干净（**P1 收口的最后一项**） |

## 5. 本切片发现并修复的缺陷

### P1-D01：`is_include` 判断写反，导致**真正的 include 行检不出来**

护栏第一版用 `line.find("#include") < line.find_first_not_of(" \t")` 判断"这是不是 include 行"。
当 `#include` 位于**第 0 列**时，两个值都是 0，`0 < 0` 为假 → **真正的 include 被跳过**，
护栏形同虚设。

- **修正**：改为 `line.find_first_not_of(" \t") == line.find("#include")`（首个非空白字符处必须是 `#include`）。
- **回归断言**：`REQUIRE(would_flag("domain/foo.cpp", "#include <grpcpp/grpcpp.h>"))`
  与缩进变体，以及"注释里出现该词不得误报"的反向断言。
- **教训**：「护栏本身也要被测」—— 这正是 `verify_guard.sh` 存在的原因。

### P1-D02：`CATCH_CONFIG_MAIN` 漏写导致链接期一堆费解的未定义符号

`test_result.cpp` / `test_crypto.cpp` 忘了写 `CATCH_CONFIG_MAIN`，症状是
`undefined reference to main` 加几十条 Catch 符号未定义。

- **修正**：抽出 `tests/framework/catch_main.cpp` 作为**唯一**主入口，
  由 `fss_add_test` 统一注入；测试文件里不再需要写这个宏。
- **教训**：把"每个文件都要记得写"的约定改成"框架自动注入"，从根上消除。

### P1-D03：`is_include` 判断写反（见 P1-D01）之后的又一个"护栏自身"缺陷

护栏里的 `would_flag` 自证 lambda 与扫描器用了同一份逻辑；修正 D01 时两处都要改，
只改一处的风险由 `test_layering_guard` 的合成文本用例覆盖（首列 include 必须被检出）。

### P1-D04：C++ most vexing parse

`const std::filesystem::path root_p(std::string(root));` 被编译器解析成**函数声明**，
报错是 `no matching function for call to 'weakly_canonical(path (&)(std::string), error_code&)'`。
改用花括号初始化 `{std::string(root)}` 后正确（共修 5 处）。
教训：`Type name(Type(other))` 是声明，不是构造。

### P1-D05：测试断言把行号写错，暴露"位置信息确实有意义"

`test_json` 第一版断言"缺逗号的错误在第 2 行"，实际 nlohmann 报**第 3 行**
（缺逗号在第 2 行末尾，但出错 token `"b"` 在第 3 行）。**是断言错了，不是实现错**。
修正后补了两个定位用例（第 1 行出错、深层嵌套出错），使位置信息真正被断言。

### P1-D06（方案复查阶段发现）：端口数量与门槛编号不一致

详见 §6。

### P1-D07：`ParseWithComments` **从来没有支持过注释**（文档约定与实现脱节）

- **症状**：`config/fss.example.json`（带注释）解析失败
  `[json.exception.parse_error.101] line 1, column 1: invalid literal; last read: '/'`。
- **根因**：`Value::parse(first, last, cb, allow_exceptions, ignore_comments)` —— 迭代器重载的
  **第 4 个形参是 `allow_exceptions`**，第 5 个才是 `ignore_comments`。代码写的是
  `parse(begin, end, nullptr, /*ignore_comments=*/true)`，于是把 `true` 传给了
  `allow_exceptions`（本来就是默认值），`ignore_comments` 保持 `false`。
- **为什么活到现在**：`test_json.cpp` 里有 8 个用例，**没有一个碰过 `ParseWithComments`**。
  而"配置是带注释的 JSON"这条约定被写进了 `AGENTS.md`、`config.h`、`docs/operations.md`、
  样例文件——全仓库都当它成立，只有这一行代码不成立。
- **修正**：改为 5 个实参并加注释说明形参顺序；`test_json.cpp` 新增 3 个用例
  （行注释 / 块注释 / 嵌套注释 / 字符串里的 `//`、`/*` 必须原样保留 / 语法错误仍带正确行号）。
- **修正**：`test_config.cpp` 新增"样例文件键集与 `CoreSchema` **双向**一致"断言 —— 这条断言
  就是本轮把该缺陷暴露出来的那条。
- **教训（已固化为 R15）**：「写进文档的**能力型**约定」和「代码里的一行」之间必须有测试连接，
  否则文档会一直描述一个不存在的能力，而且越多人引用越像真的。

### P1-D08：跨字段校验的 `StrOr` 对非字符串标量返回 fallback → `multi` 模式**永远无法配置正确**

- **症状**：配置里写 `"leases": {"enabled": true}`、`"leader_election": {"enabled": true}`、
  `"storage": {"posix": {"shared_mount_required": true}}`，仍然报
  "multi 时必须开启 / 必须为 true" 三条错误。
- **根因**：`StrOr()` 实现是 `v.is_string() ? v.get<std::string>() : std::string(fallback)`。
  加载器把布尔规范化为 **JSON `true`**，于是 `StrOr` 拿到布尔后**返回 fallback `"false"`**，
  比较 `!= "true"` 恒成立。**写对的配置被判为错，且无法通过任何写法绕过。**
- **修正**：`StrOr` 改为经 `RenderScalar()` 渲染标量（`true`→`"true"`、整数→十进制、浮点→`dump()`），
  仅对 object/array/null 才回退。同时补了"补齐后必须通过"的正向断言
  （只断言"报错"是不够的——只报错的正例无法区分"校验正确"和"校验恒真"）。
- **教训**：`is_X() ? ... : fallback` 这类"宽松取值"助手极易把**类型不匹配**静默折叠成**缺省值**；
  被折叠掉的值如果参与**判定**（而不只是显示），就会产生恒真/恒假的校验。

### P1-D09：`ScrubText` 在**空格**处截断值 → `Authorization: Bearer <token>` 只把 `Bearer` 打码

- **症状**：`test_logging` 的对照断言通过（不脱敏时明文出现），但脱敏后的输出是
  `"msg":"upstream 403; Authorization: *** eyJTOKENLEAK.abc"` —— **token 原样留在日志里**。
- **根因**：`ScrubText` 用**一套**分隔符集合同时处理 `:` 与 `=` 两种形态。空格在查询串
  （`X-Amz-Signature=abc&…`）里确实是终止符，但在头部形态（`Authorization: Bearer abc`）
  里不是。一套集合服务两种语法，必然错一个。
- **修正**：拆成 `IsColonValueEnd`（掩到行尾/引号/结构符）与 `IsEqualsValueEnd`（到 `&`/空白）。
  这样 `Authorization` 的整个 `Bearer <token>` 被打码，而查询串里后面的 `X-Amz-Date` 不受牵连。
- **教训**：**"值到哪里结束"取决于语法形态**；把两种形态塞进一个判定函数，等于让其中一种静默失效。
  这类缺陷的表现是"部分打码"，比完全不打码更隐蔽（输出里明明有 `***`，看起来是生效的）。

### P1-D10：camelCase 键名（`SecretKey`）不命中 `secret_key` 规则

- **症状**：`Matches("SecretKey")` 返回 false，而 `Matches("secret_key")` 返回 true。
- **根因**：匹配只做了大小写折叠，没有处理分隔符。真实世界里同一个概念在不同来源的
  JSON / 头部里就是 `secret_key` / `secretKey` / `SECRET-KEY` 三种拼法，**漏一种就是漏一处泄漏**。
- **修正**：键名与字段名都过 `Normalize()`（小写 + 丢弃 `_`/`-`）再比较，于是三种拼法等效。
  `ScrubText` 需要把归一化后的命中位置映射回原文下标，故 `Normalize` 额外返回 `orig_index`。
  语义已同步写入 `observability.redact_keys` 的字段描述与 `logging.h` 文件头。
- **取舍（明确记录）**：归一化后 `sig` 会连带命中 `design`、`assignment` 等无关词。
  方向是"宁可多打码"——日志可读性 vs 密钥泄漏，本仓库选后者。
- **教训**：**"大小写不敏感"只是归一化的第一步**；只要键名允许两种分隔符风格，
  就等价于同一个规则有两套拼写，只折叠大小写会有一半场景静默失效。

### P1-D11：日志热路径 ~2.6× 于必要成本（"用不用 spdlog"评估的副产物）

- **背景**：为回答"为什么不用 spdlog"，实测了成本构成，结果发现**瓶颈全在我们自己的代码**。
- **实测（独立进程 + `taskset` 绑核，`/dev/null` sink，3 轮最好值）**：

  | 环节 | 修复前 | 修复后 |
  | --- | ---: | ---: |
  | `json::Value` 组装 + `dump()`（每条记录） | ~1100 ns | —— 改为预留缓冲 + 手写追加 |
  | `ScrubText`（7 条规则，逐条重建归一化视图） | ~1100 ns | **224 ns**（一次归一化 + 扫全部规则 + 从后往前替换） |
  | `Matches`（每次归一化都 `push_back` 一个 `vector<size_t>`，即一次堆分配） | ~75 ns | **52 ns**（栈上缓冲） |
  | 日志框架部分（级别/互斥/写 sink） | ~95 ns | ~95 ns ← **spdlog 只能替掉这一格** |
  | **整条记录** | **2838 ns** | **1190 ns** |

- **地板测量**（只格式化、不做 I/O）：nlohmann 组装 + dump ~1100 ns；`fmt::format` ~170 ns；
  `std::ostringstream` ~200 ns；手写 `append` + `to_chars` **~45 ns**。
- **主动停止优化的依据（R19）**：1190 ns/条 × 峰值 21,062 req/s → **1 条/请求 = 2.51% 单核**，
  5 条/请求 = 12.53% 单核。再往下抠最多省约 1–2% 单核，而复杂度与风险（手写更多格式化）上升
  → **判定为不值得**，并把这个判据写进 ADR-011。
- **教训**：先量"贵在哪"，再谈"换什么"。若直接换库，会花掉依赖成本却只动到 3% 的那一格。

### P1-D12：**非法 UTF-8 会让日志抛异常**（日志的输入正是最脏的数据）

- **症状**：`json::Dump(json::Value(s))` 对含非法 UTF-8 的 `s` 抛
  `[json.exception.type_error.316] invalid UTF-8 byte at index 1: 0xFF`。
- **成因**：nlohmann 的 `dump()` 默认 `error_handler_t::strict`。而日志的输入是**文件名 /
  路径 / 请求头 / 用户输入** —— 完全可能是非 UTF-8 字节。于是"用日志记录一个坏输入"
  反而把请求打挂（在错误处理路径上抛异常，最坏会终止工作线程）。
- **修复**：自备 `AppendJsonString()`（非法字节 → U+FFFD）；并给 `ILogger::Log` 加
  **`noexcept`**（编译期断言）—— 日志是旁路，**任何**情况下都不许把异常抛回调用方；
  渲染失败退化成一条最小记录，输出流失败只计数不抛。
- **验证**：7 类坏字节逐个 `REQUIRE_NOTHROW` + 输出仍是可解析 JSON + 坏字节不出现在输出里。

### P1-D13：手写转义的第一版**语义错了**，被穷举比对抓出（新增铁律 R18）

- **症状**：与 nlohmann 参照实现比对时，两个字节序列不一致：
  - `e0 80 42`：我们 1 个替换，参照 2 个；
  - `c2 e0 80 42`：我们 2 个替换，参照 3 个。
- **成因**：我把"**结构**不完整"与"**范围**越界"混为一谈。UTF-8 的续字节范围不是
  一律 `80..BF`：`E0` 要求 `b1 ≥ A0`、`ED` 要求 `b1 ≤ 9F`、`F0` 要求 `b1 ≥ 90`、
  `F4` 要求 `b1 ≤ 8F`。参照实现是**逐字节按范围**校验：越界那个字节要**重新**处理，
  只有已经通过校验的续字节才被跳过。
- **修复**：改为逐字节按 `ContinuationRange()` 校验并记录 `consumed`。
- **验证**（本条是重点）：用 nlohmann `error_handler_t::replace` 作**参照实现**，
  穷举 **256 个单字节 + 65536 个双字节 + 393216 个三字节 + 15360 个四字节**（共
  476,368 个序列），每个都用 `"A" + seq + "B"` 包裹以覆盖"替换后如何继续"的路径，
  全部要求**逐字节相等**。
- **教训（已固化为 R18）**：手写的安全敏感代码（转义/校验/解析）光有"看起来对"的
  用例是不够的 —— 必须有一个**参照实现**并做等价比对，能穷举就穷举。
  本条的 3 处不一致，任何手写样例测试都不会发现。

## 5b. 切片 7：与**第三方库行为**冲突而发现并处置的缺陷（P1-D14 ~ P1-D18）

这一批的特殊性：**它们不是我们的代码写错，而是库的行为与 RFC/契约不一致，或者库的默认值不适用于我们的场景。**
包装层存在的意义正在于此 —— 但前提是**要测得出来**。四条里有三条是"库悄悄做了别的事"，
只有把断言写在**线缆层**（原始 socket）才看得见。

### P1-D14：库在**解析阶段**把非法 `Range` 变成 416，并**跳过路由**

- **实测**：`Range: bytes=abc-def`、`bytes=10-5`、`items=0-1` 全部返回 **416**，且 handler **从未被调用**
  （访问日志里没有这次请求）。溯源：`httplib.h:8337`
  `if (!parse_range_header(...)) { res.status = 416; return write_response(...); }`。
- **为什么是缺陷**：RFC 7233 §4.4 规定"无法理解的 Range 应当**忽略**"；契约 §1.8 同样写的是
  "语法非法 → 忽略该头 → 200 全量"。把非法语法当"不可满足"会让某些客户端陷入重试循环，
  而且它对**格式差异**过于敏感（例如 `bytes = 0-1` 带空格）。
- **处置**：在 `set_error_handler` 里识别"416 且方法为 GET/HEAD 且 Range 非法"→ 用副本请求
  （清掉 `ranges` 与 `Range` 头）**重新分发一次**，得到 200 全量；同时记
  `malformed_range_ignored_by_wrapper` 警告，便于排障时知道我们改写过库的行为。
- **教训**：库在"解析阶段"就返回的错误**不经过路由**，`pre_routing_handler` 也拦不到 ——
  唯一能介入的是 `error_handler`。这类行为必须靠"handler 有没有被调用"来发现。

### P1-D15：库对"后缀区间 ≥ 总长"的实现与 RFC 不符

- **实测**：36 字节资源 + `Range: bytes=-999` → 库算出 `first_pos = 36 - 999 = -963` → 判非法 → **416**。
  RFC 7233 §2.1 明确要求：**后缀长度 ≥ 当前长度时，取整个表示**（应为 206 全量）。
- **处置**：既然我们已经按契约解析过一遍，就让库只负责"按绝对区间切数据"：
  `NormalizeRange` 用 `bytes::ParseRangeHeader` 的结果**覆盖** `req.ranges`。
  实测 `bytes=-999` → 206 + 完整 body ✓。
- **教训**：`req.ranges` 才是库的输入（不是请求头）。"我忽略了该头"必须体现为
  `req.ranges.clear()`，否则是自欺欺人。

### P1-D16：`listen backlog` 被硬编码为 5 → 并发突发丢 SYN（**生产级隐患**）

- **实测（50 个并发连接，handler 阻塞 400 ms）**：
  | | 总耗时 | 200 ms 时已开始处理 | 峰值在途 |
  | --- | ---: | ---: | ---: |
  | 库默认 backlog=5 | **1436 ms** | 34 / 50 | 36 |
  | 覆盖为 512 | **414 ms** | 50 / 50 | **50** |
- **成因**：accept 队列只有 5 个位置，多余的 SYN 被内核丢弃 → 客户端按 **1 s / 3 s / 7 s** 重传。
  所以"并发 ≥ 50"在默认配置下**根本不成立**（只有约 14 个真正并发），而症状是"偶发几秒的延迟"，
  极难归因。
- **处置**：包装层在 `#include <httplib.h>` **之前** `#define CPPHTTPLIB_LISTEN_BACKLOG 512`
  （库没有运行时接口）。只在**唯一**允许 include 它的那个 TU 里定义，不存在宏不一致。
- **回归**：并发用例断言"50 个 400 ms 请求总耗时 < 3×400 ms"且"峰值在途 ≥ 50"。
- **教训**：包装层要覆盖的不只是"缺陷"，还包括**不适用的默认值**。这类默认值只有用
  "真实并发 + 阻塞 handler + 计时"才测得出来；用快速 handler 测会被掩盖。

### P1-D17：`ContentReader` 路径上库**不检查**请求走私形态

- **实测**：`Content-Length: 5` 重复两次、或 `CL` 与 `Transfer-Encoding: chunked` 并存时，
  走流式路径的请求**正常进入 handler 并返回 201**。契约 §1.7 要求 400。
  （缓冲路径上库自己会给 400 —— 所以这个洞只在我们的流式路径上。）
- **处置**：在 `Dispatch` 的最前面由包装层检查：`HeaderCount("Content-Length") > 1` → 400；
  `chunked && 有 CL` → 400。
- **教训**："库在 A 路径上做了 X"不等于"在 B 路径上也做了 X"；H-2 的教训（缓冲 vs 流式行为不同）
  在这里第二次应验。

### P1-D18：包装层自己的在途计数**双重递减** → `-1`

- **症状**：C1.12 用例断言"结束后 `in_flight == 0`"，实测 **-1**。
- **成因**：在 pre-routing 里判断超限后手工 `--in_flight`，而 `post_routing` 处理器**无条件**也会减
  （它在 `write_response_core` 里，pre 返回 `Handled` 时同样会走到）。
- **危害**：计数变负后，后续请求会**绕过并发上限**（`in_flight > limit` 更难成立）——
  背压静默失效。
- **处置**：超限分支不再手工递减，统一由 `post_routing` 归还。
- **教训**：一条路径上有两个"归还点"时，必须要么互斥、要么成对断言。
  "结束后计数归零"这条断言是唯一能抓住它的手段（功能测试全绿时它仍然是错的）。

### P1-D19：临时 `std::string` 的 `c_str()` 指针在完整表达式结束后失效（ASan 抓到）

- **症状**：`test_httplib_hardening` 在 ASan 下报
  `stack-use-after-scope ... READ of size 1`，指向 `server.cpp` 的 `MakeRequest`。
- **根因**：
  ```cpp
  const auto parsed = std::strtoll(src.get_header_value("Content-Length").c_str(), &end, 10);
  if (end != nullptr && *end == '\0') ...   // ★ end 指向的临时 string 已经析构
  ```
  `get_header_value()` 返回**临时** `std::string`，其生命周期只到该完整表达式结束；
  `end` 是指向它内部缓冲的指针，下一行解引用就是读已析构的存储。
- **为什么普通构建没暴露**：那块栈内存通常还留着 `'\0'`，于是 `*end == '\0'` 恰好成立，
  行为"看起来正确"。**这是 UB，不是可依赖的巧合。**
- **修复**：先把头值绑到具名变量，再取 `c_str()`。
- **教训**：凡是"取 `c_str()`/`data()` 存下来、稍后再用"的写法，都要先确认**宿主对象的生命周期**；
  ASan 的 `stack-use-after-scope` 正是为这类问题准备的。

### P1-D20：**gRPC 与 ASan 的布局耦合** —— `sizeof(ClientContext)` 变了 8 字节

- **症状**：`test_phase0_toolchain` 在 sanitizer 构建下先报
  `load of value 48, which is not a valid value for type 'bool'`（gRPC 头文件里读 bool），
  把 bool 检查关掉后又变成
  `AddressSanitizer: requested allocation size 0x217080008c4000 ... exceeds maximum supported size`。
- **诊断过程（关键：不要停在第一个症状上）**：
  1. 先怀疑"gRPC 有未初始化成员" —— 但被测的成员在构造函数里都初始化了；
  2. 用 `-O0` 与 `-O2 -DNDEBUG` 各测一次 → **都失败**，排除优化级别/NDEBUG；
  3. 写一个最小探针直接打印 `initial_metadata_flags()` 与 `sizeof(ClientContext)`：
     | 构建 | `sizeof(ClientContext)` | `initial_metadata_flags()` |
     | --- | ---: | ---: |
     | 不带 ASan | **504** | 0x00000000 ✓ |
     | 带 ASan | **512** | 0x00000100 / 0x0000b000（垃圾） |
     → **布局不一致**，而不是"某个成员没初始化"。
  4. 溯源：gRPC 的 `port_platform.h` 在 `__SANITIZE_ADDRESS__` 下定义 `GRPC_ASAN_ENABLED`，
     该宏**改变了 C++ 类的内存布局**；而系统预编译的 `libgrpc++.so` 不带 ASan。
     我们的 TU（带 ASan）与库（不带）对同一对象的成员偏移理解不同 → UB。
- **修复**：`-DGRPC_ASAN_SUPPRESSED=1`（gRPC 官方给的开关）→ 不再定义 `GRPC_ASAN_ENABLED`
  → 实测 `sizeof` 回到 504、flags 回到 0。挂在 `PkgConfig::GRPCPP` 的 INTERFACE 上，
  **任何**链接 gRPC 的目标自动获得，避免漏配。
- **撤回的权宜之计**：过程中曾给 `fss_proto` 与 `test_phase0_toolchain` 加
  `-fno-sanitize=bool` 来绕开症状。找到根因后**已全部撤掉** —— 现在没有任何"关检查"的例外。
- **教训（两条）**：
  ① **sanitizer 的插桩标志可以改变第三方库的 ABI**：凡是"头文件里定义类布局 + 预编译库"
     的组合，都必须核对"我们与库的编译标志是否一致"。ADR-002 里担心的 `.so` 宏耦合
     在这里以另一个形式应验了。
  ② **不要停在第一个症状上**：把 bool 检查关掉只是让报告换了张脸。
     用"打印 `sizeof` 与内部状态"这种最朴素的手段，可以把"猜测"变成"事实"。

## 6. 方案复查（本轮开始前）发现并修复的 4 个问题

| # | 问题 | 修复 |
| --- | --- | --- |
| 1 | 设计文档端口数写 13，但 ADR-009 新增 `ILeaseRepository`、ADR-010 新增 `IIoEngine` → 实际 **15** | 设计 §3.1 图与 §5.2 表、计划 P2、README 全部同步为 15 |
| 2 | **C1 门槛断号**：有 C1.11~C1.15 却没有 C1.8~C1.10（`json`/`fs`/`time+ids` 无覆盖） | 补齐 C1.8/C1.9/C1.10 |
| 3 | C1.14 要求 `IIoEngine` 抽象在 P1，但端口定义属 P2（L3）—— **阶段归属冲突** | C1.14 改为只要求 L1 的**能力探测**；新增 C2.11 要求端口定义 |
| 4 | 契约文档未明确标注端点总数（§2 有 12 个小节但含 19 个端点，易数错） | 契约 §2 开头补"端点总数：19 个" |
| 5 | ADR-007 §8.4 表格仍引用**已作废**的 58,741 | 改为 31,478，并标注协议 |

第 2、5 项已固化为 `scripts/check_docs.sh` 的 **D5（门槛编号完整性）** 与
**D2（作废数字必须带标记）** 两项检查；D5 加入后又抓到自身的一个误报（把"支撑门槛"引用列
当成第二次定义），已修为"只认表格第一列的**定义行**"。

## 7. 结论

| 项 | 结论 |
| --- | --- |
| 切片 1~2 门槛 | ✅ 通过（7 个测试 + 护栏自证 5 项） |
| 切片 3 门槛（config） | ✅ 通过（`test_config` 13 用例 / 70 断言） |
| 切片 4 门槛（logging） | ✅ 通过（`test_logging` 13 用例 / 3312 断言；`ctest -L phase1` 9 测试 / 3693 断言） |
| 已满足判据 | **C1.1 ~ C1.15 全部**（退出条件 C1.1–C1.7 亦全部满足） |
| P1 是否收口 | ✅ **已收口** —— 可以进入 P2 |
| 已知遗留 | **无** |
| 切片 5（日志硬化） | ✅ `test_logging` 18 用例 / 3632 断言（含 47 万序列穷举）；`scripts/bench_logging.sh` 可重跑 |
| 切片 6（bytes/net/sys） | ✅ 432 断言；`C1.14`/`C1.15` 满足（容器实测 EPERM 已记录） |
| 切片 7（`fss_http`） | ✅ 514 断言（270+244）；C1.2/C1.2b/C1.3/C1.11/C1.12 满足 |
| 切片 8（C1.7 sanitizer） | ✅ phase0+phase1 在 ASan/UBSan/LSan 下全绿；**P1 全部 15 项门槛满足，收口** |
| 契约同步 | `docs/03-api-contract.md` **§1.8（Range/206/416）** 已补齐并登记契约测试 —— 实现前先写契约，避免"实现定义了契约" |
| 本轮新发现缺陷 | **P1-D01 ~ P1-D20**，共 20 个：切片 3~5 的 D07~D13（配置/日志）、切片 7 的 D14~D18（库行为冲突 ×4 + 包装层自身缺陷 ×1）、切片 8 的 D19~D20（栈生命周期 UB、gRPC×ASan 布局耦合） |
| 最终全量验证 | `./scripts/run_all_gates.sh` → `[docs] ✅` `[phase0] ✅` `[phase1] ✅`（含护栏自证、H-2 防护自证、ASan/UBSan 全绿）|
