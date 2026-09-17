# L1：安全路径与原子写。依赖 fss_result 与 fss_crypto（临时文件名需要随机后缀）。
add_library(fss_fs STATIC fs.cpp)
target_include_directories(fss_fs PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fss_fs PUBLIC fss_result fss_crypto)
