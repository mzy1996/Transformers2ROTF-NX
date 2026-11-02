import re, glob, sys

# scan generated recomp cpp files: per-function, find references to the display-manager
# wait/notify globals and the 824F0xxx kernel wrapper family.
files = sorted(glob.glob('T2ROTF_recomp.*.cpp'), key=lambda s: int(re.search(r'\.(\d+)\.', s).group(1)))
offs = ['-18972', '-18976', '-18988', '-18992']
refs = {}          # func -> set of markers
funcs = {}         # name -> (file, line)

for f in files:
    lines = open(f, encoding='utf-8', errors='replace').read().splitlines()
    cur = None
    for i, ln in enumerate(lines, 1):
        m = re.match(r'DEFINE_REX_FUNC\((\w+)\)', ln)
        if m:
            cur = m.group(1)
            funcs.setdefault(cur, (f, i))
            continue
        if cur is None:
            continue
        s = ln.strip()
        if not s or s.startswith('//'):
            continue
        for o in offs:
            if o in s and ('REX_STORE' in s or 'stw' in s or 'stb' in s or 'sth' in s):
                refs.setdefault(cur, set()).add('W' + o)
            elif o in s:
                refs.setdefault(cur, set()).add('R' + o)
        for name in re.findall(r'sub_824F[0-9A-Fa-f]{4}', s):
            refs.setdefault(cur, set()).add('C' + name)

print('=== functions touching the display-manager wait globals (-18970..-18992 @0x82DBB5xx) ===')
for k in sorted(refs):
    marks = sorted(x for x in refs[k] if not x.startswith('C'))
    if marks:
        print(f'{k:14s} {funcs[k][0]}:{funcs[k][1]}  {marks}')

print()
print('=== callers of the 824F0/824F3 kernel wrappers ===')
inv = {}
for k, marks in refs.items():
    for x in marks:
        if x.startswith('C'):
            inv.setdefault(x[1:], set()).add(k)
for name in sorted(inv):
    print(f'{name}: {len(inv[name])} callers -> {sorted(inv[name])[:8]}')

print()
print('=== 82881848 callers ===')
inv2 = {}
for f in files:
    lines = open(f, encoding='utf-8', errors='replace').read().splitlines()
    cur = None
    for i, ln in enumerate(lines, 1):
        m = re.match(r'DEFINE_REX_FUNC\((\w+)\)', ln)
        if m:
            cur = m.group(1)
            continue
        if cur and 'sub_82881848(' in ln:
            inv2.setdefault(cur, []).append(i)
print(inv2)