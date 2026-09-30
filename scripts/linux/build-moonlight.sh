#!/usr/bin/env bash
# One-shot native build inside the repo. Everything stays under build/ (git
# ignored) and nothing needs root.
#
#   scripts/linux/build-moonlight.sh             incremental
#   scripts/linux/build-moonlight.sh clean       wipe the build tree first
#   scripts/linux/build-moonlight.sh bootstrap   also (re)fetch the .deb sysroot
#
# Any further argument is handed to qmake, e.g. CONFIG+=disable-pyrowave or
# CONFIG+=tests for the deterministic VRR and PyroWave test targets.
set -euo pipefail

_here="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
# shellcheck disable=SC1091
source "$_here/env.sh"

MODE="${1:-}"
case "$MODE" in
    bootstrap)
        shift
        bash "$_here/fetch-debs.sh"
        ;;
    clean)
        shift
        ;;
esac
# Whatever is left is a qmake argument (CONFIG+=tests, CONFIG+=disable-pyrowave, ...).

if [ "$MODE" = clean ]; then
    rm -rf "$ML_BUILD/tree"
fi

cd "$ML_REPO"
git submodule update --init --recursive

TREE="$ML_BUILD/tree"
mkdir -p "$TREE"
cd "$TREE"

# ffnvcodec headers only exist on machines that fetched them; the CUDA/NVDEC
# renderer is opt-in and stays off otherwise (VDPAU, VA-API and Vulkan remain
# available).
ML_CONFIG=(CONFIG+=release QMAKE_CXXFLAGS+=-fPIC)
if pkg-config --exists ffnvcodec 2>/dev/null; then
    echo "build-moonlight: ffnvcodec found -> CONFIG+=enable-cuda"
    ML_CONFIG+=(CONFIG+=enable-cuda)
else
    echo "build-moonlight: no ffnvcodec -> CUDA renderer off"
fi

# Passing any qmake argument re-runs qmake, so CONFIG+=tests actually reaches
# the existing Makefile instead of being silently ignored.
if [ ! -f Makefile ] || [ "$#" -gt 0 ]; then
    qmake6 "$ML_REPO/moonlight-qt.pro" "${ML_CONFIG[@]}" "$@"
fi
make -j"$(nproc)"

echo "Binary: $TREE/app/moonlight"
