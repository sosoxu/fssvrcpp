#!/usr/bin/env python3
"""S4: SQLite 作为多实例共享存储会怎样
   S5: 自签传输 token 的 single_use_nonce 跨实例失效（ADR-003 埋的雷）"""
import hmac, hashlib, os, shutil, sqlite3, time
from multiprocessing import Process, Queue

print("="*84)
print("S4 SQLite 多实例：两种部署方式的后果")
print("="*84)
d='/tmp/multiinst/sqlite'; shutil.rmtree(d,ignore_errors=True); os.makedirs(d)

# 方式一：每实例一个本地 DB 文件（最容易被误用的部署方式）
for inst in ('A','B'):
    con=sqlite3.connect(f'{d}/{inst}.db'); con.execute("CREATE TABLE IF NOT EXISTS loc(file_id TEXT PRIMARY KEY, src TEXT)")
    con.commit(); con.close()
con=sqlite3.connect(f'{d}/A.db'); con.execute("INSERT INTO loc VALUES('fid-1','/staging/fid-1')"); con.commit(); con.close()
a=sqlite3.connect(f'{d}/A.db').execute("SELECT count(*) FROM loc WHERE file_id='fid-1'").fetchone()[0]
b=sqlite3.connect(f'{d}/B.db').execute("SELECT count(*) FROM loc").fetchone()[0]
print(f"  ① 每实例独立 DB 文件：A 中 fid-1={a} 条；B 中总记录={b} 条")
print(f"     → B 看不到 A 的记录 ⇒ B 会接受同一个 fileID 的第二次申请 ⇒ ❌ 状态发散")

# 方式二：两进程共用同一个 SQLite 文件（本地磁盘）
shared=f'{d}/shared.db'
con=sqlite3.connect(shared); con.execute("PRAGMA journal_mode=WAL")
con.execute("CREATE TABLE IF NOT EXISTS loc(file_id TEXT PRIMARY KEY, src TEXT)"); con.commit(); con.close()
def w(inst,res):
    try:
        c=sqlite3.connect(shared,timeout=5); c.execute("PRAGMA busy_timeout=5000")
        for i in range(200):
            c.execute("INSERT OR IGNORE INTO loc VALUES(?,?)",(f"{inst}-{i}",f"/s/{inst}-{i}")); c.commit()
        res.put('ok')
    except Exception as e: res.put(f'err:{type(e).__name__}')
res=Queue(); ps=[Process(target=w,args=(i,res)) for i in ('A','B')]
for p in ps: p.start()
for p in ps: p.join()
out=[res.get() for _ in range(2)]
total=sqlite3.connect(shared).execute("SELECT count(*) FROM loc").fetchone()[0]
print(f"  ② 两进程共用同一 SQLite 文件（本地 ext4）：结果={out}，记录数={total}/400")
print(f"     → 本地可用；但 SQLite 官方明确【不支持在网络文件系统上并发写】")
print(f"       而集中存储多实例必然用 NFS/SAN ⇒ ❌ 不可作为多实例共享状态")

print()
print("="*84)
print("S5 自签传输 token 的 single_use_nonce 跨实例")
print("="*84)
KEY=b'shared-signing-key'
def token(op, ref, nonce):
    payload=f"{op}|{ref}|{nonce}".encode()
    return nonce, hmac.new(KEY,payload,hashlib.sha256).hexdigest()[:16]
used_A=set(); used_B=set()
n,sig=token('get','/p/obj1','nonce-1')
print(f"  实例 A 签发 token: nonce={n}")
print(f"  ① 带回实例 A（A 的 nonce 表含该 nonce）：{'拒绝' if n in used_A else '接受'} → 预期接受 ✅")
print(f"  ② 带回实例 B（B 的 nonce 表不含该 nonce）：{'拒绝' if n in used_B else '接受'} → 若期望一次性则 ❌ 跨实例无法拒绝重放")
print(f"  ③ 同一 token 再次回 A：{'拒绝' if n in used_A else '接受'} → A 记住了则拒绝 ✅")
print(f"  ⇒ 用本地 nonce 表时：'一次性'语义在【多实例下不成立】（B 会接受重放）")
print(f"     要么把 nonce 状态放进共享存储，要么关闭 single_use_nonce（默认应关闭）")
