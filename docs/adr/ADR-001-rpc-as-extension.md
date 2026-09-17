# ADR-001：以 gRPC/RPC 作为**平台外扩展**协议，而非 OSDU 合规接口

- 状态：已采纳（Accepted）
- 日期：2025（阶段 0）
- 决策者：项目组
- 相关文档：`docs/01-osdu-research.md` §9、`docs/03-api-contract.md`

## 背景

需求方希望本服务"提供 http 和 rpc 两种协议接口"，并询问"OSDU 的接口是否支持 rpc"。

需要先回答一个事实性问题：OSDU 规范里是否存在 RPC/二进制接口？

## 调研证据

| 证据 | 结论 |
| --- | --- |
| `osdu/platform/system/file` 仓库 master 分支全量文件检索 `*.proto` | **0 个匹配**（本项目实际拉取仓库后验证） |
| File Service 全部 API 声明方式 | Spring `@RestController` + `@GetMapping/@PostMapping`，Swagger/OpenAPI 注解 |
| 各服务 `docs/api/<version>/openapi.yaml` | 全部为 OpenAPI 3.x，`securitySchemes` 仅 `http/bearer` |
| Core Services Overview wiki 的 "Open API Spec" 列 | 每项均为 `.yaml`/`.json` OpenAPI 文件，无一条 RPC |
| M26 版本说明（最新已验证发布版本） | 未列出任何 gRPC/protobuf/RPC 特性或依赖 |
| OSDU GitLab 中 "grpc" 命中的位置 | 仅为 GCP/`google-cloud-cpp` 的传递依赖，非 API 定义 |
| Reservoir DDMS 的 ETP 协议 | WebSocket + Apache Avro，**明确声明不使用 Avro RPC 机制**（"ETP does not use the Avro RPC facility"） |
| Reservoir DDMS 的 GraphQL | HTTP/JSON 之上的查询语言，非 gRPC |
| Notification / Stream Admin Service | REST + HTTP webhook / Kafka，接口本身仍是 OpenAPI |

参考来源：
- [File Service 源仓库](https://community.opengroup.org/osdu/platform/system/file)
- [File Service 文档](https://osdu.pages.opengroup.org/platform/system/file/File-Service/)
- [Core Services Overview](https://community.opengroup.org/groups/osdu/platform/-/wikis/Core-Services-Overview)
- [Storage OpenAPI（AppError / bearer / cursor 分页）](https://community.opengroup.org/osdu/platform/system/storage/-/blob/master/docs/api/community/v2/openapi.yaml)
- [Register OpenAPI（DDMS 注册走 OpenAPI 3.0 文档）](https://community.opengroup.org/osdu/platform/system/register/-/blob/master/docs/api/community/v1/openapi.yaml)
- [HowToBecomeADDMS](https://osdu.pages.opengroup.org/platform/system/register/HowToBecomeADDMS/)
- [M26 Release Notes](https://community.opengroup.org/osdu/governance/project-management-committee/-/wikis/M26-Release-Notes)
- [ETP 1.1 规范](https://docs.energistics.org/ETP/ETP_TOPICS/ETP-000-012-0-C-sv1100.html)

**结论：OSDU 不存在、也不认可 gRPC/RPC API。** OSDU 的规范面（合规面）是 REST/HTTP+JSON + OpenAPI 3.x。

诚实标注未验证项：GitLab 匿名全局 blob 检索被拒绝（HTTP 401），因此"OSDU 全组无任何 `.proto` 文件"未能穷尽证明；但**已证明没有任何已发布的 OSDU API 契约或文档定义 protobuf/gRPC 接口**。

## 决策

1. **REST/HTTP+JSON 作为唯一 OSDU 合规面。** 端点路径、请求/响应 JSON、状态码、错误体、请求头严格对齐 OSDU File Service 规范，可被 OSDU 客户端直接使用。
2. **gRPC 作为平台外扩展面**，在同一进程、同一应用层之上提供，用 `proto/osdu/file/v1/file_service.proto` 定义。
3. **扩展必须与规范面隔离**：
   - gRPC 监听独立端口（默认 50051），不复用 REST 端口；
   - 集中存储模式下的字节通道使用 `/v1/transfer/...` 前缀，与 `/v2/...` OSDU 命名空间隔离；
   - 扩展端点不得改变任何 OSDU 端点的既有语义。
4. **语义强制一致**：同一操作经 REST 与 RPC 必须产出等价的领域结果与等价的错误分类，并由**双协议等价性测试**（阶段 7 门槛）在 CI 中强制。
5. 不在短期内尝试走 OSDU 的 DDMS 注册流程（`POST /api/register/v1/ddms` 只接受 OpenAPI 文档，无法承载 RPC）。

## 备选方案与取舍

| 方案 | 优点 | 缺点 | 结论 |
| --- | --- | --- | --- |
| A. 只做 REST | 完全合规、工作量最小 | 不满足"双协议"需求；无法提供真正的流式传输 | 否决 |
| B. gRPC 作为主要接口，REST 由网关转换（grpc-gateway 思路） | 单一 IDL 源 | OSDU 的 JSON 契约（PascalCase schema 字段、`AppError`、`data-partition-id`）无法由 proto 自动生成；网关会引入不可控的契约漂移 | 否决 |
| C. **REST 为规范面 + gRPC 为扩展面，共享应用层，契约显式映射 + 等价性测试** | 合规性可控；两种协议零业务重复；漂移可被测试捕获 | 需人工维护映射表（数量有限，约 15 个操作） | **采纳** |
| D. 采用 ETP（Avro over WebSocket）作为二进制面 | 有 OSDU 先例 | 面向 Reservoir 领域数据流，语义与本服务不匹配；实现成本高于 gRPC | 否决（记录为未来可选） |

## 影响

- 正面：REST 面的 OSDU 兼容性可独立于 RPC 面进行符合性验证；RPC 面提供 gRPC 流式上传/下载，弥补 POSIX 集中存储没有原生签名的短板。
- 负面 / 成本：`docs/03-api-contract.md` 中的映射表必须随接口演进同步维护，否则等价性测试会失败（这是有意的——把漂移变成构建失败）。
- 需要向使用方明确沟通：**gRPC 面不属于 OSDU 规范，不参与 OSDU 合规性认证**。
