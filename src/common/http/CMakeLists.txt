# L1：fss_http —— cpp-httplib 之上的强化包装层。
# ★ 全仓库**唯一**允许 include <httplib.h> 的目标（分层护栏会在源码层再查一遍）。
add_library(fss_http STATIC server.cpp)
target_include_directories(fss_http PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(fss_http PUBLIC fss_result fss_bytes fss_net fss_logging fss_ids fss_time Threads::Threads)
