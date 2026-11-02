#!/usr/bin/env bash
# T2ROTF codegen wrapper.
#
# Re-runs the host codegen for T2ROTF after the layered-VFS SDK change:
#   - the Runtime now defaults update_data_root to <game>/update, so the
#     TU code patch default.xexp is found as the sibling of default.xex and
#     applied during XEX load;
#   - max_file_size_bytes is not parsed from the manifest by the manifest
#     parser, so it is passed on the command line as a cvar (4 MB, needed by
#     the TU-patched 0x82B7E3A0 function).
#
# Usage:
#   REXGLUE=/path/to/rexglue(.exe) bash recomp/codegen_t2rotf.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RECOMP="$ROOT/recomp"
REXGLUE="${REXGLUE:?set REXGLUE to the rexglue host executable built from sdk/}"
cd "$RECOMP"
echo "== codegen T2ROTF (TU patch + max_file_size_bytes=4MB) =="
"$REXGLUE" codegen T2ROTF_manifest.toml --update_data_root="../update" --max_file_size_bytes=4194304
# The prebuilt rexglue.exe embeds an old pch template whose non-Apple branch is
# `__attribute__((alias(...)))`; GCC 15 ignores that in declaration position, so
# codegen rewrites T2ROTF_pch.h into a form that fails to link (thousands of
# undefined `sub_*`). Restore the GCC weak-thunk variant used by the successful
# builds (same shape as the nfsmw-nx SDK template).
cp "$RECOMP/codegen_fix/pch_switch_gcc_fix.h" "$RECOMP/generated/default/T2ROTF_pch.h"
echo "codegen ok"