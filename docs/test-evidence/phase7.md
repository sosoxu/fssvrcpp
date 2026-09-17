# 阶段 7 测试证据（✅ 已完成）

| 项 | 值 |
| --- | --- |
| 阶段 | P7（gRPC 适配层 + 双协议等价性；RPC 是**平台外扩展**，见 ADR-001） |
| 状态 | ✅ **已完成并通过门槛 —— C7.1~C7.10 全部满足**（3 个切片全部收口） |
| 门槛命令 | `ctest -L phase7` |
| 退出码 | `0`（**6 测试 / 5378 断言**） |

测试明细：

| 测试 | 用例 | 断言 | 覆盖 |
| --- | --- | --- | --- |
| `test_error_equivalence`（conformance） | 3 | 188 | C7.2 |
| `test_grpc_basics`（integration） | 4 | 34 | C7.1（运维 RPC + 错误语义 + 不再 UNIMPLEMENTED） |
| `test_protocol_equivalence`（conformance） | 3 | 222 | C7.3（矩阵 12 行）+ C7.4 |
| `test_proto_json_mapping`（unit） | 2 | 43 | C7.5 |
| `test_grpc_streaming`（integration） | 9 | 4812 | C7.1 / C7.6 / C7.10 |
| `test_dual_protocol_concurrency`（integration） | 1 | 79 | C7.8（**真实二进制**） |

---

## 1. 三个切片交付物

### 切片 1（错误表 + 运维 RPC + proto 护栏）

| 路径 | 内容 |
| --- | --- |
| `src/domain/contract/error_table.{h,cpp}` | **契约 §5 的唯一权威表**（L3，无协议类型） |
| `src/adapters/http/http_error_mapper.cpp` | `HttpStatusFor`/`ReasonFor` 改为**从共享表派生** |
| `src/adapters/grpc/grpc_error_mapper.{h,cpp}` | gRPC 投影 + 尾随元数据 `fss-error-kind` / `fss-error-details` |
| `src/app/usecases/caller_context.{h,cpp}` | 两条协议共用的调用方解析 |
| `tests/unit/test_layering_guard.cpp` | C7.7：proto 头只能出现在 `adapters/grpc/`（含 4 条自证） |

### 切片 2（14 个一元 RPC + 等价性矩阵）

| 路径 | 内容 |
| --- | --- |
| `src/adapters/grpc/file_service_adapter.{h,cpp}` | 14 个一元 RPC 在真实端口上实现 |
| `src/adapters/grpc/dto/grpc_dto.{h,cpp}` | proto ↔ 领域全字段双向转换 + 投影/解析 |
| `src/app/usecases/wire_shapes.{h,cpp}` | 两条协议**共用**的线上形状（`DmsLocationJson` / `FileSourceFromRecordNode`），REST 与 gRPC 都从这里取 |
| `tests/conformance/test_protocol_equivalence.cpp` | C7.3 矩阵 12 行 + C7.4（含 4 条反向测试） |
| `tests/unit/test_proto_json_mapping.cpp` | C7.5 黄金样例逐字段互操作 |

### 切片 3（3 个扩展 RPC + 组合根双端口 + 流式判据）

| 路径 | 内容 |
| --- | --- |
| `src/app/usecases/usecases.{h,cpp}`（新增 3 个用例） | `UploadFile`（客户端流）/ `DownloadFile`（服务端流）/ `ServerSideCopy`：授权、位置记录解析、存储副作用、审计都在 L4；`registerMetadata=true` 复用**同一个** `CreateFileMetadata` 12 步用例 |
| `src/adapters/grpc/grpc_streaming_io.{h,cpp}` | gRPC 流 ↔ `ByteSource`/`ByteSink` 的翻译层（≤64 KiB 分片；**取消 ≠ EOF**） |
| `src/adapters/grpc/file_service_adapter.cpp` | 17/17 RPC 实现（`UNIMPLEMENTED` 分支全部消失） |
| `src/adapters/grpc/grpc_server.{h,cpp}` | gRPC 服务器的**生命周期包装**（`StartGrpcServer` / 端口 / `Shutdown()`）：让组合根不必出现 `<grpcpp/`，从而**不放宽** C7.7 的护栏（P7-D09） |
| `src/main/server_main.cpp` | 组合根**同时**开 REST 与 gRPC 两个端口（`FSS_GRPC_PORT`；`-1` = 系统分配，横幅给出实际端口），gRPC 侧通过 `GrpcServerHandle` 持有 |
| `tests/integration/test_grpc_streaming.cpp` | C7.6（1 GiB / 0 字节 / 中途取消 / 慢客户端）+ C7.10（与 HTTP `Range` 字节一致且不整文件读）+ 授权锚点 |
| `tests/integration/test_dual_protocol_concurrency.cpp` + `tests/framework/server_process.h` | C7.8：拉起**真实 `fss_server`**，两个端口并发混合协议读写同一份状态 |

## 2. 判据进展（全部满足）

| 判据 | 状态 | 证据 |
| --- | --- | --- |
| **C7.1** 17 个 RPC 可调用 | ✅ **17/17** | 14 个一元 RPC（`test_grpc_basics` + 矩阵）+ 3 个扩展 RPC（`test_grpc_streaming`）；`test_grpc_basics` 用**非法输入**断言三者都**不再**回 `UNIMPLEMENTED`（INVALID_ARGUMENT / NOT_FOUND） |
| **C7.2** 错误码双向映射无空缺 | ✅ | `test_error_equivalence` 3 用例 / 188 断言（14 行逐字段 + 枚举无空缺 + 越界不返回 OK） |
| **C7.3** 契约 §6 矩阵 12 行 | ✅ | `test_protocol_equivalence`（领域结果 + 副作用 + 错误分类）+ `test_dual_protocol_concurrency`（跨协议交叉读） |
| **C7.4** `SignedUrlEquivalent` | ✅ | 自签 token 验签解码后比声明 + 4 条反向测试 |
| **C7.5** proto `json_name` 对齐 | ✅ | `test_proto_json_mapping` 2 用例 / 43 断言 |
| **C7.6** 流式（1 GiB / 0 字节 / 取消 / 慢客户端） | ✅ | `test_grpc_streaming`：1 GiB 上传+下载 **RSS 增长 < 64 MiB**（含"整块读回"对照必须被同一测量抓到）、独立 SHA-256、0 字节、**中途取消**（无 `.tmp.` 残留 / fd 不增长 / 对象未被截断）、慢客户端（分片间停顿 0.4 s） |
| **C7.7** proto 隔离护栏 | ✅ | `test_layering_guard`（L3/L4/HTTP 禁止 proto 头 + 4 条自证） |
| **C7.8** gRPC 与 REST 同时运行 | ✅ | `test_dual_protocol_concurrency`：真实 `fss_server`（`FSS_GRPC_PORT=-1`），4×REST + 4×gRPC 线程并发读写同一批记录，0 失败；跨协议内容一致 |
| **C7.9** 回归（P0–P6 全绿） | ✅ | `./scripts/run_all_gates.sh`：phase0~7 全部通过（见 §6） |
| **C7.10** gRPC 区间读 == HTTP `Range` | ✅ | 相同 offset/length 下 SHA-256 逐字节一致，且**两条链路都走区间读**（在 `IBlobStore::get` 边界上用记账装饰器断言 `last_range` 与 `whole_reads == 0`） |

## 3. 契约 §6 的判据修订（切片 2，保留记录）

自签传输 URL（`/v1/transfer/<token>?exp=…&sig=…`）的 path 内嵌**密文 + 随机 nonce**，
"path 相同"**永远不成立**。判据改为分形态：通用形态比 path；自签形态比
`/v1/transfer/` 前缀 + query 键集合 + 过期 ±5 s + **解码后的 token 声明逐项相等**
（`partition/file_id/container/object_key/zone/op`）。这是**收紧**而非放宽 ——
只比状态码无法区分"同一条位置记录"与"各自签发了一个 URL"。

切片 3 又补了一行：`UploadFile`/`DownloadFile` 与 REST 数据面的等价性用
**内容一致**判定（同一批字节的 SHA-256 / 同一区间的 SHA-256），因为二者没有可逐字段
比较的响应体（契约 §6 的扩展对照表）。

## 4. 自证对照（R1：关键断言必须能区分"实现正确"与"测试无效"）

| 注入点 | 注入内容 | 期望失败 | 实测 |
| --- | --- | --- | --- |
| `GrpcDto::FillFileLocationProto` | gRPC 侧 driver 拼错（只改一侧投影） | 矩阵第 2 行的 `driver` 必须失败 | ✅ `test_protocol_equivalence.cpp:309 FAILED` |
| `domain/contract/error_table.cpp` | `kNotFound` 的 gRPC 列改成 `INVALID_ARGUMENT` | 手抄期望值的 C7.2 + 双协议错误分类必须失败 | ✅ `test_error_equivalence.cpp:69,119` + `test_protocol_equivalence.cpp:560,580 FAILED` |
| `build/tests/CMakeFiles/_zz_selftest.dir/link.txt`（只动构建目录） | 伪造一份链接 `libfss_domain.a` + `libfss_http.a` 的 `link.txt` | 修好的 C2.1 传递闭包佐证必须仍能判失败 | ✅ 退出码 1；清理后 0（P7-D06） |
| `grpc_streaming_io.cpp` | 去掉 `IsCancelled()` 分支 → **把取消当 EOF** | 取消用例必须发现"截断对象被提交" | ✅ `test_grpc_streaming.cpp:379 FAILED`（`downloaded.bytes == 0` 不成立）—— P7-D07 |
| `DownloadFile::Execute` | `ByteRange{}`（忽略区间，整文件读） | C7.10 必须失败 | ✅ `test_grpc_streaming.cpp:408 FAILED`（字节数不等于区间长度） |
| `DownloadFile::Execute` | **整块读回再切片**（返回的字节正确，但读了整文件） | 端口级记账必须抓到 | ✅ `test_grpc_streaming.cpp:410 FAILED`（`recording.last_range.length` 不等于请求区间） |
| `server_process.h` | 组合根改成 `FSS_GRPC_PORT=0`（关掉 gRPC） | C7.8 用例必须失败 | ✅ `server_process.h:76 FAILED`（解析不到 gRPC 端口） |
| `UploadFile::Execute` | 去掉"位置记录是授权锚点"的校验（允许任意 file_source） | 授权锚点用例必须失败 | ✅ `test_grpc_streaming.cpp:548 FAILED`（伪造 file_source 未被拒绝） |

注入已全部回滚；`git diff -- src/` 中不含 `FSS_SELFTEST`/`INJECTED` 标记
（`run_all_gates.sh` 的前置检查会拦）。

## 5. 本阶段发现的实现陷阱

| 编号 | 症状 | 根因 | 规避 |
| --- | --- | --- | --- |
| **P7-D01** | 编译期一片 `'Status' in namespace 'fss::adapters::grpc' does not name a type` | **命名空间名 `grpc` 遮蔽了全局 `::grpc`** | 库类型一律写 `::grpc::Status` / `::grpc::StatusCode` |
| **P7-D02** | `SignedUrlEquivalent` 用字面 path 比较，REST 与 gRPC 的 URL 必然不等 | 自签 token 是密文 + 随机 nonce | 判据改为"验签解码后比声明"，并配 4 条反向测试防恒真 |
| **P7-D03** | 双协议测试里 gRPC 侧 `UNAUTHENTICATED` 而 REST 侧 200 | 早期 fixture 在 gRPC 分支忘了带调用元数据 | 统一用 `fx.Context()`；保留"缺 token → UNAUTHENTICATED"正向用例 |
| **P7-D04** | `GetStorageInstructions` 两次调用得到**不同** `file_id`，无法字面比对 | 每次调用都新建空对象（契约 §6 该行"创建空对象"） | 该行只比键集合与形状；契约 §6 行内注明"内容可不同" |
| **P7-D05** | proto3-JSON 缺少默认值字段（`"Number":0` 不出现） | proto3 的 JSON 打印省略默认值 | 断言"非默认值必须出现"+ 存在性语义；互操作按"领域解析器能吃下"判定 |
| **P7-D06** | 没有越层依赖时，门槛报"phase2 链接图出现越层依赖" | `find … \| head -50` 在 `link.txt` 从 49 涨到 61 个后触发 **SIGPIPE** → `pipefail` 下 141 → `set -e` 中止 | 改"全文收集 + `sed -n 1p`"；同类 `\| head` 写法一并修正；传递闭包佐证从"随便挑一个"改为遍历 30 个纯分层二进制 + 注入自证 |
| **P7-D07** | 客户端**中途取消**上传后，`.tmp.` 临时文件残留；更严重的是**去掉取消判断后截断的对象被当成完整对象提交** | `ServerReader::Read()` 在"正常读完"与"被取消"两种情况下都返回 false；把后者当 EOF → `put` 正常 rename 半截对象 | ① `GrpcUploadSource` 持有 `ServerContext`，`Read()==false` 后查 `IsCancelled()`：取消 → `kUnavailable`（**不是** EOF），让存储层走失败清理；② 测试用**轮询**等待服务端清理完成（客户端 `Finish()` 返回时服务端可能还在写） |
| **P7-D09** | 加完组合根的 gRPC 启动后，phase1 的**护栏自证**报"基线不通过"：`src/main/server_main.cpp` 出现 `<grpcpp/`（C7.7 的护栏在 `src/` 全树除 `adapters/grpc/` 外一律禁止） | 看似是"护栏与 R12（组合根创建具体实现）冲突"，实际是**组合根直接抓住了第三方库类型**。`src/main` 早就只 include `fss_http` 的包装头（httplib 同样被挡在 `common/http/` 内），gRPC 缺了对应的一层 | **不放宽护栏**：新增 `adapters/grpc/grpc_server.{h,cpp}`（只暴露 `StartGrpcServer` / 端口 / `Shutdown()`），组合根持有 `unique_ptr<GrpcServerHandle>`，看不到任何 grpc 类型。`verify_guard.sh` 的 ①~⑤ 全部恢复通过 |
| **P7-D08** | `UploadFile(registerMetadata=true)` 报"metadata 的 data.FileSource 与上传目标不一致"，但两者明明是同一个值 | 测试 fixture `AppFixture::MakeRecord` 没置 `data.dataset_properties.present`，而 proto 侧用 `present` 判断整段是否存在（proto3 无字段 presence）→ 记录经 proto 往返**整段丢失** | fixture 显式置 `present = true`；这类"看起来有值、proto 往返后消失"的字段必须由 `present` 表达（已写入 AGENTS.md §4.3 的同类条目） |

## 6. 收工验证（切片 3 + 阶段收口）

| 命令 | 结果 |
| --- | --- |
| `ctest --test-dir build -L phase7 --output-on-failure` | ✅ **6 测试 / 5378 断言** 全通过（明细见开头表） |
| `./scripts/check_docs.sh` | ✅ D1~D5 全通过（链接、ADR 索引、阶段表与 `IMPLEMENTED_PHASES` 一致） |
| `./scripts/run_all_gates.sh` | ✅ **phase0~7 全部通过**，`失败: 无`（C7.9 回归；完整日志 `build/gates-slice3.log`）——其中 phase1 的**护栏自证** ①~⑤ 全绿、phase2 的链接图/能力护栏自证全绿、phase5 驱动切换全绿 |
| sanitizer（`run_all_gates.sh` 内嵌，`build-asan`） | ✅ ASan + UBSan + LSan 全绿，**含 `ctest -L phase7` 的 6 个测试**（其中 `test_dual_protocol_concurrency` 会在 sanitizer 构建下拉起 `build-asan/bin/fss_server`）。规模注释：sanitizer 下用 `FSS_TEST_BIG_BYTES=64MiB` / `FSS_TEST_RSS_LIMIT_KIB=512MiB` 缩小，**紧的上限在普通构建的门槛里跑** |
| `build/bin/fss_server` 手工冒烟 | ✅ 横幅同时给出 `bind` 与 `grpc bind`；REST `/v2/info` 200 |

**结论**：P7 的退出条件（C7.1–C7.9 + C7.10）**全部满足**，"双协议"目标（G3）达成。
**未做（不影响判据，已登记）**：真实 S3/MinIO 上的流式 RPC 端到端、
多租户/多分区并发下的 gRPC 流控调优、`ServerSideCopy` 的跨存储实例（staging 与 persistent
落在不同 store）路径（当前由 `CopyBetweenZones` 的跨 store 分支复用，已被 P6 覆盖，
但**没有**在 RPC 面单独跑一遍）。
