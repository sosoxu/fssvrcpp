# =============================================================================
#  基准与负载生成器（P9 容量基线 / ADR-006 受控复核）
# =============================================================================
#  ★ 全部 EXCLUDE_FROM_ALL：日常构建与门槛**不**编译它们（基准不属于门槛的通过条件，
#    它们是"记录数字"的工具）；由 `scripts/bench_baseline.sh` 显式构建。
#
#  ★ 铁律 R2：客户端必须是**独立进程**，脚本用 `taskset` 把服务端与客户端绑到
#    不同的核上。这些二进制不参与任何 ctest 用例，因此不会把"进程内压测"带进门槛。
add_executable(fss_bench_capacity EXCLUDE_FROM_ALL capacity_bench.cpp)
target_link_libraries(fss_bench_capacity PRIVATE fss_domain fss_json fss_time fss_result
                                                 Threads::Threads)

add_executable(fss_bench_sqlite_write EXCLUDE_FROM_ALL sqlite_write_bench.cpp)
target_link_libraries(fss_bench_sqlite_write PRIVATE fss_location_sqlite fss_metadata_sqlite
                                                       fss_domain fss_json fss_time fss_result
                                                       SQLite::SQLite3 Threads::Threads)

# ADR-006 受控复核（C9.12）的 A/B 服务端：一个二进制、两种数据面
#   --mode httplib   产品路径（fss::http::Server + Response::Stream）
#   --mode sendfile  裸 socket + sendfile(2)（原型，只支持 GET /blob）
# 运行方式见 scripts/bench_sendfile_ab.sh（服务端/客户端分别 taskset 绑核，铁律 R2）。
add_executable(fss_bench_sendfile_ab EXCLUDE_FROM_ALL sendfile_ab.cpp)
target_link_libraries(fss_bench_sendfile_ab PRIVATE fss_http fss_logging fss_time fss_bytes
                                                      fss_result Threads::Threads)
