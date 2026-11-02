import re, glob, sys

# scan: which recompiled functions write to offsets 248/272/276/280 (arm counts)
# and 256/260/264 (backup arm counts) of some base, and which read them.
pats = {
    "WR248": re.compile(r"REX_STORE_U32\(ctx\.r\d+\.u32 \+ 248,"),
    "WR272": re.compile(r"REX_STORE_U32\(ctx\.r\d+\.u32 \+ 272,"),
    "WR276": re.compile(r"REX_STORE_U32\(ctx\.r\d+\.u32 \+ 276,"),
    "WR280": re.compile(r"REX_STORE_U32\(ctx\.r\d+\.u32 \+ 280,"),
    "WR256": re.compile(r"REX_STORE_U32\(ctx\.r\d+\.u32 \+ 256,"),
    "WR260": re.compile(r"REX_STORE_U32\(ctx\.r\d+\.u32 \+ 260,"),
    "WR264": re.compile(r"REX_STORE_U32\(ctx\.r\d+\.u32 \+ 264,"),
    "WR16548": re.compile(r"REX_STORE_U32\(ctx\.r\d+\.u32 \+ 16548,"),
    "WR16568": re.compile(r"REX_STORE_U32\(ctx\.r\d+\.u32 \+ 16568,"),
}
hits = {}
for f in sorted(glob.glob("T2ROTF_recomp.*.cpp")):
    lines = open(f, encoding="utf-8", errors="replace").read().splitlines()
    cur = None
    for i, ln in enumerate(lines, 1):
        m = re.match(r"DEFINE_REX_FUNC\((\w+)\)", ln)
        if m:
            cur = m.group(1)
            continue
        if cur is None:
            continue
        for tag, p in pats.items():
            if p.search(ln):
                hits.setdefault(tag, []).append(f"{cur} {f}:{i}")
for tag in sorted(hits):
    print(f"=== {tag} ({len(hits[tag])}) ===")
    for h in hits[tag][:40]:
        print("  ", h)