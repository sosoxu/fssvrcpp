#!/usr/bin/env python3
"""S3：共享存储上临时文件名跨实例冲突
两个实例并发写入同一个 staging 目录，最终名不同但 tmp 名相同（因为 tmp 名只用了 fileID 序号）
naive: tmp = .tmp_<idx>            → 冲突：A 的 rename 可能发布 B 的内容
fixed: tmp = .tmp_<instance>_<pid>_<counter>_<uuid>  → 无冲突
"""
import os, shutil, sys, random
from multiprocessing import Process, Queue

def payload(idx, tag):
    # 内容里嵌入写者标识，便于检测"发布了他人的内容"
    head = f"idx={idx};writer={tag};".encode()
    return head + bytes(((idx*1315423911+i*2654435761)>>13)&0xff for i in range(4096-len(head)))

def worker(inst, idx, shared_dir, mode, barrier, q):
    fin = os.path.join(shared_dir, f"f_{inst}_{idx}")
    tmp = (os.path.join(shared_dir, f".tmp_{idx}") if mode=='naive'
           else os.path.join(shared_dir, f".tmp_{inst}_{os.getpid()}_{idx}"))
    data = payload(idx, inst)
    # 让两个进程尽量同时写同一个 tmp
    barrier.wait()
    with open(tmp,'wb') as f:
        f.write(data)
    barrier.wait()
    os.rename(tmp, fin)
    q.put((inst, fin, data[:64]))

def run(mode, tries=40):
    bad = 0
    for t in range(tries):
        d = f'/tmp/multiinst/s3_{mode}_{t}'
        shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
        idx = 7   # 故意让两个实例用同一个 idx → tmp 名相同
        b = __import__('multiprocessing').Barrier(2)
        q = Queue()
        ps = [Process(target=worker, args=(inst, idx, d, mode, b, q)) for inst in ('A','B')]
        for p in ps: p.start()
        for p in ps: p.join()
        results = []
        while not q.empty(): results.append(q.get())
        # 校验：每个最终文件的内容必须与它自己的 writer 标识一致
        for inst, fin, _ in results:
            with open(fin,'rb') as f: got = f.read(64)
            expect = f"idx={idx};writer={inst};".encode()
            if got[:len(expect)] != expect:
                bad += 1
                if bad <= 3:
                    print(f"    ❌ 内容错乱：{os.path.basename(fin)} 期望 {expect[:20]!r} 实际 {got[:20]!r}")
                break
        shutil.rmtree(d, ignore_errors=True)
    return bad

print("S3 临时文件名跨实例冲突（40 次尝试，两实例同 idx 并发写同名 tmp）")
for mode, name in [('naive','naive: tmp=.tmp_<idx>'), ('fixed','fixed: tmp=.tmp_<inst>_<pid>_<n>')]:
    bad = run(mode)
    print(f"  {name:<42} → 内容错乱 {bad}/40 次  {'❌ 存在竞态' if bad else '✅ 无冲突'}")
