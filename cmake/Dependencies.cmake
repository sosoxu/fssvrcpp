# =============================================================================
#  Dependencies.cmake —— 依赖探测
# =============================================================================
#
#  策略说明
#  ---------------------------------------------------------------------------
#  本机为 Ubuntu 22.04 / g++ 11.4 / 无 root 权限（sudo 需要密码），因此依赖
#  全部采用"系统已安装 + apt-get download 解包到 third_party"的方式获取，
#  不引入 conan/vcpkg，也不需要网络下载源码。
#
#  已验证可用（见 docs/04-implementation-plan.md 阶段 0 的测试证据）：
#    grpc++ 1.30.2, protobuf 3.12.4, protoc 3.12.4, grpc_cpp_plugin
#    openssl 3.0.2 (libcrypto: HMAC-SHA256，用于 S3 SigV4 预签名)
#    libcurl 7.81 (dev headers 位于 /usr/include/x86_64-linux-gnu)
#    sqlite3 3.37
#    nlohmann/json 3.10.5   -> third_party/nlohmann/json.hpp  (header-only)
#    Catch2 2.13.8          -> third_party/catch2/catch.hpp   (header-only)
#    cpp-httplib 0.26.0     -> third_party/httplib.h          (header-only, MIT)
#
#  HTTP 传输栈的选型结论见 docs/adr/ADR-002-http-framework.md：
#  使用 cpp-httplib 0.26.0 的**上游源码单头文件**（third_party/httplib.h，MIT）
#  作传输层，并在 src/common/http/ 建 fss_http 强化包装层承担安全与正确性边界。
#
#  ⚠️ 不要改用 Ubuntu 的 libcpp-httplib-dev/libcpp-httplib0 二进制包：
#     ① 只有声明头，实现是预编译 .so，且以 CPPHTTPLIB_OPENSSL|ZLIB|BROTLI_SUPPORT
#        编译，要求使用方宏完全一致，否则类布局不一致（ABI 耦合）；
#     ② jammy 只提供 0.10.3，该版本的"流式响应 + 越界 Range"存在 size_t 下溢缺陷。
#  出站 HTTP 客户端统一使用 libcurl。
#  只有 src/common/http/ 允许 #include <httplib.h>（分层护栏强制）。
#
#  重要约束：protobuf 3.12 不支持 proto3 `optional` 字段（需 3.15+），
#  因此 proto 文件中禁止使用 optional，改用 wrapper/message 表达可选性。
# =============================================================================

find_package(PkgConfig REQUIRED)

# -----------------------------------------------------------------------------
#  OpenSSL（S3 SigV4 / 本地签名 URL 的 HMAC-SHA256）
# -----------------------------------------------------------------------------
find_package(OpenSSL REQUIRED)
set(FSS_OPENSSL_VERSION ${OPENSSL_VERSION})

# -----------------------------------------------------------------------------
#  SQLite3（文件位置 / 元数据记录的本地持久化）
# -----------------------------------------------------------------------------
find_package(SQLite3 REQUIRED)
set(FSS_SQLITE3_VERSION ${SQLite3_VERSION})

# -----------------------------------------------------------------------------
#  CURL（S3 REST 数据面 + 对外 HTTP 客户端：Entitlements / Storage / Partition）
# -----------------------------------------------------------------------------
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
if(NOT TARGET CURL::libcurl)
  add_library(CURL::libcurl ALIAS PkgConfig::CURL)
endif()
# -----------------------------------------------------------------------------
#  Threads
# -----------------------------------------------------------------------------
find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
#  gRPC + protobuf（RPC 协议适配层）
# -----------------------------------------------------------------------------
if(FSS_ENABLE_GRPC)
  pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
  pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)

  find_program(PROTOC_EXECUTABLE NAMES protoc REQUIRED)
  find_program(GRPC_CPP_PLUGIN_EXECUTABLE NAMES grpc_cpp_plugin REQUIRED)

  execute_process(COMMAND ${PROTOC_EXECUTABLE} --version
                  OUTPUT_VARIABLE _protoc_out OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" FSS_PROTOBUF_VERSION "${_protoc_out}")
  pkg_check_modules(_grpc_ver QUIET grpc++)
  set(FSS_GRPC_VERSION "${_grpc_ver_VERSION}")
  if(NOT FSS_GRPC_VERSION)
    set(FSS_GRPC_VERSION "unknown")
  endif()
endif()

# -----------------------------------------------------------------------------
#  第三方 header-only 依赖的存在性断言（快速失败，给出可执行的修复指令）
# -----------------------------------------------------------------------------
foreach(_hdr
        third_party/nlohmann/json.hpp
        third_party/catch2/catch.hpp
        third_party/httplib.h)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_hdr})
    message(FATAL_ERROR
      "缺少第三方依赖 ${_hdr}。\n"
      "修复方式（无需 root）：\n"
      "  cd /tmp && apt-get download <pkg> && dpkg-deb -x <pkg>.deb ext\n"
      "  获取方式见 third_party/README.md（均无需 root）：\n"
      "    nlohmann-json3-dev / catch2  -> apt-get download + dpkg-deb -x\n"
      "    cpp-httplib 0.26.0           -> 镜像 pool 的上游 orig.tar.xz（勿用二进制包！）")
  endif()
endforeach()


# -----------------------------------------------------------------------------
#  cpp-httplib 版本锁定断言
# -----------------------------------------------------------------------------
#  0.10.3 在"流式响应 + 越界 Range"上有 size_t 下溢缺陷（H-1），必须避开。
#  0.26.0 修复了 H-1，但仍有 H-2（流式请求体超限时静默 201/0 字节），
#  由 src/common/http/ 的 fss_http 包装层兜住。详见 docs/adr/ADR-002。
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib.h _httplib_head LIMIT 4096)
if(NOT _httplib_head MATCHES "CPPHTTPLIB_VERSION \"0\\.26\\.0\"")
  message(FATAL_ERROR
    "third_party/httplib.h 不是 cpp-httplib 0.26.0。\n"
    "  本项目依赖 0.26.0 修复的 H-1（越界 Range 下溢）行为，且 fss_http 包装层\n"
    "  的 H-2 防护与回归测试是按该版本行为标定的。\n"
    "  升级前请阅读 docs/adr/ADR-002-http-framework.md §4 与 third_party/README.md，\n"
    "  更新 third_party/CHECKSUMS.txt，并重跑 ctest -L phase1。")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/CHECKSUMS.txt)
  message(STATUS "  httplib      : 0.26.0 (pinned, sha256 见 third_party/CHECKSUMS.txt)")
endif()
