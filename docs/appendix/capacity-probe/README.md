# 附录：容量与并发探针（证据归档）

本目录是 `docs/05-capacity-and-concurrency.md` 的**证据来源**。
所有探针均可重跑复现，结论与原始输出见该文档。

## 探针清单

| 文件 | 回答的问题 | 关键结论 |
| --- | --- | --- |
| `bench_nodelay.cpp` | `TCP_NODELAY` 对 keep-alive 小请求的影响 | **950x**（23 → 21,062 req/s）；43.8 ms ≈ 40 ms delayed-ACK |
| `bench_pool.cpp` | 线程池大小是否等于并发上限 | 默认池 = `max(8, nproc-1)` = 15，**是硬上限**；keep-alive 连接occupies 一个线程 |
| `srv.cpp` + `loadgen.cpp` | 正确的负载测量方法 | **独立进程 + 绑核**；`max_inflight` 达到客户端并发数，吞吐持平 ~3.2–3.7 GiB/s |
| `bench_bigfile.cpp` | 大文件分段读的正确性与开销 | 150 GiB 偏移读取正确；1 MiB 段 1.5 ms **无读放大**；越界 416 |
| `bench_sendfile.cpp` | `sendfile` vs `pread+write` | **2.05x** 吞吐，每 GiB CPU 少 42% |
| `sqlite_bench.cpp` | 小文件写路径的 DB 固定成本 | WAL+NORMAL **25,471 tx/s**；`synchronous=FULL` **1,219 tx/s**（差 21x）；32 线程反而比 8 线程差 |
| `bench_inflight.cpp` | **反例保留**：进程内客户端导致错误结论 | 曾得出"T=32 比 T=4 慢 15 倍"的假象；根因是客户端与服务端争抢同一批核 |
| `bench_scale.cpp` | 同上（错误方法） | 保留以对照 |
| `bench_conc2.cpp` / `bench_alloc.cpp` | 早期探索：也测过"缓冲分配假设" | **假设被证伪**：`thread_local` 缓冲复用无影响 |

## ⚠️ 方法学教训（必须遵守）

1. **负载生成器必须是独立进程**，并与服务端**绑不同核**（`taskset`）。
   在同进程内用库客户端压测会得出**方向性错误**的结论——
   我们因此一度以为"并发越高越慢"，浪费了大量时间。
2. **HTTP 客户端也必须开 `TCP_NODELAY`**，否则客户端侧也会引入 40 ms 停顿。
3. **用稀疏文件不能代表真实磁盘性能**。稀疏区读的是内存页，
   本文所有吞吐数字**只是量级参考**，真实定稿必须在真实存储上复核（P9 C9.14）。
4. **不要一次只改一个变量就下结论**：`bench_alloc.cpp` 的"分配假设"就是被证伪的例子。
5. 保留 `bench_inflight.cpp` / `bench_scale.cpp` 这两个**错误方法**的探针，
   作为后续避免重犯的对照。

## 复现

```bash
cd /tmp && mkdir -p fss_bench && cd fss_bench
I=-I/path/to/fssvrcpp/third_party

# 1) TCP_NODELAY
g++ -std=c++20 -O2 $I bench_nodelay.cpp -o bench_nodelay -lpthread && ./bench_nodelay

# 2) 线程池
g++ -std=c++20 -O2 $I bench_pool.cpp -o bench_pool -lpthread && ./bench_pool

# 3) 独立进程负载（★ 推荐方法）
g++ -std=c++20 -O2 $I srv.cpp     -o srv     -lpthread
g++ -std=c++20 -O2     loadgen.cpp -o loadgen -lpthread
#   造一个 200 GiB 稀疏文件并在 150 GiB 处写标记
python3 - <<'PY'
import os
f=open('/tmp/fss_bench/big.img','wb'); f.truncate(200*1024**3); f.close()
fd=os.open('/tmp/fss_bench/big.img',os.O_WRONLY)
os.pwrite(fd,b'FSS-MARKER-AT-150GiB',150*1024**3); os.close(fd)
PY
./srv 512 /tmp/fss_bench/big.img 18082 &
sleep 1
for SEG in 4 16 64 256; do taskset -c 0-3 ./loadgen 18082 64 3 $SEG; done
curl -s http://127.0.0.1:18082/stat     # 看 max_inflight

# 4) 大文件正确性
g++ -std=c++20 -O2 $I bench_bigfile.cpp -o bench_bigfile -lpthread && ./bench_bigfile /tmp/fss_bench/big.img

# 5) sendfile 对比
g++ -std=c++20 -O2 bench_sendfile.cpp -o bench_sendfile -lpthread && ./bench_sendfile

# 6) SQLite
g++ -std=c++20 -O2 sqlite_bench.cpp -o sqlite_bench -lsqlite3 && ./sqlite_bench
```

## 探针自身的踩坑记录（保留供参考）

1. `bench_conc.cpp` 首版：`cd dir && cmd &` 把 `cd` 放进了子 shell，
   导致后续 `./loadgen` 找不到 → 用 `nohup ./srv ... &` 且**先 cd**。
2. `bench_bigfile.cpp` 首版：`set_content_provider` 的 `content_length` 传成了**区间长度**，
   等于让 httplib 在已算好的区间上再套一次 `Range` → 416 + `std::bad_alloc`。
   **正确用法是声明完整文件大小**，httplib 会把 `Range` 翻译成 provider 的 `(offset,length)`。
3. `probe*.cpp`（见 `../httplib-hardening-probe/`）：
   `set_content_provider` 回调在 handler 返回后才执行，**不能引用捕获 handler 的局部变量**。
4. 原始 socket 客户端必须处理 `keep_alive_max_count`（默认 100）后服务端主动断连的情况。
