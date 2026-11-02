#!/usr/bin/env bash
# T2ROTF - build fixup: rebuild libexpat.a / libzstd.a so their .rodata jump
# tables (non-PIC absolute pointers) land in .data.rel.ro (RW) instead of the
# read-only segment.
#
# WHY (v6.28-RELROFIX):
#   devkitPro's portlibs expat/zstd are compiled without -fPIC. AArch64 switch
#   tables and function-pointer sets go into .rodata as absolute addresses,
#   so linking a PIE performs RELATIVE relocations *inside the read-only
#   segment*:
#     - default ld (-z text) -> "read-only segment has dynamic relocations"
#     - previous workaround -z notext rebuilt fine, but __nx_dynamic writes
#       those relocs to the RO page at NRO startup, before C++ runtime init,
#       so the process Data-Aborts with NO NRO log at all (only Atmosphere
#       crash report 0xCD59 => svcBreak from diagAbortWithResult).
#   Fix: objcopy --rename-section .rodata=.data.rel.ro on the affected members
#   so the linker places them under the *RW* :data PHDR via the .data.rel.ro
#   pattern in switch.ld.
#
# Idempotent: regenerates the fixed archives into FIXED_DIR every run.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXED_DIR="$ROOT/repos/nfsmw-nx/fixed-libs"

# The linking toolchain resolves DEVKITPRO to the sandbox copy (see
# CMakeCache.txt: DEVKITPRO=C:/Users/.../lightsandbox/git/opt/devkitpro).
# The environment variable may point at a different install (/opt/devkitpro)
# whose archives use different member names, so resolve the source consistently
# from the CMake cache of the last Switch configure, falling back to the
# sandbox layout then to $DEVKITPRO.
_CACHE_DEVKITPRO=""
if [[ -f "$ROOT/recomp/out/sw8/CMakeCache.txt" ]]; then
    _CACHE_DEVKITPRO="$(sed -n 's/^DEVKITPRO:PATH=//p' "$ROOT/recomp/out/sw8/CMakeCache.txt" | head -1)"
fi
if [[ -n "$_CACHE_DEVKITPRO" && -d "$_CACHE_DEVKITPRO/portlibs/switch/lib" ]]; then
    SRC_DIR="$_CACHE_DEVKITPRO/portlibs/switch/lib"
elif [[ -d "/c/Users/Administrator/AppData/Roaming/qianfan-desktop-app/lightsandbox/git/opt/devkitpro/portlibs/switch/lib" ]]; then
    SRC_DIR="/c/Users/Administrator/AppData/Roaming/qianfan-desktop-app/lightsandbox/git/opt/devkitpro/portlibs/switch/lib"
else
    SRC_DIR="${DEVKITPRO:-/opt/devkitpro}/portlibs/switch/lib"
fi
echo "==> Using devkitPro portlibs: $SRC_DIR"

# devkitA64 binutils may not be on PATH; resolve them so the script is runnable
# from a bare shell (build_switch.sh calls this before cmake configures).
if ! command -v aarch64-none-elf-objcopy >/dev/null 2>&1; then
    for _cand in \
        "${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin" \
        "/c/Users/Administrator/AppData/Roaming/qianfan-desktop-app/lightsandbox/git/opt/devkitpro/devkitA64/bin" \
        "/opt/devkitpro/devkitA64/bin"; do
        if [ -x "$_cand/aarch64-none-elf-objcopy.exe" ] || [ -x "$_cand/aarch64-none-elf-objcopy" ]; then
            PATH="$_cand:$PATH"
            break
        fi
    done
fi

NM="${NM:-aarch64-none-elf-nm}"
OBJCOPY="${OBJCOPY:-aarch64-none-elf-objcopy}"

fix_lib() {
  local archive="$1"; shift      # basename of the source archive
  local tmp; tmp="$(mktemp -d)"
  local members=("$@")           # archive members to fix (empty = all with relocs)

  echo "==> Fixing $archive -> $FIXED_DIR/$archive"
  ( cd "$tmp" && ar x "$SRC_DIR/$archive" )

  if [[ ${#members[@]} -eq 0 ]]; then
    # auto-detect: rename .rodata->.data.rel.ro only on members whose .rodata
    # carries ABS64/RELATIVE/GSYM relocations
    for f in "$tmp"/*.o; do
      if "$OBJCOPY" -r "$f" 2>/dev/null | awk '
          /RELOCATION RECORDS FOR \[\.rodata/{p=1;next}
          /RELOCATION RECORDS FOR/{p=0}
          p && /ABS64|RELATIVE|GSYM/{found=1}
          END{exit !found}' ; then
        members+=("$(basename "$f")")
      fi
    done
  fi

  for m in "${members[@]}"; do
    echo "    - rename .rodata -> .data.rel.ro : $m"
    ( cd "$tmp" && "$OBJCOPY" --rename-section .rodata=.data.rel.ro,alloc,load,data "$m" )
  done

  mkdir -p "$FIXED_DIR"
  ( cd "$tmp" && ar rcs "$FIXED_DIR/$archive" *.o )
  rm -rf "$tmp"
}

# expat: xmlparse.o/xmlrole.o/xmltok.o all carry .rodata ABS64 relocs
# zstd: error_private.o/zstd_compress.o/zstd_opt.o/zstd_preSplit.o
fix_lib libexpat.a xmlparse.o xmlrole.o xmltok.o
fix_lib libzstd.a error_private.o zstd_compress.o zstd_opt.o zstd_preSplit.o

echo "==> Fixed libs ready in $FIXED_DIR"
ls -la "$FIXED_DIR"