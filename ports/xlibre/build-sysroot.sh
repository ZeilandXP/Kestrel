#!/bin/sh
# ports/xlibre/build-sysroot.sh SRC_DIR SYSROOT
#
# Builds the libraries the XLibre server needs as static musl libraries into
# SYSROOT (headers, lib*.a, pkg-config files). SRC_DIR holds the upstream
# release tarballs (*.orig.tar.* from Ubuntu's source packages work:
# `apt-get source --download-only xorgproto xtrans libxau libxdmcp xcb-proto
# libxcb libx11 libxkbfile libfontenc libxfont pixman zlib libxext libxrender
# libxfixes libxdamage libxcomposite libxinerama libxrandr libxpm libpng1.6
# libice libsm libxt libxmu libxaw ncurses x11-xserver-utils libxshmfence libglvnd`;
# x11-xserver-utils holds xrandr, which build.sh compiles; libglvnd only
# gives the OpenGL headers). The current
# XLibre server needs xorgproto 2024.1 or newer (presentproto 1.4); Ubuntu
# 24.04 has 2023.2, so take xorgproto_2024.1.orig.tar.gz from a later
# release's pool.
set -eu
SRC=$(cd "$1" && pwd)
SYSROOT=$(mkdir -p "$2" && cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$SYSROOT/.build
mkdir -p "$WORK"

export CC="musl-gcc"
export CFLAGS="-O2 -g -fPIE"
export CPPFLAGS="-I$SYSROOT/include"
export LDFLAGS="-L$SYSROOT/lib"
export PKG_CONFIG_PATH=
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig:$SYSROOT/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=
export ACLOCAL_PATH="$SYSROOT/share/aclocal"
HOSTOPT="--host=x86_64-linux-musl --prefix=$SYSROOT --enable-static --disable-shared"

unpack() {      # unpack NAME-PREFIX -> echo directory
    t=$(ls "$SRC"/$1_*.orig.tar.* | grep -v '\.asc$' | head -1)
    d="$WORK/$1"
    rm -rf "$d"; mkdir -p "$d"
    tar -xf "$t" -C "$d" --strip-components=1
    # the distribution's fixes: a .diff.gz, or debian/patches from a .debian.tar.*
    diff=$(ls "$SRC"/$1_*.diff.gz 2>/dev/null | head -1 || true)
    deb=$(ls "$SRC"/$1_*.debian.tar.* 2>/dev/null | head -1 || true)
    if [ -n "$diff" ]; then zcat "$diff" | (cd "$d" && patch -p1 -s >/dev/null); fi
    if [ -n "$deb" ]; then tar -xf "$deb" -C "$d"; fi
    if [ -f "$d/debian/patches/series" ]; then
        grep -v '^#' "$d/debian/patches/series" | while read -r p _; do
            [ -n "$p" ] && (cd "$d" && patch -p1 -s < "debian/patches/$p" >/dev/null) || true
        done
    fi
    echo "$d"
}

autotools() {   # NAME [configure args...]
    n=$1; shift
    [ -f "$WORK/$n.done" ] && return 0
    echo "=== $n"
    d=$(unpack "$n")
    (cd "$d" && ./configure $HOSTOPT "$@" >"$WORK/$n.log" 2>&1 && make -j${MAKEJ:-8} >>"$WORK/$n.log" 2>&1 && make install >>"$WORK/$n.log" 2>&1) \
        || { echo "$n failed, see $WORK/$n.log"; tail -20 "$WORK/$n.log"; exit 1; }
    touch "$WORK/$n.done"
}

mesonbuild() {  # NAME [meson args...]
    n=$1; shift
    [ -f "$WORK/$n.done" ] && return 0
    echo "=== $n"
    d=$(unpack "$n")
    (cd "$d" && meson setup _b --prefix="$SYSROOT" --libdir=lib --default-library=static --buildtype=release "$@" \
        >"$WORK/$n.log" 2>&1 && ninja -C _b install >>"$WORK/$n.log" 2>&1) \
        || { echo "$n failed, see $WORK/$n.log"; tail -20 "$WORK/$n.log"; exit 1; }
    touch "$WORK/$n.done"
}

# zlib: its own configure (no --host)
if [ ! -f "$WORK/zlib.done" ]; then
    echo "=== zlib"
    d=$(unpack zlib)
    (cd "$d" && CC=musl-gcc ./configure --prefix="$SYSROOT" --static >"$WORK/zlib.log" 2>&1 && make -j8 install >>"$WORK/zlib.log" 2>&1) \
        || { echo "zlib failed"; tail -20 "$WORK/zlib.log"; exit 1; }
    touch "$WORK/zlib.done"
fi

mesonbuild xorgproto -Dlegacy=false
autotools  xtrans
autotools  libxau
autotools  libxdmcp
autotools  xcb-proto
MAKEJ=1 autotools  libxcb --disable-devel-docs --without-doxygen
autotools  libx11 --disable-specs --without-xmlto --without-fop --disable-loadable-i18n --disable-xf86bigfont
autotools  libxkbfile
autotools  libfontenc
autotools  libxfont --disable-devel-docs --disable-freetype --disable-bzip2
mesonbuild pixman -Dgtk=disabled -Dlibpng=disabled -Dopenmp=disabled -Dtests=disabled
# DRI3's fences: shared-memory futexes, the same layout Mesa's clients use.
# musl-gcc sees no kernel headers; the two futex operations it needs:
if [ ! -f "$SYSROOT/include/linux/futex.h" ]; then
    mkdir -p "$SYSROOT/include/linux"
    printf '#ifndef _LINUX_FUTEX_H\n#define _LINUX_FUTEX_H\n#define FUTEX_WAIT 0\n#define FUTEX_WAKE 1\n#endif\n' \
        > "$SYSROOT/include/linux/futex.h"
fi
autotools  libxshmfence --enable-futex

# client libraries for window managers and toolkits (IceWM)
autotools  libxext --disable-specs --without-xmlto --without-fop
autotools  libxrender
autotools  libxfixes
autotools  libxdamage
autotools  libxcomposite --disable-doc --without-xmlto
autotools  libxinerama
autotools  libxrandr
autotools  libxpm --disable-open-zfile --disable-stat-zfile
autotools  libpng1.6 --disable-tools

# X toolkit (Xt + Athena widgets) and terminfo, for xterm
autotools  libice --disable-docs --disable-specs --without-xmlto --without-fop
autotools  libsm --disable-docs --without-xmlto --without-fop --without-libuuid
autotools  libxt --disable-specs --without-xmlto --without-fop --with-appdefaultdir=/boot/share/X11/app-defaults
autotools  libxmu --disable-docs --without-xmlto --without-fop
autotools  libxaw --disable-specs --without-xmlto --without-fop --disable-xaw6
autotools  ncurses --without-cxx --without-cxx-binding --without-ada --without-progs --without-tests \
           --without-manpages --without-debug --enable-widec --disable-db-install --enable-pc-files \
           --with-pkg-config-libdir="$SYSROOT/lib/pkgconfig" --with-terminfo-dirs=/boot/share/terminfo \
           --with-default-terminfo-dir=/boot/share/terminfo --disable-home-terminfo --without-shared

# libsha1 (ours)
if [ ! -f "$WORK/libsha1.done" ]; then
    echo "=== libsha1"
    musl-gcc $CFLAGS -c "$HERE/libsha1.c" -I"$HERE" -o "$WORK/libsha1.o"
    ar rcs "$SYSROOT/lib/libsha1.a" "$WORK/libsha1.o"
    cp "$HERE/libsha1.h" "$SYSROOT/include/"
    cat > "$SYSROOT/lib/pkgconfig/libsha1.pc" <<PC
prefix=$SYSROOT
Name: libsha1
Description: SHA1 for the X server (Kestrel)
Version: 0.3
Cflags: -I\${prefix}/include
Libs: -L\${prefix}/lib -lsha1
PC
    touch "$WORK/libsha1.done"
fi
# OpenGL headers and a libGL of empty entry points, for the server's GLX
# (hw-kestrel/kglx.c). Xkestrel only offers direct rendering: the client's
# own libGL (Mesa) draws, and the indirect-rendering code GLX carries, which
# calls GL itself, never runs because the server makes no GL contexts. So
# the functions only need to exist for the link. The headers are libglvnd's.
if [ ! -f "$WORK/gl.done" ]; then
    echo "=== gl (headers, stub libGL)"
    d=$(unpack libglvnd)
    mkdir -p "$SYSROOT/include/GL" "$SYSROOT/include/KHR"
    cp "$d"/include/GL/gl.h "$d"/include/GL/glext.h "$d"/include/GL/glx.h "$d"/include/GL/glxext.h \
       "$d"/include/GL/glcorearb.h "$SYSROOT/include/GL/"
    cp "$d"/include/KHR/khrplatform.h "$SYSROOT/include/KHR/"
    {
        echo '/* generated by build-sysroot.sh: GL entry points that do nothing (see there) */'
        cat "$d"/include/GL/gl.h "$d"/include/GL/glext.h | grep -oE 'APIENTRY +gl[A-Za-z0-9_]+ *\(' \
            | sed -E 's/APIENTRY +//; s/ *\(//' | sort -u | sed 's/.*/void &(void) { }/'
    } > "$WORK/glstub.c"
    musl-gcc $CFLAGS -c "$WORK/glstub.c" -o "$WORK/glstub.o"
    ar rcs "$SYSROOT/lib/libGL.a" "$WORK/glstub.o"
    cat > "$SYSROOT/lib/pkgconfig/gl.pc" <<PC
prefix=$SYSROOT
Name: gl
Description: OpenGL headers and empty entry points for the X server's GLX (Kestrel)
Version: 4.6
Cflags: -I\${prefix}/include
Libs: -L\${prefix}/lib -lGL
PC
    touch "$WORK/gl.done"
fi

# Everything is static: put the private dependencies on the Libs lines so
# a plain `pkg-config --libs` (what meson asks for) links completely.
sed -i 's|^Libs: \(.*-lXfont2\)$|Libs: \1 -lfontenc -lz|' "$SYSROOT/lib/pkgconfig/xfont2.pc"
sed -i 's|^Libs: \(.*-lfontenc\)$|Libs: \1 -lz|' "$SYSROOT/lib/pkgconfig/fontenc.pc"
echo "sysroot ready: $SYSROOT"
