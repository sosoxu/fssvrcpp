-- =============================================================================
--  db/tests/001_verify_invariants.sql —— schema 不变量自检
-- =============================================================================
--  用法：
--     scripts/dev_postgres.sh migrate
--     scripts/dev_postgres.sh psql -v ON_ERROR_STOP=1 -f db/tests/001_verify_invariants.sql
--
--  设计：任何不变量被破坏 → RAISE EXCEPTION → psql 以非 0 退出 → 门槛失败。
--        所有断言都在一个事务里，结束时 ROLLBACK，**不污染数据库**。
--
--  这些断言不是"测试 SQL 语法"，而是验证 schema **真的**强制了 ADR-009 的约束。
--  若某条约束被误删（例如把唯一索引建在随机主键上），本脚本会立刻失败。
-- =============================================================================
\set ON_ERROR_STOP on

BEGIN;

DO $$
DECLARE
  n       INTEGER;
  got     TEXT;
  lock1   BOOLEAN;
  lock2   BOOLEAN;
  id1     TEXT;
  id2     TEXT;
  leaked  INTEGER;
BEGIN
  ---------------------------------------------------------------------------
  -- 前置：不变量自检本身要能失败（自证）
  --   若下面的"应当失败"用例没有失败，说明约束缺失。
  ---------------------------------------------------------------------------

  ---------------------------------------------------------------------------
  -- I1  幂等键唯一：同一 (partition_id, file_source) 只能有一条未删除记录
  --     ★ 这是 ADR-009 M2 的核心修复。约束必须建在 file_source 上，
  --       不能建在随机主键 id 上。
  ---------------------------------------------------------------------------
  INSERT INTO file_metadata_records(partition_id,id,version,kind,state,created_by,file_source,data)
  VALUES ('p1','rec-a',1,'p1:wks:dataset--File.Generic:1.0.0','ready','u','/fs/x','{}'::jsonb);

  BEGIN
    INSERT INTO file_metadata_records(partition_id,id,version,kind,state,created_by,file_source,data)
    VALUES ('p1','rec-b',1,'p1:wks:dataset--File.Generic:1.0.0','ready','u','/fs/x','{}'::jsonb);
    RAISE EXCEPTION 'I1 失败：重复的 (partition_id,file_source) 被接受了 —— ux_mr_source 缺失或键错误';
  EXCEPTION WHEN unique_violation THEN
    NULL;  -- 预期
  END;

  -- 冲突目标必须支持 ON CONFLICT（部分唯一索引必须先被识别）
  BEGIN
    INSERT INTO file_metadata_records(partition_id,id,version,kind,state,is_latest,created_by,file_source,data)
    VALUES ('p1','rec-c',1,'k','ready',TRUE,'u','/fs/x','{}'::jsonb)
    ON CONFLICT (partition_id,file_source) WHERE state <> 'deleted' AND is_latest DO NOTHING
    RETURNING id INTO got;
    IF got IS NOT NULL THEN
      RAISE EXCEPTION 'I1 失败：ON CONFLICT 未生效，仍插入了 id=%', got;
    END IF;
  EXCEPTION WHEN unique_violation THEN
    RAISE EXCEPTION 'I1 失败：ON CONFLICT (partition_id,file_source) WHERE ... AND is_latest 推断失败（部分索引不匹配）';
  END;

  -- 软删除后允许重新登记（部分索引 WHERE state <> ''deleted''）
  UPDATE file_metadata_records SET state='deleted' WHERE partition_id='p1' AND file_source='/fs/x';
  INSERT INTO file_metadata_records(partition_id,id,version,kind,state,created_by,file_source,data)
  VALUES ('p1','rec-d',1,'k','ready','u','/fs/x','{}'::jsonb);
  SELECT count(*) INTO n FROM file_metadata_records
   WHERE partition_id='p1' AND file_source='/fs/x' AND state <> 'deleted';
  IF n <> 1 THEN RAISE EXCEPTION 'I1 失败：软删除后重新登记，活跃记录数应为 1，实际 %', n; END IF;

  ---------------------------------------------------------------------------
  -- I2  原子领取语义（ADR-009 §4.2）：模拟两实例并发领取，只有一个能拿到
  --     `INSERT ... ON CONFLICT DO NOTHING RETURNING` 在唯一索引保护下必须只返回一行
  ---------------------------------------------------------------------------
  INSERT INTO file_metadata_records(partition_id,id,version,kind,state,is_latest,created_by,file_source,data)
  VALUES ('p2','claim-1',1,'k','claiming',TRUE,'A','/fs/y','{}'::jsonb)
  ON CONFLICT (partition_id,file_source) WHERE state <> 'deleted' AND is_latest DO NOTHING
  RETURNING id INTO id1;

  INSERT INTO file_metadata_records(partition_id,id,version,kind,state,is_latest,created_by,file_source,data)
  VALUES ('p2','claim-2',1,'k','claiming',TRUE,'B','/fs/y','{}'::jsonb)
  ON CONFLICT (partition_id,file_source) WHERE state <> 'deleted' AND is_latest DO NOTHING
  RETURNING id INTO id2;

  IF id1 IS NULL OR id2 IS NOT NULL THEN
    RAISE EXCEPTION 'I2 失败：领取语义错误（实例A得到=%，实例B得到=%）', id1, id2;
  END IF;
  SELECT count(*) INTO n FROM file_metadata_records WHERE partition_id='p2' AND file_source='/fs/y';
  IF n <> 1 THEN RAISE EXCEPTION 'I2 失败：并发领取产生了 % 条记录（应为 1）', n; END IF;

  ---------------------------------------------------------------------------
  -- I3  state 只能取合法值（防止状态机被绕过）
  ---------------------------------------------------------------------------
  BEGIN
    INSERT INTO file_metadata_records(partition_id,id,version,kind,state,created_by,file_source,data)
    VALUES ('p3','bad',1,'k','bogus','u','/fs/z','{}'::jsonb);
    RAISE EXCEPTION 'I3 失败：非法 state 被接受 —— CHECK 约束缺失';
  EXCEPTION WHEN check_violation THEN
    NULL;
  END;

  ---------------------------------------------------------------------------
  -- I4  同一 id 只能有一条 latest（版本链）
  ---------------------------------------------------------------------------
  INSERT INTO file_metadata_records(partition_id,id,version,kind,state,is_latest,created_by,file_source,data)
  VALUES ('p4','rec-v',1,'k','ready',FALSE,'u','/fs/v1','{}'::jsonb);
  INSERT INTO file_metadata_records(partition_id,id,version,kind,state,is_latest,created_by,file_source,data)
  VALUES ('p4','rec-v',2,'k','ready',TRUE,'u','/fs/v1','{}'::jsonb);
  BEGIN
    INSERT INTO file_metadata_records(partition_id,id,version,kind,state,is_latest,created_by,file_source,data)
    VALUES ('p4','rec-v',3,'k','ready',TRUE,'u','/fs/v1','{}'::jsonb);
    RAISE EXCEPTION 'I4 失败：同一 id 出现两条 latest —— ux_mr_latest 缺失';
  EXCEPTION WHEN unique_violation THEN
    NULL;
  END;

  ---------------------------------------------------------------------------
  -- I5  位置记录的幂等键 (partition_id, file_id) 与 (partition_id, file_source)
  ---------------------------------------------------------------------------
  INSERT INTO file_locations(partition_id,file_id,file_source,container,object_key,zone,driver,created_by,created_at)
  VALUES ('p5','fid-1','/fs/l1','c','k','staging','posix','u',1);
  BEGIN
    INSERT INTO file_locations(partition_id,file_id,file_source,container,object_key,zone,driver,created_by,created_at)
    VALUES ('p5','fid-1','/fs/l2','c','k','staging','posix','u',1);
    RAISE EXCEPTION 'I5 失败：重复 file_id 被接受 —— 主键缺失';
  EXCEPTION WHEN unique_violation THEN NULL;
  END;
  BEGIN
    INSERT INTO file_locations(partition_id,file_id,file_source,container,object_key,zone,driver,created_by,created_at)
    VALUES ('p5','fid-2','/fs/l1','c','k','staging','posix','u',1);
    RAISE EXCEPTION 'I5 失败：重复 file_source 被接受 —— ux_fl_source 缺失';
  EXCEPTION WHEN unique_violation THEN NULL;
  END;

  ---------------------------------------------------------------------------
  -- I6  租约 GC 查询（ADR-009 §4.3）：只回收【租约已过期 且 无元数据记录】的对象
  --     三种情形：①在途(租约有效) 不回收 ②过期且无记录 回收 ③过期但有记录 不回收
  ---------------------------------------------------------------------------
  INSERT INTO file_locations(partition_id,file_id,file_source,container,object_key,zone,driver,created_by,created_at)
  VALUES ('p6','f-inflight','/fs/inflight','c','k','staging','posix','u',1),
         ('p6','f-expired', '/fs/expired', 'c','k','staging','posix','u',1),
         ('p6','f-recorded','/fs/recorded','c','k','staging','posix','u',1);

  -- ① 在途：租约未过期
  INSERT INTO staging_leases(partition_id,file_source,owner,expires_at)
  VALUES ('p6','/fs/inflight','A', now() + interval '60 seconds');
  -- ② 过期且无记录
  INSERT INTO staging_leases(partition_id,file_source,owner,expires_at)
  VALUES ('p6','/fs/expired','A', now() - interval '1 second');
  -- ③ 过期但有元数据记录
  INSERT INTO staging_leases(partition_id,file_source,owner,expires_at)
  VALUES ('p6','/fs/recorded','A', now() - interval '1 second');
  INSERT INTO file_metadata_records(partition_id,id,version,kind,state,created_by,file_source,data)
  VALUES ('p6','rec-r',1,'k','ready','u','/fs/recorded','{}'::jsonb);

  CREATE TEMP TABLE claimed(file_source TEXT) ON COMMIT DROP;
  INSERT INTO claimed
  SELECT l.file_source
    FROM staging_leases l
   WHERE l.expires_at < now()
     AND NOT EXISTS (SELECT 1 FROM file_metadata_records m
                      WHERE m.partition_id = l.partition_id
                        AND m.file_source  = l.file_source
                        AND m.state <> 'deleted')
     AND l.partition_id = 'p6';

  SELECT count(*) INTO n FROM claimed;
  IF n <> 1 THEN
    RAISE EXCEPTION 'I6 失败：应恰好领取 1 个对象（/fs/expired），实际 %', n;
  END IF;
  SELECT file_source INTO got FROM claimed;
  IF got <> '/fs/expired' THEN
    RAISE EXCEPTION 'I6 失败：领取了错误的对象 %（在途与有记录的对象都不能被回收）', got;
  END IF;

  ---------------------------------------------------------------------------
  -- I7  领导者选举：advisory lock 互斥，且会话结束后不残留
  ---------------------------------------------------------------------------
  SELECT pg_try_advisory_lock(123456789) INTO lock1;
  IF NOT lock1 THEN RAISE EXCEPTION 'I7 失败：第一次获取 advisory lock 失败'; END IF;

  -- 同一会话内重复获取同一 key 会成功（PG 的会话级锁可重入），
  -- 因此互斥性要在**另一个会话**里验证（见 db/tests/002_advisory_lock.sh）。
  PERFORM pg_advisory_unlock(123456789);
  SELECT pg_try_advisory_lock(123456789) INTO lock2;
  IF NOT lock2 THEN RAISE EXCEPTION 'I7 失败：释放后无法重新获取 advisory lock'; END IF;
  PERFORM pg_advisory_unlock(123456789);

  ---------------------------------------------------------------------------
  -- I8  排障视图可用且语义正确
  ---------------------------------------------------------------------------
  SELECT count(*) INTO n FROM v_staging_summary WHERE partition_id = 'p6';
  IF n <> 1 THEN RAISE EXCEPTION 'I8 失败：v_staging_summary 应有 p6 的一行，实际 %', n; END IF;
  SELECT without_lease INTO n FROM v_staging_summary WHERE partition_id='p6';
  IF n <> 0 THEN RAISE EXCEPTION 'I8 失败：p6 的 3 个对象都应有租约，without_lease=%', n; END IF;
  SELECT lease_expired INTO n FROM v_staging_summary WHERE partition_id='p6';
  IF n <> 2 THEN RAISE EXCEPTION 'I8 失败：p6 应有 2 个租约过期，实际 %', n; END IF;

  ---------------------------------------------------------------------------
  -- I9  防止"约束建错位置"的回归：确认 ux_mr_source 的键确实是 file_source
  --     本项目曾把唯一约束建在随机 UUID 主键上，导致幂等完全失效（ADR-009 §3 M2）
  ---------------------------------------------------------------------------
  SELECT count(*) INTO n
    FROM pg_index i
    JOIN pg_class c ON c.oid = i.indexrelid
    JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = ANY(i.indkey)
   WHERE c.relname = 'ux_mr_source' AND a.attname = 'file_source';
  IF n <> 1 THEN
    RAISE EXCEPTION 'I9 失败：ux_mr_source 未建在 file_source 上（幂等会失效）';
  END IF;

  -- I9b 谓词必须含 is_latest，否则会阻断合法的版本链
  SELECT count(*) INTO n FROM pg_indexes
   WHERE schemaname='public' AND indexname='ux_mr_source'
     AND indexdef ILIKE '%is_latest%';
  IF n <> 1 THEN
    RAISE EXCEPTION 'I9b 失败：ux_mr_source 的谓词缺少 is_latest —— 会阻断版本链（见迁移注释）';
  END IF;

  RAISE NOTICE '全部不变量自检通过（I1~I9b）';
END $$;

ROLLBACK;   -- 自检不污染数据库
