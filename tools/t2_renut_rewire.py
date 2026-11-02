#!/usr/bin/env python3
"""T2ROTF: 把 Renut D3D 钩子表里注册的参考构建地址换成 T2 自己的地址。

背景
----
recomp/src/renut_engine/gpu/d3d/hooks/{device,draw,resource,shader}.cpp 的钩子注册点
是从参考构建（sa2-nx / mgr-nx-deko，XDK 2.0.21076 的 0x826Bxxxx / 0x826Cxxxx 段）抄来的。
T2ROTF 的镜像是 0x822xxxxx 段，参考地址在 T2 的链接里全是死符号（27 条角色里 25 条没有
__imp__sub_XXXX，LTO 直接丢弃），所以钩子从来没生效过。本脚本按
recomp/src/renut_engine/gpu/d3d/t2_hook_map.h 的 27 条角色表，把**非注释行**里的标识符
整体替换成 T2 地址，并处理三处特殊情况：

  1. SHARED -> HOOK：Swap(sub_826B3510) / DrawVertices(sub_826BC928) /
     DrawIndexedVertices(sub_826BCD40)。参考构建里这些地址另有 sa2-nx 自己的 REX_FUNC
     定义（它再调用 renut_native_sub_X），所以 Renut 侧只能以 SHARED 形式提供、等别人来
     调。T2 的树里没有任何其它文件在这些地址上定义 guest 入口（已全树核对），沿用 SHARED
     会让 Renut 侧永远没人调用 -> Swap / 绘制永远进不到 Renut -> 永不呈现。因此改为普通
     BANJO_RENUT_HOOK，由 Renut 侧独占这些 guest 入口（T2 构建里 Active() 恒为真）。
  2. HOOK -> SHARED：CBlocker_Check(sub_826B1C88) / BlockOnFence(sub_826B42A8) /
     SetRingBufferParameters(sub_826B5A08)。这三个地址已经由 src/t2rotf_video_nativo.cpp
     的 REX_HOOK_RAW 诊断钩子占用（Vulkan 期的 [T2DIAG] ANILLO / CP 取证）。同一个 guest
     入口只能有一个定义，所以 Renut 侧改以 SHARED 提供，由 t2rotf_video_nativo.cpp 的钩子
     打完诊断后让出（见该文件里 renut_native_sub_822D13F0 / _822D4488 / _822D4A08 的让出块）。
  3. DROP：D3DDevice_InsertCallback（参考 0x826B59B0）在 T2 镜像里找不到对应地址
     （0x822D3000-0x822D4B00 逐条排查过），整行注释掉并写明理由。

用法
----
    python tools/t2_renut_rewire.py             # 预演，只打印将发生的改动
    python tools/t2_renut_rewire.py --write     # 落盘（每个文件先写 .bak）
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = ROOT / "recomp" / "src" / "renut_engine" / "gpu"

FILES = [
    BASE / "d3d" / "hooks" / "device.cpp",
    BASE / "d3d" / "hooks" / "draw.cpp",
    BASE / "d3d" / "hooks" / "resource.cpp",
    BASE / "d3d" / "hooks" / "shader.cpp",
    BASE / "banjo_shared_hooks.h",
]

# 参考构建的标识符 -> T2 的标识符（None = 丢弃该钩子）。
# 地址取自 recomp/src/renut_engine/gpu/d3d/t2_hook_map.h 的角色表（逐字节校验过的）。
MAP: dict[str, str | None] = {
    # --- device（DH = 参考的 SA2/MGR 地址，T2 = 角色地址） ---
    "sub_826B2418": "sub_822D1640",  # Direct3D_CreateDevice
    "sub_826C0C48": "sub_822E1240",  # D3D::InitializePresentationParameters
    "sub_826C0278": "sub_822E0F60",  # D3D::InitializeApiState
    "sub_826B3050": "sub_822D9838",  # D3D::SynchronizeToPresentationInterval
    "sub_826B5688": "sub_822D4388",  # D3D_CDevice_KickOff
    "sub_826B5348": "sub_822D4000",  # D3D_CDevice_KickOffSegment
    "sub_826B58C0": "sub_822D3058",  # D3D_CDevice_BeginRingBig
    "sub_826B4078": "sub_822D1AF0",  # D3D_RingBufferDeviceAllocate
    "sub_826B44C0": "sub_822D3630",  # D3D_CDevice_AllocateInlineData
    "sub_826B45A8": "sub_822D37D0",  # ring 段分配器（参考里叫 NewSegment）
    "sub_826B5A08": "sub_822D4A08",  # D3DDevice_SetRingBufferParameters（见情况 2）
    "sub_826B3510": "sub_822D9D70",  # D3DDevice_Swap（见情况 1）
    "sub_826B42A8": "sub_822D4488",  # D3D_CDevice_BlockOnFence（见情况 2）
    "sub_826B1C88": "sub_822D13F0",  # D3D_CBlocker_Check（见情况 2）
    "sub_826B57C8": "sub_822D4920",  # D3DDevice_IsFencePending
    # --- draw ---
    "sub_826BC928": "sub_822CA748",  # D3DDevice_DrawVertices（见情况 1）
    "sub_826BCD40": "sub_822CAB38",  # D3DDevice_DrawIndexedVertices（见情况 1）
    "sub_826BC468": "sub_822C9CB0",  # D3DDevice_BeginVertices
    "sub_826B5FC8": "sub_822C68B8",  # D3DDevice_WindowScissor
    "sub_826BC260": "sub_822D8CD8",  # D3DDevice_ClearF
    "sub_826BB0D0": "sub_822DB2D8",  # D3DDevice_Resolve
    # --- resource ---
    "sub_826AAF80": "sub_822CB9E0",  # D3DResource_Destroy
    # --- shader ---
    "sub_826ACD00": "sub_822C0A98",  # XGRegisterVertexShader
    "sub_826AC3D8": "sub_822C07B0",  # XGRegisterPixelShader
    "sub_826AC938": "sub_822C0F58",  # D3DDevice_CreateVertexDeclaration
    # --- 丢弃 ---
    "sub_826B59B0": None,  # D3DDevice_InsertCallback：T2 镜像里没有对应地址
}

# 情况 1：这些 T2 标识符所在的行，宏要从 SHARED 改成普通 HOOK。
SHARED_TO_HOOK = {
    "sub_822D9D70": "D3DDevice_Swap：T2 树里没有其它定义，Renut 侧必须独占该 guest 入口",
    "sub_822CA748": "D3DDevice_DrawVertices：同上",
    "sub_822CAB38": "D3DDevice_DrawIndexedVertices：同上",
}

# 情况 2：这些 T2 标识符所在的行，宏要从普通 HOOK 改成 SHARED（t2rotf_video_nativo.cpp 占用）。
HOOK_TO_SHARED = {
    "sub_822D13F0": "D3D_CBlocker_Check：src/t2rotf_video_nativo.cpp 的 REX_HOOK_RAW 已占用该入口，Renut 侧改为 SHARED 供它让出",
    "sub_822D4488": "D3D_CDevice_BlockOnFence：同上",
    "sub_822D4A08": "D3DDevice_SetRingBufferParameters：同上",
}

# 三种书写形态都要认：裸 `sub_XXXX`、`__imp__sub_XXXX`（guest 原体）与
# `renut_native_sub_XXXX`（Renut 侧的 SHARED 实现）。**不能用 \b 或裸 substring**：
# `__imp__sub_X` 里 "sub" 前面是下划线，\b 不成立 —— 第一版就是因此漏掉了 3 个 __imp__
# 调用点（draw.cpp:622/636/659），dry-run 还谎报「0 残留」。前缀必须显式匹配。
HEX8 = r"[0-9A-Fa-f]{8}"
PRE_IMP = re.compile(r"(?<![0-9A-Za-z_])__imp__sub_" + HEX8 + r"(?![0-9A-Za-z_])")
PRE_RENUT = re.compile(r"(?<![0-9A-Za-z_])renut_native_sub_" + HEX8 + r"(?![0-9A-Za-z_])")
BARE = re.compile(r"(?<![0-9A-Za-z_])sub_" + HEX8 + r"(?![0-9A-Za-z_])")
# 残留/丢弃检查用：任意形态的参考构建标识符（0x826xxxxx 段）。
ANY_REF = re.compile(
    r"(?<![0-9A-Za-z_])(?:__imp__|renut_native_)?sub_826[ABC][0-9A-Fa-f]{4}(?![0-9A-Za-z_])")
DROP_NAMES = {name for name, t2 in MAP.items() if t2 is None}


def split_comment(line: str) -> tuple[str, str]:
    """把一行拆成代码部分与注释部分（注释以第一个 // 开始；本工程这些行没有字符串字面量）。"""
    at = line.find("//")
    if at < 0:
        return line, ""
    return line[:at], line[at:]


def strip_prefix(text: str) -> str:
    for p in ("__imp__", "renut_native_"):
        if text.startswith(p):
            return text[len(p):]
    return text


def rewrite_line(line: str) -> tuple[str, list[str]]:
    body, comment = split_comment(line)
    newline = "\n" if line.endswith("\n") else ""
    notes: list[str] = []

    # 丢弃：该角色在 T2 镜像里没有地址，整行注释掉（保留原始文本，便于追溯）。
    for m in ANY_REF.finditer(body):
        if strip_prefix(m.group(0)) in DROP_NAMES:
            return (
                "// [T2ROTF] " + line.rstrip("\n")
                + "  // DROPPED: T2 镜像里没有对应地址（0x822D3000-0x822D4B00 逐条排查无果）\n",
                [f"DROP {strip_prefix(m.group(0))}"],
            )

    def repl(match: re.Match) -> str:
        text = match.group(0)
        pref = text[: len(text) - len(strip_prefix(text))]
        name = strip_prefix(text)
        if name not in MAP:  # 已经是 T2 地址（幂等重跑）
            return text
        new = MAP[name]
        notes.append(f"{name} -> {new}")
        return pref + new

    body = PRE_IMP.sub(repl, body)
    body = PRE_RENUT.sub(repl, body)
    body = BARE.sub(repl, body)

    # 情况 1：SHARED -> HOOK
    for t2, why in SHARED_TO_HOOK.items():
        if t2 in body and "BANJO_RENUT_HOOK_SHARED" in body:
            body = body.replace("BANJO_RENUT_HOOK_SHARED", "BANJO_RENUT_HOOK")
            notes.append(f"SHARED->HOOK（{why}）")

    # 情况 2：HOOK -> SHARED
    for t2, why in HOOK_TO_SHARED.items():
        if t2 in body and "BANJO_RENUT_HOOK(" in body:
            body = body.replace("BANJO_RENUT_HOOK(", "BANJO_RENUT_HOOK_SHARED(")
            notes.append(f"HOOK->SHARED（{why}）")

    if not notes:
        return line, notes
    return body + comment + newline, notes


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="落盘（默认只预演）")
    args = ap.parse_args()

    changed = 0
    dropped = 0
    per_file: dict[str, int] = {}
    for path in FILES:
        if not path.exists():
            print(f"MISSING: {path}")
            return 2
        text = path.read_text(encoding="utf-8")
        lines = text.splitlines(keepends=True)
        out: list[str] = []
        for i, line in enumerate(lines, 1):
            new, notes = rewrite_line(line)
            if notes:
                changed += 1
                per_file[str(path.relative_to(ROOT))] = per_file.get(str(path.relative_to(ROOT)), 0) + 1
                for n in notes:
                    if n.startswith("DROP"):
                        dropped += 1
                    print(f"{path.relative_to(ROOT)}:{i}: {n}")
                print(f"    - {line.rstrip()}")
                print(f"    + {new.rstrip()}")
            out.append(new)
        if args.write:
            path.with_suffix(path.suffix + ".bak").write_text(text, encoding="utf-8")
            path.write_text("".join(out), encoding="utf-8")

    print()
    print(f"{'WROTE' if args.write else 'DRY RUN'}: {changed} lines rewritten ({dropped} dropped) in "
          f"{len(per_file)} files")
    for f, n in sorted(per_file.items()):
        print(f"  {f}: {n}")

    # 残留检查：非注释代码里不应再有参考构建的 0x826xxxxx / 0x826Cxxxx 标识符。
    residual = 0
    for path in FILES:
        text = args.write and path.read_text(encoding="utf-8") or path.read_text(encoding="utf-8")
        for i, line in enumerate(text.splitlines(), 1):
            body, _ = split_comment(line)
            for m in re.finditer(r"\bsub_826[ABC][0-9A-Fa-f]{4}\b", body):
                residual += 1
                print(f"RESIDUAL {path.relative_to(ROOT)}:{i}: {m.group(0)}")
    print(f"RESIDUAL non-comment reference identifiers: {residual}")
    return 0 if residual == 0 or not args.write else 1


if __name__ == "__main__":
    sys.exit(main())