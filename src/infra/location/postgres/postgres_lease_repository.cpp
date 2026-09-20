// PostgresLeaseRepository 实现。端口签名与 ADR-009 §4.3 的阻抗匹配见头文件。
#include "infra/location/postgres/postgres_lease_repository.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace fss::infra {

namespace {

//  ---- SQL（全部写成 raw string；每条 DML 都带 partition_id）----
//  ★ 带值的 SQL 一律用 `PQexecParams` 占位符（$1..$n），不拼字符串。
//  ★ `make_interval(secs => …)` 自 PG 9.4 起可用（本文件必须能跑在 PG 12 与 14 上）。
//  ★ TTL/过期一律由数据库 `now()` 计算（ADR-009 §6.4）。
constexpr const char* kAcquire = R"sql(
INSERT INTO staging_leases
  (partition_id, file_source, owner, acquired_at, renewed_at, expires_at)
VALUES ($1, $2, $3, now(), now(), now() + make_interval(secs => $4::bigint / 1000.0))
ON CONFLICT (partition_id, file_source) DO UPDATE
   SET owner      = EXCLUDED.owner,
       renewed_at = EXCLUDED.renewed_at,
       expires_at = EXCLUDED.expires_at
 WHERE staging_leases.expires_at <= now()
RETURNING owner, (extract(epoch from expires_at) * 1000)::bigint)sql";

constexpr const char* kRenew = R"sql(
UPDATE staging_leases
   SET expires_at = now() + make_interval(secs => $4::bigint / 1000.0),
       renewed_at = now()
 WHERE partition_id = $1 AND file_source = $2 AND owner = $3
RETURNING owner)sql";

constexpr const char* kRelease = R"sql(
DELETE FROM staging_leases
 WHERE partition_id = $1 AND file_source = $2 AND owner = $3
RETURNING owner)sql";

constexpr const char* kSelectOwner =
    R"sql(SELECT owner FROM staging_leases WHERE partition_id = $1 AND file_source = $2)sql";

//  ★ 原子领取（ADR-009 §4.3 的 M3 修复）：
//    ① `expired`：只挑本 partition 下已过期的行，`FOR UPDATE SKIP LOCKED` +
//       `LIMIT` —— 并发领取者**跳过**别人正在处理的行而不是排队等待（非阻塞）；
//    ② `claimed`：同一事务内改 owner、把过期时间推后 60 秒（与内存实现一致）；
//    ③ 外层再 `LEFT JOIN file_locations` 反解 `file_id`（没有位置记录则留空，
//       GC 会当作"查不到位置"跳过 —— 安全方向）。
constexpr const char* kClaimExpired = R"sql(
WITH expired AS (
  SELECT partition_id, file_source
    FROM staging_leases
   WHERE partition_id = $1 AND expires_at <= now()
   ORDER BY expires_at ASC, file_source ASC
   LIMIT $2::int
   FOR UPDATE SKIP LOCKED
),
claimed AS (
  UPDATE staging_leases lease
     SET owner = $3,
         renewed_at = now(),
         expires_at = now() + interval '60 seconds'
    FROM expired
   WHERE lease.partition_id = expired.partition_id
     AND lease.file_source = expired.file_source
  RETURNING lease.partition_id, lease.file_source, lease.owner,
            (extract(epoch from lease.expires_at) * 1000)::bigint AS expires_millis
)
SELECT claimed.partition_id,
       claimed.file_source,
       claimed.owner,
       claimed.expires_millis,
       location.file_id
  FROM claimed
  LEFT JOIN file_locations location
    ON location.partition_id = claimed.partition_id
   AND location.file_source = claimed.file_source)sql";

std::int64_t ParseMillis(const std::string& text) {
  if (text.empty()) return 0;
  return static_cast<std::int64_t>(std::strtoll(text.c_str(), nullptr, 10));
}

// =============================================================================
//  ★ C2：`leases.time_source=local` 的变体（时间基准 = 注入的 `IClock`）
// =============================================================================
//  与 database 变体**逐字同构**，只把 3 处 `now()` 换成 `to_timestamp($N::bigint)`：
//    · `$5` = `clock.NowEpochSeconds()`（Acquire/Renew/ClaimExpired 各多带一个参数）；
//    · `expires_at = to_timestamp($5 + ttl_ms/1000)`（整秒精度，与 `to_timestamp(clock+ttl)` 一致）。
//  ★ 三个方法必须用**同一个**时间基准，否则 `Acquire` 写进去的 `expires_at` 与
//    `ClaimExpired` 的比较会互相矛盾（本切片的 time_source 测试就是钉这一点）。
constexpr const char* kAcquireLocal = R"sql(
INSERT INTO staging_leases
  (partition_id, file_source, owner, acquired_at, renewed_at, expires_at)
VALUES ($1, $2, $3, to_timestamp($5::bigint), to_timestamp($5::bigint),
        to_timestamp($5::bigint + $4::bigint / 1000))
ON CONFLICT (partition_id, file_source) DO UPDATE
   SET owner      = EXCLUDED.owner,
       renewed_at = EXCLUDED.renewed_at,
       expires_at = EXCLUDED.expires_at
 WHERE staging_leases.expires_at <= to_timestamp($5::bigint)
RETURNING owner, (extract(epoch from expires_at) * 1000)::bigint)sql";

constexpr const char* kRenewLocal = R"sql(
UPDATE staging_leases
   SET expires_at = to_timestamp($5::bigint + $4::bigint / 1000),
       renewed_at = to_timestamp($5::bigint)
 WHERE partition_id = $1 AND file_source = $2 AND owner = $3
RETURNING owner)sql";

constexpr const char* kClaimExpiredLocal = R"sql(
WITH expired AS (
  SELECT partition_id, file_source
    FROM staging_leases
   WHERE partition_id = $1 AND expires_at <= to_timestamp($4::bigint)
   ORDER BY expires_at ASC, file_source ASC
   LIMIT $2::int
   FOR UPDATE SKIP LOCKED
),
claimed AS (
  UPDATE staging_leases lease
     SET owner = $3,
         renewed_at = to_timestamp($4::bigint),
         expires_at = to_timestamp($4::bigint + 60)
    FROM expired
   WHERE lease.partition_id = expired.partition_id
     AND lease.file_source = expired.file_source
  RETURNING lease.partition_id, lease.file_source, lease.owner,
            (extract(epoch from lease.expires_at) * 1000)::bigint AS expires_millis
)
SELECT claimed.partition_id,
       claimed.file_source,
       claimed.owner,
       claimed.expires_millis,
       location.file_id
  FROM claimed
  LEFT JOIN file_locations location
    ON location.partition_id = claimed.partition_id
   AND location.file_source = claimed.file_source)sql";

}  // namespace

fss::Result<std::unique_ptr<PostgresLeaseRepository>> PostgresLeaseRepository::Open(
    PostgresLeaseRepositoryOptions options) {
  if (options.time_source == LeaseTimeSource::kLocal && options.clock == nullptr) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "leases.time_source=local 需要注入 IClock（组合根未提供）");
  }
  FSS_TRY(pool, PgPool::Create(std::move(options.pg)));
  std::unique_ptr<PostgresLeaseRepository> repository(new PostgresLeaseRepository());
  repository->time_source_ = options.time_source;
  repository->clock_ = options.clock;
  repository->pool_ = std::move(pool);
  return Ok(std::move(repository));
}

PostgresLeaseRepository::~PostgresLeaseRepository() = default;

fss::Error PostgresLeaseRepository::OwnerMismatch(std::string_view partition,
                                                  std::string_view lease_key,
                                                  PgConnection& connection) {
  const auto existing = connection.ExecParams(
      kSelectOwner, {std::string(partition), std::string(lease_key)});
  if (!existing.ok()) return Annotate(existing.error(), "查询租约失败");
  if (existing.value().RowCount() == 0) {
    return Err(fss::ErrorKind::kNotFound, "租约不存在");
  }
  return Err(fss::ErrorKind::kPermissionDenied, "租约属于别的实例");
}

fss::Result<domain::ILeaseRepository::Lease> PostgresLeaseRepository::Acquire(
    std::string_view partition, std::string_view file_id, std::string_view owner_instance_id,
    std::int64_t ttl_millis) {
  FSS_TRY(handle, pool_->Borrow());
  const bool local = time_source_ == LeaseTimeSource::kLocal;
  const std::string now_seconds =
      local ? std::to_string(clock_->NowEpochSeconds()) : std::string();
  std::vector<PgConnection::Param> params{std::string(partition), std::string(file_id),
                                          std::string(owner_instance_id),
                                          std::to_string(ttl_millis)};
  if (local) params.emplace_back(now_seconds);
  const auto result = handle->ExecParams(local ? kAcquireLocal : kAcquire, params);
  if (!result.ok()) return Annotate(result.error(), "写入租约失败");  //  冲突且未过期 → `DO UPDATE ... WHERE false` 不产生行：与内存实现的
  //  "租约已被占用"（kUnavailable）一致。
  if (result.value().RowCount() == 0) {
    return Err(fss::ErrorKind::kUnavailable, "租约已被占用");
  }
  Lease lease;
  lease.file_id = std::string(file_id);
  //  ★ 表身份列就是 file_source（文件头的阻抗匹配）：这里显式回填租约键。
  //    内存实现留空（它不区分两者），因此**共用的契约测试不对 file_source 断言**。
  lease.file_source = std::string(file_id);
  lease.owner_instance_id = std::string(owner_instance_id);
  lease.expires_at_epoch_millis = ParseMillis(result.value().Value(0, 1));
  return lease;
}

fss::Result<void> PostgresLeaseRepository::Renew(std::string_view partition,
                                                 std::string_view file_id,
                                                 std::string_view owner_instance_id,
                                                 std::int64_t ttl_millis) {
  FSS_TRY(handle, pool_->Borrow());
  const bool local = time_source_ == LeaseTimeSource::kLocal;
  std::vector<PgConnection::Param> params{std::string(partition), std::string(file_id),
                                          std::string(owner_instance_id),
                                          std::to_string(ttl_millis)};
  if (local) params.emplace_back(std::to_string(clock_->NowEpochSeconds()));
  const auto result = handle->ExecParams(local ? kRenewLocal : kRenew, params);
  if (!result.ok()) return Annotate(result.error(), "续租失败");
  //  `UPDATE ... RETURNING` 的结果状态是 TUPLES_OK → 用返回行数判定是否命中（0 = 未命中）。
  if (result.value().RowCount() == 0) {
    //  没更新到任何行：区分"租约不存在"与"不是自己的租约"（后者 kPermissionDenied）。
    return OwnerMismatch(partition, file_id, *handle);
  }
  return Ok();
}

fss::Result<void> PostgresLeaseRepository::Release(std::string_view partition,
                                                   std::string_view file_id,
                                                   std::string_view owner_instance_id) {
  FSS_TRY(handle, pool_->Borrow());
  const auto result =
      handle->ExecParams(kRelease, {std::string(partition), std::string(file_id),
                                    std::string(owner_instance_id)});
  if (!result.ok()) return Annotate(result.error(), "释放租约失败");
  //  `DELETE ... RETURNING` 的结果状态是 TUPLES_OK → 用返回行数判定是否命中（0 = 未命中）。
  if (result.value().RowCount() == 0) {
    return OwnerMismatch(partition, file_id, *handle);
  }
  return Ok();
}

fss::Result<std::vector<domain::ILeaseRepository::Lease>> PostgresLeaseRepository::ClaimExpired(
    std::string_view partition, int limit, std::string_view claimant_instance_id) {
  std::vector<Lease> claimed;
  if (limit <= 0) return claimed;  // 与内存实现一致：limit <= 0 → 空结果（不是错误）
  FSS_TRY(handle, pool_->Borrow());
  const bool local = time_source_ == LeaseTimeSource::kLocal;
  std::vector<PgConnection::Param> params{std::string(partition), std::to_string(limit),
                                          std::string(claimant_instance_id)};
  if (local) params.emplace_back(std::to_string(clock_->NowEpochSeconds()));
  const auto result = handle->ExecParams(local ? kClaimExpiredLocal : kClaimExpired, params);
  if (!result.ok()) return Annotate(result.error(), "领取过期租约失败");
  for (int row = 0; row < result.value().RowCount(); ++row) {
    Lease lease;
    //  file_id 由 `LEFT JOIN file_locations` 反解；没有位置记录时为空串。
    lease.file_id = result.value().Value(row, 4);
    lease.file_source = result.value().Value(row, 1);
    lease.owner_instance_id = result.value().Value(row, 2);
    lease.expires_at_epoch_millis = ParseMillis(result.value().Value(row, 3));
    claimed.push_back(std::move(lease));
  }
  return claimed;
}

}  // namespace fss::infra
