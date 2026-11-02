#!/usr/bin/env python3
"""T2ROTF Renut/deko3d D3D 入口发现工具。

字节源规则（已在 3 处独立验证）：
    extracted/TU/default_tu.flat 的文件偏移 = guest_vaddr - 0x82000000
    （验证点：0x2D4488 sub_822D4488 序言+体内联指令逐字对齐；
              0x2D363C = sub_822D3630 +0xC 的 lwz r11,10941(r3)；
              0x2CA02C = sub_822C9CB0 + 223*4 的 ori r11,r11,8705）

子命令：
  --dump ADDR LEN        按 vaddr 转储字节并逐条解码
  --ptrtables            在 .rdata/.data 扫"连续合法函数指针串"（D3DRS/sampler 派发表候选）
  --fn ADDR              用 t2_index.json 打印该函数的指令文本
  --refs ADDR            在 index 中找引用该数据地址的函数（表基址引用者）
"""
import argparse, json, os, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE = os.path.join(ROOT, "extracted", "TU", "default_tu.flat")
IMAGE_BASE = 0x82000000
DEFAULT_INDEX = os.path.join(ROOT, "out", "t2-tools", "t2_index.json")
TEXT_LO, TEXT_HI = 0x82010000, 0x82C00000


def load_image(path=IMAGE):
    with open(path, "rb") as fh:
        return fh.read()


def read_words(buf, vaddr, n):
    off = vaddr - IMAGE_BASE
    return [struct.unpack_from(">I", buf, off + 4 * i)[0] for i in range(n)]


def load_index(path=DEFAULT_INDEX):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def key_of(addr, idx):
    for cand in (f"{addr:08X}", f"{addr:08x}", f"0x{addr:x}", f"{addr:x}"):
        if cand in idx:
            return cand
    return None


def valid_target(addr, starts):
    """text 段内地址视为合法函数指针；命中函数起点集合则 stronger。"""
    if addr in starts:
        return 2
    if TEXT_LO <= addr < TEXT_HI:
        return 1
    return 0


def cmd_ptrtables(buf, args):
    starts = set()
    if os.path.exists(args.index):
        starts = {int(k, 16) if isinstance(k, str) else int(k) for k in load_index(args.index).keys()}
    print(f"[info] 函数起点集合 {len(starts)} 条")
    runs = []
    for lo, hi, name in ((0x82001000, 0x82105000, ".rdata"), (0x82BB0000, 0x82EA0000, ".data"),
                         (0x82000000, 0x82001000, "headers")):
        for vaddr in range(lo, hi, 4):
            off = vaddr - IMAGE_BASE
            if off + 4 > len(buf):
                break
            w = struct.unpack_from(">I", buf, off)[0]
            if valid_target(w, starts):
                runs.append((name, vaddr))
    # 合并成连续串
    groups = []
    prev = None
    for name, vaddr in runs:
        if prev and vaddr - prev == 4:
            groups[-1][2] += 1
        else:
            groups.append([name, vaddr, 1])
        prev = vaddr
    groups = [g for g in groups if g[2] >= args.min]
    groups.sort(key=lambda g: -g[2])
    print(f"[info] 长度 >= {args.min} 的指针串 {len(groups)} 条，前 40：")
    for name, vaddr, ln in groups[:40]:
        words = read_words(buf, vaddr, min(ln, 6))
        tail = read_words(buf, vaddr + 4 * (ln - 2), 2)
        print(f"  {name} @ {vaddr:#010x} len={ln:5d}  head={[hex(w) for w in words]} tail={[hex(w) for w in tail]}")
    for ln_target in (101, 100, 20, 64, 21, 19):
        sel = [g for g in groups if g[2] == ln_target]
        if sel:
            print(f"[match] 长度正好 {ln_target} 的串：{[(g[0], hex(g[1]), g[2]) for g in sel[:8]]}")
    return groups


def cmd_dump(buf, args):
    words = read_words(buf, args.addr, args.len // 4)
    for i, w in enumerate(words):
        print(f"  {args.addr + 4 * i:#010x}  {w:#010x}")


def cmd_fn(args):
    addr = args.fn
    idx = load_index(args.index)
    key = key_of(addr, idx)
    e = idx.get(key) if key else None
    if e is None:
        print(f"[miss] {addr:#x} 不在 index 中")
        return
    print(f"[fn] {addr:#x}  insns={e['n']}  callees={len(e['c'])}  tail={e['t']}")
    for line in e["i"]:
        print("   ", line)


def cmd_refs(args):
    idx = load_index(args.index)
    pat = re.compile(r"(?<![-0-9A-Fa-f])(?:0x)?0*%x\b" % args.addr, re.I)
    hits = []
    for k, e in idx.items():
        for line in e["i"]:
            if pat.search(line):
                hits.append((k, line))
                break
    print(f"[refs] {args.addr:#x} 被 {len(hits)} 个函数引用，前 60：")
    for k, line in hits[:60]:
        print(f"  {k}  <- {line}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", default=IMAGE)
    ap.add_argument("--index", default=DEFAULT_INDEX)
    ap.add_argument("--ptrtables", action="store_true")
    ap.add_argument("--min", type=int, default=8)
    ap.add_argument("--dump", nargs=2, type=lambda s: int(s, 0), metavar=("ADDR", "LEN"))
    ap.add_argument("--fn", type=lambda s: int(s, 0))
    ap.add_argument("--refs", type=lambda s: int(s, 0))
    args = ap.parse_args()
    buf = load_image(args.image)
    if args.ptrtables:
        cmd_ptrtables(buf, args)
    if args.dump:
        args.addr, args.len = args.dump
        cmd_dump(buf, args)
    if args.fn is not None:
        cmd_fn(args)
    if args.refs is not None:
        cmd_refs(args)


if __name__ == "__main__":
    main()