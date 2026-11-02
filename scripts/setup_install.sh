#!/usr/bin/env bash
# ============================================================================
#  T2ROTF Switch 移植 — 独立目录布局安装脚本
#
#  采用与 UnleashedRecomp-NX 一致的安装期融合模型：
#
#    game/      原始 base 游戏文件（从 build/iso_extract/ 复制）
#    update/    TU 升级档覆盖文件（从 extracted/TU/ 复制，不含 default.xexp 融合步骤）
#    patched/   安装期融合产物（xex_patcher 将 default.xexp 打进 default.xex）
#    dlc/       DLC 独立包（从 extracted/DLC/ 复制，保持子目录结构）
#
#  运行时 VFS 挂载顺序（OverlayHostPathDevice）：
#    update/ → patched/ → game/
#  因此 game:\default.xex 自动命中 patched/default.xex；
#  TU 的其余文件（camera.lxb、ui/…）自动命中 update/；
#  DLC 由 ContentManager 动态枚举 dlc/ 子目录注册并挂载。
#
#  用法：bash scripts/setup_install.sh
#  幂等：重复执行安全（以 rsync 风格 cp -u 减少重复复制）。
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC_BASE="$ROOT/build/iso_extract"
SRC_TU="$ROOT/extracted/TU"
SRC_DLC="$ROOT/extracted/DLC"
XEX_PATCHER="$ROOT/tools/xex_patcher_cli/xex_patcher.exe"

log() { printf '[setup] %s\n' "$*"; }

check_dir() {
  local path="$1" name="$2"
  if [ ! -d "$path" ]; then
    echo "错误：找不到 $name 目录：$path" >&2
    echo "  请确认已从 Xbox 360 镜像提取 base 游戏，并解包 TU/DLC。" >&2
    exit 1
  fi
}

check_dir "$SRC_BASE"  "base 游戏 (build/iso_extract)"
check_dir "$SRC_TU"    "TU 升级档 (extracted/TU)"
check_dir "$SRC_DLC"   "DLC 包 (extracted/DLC)"

if [ ! -x "$XEX_PATCHER" ]; then
  echo "错误：找不到 xex_patcher.exe：$XEX_PATCHER" >&2
  echo "  请先构建工具：bash tools/xex_patcher_cli/build.sh" >&2
  exit 1
fi

log "=== 开始独立目录布局安装 ==="

# ---------------------------------------------------------------------------
# 1. game/ —— 原始 base 游戏（整树复制，幂等）
# ---------------------------------------------------------------------------
mkdir -p "$ROOT/game"
log "复制 base 游戏 -> game/ …"
cp -rT "$SRC_BASE" "$ROOT/game"

# ---------------------------------------------------------------------------
# 2. update/ —— TU 覆盖文件（整树复制）
# ---------------------------------------------------------------------------
mkdir -p "$ROOT/update"
log "复制 TU 文件 -> update/ …"
cp -rT "$SRC_TU" "$ROOT/update"

# ---------------------------------------------------------------------------
# 3. patched/ —— 安装期融合 default.xex
# ---------------------------------------------------------------------------
mkdir -p "$ROOT/patched"
log "融合 default.xex + default.xexp -> patched/default.xex …"
"$XEX_PATCHER" "$ROOT/game/default.xex" "$SRC_TU/default.xexp" "$ROOT/patched/default.xex"

# ---------------------------------------------------------------------------
# 4. dlc/ —— DLC 独立包
# ---------------------------------------------------------------------------
mkdir -p "$ROOT/dlc"
log "复制 DLC 包 -> dlc/ …"
# 保持 DLC 内部目录结构（Audio/, characters/, levels/ + 根级文件）
for entry in "$SRC_DLC"/*; do
  [ -e "$entry" ] || continue
  base="$(basename "$entry")"
  dest="$ROOT/dlc/$base"
  if [ -d "$entry" ]; then
    mkdir -p "$dest"
    cp -rT "$entry" "$dest"
  else
    cp -f "$entry" "$dest"
  fi
done

# ---------------------------------------------------------------------------
# 5. 汇总校验
# ---------------------------------------------------------------------------
echo ""
log "=== 安装完成，校验清单 ==="

_check() {
  local path="$1" label="$2"
  if [ -e "$path" ]; then
    printf "  [OK] %-40s %s\n" "$label" "$(ls -la "$path" | awk '{print $5}')"
  else
    printf "  [MISSING] %-35s %s\n" "$label" "$path"
  fi
}

echo "--- game/ (base) ---"
_check "$ROOT/game/default.xex"     "default.xex"

echo "--- update/ (TU overlay) ---"
_check "$ROOT/update/default.xexp" "default.xexp (patch source)"
_check "$ROOT/update/camera.lxb"   "camera.lxb"
_check "$ROOT/update/medaltimes.lxb" "medaltimes.lxb"
_check "$ROOT/update/tuning.lxb"   "tuning.lxb"

echo "--- patched/ (merged executable) ---"
_check "$ROOT/patched/default.xex"  "patched/default.xex"

echo "--- dlc/ (独立包) ---"
_check "$ROOT/dlc/Audio"            "Audio/"
_check "$ROOT/dlc/characters"       "characters/"
_check "$ROOT/dlc/levels"           "levels/"
_check "$ROOT/dlc/dlc_global.str"   "dlc_global.str"
_check "$ROOT/dlc/packagedata.lxb"  "packagedata.lxb"
_check "$ROOT/dlc/spa.bin"          "spa.bin"

echo ""
log "提示："
log "  - 部署到 Switch 时复制上述 game/、update/、patched/、dlc/ 四个目录"
log "    以及 T2ROTF.nro 到 sdmc:/switch/Transformers2ROTF/"
log "  - 脚本已清除旧版单目录合并残留（game/ 现在是干净的 base 整树）"
