# L1：时钟与时间格式。IClock 在这里声明（见头文件里的说明）。
add_library(fss_time STATIC clock.cpp time_format.cpp)
target_include_directories(fss_time PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fss_time PUBLIC fss_result)
