# =============================================================================
#  分层目标图（docs/02-design.md §4）
# =============================================================================
#
#  ★ 这个文件是"低耦合"承诺的**编译期执行者**，不是目录清单。
#     依赖方向：L5 ─▶ L4 ─▶ L3 ◀─ L2 ─▶ L1   （L2 实现 L3 的端口：依赖倒置）
#     一旦有人在领域层 #include <grpcpp/grpcpp.h> 或 <sqlite3.h>，构建期就失败。
#     源码级检查见 tests/unit/test_layering_guard.cpp，其自证见 scripts/verify_guard.sh。
#
#  允许的依赖（只有这些）：
#       fss_domain       → fss_result, fss_common
#       fss_app          → fss_domain
#       fss_infra_*      → fss_domain (+ 各自外部库)
#       fss_http_adapter → fss_app (+ fss_http)
#       fss_grpc_adapter → fss_app (+ fss_proto)
#       fss_server       → 以上全部（**唯一**允许实例化具体实现的地方）
# =============================================================================

# -----------------------------------------------------------------------------
#  L1 通用库
# -----------------------------------------------------------------------------
add_subdirectory(common)

# -----------------------------------------------------------------------------
#  L3 领域层：只允许链接 L1
#     即使内容为空也要建成 STATIC（不是 INTERFACE）—— 否则无法在构建期捕获违规
# -----------------------------------------------------------------------------
add_library(fss_domain STATIC
  domain/version.cpp
  domain/contract/error_table.cpp
  domain/model/file_metadata.cpp)
target_include_directories(fss_domain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# 端口签名用到字节流抽象（L1）与 IClock/IIdGenerator（L1）→ 允许且必须（L3 → L1）
target_link_libraries(fss_domain PUBLIC fss_result fss_json fss_time fss_bytes fss_ids)

# -----------------------------------------------------------------------------
#  L4 应用层：只允许链接 L3
# -----------------------------------------------------------------------------
add_library(fss_app STATIC
  app/version.cpp
  app/services/expiry_policy.cpp
  app/services/kind_validator.cpp
  app/services/object_key_policy.cpp
  app/services/clock_skew_guard.cpp
  app/services/location_issuer.cpp
  app/usecases/usecases.cpp
  app/usecases/caller_context.cpp
  app/usecases/wire_shapes.cpp
  app/tasks/gc_task.cpp)
target_include_directories(fss_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# fss_crypto（L1）：CreateFileMetadata 的校验和覆写需要 SHA-256。L4 → L1 允许。
target_link_libraries(fss_app PUBLIC fss_domain fss_crypto fss_metrics)

# -----------------------------------------------------------------------------
#  L5 适配层：REST。只允许链接 fss_app（+ fss_http 包装层）。
# -----------------------------------------------------------------------------
add_library(fss_http_adapter STATIC
  adapters/http/http_error_mapper.cpp
  adapters/http/dto/dto.cpp
  adapters/http/metrics.cpp
  adapters/http/router.cpp)
target_include_directories(fss_http_adapter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fss_http_adapter PUBLIC fss_app fss_http)

# -----------------------------------------------------------------------------
#  L5 适配层：gRPC（RPC 是**平台外扩展**，见 ADR-001）。只允许链接 fss_app（+ fss_proto）。
# -----------------------------------------------------------------------------
if(FSS_ENABLE_GRPC)
  add_library(fss_grpc_adapter STATIC
    adapters/grpc/file_service_adapter.cpp
    adapters/grpc/grpc_error_mapper.cpp
    adapters/grpc/grpc_server.cpp
    adapters/grpc/grpc_streaming_io.cpp
    adapters/grpc/dto/grpc_dto.cpp)
  target_include_directories(fss_grpc_adapter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(fss_grpc_adapter PUBLIC fss_app fss_proto)
endif()

# -----------------------------------------------------------------------------
#  L2 认证（P8）：本地 JWT 校验（ADR-012）
#    只依赖 fss_domain（端口）+ fss_crypto/json/time（L1）
# -----------------------------------------------------------------------------
add_library(fss_auth_local STATIC
  infra/auth/local/local_jwt_authorizer.cpp)
target_include_directories(fss_auth_local PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fss_auth_local PUBLIC fss_domain fss_crypto fss_json fss_time)

# 远端 Entitlements（auth.mode=remote-entitlements）：HTTP + libcurl，严格 fail-closed
add_library(fss_auth_remote STATIC
  infra/auth/remote/remote_entitlements_authorizer.cpp)
target_include_directories(fss_auth_remote PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fss_auth_remote PUBLIC fss_domain fss_json)
find_package(CURL REQUIRED)
target_link_libraries(fss_auth_remote PUBLIC CURL::libcurl)

# -----------------------------------------------------------------------------
#  组合根（唯一实例化具体实现的地方；R12）
# -----------------------------------------------------------------------------
add_executable(fss_server
  main/server_main.cpp)
target_link_libraries(fss_server PRIVATE
  fss_http_adapter fss_grpc_adapter fss_auth_local fss_auth_remote fss_blob_posix fss_blob_s3
  fss_location_sqlite fss_transfer fss_metadata_memory fss_logging Threads::Threads)

# -----------------------------------------------------------------------------
#  L2 基础设施适配层：每个端口实现一个独立目标（依赖倒置：L2 → L3 端口）
#     只允许链接 fss_domain（+ 该实现真正需要的 L1 / 外部库）。
#     内存实现是"契约测试基准"：POSIX / SQLite / PostgreSQL 实现上线后，必须
#     与它共用 tests/framework/port_contract.h 的同一套断言（C2.10）。
# -----------------------------------------------------------------------------
add_library(fss_blob_memory STATIC
  infra/blob/memory/memory_blob_store.cpp)
target_include_directories(fss_blob_memory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# fss_crypto：put 的 expected_checksum 校验需要 SHA-256（OpenSSL 只允许出现在 L1/L2）
target_link_libraries(fss_blob_memory PUBLIC fss_domain fss_crypto)

# L2：I/O 引擎（ADR-010）与落盘接缝（ADR-008 的 fsync 分级）
add_library(fss_io STATIC
  infra/io/file_sync.cpp
  infra/io/blocking_io_engine.cpp
  infra/io/uring_io_engine.cpp)
target_include_directories(fss_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fss_io PUBLIC fss_domain fss_sys)

# L2：集中存储驱动（POSIX）。依赖 fss_fs（安全路径 + 原子写）、fss_crypto（流式校验和）
# 与 fss_io（fsync 分级 + 可注入落盘接缝）。
add_library(fss_blob_posix STATIC
  infra/blob/posix/posix_blob_store.cpp)
target_include_directories(fss_blob_posix PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fss_blob_posix PUBLIC fss_domain fss_crypto fss_fs fss_io)

# S3 驱动（P5）：SigV4 签名器 + S3BlobStore（逐步补齐）
add_library(fss_blob_s3 STATIC
  infra/blob/s3/sigv4_signer.cpp
  infra/blob/s3/s3_blob_store.cpp)
target_include_directories(fss_blob_s3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fss_blob_s3 PUBLIC fss_domain fss_crypto)
# S3 数据面用 libcurl（P5 切片 2 起）；这里先声明，避免后面忘记
find_package(CURL QUIET)
if(CURL_FOUND)
  target_link_libraries(fss_blob_s3 PUBLIC CURL::libcurl)
  target_compile_definitions(fss_blob_s3 PUBLIC FSS_WITH_LIBCURL=1)
endif()

# P9：存储指标装饰器（操作数 + 字节数；所有驱动共用一份实现）
add_library(fss_blob_metered STATIC
  infra/blob/metered/metered_blob_store.cpp)
target_include_directories(fss_blob_metered PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fss_blob_metered PUBLIC fss_domain fss_metrics)

add_library(fss_location_memory STATIC
  infra/location/memory/memory_location_repository.cpp)
target_include_directories(fss_location_memory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fss_location_memory PUBLIC fss_domain)

# L2：单实例位置仓储（SQLite）。只链接 fss_domain + SQLite3。
add_library(fss_location_sqlite STATIC
  infra/location/sqlite/sqlite_location_repository.cpp)
target_include_directories(fss_location_sqlite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fss_location_sqlite PUBLIC fss_domain SQLite::SQLite3)

# L2：集中存储的自签传输 token 与 `/v1/transfer` 内核（ADR-003 §6.3）
add_library(fss_transfer STATIC
  infra/transfer/transfer_token.cpp
  infra/transfer/transfer_endpoint.cpp)
target_include_directories(fss_transfer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fss_transfer PUBLIC fss_domain fss_crypto)

add_library(fss_metadata_sqlite STATIC
  infra/metadata/sqlite/sqlite_metadata_repository.cpp)
target_include_directories(fss_metadata_sqlite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fss_metadata_sqlite PUBLIC fss_domain fss_json SQLite::SQLite3)

add_library(fss_metadata_memory STATIC
  infra/metadata/memory/memory_metadata_repository.cpp)
target_include_directories(fss_metadata_memory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# IClock 声明在 L1（fss_time），元数据仓储用它记录 created_at
target_link_libraries(fss_metadata_memory PUBLIC fss_domain fss_time)
