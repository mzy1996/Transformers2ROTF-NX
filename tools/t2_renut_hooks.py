"""Build T2ROTF's reNut D3D hook map from its own image and generated listing.

Mirrors tools/mgr_renut_hooks.py, but the addresses are T2ROTF's own. Two sources
are combined:

  * the render/sampler state hook table, extracted from T2's own code
    (out/t2-tools/t2_state_tables.json: the 101-entry render state table and the
    20-entry sampler state table, each {getter, setter, default}) with the role
    names taken from the reference engine's hook list (state.cpp), and
  * out/t2-tools/t2_renut_extra.json: the remaining D3D entry points
    (device/draw/shader/resource roles), each {address, role, witnesses}.

Every entry is checked against the generated PowerPC listing
(recomp/generated/default/T2ROTF_recomp.*.cpp, indexed in
out/t2-tools/t2_index.json): the address must be a DEFINE_REX_FUNC body and the
witness instructions must appear in it. The runtime guard is FNV-1 over the
first min(64, n*4) bytes of the image at guest_vaddr = 0x82000000 + file offset,
computed from extracted/TU/default_tu.flat (the TU-patched image codegen and the
runtime both use).

No reference address is inferred by adding a fixed displacement.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = 0x82000000
IMAGE = ROOT / "extracted/TU/default_tu.flat"
INDEX = ROOT / "out/t2-tools/t2_index.json"
TABLES = ROOT / "out/t2-tools/t2_state_tables.json"
EXTRA = ROOT / "out/t2-tools/t2_renut_extra.json"
STATE_CPP = ROOT / "recomp/src/renut_engine/gpu/d3d/hooks/state.cpp"
HEADER = ROOT / "recomp/src/renut_engine/gpu/d3d/t2_hook_map.h"
REPORT = ROOT / "out/t2-tools/t2_renut_hooks.json"

# The device / draw / shader / resource entry points Renut substitutes, with
# T2ROTF's own addresses. Every one is derived in this project's own reports
# (out/t2-tools/t2_roles_device.md, t2_roles_draw.md, t2-roles-state-api.md) from
# structure, parameter contracts and the PM4 packets the body emits - never by
# adding the reference's displacement to a reference address. The roles the
# reports leave as "unknown" are listed separately: Renut loses those hooks and
# hooks/device.cpp says so where the site would be.
ROLES: list[tuple[str, int]] = [
    # device.cpp
    ("Direct3D_CreateDevice", 0x822D1640),
    ("InitializeApiState", 0x822E0F60),
    ("InitializePresentationParameters", 0x822E1240),
    ("XenonGpu_Initialize", 0x822E0D78),
    ("HardwareDeviceSetup_reference", 0x822E1690),
    ("D3D_SynchronizeToPresentationInterval", 0x822D9838),
    ("D3D_CDevice_KickOff", 0x822D4388),
    ("D3D_CDevice_KickOffSegment", 0x822D4000),
    ("D3D_CDevice_BeginRingBig", 0x822D3058),
    ("D3D_RingBufferDeviceAllocate", 0x822D1AF0),
    ("D3D_CDevice_AllocateInlineData", 0x822D3630),
    ("D3D_CDevice_NewSegment", 0x822D37D0),
    ("D3DDevice_SetRingBufferParameters", 0x822D4A08),
    ("D3DDevice_Swap", 0x822D9D70),
    ("D3D_CDevice_BlockOnFence", 0x822D4488),
    ("D3D_CBlocker_Check", 0x822D13F0),
    ("D3DDevice_IsFencePending", 0x822D4920),
    # draw.cpp
    ("D3DDevice_DrawVertices", 0x822CA748),
    ("D3DDevice_DrawIndexedVertices", 0x822CAB38),
    ("D3DDevice_BeginVertices", 0x822C9CB0),
    ("D3DDevice_WindowScissor", 0x822C68B8),
    ("D3DDevice_ClearF", 0x822D8CD8),
    ("D3DDevice_Resolve", 0x822DB2D8),
    # resource.cpp / shader.cpp
    ("D3DResource_Destroy", 0x822CB9E0),
    ("XGRegisterVertexShader", 0x822C0A98),
    ("XGRegisterPixelShader", 0x822C07B0),
    ("CreateVertexDeclaration", 0x822C0F58),
]

ROLES_UNKNOWN = ["D3DDevice_InsertCallback", "EngineSwapProgress_reference"]

HOOK = re.compile(
    r"BANJO_RENUT_HOOK(?:_SHARED)?\(sub_[0-9A-Fa-f]+,\s*(SetRenderStateHook|GetRenderStateHook"
    r"|SetSamplerStateHook|GetSamplerStateHook)<(\d+)>\);\s*//\s*(\S+)"
)


# Any base register: the state setters often reach the device through a saved
# copy (r11 = dev + index * 24 in the sampler setters), so pinning the
# displacement matters, not the register the compiler chose.
FIELD = re.compile(r",(-?\d+)\(r\d+\)$")
PRIVATE_TAIL = 0x2E00


def displacement_frequency(index: dict[int, dict]) -> dict[int, int]:
    """How many times each device displacement appears anywhere in the listing.

    The witness must be the role's own field, so a rare displacement is worth far
    more than the packet fields (0x2934, 0x2948...) that almost every state call
    touches.
    """
    frequency: dict[int, int] = {}
    for body in index.values():
        for ins in body["i"]:
            match = FIELD.search(ins)
            if match:
                offset = int(match[1])
                frequency[offset] = frequency.get(offset, 0) + 1
    return frequency


def pick_witnesses(instructions: list[str], frequency: dict[int, int]) -> list[str]:
    """Up to three instructions that pin this body down, then its last one.

    Ranked so a store into the private tail (already T2ROTF's own layout, not the
    reference's) wins, then the rarest displacement.  Indexed setters that reach
    their slot without a device displacement fall back to any load/store with a
    displacement, so each guard still names instructions unique to its body.
    """
    ranked = []
    fallback = []
    for position, ins in enumerate(instructions):
        match = FIELD.search(ins)
        if not match:
            continue
        offset = int(match[1])
        if offset < 0x2000:
            fallback.append(((-frequency.get(offset, 0), -position), ins))
            continue
        score = (1 if offset >= PRIVATE_TAIL else 0,
                 1 if ins.startswith("st") else 0,
                 -frequency.get(offset, 0))
        ranked.append((score, ins))
    ranked.sort(key=lambda item: item[0], reverse=True)
    fallback.sort(key=lambda item: item[0], reverse=True)

    witnesses = []
    used = set()
    for _, ins in ranked + fallback:
        match = FIELD.search(ins)
        if match:
            offset = int(match[1])
            if offset in used:
                continue
            used.add(offset)
        witnesses.append(ins)
        if len(witnesses) == 3:
            break
    if len(witnesses) < 2:
        # Bodies that reach their slot through a computed address (lwzx/stwx) carry
        # no displacement at all; pin them with their own distinguishing opcode.
        noise = ("b ", "bl ", "blr", "bctr", "mflr", "mtlr", "stwu", "mr ", "cmpdi")
        for ins in instructions:
            if ins in witnesses or ins.startswith(noise) or FIELD.search(ins):
                continue
            witnesses.append(ins)
            if len(witnesses) == 2:
                break
    if instructions and instructions[-1] not in witnesses:
        witnesses.append(instructions[-1])
    return witnesses


def fnv32(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def load_index() -> dict[int, dict]:
    raw = json.loads(INDEX.read_text(encoding="utf-8"))
    return {int(k, 16): v for k, v in raw.items()}


def state_roles() -> list[tuple[int, str, int]]:
    """[(table index, role name, kind)] from state.cpp's own hook list."""
    kinds = {"SetRenderStateHook": ("render", "set"), "GetRenderStateHook": ("render", "get"),
             "SetSamplerStateHook": ("sampler", "set"), "GetSamplerStateHook": ("sampler", "get")}
    out = []
    for match in HOOK.finditer(STATE_CPP.read_text(encoding="utf-8")):
        table, which = kinds[match[1]]
        out.append((table, which, int(match[2]), match[3]))
    return out


def guard(image: bytes, index: dict[int, dict], va: int, role: str, witnesses: list[str]) -> dict:
    body = index.get(va)
    if body is None:
        raise SystemExit(f"{role} {va:#010x}: no DEFINE_REX_FUNC body")
    missing = [w for w in witnesses if w not in body["i"]]
    if missing:
        raise SystemExit(f"{role} {va:#010x}: instruction witnesses missing: {missing}")
    size = min(64, max(4, body["n"] * 4))
    code = image[va - BASE:va - BASE + size]
    if len(code) != size:
        raise SystemExit(f"{role} {va:#010x}: beyond the supplied image")
    return dict(address=va, role=role, guard_bytes=size, fnv32=fnv32(code),
                instructions=body["n"], witnesses=witnesses)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=IMAGE)
    parser.add_argument("--write", action="store_true", help="create t2_hook_map.h and the report")
    parser.add_argument("--json", action="store_true", help="print the table as JSON")
    args = parser.parse_args()

    image = args.image.read_bytes()
    sha = hashlib.sha256(image).hexdigest()
    index = load_index()
    tables = json.loads(TABLES.read_text(encoding="utf-8"))

    entries: list[dict] = []
    skipped: list[str] = []
    for table, which, i, name in state_roles():
        # The state tables' "set" member is the setter, "get" the getter: verified
        # on render[10] (ZEnable: 0x822C7548 stores the 0x2E6C shadow, 0x822C7580
        # loads it) and on the sampler entries.
        call = "get" if which == "get" else "set"
        va = tables[table][i][call]
        if not va:
            skipped.append(f"{name}: no {call} entry")
            continue
        entries.append((va, name, tables[table][i][call], table, i, which))

    # Render state indices 0-9 are aliases (all share one n=1 body): they are not
    # independent implementations, so they carry no guard.
    field_frequency = displacement_frequency(index)
    guards: list[dict] = []
    seen: set[tuple[int, str]] = set()
    for va, name, _, _, _, _ in entries:
        if (va, name) in seen:
            continue
        seen.add((va, name))
        body = index.get(va)
        if body is None:
            skipped.append(f"{name} {va:#010x}: not in the listing")
            continue
        witnesses = pick_witnesses(body["i"], field_frequency)
        guards.append(guard(image, index, va, name, witnesses))

    if EXTRA.exists():
        for item in json.loads(EXTRA.read_text(encoding="utf-8"))["roles"]:
            guards.append(guard(image, index, int(item["address"], 16), item["role"],
                                item["witnesses"]))
    for role, va in ROLES:
        body = index.get(va)
        if body is None:
            skipped.append(f"{role} {va:#010x}: not in the listing")
            continue
        witnesses = pick_witnesses(body["i"], field_frequency)
        guards.append(guard(image, index, va, role, witnesses))
    for role in ROLES_UNKNOWN:
        skipped.append(f"{role}: no address in T2ROTF's image (left unhooked)")

    guards.sort(key=lambda g: (g["address"], g["role"]))
    by_address: dict[int, list[str]] = {}
    for entry in guards:
        by_address.setdefault(entry["address"], []).append(entry["role"])
    aliases = {f"0x{addr:08X}": roles for addr, roles in sorted(by_address.items())
               if len(roles) > 1}
    if args.json:
        print(json.dumps(dict(image_sha256=sha, hooks=guards), indent=2))
        return
    if args.write:
        REPORT.write_text(json.dumps(dict(image_sha256=sha, image=str(args.image),
                                          hooks=guards, skipped=skipped,
                                          aliases=aliases), indent=2) + "\n")
        lines = [
            "// Generated by tools/t2_renut_hooks.py from T2ROTF's TU-patched image",
            "// (extracted/TU/default_tu.flat, guest_vaddr = 0x82000000 + file offset) and its",
            "// generated listing. Emulates mgr_hook_map.h: the guards are the guest interfaces",
            "// Renut substitutes; a mismatch makes Direct3D_CreateDevice refuse the binary.",
            "#pragma once",
            "#include <cstdint>",
            "",
            "namespace renut::gpu::t2 {",
            "",
            "struct GuestCodeGuard { uint32_t address, bytes, fnv32; const char* name; };",
            "",
            "inline constexpr GuestCodeGuard kGuestCodeGuards[] = {",
        ]
        for g in guards:
            lines.append(f'  {{0x{g["address"]:08X}u, {g["guard_bytes"]}u, 0x{g["fnv32"]:08X}u, "{g["role"]}"}},')
        lines += ["};", "",
                  f"// {len(guards)} entries, image SHA256 {sha}", "",
                  "}  // namespace renut::gpu::t2", ""]
        HEADER.write_text("\n".join(lines), encoding="utf-8")
    print(f"PASS: {len(guards)} T2 interfaces have PowerPC instruction witnesses; "
          f"{len(by_address)} distinct addresses; {len(aliases)} aliased; "
          f"{len(skipped)} skipped; image SHA256 {sha}")
    for note in skipped:
        print("  skip:", note)


if __name__ == "__main__":
    main()