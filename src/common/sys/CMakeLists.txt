# L1：系统能力探测（io_uring）。不依赖任何其它 L1 目标（只用 libc + syscall）。
add_library(fss_sys STATIC capability.cpp)
target_include_directories(fss_sys PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fss_sys PUBLIC fss_result)
