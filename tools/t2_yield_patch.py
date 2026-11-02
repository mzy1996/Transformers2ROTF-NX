#!/usr/bin/env python3
"""T2ROTF (2026-10-25): 让出补丁 —— 一次把 t2rotf_video_nativo.cpp 的三处让出点与 include 落盘。

为什么用脚本而不是手改：同一文件在一个 assistant 步骤里连做多次 edit，后一条会撞上
「file_modified_since_read」时间戳校验而失败（本轮实测：4 条 edit 只落 1 条）。脚本一次
写盘 + .bak 备份 + 幂等重跑，避免这个问题。

写入内容：Renut(deko3d) 侧在 sub_822D13F0 / sub_822D4488 / sub_822D4A08 三个 guest 入口
以 renut_native_<T2 地址> 提供实现（device.cpp 的 BANJO_RENUT_HOOK_SHARED），本文件的
REX_HOOK_RAW（[T2DIAG] ANILLO/externo/init 取证钩子）打完诊断后按渲染器选择让出。
"""
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TARGET = ROOT / "recomp/src/t2rotf_video_nativo.cpp"

INCLUDE_OLD = """#include <rex/system/kernel_state.h>

#include <algorithm>
"""

INCLUDE_NEW = """#include <rex/system/kernel_state.h>

// T2ROTF (2026-10-25): Renut/deko3d 侧的让出。Renut 的 D3D 钩子在游戏自己的地址上取代
// guest 实现，但 sub_822D13F0 / sub_822D4488 / sub_822D4A08 三个 guest 入口已经被本文件
// 的 REX_HOOK_RAW 占用（一个 guest 入口只能有一个定义），所以 Renut 侧以
// renut_native_<T2 地址> 提供实现，由本文件在打完诊断后让出（见文件末尾的三个让出点）。
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/banjo_shared_hooks.h"

#include <algorithm>
"""

# (旧文本, 新文本, 说明, 幂等标识)
PATCHES = [
    (
        INCLUDE_OLD,
        INCLUDE_NEW,
        "include",
        'banjo_shared_hooks.h',
    ),
    (
        "  __imp__sub_822D13F0(ctx, base);\n  const uint32_t ret = ctx.r3.u32;",
        "  if (::banjo::renut_gpu::Active()) {\n"
        "    ::renut_native_sub_822D13F0(ctx, base);  // Renut（deko3d）侧的 D3D_CBlocker_Check\n"
        "  } else {\n"
        "    __imp__sub_822D13F0(ctx, base);\n"
        "  }\n"
        "  const uint32_t ret = ctx.r3.u32;",
        "yield sub_822D13F0",
        "renut_native_sub_822D13F0(ctx, base)",
    ),
    (
        "  __imp__sub_822D4488(ctx, base);\n}",
        "  if (::banjo::renut_gpu::Active()) {\n"
        "    ::renut_native_sub_822D4488(ctx, base);  // Renut（deko3d）侧的 D3D_CDevice_BlockOnFence\n"
        "  } else {\n"
        "    __imp__sub_822D4488(ctx, base);\n"
        "  }\n}",
        "yield sub_822D4488",
        "renut_native_sub_822D4488(ctx, base)",
    ),
    (
        "  __imp__sub_822D4A08(ctx, base);\n  REXLOG_ERROR(\"[video] T2DIAG init sub_822D4A08 ret={}\", ctx.r3.u32);",
        "  if (::banjo::renut_gpu::Active()) {\n"
        "    ::renut_native_sub_822D4A08(ctx, base);  // Renut（deko3d）侧的 D3DDevice_SetRingBufferParameters\n"
        "  } else {\n"
        "    __imp__sub_822D4A08(ctx, base);\n"
        "  }\n"
        "  REXLOG_ERROR(\"[video] T2DIAG init sub_822D4A08 ret={}\", ctx.r3.u32);",
        "yield sub_822D4A08",
        "renut_native_sub_822D4A08(ctx, base)",
    ),
]


def main() -> int:
    text = TARGET.read_text(encoding="utf-8")
    original = text
    report = []
    for old, new, label, marker in PATCHES:
        if marker in text:
            report.append(f"SKIP (已应用) {label}")
            continue
        if text.count(old) != 1:
            report.append(f"FAIL {label}: 匹配 {text.count(old)} 次（需要 1 次）")
            continue
        text = text.replace(old, new)
        report.append(f"OK   {label}")

    if any(line.startswith("FAIL") for line in report):
        print("\n".join(report))
        print("有失败项，未写盘。")
        return 1

    if "--write" in sys.argv and text != original:
        shutil.copy2(TARGET, TARGET.with_suffix(".cpp.bak.yield"))
        TARGET.write_text(text, encoding="utf-8")
        report.append("WROTE (备份 t2rotf_video_nativo.cpp.bak.yield)")

    print("\n".join(report))
    # 落盘后的结构校验
    for _, _, label, marker in PATCHES:
        n = text.count(marker)
        print(f"CHECK {label}: marker x{n} (期望 {0 if label==chr(0) else 1})")
    n_imp = text.count("__imp__sub_822D13F0(ctx, base);") + text.count(
        "__imp__sub_822D4488(ctx, base);") + text.count("__imp__sub_822D4A08(ctx, base);")
    print(f"CHECK __imp__ 原体调用点 x{n_imp} (期望 3：各一处 else 分支)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())