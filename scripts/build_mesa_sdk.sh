#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# T2ROTF - build the Mesa Switch OpenGL/EGL SDK (Nagaa95/mesa-switch fork)
#
# Produces app/sdk with libEGL.a, libGLESv2.a, libglapi.a and the EGL/GLES
# headers, built with the same devkitPro MSYS2 toolchain used by app/Makefile.
#
# Notes
# - Runs entirely inside the devkitPro MSYS2 (C:/devkitPro/msys2).
# - The devkitPro package repo (pkg.devkitpro.org) returns 403 in this
#   network, so zlib/zstd/expat/xmlconfig are disabled (they are optional
#   Mesa features). Shader cache is off for now; re-enable it once a
#   cross-compiled zstd is available (performance stage).
# -
# Usage:  bash scripts/build_mesa_sdk.sh
# ---------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="${ROOT}/repos/mesa-switch-nagaa"
MSYS_BASH="/c/devkitPro/msys2/usr/bin/bash.exe"
WORK="${ROOT}/build"
LOG="${WORK}/mesa-opengl-build.log"

if [[ ! -d "$REPO" ]]; then
    echo "ERROR: mesa-switch repo not found: $REPO" >&2
    exit 1
fi
if [[ ! -x "$MSYS_BASH" ]]; then
    echo "ERROR: devkitPro MSYS2 bash not found: $MSYS_BASH" >&2
    exit 1
fi

mkdir -p "$WORK"

# Convert the repo to a MSYS2 POSIX path (/j/...).
REPO_WIN="$(cygpath -w "$REPO")"
REPO_POSIX="$("$MSYS_BASH" -lc "cygpath -u '${REPO_WIN}'")"
echo "mesa repo (msys2): ${REPO_POSIX}"

# Inner script that runs inside devkitPro MSYS2.
INNER="${WORK}/mesa_build_inner.sh"
cat > "$INNER" <<EOF
#!/usr/bin/env bash
set -euo pipefail
export MSYSTEM=MSYS

REPO='${REPO_POSIX}'
cd "\$REPO"

echo "=== [1/3] meson configure (OpenGL/EGL, no zlib/zstd/expat) ==="
rm -rf builddir-opengl mesa-install
meson setup builddir-opengl \
    --cross-file switch_cross_file.txt \
    --default-library=static \
    --prefix=/opt/devkitpro/portlibs/switch \
    --libdir=lib \
    --buildtype=release \
    -Doptimization=2 \
    -Db_lto=false \
    -Db_ndebug=true \
    -Dvulkan-drivers= \
    -Dgallium-drivers=nouveau \
    -Dgallium-rusticl=false \
    -Dplatforms=switch \
    -Degl-native-platform=switch \
    -Dglx=disabled \
    -Degl=enabled \
    -Dopengl=true \
    -Dgles1=enabled \
    -Dgles2=enabled \
    -Dvideo-codecs= \
    -Dshader-cache=disabled \
    -Dxmlconfig=disabled \
    -Dexpat=disabled \
    -Dzlib=disabled \
    -Dzstd=disabled \
    -Dllvm=disabled \
    -Dshared-llvm=disabled \
    -Dcpp_rtti=false \
    -Dtools=[] \
    -Dbuild-tests=false

echo "=== [2/3] ninja build ==="
ninja -C builddir-opengl

echo "=== [3/3] install to mesa-install/opt/devkitpro/portlibs/switch ==="
meson install -C builddir-opengl --destdir mesa-install

echo "=== DONE ==="
EOF
sed -i 's/\r$//' "$INNER"

echo "Starting Mesa SDK build (log: $LOG) ..."
( "$MSYS_BASH" -lc "bash '${INNER}'" ) > "$LOG" 2>&1 &
echo $! > "${WORK}/mesa_build.pid"
echo "build pid: $(cat ${WORK}/mesa_build.pid)"

# Copy the staged SDK into app/sdk once the inner build has finished.
(
    while kill -0 "$(cat ${WORK}/mesa_build.pid)" 2>/dev/null; do
        sleep 15
    done
    if grep -q "=== DONE ===" "$LOG"; then
        SDK_STAGED="${REPO}/mesa-install/opt/devkitpro/portlibs/switch"
        if [[ -d "$SDK_STAGED" ]]; then
            echo "Syncing SDK to app/sdk ..."
            rm -rf "${ROOT}/app/sdk"
            mkdir -p "${ROOT}/app/sdk"
            cp -r "${SDK_STAGED}/." "${ROOT}/app/sdk/"
            echo "SDK ready at ${ROOT}/app/sdk"
        fi
    fi
) > "${WORK}/mesa_sdk_sync.log" 2>&1 &
echo "sync watcher started"
echo "TIP: tail -f ${LOG}"