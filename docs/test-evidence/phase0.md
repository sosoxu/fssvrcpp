# 阶段 0 测试证据 —— 环境与契约可行性

| 项 | 值 |
| --- | --- |
| 阶段 | P0（环境与契约可行性） |
| 日期 | 2025（本次会话） |
| 结论 | ✅ **通过** |
| 门槛命令 | `cmake -S . -B build && cmake --build build -j8 && ctest --test-dir build -L phase0 --output-on-failure` |
| 退出码 | `0` |
| 用例数 / 断言数 | **7 test cases / 55 assertions** |

---

## 1. 门槛命令与输出

```console
$ cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
-- Found OpenSSL: /usr/lib/x86_64-linux-gnu/libcrypto.so (found version "3.0.2")
-- Found SQLite3: /usr/include (found version "3.37.2")
-- Checking for module 'libcurl'      -- Found libcurl, version 7.81.0
-- Checking for module 'grpc++'       -- Found grpc++, version 1.30.2
-- Checking for module 'protobuf'     -- Found protobuf, version 3.12.4
-- ----------------------------------------------------------
--  fssvrcpp 0.1.0 (RelWithDebInfo)
--    C++ standard : 20
--    compiler     : GNU 11.4.0
--    grpc         : 1.30.2  (enabled=ON)
--    protobuf     : 3.12.4
--    openssl      : 3.0.2
--    sqlite3      : 3.37.2
--    tests        : ON
-- ----------------------------------------------------------
-- Configuring done / Generating done

$ cmake --build build -j8
[ 16%] protoc: 生成 gRPC/protobuf C++ 代码 (proto/osdu/file/v1/file_service.proto)
[ 50%] Linking CXX static library lib/libfss_proto.a
[100%] Built target test_phase0_toolchain

$ ctest --test-dir build -L phase0 --output-on-failure
    Start 1: test_phase0_toolchain
1/1 Test #1: test_phase0_toolchain ............   Passed    0.02 sec

100% tests passed, 0 tests failed out of 1
```

逐用例明细（`./build/bin/test_phase0_toolchain -s` 摘要）：

```console
Q1.1 C++20 语言特性可用
Q1.2 nlohmann/json 能精确处理 OSDU 的 PascalCase 字段
Q1.3 OpenSSL 提供 HMAC-SHA256（S3 SigV4 预签名的基础）
Q1.4 SQLite3 可持久化文件位置记录
Q1.5 libcurl 可完成真实 HTTP 往返（出站客户端基线）
Q2   proto 可生成、可编译、可在回环上完成 gRPC 调用
Q3   同一份 OSDU JSON 经 REST 语义与 RPC 语义两条链路产出等价领域结果

===============================================================================
All tests passed (55 assertions in 7 test cases)
```

---

## 2. 对 P0 目标的逐条回答

| 目标 | 问题 | 结论 | 证据 |
| --- | --- | --- | --- |
| Q1 | 外部依赖齐全吗？ | ✅ | Q1.1–Q1.5 |
| Q2 | RPC 通路通吗？ | ✅ | Q2（真实 gRPC 服务 + 回环客户端，4 个 SECTION） |
| Q3 | 双协议语义同构可行吗？ | ✅ | Q3（含真实 gRPC 序列化往返） |
| — | 依赖选型是否需要调整？ | ✅ 已调整 | 否决 cpp-httplib → ADR-002 |

### Q1 细节

| 用例 | 验证内容 | 关键断言 |
| --- | --- | --- |
| Q1.1 | C++20 | 指定初始化器 + `constexpr` + `requires` 表达式 + `__cplusplus >= 202002L` |
| Q1.2 | JSON 与 OSDU 契约 | PascalCase（`Name`/`FileSource`/`FileSize`）可读；**camelCase（`name`/`fileSourceInfo`）访问必须抛异常**；`dump()`→`parse()` 往返等价 |
| Q1.3 | HMAC-SHA256 链式派生 | 4 个 AWS SigV4 官方派生向量逐字节匹配；断言中间摘要为 **32 字节原始二进制**（不是 64 字符 hex） |
| Q1.4 | SQLite3 | DDL + 参数化 INSERT + SELECT，读出值与写入值一致 |
| Q1.5 | libcurl | 对自建回环 TCP 服务发起真实 HTTP GET，返回 200 且 JSON 可解析 |

### Q2 细节

真实启动 `grpc::Server`（`127.0.0.1:0` 由内核分配端口），用 `FileService::NewStub` 调用：

| SECTION | 断言 |
| --- | --- |
| `GetUploadLocation` | `file_id` 非空；`location.file_source == "/osdu-user/phase0/" + file_id`；`location.signed_url` 以 `http://` 开头；`driver == STORAGE_DRIVER_POSIX`；`zone == STORAGE_ZONE_STAGING` |
| 传入 `fileID` | 原样保留（与 REST `/v2/getLocation` 语义一致） |
| 错误语义 | 空 id → `INVALID_ARGUMENT`（对应 REST 400） |
| 调用元数据 | `data-partition-id` / `correlation-id` 可被服务端读取（P8 鉴权链路的前提） |

### Q3 细节（本阶段**最关键**的验证）

```
链路 A（REST 语义）： OSDU JSON ──直接投影──▶ DomainProjection
链路 B（RPC 语义） ： OSDU JSON ──▶ proto 消息 ──(真实 gRPC 序列化往返)──▶ DomainProjection

断言：A == B（id / kind / data.Name / FileSource / FileSize / acl.viewers / legal.legaltags 全部相等）
```

- 链路 B 走的是**真实 gRPC 服务与序列化**，不是在进程内直接构造 proto 对象（避免"假验证"）。
- 该断言成立 ⇒ **"一套应用层 + 两个薄适配器"架构可行**，proto 契约与 OSDU REST 契约无语义漂移。
- 若该断言失败，说明必须先修契约、再写业务代码——它把架构风险前移到了第 0 阶段。

---

## 3. 本阶段发现并修复的缺陷（门槛的价值证明）

### D-01：HMAC 链式派生误用十六进制字符串作为 key

**发现方式**：Q1.3 的断言直接失败（`kRegion` 不匹配 AWS 官方向量）。

```console
/home/ll/fssvrcpp/tests/integration/test_phase0_toolchain.cpp:390: FAILED:
  REQUIRE( k_region == "f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d" )
with expansion:
  "a59e30f9d899c47b3dd68ea1c0ab3bb529e03a8f4ed2f54cb64af547330a22a0" == "f4780e..."
```

**根因**：原实现用"上一轮结果的**十六进制字符串**"作为下一轮的 HMAC key。
SigV4 要求用**原始摘要字节**作为 key。此外初版把断言错挂在 `kRegion` 上，
实际 `f4780e...` 是 `kSigning` 的值。

**修复**：改为以原始 32 字节摘要链式派生，并把 4 个中间值全部固定为回归断言：

```
kDate    = 969fbb94feb542b71ede6f87fe4d5fa29c789342b0f407474670f0c2489e0a0d
kRegion  = 69daa0209cd9c5ff5c8ced464a696fd4252e981430b10e3d3fd8e2f197d7a70c
kService = f72cfd46f26bc4643f06a11eabb6c0ba18780c19a8da0c31ace671265e3c87fa
kSigning = f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d
```

**影响**：这是一个**在阶段 5 才会暴露**的缺陷（S3 预签名）。若没有 P0 的这道门槛，
它会在写出整个 S3 驱动后才以"签名不匹配"的模糊症状浮现。
⇒ 直接支撑了"每步先测"的流程价值。

### D-02：protoc 输出路径计算错误

**发现方式**：首次构建失败——`fatal error: .../build/fss_gen/file_service.pb.cc: No such file or directory`。

**根因**：`ProtoGen.cmake` 未考虑 protoc 会按"相对于 `-I` 目录的路径"在输出目录下建子目录
（`-I proto` + `proto/osdu/file/v1/...` ⇒ 输出 `<gen>/osdu/file/v1/file_service.pb.cc`）。

**修复**：在 `fss_add_proto_library` 中显式计算"相对某个 import dir 的路径"，
若 proto 不在任何 `IMPORT_DIRS` 之下则 `FATAL_ERROR`（快速失败）。

### D-03：Ubuntu 的 `libcpp-httplib-dev` **二进制包**不可用（结论仅限该分发形态）

**发现方式**：实测 Ubuntu 的 `libcpp-httplib-dev` 只提供**声明头**（1851 行），
实现位于单独的 `libcpp-httplib0` 预编译 `.so`，且该 `.so` 以
`CPPHTTPLIB_OPENSSL_SUPPORT`/`ZLIB`/`BROTLI` 编译
（`nm -D` 显示 53 个 `SSL_*`/`EVP_*`、6 个 `inflate/deflate`、7 个 `Brotli*` 未定义符号）。

**结论（限定范围）**：使用方必须**精确复刻打包方的宏集合**才能保证类布局一致，否则是未定义行为；
而复刻宏又需要本机没有的 `libbrotli-dev` 头文件。因此**该二进制包**存在**不可接受的 ABI 耦合**。

> ⚠️ **这个结论当时被过度外推了**：初版据此得出"应当自研 HTTP 内核"，
> 属于推理跳跃——正确结论只是"不要用这个二进制包"（换源码分发形态即可）。
> 修订过程见 **D-05**。现行决策：使用**上游源码单头文件**（无 ABI 耦合）
> + `fss_http` 强化包装层。

**仍然有效的结论**：出站 HTTP 统一用 libcurl。

### D-04：C++20 `constexpr std::string` 在 g++ 11 不可用

**发现方式**：`constexpr Config{.port=8080, .root="/var/lib/fss"}` 编译失败
（`Config` 不是字面类型，因为 `std::string` 不是）。

**修复**：测试中改用 `const char*`（`constexpr std::string` 需要完整的 constexpr 析构支持）。

---

### D-05：ADR-002 的"完全自研 HTTP 内核"结论被推翻（**决策级缺陷**）

**发现方式**：评审时被质疑"为何自研而不用现成开源模块"。复核后确认**初版结论错误**，已重写 ADR-002。

**错误链条**：

| 步骤 | 初版推理 | 复核结果 |
| --- | --- | --- |
| 1 | Ubuntu 的 `libcpp-httplib-dev` 只有声明头，实现是预编译 `.so`，且以 `SSL/ZLIB/BROTLI` 宏编译 → 使用方需精确复刻宏，否则类布局不一致（UB） | ✅ 事实正确（`nm -D` 复核：53 个 `SSL_*`/`EVP_*`、7 个 `Brotli*` 未定义符号；头文件 1851 行仅声明） |
| 2 | 离线环境拿不到上游单头文件版本 | ❌ **错**：Ubuntu 镜像 pool 可浏览，`pool/universe/c/cpp-httplib/` 下存在上游 **0.26.0** 单头文件源码（11900 行 / 405 KB） |
| 3 | 因此应当自研 HTTP 内核 | ❌ **推理跳跃**：从"某个二进制包不可用"直接推出"自写整个 HTTP 栈"，跳过了"换分发形态/版本"，且只评估了 1 个候选 |

**同时补充的实测（这才是"不能裸用、但也不必自研"的真正理由）**：

| ID | 版本 | 缺陷 | 复现输出 |
| --- | --- | --- | --- |
| H-1 | 0.10.3 | 流式响应 + 越界 `Range` 无 416 分支，`size_t` 下溢 | `HTTP/1.1 206` + `Content-Length: 18446744072717940225` + `Content-Range: bytes 999999999-8388607/8388608` |
| H-2 | 0.26.0 | 流式请求体 + `set_payload_max_length` 超限：handler 仍被调用、reader 交出 **0 字节**、返回 **201** | `/reader 2MiB vs limit 1MiB -> HTTP/1.1 201 Created \| reader_bytes=0`（对照：缓冲 handler 同一输入 → `413`） |

H-2 是**静默数据丢失**（上传"成功"但对象为空），且恰好落在本项目最关键的数据面路径上。

**修订后的决策**（ADR-002 修订版）：

- 使用上游 cpp-httplib **0.26.0 源码单头文件**（MIT，vendored 到 `third_party/httplib.h`，
  `sha256 = 4a42da28ce477d06f53942c172197d3b83b4fe42b7e37b8af78c70e063c06da7`）作传输层；
- 在 `src/common/http/` 建 `fss_http` **强化包装层**（本项目自有代码），承担硬上限、Range 归一化、
  中间件链、错误归一化，以及 **H-2 三重防护**（按 `Content-Length` 前置拒绝 → 413；
  自封装计数 reader；读取字节数与 `Content-Length` 一致性断言）；
- **不使用** Ubuntu 的二进制包（ABI 耦合 + H-1）；
- 只有 `src/common/http/` 允许 `#include <httplib.h>`（分层护栏强制，保证库可替换）；
- H-1/H-2 固化为回归测试，纳入**阶段 1 门槛**（`test_httplib_hardening.cpp`）。

**证据归档**：`docs/appendix/httplib-hardening-probe/`（6 个探针源码 + 原始输出）。
**契约固化**：`docs/03-api-contract.md` §1.7（HTTP 层硬上限与拒绝语义，
含"chunked 超限 → 400 而非 413"、"方法不匹配 → 404 而非 405"两条实测行为）。

**门槛的价值**：这是一次**决策级**错误，靠评审而非测试发现。但复核过程本身产出了
两个可复现的上游缺陷与其防护方案——若按初版结论直接自研，这两个缺陷会以
"我们自己实现的同类边界 bug"的形式重新出现。

## 4. 环境事实（实测，已写入实现计划 §5）

| 项 | 值 | 影响 |
| --- | --- | --- |
| OS / 编译器 | Ubuntu 22.04.5 / g++ 11.4.0 | C++20 可用；`clang++`/`ninja` 未安装 |
| CMake / Make | 3.22.1 / GNU Make 4.3 | 不使用需要更新 CMake 的特性 |
| root 权限 | ❌（`sudo` 需密码） | 依赖一律 `apt-get download` + `dpkg-deb -x` |
| 外网 | `github.com` 直连不可达；`community.opengroup.org` 可达；apt 镜像（USTC）可用 | OSDU 源码可直接获取；第三方库只能走 apt 解包 |
| gRPC / protobuf | 1.30.2 / 3.12.4 | ★ **proto3 `optional` 不可用**（需 3.15+）→ 已作为硬约束写入 proto 与文档 |
| OpenSSL | 3.0.2 | HMAC/SHA 可用（S3 SigV4 无需 AWS SDK） |
| SQLite3 / libcurl | 3.37.2 / 7.81 | 可用；libcurl 头文件在 `/usr/include/x86_64-linux-gnu/curl` |
| 缺失 | boost、aws-sdk-cpp、nlohmann-json（已 vendored）、gtest（改用 Catch2） | 设计上不依赖这些 |

---

## 5. 阶段 0 交付物清单

| 路径 | 说明 |
| --- | --- |
| `CMakeLists.txt` | 顶层构建；特性开关先于依赖探测 |
| `cmake/Dependencies.cmake` | 依赖探测 + 缺失时的可执行修复指令 |
| `cmake/ProtoGen.cmake` | `protoc` + `grpc_cpp_plugin` 封装（含 import dir 路径推导与快速失败） |
| `proto/osdu/file/v1/file_service.proto` | RPC 契约 v0.2（已按实测线上契约修正：`Location.SignedURL`/`Location.FileSource`、Spring Page 的 `FileListResponse`、DMS 模型、Delivery 路径） |
| `tests/CMakeLists.txt` | `fss_add_test` 辅助函数 + phase0 目标注册 |
| `tests/integration/test_phase0_toolchain.cpp` | 7 个测试用例 / 55 断言 |
| `third_party/nlohmann/json.hpp` | JSON 库（header-only，apt 解包获得） |
| `third_party/catch2/catch.hpp` | 测试框架（header-only，apt 解包获得） |
| `docs/01-osdu-research.md` | OSDU 一手调研报告 |
| `docs/02-design.md` | 架构设计文档 |
| `docs/03-api-contract.md` | 接口契约（实现与测试的唯一基准） |
| `docs/04-implementation-plan.md` | 10 阶段实现计划与测试门槛 |
| `docs/adr/ADR-001..003.md` | 三项关键决策记录 |
| `docs/appendix/osdu-file-service-source-notes.md` | 上游源码逐文件调研原始笔记（含 ~60 个已验证 URL） |
| `docs/test-evidence/phase0.md` | 本文件 |

---

## 6. 结论与下一步

| 项 | 结论 |
| --- | --- |
| 阶段 0 门槛 | ✅ **通过**（7 cases / 55 assertions / exit 0） |
| 可进入阶段 1？ | ✅ **可以** |
| 阶段 1 前置条件是否满足？ | ✅ 全部依赖已验证可链接可运行；proto 代码生成链路已验证；分层目标图的构建基础已就绪；**HTTP 传输层依赖（cpp-httplib 0.26.0）已 vendored 并通过 5 组探针实测** |
| 已知必须带入阶段 1 的约束 | ① protobuf 3.12 不支持 proto3 `optional`；② 无 root 权限，不得引入需安装的依赖；③ 出站 HTTP 用 libcurl；④ HTTP 传输层用 cpp-httplib 0.26.0 + `fss_http` 包装层（ADR-002 **修订版**），且必须兜住 H-1/H-2 两个已复现缺陷 |

**下一步动作**（详见 `docs/04-implementation-plan.md` §9）：
建立完整 CMake 目标图 + 分层护栏脚本（`scripts/verify_guard.sh`）并证明护栏**能失败**，
然后实现 `common/result` → `common/crypto` → 其余通用库 → `common/http`，
最后以 `ctest -L phase1` 满足 C1.1–C1.7 并归档 `docs/test-evidence/phase1.md`。
