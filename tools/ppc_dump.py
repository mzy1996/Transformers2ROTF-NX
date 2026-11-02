#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Dump recompiled guest functions from the XenosRecomp generated sources.

Usage:  python ppc_dump.py sub_824F3580 sub_824F0658 ...
        python ppc_dump.py --grep "8277C2"      (list DEFINE_REX_FUNC names matching)
        python ppc_dump.py --callers sub_824F3580   (list call sites + containing function)
"""
import sys, os, glob, re

ROOT = r"J:/XBOX360 port/Transformers Revenge of the Fallen/recomp/generated/default"
FILES = None

def files():
    global FILES
    if FILES is None:
        FILES = sorted(glob.glob(os.path.join(ROOT, "T2ROTF_recomp.*.cpp")))
        FILES += sorted(glob.glob(os.path.join(ROOT, "T2ROTF_init.cpp")))
    return FILES

def load():
    out = []
    for f in files():
        with open(f, "r", encoding="utf-8", errors="replace") as fh:
            out.append((f, fh.readlines()))
    return out

CACHE = None
def data():
    global CACHE
    if CACHE is None:
        CACHE = load()
    return CACHE

def locate(name):
    for f, lines in data():
        for i, ln in enumerate(lines):
            if ln.startswith("DEFINE_REX_FUNC(" + name + ")"):
                return f, i
    return None, None

def body(name, indent=""):
    f, i = locate(name)
    if f is None:
        return None
    lines = dict(data())[f]
    out = []
    j = i
    while j < len(lines):
        out.append(lines[j])
        if j > i and lines[j].rstrip("\n") == "}":
            break
        j += 1
    return "".join(out)

def callers(name):
    res = []
    for f, lines in data():
        cur = "?"
        for i, ln in enumerate(lines):
            m = re.match(r"DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]+)\)", ln)
            if m:
                cur = m.group(1)
            if re.search(r"\b" + name + r"\s*\(", ln) and "DEFINE_REX_FUNC(" not in ln:
                res.append((os.path.basename(f), i + 1, cur, ln.strip()))
    return res

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return
    if args[0] == "--grep":
        pat = args[1]
        for f, lines in data():
            for i, ln in enumerate(lines):
                m = re.match(r"DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]+)\)", ln)
                if m and re.search(pat, m.group(1), re.I):
                    print("%s:%d %s" % (os.path.basename(f), i + 1, m.group(1)))
        return
    if args[0] == "--callers":
        for name in args[1:]:
            print("#### callers of %s" % name)
            for r in callers(name):
                print("  %s:%d  in %s   %s" % r)
        return
    for name in args:
        b = body(name)
        if b is None:
            print("#### %s : NOT FOUND" % name)
            continue
        f, i = locate(name)
        print("#### %s  (%s:%d)" % (name, os.path.basename(f), i + 1))
        print(b)

main()