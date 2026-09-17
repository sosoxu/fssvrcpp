# 阶段 5 测试证据（进行中）

| 项 | 值 |
| --- | --- |
| 阶段 | P5（对象存储驱动（S3 SigV4）） |
| 状态 | ✅ **已完成（切片 1~5/5）** —— C5.1~C5.10 全部满足（5：**AWS 官方已知答案向量** + virtual-host 数据面 + ADR-005 定稿） |
| 门槛命令 | `ctest -L phase5` |
| 退出码 | `0`（6 测试 / 1737 断言 + `verify_driver_switch.sh` 两模式端到端） |

> **未实测（如实标注，不影响判据）**：真实 S3/MinIO/Ceph/SeaweedFS 端到端（无端点与凭证）；
> 分片上传（>5 GiB）；`SlowDown` 退避重试；STS 会话令牌刷新。详见 `docs/adr/ADR-005-s3-driver.md` §7。

---

## 1. 切片 1 交付物（SigV4 签名器）

| 路径 | 内容 |
| --- | --- |
| `src/infra/blob/s3/sigv4_signer.{h,cpp}` | `SigV4Encode`（两种场景）、canonical request / string-to-sign / 派生密钥 / HMAC、头部签名（`Authorization`）、查询串预签名（`X-Amz-*`）、path-style / virtual-host、`BuildUrl`/`HostFor` |
| `tests/unit/test_sigv4_signer.cpp` | C5.5 编码边界、canonical request 逐字节形状、**签名覆盖性**（改动任一输入必须改签名）、预签名结构/有效期上限（1..604800）/两种寻址 |
| `tests/integration/test_sigv4_crosscheck.cpp` | **两条独立实现对拍**：libcurl `--aws-sigv4`（第三方实现）+ Python 参照实现（10 个向量 + 预签名 URL 逐字节） |
| `tests/tools/sigv4_reference.py` | Python 独立参照实现（只用 `hmac`/`hashlib`）；`--sign-vector` / `--presign-vector` 两个 CLI，后续给 mock-S3 复用 |

### 为什么"独立实现对拍"是这一层的核心证据

单元测试里的期望值**全部由我们自己的规则推出**（canonical URI 要不要二次编码、query 怎么排序、
头值怎么压缩空白）。规则若理解错，单元测试会**一起错**而全绿 —— 这是 P1-D07 的原样教训。
因此加了两条**不共享我们代码**的路径：

| 路径 | 做法 | 结果 |
| --- | --- | --- |
| libcurl `--aws-sigv4` | 让 curl 对同一请求签名（捕获它真正发出去的请求），我们按**它实际签的那组输入**复算，逐字节比对整个 `Authorization` | 3 个用例全部一致（含带体 PUT） |
| Python 参照实现 | 同一组向量两边各算一次签名；含 `~`/空格/`+`/`%`/非 ASCII/多层 `/` 的编码边界 | 10 个向量 + 预签名 URL **逐字节相同** |

## 1b. 切片 2 交付物（mock-S3 独立验签 + 预签名地址）

| 路径 | 内容 |
| --- | --- |
| `tests/tools/mock_s3.py` | Python 标准库写的假 S3（path-style）：`PUT/HEAD /bucket`、`PUT/GET/HEAD/DELETE /bucket/key`、`GET /bucket?list-type=2`（prefix / max-keys / continuation-token）、`x-amz-copy-source` 复制、`Range`（含 206/416），以及可触发的错误（`.denied`→403 `AccessDenied`、`.slowdown`→503 `SlowDown`、缺失键→404 `NoSuchKey`）。**验签用 `sigv4_reference.py` 重算**，两种形态都验：查询串预签名与头部签名 |
| `src/infra/blob/s3/s3_blob_store.{h,cpp}` | `S3BlobStore`：`capabilities()`（`native_presign`/`server_side_copy`/`range_read`/`streaming_put`）、`presign_put`/`presign_get`（有效期缺省/上限校验、`UNSIGNED-PAYLOAD`）、`BuildUrl`、path-style/virtual-host；数据面显式 `kUnimplemented`（切片 3） |
| `tests/integration/test_s3_presign_verify.cpp` | C5.2：预签名 PUT→独立验签 200→预签名 GET 取回**字节一致**；**篡改矩阵 7 条**全部 403；头部签名（数据面口径）被独立验过；**反向自证**（换密钥的 mock 必须拒绝同一个 URL） |

### 为什么"C5.2 通过"这件事本身也需要自证

"mock 验签通过"很容易是假象：mock 可能压根没验、或者验的是别的东西。因此：

| 自证 | 做法 | 若缺失会漏掉什么 |
| --- | --- | --- |
| **反向**：换密钥 | 用另一个 `secret_key` 起第二个 mock，同一个 URL 必须 403 | mock 根本没验签时，"通过"毫无意义 |
| **篡改矩阵** | 改签名/key/bucket/`X-Amz-Expires`/`X-Amz-Credential`/`X-Amz-SignedHeaders`/过期 → 逐条 403 | 只测"合法 URL 通过"无法证明签名**覆盖**了这些输入 |
| **基线对照** | 篡改矩阵开始前先跑一次未篡改的请求（必须 200） | 否则一串 403 可能只是因为 URL 本来就不可用 |
| **头的反例** | 头部签名通过后，把 `x-amz-content-sha256` 改成别的值（体不变）→ 403 | 证明"体哈希确实进了签名" |

---

## 1c. 切片 3 交付物（S3 数据面：libcurl 流式 + 同一套契约）

| 路径 | 内容 |
| --- | --- |
| `src/common/result/result.h` + `docs/03-api-contract.md` §5 | **新增 `kStorageAccessDenied`**（存储侧拒绝 ≠ 调用方没权限）：enum + 名字 + 映射（403）+ 契约表 + 覆盖率矩阵 13/13 + 显式清单 13 |
| `src/infra/blob/s3/s3_blob_store.cpp` | 数据面：`ensure_container`（HEAD → 404 则 PUT）、`put`（流式上传）、`get`（流式下载 + `Range`）、`stat`（HEAD）、`remove`（幂等）、`copy`（`x-amz-copy-source` + 补一次 HEAD 拿 size）、`list`（ListObjectsV2 + 分页） |
| 同上（错误映射） | `<Error><Code>` → `ErrorKind`：`NoSuchKey`/`NoSuchBucket`→`kNotFound`、`AccessDenied`/`SignatureDoesNotMatch`→**`kStorageAccessDenied`**、`SlowDown`→`kUnavailable`、`BadDigest`→`kChecksumMismatch`、`InvalidRange`/416→`kInvalidArgument`、其余 5xx→`kBadGateway` |
| `tests/tools/mock_s3.py` | 补：桶必须**先**创建（`NoSuchBucket`）、`x-amz-checksum-sha256` 独立重算校验（`BadDigest`）、`--seed-count` 便于分页用例、**不透明**的 continuation token |
| `tests/integration/test_s3_blob_store.cpp` | C5.3：`CheckBlobStoreContract(S3BlobStore)`（与 memory/POSIX **同一份**断言）；C5.6：1005 个键分页"不重不漏 + 严格字典序"；C5.7：错误映射逐条 |

### 数据面的三条口径（都是实测/契约决定，不是"看起来合理"）

| 口径 | 取值 | 依据 |
| --- | --- | --- |
| 上传的 `x-amz-content-sha256` | `UNSIGNED-PAYLOAD` | 流式上传无法在发第一字节前知道整包哈希；HTTPS 下 S3 接受（本 mock 也接受） |
| 有 `expected_checksum` 时 | 同时发 `x-amz-checksum-sha256`（base64）**并**本地边传边算 | 存储侧拒收错包 + 客户端能给出 `kChecksumMismatch` 的 `details{expected,actual}`（契约 §5） |
| 超时 | 数据面无整体超时；只有"无进展"空闲断开（`transfer_idle_timeout_seconds`） | 与 C4.11 同一原则：TB 级传输不能被整体超时打断 |

### 由"同一套契约"暴露出来的三个真缺陷

| # | 现象 | 根因 | 为什么只有跑到第三个实现才发现 |
| --- | --- | --- | --- |
| **P5-D04** | 契约的分页用例期望 3 页，实际 4 页且末页重复 | mock 用"上一页最后一个 key"当 continuation token（S3 的 token 是**不透明**的，客户端只回传） | 分页语义在 memory/POSIX 实现里是"直接切片"，根本没有 token 往返 |
| **P5-D05** | `list` 只解析出 ~425 个键（1005 个里） | 响应体采集上限 64 KiB 是给**错误体**设计的，成功的 `ListBucketResult`（一页 1000 键 ≈ 200 KiB）被**静默截断** | 前两个实现没有"HTTP 响应体大小"这个维度；截断后 XML 仍是合法前缀，解析不报错 |
| **P5-D06** | `copy` 返回的 `ObjectStat.size == 0`；`get` 不可满足区间报 `kInternal` | S3 的 `<CopyObjectResult>` 不含大小（契约却要求完整 `ObjectStat`）；416 没有被映射 | 前两个实现天然能返回 size；错误码映射只有真实 S3 才有 |

> P5-D05 的教训值得单独记：**"截断"比"报错"危险**。当时的写法是"采满上限就丢掉后面的"，
> 解析出来的仍是一个语法合法的 XML 前缀，只是少了 580 条 —— 表现为"分页数字不对"而不是
> "解析失败"。现在采满即**报错**（`sink_failed`），不再静默丢数据。

---

## 1d. 切片 4 交付物（S3 模式端到端 + 按配置切驱动）

| 路径 | 内容 |
| --- | --- |
| `tests/integration/test_upload_flow_s3.cpp` | **C5.8**：`uploadURL` → **直连存储端点** PUT → `metadata`（服务端自己做 staging→persistent 复制）→ `downloadURL` → **直连存储端点** GET → 字节一致 → `DELETE` 204 → 再读 404；另测 `Range`（206 + 尾部 8 字节） |
| `tests/framework/mock_s3.h` | 把"起 mock-S3 进程"抽成公共脚手架（`--port 0` + 轮询 stdout 的 `LISTENING`，无固定 sleep）；三个 S3 测试共用，消除三份重复 |
| `src/main/server_main.cpp` | **驱动按配置选择**：`FSS_STORAGE_DRIVER=posix|s3`（默认 posix）+ `FSS_STORAGE_S3_*`；两种驱动都只在组合根创建（R12）；S3 模式下**不注册**自签数据面（原生预签名 → 客户端直连） |
| `scripts/verify_driver_switch.sh` | **C5.9**：同一二进制（记录 sha256）+ **同一段**端到端脚本，posix/s3 各跑一遍全绿；并断言两种模式的 `SignedURL` host **确实不同**（本服务 vs 存储端点）——否则"只改配置"是空话 |

### C5.8 的三条断言为什么必须写在**地址**上

| 断言 | 若缺失会漏掉什么 |
| --- | --- |
| `SignedURL` 的 host == 存储端点（mock） | 实现可能"假装原生预签名"，实际仍把字节代理过服务（那就退化成 POSIX 语义，S3 的带宽优势全没了） |
| URL 里**不得**出现 `/api/file/` 与 `/v1/transfer/` | 只断言 host 会被"同域反代"式实现蒙混（服务恰好与存储同主机时看不出来） |
| URL 里必须有 `X-Amz-Signature=` | 证明走的是**原生 SigV4 预签名**，而不是别的自签方案 |

### 两种模式的可观测差异（C5.9 的判据落点）

| 模式 | `SignedURL` host | 字节路径 | 服务端数据面 |
| --- | --- | --- | --- |
| `FSS_STORAGE_DRIVER=posix` | `127.0.0.1:<本服务端口>` | 经服务代理（`/api/file/v1/transfer/{token}`） | 注册 |
| `FSS_STORAGE_DRIVER=s3` | `<存储端点>`（mock-S3） | **客户端直连存储** | **不注册**（预签名已足够） |

---

## 1e. 切片 5 交付物（AWS 官方向量 + virtual-host 数据面 + ADR-005）

| 路径 | 内容 |
| --- | --- |
| `tests/unit/test_sigv4_aws_vectors.cpp` | **C5.1**：AWS 官方已知答案向量 —— **5 条完整请求逐字节匹配** + **2 条已知有意不同**（各附理由与断言） |
| `tests/tools/mock_s3.py` | virtual-host 形态识别（按 `Host` 的子域取桶名）；`--virtual-host-suffix` 可配 |
| `tests/integration/test_upload_flow_s3.cpp` | **C5.4**：virtual-host 预签名 PUT/GET 经 `curl --connect-to` 直连 mock；与 path-style 指向同一份存储 |
| `src/infra/blob/s3/s3_blob_store.{h,cpp}` | 新增 `ObjectPath()`/`BucketPath()`：**寻址形态决定路径里是否含桶名**（P5-D09） |
| `docs/adr/ADR-005-s3-driver.md` | S3 驱动定稿：4 个备选方案取舍、编码/签名/寻址/错误映射约定、与 libcurl 和 Go SDK 的 4 处实测差异、兼容矩阵（**明确标注哪些未实测**）、未实现项 |

### C5.1 的官方向量从哪来（可复查）

| 项 | 值 |
| --- | --- |
| 包 | Ubuntu jammy `golang-github-aws-aws-sdk-go-dev` **1.41.14-1ubuntu1**（Apache-2.0） |
| 文件 | `usr/share/gocode/src/github.com/aws/aws-sdk-go/aws/signer/v4/{v4_test.go,functional_test.go}` |
| 取证 | `apt-get download golang-github-aws-aws-sdk-go-dev && dpkg-deb -x *.deb out`（无需 root） |
| 凭据/时间 | `AKID` / `SECRET` / 会话令牌 `SESSION`；`19700101T000000Z`（AWS 自己的测试常量） |

**5 条逐字节匹配的向量**（覆盖了完全不同的分支）：

| # | 来源 | 覆盖点 | 期望值 |
| --- | --- | --- | --- |
| ① | `functional_test.go` `TestPresignHandler`(29) | **S3 预签名 + virtual-host + 头值含 `+`/空格/`$`** | `2d76a414…54f55b` |
| ② | `functional_test.go` `TestStandaloneSign_WithPort`(151) | 头部签名 + **非默认端口进 canonical host** | `cd9d926a…f51b4f` |
| ③ | 同上（default HTTP port） | 头部签名 + **默认端口不进 canonical host** | `54ebe60c…7cb951` |
| ⑤ | `v4_test.go` `TestSignRequest`(196) | **同名头逗号连接** + 会话令牌进 SignedHeaders | `a5182993…652ad9` |
| ⑥ | `v4_test.go` `TestPresignRequest`(129) | **非 S3 预签名要签真实体的哈希** | `122f0b9e…be5581` |

**2 条已知有意不同**（不是"漏了"，是可执行记录）：

| 向量 | 对方行为 | 我们的选择 |
| --- | --- | --- |
| `TestPresignBodyWithArrayRequest`(162) | Go 的 `url.Values.Encode()` 对同名 query 参数**保留解析顺序** | 按 AWS 规范**按值排序** |
| `standaloneSignCases[0]`(18) | Go 对已含 `%XX` 的路径**二次编码**（`%2A`→`%252A`） | **单次编码**（canonical URI = 线上路径字节） |

### 官方向量抓到的新缺陷（P5-D08）

| # | 现象 | 根因 |
| --- | --- | --- |
| **P5-D08①** | 向量⑤的签名对不上 | 同名头我们写成"**后值覆盖前值**"，而 AWS 规范要求**逗号连接**后再去多余空白 |
| **P5-D08②** | 向量⑥的签名对不上 | 预签名的 payload 哈希被**硬编码**成 `UNSIGNED-PAYLOAD`；非 S3 服务（如 DynamoDB）的预签名要签真实体的哈希 |

> 这两条都只有"**别人写的期望值**"才抓得到：我们自己的规则推导出的测试会一起错（P1-D07 的同一课）。

### C5.4：virtual-host 的数据面

`force_path_style=false` 时，桶名在 **Host 的子域**里，因此**路径里不能带桶名**——数据面原先硬编码
`"/" + container + "/" + key`，在 virtual-host 下会变成 `/<bucket>/<bucket>/<key>`（P5-D09，已修）。
测试不引入生产代码钩子：预签名 URL 用 virtual-host 形态，客户端用 `curl --connect-to
bucket.s3.amazonaws.com:80:127.0.0.1:<mock_port>` 连到 mock（真实部署里由 DNS 完成）。

---

## 2. 门槛命令与输出

```console
$ cmake --build build -j8 && ctest --test-dir build -L phase5 --output-on-failure
    Start 34: test_sigv4_signer ....................   Passed    0.00 sec
    Start 35: test_sigv4_crosscheck ................   Passed    0.06 sec
    Start 36: test_s3_presign_verify ...............   Passed    0.80 sec
    Start 37: test_s3_blob_store ...................   Passed    2.30 sec
    Start 38: test_upload_flow_s3 ..................   Passed    0.60 sec
    Start 39: test_sigv4_aws_vectors ...............   Passed    0.00 sec
100% tests passed, 0 tests failed out of 6

逐测试断言数：
  test_sigv4_signer           91 assertions in 4 test cases   ← C5.1（形状/覆盖性）+ C5.5（编码）
  test_sigv4_crosscheck      118 assertions in 2 test cases   ← C5.1/C5.5（两条独立实现对拍）
  test_s3_presign_verify      70 assertions in 3 test cases   ← C5.2（独立验签 + 篡改矩阵）
  test_s3_blob_store        1338 assertions in 3 test cases   ← C5.3（同一套契约）+ C5.6（分页）+ C5.7（错误映射）
  test_upload_flow_s3         93 assertions in 3 test cases   ← C5.8（S3 端到端 + Range）+ C5.4（virtual-host 数据面）
  test_sigv4_aws_vectors      27 assertions in 6 test cases   ← C5.1（AWS 官方向量 5 条 + 2 条有意不同）
  ─────────────────────────────────────────────
  合计 1737 个断言 / 19 个测试用例 / 6 个测试

$ ./scripts/verify_driver_switch.sh
  · ① POSIX 模式（自签数据面）    ✓ 端到端通过（SignedURL host = 127.0.0.1:18401）
  · ② S3 模式（原生预签名）      ✓ 端到端通过（SignedURL host = 127.0.0.1:41365）
  ✓ 同一二进制（sha256=5d718bc5e44d8317…）+ 同一段脚本，两种模式全绿，且地址形态确实不同
```

## 3. 判据进展

| 判据 | 状态 | 证据 |
| --- | --- | --- |
| **C5.1** SigV4 官方向量逐字节匹配 | ✅（**5 条完整请求**，另 2 条已知有意不同并附理由） | `test_sigv4_aws_vectors`：AWS Go SDK 自带的已知答案向量（见 §1e 的来源与取证命令）。另有两条独立实现对拍（libcurl `--aws-sigv4` + Python 参照实现）作为补充证据 |
| **C5.5** URL 编码边界 | ✅（编码部分） | 单元测试 20 条边界（`~` 不编码、空格 `%20`、`+` `%2B`、`%` `%25`、非 ASCII 逐字节大写、`/` 分场景）+ Python 参照实现对拍 10 个向量 |
| **C5.2** mock-S3 独立验签 + 篡改必失败 | ✅（预签名与头部签名两条路径） | `test_s3_presign_verify`：预签名 PUT 经 Python 独立重算后 200 并写入、预签名 GET 取回**字节一致**；**篡改矩阵 7 条**（签名/key/bucket/`X-Amz-Expires`/`X-Amz-Credential`/`X-Amz-SignedHeaders`/过期）全部 403；头部签名 + 篡改 `x-amz-content-sha256` → 403；**换密钥的 mock 拒绝同一 URL**（反向自证） |
| **C5.3** 同一套契约跑第三遍 | ✅ | `test_s3_blob_store`：`CheckBlobStoreContract(S3BlobStore)` —— 与 memory/POSIX **逐条相同**的断言（put/get/空对象/expected_size/expected_checksum+details/stat/Range/幂等删除/copy/分页/容器隔离） |
| **C5.6** 分页 >1000 键 | ✅ | 1005 个键、`max-keys=1000`：翻页收集全部键，断言**不重不漏**、页数 ≥2、严格字典序、首尾键正确；另测前缀过滤 |
| **C5.7** 错误映射 | ✅ | `AccessDenied`→`kStorageAccessDenied`（**不是** `kPermissionDenied`）、`SlowDown`→`kUnavailable`、`NoSuchBucket`→`kNotFound`、`get` 缺失→`kNotFound` **且错误 XML 不污染 sink**、`remove` 缺失→`Ok`、416→`kInvalidArgument` |
| **C5.8** S3 模式端到端 | ✅ | `test_upload_flow_s3`：`SignedURL` 指向存储端点、无 `/api/file/`、含 `X-Amz-Signature`；直连 PUT/GET 字节一致；`metadata` 走服务端复制；`Range` 206；`DELETE` 204 → 404。`verify_driver_switch.sh` 在**真实二进制**上再跑一遍 |
| **C5.9** 按配置切驱动 | ✅ | `scripts/verify_driver_switch.sh`：同一二进制（sha256 记录并复核）+ 同一段端到端脚本，`posix`/`s3` 各跑一遍全绿；并断言 `SignedURL` host 分别为本服务/存储端点 |
| C5.4（寻址部分） | 🚧 部分 | path-style / virtual-host 的 host 与路径已有单元断言 + 预签名 URL 断言；两模式的**数据面**待切片 3 |

## 4. 本切片发现并修复的缺陷与实测发现

| # | 内容 | 处置 |
| --- | --- | --- |
| **P5-D01** | **`std::move` 之后再用同一变量 → SigV4 签名变成"与输入无关的常量"**：`out.string_to_sign = std::move(string_to_sign);` 之后才 `HmacHex(signing_key, string_to_sign)`，于是 HMAC 的输入是已被搬空的字符串。**GET 与 PUT、不同路径、不同密钥算出的签名完全相同** | 先移动、再用 `out.string_to_sign` 求 HMAC。★ 抓到它的正是"签名覆盖性"用例（R1）：只断言"签名是 64 位 hex / 非空"的测试会**完全放过**这个 bug —— 这类"输出的形状对、内容恒等"的缺陷必须有"改一个输入就必须变"的断言 |
| P5-M01 | **libcurl 7.81 的 `--aws-sigv4` 把 `host` 签成"URL 主机名（去端口）"**，而请求里发出的 `Host:` 带端口（实测：signature 只在 `host=127.0.0.1` 时与它一致） | **不跟随**：真实 S3/MinIO 按收到的 `Host`（含非默认端口）重算。对拍时显式把 curl 用的 host 传进我们的签名器，保证比的是"同一组输入 + 同一个算法" |
| P5-M02 | **libcurl 7.81 不排序 canonical query**（AWS 规范要求字典序）：`prefix=J&max-keys=2` 时 curl 的签名等于"原文序"计算，排序后反而不同（实测两者签名不同） | **不跟随**：我们的签名器始终排序（单元测试有正例断言"顺序不同、签名相同"）。对拍只使用**已排序**的 query，并在测试里写明原因 |
| **P5-D02** | **`x-amz-date` 少了"截到秒"这一步**：从 `time::ToIso8601Utc`（带毫秒）直接删分隔符得到 `20231114T221320.000Z` —— mock 侧 `strptime` 解析失败，**表现成"签名不匹配"的 403**（错误的时间格式伪装成签名错误，排障方向完全被带偏） | 先截到秒再删分隔符（`iso.substr(0, 19)`）。教训：时间格式错误往往在**对面**才暴露，且症状与"签名错"无法区分 |
| **P5-D03** | **mock/参照实现的验签把 `X-Amz-Signature` 也算进了被签内容** → 所有预签名请求都被拒，且原因显示为 `SignatureDoesNotMatch`（看起来像 C++ 侧签错）。根因是 `verify()` 里"剔除签名参数"只做了一半：`creq` 剔了，交给 `sign()` 重算时又传了**全量 query** | `verify()` 统一用剔除后的 `signing_query`；并在注释里写明"签名本身不参与被签内容"。教训：**验证器的缺陷会伪装成被测实现的缺陷** —— 所以对拍失败时先怀疑验证器，再用"最小用例逐项消元"定位 |
| **P5-D04** | **continuation token 的语义错在 mock 侧**：用"上一页最后一个 key"当 token，导致下一页把该 key 再返回一次（多一页 + 重复）。契约的"不重不漏"用例一眼看出 | mock 改成**不透明** token（索引），与 S3 语义一致；客户端本来就只回传 token，不需要改 |
| **P5-D05** | **成功的 `list` 响应被 64 KiB 采集上限静默截断**（1005 个键只解析出 425 个）：那个上限是给**错误体**做映射用的。截断后的 XML 仍是合法前缀，解析不报错，只表现为"分页数字不对" | 采集上限按用途分开：错误体 64 KiB；XML 响应 8 MiB；并且**采满即报错**（不再静默丢后半段） |
| **P5-D06** | `copy` 返回的 `ObjectStat.size == 0`；`get` 的不可满足区间（`offset>=size`）被报成 `kInternal` | `copy` 完成后补一次 HEAD 拿完整 `ObjectStat`（S3 的 `<CopyObjectResult>` 不含 size）；416 → `kInvalidArgument`（契约：不可满足区间属于参数问题） |
| **P5-D08** | **两处与 AWS 规范不一致，只有官方向量能抓到**：① 同名头写成"后值覆盖前值"（规范要求逗号连接）；② 预签名的 payload 哈希硬编码 `UNSIGNED-PAYLOAD`（非 S3 服务要签真实体哈希） | 都按 AWS Go SDK 的期望值修正；两条向量从此逐字节匹配（见 §1e） |
| **P5-D09** | **virtual-host 形态下数据面路径带上了桶名**：`put/get/stat/remove/copy/list/ensure_container` 都硬编码 `"/" + container + "/" + key` → 变成 `/bucket/bucket/key` | 抽 `ObjectPath()`/`BucketPath()`，由 `force_path_style` 决定路径形态；补 virtual-host 数据面用例 |
| **P5-D07** | 新增 `kStorageAccessDenied` 后 **C2.2 的覆盖率矩阵立刻出现空缺**：`test_error_kind_coverage` 要求每个 `ErrorKind` 都有触发用例，而内存适配器**永远不会**产生"存储侧拒绝了本服务" | 在矩阵里用"注入了错误的端口替身 + **真实的用例路径**"（`LocationIssuer` → `store.presign_get`）触发它，并断言应用层把该 kind **原样透出**（不折叠成 `kInternal`）；契约 §5 表同步新增一行，`test_http_error_mapper` 的显式清单 12 → 13 |
| P5-M03 | 带体 PUT 时 curl 用 `sha256(body)` 作 payload 哈希，且**不**发 `x-amz-content-sha256` 头；GET 用空串哈希 | 记录为切片 2 数据面的设计输入：预签名 PUT/GET 用 `UNSIGNED-PAYLOAD`（URL 无法预知体），服务端代理请求按实际体哈希 |

## 5. C5.1 的"官方文档向量"如何补齐（留给下一切片）

拿到任一来源的官方向量后，只需在 `tests/unit/test_sigv4_signer.cpp` 增加一个 `SECTION`：
把 `method/host/path/query/headers/payload/date` 与**期望签名**成对写入（来源注明 URL + 抓取日期），
断言 `result.signature == 期望值` 与 `result.canonical_request` 逐字节相等。
在此之前该条**如实标注为未验证**，不用"两条独立实现对拍"冒充"官方向量"。

---

## 6. 结论

| 项 | 结论 |
| --- | --- |
| 切片 1 门槛（SigV4 签名器 + 编码 + 独立对拍） | ✅ `test_sigv4_signer` + `test_sigv4_crosscheck`；`ctest -L phase5` 合计 **2 测试 / 209 断言** |
| 已满足判据 | **C5.5（编码部分）**；C5.1 部分（独立对拍口径） |
| 未验证项 | **C5.4** path-style 与 virtual-host | ✅ | path-style：预签名 + 数据面 + S3 模式端到端全程；virtual-host：`test_upload_flow_s3` 用 `curl --connect-to` 让 mock 收到带桶前缀的 `Host`，PUT/GET 都与 path-style 指向同一份存储（并修掉 P5-D09 的路径拼接缺陷） |
| C5.1 的 AWS 官方文档向量（本机无来源，见 §5）；C5.2~C5.4、C5.6~C5.10（待切片 2+） |
| P5 是否收口 | ✅ **收口**：C5.1~C5.10 满足；`check_docs.sh` + `run_all_gates.sh` 全绿（含 ASan/UBSan/LSan 与两处护栏自证） |
