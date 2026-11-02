#!/bin/bash
# ============================================================================
#  T2ROTF Switch 移植 - 安装期合并脚本（TU 升级档 + DLC 并入 game/）
#
#  采用与 UnleashedRecomp-NX 相同的"安装期合并"模型：把兄弟目录中的 TU
#  解包（update/）与 DLC 解包（DLC/）合并进 game/ 单目录，运行时只需挂载
#  game/ 一层，不需要任何动态 Overlay。
#
#  合并后的生效规则：
#    [TU 升级档]
#      1. default.xexp -> game/（default.xex 同级）
#         rexglue SDK 的 UserModule::LoadFromFile 在加载 default.xex 时会
#         自动查找 path+"p"（即 game:\default.xexp）并调用
#         XexModule::ApplyPatch，把 base 补丁为 TU 版。
#      2. TU 数据覆盖（同名覆盖 base）：
#         camera.lxb / medaltimes.lxb / tuning.lxb  -> game/
#         text/{eng,fre,ger,ita,spa}/global.lxb     -> game/text/
#         ui/Flash/mpui_tu.{apk,gfx}                -> game/ui/Flash/
#         ui/uiglobal.lxb                           -> game/ui/
#
#    [DLC：Maps and Characters Pack]
#      - Audio/eng/{dlc_chatter.fev, DLC_CHATTER.fsb} -> game/audio/eng/
#        （DLC 源为 Audio 大写，game/ 内为 audio 小写，与 base 风格一致）
#      - characters/dlc* 角色目录/文件               -> game/characters/
#      - levels/mp_{cairocity,courtyard,hillside}    -> game/levels/
#      - dlc_global.str / packagedata.lxb / spa.bin  -> game/
#
#  用法：bash scripts/setup_tu_dlc.sh
#  重复执行安全（幂等：覆盖同名文件、不删除已有文件）。
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$ROOT/game"
TU_SRC="$ROOT/update"
DLC_SRC="$ROOT/DLC"

log() { printf '[setup] %s\n' "$*"; }

if [ ! -d "$GAME" ]; then
    echo "错误：找不到 $GAME（game/ 目录不存在，请先提取并解包 base 游戏）" >&2
    exit 1
fi
if [ ! -d "$TU_SRC" ] && [ ! -d "$DLC_SRC" ]; then
    echo "提示：update/ 与 DLC/ 都不存在，无需合并。" >&2
    exit 0
fi

# ---------------------------------------------------------------------------
# 1. TU 代码补丁 + TU 数据覆盖：整树并入 game/（default.xexp 落在 default.xex 同级）
# ---------------------------------------------------------------------------
if [ -d "$TU_SRC" ]; then
    cp -rf "$TU_SRC/." "$GAME/"
    log "TU 合并 -> $GAME/（来自 $TU_SRC/）"
fi

# ---------------------------------------------------------------------------
# 2. DLC 资源合并：Audio（大写 A）映射到 game/audio（小写，与 base 一致），其余整树并入
# ---------------------------------------------------------------------------
if [ -d "$DLC_SRC" ]; then
    if [ -d "$DLC_SRC/Audio" ]; then
        mkdir -p "$GAME/audio"
        cp -rf "$DLC_SRC/Audio/." "$GAME/audio/"
        log "DLC Audio -> $GAME/audio/（大小写归一为 base 风格）"
    fi
    # characters / levels / 根级文件：整树并入（与 base 无同名冲突）
    for entry in "$DLC_SRC"/characters "$DLC_SRC"/levels "$DLC_SRC"/dlc_global.str \
                 "$DLC_SRC"/packagedata.lxb "$DLC_SRC"/spa.bin; do
        if [ -e "$entry" ]; then
            cp -rf "$entry" "$GAME/"
            log "DLC 合并 -> $GAME/$(basename "$entry")"
        fi
    done
fi

# ---------------------------------------------------------------------------
# 3. 汇总结果
# ---------------------------------------------------------------------------
echo ""
log "=== 合并完成，结果汇总 ==="
echo "[TU] default.xexp:          $([ -f "$GAME/default.xexp" ] && echo OK || echo MISSING)"
echo "[TU] camera.lxb:            $([ -f "$GAME/camera.lxb" ] && echo OK || echo MISSING)"
echo "[TU] medaltimes.lxb:        $([ -f "$GAME/medaltimes.lxb" ] && echo OK || echo MISSING)"
echo "[TU] tuning.lxb:            $([ -f "$GAME/tuning.lxb" ] && echo OK || echo MISSING)"
echo "[TU] ui/uiglobal.lxb:       $([ -f "$GAME/ui/uiglobal.lxb" ] && echo OK || echo MISSING)"
echo "[TU] ui/Flash/mpui_tu.apk:  $([ -f "$GAME/ui/Flash/mpui_tu.apk" ] && echo OK || echo MISSING)"
echo "[TU] ui/Flash/mpui_tu.gfx:  $([ -f "$GAME/ui/Flash/mpui_tu.gfx" ] && echo OK || echo MISSING)"
echo "[DLC] dlc_global.str:       $([ -f "$GAME/dlc_global.str" ] && echo OK || echo MISSING)"
echo "[DLC] packagedata.lxb:      $([ -f "$GAME/packagedata.lxb" ] && echo OK || echo MISSING)"
echo "[DLC] spa.bin:              $([ -f "$GAME/spa.bin" ] && echo OK || echo MISSING)"
echo "[DLC] 角色文件数:            $(find "$GAME/characters" -maxdepth 1 -name 'dlc*' 2>/dev/null | wc -l)"
echo "[DLC] 新地图目录:            $(ls -d "$GAME"/levels/mp_cairocity "$GAME"/levels/mp_courtyard "$GAME"/levels/mp_hillside 2>/dev/null | xargs -n1 basename 2>/dev/null | tr '\n' ' ')"
echo ""
log "提示：TU 代码补丁在打包进生成代码的基础上，运行时还会随 default.xex 由"
log "      UserModule 自动应用 default.xexp；DLC 资源由游戏启动后自行接入。"
log "      部署到 Switch 时只需复制 game/ 单目录即可（update/、DLC/ 不再需要）。"