import re, glob
files = sorted(glob.glob("T2ROTF_recomp.*.cpp"))
pats = [",248", ",256", ",260", ",264", ",272", ",276", ",280", ",284", ",288", ",24096", ",23776", ",24120", ",24416", ",24088"]
res = {}
for f in files:
    cur=None
    for i,ln in enumerate(open(f, encoding="utf-8", errors="replace"), 1):
        m=re.match(r"DEFINE_REX_FUNC\((\w+)\)", ln)
        if m:
            cur={"f":f,"n":m.group(1),"ln":i,"hits":{}}
            res[(f,m.group(1))]=cur
            continue
        if cur is not None:
            if ln.startswith("}"):
                cur=None; continue
            for p in pats:
                if p+"(" in ln or p+" " in ln or ln.rstrip().endswith(p) or (p+"$") in ln:
                    cur["hits"][p]=cur["hits"].get(p,0)+1
interesting=[]
for k,v in res.items():
    h=v["hits"]
    if (",24096" in h or ",23776" in h) and any(p in h for p in [",248",",256",",272",",276",",280",",284",",288",",264",",260"]):
        interesting.append(v)
for v in interesting:
    print(v["f"], v["n"], v["ln"], {a:b for a,b in v["hits"].items() if a in (",248",",256",",260",",264",",272",",276",",280",",284",",288",",24096",",23776",",24120")})
