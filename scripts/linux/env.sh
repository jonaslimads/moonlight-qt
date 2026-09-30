# Moonlight Linux build environment. Nothing is installed system-wide and no
# command in this directory needs root.
#
# This file is meant to be *sourced* (it exports variables), never executed.
#
# Qt and the -dev packages are large and identical for every moonlight-qt
# checkout on this machine, so they are shared with a sibling checkout when one
# already has them. ML_TOOLS points at the directory that carries Qt/, sysroot/
# and vendor/; it defaults to this repo's build/tools and falls back to the
# sibling that first has a usable Qt. Set ML_TOOLS to pin it explicitly.
_ml_here="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"   # <repo>/scripts/linux
export ML_REPO="${ML_REPO:-"$(cd "$_ml_here/../.." && pwd)"}"  # <repo>
export ML_BUILD="${ML_BUILD:-"$ML_REPO/build"}"                # <repo>/build

if [ -z "${ML_TOOLS:-}" ]; then
    for _ml_cand in "$ML_BUILD/tools" \
                    "$ML_REPO/../vibemis/build/tools"; do
        if [ -x "$_ml_cand/Qt/6.8.3/gcc_64/bin/qmake6" ]; then
            ML_TOOLS="$_ml_cand"
            break
        fi
    done
    export ML_TOOLS="${ML_TOOLS:-$ML_BUILD/tools}"
fi

export QT_DIR="$ML_TOOLS/Qt/6.8.3/gcc_64"
# This repo's own sysroot (fetch-debs.sh) plus any shared one, own first.
export ML_SYSROOTS="$ML_BUILD/tools/sysroot:$ML_TOOLS/sysroot"
export VENDOR="$ML_TOOLS/vendor"

export PATH="$QT_DIR/bin:$ML_BUILD/tools/sysroot/usr/bin:$ML_TOOLS/sysroot/usr/bin:$VENDOR/bin:$PATH"

_ml_pc=""
for _ml_s in ${ML_SYSROOTS//:/ }; do
    [ -d "$_ml_s" ] && _ml_pc="$_ml_pc:$_ml_s/usr/lib/x86_64-linux-gnu/pkgconfig:$_ml_s/usr/share/pkgconfig"
done
export PKG_CONFIG_PATH="${_ml_pc#:}:$VENDOR/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# Extracted .pc files keep their /usr paths and many headers live in versioned
# subdirs (SDL2, opus, va, ...), so put every include dir of every sysroot on
# CPATH rather than rewriting the .pc files. LIBRARY_PATH covers linking and
# LD_LIBRARY_PATH lets the sysroot pkg-config find libpkgconf.
if [ -z "${ML_CPATH_DONE:-}" ]; then
    _ml_cpath=""
    _ml_libpath=""
    for _ml_s in ${ML_SYSROOTS//:/ }; do
        [ -d "$_ml_s" ] || continue
        _ml_cpath="$_ml_cpath:$_ml_s/usr/include:$_ml_s/usr/include/x86_64-linux-gnu:$_ml_s/usr/include/SDL2"
        _ml_libpath="$_ml_libpath:$_ml_s/usr/lib/x86_64-linux-gnu"
        export LD_LIBRARY_PATH="$_ml_s/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    done
    export CPATH="${_ml_cpath#:}:$VENDOR/include"
    export LIBRARY_PATH="${_ml_libpath#:}${LIBRARY_PATH:+:$LIBRARY_PATH}"
    _ml_extra=""
    for _ml_s in ${ML_SYSROOTS//:/ }; do
        [ -d "$_ml_s" ] || continue
        for d in "$_ml_s"/usr/include/*/ "$_ml_s"/usr/lib/x86_64-linux-gnu/glib-2.0/include/; do
            [ -d "$d" ] && _ml_extra="$_ml_extra:$d"
        done
    done
    for d in "$VENDOR"/include/*/; do
        [ -d "$d" ] && _ml_extra="$_ml_extra:$d"
    done
    export CPATH="$CPATH${_ml_extra}"
    export ML_CPATH_DONE=1
fi

echo "env: qmake=$QT_DIR/bin/qmake6 tools=$ML_TOOLS"
