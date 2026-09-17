# 附录：cpp-httplib 强化探针（证据归档）

本目录保存的是**决策复核过程**的原始探针源码。它们不是在评审"要不要用 httplib"时凭印象判断，
而是实际编译运行、观察线上字节得到的证据。目的是：**任何人可以重跑并得到同样的结论**。

结论与其在产品代码中的落点见：
- `docs/adr/ADR-002-http-framework.md`（修订版决策）
- `docs/02-design.md` §7.1
- `docs/03-api-contract.md` §1.7（HTTP 层硬上限与拒绝语义）
- `docs/test-evidence/phase0.md` D-03 / D-05

## 探针清单与结果

| 文件 | 目的 | 结论 |
| --- | --- | --- |
| `probe.cpp` | 8 MiB 流式 PUT（SHA256 比对）、`Range: bytes=100-199` / `bytes=-50` / 越界、全量 GET、50 并发、重复 `Content-Length` | **0.10.3 失败**（越界 Range）；**0.26.0 全部通过** |
| `raw_range.cpp` | 用原始 socket 直接观察越界 Range 的**线上响应字节**（避免客户端库掩盖问题） | **0.10.3**：`206` + `Content-Length: 18446744072717940225` + `Content-Range: bytes 999999999-8388607/8388608`（`size_t` 下溢，协议非法）<br>**0.26.0**：`416` + `Content-Length: 0` ✅ |
| `probe2.cpp` | chunked PUT、`HEAD`、64 KiB 请求头、未知方法/路径、keep-alive 复用 | 0.26.0：chunked 201、HEAD 无体、64 KiB 头 → 400、未知 → 404、keep-alive 两次 206 |
| `probe413.cpp` | 超限请求体：**缓冲 handler** vs **流式 `ContentReader` handler** | ⚠️ **0.26.0 缺陷 H-2**：缓冲路径 → `413`；**流式路径 → `201` 且 `reader_bytes=0`**（静默数据丢失） |
| `probe_fix.cpp` | 验证包装层的修法（`set_pre_routing_handler` 前置拦截 + 计数 reader + 长度一致性断言） | `Content-Length` 超限 → **413** ✅；chunked 超限 → 400（库强制）；正常 5B → 201 ✅ |
| `probe_fix2.cpp` | 尝试在 callback 内设置 413 以覆盖 chunked 路径 | ❌ 无效：库在读取失败后强制 400。**故契约按实测定义为"chunked 超限 → 400"** |

## 复现方式

```bash
# 取两个版本的**上游源码**（注意：不是 Ubuntu 的二进制包）
cd /tmp && mkdir -p httplib_src && cd httplib_src
for v in "0.10.3%2Bds" "0.26.0%2Bds"; do
  curl -O "https://mirrors.ustc.edu.cn/ubuntu/pool/universe/c/cpp-httplib/cpp-httplib_${v}.orig.tar.xz"
done
for t in *.tar.xz; do d="x_$(basename "$t" .orig.tar.xz)"; mkdir -p "$d" && tar xJf "$t" -C "$d"; done

# 编译并运行（把 <D> 换成解包出的目录）
g++ -std=c++20 -O2 -I<D>/0.26.0  probe.cpp     -o probe     -lssl -lcrypto -lpthread && ./probe
g++ -std=c++20 -O2 -I<D>/0.26.0  raw_range.cpp -o raw_range -lpthread            && ./raw_range
g++ -std=c++20 -O2 -I<D>/0.26.0  probe413.cpp  -o probe413  -lpthread            && ./probe413
g++ -std=c++20 -O2 -I<D>/0.26.0  probe_fix.cpp -o probe_fix -lpthread            && ./probe_fix
```

## 探针本身踩过的坑（保留在源码里，供参考）

1. `probe.cpp` 初版崩溃（SIGSEGV）：`set_content_provider` 的回调在 handler 返回**之后**才执行，
   而初版用引用捕获了 handler 内的局部 `std::ifstream` → use-after-free。
   修正为 `shared_ptr<ifstream>` 按值捕获。
2. 初版崩溃（`terminate called without an active exception`）：提前 `return` 时 `std::thread` 仍 joinable。
   修正为统一退出路径先 `stop()` 再 `join()`。
3. `probe_fix.cpp` 中使用 `res.req` 编译失败：httplib 的 `Response` 没有 `req` 成员，
   必须使用 handler 形参里的 `Request`。
