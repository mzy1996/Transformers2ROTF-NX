import sys, re, glob
want = set(sys.argv[1:])
for f in sorted(glob.glob("T2ROTF_recomp.*.cpp")):
    lines = open(f, encoding="utf-8", errors="replace").read().splitlines()
    cur=None
    for i,ln in enumerate(lines,1):
        m=re.match(r"DEFINE_REX_FUNC\((\w+)\)", ln)
        if m:
            cur = m.group(1) if m.group(1) in want else None
            if cur: print(f"=== {f}:{i} {cur} ===")
            continue
        if cur is not None:
            if ln.startswith("}"):
                cur=None; continue
            if not ln.strip().startswith("//") and "//" in ln:
                pass
            print(f"{i}: {ln.strip()}")
