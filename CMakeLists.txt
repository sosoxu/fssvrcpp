cmake_minimum_required(VERSION 3.16)

project(fssvrcpp
        VERSION 0.1.0
        DESCRIPTION "OSDU-compatible File Service backend in C++ (centralized + object storage, HTTP + RPC)"
        LANGUAGES CXX)

# -----------------------------------------------------------------------------
#  特性开关（必须在 Dependencies.cmake 之前定义：依赖探测依赖它们）
# -----------------------------------------------------------------------------
option(FSS_BUILD_TESTS "Build unit / integration / conformance tests" ON)
option(FSS_ENABLE_GRPC "Build the gRPC adapter and RPC tests" ON)
option(FSS_ENABLE_S3   "Build the S3 object-storage driver" ON)

# -----------------------------------------------------------------------------
#  全局编译设置
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# -----------------------------------------------------------------------------
#  L1 通用库（header-only）：不依赖任何基础设施
# -----------------------------------------------------------------------------
add_library(fss_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_compile_options(fss_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
    -Werror=return-type
    -Wno-deprecated-declarations)   # 第三方头文件（httplib/grpc）会产生告警噪音
endif()

add_library(fss_common INTERFACE)
target_include_directories(fss_common INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(fss_common INTERFACE fss_warnings)

# -----------------------------------------------------------------------------
#  依赖探测
# -----------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake)

# -----------------------------------------------------------------------------
#  RPC 契约（protobuf / gRPC 代码生成）
# -----------------------------------------------------------------------------
if(FSS_ENABLE_GRPC)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ProtoGen.cmake)

  # ---------------------------------------------------------------------------
  #  ★ GRPC_ASAN_SUPPRESSED：gRPC 与 ASan 的**布局耦合**，必须显式打断
  # ---------------------------------------------------------------------------
  #  实测（P1-D20）：`-fsanitize=address` 下 gRPC 的 `port_platform.h` 会因
  #  `__SANITIZE_ADDRESS__` 而定义 `GRPC_ASAN_ENABLED`，进而**改变 C++ 类的内存布局**：
  #      sizeof(grpc::ClientContext) = 512（带 ASan） / 504（不带 ASan）
  #  而系统预编译的 `libgrpc++.so` 是**不带 ASan** 构建的。于是我们的 TU 与库对同一个
  #  对象的成员偏移理解不一致 → 读到的 bool/计数是垃圾 → gRPC 内部按垃圾长度去 malloc
  #  （ASan 报 `requested allocation size 0x217080008c4000 ... exceeds maximum`）。
  #  这不是我们代码的问题，也不是"gRPC 有未初始化成员"——是**布局不一致**。
  #  gRPC 为此提供了开关：定义 `GRPC_ASAN_SUPPRESSED` 即不再定义 `GRPC_ASAN_ENABLED`，
  #  于是我们的布局与预编译库一致（实测 sizeof 回到 504、flags 回到 0）。
  #  这里挂在 gRPC 的 INTERFACE 上 → **任何**链接 gRPC 的目标都自动获得，避免漏配。
  #  代价：失去 gRPC 自身的 ASan 注解（那些注解只对"gRPC 自己用 ASan 编译"有意义，
  #  对预编译库本来就不生效）。我们自己的代码仍由 ASan 完整检查。
  target_compile_definitions(PkgConfig::GRPCPP INTERFACE GRPC_ASAN_SUPPRESSED=1)

  fss_add_proto_library(fss_proto
    PROTOS proto/osdu/file/v1/file_service.proto
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto)
  target_link_libraries(fss_proto PUBLIC fss_common)
endif()

# -----------------------------------------------------------------------------
#  源码（分层目标图）
# -----------------------------------------------------------------------------
add_compile_definitions(FSS_BUILD_VERSION="${PROJECT_VERSION}")
add_subdirectory(src)

# -----------------------------------------------------------------------------
#  基准 / 负载生成器（EXCLUDE_FROM_ALL；见 bench/CMakeLists.txt）
# -----------------------------------------------------------------------------
add_subdirectory(bench)

# -----------------------------------------------------------------------------
#  测试
# -----------------------------------------------------------------------------
if(FSS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
#  配置摘要
# -----------------------------------------------------------------------------
message(STATUS "----------------------------------------------------------")
message(STATUS " fssvrcpp ${PROJECT_VERSION} (${CMAKE_BUILD_TYPE})")
message(STATUS "   C++ standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "   compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "   grpc         : ${FSS_GRPC_VERSION}  (enabled=${FSS_ENABLE_GRPC})")
message(STATUS "   protobuf     : ${FSS_PROTOBUF_VERSION}")
message(STATUS "   openssl      : ${FSS_OPENSSL_VERSION}")
message(STATUS "   sqlite3      : ${FSS_SQLITE3_VERSION}")
message(STATUS "   tests        : ${FSS_BUILD_TESTS}")
message(STATUS "----------------------------------------------------------")
