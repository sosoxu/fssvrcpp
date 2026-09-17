# 阶段 7 测试证据（进行中）

| 项 | 值 |
| --- | --- |
| 阶段 | P7（gRPC 适配层 + 双协议等价性；RPC 是**平台外扩展**，见 ADR-001） |
| 状态 | 🚧 **进行中 —— 切片 2/3 完成**（14 个一元 RPC 全部实现 + 契约 §6 等价性矩阵 + `json_name` 对齐） |
| 门槛命令 | `ctest -L phase7` |
| 退出码 | `0`（**4 测试 / 483 断言**） |

测试明细（`ctest -L phase7` 的 4 个二进制）：

| 测试 | 用例 | 断言 |
| --- | --- | --- |
| `test_error_equivalence`（conformance，C7.2） | 3 | 188 |
| `test_grpc_basics`（integration，C7.1） | 4 | 30 |
| `test_protocol_equivalence`（conformance，C7.3/C7.4） | 3 | 222 |
| `test_proto_json_mapping`（unit，C7.5） | 2 | 43 |

> **剩余（切片 3/3）**：3 个扩展 RPC 的字节通道（`UploadFile` / `DownloadFile` / `ServerSideCopy`）
> + C7.6（流式 1 GiB / 0 字节 / 中途取消）+ C7.8（REST 与 gRPC **同时**运行）+ C7.10（gRPC 流式下载
> 与 HTTP `Range` 的 SHA-256 一致）。当前这 3 个 RPC 明确回 `UNIMPLEMENTED`，由
> `test_grpc_basics` 的"切片边界"用例钉住（**不会**因为"忘了实现"而静默变成别的错误）。

---

## 1. 切片 1 交付物（已完成）

| 路径 | 内容 |
| --- | --- |
| `cmake/ProtoGen.cmake` + `fss_proto` | proto → C++/gRPC 代码生成（P0/P1 已就绪，切片 1 首次真正使用） |
| `src/domain/contract/error_table.{h,cpp}` | **契约 §5 的唯一权威表**（L3，无协议类型）：`ErrorKind` → REST `(status, reason)` + gRPC `(code, name)` |
| `src/adapters/http/http_error_mapper.cpp` | `HttpStatusFor`/`ReasonFor` 改为**从共享表派生**（此前是本地 switch） |
| `src/adapters/grpc/grpc_error_mapper.{h,cpp}` | 同一张表的 gRPC 投影 + 尾随元数据 `fss-error-kind` / `fss-error-details` |
| `src/app/usecases/caller_context.{h,cpp}` | **两条协议共用**的调用方解析（REST 头 / gRPC metadata 同名键） |
| `tests/conformance/test_error_equivalence.cpp` | C7.2：契约 §5 逐行（**手抄**的期望值）× 实现表 × 两个适配器 |
| `tests/unit/test_layering_guard.cpp` | C7.7：proto 头只能出现在 `adapters/grpc/`（含 4 条自证用例） |

## 2. 切片 2 交付物

| 路径 | 内容 |
| --- | --- |
| `src/adapters/grpc/file_service_adapter.{h,cpp}` | **14 个一元 RPC 全部实现在真实端口上**：`GetInfo`/`Check`/`GetUploadLocation`/`GetFileLocation`/`GetDownloadLocation`/`GetFileList`/`CreateFileMetadata`/`GetFileMetadata`/`DeleteFileMetadata`/`GetStorageInstructions`/`GetRetrievalInstructions`/`CopyFilesToPersistent`/`GetFileSignedUrl`/`RevokeUrl`；仅剩 3 个扩展 RPC 回 `UNIMPLEMENTED` |
| `src/adapters/grpc/dto/grpc_dto.{h,cpp}` | proto ↔ 领域全字段双向转换（presence 语义、`Struct`/`tags`/`meta`、枚举↔小写驱动名、`Timestamp`↔epoch）+ 8 个 `Fill*Proto` 投影 + 请求侧解析 |
| `src/app/usecases/wire_shapes.{h,cpp}` | **两条协议共用的"线上形状"**（L4）：`FileNameOfPath`、`DmsLocationJson(...)`（DMS/Delivery 的 location JSON）、`FileSourceFromRecordNode`。REST 的 `dto.cpp` 与 gRPC 适配器**都**从这里取，避免两处各写一份 JSON |
| `tests/framework/grpc_fixture.h` | `GrpcFixture`（真实 gRPC 端口 + 真实 channel + 调用元数据）+ **`DualProtocolFixture`**（REST 与 gRPC 挂在**同一份** metadata/location/blob 状态上） |
| `tests/conformance/test_protocol_equivalence.cpp` | C7.3 契约 §6 矩阵（12 行）+ 错误分类等价 + C7.4 `SignedUrlEquivalent`（含 4 条反向测试） |
| `tests/unit/test_proto_json_mapping.cpp` | C7.5：proto3-JSON 的键名与契约 §3.3 黄金样例逐字段对齐，且能被**领域解析器**接受（不是只比字符串） |
| `tests/integration/test_grpc_basics.cpp` | 更新切片边界：已实现 RPC 的线上错误语义（`NOT_FOUND` + 尾随元数据、缺 token → `UNAUTHENTICATED`）+ 剩余 3 个 RPC 的 `UNIMPLEMENTED` |

## 3. 判据进展

| 判据 | 状态 | 证据 |
| --- | --- | --- |
| **C7.2** 错误码双向映射无空缺 | ✅ | `test_error_equivalence` 3 用例 / 188 断言：14 行逐字段比对（REST 状态 + reason + gRPC 枚举数值 + `Error` 走一遍）+ 枚举无空缺 + 状态码集合相等 + 越界枚举值不返回 OK |
| **C7.7** proto 隔离护栏 | ✅ | `test_layering_guard`：L3/L4 禁止 `osdu/file/v1`/`<google/protobuf>`/`<grpcpp/`；"proto 头只能出现在 `adapters/grpc/`"规则 + 4 条自证（域/应用/HTTP 命中必须失败、gRPC 目录必须放行） |
| **C7.1** 17 个 RPC 可调用 | 🚧 **14/17** | 14 个一元 RPC 在真实端口上跑通（`test_grpc_basics` 4 用例 / 30 断言 + 矩阵测试覆盖其中 12 个）；3 个流式/代理 RPC 明确 `UNIMPLEMENTED`（切片 3） |
| **C7.3** 契约 §6 矩阵 12 行 | ✅ | `test_protocol_equivalence` 用例 2：`GetUploadLocation`/`GetFileLocation`/`GetDownloadLocation`/`GetFileList`/`CreateFileMetadata`/`GetFileMetadata`/`DeleteFileMetadata`/`GetStorageInstructions`/`GetRetrievalInstructions`/`CopyFilesToPersistent`/`GetFileSignedUrl`/`RevokeUrl` 逐行比较**领域结果 + 副作用 + 错误分类** |
| **C7.4** `SignedUrlEquivalent` | ✅ | 同一测试文件用例 1：自签 URL 的 token 用 `HmacTransferTokenCodec::Decode` **验签并解码**后逐项比对 `partition/file_id/container/object_key/zone/op`；4 条**反向测试**（改 query 名 / 改 token 声明 / 换 host/scheme / 换对象）必须判不等 |
| **C7.5** proto `json_name` 对齐 | ✅ | `test_proto_json_mapping` 2 用例 / 43 断言：黄金样例经 proto3-JSON 序列化后，`data` 内 PascalCase、信封 camelCase，且**领域解析器**能吃下同一份 JSON；`SignedURL`/`providerKey`/`storageLocation` 三个易错键单独钉住 |
| C7.6 / C7.8 / C7.10 | ⬜ 未开始 | 见开头"剩余"（切片 3） |

### 3.1 契约 §6 中"签名 URL 等价"的判据已按实测细化

切片 2 实测暴露：集中存储模式下 REST 与 gRPC 返回的**是自签传输 URL**
（`/v1/transfer/<token>?exp=…&sig=…`），其 path 里内嵌的是**密文**，每次签发带新 nonce —
"path 相同"这条判据**永远不可能成立**。契约 §6 原文的判据（"path 相同"）据此改为**分两种形态**：
通用形态（签名在 query，如 S3 预签名）仍比 path；自签形态则比
`/v1/transfer/` 前缀 + query 键集合 + 过期 ±5s + **解码后的 token 声明逐项相等**。
这是**判据从"字面"到"语义"的收紧而非放宽**：解码比对能区分"两条链路指向同一条位置记录"
与"两条链路各自签发了一个 URL"，只比状态码做不到后者。

## 4. 自证对照（R1：关键断言必须能区分"实现正确"与"测试无效"）

| 注入点 | 注入内容 | 期望失败 | 实测 |
| --- | --- | --- | --- |
| `GrpcDto::FillFileLocationProto` | `set_driver("posix_INJECTED")`（只改 gRPC 一侧投影） | 矩阵第 2 行的 `rpc_location.driver() == REST Driver` 必须失败 | ✅ `test_protocol_equivalence.cpp:309 FAILED` |
| `domain/contract/error_table.cpp` | `kNotFound` 行的 gRPC 列 `kNotFound/"NOT_FOUND"` → `kInvalidArgument/"INVALID_ARGUMENT"` | ① 手抄契约期望值的 `test_error_equivalence` 必须失败；② 双协议错误分类必须失败 | ✅ `test_error_equivalence.cpp:69,119 FAILED` + `test_protocol_equivalence.cpp:560,580 FAILED` |
| `build/tests/CMakeFiles/_zz_selftest.dir/link.txt`（**只注入到构建目录，不动 `src/`**） | 伪造一份链接 `libfss_domain.a` + `libfss_http.a`（或 `libfss_app.a` + `libfss_proto.a`）的 `link.txt` | 修好的 C2.1 传递闭包佐证必须**仍然能判失败**（否则"改成遍历"只是把检查变成恒真） | ✅ `纯分层测试 _zz_selftest.dir 链接了 fss_http`，`verify_link_graph.sh` 退出码 **1**；清理后退出码 **0** |

两次注入都**只改一侧**（gRPC 投影 / 权威表），这证明：
- 矩阵确实在逐字段比较**两条链路各自的输出**，不是拿一份数据比它自己；
- C7.2 的"手抄期望值"确实独立于实现表（表被改坏时能抓到），而不是从表里生成期望值。

注入已全部回滚；`git diff -- src/` 中不含 `INJECTED` 标记（`run_all_gates.sh` 的前置检查会拦）。

## 5. 本阶段发现的实现陷阱

| 编号 | 症状 | 根因 | 规避 |
| --- | --- | --- | --- |
| **P7-D01** | 编译期一片 `'Status' in namespace 'fss::adapters::grpc' does not name a type` | **命名空间名 `grpc` 把全局 `::grpc` 遮蔽了**：在 `namespace fss::adapters::grpc` 内部，`grpc::Status` 解析到本命名空间 | 库类型一律写全局限定 `::grpc::Status` / `::grpc::StatusCode`（本目录所有文件已生效）。同类问题在 `namespace http` 下用 `httplib::` 不会出现（名字不同），**只有与库同名的命名空间才会踩** |
| **P7-D02** | `SignedUrlEquivalent` 用字面 path 比较时，REST 与 gRPC 的 URL 必然不等，把"实现正确"判成失败 | 自签 token 是**密文 + 随机 nonce**，`path` 每次不同（见 §3.1） | 判据改为"验签解码后比声明"；并用 4 条反向测试保证判据不是恒真（只比 `exp` 之类会恒真） |
| **P7-D03** | 双协议测试里 gRPC 侧返回 `UNAUTHENTICATED`，而 REST 侧 200 | 早期 fixture 在 gRPC 分支**忘了带调用元数据**（`authorization` / `data-partition-id`），REST 分支走了 `Authed()` | 统一用 `fx.Context()` 构造带元数据的 `ClientContext`；并保留一条"缺 token → `UNAUTHENTICATED`"的**正向**用例（契约 §5 那一行） |
| **P7-D04** | `GetStorageInstructions` 连续调用两次得到**不同**的 `file_id`，无法字面比对 | 该 RPC 每次调用都新建空对象（契约 §6 该行"创建空对象 + 新增 1 条位置记录"） | 该行只比**键集合与形状**（`providerKey`/`storageLocation` 的键），不比生成型 id；并在契约 §6 行内注明"内容可不同" |
| **P7-D05** | proto3-JSON 序列化结果里**缺少**默认值字段（如 `"Number":0` 不出现），逐字段断言失败 | proto3 的 JSON 打印**省略默认值**（与 proto3 无 unknown-field 保留同源） | 断言改为"非默认值必须出现"，并对默认值字段单独断言其**存在性语义**；契约 §3.3 的互操作按"领域解析器能吃下"判定，而非"键集合完全相等" |
| **P7-D06** | `run_all_gates.sh` 在**没有越层依赖**时报告「phase2 链接图出现越层依赖」，门槛整体失败 | `scripts/verify_link_graph.sh` 的传递闭包佐证写成 `find ... \| head -50 \| xargs grep -l ... \| head -1`。插桩前 `build/tests` 下的 `link.txt` 少于 50 个（`head` 不会提前关闭管道）；切片 2 新增 2 个测试目标后变成 **61 个**，`head -50` 提前退出 → 上游 `find` 收到 **SIGPIPE** → `set -euo pipefail` 下整条管道返回 **141** → 函数被 `set -e` 中止 → 判为"越层依赖"（**与真实原因完全无关**）。同一文件里"取第一个 `DependInfo.cmake`"也用了 `\| head -1`，属同一类隐患 | ① 一律改成"全文收集后再用 `sed -n 1p` 取第一行"（`sed` 读完全部输入，不会关闭管道）；`check_io_uring.sh`（`docker images \| grep \| head -1`）与 `verify_http_hardening.sh`（`grep \| head -20`）同类写法一并修正。② 顺带把传递闭包佐证从"随便挑一个链接 `fss_domain` 的可执行文件"改成**遍历所有"不含传输适配器"的纯分层可执行文件**（30 个）：原写法一旦挑中 phase4 起的端到端测试（它们本来就合法链接 `fss_http_adapter`）会**误报污染**；并补了注入自证（见 §4） |

## 6. 已知表达力差异（如实登记，不静默丢失）

1. proto3 **没有未知字段保留**：领域模型用 `extra`（`json::Value`）承载的**未识别字段**
   无法经 proto 往返。当前规则：
   - 领域 → proto：`data.extra["ExtensionProperties"]` → `data.extension_properties`；
     `data.extra` 里的其它键**不会**出现在 proto 里；
   - proto → 领域：`data.extension_properties` → `data.extra["ExtensionProperties"]`。
   因此"全字段无损往返"只在 **REST 链路**上成立（C6.1 的语境）；RPC 链路的等价性按
   **已建模字段**比对（C7.3 的矩阵比较的是**领域结果**，不是 JSON 字符串）。
2. proto3-JSON **省略默认值**（见 P7-D05）：空字符串/0 字段不出现。需要"显式为空"的语义时，
   用 message 包裹（proto3 无 `optional`，见 AGENTS.md §1）。
3. DMS/Delivery 的 location JSON 在 REST 侧输出 `connectionString: null`，而 proto3 的 `string`
   无法表达"显式 null"——该行的等价性判据是**键集合 + 非空字段**（矩阵用例内已如实注释）。

## 7. 收工验证（切片 2）

| 命令 | 结果 |
| --- | --- |
| `ctest --test-dir build -L phase7 --output-on-failure` | ✅ **4 测试 / 483 断言** 全通过（error_equivalence 188 / proto_json_mapping 43 / protocol_equivalence 222 / grpc_basics 30） |
| `./scripts/check_docs.sh` | ✅ D1~D5 全通过（47 个本地链接、ADR 索引完整、阶段表与 `IMPLEMENTED_PHASES` 一致） |
| `./scripts/run_all_gates.sh` | ✅ **全部已启用阶段（0~7）门槛通过**，`失败: 无`（完整日志：`build/gates-slice2.log`）。摘要：phase0 ✅ / phase1 ✅（含护栏自证、H-2 自证）/ phase2 ✅（链接图 + 能力护栏自证）/ phase3 ✅ / phase4 ✅（组合根护栏自证）/ phase5 ✅（驱动切换）/ phase6 ✅ / phase7 ✅ |
| sanitizer（`run_all_gates.sh` 内嵌，`build-asan`） | ✅ **ASan + UBSan + LSan 全绿**，含 `ctest -L phase7`（sanitizer 下 4 个测试全通过）—— gRPC 侧未触发 P1-D20 的布局耦合问题（`GRPC_ASAN_SUPPRESSED=1` 生效） |

**结论**：切片 2 的判据 C7.3 / C7.4 / C7.5 已满足（连同切片 1 的 C7.2 / C7.7）；
C7.1 进度 14/17（余 3 个流式/代理 RPC）。**本切片未使任何已启用阶段退化**（phase0~7 回归全绿）。
未验证项：流式 RPC（C7.6）、双协议同进程并发（C7.8）、gRPC 流式 ↔ HTTP `Range` 的字节一致（C7.10）——
均为切片 3 的交付内容，当前这些 RPC 明确 `UNIMPLEMENTED`。
