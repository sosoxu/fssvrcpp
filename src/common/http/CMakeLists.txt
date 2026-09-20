# L1：fss_http —— cpp-httplib 之上的强化包装层。
# ★ 全仓库**唯一**允许 include <httplib.h> 的目标（分层护栏会在源码层再查一遍）。
#  ADR-006：大文件数据面（第二个监听 socket + sendfile）也在这里 —— 它是**传输层**
#  的关注点（原始 socket / 请求解析 / 访问日志同源），不依赖任何上层类型。
add_library(fss_http STATIC server.cpp large_file_plane.cpp)
target_include_directories(fss_http PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(fss_http PUBLIC fss_result fss_bytes fss_net fss_logging fss_ids fss_time
                                     fss_metrics Threads::Threads)
