#!/bin/sh
# ports/sysvinit/build.sh SYSVINIT_SRC BUILD
#
# SysVinit for Kestrel: init (pid 1), telinit, halt/reboot/poweroff,
# shutdown, runlevel, killall5/pidof and wall, as static-PIE musl programs
# in BUILD/sysvinit-root/sbin, with Kestrel's boot scripts and
# /etc/inittab (ports/sysvinit/root). The Makefile lays this tree into the
# root file system; the kernel runs /sbin/init when it is there.
#
# SYSVINIT_SRC  an unpacked SysVinit release (Ubuntu's sysvinit_3.08.orig.tar.gz)
# BUILD         Kestrel's build directory
#
# Host needs: musl-gcc, make, and the Linux UAPI headers (linux-libc-dev).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$1" && pwd)
mkdir -p "$2"
BUILD=$(cd "$2" && pwd)
OUT=$BUILD/sysvinit
ROOT=$BUILD/sysvinit-root

# the same compiler wrapper and headers as BusyBox (ports/busybox/build.sh)
KH=$BUILD/kheaders
mkdir -p "$KH"
ARCH=$(gcc -print-multiarch 2>/dev/null || echo x86_64-linux-gnu)
for d in linux asm-generic mtd; do [ -e "$KH/$d" ] || ln -s "/usr/include/$d" "$KH/$d"; done
[ -e "$KH/asm" ] || ln -s "/usr/include/$ARCH/asm" "$KH/asm"
CC=$BUILD/musl-pie-gcc
printf '%s\n' '#!/bin/sh' \
    'for a in "$@"; do' \
    '    case "$a" in -c|-S|-E|-r|-Wl,-r|-Wl,--relocatable|-shared) exec musl-gcc "$@";; esac' \
    'done' \
    'exec musl-gcc -static-pie "$@"' > "$CC"
chmod +x "$CC"

rm -rf "$OUT"
mkdir -p "$OUT"
cp -a "$SRC/." "$OUT/"
cd "$OUT/src"
PROGS="init halt shutdown runlevel killall5 wall"
echo "  BUILD   sysvinit"
make -s CC="$CC" CFLAGS="-O2 -fPIE -isystem $KH" VERSION="$(sed -rn "1s/.*[[:blank:]]\((.*)\)[[:blank:]].*/\1/p" "$OUT/doc/Changelog")" $PROGS >"$BUILD/sysvinit.log" 2>&1 || {
    grep -E "error|Error" "$BUILD/sysvinit.log" | head -20; exit 1; }

rm -rf "$ROOT"
mkdir -p "$ROOT/sbin"
for p in $PROGS; do install -m 755 "$p" "$ROOT/sbin/$p"; strip "$ROOT/sbin/$p"; done
ln -s init "$ROOT/sbin/telinit"
ln -s halt "$ROOT/sbin/reboot"
ln -s halt "$ROOT/sbin/poweroff"
ln -s killall5 "$ROOT/sbin/pidof"
cp -a "$HERE/root/." "$ROOT/"
chmod 640 "$ROOT/etc/shadow"
mkdir -p "$ROOT/etc/inittab.d" "$ROOT/run"
touch "$ROOT/.stamp"
echo "sysvinit: $PROGS in $ROOT/sbin"
