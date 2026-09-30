#!/usr/bin/env bash
# Install the Linux build into one stable, self-contained directory and register
# it with the desktop. Nothing here needs root: everything lands under $HOME.
#
#   scripts/linux/install-native.sh                    build, install, register
#   scripts/linux/install-native.sh --no-build         install what is already built
#   scripts/linux/install-native.sh --no-desktop       skip .desktop, icon and dock
#   scripts/linux/install-native.sh --prefix DIR       install somewhere else
#
# Why a wrapper script instead of an rpath rewrite: there is no patchelf on this
# machine, and the Qt we ship is a full runtime directory that also wants
# QT_PLUGIN_PATH and a QML import path pointed at it. One three-line launcher is
# less machinery than re-writing a binary, and it keeps the .desktop Exec a path
# you can run by hand.
#
# The desktop file is named com.moonlight_stream.Moonlight because that is what
# app/main.cpp passes to setDesktopFileName(); GNOME matches the running window to
# the launcher through it. The snap's own entry is moonlight_moonlight.desktop, so
# the two do not overwrite each other, and the Name carries a suffix so that the
# dock shows which of them is which.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$HERE/env.sh"

PREFIX="${ML_PREFIX:-$HOME/.local/opt/moonlight-pyrowave}"
DO_BUILD=1
DO_DESKTOP=1
while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) DO_BUILD=0 ;;
        --no-desktop) DO_DESKTOP=0 ;;
        --prefix) shift; PREFIX="$1" ;;
        *) echo "install-native: unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

if [ "$DO_BUILD" = 1 ]; then
    bash "$HERE/build-moonlight.sh"
fi

BINARY="$ML_BUILD/tree/app/moonlight"
if [ ! -x "$BINARY" ]; then
    echo "install-native: $BINARY does not exist; build first" >&2
    exit 1
fi

if [ ! -x "$QT_DIR/bin/qmake6" ]; then
    echo "install-native: no Qt at $QT_DIR; nothing to bundle" >&2
    exit 1
fi

APP_ID=com.moonlight_stream.Moonlight
ICON_NAME="$APP_ID.svg"
rm -rf "$PREFIX"
mkdir -p "$PREFIX/bin" "$PREFIX/lib" "$PREFIX/plugins" "$PREFIX/qml" "$PREFIX/share"

cp "$BINARY" "$PREFIX/bin/moonlight-bin"
cp "$ML_REPO/app/res/moonlight.svg" "$PREFIX/share/$ICON_NAME"

# Which libraries have to come along.
#
# Everything Ubuntu already provides stays system-provided: bundling a second
# libGL, libplacebo or libX11 next to the app would risk a driver that does not
# match the rest of the system, which is a much worse failure than a missing file
# at startup. What does come along is Qt, because the Qt in noble is 6.4.2 and
# this does not build against it, plus anything the system loader cannot name at
# all (SDL2_ttf is the one that matters).
# The host's loader cache, not the sysroot's.
LDCONFIG_BIN=""
[ -x /usr/sbin/ldconfig ] && LDCONFIG_BIN=/usr/sbin/ldconfig
[ -n "$LDCONFIG_BIN" ] || LDCONFIG_BIN=$(command -v ldconfig 2>/dev/null)

BUNDLE_DENY="libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 \
ld-linux-x86-64-linux.so.2 ld-linux-x86-64.so.2 libgcc_s.so.1 libnss_compat.so.2 \
libnss_nis.so.2 libnss_files.so.2 libstdc++.so.6"

is_system_provided() {
    local soname="$1" deny dir
    for deny in $BUNDLE_DENY; do
        [ "$soname" = "$deny" ] && return 0
    done
    # The build sysroot puts its own ldconfig earlier on PATH and that one reads the
    # sysroot's cache, which knows about almost nothing the real system provides, so
    # use the host's by absolute path. If it is somehow missing, fall back to the
    # multiarch directories.
    if [ -n "$LDCONFIG_BIN" ]; then
        "$LDCONFIG_BIN" -p 2>/dev/null | grep -qE "[[:space:]]$soname \(" && return 0
    fi
    for dir in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib /lib                /usr/lib/x86_64-linux-gnu/pulseaudio; do
        [ -e "$dir/$soname" ] && return 0
    done
    return 1
}

# Where to look for a library the system does not have. SDL2_ttf is the real case:
# it comes from the .deb sysroot the build linked against, and nothing on this
# machine provides it.
find_unprovided() {
    local soname="$1" root candidate
    for candidate in "$QT_DIR/lib/$soname" "$PREFIX/lib/$soname"; do
        [ -e "$candidate" ] && { echo "$candidate"; return 0; }
    done
    for root in ${ML_SYSROOTS//:/ }; do
        candidate="$root/usr/lib/x86_64-linux-gnu/$soname"
        [ -e "$candidate" ] && { echo "$candidate"; return 0; }
        candidate="$root/usr/lib/$soname"
        [ -e "$candidate" ] && { echo "$candidate"; return 0; }
    done
    return 1
}

# Print what a file needs, one line per dependency: either the absolute path it
# resolves to, or "MISSING <soname>" for something the loader could not find. The
# search path is what the installed app will see, so a library that only exists in
# the build sysroot shows up as missing here instead of quietly being shipped.
deps_of() {
    LD_LIBRARY_PATH="$PREFIX/lib:$QT_DIR/lib" ldd "$1" 2>/dev/null | while read -r line; do
        case "$line" in
            *"=> not found"*) echo "MISSING ${line%% =>*}" ;;
            *"=> /"*) echo "$line" | sed 's/.*=> \(\/[^ ]*\).*/\1/' ;;
            *) [ -e "${line%% *}" ] && echo "${line%% *}" ;;
        esac
    done
}

bundle_lib() {
    local src="$1" soname

    case "$src" in
        MISSING\ *)
            soname="${src#MISSING }"
            soname="$(basename "$soname")"
            src=$(find_unprovided "$soname") || { echo "  MISSING: $soname (nowhere to copy it from)" >&2; return 0; }
            ;;
        /*.so|/*.so.*) soname=$(basename "$src") ;;
        *) return 0 ;;
    esac

    [ -e "$PREFIX/lib/$soname" ] && return 0

    # Qt's own runtime directory carries private copies of assorted system
    # libraries (ffmpeg, icu, vulkan, pulse ...), and those must not come along: the
    # versions Ubuntu ships are the ones the rest of the system was built against,
    # and a driver or codec loaded out of Qt's tree instead of the system's is a
    # failure that only shows up as a black screen. Only Qt's own libraries are
    # taken on faith, and they have to stay internally consistent - which is also
    # why Qt's icu is shipped even though the system has a newer one.
    case "$soname" in
        libQt6*) ;;
        *) is_system_provided "$soname" && return 0 ;;
    esac

    cp -p "$src" "$PREFIX/lib/$soname" 2>/dev/null || return 0
    echo "  lib: $soname"
    deps_of "$PREFIX/lib/$soname" | while read -r dep; do
        bundle_lib "$dep"
    done
}

echo "install-native: collecting libraries into $PREFIX/lib"
deps_of "$PREFIX/bin/moonlight-bin" | while read -r dep; do
    bundle_lib "$dep"
done

# Qt loads its plugins and its QML modules by path, at run time, so the set is not
# something ldd can tell us. Both trees are small enough to take whole.
echo "install-native: copying Qt plugins and QML modules"
cp -a "$QT_DIR/plugins/." "$PREFIX/plugins/" 2>/dev/null || echo "install-native: no plugins to copy"
if [ -d "$QT_DIR/qml" ]; then
    cp -a "$QT_DIR/qml/." "$PREFIX/qml/" 2>/dev/null || echo "install-native: no qml directory"
fi

cat > "$PREFIX/bin/moonlight" <<WRAPPER
#!/bin/sh
# Generated by scripts/linux/install-native.sh. Sets up the bundled Qt runtime and
# runs the real binary. QT_QPA_PLATFORM is left alone so that a Wayland session, a
# plain X11 session and QT_QPA_PLATFORM=offscreen all keep working the way they do
# today.
HERE=\$(dirname "\$0")
export LD_LIBRARY_PATH="\$HERE/../lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="\$HERE/../plugins\${QT_PLUGIN_PATH:+:\$QT_PLUGIN_PATH}"
export QML_IMPORT_PATH="\$HERE/../qml\${QML_IMPORT_PATH:+:\$QML_IMPORT_PATH}"
export QML2_IMPORT_PATH="\$HERE/../qml\${QML2_IMPORT_PATH:+:\$QML2_IMPORT_PATH}"
exec "\$HERE/moonlight-bin" "\$@"
WRAPPER
chmod +x "$PREFIX/bin/moonlight"

echo "install-native: installed into $PREFIX"
if [ "$DO_DESKTOP" = 0 ]; then
    exit 0
fi

APPS_DIR="$HOME/.local/share/applications"
ICONS_DIR="$HOME/.local/share/icons/hicolor/scalable/apps"
mkdir -p "$APPS_DIR" "$ICONS_DIR"
cp "$PREFIX/share/$ICON_NAME" "$ICONS_DIR/$ICON_NAME"

# StartupWMClass is the desktop file name rather than "Moonlight" because Qt makes
# the X11 WM_CLASS res_class from app.setDesktopFileName(), which app/main.cpp sets
# to this same id - that is what lets GNOME light up this launcher for the running
# window. Categories carries exactly one main category: with two or more, the
# freedesktop spec says the entry can show up once per category in the app grid.
cat > "$APPS_DIR/$APP_ID.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Version=1.0
Name=Moonlight (PyroWave)
GenericName=Game streaming client
GenericName[pt_BR]=Cliente de streaming de jogos
Comment=Stream games from a PC with GameSync or Sunshine (PyroWave Linux build)
Exec=$PREFIX/bin/moonlight %u
Icon=$PREFIX/share/$ICON_NAME
Terminal=false
Categories=Game;
Keywords=game;stream;sunshine;pyrowave;moonlight;
StartupNotify=true
StartupWMClass=com.moonlight_stream.Moonlight
X-GNOME-SingleWindow=true
DESKTOP

# A desktop file is only picked up once the MIME cache knows about it, and GNOME
# re-reads favourite-apps from gsettings rather than from the file we edit here.
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS_DIR" 2>/dev/null
command -v gtk-update-icon-cache >/dev/null 2>&1 && \
    gtk-update-icon-cache -q "$HOME/.local/share/icons/hicolor" 2>/dev/null

if command -v gsettings >/dev/null 2>&1; then
    current=$(gsettings get org.gnome.shell favorite-apps 2>/dev/null)
    if [ -n "$current" ] && ! grep -q "$APP_ID.desktop" <<<"$current"; then
        # Append rather than set: this list also carries everything else pinned to
        # the dock, and replacing it would quietly unpin the user's setup. The list
        # gsettings prints already ends in an apostrophe, so the new entry goes
        # after it, and an empty list has to be filled rather than extended.
        body="${current#\[}"
        body="${body%\]}"
        body="${body#[[:space:]]}"
        body="${body%[[:space:]]}"
        if [ -z "$body" ]; then
            updated="['$APP_ID.desktop']"
        else
            updated="[$body, '$APP_ID.desktop']"
        fi
        if gsettings set org.gnome.shell favorite-apps "$updated" 2>/dev/null; then
            echo "install-native: pinned $APP_ID to the dock"
        else
            echo "install-native: could not pin to the dock; add it from the apps grid"
        fi
    elif [ -n "$current" ]; then
        echo "install-native: already pinned to the dock"
    fi
fi

echo "install-native: desktop entry $APPS_DIR/$APP_ID.desktop"
echo "install-native: launch with $PREFIX/bin/moonlight"
