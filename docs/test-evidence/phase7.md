# 阶段 7 测试证据（进行中）

| 项 | 值 |
| --- | --- |
| 阶段 | P7（gRPC 适配层 + 双协议等价性；RPC 是**平台外扩展**，见 ADR-001） |
| 状态 | 🚧 **进行中 —— 切片 1/3 完成**（代码生成 + 错误映射等价 + 运维 RPC + 协议隔离护栏） |
| 门槛命令 | `ctest -L phase7` |
| 退出码 | `0`（2 测试 / 210 断言） |

> 剩余：**切片 2** = 其余 14 个一元 RPC（位置/列表/元数据写/DMS/Delivery/RevokeUrl/ServerSideCopy）
> + 契约 §6 的等价性矩阵（C7.1/C7.3/C7.5）；**切片 3** = 3 个流式/代理 RPC
> （`UploadFile`/`DownloadFile`/`ServerSideCopy` 的字节通道）+ `SignedUrlEquivalent`（C7.4）
> + 流式 1 GiB / 0 字节 / 中途取消（C7.6）+ 双协议同时运行（C7.8）+ Range 一致（C7.10）。

---

## 1. 切片 1 交付物

| 路径 | 内容 |
| --- | --- |
| `cmake/ProtoGen.cmake` + `fss_proto` | proto → C++/gRPC 代码生成（P0/P1 已就绪，本切片首次真正使用） |
| `src/domain/contract/error_table.{h,cpp}` | **契约 §5 的唯一权威表**（L3，无协议类型）：`ErrorKind` → REST `(status, reason)` + gRPC `(code, name)` |
| `src/adapters/http/http_error_mapper.cpp` | `HttpStatusFor`/`ReasonFor` 改为**从共享表派生**（此前是本地 switch） |
| `src/adapters/grpc/grpc_error_mapper.{h,cpp}` | 同一张表的 gRPC 投影 + 尾随元数据 `fss-error-kind` / `fss-error-details` |
| `src/app/usecases/caller_context.{h,cpp}` | **两条协议共用**的调用方解析（REST 头 / gRPC metadata 同名键） |
| `src/adapters/grpc/file_service_adapter.{h,cpp}` | `GetInfo` + `Check` 已实现；其余 15 个明确 `UNIMPLEMENTED`（切片边界可见） |
| `src/adapters/grpc/dto/grpc_dto.{h,cpp}` | proto ↔ 领域双向转换（元数据全字段 + `Struct`/`tags`/`meta`；未建模字段的表达力差异见下） |
| `tests/framework/grpc_fixture.h` | 真实 gRPC 端口（`127.0.0.1:0`）+ 真实 channel + 调用元数据构造 |
| `tests/conformance/test_error_equivalence.cpp` | C7.2：契约 §5 逐行（**手抄**的期望值）× 实现表 × 两个适配器 |
| `tests/integration/test_grpc_basics.cpp` | 真实端口上的 `GetInfo`/`Check` + 切片边界（UNIMPLEMENTED） |
| `tests/unit/test_layering_guard.cpp` | C7.7：proto 头只能出现在 `adapters/grpc/`（含 4 条自证用例） |

## 2. 判据进展

| 判据 | 状态 | 证据 |
| --- | --- | --- |
| **C7.2** 错误码双向映射无空缺 | ✅ | `test_error_equivalence` 3 用例 / 188 断言：14 行逐字段比对（REST 状态 + reason + gRPC 枚举数值 + `Error` 走一遍）+ 枚举无空缺 + 状态码集合相等 + 越界枚举值不返回 OK |
| **C7.7** proto 隔离护栏 | ✅ | `test_layering_guard`：L3/L4 禁止 `osdu/file/v1`/`<google/protobuf>`/`<grpcpp/`；"proto 头只能出现在 `adapters/grpc/`"规则 + 4 条自证（域/应用/HTTP 命中必须失败、gRPC 目录必须放行） |
| C7.1 17 个 RPC 可调用 | 🚧 **2/17** | 本切片：`GetInfo`、`Check`（真实端口）；其余 15 个明确 `UNIMPLEMENTED`（`test_grpc_basics` 断言这条边界） |
| C7.3 / C7.5 / C7.4 / C7.6 / C7.8 / C7.10 | ⬜ 未开始 | 见开头"剩余" |

## 3. 本切片的两条实现要点（都是"不这样做就会静默出错"）

1. **映射表只有一份**：此前 `HttpStatusFor` 是本地的 switch；如果 gRPC 侧再写一份，
   两条链路就会各自漂移（而 C7.2 恰恰要求"同一行"）。现在数据在 L3
   （`domain/contract/error_table.h`），两个适配器都从它派生；测试用手抄的契约期望值
   同时钉住两边 —— **表错、适配器错、契约漂移**三种情况都会失败。
2. **gRPC 尾随元数据必须写在 `ServerContext` 上**：`grpc::Status` 只有 code + message
   两个字段。`AttachErrorMetadata()` 把 `fss-error-kind`（REST 错误体里 `code`/`reason`
   的等价物）与 `fss-error-details`（`expected/actual` 之类）写进尾随元数据。

## 4. 切片 1 发现的实现陷阱

| 编号 | 症状 | 根因 | 规避 |
| --- | --- | --- | --- |
| **P7-D01** | 编译期一片 `'Status' in namespace 'fss::adapters::grpc' does not name a type` | **命名空间名 `grpc` 把全局 `::grpc` 遮蔽了**：在 `namespace fss::adapters::grpc` 内部，`grpc::Status` 解析到本命名空间 | 库类型一律写全局限定 `::grpc::Status` / `::grpc::StatusCode`（已在本目录所有文件生效）。同类问题在 `namespace http` 下用 `httplib::` 不会出现（名字不同），**只有与库同名的命名空间才会踩** |

## 5. 已知表达力差异（如实登记，不静默丢失）

proto3 **没有未知字段保留**：领域模型用 `extra`（`json::Value`）承载的**未识别字段**
无法经 proto 往返。当前规则：

- 领域 → proto：`data.extra["ExtensionProperties"]` → `data.extension_properties`；
  `data.extra` 里的其它键**不会**出现在 proto 里；
- proto → 领域：`data.extension_properties` → `data.extra["ExtensionProperties"]`。

因此"全字段无损往返"只在 **REST 链路**上成立（C6.1 的语境）；RPC 链路的等价性按
**已建模字段**比对（C7.3 的矩阵比较的是**领域结果**，不是 JSON 字符串）。
