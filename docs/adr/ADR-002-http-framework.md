# ADR-002：使用上游 cpp-httplib（源码 vendored）作为 HTTP 传输层，并在其上自建强化包装层

- 状态：**已采纳（Accepted）—— 取代本 ADR 的初版结论（初版为"完全自研 HTTP 内核"）**
- 日期：2025（阶段 0 / 评审期修订）
- 相关文档：`third_party/README.md`、`docs/02-design.md` §7、
  `docs/appendix/httplib-hardening-probe/`、`docs/test-evidence/phase0.md` D-05

---

## 0. 修订说明（为什么推翻初版结论）

初版结论是"不引入任何第三方 HTTP 框架，自研最小 HTTP/1.1 内核"。评审时被质疑
"为何自研而不用现成开源模块"，复核后**确认初版推理有跳跃，结论错误**，故重写本 ADR。

初版的错误链条：

| 步骤 | 初版推理 | 问题 |
| --- | --- | --- |
| 1 | Ubuntu 的 `libcpp-httplib-dev` 只有声明头，实现是预编译 `.so`，且该 `.so` 以 `SSL/ZLIB/BROTLI` 宏编译，要求使用方宏完全一致，否则类布局不一致（UB） | ✅ **这一步的事实是对的**（已复核：`nm -D` 显示 53 个 `SSL_*`/`EVP_*`、7 个 `Brotli*` 未定义符号；头文件 1851 行仅声明） |
| 2 | 离线环境拿不到上游单头文件版本 | ❌ **错**。Ubuntu 镜像 **pool** 可浏览，`pool/universe/c/cpp-httplib/` 下同时存在 `cpp-httplib_0.10.3+ds.orig.tar.xz` 与 **`cpp-httplib_0.26.0+ds.orig.tar.xz`**（上游原版单头文件，0.26.0 为 11900 行 / 405 KB） |
| 3 | 因此应当自研 HTTP 内核 | ❌ **从"某个二进制包不可用"直接推出"必须自己写整个 HTTP 栈"，跳过了"换一个分发形态/版本"这一步**；也未评估 Boost.Beast、libmicrohttpd、civetweb 等候选 |

因此本 ADR 的结论改为：**用上游 cpp-httplib 源码（0.26.0 单头文件）作为传输层，
在其上自建一层薄强化包装（`:lang:fss_http`），把"安全与正确性边界"留在我们自己的代码里。**

---

## 1. 背景与需求

服务需要：

| # | 需求 | 说明 |
| --- | --- | --- |
| N1 | HTTP/1.1 服务端承载 OSDU REST（19 个端点，JSON 为主） | 需要路由、中间件、keep-alive、`Content-Length`/chunked |
| N2 | **数据面**：大文件字节流 | 流式收发（不整文件驻留内存）、`Range` → `206`/`416`、`HEAD`、请求体硬上限（`413`） |
| N3 | HTTP 客户端 | 出站调用（Entitlements/Storage/Partition）**由 libcurl 承担**；测试用客户端可用 httplib |
| N4 | 不引入需要 root 安装或联网拉取的依赖 | 本机无 root、`github.com` 直连不可达，仅 apt 镜像可用 |
| N5 | 不引入预编译二进制产物的 ABI/宏耦合 | 见第 0 节步骤 1 |

---

## 2. 候选方案与实测结论

### 候选 1：Ubuntu 二进制包 `libcpp-httplib-dev` (+ `libcpp-httplib0`) —— **否决**

- 只有声明头；实现是 `.so`，以 `CPPHTTPLIB_OPENSSL_SUPPORT`/`ZLIB`/`BROTLI` 编译
  → 使用方必须精确复刻宏集合，否则类布局不一致（UB）；复刻又需要本机没有的 `libbrotli-dev` 头。
- **jammy 只提供 0.10.3**，而该版本在本项目必需的路径上有**协议级缺陷 H-1**（见 §4），**必须避开**。

### 候选 2：上游 cpp-httplib 单头文件（0.26.0），源码 vendored —— **采纳**

获取方式（无需 root，实测可行）：

```bash
curl -O https://mirrors.ustc.edu.cn/ubuntu/pool/universe/c/cpp-httplib/cpp-httplib_0.26.0%2Bds.orig.tar.xz
tar xJf 'cpp-httplib_0.26.0+ds.orig.tar.xz'
cp cpp-httplib-0.26.0+ds/httplib.h     third_party/httplib.h
cp cpp-httplib-0.26.0+ds/LICENSE       third_party/licenses/cpp-httplib-LICENSE
```

- 版本宏：`CPPHTTPLIB_VERSION "0.26.0"`；许可：MIT。
- `sha256(httplib.h) = 4a42da28ce477d06f53942c172197d3b83b4fe42b7e37b8af78c70e063c06da7`
- **单头文件、源码形式** → 无 ABI 耦合，可直接审计。

**实测能力（0.26.0 全部通过，见 §3 探针）**：
`set_content_provider`（流式响应）、`ContentReader`（流式请求体）、
`parse_range_header` + `Content-Range` + `206`/`416`（含 suffix range 与 `multipart/byteranges`）、
`HEAD`、chunked 请求体、keep-alive、`new_task_queue`/`ThreadPool`（并发）、
`set_pre_routing_handler`（前置拦截）、`set_mount_point`、`set_read_timeout`、
`CPPHTTPLIB_REQUEST_URI_MAX_LENGTH`(8192) / `CPPHTTPLIB_HEADER_MAX_LENGTH`(8192) 上限。

### 候选 3：Boost.Beast —— 否决

header-only 且质量高，但属 Asio 低层 API：路由、`Range`/`206`/`416`、keep-alive 管理、
请求体上限等**都要自己写**，工作量与自研相当；Boost 体积大、编译慢。**未做实测**
（理由：候选 2 已全项通过；如后续候选 2 出问题，应优先回来评估本候选）。

### 候选 4：GNU libmicrohttpd / civetweb / libevent-evhttp —— 否决

都是"头文件 + 预编译 `.so`"的 C 库，面临与候选 1 相同的 vendoring 与宏耦合问题；
且 `Range`/路由需自行实现。**未做实测**（同上理由）。

### 候选 5：完全自研最小 HTTP/1.1 内核（初版结论） —— 否决（降级为"包装层"）

放弃它的理由：

1. 候选 2 已覆盖全部功能需求，自研等于重复劳动；
2. 自研意味着**自己承担**请求走私防范、畸形请求处理、keep-alive 状态机、分块编码、
   multipart 等全部边界的正确性，且没有社区与模糊测试在后面兜底——这是更差的工程选择；
3. 用两个已复现的窄路径缺陷去论证"推翻整个库"是不成比例的。

**但保留其合理内核**：把"安全与正确性边界"放在我们自己的代码里（见 §5 的 `fss_http` 包装层）。

---

## 3. 实测探针（证据）

完整源码与原始输出：`docs/appendix/httplib-hardening-probe/`。

| 探针 | 覆盖 | 0.10.3 | **0.26.0** |
| --- | --- | --- | --- |
| `probe.cpp` | 8 MiB 流式 PUT（SHA256 比对）、`Range: bytes=100-199`、`bytes=-50`、越界、全量 GET、50 并发、重复 `Content-Length` | ❌ 越界 Range 失败 | ✅ 全部通过 |
| `raw_range.cpp` | 用原始 socket 观察线上响应字节 | ❌ `206` + `Content-Length: 18446744072717940225` + `Content-Range: bytes 999999999-8388607/8388608` | ✅ `416` + `Content-Length: 0` |
| `probe2.cpp` | chunked PUT、HEAD、64 KiB 头、未知方法/路径、keep-alive | — | ✅ chunked 201、HEAD 无体、64 KiB 头 → 400、未知 → 404、keep-alive 两次 206 |
| `probe413.cpp` | 超限请求体（缓冲 handler vs 流式 handler） | — | ⚠️ 缓冲路径 → **413**；**流式 `ContentReader` → 201 且 0 字节（缺陷 H-2）** |
| `probe_fix.cpp` / `probe_fix2.cpp` | 验证包装层修法 | — | ✅ `Content-Length` 超限 → 413；chunked 超限 → httplib 强制 400（读失败语义），按实测定义契约 |

---

## 4. 已知缺陷与回归测试（**必须由 `fss_http` 兜住**）

| ID | 版本 | 缺陷 | 后果 | 处置 | 回归测试 |
| --- | --- | --- | --- | --- | --- |
| **H-1** | 0.10.3 | `set_content_provider` + 越界 `Range` 无 416 分支，`get_range_offset_and_length` 返回的 `size_t` 下溢 | 响应头非法（`Content-Length` 为 2^64 量级、`Content-Range` 倒序）→ 客户端挂死或错帧 | **不使用 0.10.3**，升级到 0.26.0 | `test_httplib_hardening.cpp` 的 out-of-range 用例 |
| **H-2** | 0.26.0 | `ContentReader` + `set_payload_max_length` 超限：handler 被调用，reader 交出 0 字节，最终返回 **201** | **静默数据丢失**：上传"成功"而对象为空 | 包装层三重防护：① `set_pre_routing_handler` 按 `Content-Length` 前置拒绝 → 413；② 自封装计数 reader，超限立即中止；③ 读取字节数与 `Content-Length` 一致性断言 | `test_httplib_hardening.cpp` 的 oversize-CL / oversize-chunked / 长度不符 用例 |

### 4.1 阶段 1 实施期间**新发现**的库行为（已处置）

| ID | 行为（实测） | 后果 | 处置 |
| --- | --- | --- | --- |
| **H-3** | `listen backlog` 硬编码为 **5**（`CPPHTTPLIB_LISTEN_BACKLOG`，无运行时接口） | 并发突发 > 5 时内核丢 SYN → 客户端 1 s/3 s 重试。实测 50 个 400 ms 请求总耗时 **1436 ms**（本该 ~400 ms） | 包装层在 `#include <httplib.h>` 前 `#define CPPHTTPLIB_LISTEN_BACKLOG 512`；实测降至 **414 ms**、峰值在途 50；有回归断言 |
| **H-4** | 非法 `Range` 在**解析阶段**直接回 416 并**跳过路由**（`httplib.h:8337`） | 与 RFC 7233 §4.4（非法 → 忽略）冲突；`bytes = 0-1`、`bytes=10-5` 这类写法会拿到 416，客户端可能陷入重试 | 包装层在 `error_handler` 里重新分发为 200 全量，并记警告 |
| **H-5** | 后缀区间 `bytes=-N` 在 `N ≥ 总长` 时算出负起点 → 416（RFC 7233 §2.1 要求按整个表示处理） | 大后缀请求（合法）被误拒 | 包装层用自己的 `bytes::ParseRangeHeader` 归一化后**覆盖** `req.ranges`，库只按绝对区间切数据 |
| **H-6** | 多段的判定与 `res.status == 206` 强耦合：handler 自己设了 200 则 Range **完全不生效** | 越界/合法 Range 都被静默忽略 | 包装层对"已知长度的流式响应"把状态留给库决定（`dst.status = -1`），并在交给库之前完成归一化 |
| **H-7**（C9.32） | `httplib::ThreadPool` 的构造函数用 `threads_.emplace_back(...)` 逐个建线程；任一次 `pthread_create` 返回 `EAGAIN` 时，**已建好的 joinable `std::thread` 向量在栈展开中被析构** → `std::terminate`（实测容器 `--pids-limit=64` 下 `ExitCode=139`，**不是 OOM**）。该 terminate 发生在 `listen_internal()` 的 **runner 线程**里，`main()` 的顶层 catch 看不到 | 资源耗尽时进程以不可读的 139 结束；且**只加顶层 catch 修不了**（实测仍是 139 / 偶发挂死） | 包装层用自己的 `SafeThreadPool`（同语义）：任一失败先 `join` 已建线程再重抛；`Server::Start()` 再把 runner 线程的异常**在主线程重抛**；并且只等 `pool_ready`（`new_task_queue()` 成功返回）才算启动成功，避免与 `is_running_` 竞态挂死。见 `docs/test-evidence/phase9.md` §14 / `phase9-image.md` §10.12 |

**契约定义（按实测行为固化，避免"想当然"）**：

| 场景 | 期望 | 依据 |
| --- | --- | --- |
| `Content-Length` > 路由上限 | **413** | 包装层前置拦截（H-2 防护①） |
| chunked 且累计 > 路由上限 | **400**（非 413） | httplib 读失败语义；包装层中止读取后状态被强制为 400。这是**实测行为**，写入契约并由测试锁定 |
| `Content-Length` > 8192 的单个请求头 | 400 | httplib `CPPHTTPLIB_HEADER_MAX_LENGTH`（实测：8000 通过、9000 → 400） |
| **请求行/target ≥ 8192** | **连接被关闭且无响应** | ★ **实测更正**（原表按编译期常量写成 400/414）：≤ 8191 正常；≥ 8192 时解析请求行阶段直接关连接 |
| 重复 `Content-Length` | 400 | ★ 实测补充：**缓冲路径**库自己给 400；**流式（`ContentReader`）路径库不检查**，必须由包装层强制 |
| `Content-Length` 与 `chunked` 并存 | 400 | ★ 同上（包装层强制） |
| 方法不匹配（已知路径 + 未注册方法） | **404**（非 405） | 0.26.0 实测；如需 405，须在包装层用 `set_pre_routing_handler` 自行实现。**当前决定：接受 404 并写入契约**（对 OSDU 兼容性无影响，因为 OSDU 只定义已注册的方法） |

---

## 5. 决策

### 5.1 `fss_http` 包装层的职责边界

```
┌───────────────────────────────────────────────────────────────────┐
│ 再上层：adapters/http（OSDU 路由、DTO、错误映射）                  │
├───────────────────────────────────────────────────────────────────┤
│ fss_http  ← **本项目拥有**（src/common/http/）                     │
│   · Server 门面（隐藏 httplib 类型，只暴露 fss::http 的接口）        │
│   · 硬上限强制：body/header/URI/连接数/超时（含 H-2 三重防护）        │
│   · 中间件链（correlation-id / access-log / auth / body-limit）     │
│   · Range 边界归一化（`bytes=a-b` / `a-` / `-N` / 越界 → 416）      │
│   · 错误响应归一化（统一走 AppError 映射，不让库的默认 400 漏出）    │
│   · 可测试面：所有边界都有单元/集成测试                             │
├───────────────────────────────────────────────────────────────────┤
│ cpp-httplib 0.26.0（vendored 源码） ← **不可信传输层**              │
│   · TCP 监听、HTTP/1.1 解析、路由分发、响应写回、线程池              │
└───────────────────────────────────────────────────────────────────┘
```

**约束**：只有 `src/common/http/` 允许 `#include <httplib.h>`。
其余任何位置出现该 include → 分层护栏测试失败。
这保证"库可替换"：换掉 httplib 不必触碰领域层/应用层/适配层。

### 5.2 为什么"包装层"是必要的（而不是多余）

| 论点 | 说明 |
| --- | --- |
| 缺陷已在我们的路径上 | H-1/H-2 都发生在"流式 + 上限/Range"这条数据面路径，正是本项目最关键路径 |
| 库不提供"安全边界"抽象 | httplib 的默认 400、`set_payload_max_length` 的语义都不足以直接作为契约；必须有归一化层 |
| 可测试性 | 包装层让"上限必须被拒绝"这类断言成为**我们的**测试，而不是对第三方行为的信任 |
| 版本可替换 | 门面隔离后，升级或更换库的影响面被限制在一个目标内 |
| 不重复劳动 | 解析/路由/keep-alive/分块/multipart 仍由成熟库承担，我们只加必需的那几十行 |

---

## 6. 后果

**正面**

- 减少约 600–900 行自研 HTTP 代码及其对应的边界测试负担；
- 直接获得成熟的 HTTP/1.1 解析、keep-alive、chunked、multipart、线程池、压缩支持；
- 依赖是**源码**（单头文件、MIT），可审计、无 ABI 耦合、离线可构建。

**负面 / 成本**

- 引入了第三方代码与其缺陷风险 → 通过 §4 的三重防护 + 版本锁定 + 校验和 + 回归测试缓解；
- 需要维护"包装层"这一层间接性（约 200–400 行，含测试）；
- 升级 `httplib.h` 需要重跑 H-1/H-2 回归测试（已写入 `third_party/README.md` 的升级清单）。

**明确不做的事**

- 不使用 Ubuntu 的 `libcpp-httplib-dev`/`libcpp-httplib0` 二进制包（ABI 耦合 + H-1）；
- 不把 httplib 的类型泄漏到 `fss_http` 之外；
- 不因为"库已经处理了"就跳过边界测试（尤其上限、Range、chunked）。

---

## 7. 待办（阶段 1 执行）

- [ ] `src/common/http/` 建立 `fss_http` 目标：`Server` 门面 + 中间件链，**唯一**允许 include `httplib.h` 的位置
- [ ] `tests/integration/test_httplib_hardening.cpp`（阶段 1 门槛的一部分），必须覆盖：
  - [ ] H-1 回归：流式响应 + 越界 Range → **416**（不是 206、不是下溢 `Content-Length`）
  - [ ] H-2 回归：流式 handler + `Content-Length` 超限 → **413**，且 handler **不产生副作用**
  - [ ] H-2 回归：流式 handler + chunked 超限 → **400**，且 handler **不产生副作用**
  - [ ] H-2 回归：`Content-Length` 与实际读取字节数不符 → **400**（防御性断言生效）
  - [ ] Range 全边界：`bytes=0-0`、`bytes=-1`、`bytes=len-1-`、`bytes=len-`、越界、多段
  - [ ] 重复 `Content-Length` / `CL`+`chunked` 并存 / 非 `\r\n` 行尾 → 400
  - [ ] 64 KiB 请求头 → 400；超长 URI → 400/414
  - [ ] 50 并发 GET + keep-alive 复用
  - [ ] 大文件（≥ 1 GiB 虚拟）流式收发期间 **RSS 峰值增长 < 64 MiB**
- [ ] 在 `docs/03-api-contract.md` §1.7 增加"HTTP 层硬上限与拒绝语义"表（按 §4 的实测行为）
- [ ] CI 增加 `sha256sum -c third_party/CHECKSUMS.txt`
- [ ] 分层护栏增加一条：`httplib.h` 只允许在 `src/common/http/` 内被 include
