#!/usr/bin/env python3
"""S1 重复创建竞态 / S2 GC 与在途写入竞态 —— 在 PostgreSQL 上对比 naive 与 fixed
用 psql 子进程访问 PG（无 python 驱动）。每次操作为一个独立连接，
因此竞态窗口是真实的跨连接窗口。
"""
import os, shutil, subprocess, sys, time, uuid
from multiprocessing import Process, Queue

PGBIN='/tmp/pgx/ext/usr/lib/postgresql/14/bin'
ENV=dict(os.environ, LD_LIBRARY_PATH='/tmp/pgx/ext/usr/lib/x86_64-linux-gnu')
def psql(sql, db='fss'):
    r=subprocess.run([f'{PGBIN}/psql','-h','127.0.0.1','-p','15432','-U','fss','-d',db,'-tAq','-c',sql],
                     capture_output=True,text=True,env=ENV)
    if r.returncode!=0: raise RuntimeError(r.stderr.strip()[:200])
    return r.stdout.strip()
def reset():
    psql("TRUNCATE metadata_records, staging_leases;")
SHARED='/tmp/multiinst/shared'

# ---------------- S1: 重复创建 ----------------
def s1_worker_naive(file_source, objkey, q):
    # 经典 check-then-act：无唯一约束
    n=psql(f"SELECT count(*) FROM metadata_records WHERE partition_id='opendes' AND file_source='{file_source}';")
    if n=='0':
        time.sleep(0.05)                      # 放大竞态窗口
        psql(f"INSERT INTO metadata_records VALUES('opendes','rec-{uuid.uuid4().hex[:8]}',1,'{file_source}','{objkey}');")
        q.put('inserted')

def s1_worker_fixed(file_source, objkey, q):
    # 原子 upsert + 唯一约束：只有第一个插入成功
    out=psql(f"""INSERT INTO metadata_records VALUES('opendes','rec-{uuid.uuid4().hex[:8]}',1,'{file_source}','{objkey}')
                 ON CONFLICT (partition_id,id,version) DO NOTHING
                 RETURNING id;""")
    q.put('inserted' if out else 'conflict')

def s1(mode, tries=20):
    dup=0
    for t in range(tries):
        reset(); fs=f"/osdu-user/staging/f_{t}"; key=f"key_{t}"
        q=Queue()
        fn = s1_worker_naive if mode=='naive' else s1_worker_fixed
        ps=[Process(target=fn,args=(fs,key,q)) for _ in range(2)]
        for p in ps: p.start()
        for p in ps: p.join()
        cnt=int(psql(f"SELECT count(*) FROM metadata_records WHERE partition_id='opendes' AND file_source='{fs}';"))
        if cnt>1: dup+=1
    return dup

# ---------------- S2: GC 与在途写入竞态 ----------------
def s2_writer_native(fs, key, q, work_ms):
    """实例 A：写 staging 对象 → 要花 work_ms 做校验和/复制 → 才登记元数据"""
    os.makedirs(SHARED, exist_ok=True)
    obj=os.path.join(SHARED,key); open(obj,'wb').write(b'REAL-DATA'*512)
    time.sleep(work_ms/1000.0)                    # ← 在途窗口
    # 登记元数据前先确认对象还在（真实实现里 copy 会失败）
    if not os.path.exists(obj):
        q.put('lost'); return
    psql(f"INSERT INTO metadata_records VALUES('opendes','rec-A',1,'{fs}','{key}') ON CONFLICT DO NOTHING;")
    q.put('ok')

def s2_writer_fixed(fs, key, q, work_ms):
    """实例 A：先建租约（带过期时间），干活期间续租，完成后登记"""
    os.makedirs(SHARED, exist_ok=True)
    obj=os.path.join(SHARED,key); open(obj,'wb').write(b'REAL-DATA'*512)
    psql(f"""INSERT INTO staging_leases VALUES('opendes','{fs}','instA', now() + interval '2 seconds')
             ON CONFLICT (partition_id,file_source) DO UPDATE SET expires_at = now() + interval '2 seconds';""")
    for _ in range(int(work_ms/100)+1):
        time.sleep(0.1)
        psql(f"""UPDATE staging_leases SET expires_at = now() + interval '2 seconds'
                 WHERE partition_id='opendes' AND file_source='{fs}';""")   # 续租
    if not os.path.exists(obj):
        q.put('lost'); return
    psql(f"INSERT INTO metadata_records VALUES('opendes','rec-A',1,'{fs}','{key}') ON CONFLICT DO NOTHING;")
    q.put('ok')

def s2_gc_naive():
    """实例 B：GC 删除"没有元数据记录"的 staging 对象"""
    n=0
    for name in os.listdir(SHARED):
        fs=f"/osdu-user/staging/{name}"
        cnt=psql(f"SELECT count(*) FROM metadata_records WHERE partition_id='opendes' AND file_source='{fs}';")
        if cnt=='0':
            try: os.remove(os.path.join(SHARED,name)); n+=1
            except FileNotFoundError: pass
    return n

def s2_gc_fixed():
    """实例 B：GC 只清理【租约已过期】且无元数据记录的对象；用 DELETE..RETURNING 原子领取"""
    out=psql("""DELETE FROM staging_leases l
                WHERE l.expires_at < now()
                  AND NOT EXISTS (SELECT 1 FROM metadata_records m
                                  WHERE m.partition_id=l.partition_id AND m.file_source=l.file_source)
                RETURNING l.file_source;""")
    n=0
    for fs in [x for x in out.splitlines() if x.strip()]:
        key=os.path.basename(fs)
        try: os.remove(os.path.join(SHARED,key)); n+=1
        except FileNotFoundError: pass
    return n

def s2(mode, tries=20, work_ms=500):
    lost=0
    for t in range(tries):
        reset(); shutil.rmtree(SHARED, ignore_errors=True); os.makedirs(SHARED)
        fs=f"/osdu-user/staging/obj_{t}"; key=f"obj_{t}"
        q=Queue()
        w = s2_writer_native if mode=='naive' else s2_writer_fixed
        p=Process(target=w,args=(fs,key,q,work_ms)); p.start()
        time.sleep(0.15)                     # 等 A 进入在途窗口
        (s2_gc_naive if mode=='naive' else s2_gc_fixed)()
        p.join()
        res=q.get() if not q.empty() else 'unknown'
        if res=='lost': lost+=1
    return lost

print("="*86)
print("S1 重复创建竞态（两个实例用同一 fileSource 并发登记元数据，20 次）")
print("="*86)
for m,n in [('naive','naive：check-then-insert，无唯一约束'),('fixed','fixed：唯一约束 + 原子 upsert')]:
    d=s1(m); print(f"  {n:<44} → 出现重复记录 {d}/20 次  {'❌ 数据重复' if d else '✅ 正确'}")
print()
print("="*86)
print("S2 GC 与在途写入竞态（实例 A 在途 500ms，实例 B 同时跑 GC，20 次）")
print("="*86)
for m,n in [('naive','naive GC：无元数据记录就删'),('fixed','fixed GC：租约未过期则跳过（原子领取）')]:
    l=s2(m); print(f"  {n:<44} → A 的数据被误删 {l}/20 次  {'❌ 数据丢失' if l else '✅ 受保护'}")
