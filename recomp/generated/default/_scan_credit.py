import os, re, glob
files = sorted(glob.glob("T2ROTF_recomp.*.cpp"))
blocks = []
for f in files:
    txt = open(f, encoding="utf-8", errors="replace").read().splitlines()
    cur = None
    for i, ln in enumerate(txt, 1):
        m = re.match(r"DEFINE_REX_FUNC\((\w+)\)", ln)
        if m:
            cur = {"f": f, "name": m.group(1), "ln": i, "body": []}
            blocks.append(cur)
            continue
        if cur is not None:
            if ln.startswith("}"):
                cur = None
                continue
            cur["body"].append((i, ln))
print("total funcs", len(blocks))
hits = []
for b in blocks:
    body = [l for _, l in b["body"]]
    joined = "\n".join(body)
    if "24096" not in joined:
        continue
    offs = {}
    for off in ("248(", "256(", "260(", "264(", "272(", "276(", "280(", "284(", "288(", "24(", "16(", "0(r30", "0(r31"):
        if off in joined:
            offs[off] = joined.count(off)
    inc = joined.count("+= 1")
    hits.append((b["f"], b["name"], b["ln"], offs, inc))
for h in hits:
    print(h)
