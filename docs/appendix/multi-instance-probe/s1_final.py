#!/usr/bin/env python3
"""S1 重复创建竞态 —— 公平对比（按模式分别建/删唯一索引）"""
import os, subprocess, time, traceback, uuid
from multiprocessing import Process, Queue
PGBIN='/tmp/pgx/ext/usr/lib/postgresql/14/bin'
ENV=dict(os.environ, LD_LIBRARY_PATH='/tmp/pgx/ext/usr/lib/x86_64-linux-gnu')
def psql(sql):
    r=subprocess.run([f'{PGBIN}/psql','-h','127.0.0.1','-p','15432','-U','fss','-d','fss','-tAq','-v','ON_ERROR_STOP=0','-c',sql],
                     capture_output=True,text=True,env=ENV)
    return r.stdout.strip(), r.returncode, r.stderr.strip()
def q(sql):
    out,rc,err = psql(sql)
    return out
def setup(mode):
    q("TRUNCATE metadata_records, staging_leases;")
    if mode=='naive': q("DROP INDEX IF EXISTS ux_mr_source;")
    else:             q("CREATE UNIQUE INDEX IF NOT EXISTS ux_mr_source ON metadata_records(partition_id,file_source) WHERE state <> 'deleted';")

def naive(fs,key,ms,res):
    try:
        n=q(f"SELECT count(*) FROM metadata_records WHERE partition_id='opendes' AND file_source='{fs}';")
        if n=='0':
            time.sleep(ms/1000.0)              # check-then-act 窗口
            out,rc,err = psql(f"INSERT INTO metadata_records(partition_id,id,version,file_source,object_key,state)"
                              f" VALUES('opendes','rec-{uuid.uuid4().hex[:8]}',1,'{fs}','{key}','ready');")
            res.put('copied' if rc==0 else 'rejected:'+err.split('\n')[0][:40])
        else: res.put('skipped')
    except Exception: res.put('exc:'+traceback.format_exc().splitlines()[-1][:50])

def fixed(fs,key,ms,res):
    try:
        got,rc,err = psql(f"INSERT INTO metadata_records(partition_id,id,version,file_source,object_key,state)"
                          f" VALUES('opendes','rec-{uuid.uuid4().hex[:8]}',1,'{fs}','{key}','claiming')"
                          f" ON CONFLICT (partition_id,file_source) WHERE state <> 'deleted' DO NOTHING RETURNING id;")
        if rc!=0: res.put('err:'+err.split('\n')[0][:50]); return
        if got:
            time.sleep(ms/1000.0)              # 只有领取者做复制
            q(f"UPDATE metadata_records SET state='ready' WHERE partition_id='opendes' AND file_source='{fs}';")
            res.put('copied')
        else:
            res.put('skipped')                 # 幂等：返回既有记录，不重复复制
    except Exception: res.put('exc:'+traceback.format_exc().splitlines()[-1][:50])

def run(mode, tries=20, ms=80):
    setup(mode); dup=0; copies=[]; errs=0
    for t in range(tries):
        q("TRUNCATE metadata_records;")
        fs=f"/osdu-user/staging/z_{t}"; key=f"key_{t}"; res=Queue()
        ps=[Process(target=(naive if mode=='naive' else fixed),args=(fs,key,ms,res)) for _ in range(2)]
        for p in ps: p.start()
        for p in ps: p.join()
        got=[]
        while not res.empty(): got.append(res.get())
        copies.append(got.count('copied'))
        if any(g.startswith(('rejected','err','exc')) for g in got): errs+=1
        c=int(q(f"SELECT count(*) FROM metadata_records WHERE partition_id='opendes' AND file_source='{fs}';") or 0)
        if c>1: dup+=1
    return dup,copies,errs

print("两个实例并发用同一 fileSource 登记元数据（20 次，每次 2 实例）\n")
for m,n in [('naive','naive：无唯一约束，check-then-insert'),
            ('fixed','fixed：幂等键唯一约束 + ON CONFLICT 原子领取')]:
    d,c,e=run(m)
    print(f"  {n}")
    print(f"      重复记录 {d}/20  每次执行复制的实例数={sorted(set(c))}  写冲突/报错 {e}/20   "
          f"{'❌ 产生重复记录' if d else '✅ 幂等，且只复制一次'}")
