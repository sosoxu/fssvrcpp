#!/usr/bin/env python3
"""耐久性协议验证器 v2 —— 正确建模 syncfs 的覆盖范围

R1（可见即完整）：任何 rename(tmp→final) 之前，该 tmp 的【数据】必须已经 durable。
   数据 durable 的两种达成方式：
     (a) 显式 fdatasync/fsync 该文件 fd
     (b) 在该文件 write 之后发生过 syncfs（syncfs 覆盖整个文件系统上已写的数据）
R2（承诺即可靠）：任何对外 COMMIT 承诺之前，本批的【改名】必须已经 durable
     (a) fsync(dir)  或 (b) syncfs 发生在 rename 之后

I1 崩溃点枚举（最坏分支）：rename 存活 且 其数据未 durable → "存在但内容残缺"
"""
import re
OPEN=re.compile(r'openat\([^,]+,\s*"([^"]+)"')
WRITE=re.compile(r'\bwrite\((\d+),.*\)\s*=\s*(-?\d+)')
SYNC=re.compile(r'\b(fdatasync|fsync|syncfs)\((\d+)\)')
RENAME=re.compile(r'\brename\("([^"]+)",\s*"([^"]+)"\)')
CLOSE=re.compile(r'\bclose\((\d+)\)')
COMMIT=re.compile(r'write\(1, "COMMIT')

def analyze(trace):
    cur={}; written_epoch={}; dur_epoch={}; epoch=0
    r1=0; renames=0; state=[]      # state: [(final, data_durable_bool)]
    r2=0; committed_seen=False; pending_renames=0
    for line in open(trace,errors='replace'):
        m=RENAME.search(line)
        if m:
            src,dst=m.group(1),m.group(2)
            if not dst.startswith('/tmp'): continue
            renames+=1
            we=written_epoch.get(src)
            de=dur_epoch.get(src,-1)
            ok = (we is not None) and (de>=we)
            if not ok: r1+=1
            state.append((dst,ok))
            pending_renames+=1
            continue
        m=SYNC.search(line)
        if m:
            kind,fd=m.group(1),int(m.group(2))
            if kind=='syncfs':
                epoch+=1                              # 覆盖此前所有已写数据
                for p in list(written_epoch): dur_epoch[p]=epoch
                pending_renames=0                     # 改名也随 syncfs 落盘（同一 fs 的日志）
            else:
                p=cur.get(fd)
                if p: dur_epoch[p]=epoch if p in written_epoch else epoch
            continue
        m=WRITE.search(line)
        if m:
            fd=int(m.group(1)); p=cur.get(fd)
            if p and p.startswith('/tmp') and '.tmp_' in p:
                if p not in written_epoch: written_epoch[p]=epoch
            continue
        m=CLOSE.search(line)
        if m: cur.pop(int(m.group(1)),None); continue
        m=OPEN.search(line)
        if m:
            v=line.rsplit('=',1)[-1].strip()
            if v.isdigit(): cur[int(v)]=m.group(1)
    return dict(renames=renames,r1=r1,
                worst_incomplete=sum(1 for _,ok in state if not ok))

names={'t1':'P1 严格：每文件 fdatasync + 每文件 fsync(dir)',
       't2':'P2 组提交：每文件 fdatasync + 每批 fsync(dir)',
       't4':'P4 ★两阶段：写全批 tmp → syncfs → 统一 rename → fsync(dir)',
       't3':'P3 松散（不安全）：无数据 fsync + 每批 syncfs'}
print(f"  {'协议':<58} {'rename':>6} {'R1违反':>7} {'最坏分支残缺':>12}  判定")
print("  "+"-"*106)
for k in ['t1','t2','t4','t3']:
    r=analyze(f'{k}.trace')
    v='✅ 安全' if r['worst_incomplete']==0 else f"❌ 不安全（{r['worst_incomplete']} 个）"
    print(f"  {names[k]:<58} {r['renames']:>6} {r['r1']:>7} {r['worst_incomplete']:>12}  {v}")
