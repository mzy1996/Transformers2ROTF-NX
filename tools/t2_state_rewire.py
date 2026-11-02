#!/usr/bin/env python3
"""T2ROTF (2026-10-25): 把 Renut 状态层的钩子注册（hooks/state.cpp）换成 T2ROTF 的地址。

state.cpp 是 Renut 的「记录层」：255 条 Set/Get RenderState、Set/Get SamplerState 钩子，
Renut 靠它们把 guest 的状态调用记进设备影子块。它们的地址原来全是参考构建（MGR/SA2）的
0x826xxxxx / 0x822xxxxx；T2ROTF 的树里这些符号不存在，钩子就是死的。

映射不按位次、按角色：每条注册行末尾的注释就是角色名（如 D3DDevice_SetRenderState_ZEnable），
直接查 out/t2-tools/t2_renut_hooks.json（249 条守卫，角色 → T2 地址，全部经字节源校验）。
参考项目的原话是「No SA2 addresses are inferred by adding a fixed displacement.」——所以
只能按角色对，不能按位移推。

用法：
  python tools/t2_state_rewire.py            # 预演
  python tools/t2_state_rewire.py --write    # 落盘（先备份 .bak.state）
"""
import json
import pathlib
import re
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TARGET = ROOT / "recomp/src/renut_engine/gpu/d3d/hooks/state.cpp"
HOOKS = ROOT / "out/t2-tools/t2_renut_hooks.json"

LINE = re.compile(
    r"^(?P<indent>\s*)BANJO_RENUT_HOOK(?P<kind>_SHARED|_RAW|)?\("
    r"(?P<addr>sub_[0-9A-Fa-f]{8})\s*,\s*(?P<fn>[^)]*)\);\s*"
    r"//\s*(?P<role>[A-Za-z][\w:]*)")


def main() -> int:
    data = json.loads(HOOKS.read_text(encoding="utf-8"))
    role2addr = {}
    dup = []
    for h in data["hooks"]:
        a = "%08X" % h["address"]
        if h["role"] in role2addr and role2addr[h["role"]] != a:
            dup.append((h["role"], role2addr[h["role"]], a))
        role2addr[h["role"]] = a
    print(f"角色表: {len(role2addr)} 条（重复角色 {len(dup)}: {dup[:5]}）")

    text = TARGET.read_text(encoding="utf-8")
    out_lines = []
    done, already, miss_role, other = [], [], [], []
    for i, line in enumerate(text.splitlines(keepends=True), 1):
        m = LINE.match(line)
        if not m or m.group("kind") == "_RAW":
            if "BANJO_RENUT_HOOK" in line and line.lstrip().startswith("BANJO_RENUT_HOOK"):
                other.append((i, line.strip()))
            out_lines.append(line)
            continue
        role = m.group("role")
        old = m.group("addr")
        new = role2addr.get(role)
        if new is None:
            # 角色在 T2ROTF 的镜像里没有定位到：留成死钩子会误导后来人，直接注释掉并写明理由。
            ind = m.group("indent")
            miss_role.append((i, role, old))
            out_lines.append(f"{ind}// [T2ROTF] DROPPED({role}): 角色在 T2ROTF 镜像中未定位"
                             f"（参考注册 {old}）；未纳入构建，留待后续定位。\n")
            continue
        if old == "sub_" + new:
            already.append((i, role))
            out_lines.append(line)
            continue
        kind = m.group("kind") or ""
        line = line.replace(old, "sub_" + new, 1)
        done.append((i, m.group("kind") or "HOOK", old, "sub_" + new, role, m.group("fn")))
        out_lines.append(line)

    for i, kind, old, new, role, fn in done:
        print(f"  {i:4d} {kind:7s} {old} -> {new}  {fn}  // {role}")
    print(f"\n改写 {len(done)} 条；已是 T2 地址 {len(already)} 条；角色表缺 {len(miss_role)} 条")
    for i, role, old in miss_role:
        print(f"  MISS {i}: {role} ({old})")
    if other:
        print("非标准注册行（未处理，人工确认）:")
        for i, l in other[:10]:
            print(f"  {i}: {l[:120]}")

    residual = []
    for i, line in enumerate(out_lines, 1):
        body = line.split("//")[0]
        if re.search(r"(?<![0-9A-Za-z_])(?:__imp__|renut_native_)?sub_(?:826|822)[0-9A-Fa-f]{5}"
                     r"(?![0-9A-Za-z_])", body):
            residual.append((i, body.strip()[:100]))
    print(f"改写后非注释残留 {len(residual)}:")
    for i, l in residual[:10]:
        print(f"  {i}: {l}")

    if "--write" in sys.argv and done:
        shutil.copy2(TARGET, TARGET.with_suffix(".cpp.bak.state"))
        TARGET.write_text("".join(out_lines), encoding="utf-8")
        print("WROTE（备份 state.cpp.bak.state）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())