#!/usr/bin/env python3
"""T2ROTF (2026-10-25): dumps every Xenos shader container T2's game files hold, into the directory
Renut's shader cache is built from.

Same role as banjo-nx/sa2-nx/mgr-nx-deko's tools/dump_shaders.py (which those ports do not ship: the
containers are game data). This port has no dump of them, and the black-screen state means the
runtime capture (renut_shader_capture_dir) is not usable yet, so they are swept out of the game's own
files instead: a complete sweep of what the game ships is what makes "MissingGameShaders() == 0"
provable - the game can only register a container it has on the disc.

A container is XDK's shader container: big-endian dwords, flags & 0xFFFFFF00 == 0x102A1100 (bit 0
tells a vertex shader from a pixel shader), and a length of virtualSize + physicalSize from its first
byte. The bytes are validated exactly as repos/mgr-nx-deko/tools/mgr_chapter_cache.py's
validate_container does (magic and length, the shader header, its sequencer, and for a vertex shader
the vertex declaration), because a magic that appears by chance inside a texture or an audio bank
must never reach the recompiler.

Each container is written as {vs|ps}_{hash:016x}.bin, hash = XXH3-64 of the whole container - the key
Renut's cache and shader_library.cpp's CaptureMissingShader use, so a file from here and a runtime
capture of the same shader are the same name.

The recompiler reads every file of a directory and finds the containers inside it, so one .bin per
container is exact (and keeps the report readable). Containers are game data: the build tree only.

    python tools/t2_sweep_shader_containers.py --out out/t2-deko-shaders/containers
    python tools/t2_sweep_shader_containers.py --out DIR --assets game --max-bytes 67108864 \\
        --also extracted/TU/default_tu.flat --also extracted/base_default.flat
"""
import argparse
import csv
import hashlib
import json
import mmap
import os
import struct
import sys

import xxhash

MAGICS = (b'\x10\x2a\x11\x00', b'\x10\x2a\x11\x01')  # pixel shader, vertex shader
MAX_CONTAINER = 8 << 20
DEFAULT_MAX_BYTES = 64 << 20
MIN_CONTAINER = 28


def validate(data):
    """(stage, length) of the container at data[0:], or None. Mirrors mgr_chapter_cache.validate_container."""
    if len(data) < MIN_CONTAINER or len(data) > MAX_CONTAINER:
        return None
    magic, virtual, physical = struct.unpack_from('>III', data)
    if magic & 0xFFFFFF00 != 0x102A1100 or virtual < MIN_CONTAINER or virtual + physical != len(data):
        return None
    header = struct.unpack_from('>I', data, 24)[0]
    if header > virtual or virtual - header < 8:
        return None
    offset, size = struct.unpack_from('>II', data, header)
    if not size or size % 12 or offset > physical or size > physical - offset:
        return None
    if magic & 1:  # vertex shader: the shader header carries a vertex declaration
        if virtual - header < 36:
            return None
        defs, fetches = struct.unpack_from('>II', data, header + 24)
        if header + 36 + defs * 4 + fetches * 4 > virtual:
            return None
    return ('vs' if magic & 1 else 'ps')


def candidates(path, max_bytes):
    """(name, size) pairs of the files to sweep under path, bounded by max_bytes, plus what was skipped."""
    if os.path.isfile(path):
        size = os.path.getsize(path)
        return ([(path, size)] if size <= max_bytes else []), ([] if size <= max_bytes else [path])
    files, skipped = [], []
    for root, _dirs, names in os.walk(path):
        for name in sorted(names):
            full = os.path.join(root, name)
            if os.path.islink(full):
                continue
            try:
                size = os.path.getsize(full)
            except OSError:
                continue
            (files if size <= max_bytes else skipped).append((full, size) if size <= max_bytes else full)
    return files, skipped


def sweep_file(path, out_dir, found, duplicates, out_root):
    """Adds every container of one file to found, writes the new ones. Returns (containers, bad magics)."""
    size = os.path.getsize(path)
    if size < MIN_CONTAINER:
        return 0, 0
    hits, bad = 0, 0
    with open(path, 'rb') as stream:
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            for magic in MAGICS:
                offset = 0
                while True:
                    offset = data.find(magic, offset)
                    if offset < 0:
                        break
                    start = offset
                    offset += 4
                    if len(data) - start < MIN_CONTAINER:
                        continue
                    virtual, physical = struct.unpack_from('>II', data, start + 4)
                    length = virtual + physical
                    if length < MIN_CONTAINER or length > MAX_CONTAINER or length > len(data) - start:
                        bad += 1
                        continue
                    container = data[start:start + length]
                    stage = validate(container)
                    if stage is None:
                        bad += 1
                        continue
                    digest = hashlib.sha256(container).hexdigest()
                    if digest in found:
                        duplicates.append({'sha256': digest, 'hash': found[digest]['hash'], 'source': out_root(path),
                                           'offset': start, 'stage': stage, 'bytes': length})
                        hits += 1
                        continue
                    hash64 = xxhash.xxh3_64_intdigest(container)
                    entry = {'hash': hash64, 'stage': stage, 'bytes': length, 'sha256': digest,
                             'source': out_root(path), 'offset': start}
                    found[digest] = entry
                    name = f'{stage}_{hash64:016x}.bin'
                    entry['file'] = name
                    with open(os.path.join(out_dir, name + '.tmp'), 'wb') as out:
                        out.write(container)
                    os.replace(os.path.join(out_dir, name + '.tmp'), os.path.join(out_dir, name))
                    hits += 1
    return hits, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default='out/t2-deko-shaders/containers',
                    help='where the containers are written (RENUT_SHADER_DUMP_DIR)')
    ap.add_argument('--assets', default='game', help="the game's assets directory (RENUT_ASSETS_DIR)")
    ap.add_argument('--also', nargs='*', default=['extracted/TU/default_tu.flat', 'extracted/base_default.flat',
                                                  'extracted/DLC'],
                    help='more files or directories to sweep (the TU image, the DLC)')
    ap.add_argument('--max-bytes', type=lambda s: int(s, 0), default=DEFAULT_MAX_BYTES,
                    help='files above this are skipped (audio banks, movies)')
    ap.add_argument('--include-debug', action='store_true',
                    help="also sweep the *.debug.lsa debug shader libraries (not shipped in the retail build)")
    a = ap.parse_args()

    out_dir = os.path.abspath(a.out)
    os.makedirs(out_dir, exist_ok=True)
    root = os.getcwd()

    def out_root(path):
        try:
            return os.path.relpath(path, root).replace('\\', '/')
        except ValueError:
            return path.replace('\\', '/')

    targets, skipped = candidates(a.assets, a.max_bytes)
    for extra in a.also:
        if not os.path.exists(extra):
            print(f't2_sweep_shader_containers: {extra} does not exist, skipped', file=sys.stderr)
            continue
        more, big = candidates(extra, a.max_bytes)
        targets += more
        skipped += big
    if not a.include_debug:
        debug = [t for t in targets if t[0].lower().endswith('.debug.lsa')]
        targets = [t for t in targets if not t[0].lower().endswith('.debug.lsa')]
    else:
        debug = []

    found, duplicates = {}, []
    per_file, bad_total, containers_total = [], 0, 0
    for path, size in targets:
        hits, bad = sweep_file(path, out_dir, found, duplicates, out_root)
        containers_total += hits
        bad_total += bad
        if hits or bad:
            per_file.append({'source': out_root(path), 'bytes': size, 'containers': hits, 'rejected': bad})

    if found:
        for name in os.listdir(out_dir):
            if name.endswith('.bin') and not any(e['file'] == name for e in found.values()):
                os.remove(os.path.join(out_dir, name))

    manifest = os.path.join(out_dir, 'manifest.csv')
    with open(manifest, 'w', newline='', encoding='utf-8') as out:
        writer = csv.writer(out)
        writer.writerow(['file', 'stage', 'hash', 'hash_hex', 'bytes', 'sha256', 'source', 'offset'])
        for entry in sorted(found.values(), key=lambda e: (e['stage'], e['hash'])):
            writer.writerow([entry['file'], entry['stage'], entry['hash'], f"{entry['hash']:016x}",
                             entry['bytes'], entry['sha256'], entry['source'], entry['offset']])

    report = {'assets': out_root(os.path.abspath(a.assets)), 'out': out_root(out_dir),
              'files_swept': len(targets), 'containers': len(found), 'occurrences': containers_total,
              'rejected_magics': bad_total, 'duplicate_occurrences': len(duplicates),
              'files_above_max_bytes': len(skipped),
              'debug_libraries_excluded': [out_root(p) for p, _ in debug],
              'per_file': per_file,
              'skipped_largest': sorted((out_root(p) if isinstance(p, str) else out_root(p[0])
                                         for p in skipped))[:10],
              'duplicates': duplicates}
    with open(os.path.join(out_dir, '..', 'sweep_report.json'), 'w', encoding='utf-8') as out:
        json.dump(report, out, indent=2)

    by_stage = {}
    by_bytes = 0
    for entry in found.values():
        by_stage[entry['stage']] = by_stage.get(entry['stage'], 0) + 1
        by_bytes += entry['bytes']
    for item in sorted(per_file, key=lambda e: -e['containers'])[:10]:
        print(f"  {item['source']:34s} {item['containers']:>4d} containers, {item['rejected']:>3d} magic(s) rejected")
    print(f'{len(found)} containers ({by_stage}) in {by_bytes >> 10} KB, {containers_total} occurrences, '
          f'{bad_total} magics rejected, {len(skipped)} files above {a.max_bytes >> 20} MB, '
          f'{len(debug)} debug librar(ies) excluded -> {out_root(out_dir)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())