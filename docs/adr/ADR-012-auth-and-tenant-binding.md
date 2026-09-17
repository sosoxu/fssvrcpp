# ADR-012：认证与租户绑定 —— 本地 JWT 校验 + `partition` claim 绑定 + fail-closed

- 状态：**已采纳（Accepted）**（P8 切片 1 落地；RS256/JWKS 与远端 Entitlements 见 §5.3 的未实现清单）
- 日期：2026-09
- 相关：`docs/03-api-contract.md` §1.2/§1.3（头与角色）、§8；`docs/02-design.md` §15（威胁模型 T6/T7）；
  `docs/04-implementation-plan.md` 阶段 8；`src/domain/ports/ports.h`（`IAuthorizer`）

---

## 1. 结论

**鉴权由本服务自己做（不假设前面一定有可信网关），默认 `auth.mode=jwt`，且一律 fail-closed。**

```
IAuthorizer（L3 端口）
 ├── LocalJwtAuthorizer           ← 默认（auth.mode=jwt）：HS256 本地校验 + 角色表
 ├── RemoteEntitlementsAuthorizer ← 可选（auth.mode=remote-entitlements）：**本轮未实现**（见 §5.3）
 └── AllowAllAuthorizer           ← 仅 auth.mode=disabled（开发/测试），启动显著告警 + /v2/info 标记
```

三条不可协商的规则：

1. **缺什么就拒绝什么**：缺 `Authorization` → `401 Missing authorization token`；
   缺 `data-partition-id` → `401 Missing partitionID`（消息逐字节对齐上游）。
2. **租户由 token 绑定，而不是由请求头声明**：`data-partition-id` 头决定"查哪个分区"，
   但**还必须**与 token 里的 `data-partition-id` claim 相等；不等 → `403`。
   否则"用 A 租户的合法 token + 把请求头改成 B"就能读到 B 的数据（§3）。
3. **依赖不可用不得降级为放行**：验签密钥缺失、算法不认识、claim 缺失、远端超时 →
   一律拒绝（`401`/`403`/`503`），**绝无**"跳过校验"的分支。
   远端模式（`auth.mode=remote-entitlements`）的实现与故障注入测试见 P8 切片 2。

---

## 2. 背景：上游是怎么做的，以及我们能依赖什么

上游 OSDU File Service 把鉴权交给两个外部组件：

- **网关/策略层**（`@PreAuthorize` 由 Spring Security 过滤器执行）负责"有没有角色"；
- **Entitlements 服务**负责"这个用户在某租户里有什么角色"（`authorizeAny(headers, roles)`）。

角色字面值与端点映射已经实测并写进契约 §1.3（9 个角色常量、19 个端点、两处"任一角色"）。

**关键问题**：本服务直接暴露时，"前面一定有可信网关"是一个**部署假设**（R8：
不要在部署假设之上做设计）。因此默认必须自己校验，同时保留"信任网关"的部署形态作为显式配置。

---

## 3. 为什么必须把 token 绑到租户（而不是只信任 `data-partition-id` 头）

所有仓储方法都强制 `partition_id`（威胁模型 T6 的一半），但"分区从哪来"决定了隔离是否成立：

```
威胁：攻击者持有 A 租户的合法 token
  请求 1：Authorization: A-token, data-partition-id: A   → 正常（读自己的）
  请求 2：Authorization: A-token, data-partition-id: B   → 若只看头，就读到了 B 的记录
```

OSDU 的 `data-partition-id` 头是**客户端可写**的（网关模式下网关会重写它，但直连时不会）。
所以"头 = 真相"只在"网关可信"的部署里成立。

**采纳的权衡**：token 里必须带 `data-partition-id` claim（可由 Entitlements/IdP 在签发时写入，
也可由网关注入的签名 token 携带）；本服务要求它与请求头一致。代价是"手工构造的测试 token 必须
带这个 claim"——这对本地测试与自动化是**优点**（它逼着测试真的表达租户）。

配置 `auth.jwt.partition_claim`（默认 `data-partition-id`）可指向其它 claim 名；
`auth.jwt.require_partition_claim=false` 只应出现在"网关已可信"的部署里，且**默认是 true**。

---

## 4. 备选方案（≥3，含未实测项）

| # | 方案 | 优点 | 代价 / 不选的理由 | 实测？ |
| --- | --- | --- | --- | --- |
| 1 | **信任网关**：不校验 token，直接采信 `data-partition-id` 头与网关注入的角色头 | 零依赖、零延迟；与上游部署形态一致 | 直接暴露（或网关被绕过）时**完全无鉴权**；无法在单进程测试里证明隔离；R8 明确禁止在部署假设上做设计 | 未实测（也不需要：这是"不设防"） |
| 2 | **本地 JWT 校验（HS256）+ partition claim 绑定**（**采纳**） | 无外部依赖、可离线与单测、fail-closed、延迟可忽略（HMAC-SHA256 约微秒级） | 需要与签发方共享密钥（HS256 的固有代价）；密钥轮换需要支持多密钥（§6 待办） | ✅ 已实测：`tests/unit/test_jwt_authorizer.cpp` + `tests/integration/test_auth_matrix.cpp` |
| 3 | **远端 Entitlements `authorizeAny`（每请求一次远端调用）** | 角色的**唯一权威源**；无共享密钥；租户成员变更即时生效 | 引入外部依赖与 RTT；不可用时必须 fail-closed（否则等于方案 1 的风险）；离线开发需要 mock | ⬜ **未实现**（`auth.mode=remote-entitlements` 目前**拒绝启动**，见 §5.3） |
| 4 | **RS256 + JWKS（非对称）** | 生产最常见：无需共享密钥，IdP 公钥轮换由 JWKS 处理 | 需要 RSA 验签 + JWKS 拉取/缓存/轮换；本轮工作量与风险都高于收益（先把"绑租户 + fail-closed"落地） | ⬜ **未实现**（登记在 §5.3） |

**没有第 5 个候选就下结论**的诱惑在于"上游就是这么做的"——但那正是 R7 警告的外推：
上游的部署形态不等于本项目的部署形态。

---

## 5. 决策细节

### 5.1 token 校验顺序（每一步失败都给固定文案，且都不泄露 token 内容）

1. 空 token → `401 Missing authorization token`
2. 空 partition → `401 Missing partitionID`
3. 去掉 `Bearer ` 前缀（大小写不敏感）
4. 恰好 3 段（`header.payload.signature`），base64url 解码头与载荷
5. `alg` **必须**是 `HS256`：`none` 与未知算法一律拒绝（不是"跳过验签"）
6. 用共享密钥算 `HMAC-SHA256(header.payload)`，与第 3 段**常量时间**比较
7. `exp` **必需**且未过期；`nbf` 存在时不得"还没到"；`iss`/`aud` 配置非空时必须匹配
8. 角色 = `roles_claim`（数组）∪ `auth.local_roles[<user_id_claim>]`（静态表）
9. `partition_claim` 必须存在且等于请求头（默认 `data-partition-id`）→ 否则 `403`

### 5.2 `disabled` 模式的约束（C8.5）

`auth.mode=disabled` 只在非 production 允许：启动日志打印显著告警，`/v2/info` 返回
`authMode: "disabled"`（**可见**，避免"忘了开鉴权"变成静默状态）；production 配置**拒绝**该值。

### 5.3 本轮未实现（如实登记，不静默降级）

| 项 | 现状 | 触发条件 |
| --- | --- | --- |
| `auth.mode=remote-entitlements` | ✅ **已实现**（L2 `RemoteEntitlementsAuthorizer`：超时/连不上/非 200/坏 JSON/缺 `allowed` → **一律 503**，`fail_closed=false` 被配置校验拒绝；接口见契约 §4.5）。**未与真实 Entitlements 联调**（本机网络不可达），故障形态由 `tests/tools/mock_entitlements.py` 注入验证 | 拿到可达的真实 Entitlements → 跑一遍联调并记录 |
| RS256 / JWKS（`auth.jwt.jwks_url`） | 配置非空 → **拒绝启动** | 对接真实 IdP 时 |
| 多密钥轮换（密钥版本化） | 只支持单一 `hmac_secret` | 需要无缝轮换时（§8 待办） |
| ACL 级鉴权（记录级 `acl.viewers`） | 目前是角色级 + 租户级；记录 ACL 未参与判定 | 需要"同一租户内再分权"时 |

---

## 6. 后果

- **好处**：默认安全（fail-closed）；租户隔离可机械验证（A 的 token 读 B → 403）；
  离线可测（无需 IdP）；与 gRPC 面共用同一个 `IAuthorizer`（两条协议行为一致）。
- **代价**：需要与签发方约定共享密钥（HS256）；生产若用 RS256 需要走 §5.3 的后续工作。
- **对测试的影响**：所有已有集成测试都显式注入 `AllowAllAuthorizer`，
  **不得**依赖"配置默认值"（判据 C8.8 明确要求这一点）。

---

## 7. 门槛（新增/衔接）

| 判据 | 证据 |
| --- | --- |
| C8.1 端点 × 角色矩阵 | `tests/integration/test_auth_matrix.cpp` |
| C8.2 跨租户拒绝 | `tests/integration/test_tenant_isolation.cpp` |
| C8.3 JWT 边界（过期/`nbf`/`aud`/`iss`/签名/畸形） | `tests/unit/test_jwt_authorizer.cpp` |
| C8.4 远端不可达 fail-closed | `tests/unit/test_remote_entitlements_failclosed.cpp`（随 `remote-entitlements` 实现） |
| C8.5 `disabled` 告警 + `/v2/info` 标记 | `tests/unit/test_config.cpp`（production 校验）+ `test_ops_endpoints.cpp` |

---

## 8. 待办（不阻塞本轮判据）

1. 远端 Entitlements 实现（超时/重试/fail-closed + mock 服务）。
2. RS256 + JWKS（含缓存与轮换）。
3. 多密钥（`kid` → 密钥表）以支持无缝轮换。
4. 记录级 ACL（`acl.viewers`/`owners`）参与判定。
