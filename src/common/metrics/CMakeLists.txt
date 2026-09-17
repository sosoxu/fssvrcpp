# L1：与协议无关的计数器/仪表注册表（P9 的 C9.6：存储/GC 指标）
add_library(fss_metrics STATIC metrics.cpp)
target_include_directories(fss_metrics PUBLIC ${CMAKE_SOURCE_DIR}/src)
