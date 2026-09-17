# third_party —— 随仓库分发的第三方依赖

本目录只放**源码形式**的第三方依赖（header-only），不放二进制产物。
设计理由见 `docs/adr/ADR-002-http-framework.md`：**避免任何预编译 `.so` 带来的 ABI/宏耦合**。

## 清单

| 依赖 | 版本 | 用途 | 许可 | 文件 |
| --- | --- | --- | --- | --- |
| nlohmann/json | 3.10.5 | JSON 解析/生成 | MIT | `nlohmann/json.hpp` |
| Catch2 | 2.13.8 | 单元/集成测试框架（v2 单头） | BSL-1.0 | `catch2/catch.hpp` |
| cpp-httplib | **0.26.0** | HTTP/1.1 传输（**仅作传输层**，见下） | MIT | `httplib.h` |

许可见 `licenses/`。

## 校验和（用于审计与"版本漂移"检测）

```
4a42da28ce477d06f53942c172197d3b83b4fe42b7e37b8af78c70e063c06da7  httplib.h          (cpp-httplib 0.26.0)
```

CI 中应校验该值：`sha256sum -c third_party/CHECKSUMS.txt`。
校验和变化意味着依赖被更新或篡改，**必须**同时在 `docs/adr/ADR-002-http-framework.md`
的"已知缺陷与回归测试"一节复核并重跑 `ctest -L phase1`。

## 获取方式（本机无 root、直连 github 不可达）

所有依赖均通过 apt 镜像获取，**不需要 root**：

```bash
# header-only 库：用 apt 的 -dev 包解包即可
cd /tmp && apt-get download nlohmann-json3-dev catch2
for f in *.deb; do dpkg-deb -x "$f" ext/; done
cp -r ext/usr/include/nlohmann      third_party/
mkdir -p third_party/catch2 && cp ext/usr/include/catch2/catch.hpp third_party/catch2/

# cpp-httplib：不要用 Ubuntu 的二进制包（见 ADR-002），改用镜像 pool 里的上游源码
#   注意 Ubuntu jammy 只提供 0.10.3 的 *二进制* 包，且该版本有 Range 下溢缺陷；
#   镜像 pool 里同时存在上游 0.26.0 源码，可直接取单头文件。
curl -O https://mirrors.ustc.edu.cn/ubuntu/pool/universe/c/cpp-httplib/cpp-httplib_0.26.0%2Bds.orig.tar.xz
tar xJf 'cpp-httplib_0.26.0+ds.orig.tar.xz'
cp cpp-httplib-0.26.0+ds/httplib.h  third_party/httplib.h
cp cpp-httplib-0.26.0+ds/LICENSE    third_party/licenses/cpp-httplib-LICENSE
```

## ⚠️ cpp-httplib 的定位：**只是传输层，不是可信的边界**

本项目把 cpp-httplib 当作**不可信组件**使用：它的职责仅限于 TCP 监听、HTTP/1.1 解析、
路由分发与响应写回。**所有"安全与正确性边界"由我们自己的 `fss_http` 包装层承担**
（`src/common/http/`），原因是实测发现它在本项目需要的两条路径上存在缺陷：

| # | 版本 | 缺陷（已实测复现） | 后果 | 我们的处置 |
| --- | --- | --- | --- | --- |
| H-1 | 0.10.3 | `set_content_provider`（流式响应）+ 越界 `Range` **无 416 分支**；`Content-Length` 发生 `size_t` 下溢 | 响应声明 `Content-Length: 18446744072717940225`、`Content-Range: bytes 999999999-8388607/8388608`（非法）；客户端按该长度等待 → 挂死/错帧 | **不使用 0.10.3**；升到 0.26.0（已修复，实测 416 正确） |
| H-2 | 0.26.0 | `ContentReader`（流式请求体）+ `set_payload_max_length` 超限时：**返回 201 且交给 handler 0 字节** | **静默数据丢失**：上传"成功"、对象为空 | `fss_http` 包装层：① `set_pre_routing_handler` 按 `Content-Length` 前置拒绝（413）；② 自封装计数 reader，超限立即中止；③ 读取字节数与 `Content-Length` 一致性断言 |

复现过程与原始输出见 `docs/appendix/httplib-hardening-probe/`；
对应的回归测试是阶段 1 门槛的一部分（`tests/integration/test_httplib_hardening.cpp`）。

**升级 `httplib.h` 时必须做的事**：

1. 更新 `third_party/CHECKSUMS.txt`；
2. 重跑 `ctest -L phase1`（其中 H-1/H-2 的回归测试会立刻暴露行为变化）；
3. 若上游已修复 H-2，**不要急着删掉我们的包装层**——它是"不依赖上游行为"的防线，
   只有在确认上游修复且回归测试覆盖之后，才可考虑简化（并在 ADR-002 中记录）。

## 为什么不用包管理器

| 方案 | 为何不用 |
| --- | --- |
| conan / vcpkg | 本机无 root、外网受限；且需要网络拉取，破坏"离线可构建" |
| Ubuntu 的 `-dev` 二进制包（如 `libcpp-httplib-dev`） | 只提供声明头 + 预编译 `.so`；`.so` 以 `CPPHTTPLIB_OPENSSL|ZLIB|BROTLI_SUPPORT` 编译，要求使用方宏完全一致，否则类布局不一致（UB）；且 jammy 的版本有 H-1 缺陷 |
| Boost.Beast | header-only 但属 Asio 低层 API，需要自行实现路由/Range/keep-alive 等，工作量与自研相当；Boost 体积大、编译慢 |
| GNU libmicrohttpd | C 回调 API，同样面临"预编译 `.so` + 宏耦合"的 vendoring 问题，且 Range/路由需自行实现 |
