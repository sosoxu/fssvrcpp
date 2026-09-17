# L1：配置加载与全量校验。依赖 fss_result 与 fss_json。
add_library(fss_config STATIC config.cpp core_schema.cpp)
target_include_directories(fss_config PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fss_config PUBLIC fss_result fss_json)
