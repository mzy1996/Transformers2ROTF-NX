#!/usr/bin/env python3
"""T2ROTF (2026-10-25): 为「主机侧写/读的设备字段」逐个找 T2 偏移。

背景：renut_engine 的游戏侧引用（d3d.h / d3d_native.h / hooks/*.cpp / d3d_native_state.cpp）
里的设备字段偏移来自参考游戏（SA2 为主动值，N&B 在方括号里），还有一批直接写死的字面量。
T2ROTF 的设备布局与它们不同，必须逐个定到 T2 的实测偏移，否则宿主把状态写进错误的槽位。

方法：用 out/t2-tools/t2_index.json（31614 条函数）建「立即数偏移 → 访问它的函数」倒排表，
对每个字段打印：原值处的访问者，以及在若干位移候选处的访问者（含写者/读者与样例指令）。
角色语义（谁是 setter、谁在 swap/present 路径、谁在 ring 路径）由人工按函数名/指令判定。

用法：python tools/t2_field_offsets.py [--delta -128,-368,-404,-408,-412,-420,+8,+4,0]
"""
import argparse
import json
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
INDEX = ROOT / "out/t2-tools/t2_index.json"

# (name, 参考主值, 方括号值或 None, 备注)
FIELDS = [
    # --- d3d.h / d3d_native.h 的具名常量（SA2 主值 / N&B 方括号） ---
    ("VertexDeclaration", 12240, 11820, "SetVertexDeclaration 写"),
    ("CreationThreadId", 11016, 10888, "CreateDevice 写"),
    ("IsTitleProcess", 11020, 10892, "CreateDevice 写"),
    ("BehaviorFlags", 24392, 23980, "CreateDevice 写"),
    ("FenceCompletion", 11024, 10896, "KickOff/IsFencePending 读"),
    ("FenceTag", 11036, 10908, "SetRingBufferParameters 写"),
    ("SwapFenceTag", 11056, 10928, "Swap 记录"),
    ("PresentFenceTag", 15344, 14940, "NewSegment 读写"),
    ("CommandBufferTagMask", 11040, 10912, "SetCommandBufferPredication"),
    ("TilePredication", 11060, 10932, "SetPredication"),
    ("RunPredication", 11064, 10936, "SetPredication"),
    ("Flags", 11068, 10940, "flags 字节"),
    ("Flags2", 11069, 10941, "immediate-fence 位"),
    ("GlobalQword", 11008, 10880, "从 kGlobalQwordVa 拷入"),
    ("IndexBuffer", 12812, 12436, "SetIndices 写"),
    ("RenderTargets", 12816, 12440, "SetRenderTarget 写"),
    ("DepthStencil", 12832, 12456, "SetDepthStencilSurface 写"),
    ("StreamSources", 12836, 12460, "SetStreamSource 写"),
    ("StreamStrides", 12904, 12528, "SetStreamSource 写 u8"),
    ("Textures", 12920, 12544, "SetTexture 写"),
    ("ScissorRect", 13052, 12676, "SetScissorRect 写 RECT"),
    ("PixelShader", 13068, 12692, "SetPixelShader 写"),
    ("VertexShader", 13072, 12696, "SetVertexShader 写"),
    ("Predication", 13084, 12708, "SetPredication 写"),
    ("SnapshotSize", 13088, 12712, "快照长度"),
    ("RunPredicationState", 13088, 12712, "ResetAllState 写"),
    ("PredicationSet", 13092, 12716, "ResetAllState 写"),
    ("TilingSurfaces", 13104, 12728, "BeginTiling 写"),
    ("TileCount", 13124, 12748, "BeginTiling 写"),
    ("TileRects", 13128, 12752, "BeginTiling 写"),
    ("TileOrigins", 13368, 12992, "BeginTiling 写"),
    ("TilingExtent", 13556, 13180, "BeginTiling 写"),
    ("VerticesRingEnd", 13844, 13444, "BeginVertices 写"),
    ("CommandBuffer", 13920, 13516, "AllocateInlineData 读"),
    ("AutoDepthStencil", 15228, 14824, "IPS 写"),
    ("AutoFrontBuffer", 15232, 14828, "IPS 写"),
    ("AutoBackBuffer", 15236, 14832, "IPS 写"),
    ("SideBuffer", 17136, 16728, "设备侧面缓冲"),
    ("SwapCount", 16968, 16560, "Swap 读写"),
    ("InKickOff", 13600, 13232, "BlockOnFence 读"),
    ("CriticalSection0", 15348, 14944, "硬件路径"),
    ("CriticalSection1", 15376, 14972, "硬件路径"),
    ("PerfFrequency", 21996, 21588, "硬件路径"),
    ("MinusOne0", 21968, 21560, "硬件路径"),
    ("MinusOne1", 21972, 21564, "硬件路径"),
    ("MinusOne2", 15340, 14936, "硬件路径"),
    ("TitleMinusOne", 23380, None, "硬件路径 41 字"),
    # --- device.cpp 里写死的字面量（假定已是 N&B 值） ---
    ("L_16712", 16712, None, "InitializeTitleDevice 置 0"),
    ("L_2E2C", 0x2E2C, None, "IPS: 声明指针"),
    ("L_2FA0", 0x2FA0, None, "IPS: 声明对象"),
    ("L_3090", 0x3090, None, "IPS 默认值"),
    ("L_351C", 0x351C, None, "present interval"),
    ("L_5428", 0x5428, None, "显示模式宽"),
    ("L_542C", 0x542C, None, "显示模式高"),
    ("L_5430", 0x5430, None, "显示模式宽2"),
    ("L_5434", 0x5434, None, "刷新率"),
    ("L_3528", 0x3528, None, "前缓冲格式"),
    ("L_4144", 0x4144, None, "swap 标志"),
    ("L_3A40", 0x3A40, None, "缓冲分配器"),
    ("L_3A44", 0x3A44, None, "缓冲分配器"),
    ("L_3A50", 0x3A50, None, "缓冲分配器"),
    ("L_3374", 0x3374, None, "表面 pitch"),
    ("L_3378", 0x3378, None, "表面对齐宽"),
    ("L_31B0", 0x31B0, None, "tiling 模式"),
    ("L_31B4", 0x31B4, None, "tiling 深度"),
    ("L_31A0", 0x31A0, None, "HiControl 拷贝"),
    ("L_3384", 0x3384, None, "窗口脏位"),
    ("L_5D94", 0x5D94, None, "swap 状态位"),
    ("L_5DAC", 0x5DAC, None, "behavior flags2"),
    # --- d3d_native_state.cpp 的私有状态字段 ---
    ("S_2E38_declStrides", 0x2E38, None, "u8[16] 声明步长"),
    ("S_2E48_blendState", 0x2E48, None, "blend"),
    ("S_2E4C_blendFlags", 0x2E4C, None, "blend flags"),
    ("S_2E50_scissorTest", 0x2E50, None, "scissor test"),
    ("S_2E54_colorWrite", 0x2E54, None, "color write [4]"),
    ("S_2E64_zEnable", 0x2E64, None, "z enable"),
    ("S_2E68_stencilEnable", 0x2E68, None, "stencil enable"),
    ("S_2E6C_pointSprite", 0x2E6C, None, "point sprite"),
    ("S_2E74_pointSize", 0x2E74, None, "point size"),
    ("S_2E78_pointSizeMin", 0x2E78, None, "point size min"),
    ("S_2E7C_pointSizeMax", 0x2E7C, None, "point size max"),
    ("S_2E94_maxAniso", 0x2E94, None, "max anisotropy"),
    ("S_2EAE_maxMip", 0x2EAE, None, "max mip"),
    ("S_2EC8_minMip", 0x2EC8, None, "min mip"),
    ("S_2EE2_zFilter", 0x2EE2, None, "z filter"),
    ("S_2EFC_hiPrecision", 0x2EFC, None, "high precision blend [4]"),
    ("S_3168_viewport", 0x3168, None, "视口 6+1"),
    ("S_34C0_exportCount", 0x34C0, None, "BeginExports"),
    ("S_34C4_exportNanDone", 0x34C4, None, "导出 NaN 顶点"),
    ("S_34C8_exportFlags", 0x34C8, None, "导出标志"),
    ("S_2898_exportAddr", 0x2898, None, "导出地址 [Index]"),
    ("S_2A18_copyControl", 0x2A18, None, "拷贝控制"),
    ("S_2A24_copyDestInfo", 0x2A24, None, "拷贝目标"),
    ("S_2A28_copyDepthClear", 0x2A28, None, "深度清除"),
    ("S_2A2C_depthClear", 0x2A2C, None, "RB_DEPTH_CLEAR"),
]

DELTAS = [-128, -368, -372, -376, -400, -404, -408, -412, -420, +4, +8, 0]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--delta", default="")
    ap.add_argument("--min", type=int, default=2, help="候选处至少几次访问才列出")
    args = ap.parse_args()
    deltas = [int(x) for x in args.delta.split(",")] if args.delta else DELTAS

    data = json.loads(INDEX.read_text(encoding="utf-8"))
    # 倒排：偏移 -> [(fn, idx, ins)]
    inv = {}
    pat = re.compile(r"(?<![0-9A-Za-z_])(?:\+|-)?(-?\d+)(?![0-9A-Za-z_])\(")
    for fn, body in data.items():
        for i, ins in enumerate(body["i"]):
            for m in pat.finditer(ins):
                off = int(m.group(1))
                if 0 < off < 0x8000:
                    inv.setdefault(off, []).append((fn, i, ins.strip()))
    print(f"索引 {len(data)} 函数；带立即数偏移的指令条目 {sum(len(v) for v in inv.values())}；不同偏移 {len(inv)}\n")

    for name, val, br, note in FIELDS:
        cands = []
        for d in deltas:
            if d == 0:
                off = val
            else:
                off = (br if br is not None else val) + d
            if not (0 < off < 0x8000):
                continue
            hits = inv.get(off, [])
            if len(hits) < args.min:
                continue
            writers = sorted({h[0] for h in hits if re.match(r"\s*(stw|stb|sth|std|stfs|stfd)", h[2])})
            cands.append((off, len(hits), len(writers), hits))
        print(f"### {name}  参考={val} 方括号={br}  ({note})")
        if not cands:
            print("   （所有候选位移处访问都少于 %d 次）" % args.min)
        for off, n, nw, hits in cands:
            fns = []
            for fn, i, ins in hits[:4]:
                fns.append(f"{fn}:{i} {ins[:60]}")
            print(f"   off={off:<6d} 访问={n:<3d} 写者={nw:<3d} | " + " ; ".join(fns))
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())