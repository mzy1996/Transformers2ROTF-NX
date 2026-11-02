#!/usr/bin/env python3
"""Locate a runtime offset (image+N reported by the Switch crash handler) inside
the build ELF of this project.

Usage:
  python tools/t2_crash_sym.py --elf <elf> --off 0x2dbe354 [--off 0x...]

For every target offset it tries a list of candidate biases, i.e.
runtime_off = ELF_vaddr + bias, and prints the containing / nearest preceding
symbol. The correct bias shows up as the candidate whose symbol names are
coherent (a recompiled sub_XXXXXXXX or a real runtime function).
"""
import bisect
import struct
import sys

CAND_BIASES = [0x100000, 0, 0x1000, 0x80, 0x100, 0x20, -0x100000]


def load_elf(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF':
        raise SystemExit('not an ELF: %s' % path)
    if data[4] != 2 or data[5] != 1:
        raise SystemExit('expect ELF64 little-endian')
    e_phoff, = struct.unpack_from('<Q', data, 0x20)
    e_shoff, = struct.unpack_from('<Q', data, 0x28)
    e_phentsize, = struct.unpack_from('<H', data, 0x36)
    e_phnum, = struct.unpack_from('<H', data, 0x38)
    e_shentsize, = struct.unpack_from('<H', data, 0x3A)
    e_shnum, = struct.unpack_from('<H', data, 0x3C)
    e_shstrndx, = struct.unpack_from('<H', data, 0x3E)
    phdrs = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        vals = struct.unpack_from('<IIQQQQQQ', data, o)
        phdrs.append(dict(type=vals[0], flags=vals[1], off=vals[2],
                          vaddr=vals[3], filesz=vals[5], memsz=vals[6],
                          align=vals[7]))
    shdrs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        vals = struct.unpack_from('<IIQQQQIIQQ', data, o)
        shdrs.append(dict(name_off=vals[0], type=vals[1], off=vals[4],
                          size=vals[5], addr=vals[3], link=vals[6]))
    shstr = b''
    if e_shstrndx < len(shdrs):
        s = shdrs[e_shstrndx]
        shstr = data[s['off']:s['off'] + s['size']]

    def namestr(buf, off):
        e = buf.find(b'\x00', off)
        return buf[off:e].decode('utf-8', 'replace')

    for s in shdrs:
        s['name'] = namestr(shstr, s['name_off']) if shstr else ''
    syms = []
    for s in shdrs:
        if s['type'] != 2:
            continue
        link = shdrs[s['link']]
        strtab = data[link['off']:link['off'] + link['size']]
        for i in range(s['size'] // 24):
            o = s['off'] + i * 24
            st_name, st_info, _other, _shndx, st_value, st_size = \
                struct.unpack_from('<IBBHQQ', data, o)
            syms.append((st_value, st_size, namestr(strtab, st_name),
                         st_info))
    return data, phdrs, shdrs, syms


def main():
    args = sys.argv[1:]
    elf = None
    offs = []
    while args:
        a = args.pop(0)
        if a == '--elf':
            elf = args.pop(0)
        elif a == '--off':
            offs.append(int(args.pop(0), 16))
        else:
            raise SystemExit('unknown arg %r' % a)
    if not elf or not offs:
        raise SystemExit(__doc__)
    data, phdrs, shdrs, syms = load_elf(elf)
    print('ELF %s  file size=0x%X (%d bytes)' % (elf, len(data), len(data)))
    print('--- program headers ---')
    for p in phdrs:
        print('  type=%d flags=0x%X off=0x%X vaddr=0x%X filesz=0x%X '
              'memsz=0x%X align=0x%X' % (p['type'], p['flags'], p['off'],
                                         p['vaddr'], p['filesz'],
                                         p['memsz'], p['align']))
    print('--- sections (non-empty) ---')
    for s in shdrs:
        if s['size']:
            print('  %-22s type=%d addr=0x%-10X off=0x%-10X size=0x%X'
                  % (s['name'], s['type'], s['addr'], s['off'], s['size']))
    funcs = [(a, sz, n) for (a, sz, n, i) in syms if (i & 0xF) == 2]
    notypes = [(a, sz, n) for (a, sz, n, i) in syms if (i & 0xF) == 0 and sz]
    funcs.sort(key=lambda t: t[0])
    notypes.sort(key=lambda t: t[0])
    faddr = [t[0] for t in funcs]
    naddr = [t[0] for t in notypes]
    print('--- symbols: %d total, %d FUNC, %d NOTYPE-with-size ---'
          % (len(syms), len(funcs), len(notypes)))
    for off in offs:
        print('=== image+0x%X ===' % off)
        for bias in CAND_BIASES:
            v = off - bias
            if v < 0:
                continue
            rows = []
            for tag, addrs, tab in (('FUNC', faddr, funcs),
                                    ('NOTYPE', naddr, notypes)):
                i = bisect.bisect_right(addrs, v) - 1
                if i < 0:
                    continue
                a, sz, n = tab[i]
                rows.append('%s %s (+0x%X, size=0x%X)'
                            % (tag, n, v - a, sz))
            print('  bias=+0x%-8X vaddr=0x%-9X  %s'
                  % (bias, v, ' | '.join(rows) if rows else '<none>'))


main()