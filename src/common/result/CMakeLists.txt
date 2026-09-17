# L1：错误模型与 Result<T>。所有层都依赖它，因此它只允许依赖 fss_common（无外部库）。
add_library(fss_result STATIC result.cpp)
target_include_directories(fss_result PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fss_result PUBLIC fss_common)
