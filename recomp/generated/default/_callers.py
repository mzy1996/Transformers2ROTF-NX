import re, sys, os, glob, collections
# 1) locate generated dir relative to script
g = os.path.dirname(os.path.abspath(__file__))
files = sorted(glob.glob(os.path.join(g,"T2ROTF_recomp.*.cpp")))
func_re = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]+)\)')
call_re = re.compile(r'\b(sub_[0-9A-Fa-f]+)\(ctx')
addr_re = re.compile(r'//\s*(?:bl|b)\s+0x([0-9A-Fa-f]{6,8})')
funcs = {}   # name -> (file, start)
name_at = {} # file -> sorted list of (start_line, name)
for f in files:
    cur=None; lst=[]
    with open(f, 'r', encoding='utf-8', errors='replace') as fh:
        for i, line in enumerate(fh, 1):
            m = func_re.match(line)
            if m:
                cur = m.group(1); funcs[cur]=(f, i); lst.append((i,cur))
    name_at[f]=lst
def enclosing(f, lineno):
    key = os.path.join(g, os.path.basename(f))
    lst = name_at.get(key) or name_at.get(f)
    if not lst: return None
    lo,hi=0,len(lst)-1; best=None
    for start,name in lst:
        if start<=lineno: best=name
        else: break
    return best
callers = collections.defaultdict(list)  # func -> list of (callerfunc, file, line)
for f in files:
    with open(f,'r',encoding='utf-8',errors='replace') as fh:
        for i,line in enumerate(fh,1):
            if func_re.match(line): continue
            for m in call_re.finditer(line):
                c = enclosing(f,i)
                callers[m.group(1)].append((c, os.path.basename(f), i))
target = sys.argv[1]
if not target.startswith('sub_'): target = 'sub_'+target.upper()
seen=set()
q=[(target,0)]
while q:
    n,d = q.pop(0)
    if n in seen and d>0: continue
    seen.add(n)
    cs = callers.get(n,[])
    ind = '  '*d
    print(f"{ind}{n}  (callers={len(cs)})")
    if d<5:
        for c,f,l in cs[:8]:
            print(f"{ind}   <- {c}  {f}:{l}")
            if c and c not in seen: q.append((c,d+1))
