# =============================================================================
#  测试工程
# =============================================================================
#
#  分层
#  ---------------------------------------------------------------------------
#   tests/framework/  -> 极少数测试基建（临时目录、进程内服务器启动器等）
#   tests/unit/       -> 纯内存、毫秒级、无 IO；每次提交必跑
#   tests/integration/-> 启动真实组件（真实 HTTP/gRPC 端口、真实文件系统、
#                        mock-S3 或临时目录），秒级
#   tests/conformance/-> 与 OSDU 规范对齐性测试（黄金样例请求/响应比对）
#
#  统一使用 Catch2 v2（header-only，third_party/catch2/catch.hpp）。
#  每个测试文件编译为独立可执行文件并注册到 CTest，便于按阶段筛选：
#     ctest -L phase0
# =============================================================================

add_library(fss_test_support INTERFACE)
target_include_directories(fss_test_support INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/framework
  ${CMAKE_CURRENT_SOURCE_DIR})
#  ★ 所有测试都能拿到源码树位置：黄金样例/守卫脚本类的用例要按绝对路径读 fixtures。
#   放在 INTERFACE 上而不是逐个目标加 —— 逐个加时"新测试忘了加"会表现成
#   `FSS_REPO_ROOT was not declared`，而那是最容易照抄邻居修好的假错误。
target_compile_definitions(fss_test_support INTERFACE FSS_REPO_ROOT="${CMAKE_SOURCE_DIR}")

# 辅助函数：注册一个测试可执行文件
#   fss_add_test(<name> SOURCES <src...> [LIBS <lib...>] [LABELS <label...>])
function(fss_add_test TEST_NAME)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;LABELS" ${ARGN})
  # ★ 统一注入 Catch2 主入口：测试文件里**不要**自己写 CATCH_CONFIG_MAIN
  #   （否则要么漏写导致 undefined reference to main，要么重复定义 main）
  add_executable(${TEST_NAME}
    ${CMAKE_CURRENT_SOURCE_DIR}/framework/catch_main.cpp
    ${ARG_SOURCES})
  target_link_libraries(${TEST_NAME} PRIVATE
    fss_common fss_test_support ${ARG_LIBS})
  if(NOT ARG_LABELS)
    set(ARG_LABELS "unit")
  endif()
  add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
  set_tests_properties(${TEST_NAME} PROPERTIES
    LABELS "${ARG_LABELS}"
    TIMEOUT 300)
endfunction()

# -----------------------------------------------------------------------------
#  阶段 0：工具链与契约可行性验证（本阶段唯一目标 —— 证明后续计划可执行）
# -----------------------------------------------------------------------------
fss_add_test(test_phase0_toolchain
  SOURCES
    integration/test_phase0_toolchain.cpp
  LIBS
    OpenSSL::Crypto
    SQLite::SQLite3
    CURL::libcurl
    Threads::Threads
  LABELS phase0 integration)

if(FSS_ENABLE_GRPC)
  target_link_libraries(test_phase0_toolchain PRIVATE fss_proto)
  target_compile_definitions(test_phase0_toolchain PRIVATE FSS_ENABLE_GRPC=1)
  # gRPC 生成的 stub 会触发告警，测试代码本身保持严格
  target_compile_options(test_phase0_toolchain PRIVATE -Wno-unused-parameter)
endif()

# =============================================================================
#  PostgreSQL 基建测试（真实 PG；可选，见 docs/development.md）
# =============================================================================
#
#  为什么可选：默认构建必须能在**没有任何外部服务**的机器上通过
#  （"每步先测"的前提是测试随时可跑）。因此默认 OFF，由门槛脚本或 CI 显式开启：
#      cmake -S . -B build -DFSS_WITH_PG=ON
#      ctest --test-dir build -L pg
#
#  这些测试验证的是**真实 PostgreSQL 才能验证的东西**（ADR-009）：
#     · 唯一约束的冲突语义与 ON CONFLICT 的原子领取   → 支撑门槛 C6.11 / C9.26
#     · 跨会话 advisory lock 的互斥与崩溃后释放        → 支撑门槛 C8.9 / C9.26
#     · 迁移后的 schema 真的强制了那些不变量            → 支撑门槛 C2.10
#
#  测试用 CTest fixture 管理 PG 生命周期：pg_fixture_setup 先启动并应用迁移，
#  三个测试都 FIXTURES_REQUIRED pg；teardown 默认不停服务（便于开发时排查），
#  设 FSS_PG_TEARDOWN=ON 可在测试结束后自动停止。
option(FSS_WITH_PG "Register the PostgreSQL-backed tests (requires scripts/dev_postgres.sh)" OFF)
option(FSS_PG_TEARDOWN "Stop the dev PostgreSQL after PG tests" OFF)

if(FSS_WITH_PG)
  set(_pg_script "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/dev_postgres.sh")
  set(_db_dir    "${CMAKE_CURRENT_SOURCE_DIR}/../db")
  if(NOT EXISTS "${_pg_script}")
    message(FATAL_ERROR "FSS_WITH_PG=ON 但找不到 ${_pg_script}")
  endif()
  #  ★ A1 起：`FSS_WITH_PG` 还要求 libpq（`src/infra/postgres/` 的 PG 仓储要链接
  #    PostgreSQL::PostgreSQL）。原来只检查 dev 脚本存在，于是"有 PG 服务但没有 libpq
  #    开发文件"的机器会在链接期才失败（甚至表现为 PG 测试被静默跳过）——
  #    这里在配置期就 fail-closed 并给出可执行的修复指令（R9）。
  if(NOT FSS_HAVE_LIBPQ)
    message(FATAL_ERROR
      "FSS_WITH_PG=ON 需要 libpq（CMake 未找到 PostgreSQL::PostgreSQL）。\n"
      "  修复：安装 libpq 开发文件（Debian/Ubuntu: libpq-dev），或去掉 -DFSS_WITH_PG=ON。\n"
      "  （`scripts/dev_postgres.sh` 只提供**服务端二进制**，不提供 libpq 头文件与库。）")
  endif()

  add_test(NAME pg_fixture_setup
           COMMAND "${_pg_script}" start)
  set_tests_properties(pg_fixture_setup PROPERTIES
    FIXTURES_SETUP pg
    LABELS "pg;infra"
    TIMEOUT 900)      # 首次运行需要 apt 解包二进制 + initdb，给足时间

  add_test(NAME pg_schema_invariants
           COMMAND "${_pg_script}" psql -v ON_ERROR_STOP=1 -q
                   -f "${_db_dir}/tests/001_verify_invariants.sql")
  set_tests_properties(pg_schema_invariants PROPERTIES
    FIXTURES_REQUIRED pg
    LABELS "pg;infra"
    TIMEOUT 300)

  add_test(NAME pg_advisory_lock
           COMMAND "${_db_dir}/tests/002_advisory_lock.sh")
  set_tests_properties(pg_advisory_lock PROPERTIES
    FIXTURES_REQUIRED pg
    LABELS "pg;infra"
    TIMEOUT 300)

  add_test(NAME pg_concurrent_claim
           COMMAND "${_db_dir}/tests/003_concurrent_claim.sh" 10 8)
  set_tests_properties(pg_concurrent_claim PROPERTIES
    FIXTURES_REQUIRED pg
    LABELS "pg;infra"
    TIMEOUT 900)

  # ===========================================================================
  #  A1（ADR-009 §10）：PostgreSQL 仓储的**共享契约测试**
  #    · 与内存/SQLite 实现跑**同一套**断言（`tests/framework/port_contract.h`）：
  #      位置仓储 `CheckLocationRepositoryContract` + 租约 `CheckLeaseContract`；
  #    · DSN 来自 `FSS_PG_DSN`（未设时用本机 dev PG 的默认 DSN），
  #      因此同一份测试可以指向任何 PG（含远端 12.6）；
  #    · **连不上就失败**（REQUIRE + libpq 消息），绝不静默跳过 —— 跳过的门槛是空证据；
  #    · 每个用例用 `pgtest-<pid>-<n>-…` 的**唯一 partition**，teardown 删自己的行。
  # ===========================================================================
  fss_add_test(test_postgres_repositories
    SOURCES integration/test_postgres_repositories.cpp
    LIBS    fss_location_postgres fss_metadata_postgres Threads::Threads
    LABELS  "pg;infra")
  set_tests_properties(test_postgres_repositories PROPERTIES
    FIXTURES_REQUIRED pg
    TIMEOUT 300)

  # ===========================================================================
  #  B1（ADR-009 §4.4/§4.5）：`deployment.mode=multi` **真的能跑**
  #    · 真实 `build/bin/fss_server` 进程 + `--config`/`--set`（DSN 走 `FSS_PG_DSN`
  #      同源的值，由测试显式写给服务器进程）；
  #    · 直连 libpq 读回 PG 行（正控）与 `pg_locks`（advisory lock 正控）；
  #    · 两个共键进程 → 恰好一个 leader；非 leader 的 `fss_gc_runs_total` 不增加；
  #    · fail-closed 负例（不可达 DSN / 非法 repository）→ exit 78 且不绑定端口。
  #  ★ 连不上 PG 就失败（REQUIRE），绝不静默跳过；等待一律轮询实际条件。
  # ===========================================================================
  fss_add_test(test_multi_mode
    SOURCES integration/test_multi_mode.cpp
    LIBS    fss_app fss_blob_memory fss_location_memory fss_metadata_memory fss_pg
            Threads::Threads
    LABELS  "pg;infra")
  add_dependencies(test_multi_mode fss_server)
  set_tests_properties(test_multi_mode PROPERTIES
    FIXTURES_REQUIRED pg
    TIMEOUT 900)

  # ===========================================================================
  #  C2（ADR-009 §4.2/§4.3）：PG 租约的 TTL / time_source / 崩溃回收可观测性
  #    · `leases.ttl_seconds` 真的写进 `staging_leases`（直接查表）；
  #    · `time_source=local` 跟随注入 IClock（与 database 对照），且三原语自洽；
  #    · 崩溃→GC 回收 claiming 行→同 fileSource 重试，端到端成立（local 钟，确定性）。
  # ===========================================================================
  fss_add_test(test_postgres_lease_lifecycle
    SOURCES integration/test_postgres_lease_lifecycle.cpp
    LIBS    fss_app fss_blob_memory fss_location_memory fss_metadata_memory
            fss_location_postgres fss_metadata_postgres Threads::Threads
    LABELS  "pg;infra")
  set_tests_properties(test_postgres_lease_lifecycle PROPERTIES
    FIXTURES_REQUIRED pg
    TIMEOUT 300)

  if(FSS_PG_TEARDOWN)
    add_test(NAME pg_fixture_teardown COMMAND "${_pg_script}" stop)
    set_tests_properties(pg_fixture_teardown PROPERTIES
      FIXTURES_CLEANUP pg
      LABELS "pg;infra"
      TIMEOUT 120)
  else()
    # 即使不停止服务，也要注册一个 cleanup fixture，否则 CTest 会告警
    add_test(NAME pg_fixture_teardown COMMAND "${CMAKE_COMMAND}" -E echo "PG 保留运行（FSS_PG_TEARDOWN=OFF）")
    set_tests_properties(pg_fixture_teardown PROPERTIES
      FIXTURES_CLEANUP pg
      LABELS "pg;infra"
      TIMEOUT 30)
  endif()
endif()

# -----------------------------------------------------------------------------
#  阶段 1：分层骨架 + 通用库
# -----------------------------------------------------------------------------
fss_add_test(test_layering_guard
  SOURCES unit/test_layering_guard.cpp
  LIBS    fss_domain fss_app
  LABELS  phase1 unit)
# 护栏需要知道源码树位置（它是文件系统扫描，与构建系统无关）



fss_add_test(test_result
  SOURCES unit/test_result.cpp
  LIBS    fss_result
  LABELS  phase1 unit)

fss_add_test(test_crypto
  SOURCES unit/test_crypto.cpp
  LIBS    fss_crypto
  LABELS  phase1 unit)

fss_add_test(test_json SOURCES unit/test_json.cpp LIBS fss_json LABELS "phase1;unit")
fss_add_test(test_fs   SOURCES unit/test_fs.cpp   LIBS fss_fs   LABELS "phase1;unit")
fss_add_test(test_time SOURCES unit/test_time.cpp LIBS fss_time LABELS "phase1;unit")
fss_add_test(test_ids  SOURCES unit/test_ids.cpp  LIBS fss_ids  LABELS "phase1;unit")

fss_add_test(test_config SOURCES unit/test_config.cpp LIBS fss_config LABELS "phase1;unit")

fss_add_test(test_logging SOURCES unit/test_logging.cpp LIBS fss_logging LABELS "phase1;unit")

fss_add_test(test_bytes SOURCES unit/test_bytes.cpp LIBS fss_bytes LABELS "phase1;unit")

fss_add_test(test_net SOURCES unit/test_net.cpp LIBS fss_net LABELS "phase1;unit")

fss_add_test(test_sys SOURCES unit/test_sys.cpp LIBS fss_sys LABELS "phase1;unit")

fss_add_test(test_httplib_hardening
  SOURCES integration/test_httplib_hardening.cpp
  LIBS    fss_http
  LABELS  phase1 integration)

fss_add_test(test_http_server
  SOURCES integration/test_http_server.cpp
  LIBS    fss_http
  LABELS  phase1 integration)

fss_add_test(test_expiry_policy SOURCES unit/test_expiry_policy.cpp LIBS fss_app LABELS "phase2;unit")
fss_add_test(test_kind_validator SOURCES unit/test_kind_validator.cpp LIBS fss_app LABELS "phase2;unit")
fss_add_test(test_object_key_policy SOURCES unit/test_object_key_policy.cpp LIBS fss_app LABELS "phase2;unit")

fss_add_test(test_file_metadata SOURCES unit/test_file_metadata.cpp LIBS fss_domain LABELS "phase2;unit")

fss_add_test(test_ports SOURCES unit/test_ports.cpp LIBS fss_domain LABELS "phase2;unit")

# C2.10：端口契约测试基类（tests/framework/port_contract.h）的第一个使用者。
# 后续 POSIX/S3/SQLite/PostgreSQL 实现只加各自的"三行"实例化，语义断言不再重复。
fss_add_test(test_port_contract_memory
  SOURCES unit/test_port_contract_memory.cpp
  LIBS    fss_blob_memory fss_location_memory fss_metadata_memory
  LABELS  phase2 unit)

# C2.6：LocationIssuer 在两种能力组合下的行为（借用内存数据面 + 能力装饰器）
fss_add_test(test_location_issuer
  SOURCES unit/test_location_issuer.cpp
  LIBS    fss_app fss_blob_memory fss_location_memory
  LABELS  phase2 unit)

# C2.7：capabilities() 调用点护栏（源码扫描；自证见 scripts/verify_capability_guard.sh）
fss_add_test(test_capability_guard
  SOURCES unit/test_capability_guard.cpp
  LIBS    fss_app
  LABELS  phase2 unit)

# C2.2：13 个用例的单元测试 + ErrorKind 覆盖率矩阵
fss_add_test(test_usecases
  SOURCES unit/test_usecases.cpp
  LIBS    fss_app fss_blob_memory fss_location_memory fss_metadata_memory
  LABELS  phase2 unit)

fss_add_test(test_error_kind_coverage
  SOURCES unit/test_error_kind_coverage.cpp
  LIBS    fss_app fss_blob_memory fss_location_memory fss_metadata_memory
  LABELS  phase2 unit)

# -----------------------------------------------------------------------------
#  阶段 3：集中存储驱动 + 位置仓储 + 数据面
# -----------------------------------------------------------------------------
# C3.1：与内存实现共用同一套契约测试；C3.2 路径安全；C3.3 原子写；C3.5 Range；C3.6 并发
fss_add_test(test_posix_blob_store
  SOURCES integration/test_posix_blob_store.cpp
  LIBS    fss_blob_posix Threads::Threads
  LABELS  phase3 integration)

# C2.10/C3.8：SQLite 位置仓储（与内存实现共用同一套契约测试）
fss_add_test(test_sqlite_location_repository
  SOURCES integration/test_sqlite_location_repository.cpp
  LIBS    fss_location_sqlite
  LABELS  phase3 integration)

# C3.9：SQL 必须带 partition_id 的护栏（源码扫描；含非空洞性断言）
fss_add_test(test_sql_guardrail
  SOURCES unit/test_sql_guardrail.cpp
  LIBS    fss_domain
  LABELS  phase3 unit)

# C3.10：BlockingIoEngine 的 pread 定位读并发安全（含单线程基线对照）
fss_add_test(test_blocking_io_engine
  SOURCES integration/test_blocking_io_engine.cpp
  LIBS    fss_io fss_fs Threads::Threads
  LABELS  phase3 integration)

# C3.11：fsync 分级策略（注入 IFileSync，直接数落盘次数）
fss_add_test(test_fsync_policy
  SOURCES integration/test_fsync_policy.cpp
  LIBS    fss_blob_posix fss_io
  LABELS  phase3 integration)

# ★ C9.23（ADR-008 的 P4）：两阶段批提交的摊销 / 顺序不变量 / 数据正确性 / 失败路径。
#   用**注入的事件记录器 + 入批门控**把"批大小"和"顺序"变成确定性判据（不靠 sleep）。
#   放在 integration（真实文件系统 + 多线程），与 test_fsync_policy 同类。
fss_add_test(test_posix_batch_commit
  SOURCES integration/test_posix_batch_commit.cpp
  LIBS    fss_blob_posix fss_io Threads::Threads
  LABELS  phase3 integration)

# C3.7：自签传输 token + /v1/transfer 内核
fss_add_test(test_transfer_endpoint
  SOURCES integration/test_transfer_endpoint.cpp
  LIBS    fss_transfer fss_blob_memory fss_crypto
  LABELS  phase3 integration)

# C10.17（阶段 10 切片 5）：self_signed.key_id 的编码/解码边界（进程内 codec 单测）
fss_add_test(test_transfer_token_key_id
  SOURCES unit/test_transfer_token_key_id.cpp
  LIBS    fss_transfer fss_crypto
  LABELS  phase10 unit)

# C3.4：≥1 GiB 流式写读的 RSS 峰值（规模与上限可用环境变量缩小，见 framework/big_file.h）
fss_add_test(test_posix_large_file
  SOURCES integration/test_posix_large_file.cpp
  LIBS    fss_blob_posix
  LABELS  phase3 integration)

# C3.12：有界写并发（32 线程零 busy）
fss_add_test(test_sqlite_write_concurrency
  SOURCES integration/test_sqlite_write_concurrency.cpp
  LIBS    fss_location_sqlite Threads::Threads
  LABELS  phase3 integration)

# =============================================================================
#  C10.20（本切片）：SQLite **数据库层组提交**（6 个键）
#    · unit：共用协调器 `infra/sqlite/sqlite_group_commit.h` 的**每操作原子性**
#      （真实主键冲突 + "半成品"回滚）与**整批 COMMIT 失败**语义（故障注入）；
#    · integration：两个仓储的摊销 == ceil(N/B)、group_commit=false 逐字回归、
#      max_wait_ms 的等待窗口、读不脏（批事务开着时读必须等待）。
#    时序全部由注入的门控/观察者决定 —— 不靠 sleep 猜批大小。
# =============================================================================
fss_add_test(test_sqlite_group_commit_unit
  SOURCES unit/test_sqlite_group_commit.cpp
  LIBS    SQLite::SQLite3 fss_metrics Threads::Threads
  LABELS  "phase10;unit")

fss_add_test(test_sqlite_group_commit
  SOURCES integration/test_sqlite_group_commit.cpp
  LIBS    fss_location_sqlite fss_metadata_sqlite Threads::Threads
  LABELS  "phase10;integration")

# -----------------------------------------------------------------------------
#  阶段 6：元数据语义完整化
# -----------------------------------------------------------------------------
# C2.10（元数据侧）/C6.5：SqliteMetadataRepository 与内存实现共用同一套契约
fss_add_test(test_sqlite_metadata_repository
  SOURCES integration/test_sqlite_metadata_repository.cpp
  LIBS    fss_metadata_sqlite fss_metadata_memory
  LABELS  "phase6;integration")

# C6.11：多实例幂等（内存 + SQLite 两个连接 + 朴素主键的对照）
fss_add_test(test_multi_instance
  SOURCES integration/test_multi_instance.cpp
  LIBS    fss_app fss_blob_memory fss_location_memory fss_metadata_memory
          fss_metadata_sqlite Threads::Threads
  LABELS  "phase6;integration")

# C8.3 + 租户绑定 + fail-closed：本地 JWT 授权器（P8 切片 1）
fss_add_test(test_jwt_authorizer
  SOURCES unit/test_jwt_authorizer.cpp
  LIBS    fss_auth_local
  LABELS  "phase8;unit")

# C8.2：跨租户隔离（两个租户 + 真实 HTTP + 真实 JWT + 签名 URL 绑对象）
fss_add_test(test_tenant_isolation
  SOURCES integration/test_tenant_isolation.cpp
  LIBS    fss_http_adapter fss_auth_local fss_blob_memory fss_location_memory
          fss_metadata_memory fss_transfer Threads::Threads
  LABELS  "phase8;integration")

# C8.10：时钟偏移护栏（快钟/慢钟 + 参考时钟是唯一时间源）
fss_add_test(test_clock_skew_guard
  SOURCES unit/test_clock_skew_guard.cpp
  LIBS    fss_app
  LABELS  "phase8;unit")

# C9.6 + C9.5/C9.25：指标完整性（含"不含 secret"）+ GC 的 .tmp_* 清理
fss_add_test(test_metrics_and_gc
  SOURCES hardening/test_metrics_and_gc.cpp
  LIBS    fss_http_adapter fss_app fss_blob_posix fss_blob_metered fss_metrics
          fss_location_memory fss_metadata_memory fss_transfer Threads::Threads
  LABELS  "phase9;hardening")

# C9.2：故障注入 + 恢复时间（存储/DB/磁盘满/远端鉴权 + 非致命依赖）
fss_add_test(test_fault_injection
  SOURCES hardening/test_fault_injection.cpp
  LIBS    fss_http_adapter fss_auth_remote fss_app fss_blob_memory fss_location_memory
          fss_metadata_memory fss_transfer Threads::Threads
  LABELS  "phase9;hardening")

# C9.3：6 类资源上限各一个"被拒绝"的测试（真实业务栈）
fss_add_test(test_resource_limits
  SOURCES hardening/test_resource_limits.cpp
  LIBS    fss_http_adapter fss_app fss_blob_memory fss_location_memory fss_metadata_memory
          fss_transfer Threads::Threads
  LABELS  "phase9;hardening")

# C9.1：并发硬化（100 并发 JSON 零 5xx；8 并发大文件逐路校验和）
fss_add_test(test_concurrency
  SOURCES hardening/test_concurrency.cpp
  LIBS    fss_http_adapter fss_app fss_blob_memory fss_blob_posix fss_location_memory
          fss_location_sqlite fss_metadata_memory fss_transfer Threads::Threads
  LABELS  "phase9;hardening")

# P9-D05：数据面（流式路由）的错误响应语义 —— handler 提前报的错不得被"读体失败 400"覆盖
#  ★ 必须链真实驱动与真实编解码器：篡改签名要在 handler 里真的失败（内存替身 `Decode` 恒失败，
#    会让用例无法区分"保留真错误"与"放过所有响应"）。
fss_add_test(test_transfer_error_semantics
  SOURCES hardening/test_transfer_error_semantics.cpp
  LIBS    fss_http_adapter fss_app fss_blob_posix fss_location_sqlite fss_metadata_memory
          fss_transfer Threads::Threads
  LABELS  "phase9;hardening")

# C8.7：审计覆盖（每个受保护用例的成功/失败两侧 + 字段齐全）
fss_add_test(test_audit_coverage
  SOURCES unit/test_audit_coverage.cpp
  LIBS    fss_app fss_blob_memory fss_location_memory fss_metadata_memory
  LABELS  "phase8;unit")

# C8.4：远端 Entitlements 的 fail-closed（超时 / 5xx / 坏 JSON / 未配置 → 一律 503）
fss_add_test(test_remote_entitlements_authorizer
  SOURCES unit/test_remote_entitlements_authorizer.cpp
  LIBS    fss_auth_remote
  LABELS  "phase8;unit")

# C8.1：19 个端点 × 角色的 401/403/200 矩阵（真实 HTTP + 真实 HS256 token）
fss_add_test(test_auth_matrix
  SOURCES integration/test_auth_matrix.cpp
  LIBS    fss_http_adapter fss_auth_local fss_blob_memory fss_location_memory
          fss_metadata_memory fss_transfer Threads::Threads
  LABELS  "phase8;integration")

# C7.2：契约 §5 的错误码双向映射（REST ↔ gRPC 同行、无空缺）
fss_add_test(test_error_equivalence
  SOURCES conformance/test_error_equivalence.cpp
  LIBS    fss_http_adapter fss_grpc_adapter
  LABELS  "phase7;conformance")

# C7.5：proto 的 json_name 与 OSDU JSON 逐字段对齐（用黄金样例做参照实现）
fss_add_test(test_proto_json_mapping
  SOURCES unit/test_proto_json_mapping.cpp
  LIBS    fss_grpc_adapter fss_domain
  LABELS  "phase7;unit")

# C7.3 + C7.4：契约 §6 的 12 行等价性矩阵 + 签名 URL 结构等价（含反向测试）
fss_add_test(test_protocol_equivalence
  SOURCES conformance/test_protocol_equivalence.cpp
  LIBS    fss_app fss_grpc_adapter fss_http_adapter fss_blob_memory fss_location_memory
          fss_metadata_memory fss_transfer Threads::Threads
  LABELS  "phase7;conformance")

# C7.1（部分）：真实 gRPC 端口上的运维 RPC + 切片边界
fss_add_test(test_grpc_basics
  SOURCES integration/test_grpc_basics.cpp
  LIBS    fss_app fss_grpc_adapter fss_blob_memory fss_location_memory fss_metadata_memory
  LABELS  "phase7;integration")

# C7.6 + C7.10：流式上传/下载（1 GiB / 0 字节 / 中途取消）+ 与 HTTP Range 的字节一致
# ★ 必须挂真实 POSIX 存储：内存适配器把对象整体放进 RAM，"流式"就无从度量（P6-D13）
fss_add_test(test_grpc_streaming
  SOURCES integration/test_grpc_streaming.cpp
  LIBS    fss_app fss_grpc_adapter fss_http_adapter fss_blob_posix fss_location_sqlite
          fss_metadata_memory fss_transfer fss_test_support Threads::Threads
  LABELS  "phase7;integration")

# C7.8：**真实二进制**（组合根）两个端口同时服务 + 并发混合协议读写
fss_add_test(test_dual_protocol_concurrency
  SOURCES integration/test_dual_protocol_concurrency.cpp
  LIBS    fss_app fss_grpc_adapter fss_http_adapter fss_blob_memory fss_location_memory
          fss_metadata_memory fss_transfer Threads::Threads
  LABELS  "phase7;integration")
# 该用例把 build/bin/fss_server 拉起来当被测进程 → 构建它必须先于本测试
add_dependencies(test_dual_protocol_concurrency fss_server)

# C6.12：GC 租约（在途不删 / 过期回收 / 原子领取不重复删 / 有记录永不删）
fss_add_test(test_gc_lease
  SOURCES integration/test_gc_lease.cpp
  LIBS    fss_app fss_blob_memory fss_location_memory fss_metadata_memory Threads::Threads
  LABELS  "phase6;integration")

# C2（ADR-009 §4.2/§4.3）：在途租约真的生效 —— 上传路径 Acquire / 慢复制期间 Renew /
# 崩溃者 claiming 行由 GC 按"已原子领取的过期租约"回收 / 跨实例冲突 503 / 默认不接租约。
fss_add_test(test_lease_lifecycle
  SOURCES integration/test_lease_lifecycle.cpp
  LIBS    fss_app fss_blob_memory fss_location_memory fss_metadata_memory Threads::Threads
  LABELS  "phase6;integration")

# C6.13：临时文件名唯一性（含"朴素临时名必错乱"的对照）
fss_add_test(test_posix_tmp_names
  SOURCES integration/test_posix_tmp_names.cpp
  LIBS    fss_blob_posix fss_io Threads::Threads
  LABELS  "phase6;integration")

# C6.7：DMS 6 端点 + Delivery 的响应键集合与端到端流程
fss_add_test(test_dms_delivery
  SOURCES conformance/test_dms_delivery.cpp
  LIBS    fss_app fss_blob_memory fss_location_memory fss_metadata_memory
          fss_http_adapter fss_transfer Threads::Threads
  LABELS  "phase6;conformance")

# C6.6：getFileList 的 Spring Page 语义（字段名 / 分页 / 时间与用户过滤 / 上游负向样例）
fss_add_test(test_file_list
  SOURCES integration/test_file_list.cpp
  LIBS    fss_app fss_blob_memory fss_location_memory fss_metadata_memory
          fss_http_adapter fss_transfer Threads::Threads
  LABELS  "phase6;integration")

# C6.8：鉴权角色常量与"端点 ↔ 角色"映射
fss_add_test(test_roles
  SOURCES unit/test_roles.cpp
  LIBS    fss_app fss_blob_memory fss_location_memory fss_metadata_memory
  LABELS  "phase6;unit")

# C6.4：校验和的 L1 基座（算法解析 / 规范名 / hex 结构 / 增量摘要公开向量）
fss_add_test(test_checksum
  SOURCES unit/test_checksum.cpp
  LIBS    fss_crypto
  LABELS  "phase6;unit")

# C6.4：校验和（服务端计算 / 客户端比对 / 回滚删除 / 流式 RSS）
fss_add_test(test_metadata_lifecycle
  SOURCES integration/test_metadata_lifecycle.cpp
  LIBS    fss_app fss_blob_memory fss_blob_posix fss_location_memory fss_metadata_memory
          fss_crypto fss_io fss_http_adapter fss_transfer Threads::Threads
  LABELS  "phase6;integration")

# -----------------------------------------------------------------------------
#  阶段 5：对象存储驱动（S3 SigV4）
# -----------------------------------------------------------------------------
# C5.1/C5.5（单元部分）：SigV4 签名器、编码边界、签名覆盖性
fss_add_test(test_sigv4_signer
  SOURCES unit/test_sigv4_signer.cpp
  LIBS    fss_blob_s3
  LABELS  "phase5;unit")

# C5.1：AWS 官方已知答案向量（来自 AWS Go SDK 的签名测试，逐字节比对）
fss_add_test(test_sigv4_aws_vectors
  SOURCES unit/test_sigv4_aws_vectors.cpp
  LIBS    fss_blob_s3
  LABELS  "phase5;unit")

# C5.1/C5.5（独立实现对拍）：libcurl `--aws-sigv4` + Python 参照实现
fss_add_test(test_sigv4_crosscheck
  SOURCES integration/test_sigv4_crosscheck.cpp
  LIBS    fss_blob_s3
  LABELS  "phase5;integration")

# C5.2：预签名/头部签名被 mock-S3（Python 独立验签）验过 + 篡改矩阵
fss_add_test(test_s3_presign_verify
  SOURCES integration/test_s3_presign_verify.cpp
  LIBS    fss_blob_s3
  LABELS  "phase5;integration")

# C5.3/C5.6/C5.7：S3BlobStore 跑同一套端口契约 + 分页 + 错误映射
fss_add_test(test_s3_blob_store
  SOURCES integration/test_s3_blob_store.cpp
  LIBS    fss_blob_s3
  LABELS  "phase5;integration")

# C5.8：S3 模式端到端（客户端直连存储端点，服务不代理字节）
fss_add_test(test_upload_flow_s3
  SOURCES integration/test_upload_flow_s3.cpp
  LIBS    fss_http_adapter fss_blob_s3 fss_blob_memory fss_location_memory fss_metadata_memory
          fss_transfer
          Threads::Threads
  LABELS  "phase5;integration")

# -----------------------------------------------------------------------------
#  阶段 4：REST 适配层 + 端到端垂直切片（POSIX）
# -----------------------------------------------------------------------------
# C4.5：ErrorKind → (status, reason) 权威表 + 三种错误体形态
fss_add_test(test_http_error_mapper
  SOURCES unit/test_http_error_mapper.cpp
  LIBS    fss_http_adapter
  LABELS  phase4 unit)

# C4.6 + C4.1/4.2/4.5/4.7 的 HTTP 侧：真实回环端口上的运维/位置/元数据
fss_add_test(test_ops_endpoints
  SOURCES conformance/test_ops_endpoints.cpp
  LIBS    fss_http_adapter fss_blob_memory fss_location_memory fss_metadata_memory Threads::Threads
  LABELS  "phase4;conformance")

# C4.4：DTO 字段大小写逐字符（线上契约）
fss_add_test(test_http_dto
  SOURCES unit/test_http_dto.cpp
  LIBS    fss_http_adapter
  LABELS  phase4 unit)

# C4.9：组合根纪律护栏（R12；自证见 scripts/verify_composition_root.sh）
fss_add_test(test_composition_root_guard
  SOURCES unit/test_composition_root_guard.cpp
  LABELS  phase4 unit)

# C4.1/C4.2/C4.3/C4.7 + §7 `/metrics`：契约 §2/§3.3/§3.4 在真实端口上的逐条比对
fss_add_test(test_rest_contract
  SOURCES conformance/test_rest_contract.cpp
  LIBS    fss_http_adapter fss_transfer fss_blob_memory fss_location_memory fss_metadata_memory
          Threads::Threads
  LABELS  "phase4;conformance")

# C4.11：数据面无整体超时 + 空闲超时断开（把空闲超时压到 2s 以放进日常门槛）
fss_add_test(test_transfer_timeouts
  SOURCES integration/test_transfer_timeouts.cpp
  LIBS    fss_http_adapter fss_transfer fss_blob_memory fss_location_memory fss_metadata_memory
          Threads::Threads
  LABELS  "phase4;integration")

# C4.5（HTTP 侧）：三种错误体形态 + 固定消息 + `X-FSS-Error-Kind`
fss_add_test(test_error_formats
  SOURCES conformance/test_error_formats.cpp
  LIBS    fss_http_adapter fss_transfer fss_blob_memory fss_location_memory fss_metadata_memory
          Threads::Threads
  LABELS  "phase4;conformance")

# C4.8：端到端垂直切片（上传自签 URL → PUT 真实字节 → 元数据 → 下载 → SHA-256 比对 → 删除）
fss_add_test(test_upload_flow
  SOURCES integration/test_upload_flow_posix.cpp
  LIBS    fss_http_adapter fss_transfer fss_blob_memory fss_location_memory fss_metadata_memory
          fss_blob_posix fss_location_sqlite
          Threads::Threads
  LABELS  "phase4;integration")

# ADR-010：UringIoEngine 探测骨架（不启用，显式拒绝）
fss_add_test(test_uring_io_engine
  SOURCES unit/test_uring_io_engine.cpp
  LIBS    fss_io
  LABELS  phase3 unit)

# C9.9：运维文档完整性 —— docs/operations.md 必须覆盖 config/fss.example.json 的每个叶子键
# （正向覆盖 + 反向"不得写不存在的键"，均用 CoreSchema().IsAllowedPath 判定）
fss_add_test(test_operations_doc
  SOURCES unit/test_operations_doc.cpp
  LIBS    fss_config fss_json
  LABELS  "phase9;docs")

# =============================================================================
#  C9.31（P9 补交，P10 期间完成）：按需 GC 的 HTTP 端点 `POST {base}/v2/gc:run`
#    真实 `build/bin/fss_server`：正例（够旧 .tmp.* 真的消失）/ dry-run 只能更保守 /
#    授权矩阵 401/403/200（admin 不需要 data-partition-id）/ 审计两侧（gcRun）/
#    `scheduled` 字段 / `/metrics` 的 `fss_gc_runs_total` 真的涨。
# =============================================================================
fss_add_test(test_gc_endpoint
  SOURCES integration/test_gc_endpoint.cpp
  LIBS    fss_app fss_crypto fss_json Threads::Threads
  LABELS  "phase9;integration")
# 被测进程就是 build/bin/fss_server → 构建它必须先于本测试
add_dependencies(test_gc_endpoint fss_server)

# =============================================================================
#  C9.32（P9 补交，P10 期间完成）：**未预期异常必须以干净退出码结束**
#    真实 `build/bin/fss_server` + 故障注入接缝 `FSS_STARTUP_FAULT_INJECT`
#    （环境变量，**不是配置键**）：
#    ① `throw_system_error`（复刻 pids EAGAIN）→ exit 70 + 可读原因 + 无 terminate
#    ② `throw_bad_alloc` → exit 70 + 可读原因
#    ③ `throw_unknown`（非 std 类型）→ exit 70 + 「未知类型」（证明 catch (...) 生效）
#    ④ **不注入** → readiness 200 且不出现「未预期异常」（R16 正例）
#    ⑤ `throw_after_start`（服务器已起 + GC 调度已在跑之后抛）→ exit 70、<5 s 退出
#       （证明 RAII 清理不卡 join）
#    ⑥ 注入点在 CLI 解析之后（`--help` 仍 exit 0）
# =============================================================================
fss_add_test(test_startup_faults
  SOURCES integration/test_startup_faults.cpp
  LIBS    Threads::Threads
  LABELS  "phase9;integration")
# 被测进程就是 build/bin/fss_server → 构建它必须先于本测试
add_dependencies(test_startup_faults fss_server)

# C9.31（P9 补交）：`GcTask::Run` 的**单飞护栏**（L4 单测；用会阻塞的 blob store 替身
# 把"确实还在跑"变成确定性判据，而不是 sleep 猜时序）
fss_add_test(test_gc_task_single_flight
  SOURCES unit/test_gc_task_single_flight.cpp
  LIBS    fss_app fss_blob_memory fss_location_memory fss_metadata_memory Threads::Threads
  LABELS  "phase9;unit")

# =============================================================================
#  阶段 10 切片 1：配置面接线（C10.1~C10.8）—— 全部在**真实二进制**上断言
#    server_process.h 负责拉起 build/bin/fss_server（可传 --config/--set/环境变量），
#    反向用例用 RunServerForExit() 拿退出码与输出。
# =============================================================================
fss_add_test(test_config_wiring
  SOURCES integration/test_config_wiring.cpp
  LIBS    fss_app fss_grpc_adapter fss_blob_posix fss_io Threads::Threads
  LABELS  "phase10;integration")
# C10.15：本文件用真实 gRPC channel 调 GetInfo → 需要 gRPC/proto 头与库；
# 同时被测进程就是 build/bin/fss_server，构建它必须先于本测试
add_dependencies(test_config_wiring fss_server)

# =============================================================================
#  阶段 10 切片 6a：远端 legal / schema 校验器（C10.18 / ADR-013）
#    真实 `build/bin/fss_server` + 独立进程 mock（`tests/tools/mock_validators.py`）：
#    正例 / 远端不通过 / fail-closed 五态 / 无残留 / 启动拒绝 / noop 不发请求 /
#    timeout_ms 真的来自配置。
# =============================================================================
fss_add_test(test_remote_validators
  SOURCES integration/test_remote_validators.cpp
  LIBS    fss_json Threads::Threads
  LABELS  "phase10;integration")
add_dependencies(test_remote_validators fss_server)

# =============================================================================
#  阶段 10 切片 6b：事件发布器 `events.publisher` / `events.webhook.*`（C10.19 / ADR-013 §9）
#    真实 `build/bin/fss_server` + 独立进程 mock（同一个 `mock_validators.py`，
#    `--mode webhook`）：正例（两个事件 + 配置 topic）/ **非致命三态**（连不上、非 2xx、
#    超时 → 请求仍 201 且记录真的建出来）/ `none` 不发请求 / `log` 不受影响 /
#    空 url → exit 78 / `timeout_ms` 真的来自配置。
# =============================================================================
fss_add_test(test_webhook_publisher
  SOURCES integration/test_webhook_publisher.cpp
  LIBS    fss_json Threads::Threads
  LABELS  "phase10;integration")
add_dependencies(test_webhook_publisher fss_server)

# =============================================================================
#  C9.30 的**第三半**（P10 期间补交）：`/v2/info` 与指标暴露
#  `ioEngine` / `ioUringAvailable`（ADR-010 的 R11；语义见文件头的"可用 ≠ 已启用"）。
#    ① REST：真 JSON 解析（python3 `json`）断言字段**存在**+**类型**+取值，既有字段正控；
#    ② gRPC 与 REST **同源**（真实进程两条协议各取一次比对 + 进程内非默认值双向对照）；
#    ③ `/metrics`：`fss_io_uring_available 0|1` 与既有 `fss_io_engine{...}` 同时存在；
#    ④⑤ 探测注入接缝 `FSS_IO_PROBE_INJECT=available|blocked`（环境变量，**不是配置键**）
#       使字段**双向**翻转 ⇒ 既不是恒 false 也不是恒 true；且 `storage.io_engine=uring`
#       在注入下**仍拒绝启动**（U1~U4 不受影响）；
#    ⑥ 三处渲染一致（`/v2/info` 的 `ioEngine` == 启动横幅 == `fss_io_engine` 指标）；
#    ⑦ 反向：该接缝不是配置键（`--set` 未知键 → exit 78）。
# =============================================================================
fss_add_test(test_io_engine_exposure
  SOURCES integration/test_io_engine_exposure.cpp
  LIBS    fss_app fss_grpc_adapter fss_http_adapter fss_blob_memory fss_location_memory
          fss_metadata_memory fss_transfer Threads::Threads
  LABELS  "phase9;integration")
# 被测进程就是 build/bin/fss_server → 构建它必须先于本测试
add_dependencies(test_io_engine_exposure fss_server)
