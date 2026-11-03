# check fatdump output against cuobjdump -elf KPARAM_INFO for the same file
import re, subprocess, sys, collections
f = sys.argv[1]
ours = {}
for l in open(sys.argv[2]):
    if l.startswith('#'): continue
    parts = l.split()
    ours[parts[0]] = [tuple(map(int, p.split(':'))) for p in parts[2:]]
out = subprocess.run(['/usr/local/cuda/bin/cuobjdump', '-elf', '-arch', 'sm_72', f], capture_output=True, text=True).stdout
ref = collections.defaultdict(dict); cur = None
for l in out.splitlines():
    m = re.match(r'\s*\.nv\.info\.(\S+)', l)
    if m and not l.strip().startswith('.nv.info:'): cur = m.group(1); ref.setdefault(cur, {}); continue
    m = re.search(r'Ordinal\s*:\s*0x([0-9a-f]+)\s+Offset\s*:\s*0x([0-9a-f]+)\s+Size\s*:\s*0x([0-9a-f]+)', l)
    if m and cur: ref[cur][int(m.group(1),16)] = (int(m.group(2),16), int(m.group(3),16))
ok = bad = missing = 0
for k, d in ref.items():
    r = [d[i] for i in sorted(d)]
    if k not in ours: missing += 1; continue
    if ours[k] == r: ok += 1
    else:
        bad += 1
        if bad <= 5: print('MISMATCH', k, ours[k], r)
print(f'{f}: cuobjdump kernels {len(ref)}, match {ok}, mismatch {bad}, missing {missing}')
