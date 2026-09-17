# L1：JSON 封装。只依赖 nlohmann（header-only）与 fss_result。
add_library(fss_json STATIC json.cpp)
target_include_directories(fss_json PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(fss_json PUBLIC fss_result)
