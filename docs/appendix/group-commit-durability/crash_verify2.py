#!/usr/bin/env python3
"""崩溃注入验证 v2
可测不变量（SIGKILL 保留页缓存，因此只能验证"可见性原子性"，不能验证"丢数据"）：
  I1  任何已存在的最终文件，内容必须完整且正确（无残缺/无截断）
  I2  残留的 .tmp_* 数量 ≤ 批大小（即"在途批"）
  I3  最终文件数 ≥ 最后一次 COMMIT 所承诺的数量（承诺过的必须都在）
不可测（如实标注）：页缓存丢失（真实断电）——需 root/dm-log-writes/VM 快照
对照组：协议 9（直接写最终路径，无 tmp+rename）应被检出残缺 → 证明检测器有效
"""
import os, select, shutil, signal, subprocess, time
PAY=4096
def expected(idx): return bytes(((idx*1315423911+i*2654435761)>>13)&0xff for i in range(PAY))
def verify(d):
    corrupt=[]; good=[]; tmp=0
    for name in sorted(os.listdir(d)):
        p=os.path.join(d,name)
        if name.startswith('.tmp_'): tmp+=1; continue
        if not name.startswith('f_'): continue
        try: idx=int(name[2:])
        except ValueError: continue
        try:
            with open(p,'rb') as f: data=f.read()
        except OSError as e: corrupt.append((name,f'读失败{e}')); continue
        if len(data)!=PAY: corrupt.append((name,f'大小{len(data)}')); continue
        if data!=expected(idx): corrupt.append((name,'内容不符')); continue
        good.append(idx)
    return good,corrupt,tmp
def run(proto,total,batch,kill_ms,seed):
    d=f'/tmp/gcverify/k{proto}_{seed}'
    shutil.rmtree(d,ignore_errors=True)
    p=subprocess.Popen(['./gcwriter',str(proto),d,str(total),str(batch)],
                       stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True)
    commits=[]
    if kill_ms is not None:
        end=time.time()+kill_ms/1000.0
        while time.time()<end:
            r,_,_=select.select([p.stdout],[],[],0.005)
            if r:
                l=p.stdout.readline()
                if l.startswith('COMMIT'): commits.append(int(l.split()[1]))
            if p.poll() is not None: break
        if p.poll() is None: os.kill(p.pid,signal.SIGKILL)
        p.wait()
    else: p.wait()
    good,corrupt,tmp=verify(d)
    return dict(good=good,corrupt=corrupt,tmp=tmp,committed=(commits[-1]+1) if commits else 0)
TOTAL,BATCH=6000,500
print("="*100)
print("A) 检测器自证：协议 9（直接写最终路径，无 tmp+rename）")
print("="*100)
det=0
for s in range(12):
    r=run(9,100000,100000,30+s*3,s)
    if r['corrupt']:
        det+=1
        if det<=3: print(f"  seed={s:<3} 检出残缺 {len(r['corrupt'])} 个，例：{r['corrupt'][0]}")
print(f"  → 12 次中 {det} 次检出残缺文件   {'✅ 检测器有效' if det>0 else '❌ 检测器无效'}")
print()
print("="*100)
print("B) 各协议崩溃注入（每协议 16 次随机时刻 SIGKILL，总文件 6000，批 500）")
print("="*100)
names={1:'P1 严格（每文件 fdatasync + 每文件 fsync dir）',
       2:'P2 组提交（每文件 fdatasync + 每批 fsync dir）',
       4:'P4 ★两阶段（写批→syncfs→rename→fsync dir）',
       3:'P3 松散（不安全：无数据 fsync）'}
print(f"  {'协议':<46} {'I1 残缺':>8} {'I2 残留tmp>批':>13} {'I3 少于承诺':>11} {'最大残留tmp':>11}")
print("  "+"-"*94)
for proto in [1,2,4,3]:
    v1=v2=v3=0; maxtmp=0
    for s in range(16):
        r=run(proto,TOTAL,BATCH,200+s*137,s)
        if r['corrupt']: v1+=1
        if r['tmp']>BATCH: v2+=1
        if len(r['good'])<r['committed']: v3+=1
        maxtmp=max(maxtmp,r['tmp'])
    print(f"  {names[proto]:<46} {v1:>6}/16 {v2:>11}/16 {v3:>9}/16 {maxtmp:>11}")
print()
print("  说明：SIGKILL 不丢弃页缓存，因此 I1~I3 只覆盖『可见性原子性』，")
print("        不能验证断电丢数据 —— 该结论由 model_check2.py 的顺序不变量给出。")
