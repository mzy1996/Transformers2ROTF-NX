"""Census of T2ROTF's D3DDevice private field accesses.

Every load/store in the generated listing whose displacement falls in the device's
private tail (0x2000-0x6200) is counted per offset, so the image itself says which
offsets the device code reads and writes and how often. Guest field offsets are
validated against the same image used by tools/t2_renut_hooks.py.

  python tools/t2_private_offsets.py --min 3            # offsets seen >= 3 times
  python tools/t2_private_offsets.py --at 0x2E6C        # who touches one offset
  python tools/t2_private_offsets.py --function 0x822C7548
  python tools/t2_private_offsets.py --nb               # N&B field -> T2 offset match

--base keeps only accesses based on one of the registers that hold the device
(bcctr motion, so r3 and the saved copy in callee-saved registers).  --d3d-block
keeps only functions inside the guest's D3D library.
"""
from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INDEX = ROOT / "out/t2-tools/t2_index.json"

ACCESS = re.compile(r"^(lwz|lbz|lhz|stb|sth|stw|stfs|lfs|ld|std|stfd|lfd|lmw|stmw)\s+"
                    r"(\w+),(-?\d+)\((\w+)\)$")
LO = 0x2000
HI = 0x6200
DEVICE_REGS = {"r3", "r29", "r30", "r31"}
D3D_LO, D3D_HI = 0x822C0000, 0x822E0000

# N&B's (the Banjo Renut reference) field offsets, from the [brackets] in
# d3d.h and from d3d_native_state.cpp's constants.  Matched against T2's image to
# see which delta T2 uses per field - the tail is not a uniform translation.
NB_FIELDS: list[tuple[str, int]] = [
    ("VertexDeclaration", 0x2E2C),
    ("StreamDeclStrides", 0x2E38),
    ("BlendState", 0x2E48),
    ("BlendFlags", 0x2E4C),
    ("ScissorTestEnable", 0x2E50),
    ("ColorWriteEnable[4]", 0x2E54),
    ("ZEnable", 0x2E64),
    ("StencilEnable", 0x2E68),
    ("PointSpriteEnable", 0x2E6C),
    ("PointSize", 0x2E74),
    ("PointSizeMin", 0x2E78),
    ("PointSizeMax", 0x2E7C),
    ("MaxAnisotropy[26]", 0x2E94),
    ("MaxMipLevel", 0x2EAE),
    ("MinMipLevel", 0x2EC8),
    ("ZFilter", 0x2EE2),
    ("HighPrecisionBlend[4]", 0x2EFC),
    ("GprAllocation", 0x2AA8),
    ("ScreenExtentQueryMode", 0x2AA4),
    ("FenceTag", 0x2A9C),
    ("SwapFenceTag", 0x2AB0),
    ("SwapFlags", 0x2ABD),
    ("Viewport", 0x3168),
    ("IndexBuffer", 0x3094),
    ("RenderTargets[4]", 0x3098),
    ("DepthStencil", 0x30A8),
    ("StreamSources[16]", 0x30AC),
    ("StreamStrides[16]", 0x30F0),
    ("Textures[26]", 0x3100),
    ("PixelShader", 0x3194),
    ("VertexShader", 0x3198),
    ("Predication", 0x31A4),
    ("SnapshotSize", 0x31A8),
    ("VerticesRingEnd", 0x3484),
    ("CommandBuffer", 0x34CC),
    ("PresentInterval", 0x351C),
    ("PresentFenceTag", 0x3A5C),
    ("SwapCount", 0x40B0),
    ("BehaviorFlags2", 0x5DAC),
]


def load_index() -> dict[str, dict]:
    return json.loads(INDEX.read_text(encoding="utf-8"))


def collect(args, raw) -> dict[int, list[tuple[int, str]]]:
    per_offset: dict[int, list[tuple[int, str]]] = defaultdict(list)
    for key, body in raw.items():
        va = int(key, 16)
        if args.d3d_block and not (D3D_LO <= va < D3D_HI):
            continue
        for ins in body["i"]:
            match = ACCESS.match(ins)
            if not match:
                continue
            if args.base != "any" and match[4] not in DEVICE_REGS:
                continue
            offset = int(match[3])
            if not (args.range[0] <= offset < args.range[1]):
                continue
            if args.status and not match[1].startswith("st"):
                continue
            per_offset[offset].append((va, ins))
    return per_offset


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--min", type=int, default=1)
    parser.add_argument("--at", type=lambda x: int(x, 0))
    parser.add_argument("--function", type=lambda x: int(x, 0))
    parser.add_argument("--nb", action="store_true")
    parser.add_argument("--range", nargs=2, type=lambda x: int(x, 0), default=(LO, HI))
    parser.add_argument("--base", default="device", help="device | any")
    parser.add_argument("--d3d-block", action="store_true", default=True)
    parser.add_argument("--all-blocks", dest="d3d_block", action="store_false")
    parser.add_argument("--status", action="store_true")
    args = parser.parse_args()

    raw = load_index()
    per_offset = collect(args, raw)

    if args.function is not None:
        by_addr = {int(k, 16): body for k, body in raw.items()}
        body = by_addr.get(args.function)
        if body is None:
            raise SystemExit(f"0x{args.function:08x}: not in the listing")
        print(f"0x{args.function:08x} n={body['n']}")
        for ins in body["i"]:
            access = ACCESS.match(ins)
            mark = "   <-- device private" if (
                access and args.range[0] <= int(access[3]) < args.range[1]
                and access[4] in DEVICE_REGS) else ""
            print(f"  {ins}{mark}")
        return

    if args.at is not None:
        rows = per_offset.get(args.at, [])
        print(f"0x{args.at:04X} ({args.at}): {len(rows)} accesses")
        for va, ins in sorted(rows):
            print(f"  0x{va:08x}  {ins}")
        return

    if args.nb:
        print("N&B field                    N&B     +0    +4    +8   T2 evidence (functions at +0/+4/+8)")
        for name, nb in NB_FIELDS:
            counts = []
            for delta in (0, 4, 8):
                counts.append(len(per_offset.get(nb + delta, [])))
            best = max(range(3), key=lambda i: counts[i])
            off = nb + (0, 4, 8)[best]
            fns = sorted({va for va, _ in per_offset.get(off, [])})
            sample = ",".join(f"{va:08x}" for va in fns[:3])
            print(f"{name:28s} 0x{nb:04X}  {counts[0]:4d}  {counts[1]:4d}  {counts[2]:4d}   "
                  f"-> 0x{off:04X} ({off}) {sample}")
        return

    ranked = sorted(per_offset.items(), key=lambda kv: -len(kv[1]))
    total = sum(len(v) for v in per_offset.values())
    print(f"{len(ranked)} private offsets, {total} accesses, range "
          f"[{args.range[0]:#x}, {args.range[1]:#x}), base={args.base}, "
          f"d3d_block={args.d3d_block}")
    for offset, rows in ranked:
        if len(rows) < args.min:
            continue
        functions = sorted({va for va, _ in rows})
        sample = ", ".join(f"0x{va:08x}" for va in functions[:6])
        more = " ..." if len(functions) > 6 else ""
        print(f"0x{offset:04X} ({offset:5d})  n={len(rows):4d}  fns={len(functions):3d}  {sample}{more}")


if __name__ == "__main__":
    main()