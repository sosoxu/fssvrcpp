# 运行手册（Runbook）：症状 → 诊断 → 处置

> 配置项全表、指标清单、容量与安全说明见 [`operations.md`](operations.md)；
> 本文件只回答"**出问题时按什么顺序查、用什么命令、不要做什么**"。
>
> ⚠️ 所有命令里的 `BASE`（`http://127.0.0.1:8080/api/file`）、`PART`（`data-partition-id` 的值，
> 组合根固定为 `opendes`）、`TOKEN`（JWT）请按部署替换。**响应体里的 `x-fss-error-kind` 头**
> 是错误类别的机器可读来源（契约 §5），排障时先看它，再看 `message`。

---

## 0. 三分钟定位（先做这四步）

```bash
BASE=http://127.0.0.1:8080/api/file
curl -sS -o /dev/null -w 'liveness=%{http_code}\n'  "$BASE/v2/liveness_check"    # 进程活着吗
curl -sS -o /dev/null -w 'readiness=%{http_code}\n' "$BASE/v2/readiness_check"   # 依赖就绪吗
curl -sS "$BASE/v2/info"                                                          # authMode/版本
curl -sS -D- -o /dev/null -H "data-partition-id: opendes" "$BASE/v2/files/uploadURL"  # 鉴权链路
```

| 观察 | 说明 |
| --- | --- |
| `liveness` 非 200 | 进程/端口问题 → §1 |
| `liveness` 200、`readiness` 非 200 | 依赖未就绪（存储根/SQLite）→ §2 |
| `/v2/info` 的 `authMode` 是 `disabled` | **生产事故级配置**：生产必须 `jwt` → §5 |
| 上面第 4 条返回 401（`authMode=jwt`） | 正常（无 token）→ 带上 `Authorization: Bearer $TOKEN` 再试 |

**访问日志是第一条线索**：JSON 行里 `route`、`status`、`note`、`duration_ms`、`correlation_id`
四个字段能定位到具体分支；`note` 的取值是固定的（见 §7 的对照表）。

---

## 1. 服务起不来 / 端口不通

```bash
# 启动横幅（stdout）会打印 bind / 端口 / storage driver / storage root / sqlite path / auth 模式
journalctl -u fss --no-pager -n 50        # 或直接看进程 stdout
ss -ltnp | grep -E ':(8080|50051)'        # 端口是否真的在听
```

| 症状 | 诊断 | 处置 |
| --- | --- | --- |
| 起进程立刻退出并打印 `监听失败` | 端口被占用或 `FSS_BIND_ADDRESS` 非法 | 换端口/改绑定地址；检查是否有僵尸进程 |
| `storage.driver=s3 需要 FSS_STORAGE_S3_ENDPOINT...` | S3 模式配置不全 | 补齐三个变量（端点/AK/SK）——**缺一个都会拒绝启动**，这是有意的 |
| `未知的 storage.posix.durability: X` | 档位拼错 | 只允许 `per_file` / `batch` / `never` |
| `打开位置仓储失败` / `打开元数据仓储失败` | 数据目录不可写或 SQLite 文件损坏 | §2 |
| `deployment.mode=multi` 相关校验失败 | **本版本不交付多实例运行形态**（缺 PG 仓储/租约） | 改回 `single`；多实例见 §8 |

**不要**为了"先起来"把 `FSS_AUTH_MODE` 改回 `disabled` 上线 —— 那等于无鉴权（`/v2/info` 会如实显示）。

---

## 2. 就绪检查失败 / 存储与 SQLite 故障

```bash
BASE=http://127.0.0.1:8080/api/file
curl -sS "$BASE/v2/readiness_check"          # 失败时的响应体里有具体原因
ls -ld "$FSS_STORAGE_ROOT" "$FSS_STORAGE_ROOT/blobs"     # 目录存在且可写？
#  ⚠️ 本镜像/环境**没有 sqlite3 CLI**（实测 `command -v sqlite3` 不存在）→ 用 python3 的 sqlite3 模块
python3 - "$FSS_SQLITE_PATH" <<'PY_SQLITE'
import sqlite3, sys
con = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
print("location quick_check:", con.execute("PRAGMA quick_check;").fetchone()[0])
PY_SQLITE
python3 - "$FSS_METADATA_SQLITE_PATH" <<'PY_SQLITE'
import sqlite3, sys
con = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
print("metadata quick_check:", con.execute("PRAGMA quick_check;").fetchone()[0])
PY_SQLITE
df -h "$FSS_STORAGE_ROOT"                                # 磁盘/ inode
df -i "$FSS_STORAGE_ROOT"
```

| 症状 | 诊断 | 处置 |
| --- | --- | --- |
| `readiness` 失败 + `无法解析 staging 存储` | `FSS_STORAGE_ROOT` 不存在/不可写 | 修权限或路径（目录由进程按需创建；父目录必须可写） |
| 写入报 `kUnavailable`（HTTP **503**，`x-fss-error-kind: unavailable`） | 磁盘满 / 配额（`ENOSPC`/`EDQUOT`）/ 存储不可达 | 清空间；确认不是把数据盘写满（GC 见 §4） |
| 12 步序列复制阶段报 **502**（`kBadGateway`） | 依赖服务/存储在复制路径上失败 | 这是**契约规定的投影**（依赖失败 → 502），查存储而不是查客户端 |
| `PRAGMA quick_check` 非 `ok` | SQLite 文件损坏 | 停进程 → 备份 `.db` → 用 `sqlite3 .recover` 导出 → 重建；**不要**直接删库（元数据与位置记录都在里面） |
| 版本链异常（同一 `file_source` 多条 `is_latest`） | 部分唯一索引被误删 | 参见 `db/tests/001_verify_invariants.sql`；**先清数据再重建唯一索引**（R10） |

---

## 3. 上传/下载失败（数据面）

```bash
# 数据面 URL 是自签的：它带 exp/sig，**不需要** Authorization 头
curl -sS "$BASE/v2/files/$FILE_ID/downloadURL" | head -c 400
curl -sS -D- -o /dev/null "<signed_url>"      # 直接看状态码与 x-fss-error-kind
```

| 症状 | 诊断 | 处置 |
| --- | --- | --- |
| `401` + `x-fss-error-kind: unauthenticated` | 自签 token 过期/被篡改/密钥变了 | 重新取 URL；确认所有实例的 `FSS_TRANSFER_SECRET` **一致** |
| `404`（下载已存在的对象） | 位置记录指向的对象不在（被手工删了？） | 用 `getFileLocation` 核对 `container`/`object_key`；缺失对象**不会**静默返回 0 字节（契约要求 404） |
| `400` + `content-length mismatch` | 客户端声明长度 ≠ 实际发送字节 | 查客户端/代理；**不要**把它当成鉴权问题 |
| `408` + `timed out` | 空闲超时（数据面**没有**整体超时） | 调大 `server.http.transfer_idle_timeout_sec`；检查是否 NAT/代理掐连接 |
| `413` / `400`（体积类） | 超过限额（头/URI/体/连接数） | 见 `operations.md` 的限额表；`/metrics` 的 `fss_http_rejected_total` 有分类计数 |
| 传大文件时 RSS 持续上涨 | 走了非流式路径 | 用区间读/流式；对比 `test_posix_large_file` 的 RSS 断言（1 GiB 流式增长 < 64 MiB） |

**不要**用 `curl -T` 对着 `/v1/transfer/<token>` 之外的路径 PUT（那是 404 的常见来源：
自签 URL 的 base path 是 `/api/file`）。

---

## 4. GC 与残留临时文件

```bash
# 指标（唯一免鉴权端点）
curl -sS http://127.0.0.1:8080/metrics | grep -E 'fss_gc_'
# 残留临时文件的直接观察（staging 容器目录；键里含 .tmp. 的文件**不是对象**）
find "$FSS_STORAGE_ROOT/blobs/opendes-staging" -name '*.tmp.*' -mmin +60 -ls | head
```

| 指标/症状 | 含义 | 处置 |
| --- | --- | --- |
| `fss_gc_runs_total{mode="dry_run"}` 涨、`fss_gc_tmp_removed_total` 不涨 | dry-run 只报候选 | 确认无误后把 `gc.dry_run` 置 false |
| `fss_gc_skipped_total{reason="has_record"}` 涨 | GC 按设计**永不删**有记录的对象 | 正常 |
| `fss_gc_skipped_total{reason="tmp_too_young"}` 涨 | **在途上传被正确保护**（TTL 内） | 正常；持续不降说明有上传卡住（查 §3 的 408） |
| `fss_gc_tmp_removed_total` 长期不涨但目录里全是 `.tmp.` | **GC 未启用**：组合根装配了周期调度，但 `gc.enabled` 的默认是 `false`（不提供配置时不跑 GC）；也可能是 `gc.dry_run=true` 只报候选 | 显式配 `gc.enabled=true` + `gc.dry_run=false` 后重启（看启动横幅的 `gc :` 行）；一次性清理用 `--once`；**不要**用 `rm -rf` 清整个容器目录 |
| 误删/误留（怀疑 TTL 判定） | 时间戳语义问题曾在 P9 被修复（P9-D04/D08） | 升级到修复版本；用 `list()`/`stat()` 的时间戳与 `stat -c %Y` 对照 |

---

## 5. 鉴权与租户（401 / 403 / 503）

| 症状 | 诊断 | 处置 |
| --- | --- | --- |
| 全部端点 401 | `auth.mode=jwt` 但 token 缺失/过期/签名不符/`iss`/`aud` 不匹配 | 用 `jwt` 的三段解码核对 `exp`/`nbf`/`iss`/`aud`；确认 `FSS_JWT_HMAC_SECRET` 与签发方一致 |
| 跨租户访问 403 | token 的 `partition` claim 与 `data-partition-id` 头不一致 | **这是设计行为**（租户绑定） |
| 读/删其它租户的记录 → 404 | 分区隔离（不泄漏存在性） | 正常 |
| `503` + `unavailable`（配了远端 Entitlements 时） | 远端鉴权服务超时/5xx/坏 JSON/连不上 | 先修依赖；**fail-closed 是有意的**（宁可拒绝，不放行） |
| 审计缺失 | 审计器写失败（`observability.audit_fail_closed=false` 时是**非致命**） | 查审计后端；若要强一致改成 `true` |

**不要**在生产把 `auth.mode` 设为 `disabled`；`/v2/info` 会把它显示出来（这是 C8.5 的可见性要求）。

---

## 6. 容量与性能异常

```bash
# 基线（独立进程 + 互不重叠绑核 + 每点位 3 次取中位数）
scripts/bench_baseline.sh            # 跑一遍并与基线比较
scripts/bench_baseline.sh --check    # 退化 >20% 直接失败（退出码 1）
```

| 症状 | 诊断 | 处置 |
| --- | --- | --- |
| 小请求吞吐骤降、p99 出现 ~40 ms 台阶 | `TCP_NODELAY` 没开（或反向代理没开） | 服务端/代理都开；参见 §1.2 的 950x 实测 |
| 并发上来后吞吐不升反降 | 并发不是越多越好（本机实测 c4 之后不再提升） | 按 `worker_threads`/`max_connections` 与目标硬件重算；先用 `--check` 定位 |
| 小文件上传只有百级 files/s | 逐文件 `fsync` | 用 `durability=batch`（实测 4.3x）；**不要**用 `never` 承载 staging/persistent 的对象 |
| `503` + `Retry-After` | 在途连接数超上限（背压生效） | 调 `worker_threads`/`max_connections` 或加副本（多实例见 §8） |
| 基线与本次差异 >20% | **先怀疑测量环境**：批量删除/回写会污染写入点位（实测把 p99 从 7~44 ms 抬到 293~890 ms） | 在空闲机器上重跑；跨会话漂移可达 40%（已登记） |

---

## 7. 访问日志 `note` 字段对照表

| `note` | 含义 | 首要怀疑 |
| --- | --- | --- |
| `route_not_found` | 路由未匹配 | 路径/base path/方法 |
| `duplicate_content_length` | 重复 `Content-Length`（走私防护） | 代理/客户端 |
| `content_length_with_chunked` | `CL` 与 `chunked` 并存 | 代理 |
| `content_length_too_large` | 前置按声明长度拒绝（413） | 客户端 |
| `body_too_large_413` / `body_too_large_400` | 读体超限 | 限额配置 |
| `request_timeout` / `request_body_idle_timeout_408` | 超时（数据面无整体超时） | §3 |
| `body_read_failed` | 读体失败（可能伴随 handler 已报错保留） | 客户端中断/网络 |
| `handler_error_kept_over_body_read_failure` | handler 的错误码被保留（P9-D05 的修复） | 看 handler 的真实错误码 |
| `handler_did_not_consume_body` | **流式 handler 没读干请求体就回了 2xx**（P9-D07 的守卫，回 500） | 这是服务端缺陷，报 bug |
| `handler_exception` | handler 抛异常 | 看栈/日志 |
| `transfer_token_rejected`（指标 `fss_transfer_token_rejected_total`） | 自签校验失败 | §3 |

---

## 8. 多实例 / 部署注意（未交付部分）

- `deployment.mode=multi` 在本版本**拒绝启动**（缺 PG 版仓储/租约/数据库时钟）。
  多实例前的硬前提：**NFS 语义验证（C9.27）**、PG 仓储与租约落地、`syncfs` 干扰评估（C9.24）。
- 共享 POSIX 挂载上的临时文件命名包含实例标识（`instance_id`+`pid`+计数），
  **不要**手工清理正在被其它实例写入的 `.tmp.*`（用 GC 的 TTL 判定）。
- `syncfs` 是**文件系统级**操作：建议按 partition 分盘，避免实例间互相拖慢。

---

## 9. 上报缺陷时请附上

1. `/v2/info` 输出（版本、`authMode`）；
2. 失败请求的 `correlation_id` + 访问日志那一行（含 `note`）；
3. `curl -D-` 的**完整响应头**（`x-fss-error-kind` 是关键）；
4. 若与容量相关：`scripts/bench_baseline.sh --check` 的输出（含 `client_cpu_pct`）；
5. 环境：`uname -sr`、容器/裸机、存储类型（本地盘/NFS/对象存储）。

**不要**在生产上"先关鉴权再复现"——那会把一个可诊断的问题变成安全事故。
