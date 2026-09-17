# 附录：协程 / 异步 I/O 探针（证据归档）

本目录是 **`docs/adr/ADR-007-async-and-coroutines.md`** 的证据来源。
回答的问题是："使用协程是否能够提高并发？" —— 结论是"**取决于等待的是什么**"，本目录给出实测。

## 探针清单

| 文件 | 用途 | 结论 |
| --- | --- | --- |
| `minimal_coro.cpp` | GCC 11 的 C++20 协程基础能力 | ✅ 可用 |
| `stdcoro.cpp` | 标准库协程设施可用性 | `std::generator` ❌（需 GCC 14）；`__cpp_lib_execution` 是**并行算法**不是 P2300，易误读 |
| `asio_coro.cpp` | Boost.Asio 1.74 + C++20 协程（`awaitable`/`co_spawn`） | ✅ 编译并实际跑通（自测请求返回 200） |
| `srv_http.cpp` | 线程模型：httplib 线程池，每请求 5ms 阻塞 | 并发上限 = pool 大小；pool=2048 时 2,049 线程 / 29.3 MB RSS / 127k req/s / 实测并发仅 1,273 |
| `srv_coro.cpp` | 协程模型：Asio，每请求 5ms **异步定时器** | **4 线程 230k req/s、16 线程 254k req/s**，RSS 3.1–3.3 MB |
| `srv_modes.cpp` | ★ **决定性实验**：同一协程服务端，只改"如何等待" | 阻塞 763 req/s / offload 758 req/s / **真异步 40,246 req/s（52x）** |
| `srv_coro_io.cpp` | 协程内阻塞 `pread` vs offload 到线程池 | 测试被页缓存带宽饱和，**无法区分**（保留以记录该尝试） |
| `lg.cpp` / `lg2.cpp` / `lg_io.cpp` | 独立进程负载生成器（按 `Content-Length` 分帧） | 方法学要求：**必须独立进程 + 绑核** |

## 核心数据（本机 16 核，2048 并发 keep-alive）

```
每请求 5ms 等待：
  线程模型  pool=64    →  12,679 req/s,   65 线程, RSS  4.7 MB, 并发 64
  线程模型  pool=512   →  79,970 req/s,  513 线程, RSS 10.3 MB, 并发 512
  线程模型  pool=2048  → 127,026 req/s, 2049 线程, RSS 29.3 MB, 并发 1273(未达 2048, err=96)
  协程 Asio 4 io 线程  → 230,437 req/s,    4 线程, RSS  3.1 MB, 并发 2048
  协程 Asio 16 io 线程 → 254,549 req/s,   16 线程, RSS  3.3 MB, 并发 2048

每请求 50ms 等待，同一协程服务端（4 io 线程）：
  mode 0 协程内阻塞调用           →    763 req/s
  mode 1 协程内 offload 到 64 线程池 →    758 req/s
  mode 2 协程内真异步等待          → 40,246 req/s   （= 2048 ÷ 0.05，理论满并发）
```

**结论**：协程的收益来自"等待时让出执行权"。协程里调用阻塞 API 不会让出，并发度退化为 io 线程数；
offload 到线程池只是把上限换成池大小，等价于线程模型。

## 复现

```bash
cd /tmp && mkdir -p coro_probe && cd coro_probe
# 1) 拿 Boost 头文件（无需 root）
apt-get download libboost1.74-dev && dpkg-deb -x libboost1.74-dev_*.deb ext/
B=/tmp/boostx/ext/usr/include          # 或按实际解包路径

# 2) 编译
g++ -std=c++20 -O2 minimal_coro.cpp -o minimal_coro && ./minimal_coro
g++ -std=c++20 -O2 stdcoro.cpp      -o stdcoro      && ./stdcoro
g++ -std=c++20 -O2 -I$B asio_coro.cpp  -o asio_coro  -lpthread && ./asio_coro
g++ -std=c++20 -O2 -I/path/to/fssvrcpp/third_party srv_http.cpp -o srv_http -lpthread
g++ -std=c++20 -O2 -I$B srv_coro.cpp  -o srv_coro  -lpthread
g++ -std=c++20 -O2 -I$B srv_modes.cpp -o srv_modes -lpthread
g++ -std=c++20 -O2 lg2.cpp -o lg2 -lpthread

# 3) 线程 vs 协程（独立客户端进程，绑 4 核）
./srv_http 2048 19101 &   sleep 1.5; taskset -c 0-3 ./lg 19101 2048 3; kill %1
./srv_coro 16   19102 &   sleep 1.5; taskset -c 0-3 ./lg 19102 2048 3; kill %1

# 4) 决定性实验（三种等待方式，50ms）
./srv_modes 4 19301 0 0   & sleep 1.5; taskset -c 0-3 ./lg2 19301 2048 3; kill %1
./srv_modes 4 19302 1 64  & sleep 1.5; taskset -c 0-3 ./lg2 19302 2048 3; kill %1
./srv_modes 4 19303 2 0   & sleep 1.5; taskset -c 0-3 ./lg2 19303 2048 3; kill %1
```

## ⚠️ 方法学与局限（务必先读）

1. **必须用独立进程做负载生成**，并绑定不同核。进程内客户端会与服务端争抢同一批核，
   曾导致"并发越高越慢"的**方向性错误结论**（见 `../capacity-probe/README.md`）。
2. **本目录的"异步等待"用定时器实现**，这是协程的**最好情况**。
   真实的 `pread`/`sendfile`/SQLite 都是**阻塞**的，除非引入 io_uring 或 offload，
   否则拿不到 mode 2 的收益。**不要把 254k req/s 当作本服务可达到的数字。**
3. 客户端与服务端同机（loopback），**绝对数字只能作量级参考**；相对对比（A/B）可信。
4. 50 ms 场景下 mode 0/1 的绝对值（~760 req/s）与"io线程数 ÷ 0.05 = 80"不完全吻合，
   可能与连接建立速率和客户端行为有关；**该组数据只用于证明 mode 2 相对 mode 0/1 的 52x 量级差距**，
   不用于容量推算。
5. `srv_coro_io.cpp` 的实验被**页缓存带宽饱和**（192 请求 × 8 MiB / 0.13 s ≈ 12 GB/s），
   三种模式无差异 → 该实验**无法区分**模型优劣，已如实保留。
6. io_uring 在本机为 **WSL2 内核**，其可用性**不能代表真实部署环境**（ADR-007 §5）。
