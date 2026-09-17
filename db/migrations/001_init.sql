-- =============================================================================
--  Migration 001 —— 初始 schema
-- =============================================================================
--  说明
--  ---------------------------------------------------------------------------
--  * 本文件只使用**幂等**语句（IF NOT EXISTS / OR REPLACE），可安全重复执行。
--  * 迁移由 scripts/dev_postgres.sh 按文件名顺序应用，并记录在 schema_migrations。
--  * 每个对象都标注了它对应的设计依据（ADR），便于追溯"为什么有这张表"。
--  * 单实例模式（SQLite）下不需要本文件；多实例模式（deployment.mode=multi）
--    强制要求 metadata/location 仓储为 postgres（见 ADR-009 §8.1）。
-- =============================================================================

-- -----------------------------------------------------------------------------
--  迁移版本表（供 readiness 的 schema_version_check 使用）
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS schema_migrations (
  version     INTEGER     PRIMARY KEY,
  name        TEXT        NOT NULL,
  applied_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- -----------------------------------------------------------------------------
--  位置记录  ——  ADR-004 §9.1
-- -----------------------------------------------------------------------------
--  相比上游 file_locations_osm（只有 id 单列唯一），这里显式加入 partition_id 维度：
--  UUID 碰撞概率极低，但契约上不应依赖概率。
CREATE TABLE IF NOT EXISTS file_locations (
  partition_id  TEXT        NOT NULL,
  file_id       TEXT        NOT NULL,
  file_source   TEXT        NOT NULL,   -- 对客户端可见的相对路径（含前导斜杠）
  container     TEXT        NOT NULL,   -- 物理容器（POSIX 目录 / S3 bucket）
  object_key    TEXT        NOT NULL,   -- 物理键
  zone          TEXT        NOT NULL,   -- 'staging' | 'persistent'
  driver        TEXT        NOT NULL,   -- 'posix' | 's3'
  created_by    TEXT        NOT NULL,
  created_at    BIGINT      NOT NULL,   -- epoch 秒
  migrated_at   BIGINT,                 -- 迁移到 persistent 的时间
  data          JSONB       NOT NULL DEFAULT '{}'::jsonb,  -- 未知字段原样保留（前向兼容）
  PRIMARY KEY (partition_id, file_id)
);

-- 幂等键：同一租户下同一 fileSource 只能有一条位置记录（ADR-009 §4.2）
CREATE UNIQUE INDEX IF NOT EXISTS ux_fl_source
  ON file_locations (partition_id, file_source);

-- getFileList 的时间区间 + 分页查询
CREATE INDEX IF NOT EXISTS ix_fl_partition_created
  ON file_locations (partition_id, created_at DESC);

CREATE INDEX IF NOT EXISTS ix_fl_zone
  ON file_locations (partition_id, zone);

-- -----------------------------------------------------------------------------
--  元数据记录  ——  ADR-004 §9.2 + ADR-009 §4.2
-- -----------------------------------------------------------------------------
--  state 状态机：claiming → ready → deleted
--    claiming 由"原子领取"产生（ADR-009 §4.2）；领取者崩溃时由租约到期后回收。
CREATE TABLE IF NOT EXISTS file_metadata_records (
  partition_id   TEXT        NOT NULL,
  id             TEXT        NOT NULL,   -- "<partition>:dataset--File.Generic:<uuid>"
  version        INTEGER     NOT NULL,
  kind           TEXT        NOT NULL,
  state          TEXT        NOT NULL DEFAULT 'ready'
                             CHECK (state IN ('claiming','ready','deleted')),
  is_latest      BOOLEAN     NOT NULL DEFAULT TRUE,
  created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
  created_by     TEXT        NOT NULL,
  acl_viewers    JSONB       NOT NULL DEFAULT '[]'::jsonb,
  acl_owners     JSONB       NOT NULL DEFAULT '[]'::jsonb,
  legal_tags     JSONB       NOT NULL DEFAULT '[]'::jsonb,
  file_source    TEXT,                    -- data.DatasetProperties.FileSourceInfo.FileSource
  data           JSONB       NOT NULL,    -- 完整记录 JSON（信封 + data）
  PRIMARY KEY (partition_id, id, version)
);

-- ★ 幂等键：必须建在 file_source 上，而不是随机主键 id 上
--   （本项目曾把约束建在 UUID 主键上导致幂等完全失效，见 ADR-009 §3 M2）
--
-- ⚠️ 关键细节：谓词必须包含 `is_latest`。
--   若只写 `WHERE state <> 'deleted'`，会**阻断合法的版本链**——
--   OSDU 中同一记录的新版本共享同一个 FileSource 与同一个记录 id，
--   于是第二个版本会被幂等约束挡掉（本项目的 schema 自检 db/tests/001 抓到过这个问题）。
--   加上 `is_latest` 后语义变为："**活跃且最新**的记录里，fileSource 唯一"：
--     · 重复登记（重试）      → 命中约束，被幂等处理  ✅
--     · 同一 id 的新版本      → 先把旧版本置 is_latest=FALSE，再插入 → 允许 ✅
DROP INDEX IF EXISTS ux_mr_source;
CREATE UNIQUE INDEX IF NOT EXISTS ux_mr_source
  ON file_metadata_records (partition_id, file_source)
  WHERE state <> 'deleted' AND is_latest;

-- 同一 id 只能有一条 latest
CREATE UNIQUE INDEX IF NOT EXISTS ux_mr_latest
  ON file_metadata_records (partition_id, id)
  WHERE is_latest;

CREATE INDEX IF NOT EXISTS ix_mr_source
  ON file_metadata_records (partition_id, file_source);

CREATE INDEX IF NOT EXISTS ix_mr_state
  ON file_metadata_records (partition_id, state);

-- -----------------------------------------------------------------------------
--  在途租约  ——  ADR-009 §4.3
-- -----------------------------------------------------------------------------
--  解决"GC 与在途上传对撞"：GC 只回收【租约已过期 且 无元数据记录】的对象。
--  时间基准一律用数据库的 now()，避免实例间时钟偏移导致误判（ADR-009 §6.4）。
CREATE TABLE IF NOT EXISTS staging_leases (
  partition_id  TEXT        NOT NULL,
  file_source   TEXT        NOT NULL,
  owner         TEXT        NOT NULL,           -- instance_id
  acquired_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
  renewed_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
  expires_at    TIMESTAMPTZ NOT NULL,           -- 续租：now() + ttl
  PRIMARY KEY (partition_id, file_source)
);

CREATE INDEX IF NOT EXISTS ix_lease_expiry
  ON staging_leases (expires_at);

-- -----------------------------------------------------------------------------
--  传输 token 的 nonce（可选）  ——  ADR-009 §4.6
-- -----------------------------------------------------------------------------
--  仅在 self_signed.single_use_nonce=true 且 nonce_store=postgres 时使用。
--  默认配置下该特性关闭（本地 nonce 表在多实例下无法拒绝重放）。
CREATE TABLE IF NOT EXISTS transfer_nonces (
  key_id    TEXT        NOT NULL,
  nonce     TEXT        NOT NULL,
  used_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
  expires_at TIMESTAMPTZ NOT NULL,
  PRIMARY KEY (key_id, nonce)
);

CREATE INDEX IF NOT EXISTS ix_nonce_expiry
  ON transfer_nonces (expires_at);

-- -----------------------------------------------------------------------------
--  实例注册表  ——  ADR-009 §5.3（readiness 的配置版本一致性）
-- -----------------------------------------------------------------------------
--  滚动升级时防止"新旧版本混杂"：readiness 比对本实例的 config_hash 与当前
--  schema/config 版本；不一致则不上流量。
CREATE TABLE IF NOT EXISTS instance_registry (
  instance_id   TEXT        PRIMARY KEY,
  service_version TEXT      NOT NULL,
  config_hash   TEXT        NOT NULL,
  started_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
  heartbeat_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- -----------------------------------------------------------------------------
--  领导者选举的观测表（可选）  ——  ADR-009 §4.4
-- -----------------------------------------------------------------------------
--  真正的互斥用 pg_try_advisory_lock（会话级锁，连接断开自动释放）。
--  这张表只用于"谁在持有锁"的可观测性，不承担互斥职责。
CREATE TABLE IF NOT EXISTS leader_state (
  task_name     TEXT        PRIMARY KEY,
  instance_id   TEXT,
  acquired_at   TIMESTAMPTZ,
  heartbeat_at  TIMESTAMPTZ
);

-- -----------------------------------------------------------------------------
--  可选视图：便于人工排障（不是契约的一部分）
-- -----------------------------------------------------------------------------
CREATE OR REPLACE VIEW v_staging_summary AS
SELECT l.partition_id,
       count(*)                                                  AS staging_objects,
       count(*) FILTER (WHERE lz.expires_at IS NULL)             AS without_lease,
       count(*) FILTER (WHERE lz.expires_at < now())             AS lease_expired,
       count(*) FILTER (WHERE m.file_source IS NOT NULL)         AS with_metadata
  FROM file_locations l
  LEFT JOIN staging_leases lz
         ON lz.partition_id = l.partition_id AND lz.file_source = l.file_source
  LEFT JOIN file_metadata_records m
         ON m.partition_id = l.partition_id AND m.file_source = l.file_source
        AND m.state <> 'deleted'
 WHERE l.zone = 'staging'
 GROUP BY l.partition_id;
