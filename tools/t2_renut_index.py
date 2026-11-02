#!/usr/bin/env python3
"""Index of T2ROTF's recompiled functions for the Renut/deko3d retargeting.

The generator keeps every PowerPC instruction of the original XEX as a `// <insn>` comment above the C it emitted,
so the original listing of any function can be read back without the image. This tool indexes that:

  python tools/t2_renut_index.py --build [out.json]        build the index (address -> listing, calls, immediates)
  python tools/t2_renut_index.py --fn 822D4488 [--max 60]  print the listing of one or more functions
  python tools/t2_renut_index.py --range 822C0000 822E0000 [--max 60] [--head 4]
                                                           list the functions of an address range
  python tools/t2_renut_index.py --callers 822D13F0        who calls a function (guest calls only)
  python tools/t2_renut_index.py --stats                   function density per 64 KB, to find the library blocks
  python tools/t2_renut_index.py --grep 'lis r[0-9]+,0x3d' [--max 40]
                                                           search instruction text across every function
  python tools/t2_renut_index.py --who 822D4488            calls inside and outside the function

The index is built from recomp/generated/default/T2ROTF_recomp.*.cpp (49 files, ~31k functions). Instruction text is
truncated to MAX_INS for the index (n keeps the true count); --fn dumps what the index holds.
"""
import argparse
import collections
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'recomp', 'generated', 'default')
DEFAULT_INDEX = os.path.join(ROOT, 'out', 't2-tools', 't2_index.json')

DEF = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)\s*\{', re.M)
INSN = re.compile(r'^\s*//\s+(\S.*?)\s*$')
CALL_HOST = re.compile(r'(?<![\w])(?:__imp__)?sub_([0-9A-F]{8})\(ctx, base\)')
CALL_INDIRECT = re.compile(r'REX_CALL_INDIRECT_FUNC')
TAIL = re.compile(r'//\s*(blr|bctr|bctrl)\s*$')
MMIO = re.compile(r'REX_MM_(?:STORE|LOAD)')

MAX_INS = 400


def build():
    files = sorted(glob.glob(os.path.join(GEN, 'T2ROTF_recomp.*.cpp')))
    if not files:
        sys.exit('no generated code under ' + GEN)
    idx = {}
    for path in files:
        cur = None
        insns = []
        calls = []
        body_tail = []
        n = 0
        with open(path, encoding='utf-8', errors='replace') as fh:
            for ln in fh:
                m = DEF.match(ln)
                if m:
                    if cur is not None:
                        idx[cur] = mk(insns, calls, n, body_tail)
                    cur, insns, calls, body_tail, n = m.group(1), [], [], [], 0
                    continue
                if cur is None:
                    continue
                if ln.startswith('}'):
                    idx[cur] = mk(insns, calls, n, body_tail)
                    cur = None
                    continue
                s = ln.strip()
                if s.startswith('// '):
                    n += 1
                    if len(insns) < MAX_INS:
                        insns.append(s[3:])
                    mm = INSN.match(s)
                    if mm and mm.group(1).split()[0] in ('blr', 'bctr', 'bctrl'):
                        body_tail.append(mm.group(1).split()[0])
                    continue
                if 'sub_' in s and '(ctx, base)' in s:
                    cm = CALL_HOST.search(s)
                    if cm:
                        calls.append(cm.group(1))
                if CALL_INDIRECT.search(s):
                    body_tail.append('indirect')
    if cur is not None:
        idx[cur] = mk(insns, calls, n, body_tail)
    os.makedirs(os.path.dirname(DEFAULT_INDEX), exist_ok=True)
    with open(DEFAULT_INDEX, 'w', encoding='utf-8') as fh:
        json.dump(idx, fh)
    print(f'{len(files)} files -> {len(idx)} functions -> {DEFAULT_INDEX} '
          f'({os.path.getsize(DEFAULT_INDEX) / 1e6:.1f} MB)')


def mk(insns, calls, n, tail):
    return {'n': n, 'i': insns, 'c': sorted(set(calls)), 't': ''.join(sorted(set(tail)))}


def load(path=None):
    path = path or DEFAULT_INDEX
    if not os.path.exists(path):
        sys.exit(f'no index at {path}; run --build')
    return json.load(open(path, encoding='utf-8'))


def addr(a):
    return a.upper().replace('0X', '')


def dump(rec, key, full=False):
    print(f'--- sub_{key}  ({rec["n"]} instructions, {len(rec["c"])} guest calls'
          f'{", ends " + rec["t"] if rec["t"] else ""}) ---')
    ins = rec['i'] if full else rec['i'][:rec.get('_cap', 0) or len(rec['i'])]
    for i, ln in enumerate(ins):
        print(f'  {i:4d}  {ln}')
    if not full and rec['n'] > len(ins):
        print(f'  ... {rec["n"] - len(ins)} more instructions')
    if rec['c']:
        print('  calls: ' + ' '.join('sub_' + c for c in rec['c']))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', nargs='?', const=DEFAULT_INDEX)
    ap.add_argument('--index', default=DEFAULT_INDEX)
    ap.add_argument('--fn', nargs='+')
    ap.add_argument('--range', nargs=2)
    ap.add_argument('--callers')
    ap.add_argument('--stats', action='store_true')
    ap.add_argument('--grep')
    ap.add_argument('--who')
    ap.add_argument('--max', type=int, default=40)
    ap.add_argument('--head', type=int, default=0)
    ap.add_argument('--full', action='store_true')
    a = ap.parse_args()
    if a.build:
        build()
        return
    idx = load(a.index)
    if a.stats:
        pages = collections.Counter()
        for k, v in idx.items():
            pages[int(k, 16) >> 16] += 1
        print('page  funcs   bar')
        for p, c in sorted(pages.items()):
            print(f'{p << 16:08X}  {c:5d}  {"#" * min(60, c // 4)}')
        print(f'total {len(idx)} functions')
        return
    if a.grep:
        rx = re.compile(a.grep)
        hits = 0
        for k in sorted(idx):
            for ln in idx[k]['i']:
                if rx.search(ln):
                    print(f'sub_{k}: {ln}')
                    hits += 1
                    break
            if hits >= a.max:
                break
        print(f'{hits} functions match (of {len(idx)}), showing at most {a.max}')
        return
    if a.fn:
        for x in a.fn:
            k = addr(x)
            if k in idx:
                dump(idx[k], k, a.full)
            else:
                print(f'--- sub_{k}  NOT IN INDEX')
        return
    if a.range:
        lo, hi = int(addr(a.range[0]), 16), int(addr(a.range[1]), 16)
        keys = sorted(k for k in idx if lo <= int(k, 16) < hi)
        print(f'{len(keys)} functions in [{lo:08X},{hi:08X})')
        for k in keys[:a.max]:
            rec = idx[k]
            head = ' ; '.join(rec['i'][:a.head]) if a.head else ''
            print(f'  sub_{k}  n={rec["n"]:5d}  calls={len(rec["c"]):3d}  {head[:120]}')
        if len(keys) > a.max:
            print(f'  ... {len(keys) - a.max} more')
        return
    if a.callers:
        k = addr(a.callers)
        out = [x for x, v in idx.items() if k in v['c']]
        print(f'{len(out)} functions call sub_{k}:')
        for x in sorted(out)[:a.max]:
            print(f'  sub_{x}  n={idx[x]["n"]}')
        return
    if a.who:
        k = addr(a.who)
        rec = idx[k]
        n = int(k, 16)
        inside = [c for c in rec['c'] if abs(int(c, 16) - n) < 0x8000]
        print(f'sub_{k}: {len(rec["c"])} calls, {len(inside)} within 32 KB (library-local): '
              + ' '.join('sub_' + c for c in inside[:20]))
        out = [x for x, v in idx.items() if k in v['c']]
        print(f'callers: {len(out)} ' + ' '.join('sub_' + x for x in sorted(out)[:20]))
        return
    ap.print_help()


if __name__ == '__main__':
    main()