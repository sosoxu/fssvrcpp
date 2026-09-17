# L1：URL 解析/构造、百分号编码、查询串编解码。只依赖 fss_result。
add_library(fss_net STATIC net.cpp)
target_include_directories(fss_net PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fss_net PUBLIC fss_result)
