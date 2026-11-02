#!/usr/bin/env python3
"""T2ROTF (2026-10-25): 把 d3d.h / d3d_native.h / hooks/device.cpp 里的设备结构偏移逐个数一遍，
看它们落在 T2ROTF 自己镜像的哪一簇。

为什么必须实测：这些常量现在的主值是参考构建（SA2）的值、方括号里是 N&B 的值，两套都可能
与 T2 不同；偏移错了，Renut 侧读写的就不是 guest 的同一个字段（串状态、丢画面）。

判定依据：T2 自己的反汇编索引 out/t2-tools/t2_index.json（31614 个函数，指令文本形如
`lwz r11,10896(r3)`）。一个真正存在的字段偏移，一定以立即数 `,<off>(r` 的形式出现在 T2 的
D3D 段（0x822C0000-0x822EFFFF = D3D 角色 / 状态机）里；落在哪一簇（−128 / −368 / −376 /
−404 / −408 / +8）就是在哪一簇。

用法：
  python tools/t2_off_verify.py            # 只报告
  python tools/t2_off_verify.py --json out/t2-tools/t2_off_verify.json
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
INDEX = ROOT / "out/t2-tools/t2_index.json"
SOURCES = [
    ROOT / "recomp/src/renut_engine/gpu/d3d/d3d.h",
    ROOT / "recomp/src/renut_engine/gpu/d3d/d3d_native.h",
    ROOT / "recomp/src/renut_engine/gpu/d3d/hooks/device.cpp",
]
D3D_LO, D3D_HI = 0x822C0000, 0x822F0000
DELTAS = [0, -128, +8, -368, -376, -404, -408, +128, -420]

# `constexpr u32 NAME = 11068;` 或 `constexpr u32 NAME = 0x28C0;`，后面可选 `// ... [0x2ABC] ...`
DECL = re.compile(
    r"^\s*(?:inline\s+)?constexpr\s+\w+\s+(?P<name>[A-Za-z_]\w*)\s*=\s*"
    r"(?P<value>0[xX][0-9A-Fa-f]+|\d+)\s*;"
    r"(?P<rest>.*)$")
BRACKET = re.compile(r"\[(0[xX][0-9A-Fa-f]+|\d+)\]")
IMM = re.compile(r",(\d+)\(r")


def parse_decls() -> list[tuple[str, str, int, int | None]]:
    """→ [(file, name, value, bracket_or_None)]"""
    out = []
    for path in SOURCES:
        for line in path.read_text(encoding="utf-8").splitlines():
            m = DECL.match(line)
            if not m:
                continue
            name = m.group("name")
            value = int(m.group("value"), 0)
            if value < 32 or value > 1 << 20:  # 只关心结构偏移
                continue
            if not name.startswith("k") and "::" not in name:
                continue
            b = BRACKET.search(m.group("rest"))
            bracket = int(b.group(1), 0) if b else None
            out.append((path.name, name, value, bracket))
    return out


def build_offset_index() -> dict[int, set[str]]:
    data = json.loads(INDEX.read_text(encoding="utf-8"))
    offsets: dict[int, set[str]] = {}
    for addr, entry in data.items():
        for insn in entry["i"]:
            m = IMM.search(insn)
            if m:
                offsets.setdefault(int(m.group(1)), set()).add(addr)
    return offsets


def main() -> int:
    decls = parse_decls()
    idx = build_offset_index()
    d3d_keys = [a for a in {k for s in idx.values() for k in s} if D3D_LO <= int(a, 16) < D3D_HI]
    print(f"索引偏移立即数 {len(idx)} 个；D3D 段函数 {len(d3d_keys)} 个")
    report = []
    print(f"\n{'常量':34s} {'现值':>7s} {'括号':>7s}  命中（值: 全镜像/D3D段）")
    for fname, name, value, bracket in sorted(decls, key=lambda t: t[2]):
        cands = []
        for delta in DELTAS:
            cand = value + delta
            if cand in idx:
                cands.append((cand, delta))
        if bracket is not None and bracket not in [c[0] for c in cands]:
            if bracket in idx:
                cands.append((bracket, bracket - value))
        hits = []
        for cand, delta in cands:
            addrs = idx[cand]
            n_d3d = sum(1 for a in addrs if D3D_LO <= int(a, 16) < D3D_HI)
            hits.append((cand, delta, len(addrs), n_d3d))
        hits.sort(key=lambda t: (-t[3], -t[2]))
        shown = "  ".join(f"{c}({d:+d}):{tot}/{d3d}" for c, d, tot, d3d in hits if d3d or tot >= 3)
        tag = "" if not hits else ("**" if hits[0][3] else "")
        br = f"{bracket}" if bracket is not None else "-"
        print(f"{(fname+':'+name)[:34]:34s} {value:>7d} {br:>7s}  {tag}{shown}")
        report.append({"file": fname, "name": name, "value": value, "bracket": bracket,
                       "hits": [{"value": c, "delta": d, "total": tot, "d3d": d3d}
                                for c, d, tot, d3d in hits]})
    if "--json" in sys.argv:
        out = pathlib.Path(sys.argv[sys.argv.index("--json") + 1])
        out.write_text(json.dumps(report, indent=1), encoding="utf-8")
        print(f"\n写入 {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())