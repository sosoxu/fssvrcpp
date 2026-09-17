# 阶段 6 测试证据（进行中）

| 项 | 值 |
| --- | --- |
| 阶段 | P6（元数据记录语义完整化） |
| 状态 | 🚧 **进行中 —— 切片 2/6 完成**（切片 1：`SqliteMetadataRepository`；切片 2：校验和 C6.4） |
| 门槛命令 | `ctest -L phase6` |
| 退出码 | `0`（3 测试 / 458 断言） |

> 剩余：12 步序列的 6 个故障注入点（C6.3）、`getFileList` 语义（C6.6）、角色常量（C6.8）、
> GC 租约与幂等并发与 tmp 名唯一性（C6.11/C6.12/C6.13）、DMS/Delivery 语义收口（C6.7）、
> **1 GiB** 大文件搬迁 RSS（C6.9；64 MiB 流式已证，绝对上限 < 64 MiB 尚未测）、
> 远端 Storage Service 仓储、组合根改接 `SqliteMetadataRepository`。

---

## 1. 切片 1：SQLite 元数据仓储（C2.10 元数据侧 / C6.5）

| 路径 | 内容 |
| --- | --- |
| `src/infra/metadata/sqlite/sqlite_metadata_repository.{h,cpp}` | `(partition_id, id, version)` 主键、`is_latest` / `previous_version` 列、`data` 列存整条记录 JSON（往返无损）、`kind`/`name`/`created_at` 抽出列 |
| `tests/integration/test_sqlite_metadata_repository.cpp` | **C2.10 元数据侧**：与内存实现共用 `CheckMetadataRepositoryContract`；另加"版本链在数据库层面的真实形态"用例 |

### 三条数据库层面的不变量（都有可执行断言）

| 不变量 | 实现 | 断言 |
| --- | --- | --- |
| 幂等键 `(partition, file_source)` 唯一 | `ux_metadata_source ... WHERE is_latest = 1` | 契约"Create 幂等"用例：第二次 `Create` 返回**第一次那条**，且 `GetById(second.id)` → `kNotFound`、`List.total == 1` |
| 每个 id 只有一个最新版 | `ux_metadata_latest ... WHERE is_latest = 1` | 契约"版本链"用例 + **自证对照**：直接往库里插第二条 `is_latest=1` 必须被唯一约束拒绝（若索引缺失，这条会通过 —— 那样"只有一个 latest"就只是巧合） |
| `Update` 不得改写幂等键 | 用例层显式比较 `file_source` | 契约"Update 不得改写 (partition,file_source)"用例 |

> **R6 的落点**：两个唯一索引的谓词都含 `is_latest = 1`。去掉谓词会**阻断合法版本链**
> （同一 `file_source` 的后续版本会被唯一约束拒掉）—— 这正是 ADR-008/R6 记录过的那个坑。

### 一处刻意的实现选择

`List` 的过滤（`kind` / `name_prefix` / 时间区间）在 **C++ 侧**做，SQL 只负责"按 partition 取 latest 行"。
原因：`name_prefix` 需要读 JSON 里的 `data.Name`，而 SQLite 的 JSON1 扩展在 3.37 上**不保证编译进来**
（3.38 起才默认启用）。分区内记录数在本阶段可控，因此选择"先隔离、后过滤"，并把 `name` 单独抽成列
以便将来把过滤下推到 SQL。**这是有意的取舍，不是遗漏**。

---

## 2. 切片 2：校验和（C6.4）

契约依据：`docs/03-api-contract.md` §2.6 第 7 步与 §3.4 的 `File_Calculate_Checksum`。

> ⚠️ **本切片推翻了一句既有结论**（P6-D05）：计划里写的「客户端提供但不符 → `400` + 对象被删除」
> 与上游证据冲突，已按 **`docs/00-final-design.md` §5** 更正为「**服务端无条件覆写**客户端传入的
> `Checksum`/`ChecksumAlgorithm`」。证据：① 调研 §2.3「校验和：**服务端覆写**客户端传入的
> `Checksum`/`ChecksumAlgorithm`（至少 Azure 实现如此）」，第 7 步失败语义为 `—`（非致命）；
> ② 权威样例 `tests/conformance/fixtures/upstream/File_CorrectPayload.json` 客户端给的是
> `MD5("") = d41d8cd9…` 却声明 `ChecksumAlgorithm: "SHA-256"`，**期望响应 201**。
> 详见 §5 的 P6-D05。

### 交付物

| 路径 | 内容 |
| --- | --- |
| `src/common/crypto/crypto.{h,cpp}` | `ChecksumAlgorithm{kSha256,kSha1,kMd5}` + `ParseChecksumAlgorithm` / `CanonicalChecksumName` / `ChecksumHexLength` / `IsHexDigestOf` + **增量** `Hasher`（OpenSSL EVP） |
| `src/common/bytes/bytes.h` | `BufferSink`：写进调用方固定缓冲（"推式 → 拉式"的 L1 通用件），`BlobByteSource` 改为复用 |
| `src/app/usecases/usecases.cpp` | `StoreByteSource`（可定位读的存储字节源）+ `HashingSink` + `ComputeChecksumStreaming`；**跨 store 复制改为流式**；第 7 步改为"原生优先 / 不可用则流式回算 / **覆写两处**" |
| `tests/integration/test_metadata_lifecycle.cpp` | 6 用例 / 230 断言（覆写语义 + MD5 驱动 + ETAG 回退 + 真实 POSIX 栈的 RSS 上限与自证对照） |
| `tests/unit/test_checksum.cpp` | 6 用例 / 106 断言：算法名解析（接受/拒绝两向）、规范名与 hex 长度自洽、hex 结构校验、**RFC/FIPS 公开向量**（SHA-256/SHA-1/MD5，含"一百万个 `a`"）、`HexDigest` 取完即重置 |
| `tests/framework/fake_ports.h` | 能力替身新增两个**驱动替身**开关：`hide_checksum`（模拟驱动不报校验和）、`copy_checksum_override/_algorithm`（模拟 Azure 那样返回 MD5） |
| `scripts/run_all_gates.sh`（前置检查） | 注入残留机械防线（P6-D04，见 §5） |

### 判定表（每一行都有断言）

| 情形 | 期望行为 | 测试 |
| --- | --- | --- |
| 客户端**未提供** `Checksum` | 服务端计算 **SHA-256**，覆写 `data.Checksum`/`data.ChecksumAlgorithm` 与 `FileSourceInfo.*` **两处** | `客户端未提供校验和 → 服务端计算 SHA-256 并写回两处` |
| 客户端提供了**错值且不自洽**的值（与 golden 样例同形：`MD5("")` + 声明 `SHA-256`） | **仍然 `201`**；记录里是服务端算出的真实 SHA-256（自证对照 R1：若原样回传客户端值则断言失败）；persistent 对象**不删除**；幂等键被占用（再提交返回同一条 id） | `客户端提供的校验和被**覆写**（不是待校验的断言）` |
| 客户端按**自己声明的算法**给了**正确**值（`SHA-256` / `MD5` / `SHA-1`） | 同样被覆写 —— 记录里是**驱动侧**的算法与值（内存驱动恒为 `SHA256`） | `客户端点名别的算法（MD5/SHA-1）也**不改变**服务端结果` |
| 驱动原生给 **MD5**（Azure 风格；写法小写 `md5`） | 采用原生值，算法名写**规范名** `MD5`（C6.4 的"算法覆盖"） | `算法跟随驱动：原生给 MD5 → 记录写回 MD5` |
| 驱动原生**不可用**（`ETAG` / 算法为空 / 不是合法 hex / 无值） | **流式回算 SHA-256**；**绝不**把 `ETAG` 之类写进记录（"看起来有值"的假象） | `原生校验和不可用 → 回退**流式回算 SHA-256**` |

### 流式（RSS）证据 + 自证对照（R1）

对象 64 MiB，持久区驱动替身 `hide_checksum = true`（模拟"驱动不提供校验和"）→ 逼出**流式回算**路径。
跑在**真实 POSIX 栈**（staging / persistent 两个不同 `PosixBlobStore`，跨 store 流式复制）上：

```console
$ ./build/bin/test_metadata_lifecycle -s
  流式回算前后 RSS 增长 80 KiB（对象 65536 KiB，上限 8192 KiB）
  对照（整块读回）RSS 增长 65664 KiB
All tests passed (230 assertions in 6 test cases)
```

**为什么用内存适配器测不出这一条**：`InMemoryBlobStore::copy` 本身就在 RAM 里复制一份对象，
RSS 增长必然 ≈ 对象大小 —— 那是**适配器**的固有开销，会把"校验和是否流式"的信号淹没。
**自证对照**：同进程内再做一次故意的整块读回（`StringSink`），RSS 增长 65664 KiB；
若没有这一步，"增长 < 8 MiB"无法区分"实现流式"与"测量根本没生效"。

**sanitizer 构建下的取舍（如实标注）**：ASan 的影子内存/隔离区会把这条路径的 RSS 增长抬到
**约 10 MiB**（同一测试在 ASan 下的整块读回对照是 139392 KiB）—— 插桩下"流式"不再度量同一件事。
因此该上限可用 `FSS_TEST_RSS_LIMIT_KIB` 覆盖（`run_sanitizers.sh` 设为 512 MiB），
**紧的那个上限（8 MiB）在普通门槛里跑**。这与 `big_file.h` / `run_sanitizers.sh` 对 1 GiB 用例的
既有约定一致，是刻意的取舍而非"绕过失败"；插桩下仍保留"整块读回必须被测量抓到"这条对照。

---

## 3. 门槛命令与输出（累计）

```console
$ cmake --build build -j8 && ctest --test-dir build -L phase6 --output-on-failure
    Start 34: test_sqlite_metadata_repository ......   Passed    0.01 sec
    Start 35: test_metadata_lifecycle ..............   Passed    0.54 sec
    Start 36: test_checksum ........................   Passed    0.01 sec
100% tests passed, 0 tests failed out of 3

逐测试断言数：
  test_sqlite_metadata_repository   122 assertions in 2 test cases
                                    ← C2.10（元数据侧契约）+ C6.5（版本链 + 唯一索引自证）
  test_metadata_lifecycle           230 assertions in 6 test cases
                                    ← C6.4（覆写语义判定表 + 流式 RSS + 自证对照）
  test_checksum                     106 assertions in 6 test cases
                                    ← C6.4 的 L1 基座（算法解析 / hex 结构 / 公开向量）
  ─────────────────────────────────────────────
  合计 458 个断言 / 14 个测试用例 / 3 个测试
```

相关护栏（改动仓储 SQL 后必跑）：

```console
$ ./build/bin/test_sql_guardrail          → 9 assertions in 2 test cases（每条 SQL 必须带 partition_id）
$ ./build/bin/test_layering_guard         → 20 assertions in 3 test cases（L2 只依赖 L3 端口 + L1）
```

---

## 4. 判据进展

| 判据 | 状态 | 证据 |
| --- | --- | --- |
| **C2.10（元数据侧）** 同一套契约跑第二遍 | ✅ | `test_sqlite_metadata_repository` 复用 `CheckMetadataRepositoryContract`（与内存实现逐条相同） |
| **C6.5** 版本链 | ✅ | 契约的版本用例 + 切片 1 的"数据库真实形态"用例（3 版共存、1 个 latest、部分唯一索引自证） |
| C6.1（黄金样例字段级往返） | 🚧 部分 | 仓储侧已无损（`data` 列存整条 JSON）；REST 侧的全字段比对已在 P4 的 C4.2 覆盖，尚缺"经 SQLite 仓储往返"的组合用例 |
| **C6.4** 校验和（服务端计算并**覆写** / 算法跟随驱动 / 大对象流式） | ✅ | 切片 2：判定表 5 行全部有断言（未提供 / 客户端值被覆写 / 客户端点名别的算法 / 原生 MD5 / 原生不可用回退）+ L1 公开向量与算法解析两向断言。**旧表述**"客户端提供但不符 → 400 + 删除对象"已推翻（P6-D05） |
| C6.9 大文件搬迁 RSS | 🚧 部分 | 切片 2 已证 **64 MiB** 对象跨 store 搬迁 + 校验和回算的 RSS 增长 80 KiB，且有整块读回对照（65664 KiB）；**≥1 GiB + 绝对上限 < 64 MiB** 尚未测 |
| C6.2 / C6.3 / C6.6 / C6.7 / C6.8 / C6.11 / C6.12 / C6.13 | ⬜ 未开始 | 见开头"剩余" |

---

## 5. 本阶段发现并修复的缺陷

| 编号 | 症状 / 根因 | 复现方式 | 修复 |
| --- | --- | --- | --- |
| ~~**P6-D02**~~（**已撤回**） | 切片 2 第一版把计划 C6.4 的"客户端提供但不符 → 400"当成**上游语义**，据此在新用例里断言"客户端给 MD5/SHA-1 的正确值必须通过"，并据此把旧实现判为缺陷。**这个判断是错的**：上游是**无条件覆写**（本表 P6-D05） | —— | **撤回**：不是代码缺陷，而是"**从计划里的一句臆断出发写测试**，没有先读 vendored 样例"。用例已按覆写语义重写。保留编号以留下教训 |
| **P6-D03** | **跨 store 复制把整个对象读进内存**：`ReadObject` → `StringSink` → `put`，1 GiB 对象会让 RSS 抬高 1 GiB —— 直接顶穿 C6.9 的红线 | 切片 2 的流式用例（跨 store + 64 MiB）；同一测试内"整块读回"对照给出 65664 KiB 增长，证明该测量确实能抓到整块驻留 | `StoreByteSource`（`BufferSink` + 分段 `get`）+ 边读边写；校验和走 `HashingSink` 增量计算 |
| **P6-D04** | **门槛可能在"被拆掉防线"的源码树上运行**：`scripts/verify_http_hardening.sh` 等自证脚本临时改源码、靠 `trap` 恢复；脚本被 **SIGKILL** 强杀时 trap 不执行 → H-2 的两道防护补丁残留在 `src/common/http/server.cpp`，下一次门槛的基线直接失败（假警报）；反过来若残留的是"让测试更容易通过"的注入，就会**静默**削弱门槛 | 复现：强杀 `run_all_gates.sh`（本轮真实发生）→ `git diff src/common/http/server.cpp` 里能看到 `自证注入` | ① `git checkout -- src` 恢复；② `scripts/run_all_gates.sh` 增加**前置机械检查**：`src/` 下不得存在 `_*selftest*`/`*_injected*` 文件，且 `git diff -- src` 不得含注入标记，命中即拒绝开跑并给出修复指令。**两向自证**：造一个残留文件/一行标记 → 前置检查必须退出 1；清理后必须通过 |
| **P6-D05** | **计划里的一条判据没有上游依据**：C6.4 写的「客户端提供 `Checksum` 但不符 → `400` + 删除已搬迁对象」与上游冲突。按它实现后，**phase4 的 C4.2/C4.3 两条已收口用例立刻失败**（`test_rest_contract`：golden 样例与 `File_Calculate_Checksum` 都期望 `201`，实测得到 `400`） | 先按旧判据实现 → `ctest -R test_rest_contract` 两条 `REQUIRE(status == 201)` 失败；证据见 `docs/01-osdu-research.md` §2.3 与 `File_CorrectPayload.json` | 实现改为"**无条件覆写 + 不校验 + 不回滚**"；契约 §2.6 第 7 步改写；计划 C6.4 更正并在 `docs/00-final-design.md` §5 登记为**被推翻的结论**（不静默改）。这条差异属于"能测出来的差异"，不是"看起来更好" |

## 6. 结论

| 项 | 结论 |
| --- | --- |
| 切片 1 门槛（SQLite 元数据仓储 + 元数据契约） | ✅ 契约在 SQLite 上跑第二遍；两个护栏全绿 |
| 切片 2 门槛（校验和 C6.4） | ✅ `ctest -L phase6` 3 测试 / 458 断言；覆写语义 + 算法跟随驱动 + 流式 + 公开向量都有断言 |
| 已满足判据 | **C2.10（元数据侧）、C6.4、C6.5** |
| P6 是否收口 | ❌ 未收口 |
