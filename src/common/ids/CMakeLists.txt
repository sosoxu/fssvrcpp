# L1：ID 生成。UuidGenerator 需要 fss_crypto 的随机源。
add_library(fss_ids STATIC id_generator.cpp)
target_include_directories(fss_ids PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fss_ids PUBLIC fss_result fss_crypto)
