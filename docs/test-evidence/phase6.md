# 阶段 6 测试证据（进行中）

| 项 | 值 |
| --- | --- |
| 阶段 | P6（元数据记录语义完整化） |
| 状态 | 🚧 **进行中 —— 切片 1/6 完成**（`SqliteMetadataRepository` + 元数据契约跑第二遍） |
| 门槛命令 | `ctest -L phase6` |
| 退出码 | `0`（1 测试 / 122 断言） |

> 剩余：校验和（SHA-256/MD5/SHA-1 流式，C6.4）、12 步序列的 6 个故障注入点（C6.3）、
> `getFileList` 语义（C6.6）、角色常量（C6.8）、GC 租约与幂等并发与 tmp 名唯一性（C6.11/C6.12/C6.13）、
> DMS/Delivery 语义收口（C6.7）、大文件搬迁 RSS（C6.9）、远端 Storage Service 仓储。

---

## 1. 切片 1 交付物（`SqliteMetadataRepository`）

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

## 2. 门槛命令与输出

```console
$ cmake --build build -j8 && ctest --test-dir build -L phase6 --output-on-failure
    Start 34: test_sqlite_metadata_repository ......   Passed    0.02 sec
100% tests passed, 0 tests failed out of 1

逐测试断言数：
  test_sqlite_metadata_repository   122 assertions in 2 test cases
                                    ← C2.10（元数据侧契约）+ C6.5（版本链 + 唯一索引自证）
  ─────────────────────────────────────────────
  合计 122 个断言 / 2 个测试用例 / 1 个测试
```

相关护栏（改动仓储 SQL 后必跑）：

```console
$ ./build/bin/test_sql_guardrail          → 9 assertions in 2 test cases（每条 SQL 必须带 partition_id）
$ ./build/bin/test_layering_guard         → 20 assertions in 3 test cases（L2 只依赖 L3 端口 + L1）
```

---

## 3. 判据进展

| 判据 | 状态 | 证据 |
| --- | --- | --- |
| **C2.10（元数据侧）** 同一套契约跑第二遍 | ✅ | `test_sqlite_metadata_repository` 复用 `CheckMetadataRepositoryContract`（与内存实现逐条相同） |
| **C6.5** 版本链 | ✅ | 契约的版本用例 + 切片 1 的"数据库真实形态"用例（3 版共存、1 个 latest、部分唯一索引自证） |
| C6.1（黄金样例字段级往返） | 🚧 部分 | 仓储侧已无损（`data` 列存整条 JSON）；REST 侧的全字段比对已在 P4 的 C4.2 覆盖，尚缺"经 SQLite 仓储往返"的组合用例 |
| C6.2 / C6.3 / C6.4 / C6.6 / C6.7 / C6.8 / C6.9 / C6.11 / C6.12 / C6.13 | ⬜ 未开始 | 见开头"剩余" |

---

## 4. 结论

| 项 | 结论 |
| --- | --- |
| 切片 1 门槛（SQLite 元数据仓储 + 元数据契约） | ✅ `ctest -L phase6` 1 测试 / 122 断言；两个护栏全绿 |
| 已满足判据 | **C2.10（元数据侧）、C6.5** |
| P6 是否收口 | ❌ 未收口 |
