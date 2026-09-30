#!/usr/bin/env bash
# Build a private sysroot of build dependencies without root: apt-get download
# plus dpkg -x into build/tools/sysroot, which env.sh searches before any shared
# sysroot. Only packages the shared sysroot cannot already resolve are fetched,
# so running this on a machine that has nothing costs a few hundred MB and on a
# machine with a sibling checkout costs almost nothing.
#
# The names below are the Xorg protocol metadata and the Xcb/X11 plumbing that
# moonlight-qt's pkg-config probes (app/app.pro: packagesExist(x11), x11-xcb)
# need on top of what the runtime libraries already ship. x11.pc Requires
# xproto and kbproto, which live in x11proto-dev and are not pulled in by
# libx11-dev itself.
set -uo pipefail

_here="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
# shellcheck disable=SC1091
source "$_here/env.sh"

SYSROOT="$ML_BUILD/tools/sysroot"
DL="$ML_BUILD/tools/debs"
mkdir -p "$SYSROOT" "$DL"

PKGS="x11proto-dev libx11-dev libx11-xcb-dev libxcb1-dev libxcb-xkb-dev
libxkbcommon-dev libxkbcommon-x11-dev libdrm-dev libgbm-dev libegl-dev
libgl-dev libgles-dev libxext-dev libxfixes-dev libxi-dev libxrandr-dev
libxinerama-dev libxcursor-dev libxss-dev libxt-dev libxv-dev libxxf86vm-dev
libxcb-dri3-dev libpthread-stubs0-dev libwayland-dev libegl1-mesa-dev
libgl1-mesa-dev libglu1-mesa-dev"

# What the pkg-config probes still cannot find through the current search path.
# The list is exactly what app/app.pro asks qmake to find, minus the optional
# probes: mmal is Raspberry Pi only and ffnvcodec is opt-in from vendor/.
missing_pkgs() {
    local probe
    for probe in openssl sdl2 SDL2_ttf opus libavcodec libavutil libswscale \
                 libva libva-x11 libva-wayland libva-drm vdpau libdrm \
                 libplacebo wayland-client x11; do
        pkg-config --exists "$probe" 2>/dev/null || echo "$probe"
    done
}

before="$(missing_pkgs | tr '\n' ' ')"
echo "fetch-debs: unresolved probes before: ${before:-none}"
if [ -z "${before// /}" ]; then
    echo "fetch-debs: nothing to do"
    exit 0
fi

cd "$DL"
# Unprivileged simulation = the closure of what is actually missing from the
# system, unioned with the list above so the sysroot stays complete.
MISSING=$(apt-get install -s --no-install-recommends $PKGS 2>/dev/null | awk '/^Inst /{print $2}' | sort -u)
ALL=$(echo "$PKGS $MISSING" | tr ' ' '\n' | sort -u | grep -v '^$')
echo "fetch-debs: $(echo "$ALL" | wc -l) packages in the closure ($(echo "$MISSING" | wc -l) missing from the system)"
for p in $ALL; do
    apt-get download -qq -o APT::Sandbox::User=root "$p" >/dev/null 2>&1 || echo "fetch-debs: no such package: $p"
done
for d in *.deb; do
    dpkg-deb -x "$d" "$SYSROOT" >/dev/null 2>&1 || echo "fetch-debs: extract failed: $d"
done

# -dev packages ship "libfoo.so -> libfoo.so.N" while the runtime .so.N often
# lives in a package that is already installed, which leaves a dangling link and
# "cannot find -lfoo". Re-point those at the real system library. Idempotent.
fixed=0
for _ml_s in ${ML_SYSROOTS//:/ }; do
    [ -d "$_ml_s/usr/lib" ] || continue
    for pass in 1 2; do
        while IFS= read -r L; do
            t=$(basename "$(readlink "$L")")
            [ -e "$(dirname "$L")/$t" ] && continue
            real=""
            for d in /usr/lib/x86_64-linux-gnu /usr/lib /lib/x86_64-linux-gnu; do
                [ -e "$d/$t" ] && real="$d/$t" && break
            done
            [ -z "$real" ] && real=$(find /usr/lib /lib -name "$t" 2>/dev/null | head -1)
            [ -n "$real" ] && { ln -sfn "$real" "$L"; fixed=$((fixed + 1)); }
        done < <(find "$_ml_s/usr/lib" -xtype l -name '*.so*')
    done
done
echo "fetch-debs: re-pointed $fixed dangling dev symlink(s)"

after="$(missing_pkgs | tr '\n' ' ')"
echo "fetch-debs: unresolved probes after: ${after:-none}"
rm -rf "$DL"
[ -z "${after// /}" ]
