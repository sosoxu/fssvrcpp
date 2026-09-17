# L1：结构化日志 + 敏感字段脱敏。依赖 fss_json（记录形态）与 fss_time（时间戳）。
add_library(fss_logging STATIC logging.cpp)
target_include_directories(fss_logging PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fss_logging PUBLIC fss_result fss_json fss_time)
