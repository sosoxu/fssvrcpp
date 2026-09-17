# L1：Range 归一化 + 字节流抽象 + 缓冲 + 限流。依赖 fss_result 与 fss_time（限流的时钟）。
add_library(fss_bytes STATIC bytes.cpp)
target_include_directories(fss_bytes PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fss_bytes PUBLIC fss_result fss_time)
