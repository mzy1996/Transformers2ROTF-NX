#!/usr/bin/env python3
"""T2ROTF (2026-10-25): 把 Renut D3D 宿主的「参考项目设备结构偏移」换成 T2ROTF 实测值。

背景：renut_engine/gpu/d3d 是从参考项目（MGR / SA2 / N&B）整树搬来的：它靠一批
硬编码的 guest 设备结构偏移去读 guest 的 D3D 状态、并把宿主的结果写回去。这些偏移
在 T2ROTF 的镜像里全都不成立（参考项目自己写明「No SA2 addresses are inferred by
adding a fixed displacement.」），必须逐字段换成 T2 实测值。

取值依据（全部落在 out/t2-tools/ 的取证产物里）：
  * out/t2-tools/t2_field_offsets.txt  逐字段「候选偏移 / 访问次数 / 写者 / 样例指令」扫描
    （31614 个函数、550444 条带立即数位移的指令），每条都带见证函数。
  * out/t2-tools/t2_off_verify.py / t2_private_offsets.py 已有的字节源比对
    （extracted/TU/default_tu.flat，映射 guest_vaddr = 0x82000000 + 文件偏移）。
  * 已用 golden bytes 三处对齐的锚点：10896（ring 控制块指针）、10908（写指针 V）、
    10941（ring 标志字节）、13232（KickOff 中标志，sub_822D4488 读）。

级别说明（写进注释，便于后续核对）：
  HIGH   有角色函数直接读写该槽（如 SetStreamSource/SetScissorRect/SetPredication…）
  MED    有同族函数或多个写者见证，但非角色函数本身
  LOW    只有规则外推（例如同一块 +8/−404 的邻居成立），标 LOW 留给 T17 实机核对
  KEEP   取证支持「T2 == 参考值」，不改（仅补注释）

用法：
  python tools/t2_offsets_patch.py            # 预演（只打印）
  python tools/t2_offsets_patch.py --write    # 落盘（每个文件先备份 .bak.t2off）
"""
import pathlib
import re
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
D3D = "recomp/src/renut_engine/gpu/d3d"

# (相对文件, 常量名, 参考值, T2 值, 级别)
NAME_RULES = [
    # ---- d3d.h：头部簇（T2 = N&B 方括号值；10896/10908/10941 已 golden-bytes 对齐） ----
    (f"{D3D}/d3d.h", "kDeviceOffsetCreationThreadId", 11016, 10888, "HIGH"),
    (f"{D3D}/d3d.h", "kDeviceOffsetIsTitleProcess", 11020, 10892, "MED"),
    (f"{D3D}/d3d.h", "kDeviceOffsetBehaviorFlags", 24392, 23980, "MED"),
    (f"{D3D}/d3d.h", "kDeviceOffsetVertexDeclaration", 12240, 11824, "MED"),
    (f"{D3D}/d3d.h", "kDeviceOffsetIndexBuffer", 12812, 12444, "HIGH"),
    (f"{D3D}/d3d.h", "kDeviceOffsetRenderTargets", 12816, 12448, "HIGH"),
    (f"{D3D}/d3d.h", "kDeviceOffsetDepthStencil", 12832, 12464, "HIGH"),
    (f"{D3D}/d3d.h", "kDeviceOffsetStreamSources", 12836, 12468, "HIGH"),
    (f"{D3D}/d3d.h", "kDeviceOffsetStreamStrides", 12904, 12536, "HIGH"),
    (f"{D3D}/d3d.h", "kDeviceOffsetTextures", 12920, 12552, "MED"),
    (f"{D3D}/d3d.h", "kDeviceOffsetPixelShader", 13068, 12700, "HIGH"),
    (f"{D3D}/d3d.h", "kDeviceOffsetVertexShader", 13072, 12704, "HIGH"),
    (f"{D3D}/d3d.h", "kDeviceOffsetPredication", 13084, 12716, "HIGH"),
    (f"{D3D}/d3d.h", "kDeviceOffsetCommandBuffer", 13920, 13516, "HIGH"),
    (f"{D3D}/d3d.h", "kDeviceSnapshotSize", 13088, 12720, "HIGH"),
    (f"{D3D}/d3d.h", "kDeviceOffsetVerticesRingEnd", 13844, 13440, "MED"),
    (f"{D3D}/d3d.h", "kDeviceOffsetSwapCount", 16968, 16560, "LOW"),
    (f"{D3D}/d3d.h", "kDeviceOffsetFenceTag", 11036, 10908, "HIGH"),
    (f"{D3D}/d3d.h", "kDeviceOffsetSwapFenceTag", 11056, 10928, "MED"),
    (f"{D3D}/d3d.h", "kDeviceOffsetSwapFlags", 11069, 10941, "HIGH"),
    # ---- d3d_native.h ----
    (f"{D3D}/d3d_native.h", "FenceCompletion", 11024, 10896, "HIGH"),
    (f"{D3D}/d3d_native.h", "FenceTag", 11036, 10908, "HIGH"),
    (f"{D3D}/d3d_native.h", "CommandBufferTagMask", 11040, 10912, "HIGH"),
    (f"{D3D}/d3d_native.h", "TilePredication", 11060, 10932, "HIGH"),
    (f"{D3D}/d3d_native.h", "RunPredication", 11064, 10936, "HIGH"),
    (f"{D3D}/d3d_native.h", "Flags", 11068, 10940, "MED"),
    (f"{D3D}/d3d_native.h", "Flags1", 11069, 10941, "HIGH"),
    (f"{D3D}/d3d_native.h", "Flags2", 11070, 10942, "LOW"),
    (f"{D3D}/d3d_native.h", "Flags3", 11071, 10943, "LOW"),
    (f"{D3D}/d3d_native.h", "ScissorRect", 13052, 12684, "HIGH"),
    (f"{D3D}/d3d_native.h", "IndexBuffer", 12812, 12444, "HIGH"),
    (f"{D3D}/d3d_native.h", "RenderTargets", 12816, 12448, "HIGH"),
    (f"{D3D}/d3d_native.h", "DepthStencil", 12832, 12464, "HIGH"),
    (f"{D3D}/d3d_native.h", "StreamSources", 12836, 12468, "HIGH"),
    (f"{D3D}/d3d_native.h", "StreamStrides", 12904, 12536, "HIGH"),
    (f"{D3D}/d3d_native.h", "Textures", 12920, 12552, "MED"),
    (f"{D3D}/d3d_native.h", "Predication", 13084, 12716, "HIGH"),
    (f"{D3D}/d3d_native.h", "RunPredicationState", 13088, 12720, "HIGH"),
    (f"{D3D}/d3d_native.h", "PredicationSet", 13092, 12724, "HIGH"),
    (f"{D3D}/d3d_native.h", "TilingSurfaces", 13104, 12736, "HIGH"),
    (f"{D3D}/d3d_native.h", "TileCount", 13124, 12756, "HIGH"),
    (f"{D3D}/d3d_native.h", "TileRects", 13128, 12760, "HIGH"),
    (f"{D3D}/d3d_native.h", "TileOrigins", 13368, 13000, "MED"),
    (f"{D3D}/d3d_native.h", "TilingExtent", 13556, 13188, "MED"),
    (f"{D3D}/d3d_native.h", "CommandBuffer", 13920, 13516, "HIGH"),
    (f"{D3D}/d3d_native.h", "SideBuffer", 17136, 16728, "LOW"),
    (f"{D3D}/d3d_native.h", "AutoDepthStencil", 15228, 14828, "HIGH"),
    (f"{D3D}/d3d_native.h", "AutoFrontBuffer", 15232, 14832, "HIGH"),
    (f"{D3D}/d3d_native.h", "AutoBackBuffer", 15236, 14836, "HIGH"),
    # ---- d3d_native_state.cpp：私有状态块（T2 = 参考 + 8，逐字段有角色见证） ----
    (f"{D3D}/d3d_native_state.cpp", "kBlendState", 11848, 11856, "HIGH"),
    (f"{D3D}/d3d_native_state.cpp", "kBlendFlags", 11852, 11860, "HIGH"),
    (f"{D3D}/d3d_native_state.cpp", "kScissorTestEnable", 11856, 11864, "HIGH"),
    (f"{D3D}/d3d_native_state.cpp", "kColorWriteEnable", 11860, 11868, "HIGH"),
    (f"{D3D}/d3d_native_state.cpp", "kZEnable", 11876, 11884, "HIGH"),
    (f"{D3D}/d3d_native_state.cpp", "kStencilEnable", 11880, 11888, "HIGH"),
    (f"{D3D}/d3d_native_state.cpp", "kPointSpriteEnable", 11884, 11892, "HIGH"),
    (f"{D3D}/d3d_native_state.cpp", "kPointSize", 11892, 11900, "HIGH"),
    (f"{D3D}/d3d_native_state.cpp", "kPointSizeMin", 11896, 11904, "HIGH"),
    (f"{D3D}/d3d_native_state.cpp", "kPointSizeMax", 11900, 11908, "HIGH"),
    (f"{D3D}/d3d_native_state.cpp", "kHighPrecisionBlend", 12028, 12036, "MED"),
    (f"{D3D}/d3d_native_state.cpp", "kMaxAnisotropy", 11924, 11932, "MED"),
    (f"{D3D}/d3d_native_state.cpp", "kMaxMipLevel", 11950, 11958, "MED"),
    (f"{D3D}/d3d_native_state.cpp", "kMinMipLevel", 11976, 11984, "MED"),
    (f"{D3D}/d3d_native_state.cpp", "kZFilter", 12002, 12010, "MED"),
    (f"{D3D}/d3d_native_state.cpp", "kStreamDeclStrides", 11832, 11840, "MED"),
    (f"{D3D}/d3d_native_state.cpp", "kVertexDeclaration", 11820, 11824, "MED"),
    (f"{D3D}/d3d_native_state.cpp", "kVertexShader", 12696, 12704, "HIGH"),
    (f"{D3D}/d3d_native_state.cpp", "kPixelShader", 12692, 12700, "HIGH"),
    (f"{D3D}/d3d_native_state.cpp", "kViewport", 12648, 12656, "HIGH"),
    (f"{D3D}/d3d_native_state.cpp", "kRenderStateTable", 0x82E506F0, 0x82BF65F0, "HIGH"),
    (f"{D3D}/d3d_native_state.cpp", "kSamplerStateTable", 0x82E50BD0, 0x82BF6AE0, "HIGH"),
    # ---- hooks/device.cpp ----
    (f"{D3D}/hooks/device.cpp", "kDeviceOffsetFlags", 11068, 10940, "MED"),
    (f"{D3D}/hooks/device.cpp", "kDeviceOffsetFlags2", 11069, 10941, "HIGH"),
    (f"{D3D}/hooks/device.cpp", "kDeviceOffsetFenceCompletion", 11024, 10896, "HIGH"),
    (f"{D3D}/hooks/device.cpp", "kDeviceOffsetInKickOff", 13600, 13232, "HIGH"),
    (f"{D3D}/hooks/device.cpp", "kDeviceOffsetSideBuffer", 17136, 16728, "LOW"),
    (f"{D3D}/hooks/device.cpp", "kDeviceOffsetPerfFrequency", 21996, 21592, "MED"),
    (f"{D3D}/hooks/device.cpp", "kVdGlobalDeviceImportSlot", 0x820005F8, 0x820011B0, "HIGH"),
]

# (相对文件, 原文, 新文, 说明) —— 需要上下文的裸字面量
CTX_RULES = [
    (f"{D3D}/hooks/device.cpp", "kDeviceOffsetsMinusOne[] = {21968, 21972, 15340}",
     "kDeviceOffsetsMinusOne[] = {21560, 21564, 14936}",
     "MinusOne 三哨兵：前两个由 822D1640:56/57 与 822E1690:78/79 连续写入 21560/21564（−404，双角色见证）"),
    (f"{D3D}/hooks/device.cpp", "d.W(0x2E4C)", "d.W(0x2E54)",
     "blend flags：kBlendFlags 的 T2 槽 (11860=0x2E54)"),
    (f"{D3D}/hooks/draw.cpp", "d.F(0x3168 + i * 4)", "d.F(0x3170 + i * 4)",
     "viewport 浮点数组：kViewport 的 T2 槽 (12656=0x3170)"),
]

# 只补注释、不改值（取证支持 KEEP，或明确标 UNRESOLVED 留给 T17）
NOTE_ONLY = [
    (f"{D3D}/hooks/device.cpp", "kDeviceOffsetGlobalQword", "MED: 822D13F0(CBlocker_Check) 读 11008；保持参考值"),
    (f"{D3D}/hooks/device.cpp", "kGlobalQwordVa", "UNRESOLVED: T2 的该 guest 全局变量地址未定位（写入点未找到）"),
    (f"{D3D}/hooks/device.cpp", "kDeviceOffsetTitleMinusOne", "UNRESOLVED: 23380 无见证，−404 候选 22976 亦无见证"),
    (f"{D3D}/hooks/device.cpp", "kDeviceOffsetCriticalSection0", "KEEP: 15348 有 827F3728/82797B68/827F3DA0 见证"),
    (f"{D3D}/hooks/device.cpp", "kDeviceOffsetCriticalSection1", "KEEP: 15376 有 827C2090/827C4088/827C3C18 见证"),
    (f"{D3D}/hooks/device.cpp", "kDeviceCapsVa", "UNRESOLVED: host 写入 0x130 字节的 caps 区未在 T2 定位（有越写风险，T17 核对）"),
    (f"{D3D}/hooks/device.cpp", "kFusionVa", "UNRESOLVED: 只读标志，错值无害"),
    (f"{D3D}/hooks/device.cpp", "kSa2DeviceOffsetGammaRamp", "UNRESOLVED: T2 的 gamma ramp 槽未定（14940 属 ring 族，不是 ramp）"),
    (f"{D3D}/hooks/device.cpp", "kDeviceOffsetGammaRamp", "UNRESOLVED: 同上（0x3A9C=14940 在 T2 是 ring 字段）"),
    (f"{D3D}/hooks/device.cpp", "kDeviceOffsetOwnerThread", "KEEP: 0x2A88 与 CreationThreadId 的 T2 槽同值（T2 只留一个线程 id 槽）"),
    (f"{D3D}/hooks/device.cpp", "device_va + 16976", "UNRESOLVED: SwapCount 邻槽，T2 候选 16564/16568 证据不足"),
    (f"{D3D}/hooks/device.cpp", "kDeviceOffsetSwapCount", "LOW: 16560 按 −408 规则（16564/16568 有 822D9688/822D9570 见证，T17 核对）"),
    (f"{D3D}/hooks/device.cpp", "0x34D0", "KEEP: 0x34C0-0x34F0 块与参考同址（CommandBuffer 0x34CC=13516 已由 822D3630/822D4000 见证）"),
    (f"{D3D}/d3d_native_state.cpp", "kFormatTable", "UNRESOLVED: T2 的 D3DFORMAT 派发表未定位"),
    (f"{D3D}/d3d_native_state.cpp", "kSkipExportNanVertex", "UNRESOLVED: guest cvar 地址未定位"),
    (f"{D3D}/d3d_native_state.cpp", "kSkipExport512Pixels", "UNRESOLVED: 同上"),
    (f"{D3D}/d3d_native_state.cpp", "kExportAddress", "KEEP: 0x2898 属 0x28xx 包体块（0x2880 SurfaceInfo 已见证同址）"),
    (f"{D3D}/d3d_native_state.cpp", "kExportCount", "KEEP: 13504 有 18 次访问/3 写者见证"),
    (f"{D3D}/d3d_native_state.cpp", "kExportNanVertexDone", "KEEP: 13508 有 6 次访问见证"),
    (f"{D3D}/d3d_native_state.cpp", "kExportFlags", "KEEP: 13512 由 822D6670/822D7418 写"),
    (f"{D3D}/d3d_native_state.cpp", "kCopyControl", "KEEP: 0x2A18-0x2A34 复制控制簇＝参考（822D1C20/822D2120 连续 stw 见证）"),
    (f"{D3D}/d3d_native_state.cpp", "kVportXScale", "KEEP: 0x2908 在 T2 有名可查（4 次访问）"),
    (f"{D3D}/d3d_native_state.cpp", "kVportZScale", "KEEP: 0x2918 在 T2 有名可查（4 次访问）"),
    (f"{D3D}/d3d_native_state.cpp", "kBehaviorFlags2", "KEEP: 0x5DAC=23980 由 822D1640:71 与 822E0F60 读写"),
    (f"{D3D}/d3d_native_state.cpp", "kPresentInterval", "KEEP: 0x351C 由 822D9838 读（37 次访问）"),
    (f"{D3D}/hooks/draw.cpp", "kFullViewport", "UNRESOLVED: T2 的满屏 viewport 常量表地址未定位"),
    (f"{D3D}/hooks/draw.cpp", "0x31B4", "KEEP: 0x31B4=12724 已是 T2 的 PredicationSet 槽"),
    (f"{D3D}/hooks/draw.cpp", "unk_82001150", "LOW: 立即数 0x47008081 取自参考 .rdata，T2 同名浮点值未比对"),
    (f"{D3D}/hooks/resource.cpp", "kFormatTable", "UNRESOLVED: T2 的 D3DFORMAT 派发表未定位"),
    (f"{D3D}/hooks/resource.cpp", "kHiZOwnerVa", "UNRESOLVED: T2 的 HiZ owner 标志地址未定位"),
    (f"{D3D}/hooks/resource.cpp", "kGameNoLargePagesVa", "UNRESOLVED: T2 的 MEM_LARGE_PAGES 开关地址未定位"),
    (f"{D3D}/hooks/shader.cpp", "kVertexShaderContainer", "UNRESOLVED: 容器在设备内的偏移未在 T2 定位（只影响 shader 转储，不影响运行）"),
]

NAME_RE = re.compile(r"(?P<head>\b(?P<name>[A-Za-z_]\w*)\s*=\s*)(?P<lit>0[xX][0-9A-Fa-f]+|\d+)(?P<tail>\s*;)")


def patch_file(rel: str, rules, ctx_rules, notes, write: bool):
    p = ROOT / rel
    text = p.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    log, changed = [], 0

    for name, old, new, level in rules:
        hit = 0
        for i, line in enumerate(lines):
            body = line.split("//")[0]
            for m in NAME_RE.finditer(body):
                if m.group("name") != name:
                    continue
                val = int(m.group("lit"), 0)
                if val != old:
                    continue
                hexform = m.group("lit")[1:2] in "xX"
                lit_new = ("0x%X" % new) if hexform else str(new)
                repl = m.group("head") + lit_new + m.group("tail")
                ann = "  // [T2ROTF] T2=%s (was %s, %s)" % (lit_new, m.group("lit"), level)
                newline = body[:m.start()] + repl + body[m.end():]
                if "//" in line:
                    comment = "//" + line.split("//", 1)[1].rstrip("\n")
                    line = newline.rstrip("\n") + "  " + comment + ann + "\n"
                else:
                    line = newline.rstrip("\n") + ann + "\n"
                lines[i] = line
                hit += 1
        if hit == 0:
            log.append(("MISS", name, "未命中（已改或值不符）"))
        else:
            changed += hit
            log.append(("OK", name, f"{old} -> {new} ({level}) x{hit}"))

    for old_s, new_s, why in ctx_rules:
        hit = 0
        for i, line in enumerate(lines):
            if old_s in line:
                lines[i] = line.replace(old_s, new_s).rstrip("\n") + "  // [T2ROTF] " + why + "\n"
                hit += 1
        if hit == 0:
            log.append(("MISS", old_s, "未命中"))
        else:
            changed += hit
            log.append(("OK", old_s, f"-> {new_s} x{hit}"))

    for pat, note in notes:
        hit = 0
        for i, line in enumerate(lines):
            if pat in line and "[T2ROTF]" not in line and not line.lstrip().startswith("//"):
                lines[i] = line.rstrip("\n") + "  // [T2ROTF] " + note + "\n"
                hit += 1
        log.append(("NOTE", pat, f"{note} x{hit}"))

    print(f"=== {rel}: {len(log)} 条规则, {changed} 处改写 ===")
    for kind, a, b in log:
        if kind != "NOTE":
            print(f"  {kind:4s} {a:38s} {b}")
    if write and changed:
        bak = p.with_suffix(p.suffix + ".bak.t2off")
        if not bak.exists():
            shutil.copy2(p, bak)
        p.write_text("".join(lines), encoding="utf-8")
        print(f"  WROTE {rel}（备份 {bak.name}）")
    return changed, log


def main() -> int:
    write = "--write" in sys.argv
    total = 0
    for rel in sorted({r[0] for r in NAME_RULES} | {r[0] for r in CTX_RULES} | {r[0] for r in NOTE_ONLY}):
        rules = [r[1:] for r in NAME_RULES if r[0] == rel]
        ctx = [r[1:] for r in CTX_RULES if r[0] == rel]
        notes = [r[1:] for r in NOTE_ONLY if r[0] == rel]
        c, _ = patch_file(rel, rules, ctx, notes, write)
        total += c
    print(f"\n总计改写 {total} 处" + ("（已落盘）" if write else "（预演；加 --write 落盘）"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())