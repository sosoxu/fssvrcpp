# =============================================================================
#  ProtoGen.cmake —— protobuf / gRPC 代码生成封装
# =============================================================================
#
#  fss_add_proto_library(<target>
#      PROTOS       <相对或绝对的 .proto 路径...>
#      IMPORT_DIRS  <protoc -I 搜索路径...>)
#
#  生成物（写入 ${CMAKE_CURRENT_BINARY_DIR}/fss_gen，避免污染源码树）：
#    <name>.pb.cc / <name>.pb.h
#    <name>.grpc.pb.cc / <name>.grpc.pb.h
#
#  设计要点
#  ---------------------------------------------------------------------------
#  * proto 只被 L5 适配层使用，因此生成的静态库 (fss_proto) 只允许被
#    src/adapters/grpc/ 链接。CMake 的 target 可见性天然形成编译期护栏：
#    L3 领域层若误引用 protobuf 类型，链接会直接失败。
#  * 已处理 protobuf 3.12 的一个已知告警：生成的代码会触发
#    -Warray-bounds / -Wstringop-overflow，因此对该 target 单独降噪。
# =============================================================================

function(fss_add_proto_library TARGET_NAME)
  cmake_parse_arguments(ARG "" "" "PROTOS;IMPORT_DIRS" ${ARGN})

  if(NOT ARG_PROTOS)
    message(FATAL_ERROR "fss_add_proto_library(${TARGET_NAME}): PROTOS 不能为空")
  endif()
  if(NOT FSS_ENABLE_GRPC)
    message(FATAL_ERROR
      "fss_add_proto_library(${TARGET_NAME}) 需要 FSS_ENABLE_GRPC=ON")
  endif()

  set(_gen_dir ${CMAKE_CURRENT_BINARY_DIR}/fss_gen)
  file(MAKE_DIRECTORY ${_gen_dir})

  set(_import_flags "")
  foreach(_dir ${ARG_IMPORT_DIRS})
    list(APPEND _import_flags "-I${_dir}")
  endforeach()

  set(_abs_protos "")
  set(_gen_srcs "")
  foreach(_proto ${ARG_PROTOS})
    if(IS_ABSOLUTE ${_proto})
      set(_abs ${_proto})
    else()
      set(_abs ${CMAKE_CURRENT_SOURCE_DIR}/${_proto})
    endif()
    if(NOT EXISTS ${_abs})
      message(FATAL_ERROR "proto 文件不存在: ${_abs}")
    endif()
    list(APPEND _abs_protos ${_abs})

    # protoc 会以"相对于某个 -I 目录"的路径在输出目录下建子目录，
    # 因此必须找到匹配的 import dir 才能推出生成文件的确切位置。
    set(_rel "")
    foreach(_dir ${ARG_IMPORT_DIRS})
      file(RELATIVE_PATH _cand ${_dir} ${_abs})
      if(NOT _cand MATCHES "^\\.\\./")
        set(_rel ${_cand})
        break()
      endif()
    endforeach()
    if(NOT _rel)
      message(FATAL_ERROR
        "proto ${_abs} 不在任何 IMPORT_DIRS 之下；protoc 会拒绝或产生意外路径。\n"
        "  IMPORT_DIRS = ${ARG_IMPORT_DIRS}")
    endif()

    string(REGEX REPLACE "\\.proto$" "" _stem ${_rel})
    list(APPEND _gen_srcs
      ${_gen_dir}/${_stem}.pb.cc
      ${_gen_dir}/${_stem}.grpc.pb.cc)
  endforeach()

  add_custom_command(
    OUTPUT ${_gen_srcs}
    COMMAND ${PROTOC_EXECUTABLE}
            ${_import_flags}
            --cpp_out=${_gen_dir}
            --grpc_out=${_gen_dir}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
            ${_abs_protos}
    DEPENDS ${_abs_protos}
    COMMENT "protoc: 生成 gRPC/protobuf C++ 代码 (${_rel})"
    VERBATIM)

  add_library(${TARGET_NAME} STATIC ${_gen_srcs})
  target_include_directories(${TARGET_NAME} PUBLIC ${_gen_dir})
  target_link_libraries(${TARGET_NAME} PUBLIC
    PkgConfig::GRPCPP
    PkgConfig::PROTOBUF
    Threads::Threads)
  # protobuf 3.12 生成代码的已知告警降噪
  target_compile_options(${TARGET_NAME} PRIVATE
    -Wno-array-bounds -Wno-stringop-overflow -Wno-unused-parameter)
endfunction()
