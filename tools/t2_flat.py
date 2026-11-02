#!/usr/bin/env python3
"""PE(view) reader for extracted/base_default.flat: vaddr<->file offset, dumps, pointer-table mining.

The .flat is a PE32 image with ImageBase 0x82000000; section VAs are RVAs.
    t2_flat.py --sections
    t2_flat.py --dump 822D4488 24
    t2_flat.py --check 822D4488 822C9CB0 ...      # compare flat bytes vs the recompiled index prologue
    t2_flat.py --ptrtables [--min 8] [--show 6]
    t2_flat.py --fnv 822D4488 64
"""
import argparse, json, os, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FLAT = os.path.join(ROOT, 'extracted', 'base_default.flat')
INDEX = os.path.join(ROOT, 'out', 't2-tools', 't2_index.json')
IMAGE_BASE = 0x82000000


def sections():
    d = open(FLAT, 'rb').read()
    e = struct.unpack_from('<I', d, 0x3C)[0]
    n = struct.unpack_from('<H', d, e + 6)[0]
    o = struct.unpack_from('<H', d, e + 20)[0]
    base = struct.unpack_from('<I', d, e + 24 + 28)[0]
    off = e + 24 + o
    out = []
    for _ in range(n):
        name = d[off:off + 8].rstrip(b'\0').decode('latin1')
        vs, va, rs, ro = struct.unpack_from('<IIII', d, off + 8)
        out.append({'name': name, 'rva': va, 'vsz': vs, 'ro': ro, 'rsz': rs})
        off += 40
    return d, base, out


D, BASE, SECS = sections()


def v2o(va):
    rva = va - BASE
    for s in SECS:
        if s['rva'] <= rva < s['rva'] + max(s['vsz'], s['rsz']) and rva - s['rva'] < s['rsz']:
            return s['ro'] + (rva - s['rva']), s
    return None, None


def fnv32(b, h=0x811C9DC5):
    for c in b:
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


def load_index():
    if not os.path.exists(INDEX):
        sys.exit('missing %s (run tools/t2_renut_index.py --build)' % INDEX)
    return json.load(open(INDEX, encoding='utf-8'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sections', action='store_true')
    ap.add_argument('--dump')
    ap.add_argument('--n', type=int, default=16)
    ap.add_argument('--check', nargs='*')
    ap.add_argument('--ptrtables', action='store_true')
    ap.add_argument('--min', type=int, default=8)
    ap.add_argument('--show', type=int, default=6)
    ap.add_argument('--only', help='section name filter for --ptrtables')
    ap.add_argument('--fnv')
    a = ap.parse_args()
    if a.sections:
        for s in SECS:
            print('%-9s RVA=0x%08X VA=0x%08X VSize=0x%X RawOff=0x%X RawSize=0x%X' %
                  (s['name'], s['rva'], BASE + s['rva'], s['vsz'], s['ro'], s['rsz']))
        return
    if a.dump:
        va = int(a.dump, 16)
        off, s = v2o(va)
        if off is None:
            sys.exit('not mapped')
        print('// %s +0x%X' % (s['name'], va - BASE - s['rva']))
        for i in range(a.n):
            w = struct.unpack_from('>I', D, off + 4 * i)[0]
            print('  0x%08X: %08X' % (va + 4 * i, w))
        return
    if a.check:
        idx = load_index()
        for h in a.check:
            va = int(h, 16)
            k = '%08X' % va
            off, s = v2o(va)
            w = [struct.unpack_from('>I', D, off + 4 * i)[0] for i in range(2)] if off is not None else []
            print('sub_%s  idx=%s\n         flat=%s  [%s]' % (k, idx.get(k, {}).get('i', [])[:2],
                                                             ['%08X' % x for x in w], s['name'] if s else '?'))
        return
    if a.fnv:
        va = int(a.fnv, 16)
        off, s = v2o(va)
        b = D[off:off + a.n]
        print('0x%08X bytes=%u fnv32=0x%08X' % (va, a.n, fnv32(b)))
        return
    if a.ptrtables:
        starts = {int(k, 16) for k in load_index()}
        for s in SECS:
            if a.only and s['name'] != a.only:
                continue
            if s['name'] not in ('.rdata', '.data'):
                continue
            raw = D[s['ro']:s['ro'] + s['rsz']]
            runs, run, st = [], 0, 0
            for i in range(0, len(raw) - 3, 4):
                v = struct.unpack_from('>I', raw, i)[0]
                if v in starts:
                    if run == 0:
                        st = i
                    run += 1
                else:
                    if run >= a.min:
                        runs.append((st, run))
                    run = 0
            if run >= a.min:
                runs.append((st, run))
            runs.sort(key=lambda x: -x[1])
            print('== %s: %d runs >= %d ==' % (s['name'], len(runs), a.min))
            for st, r in runs[:a.show]:
                va = BASE + s['rva'] + st
                ent = [struct.unpack_from('>I', raw, st + 4 * k)[0] for k in range(min(r, 4))]
                print('   @0x%08X len=%d entries=[%s ...]' % (va, r, ' '.join('%08X' % e for e in ent)))
        return
    ap.print_help()


if __name__ == '__main__':
    main()
