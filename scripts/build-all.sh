#!/bin/sh
# Cross-builds hdpvr-capture for Windows and Linux with Zig's bundled clang.
# libusb is vendored, so nothing else is needed.
#   ZIG=/path/to/zig scripts/build-all.sh [target ...]
set -e
cd "$(dirname "$0")/.."
ZIG=${ZIG:-zig}
OUT=${OUT:-dist}
CACHE=${ZIG_CACHE:-$PWD/build/zig-cache}
export ZIG_GLOBAL_CACHE_DIR=$CACHE ZIG_LOCAL_CACHE_DIR=$CACHE

TARGETS=${*:-"x86_64-windows-gnu x86-windows-gnu aarch64-windows-gnu \
x86_64-linux-gnu.2.31 x86-linux-gnu.2.31 aarch64-linux-gnu.2.31 arm-linux-gnueabihf.2.31 \
x86_64-linux-musl aarch64-linux-musl"}

V=vendor/libusb
COMMON="-O2 -Wall -Wextra -std=gnu11 -Isrc -I$V"

for t in $TARGETS; do
  name=$(echo "$t" | sed 's/\.2\.31$//')
  mkdir -p "$OUT/$name"
  case $t in
  *windows*)
    ext=.exe
    LU="$V/core.c $V/descriptor.c $V/hotplug.c $V/io.c $V/strerror.c $V/sync.c $V/os/events_windows.c \
$V/os/threads_windows.c $V/os/windows_common.c $V/os/windows_usbdk.c $V/os/windows_winusb.c"
    FL="-I$V/config/windows"
    LIBS="-ladvapi32 -lsetupapi -lole32"
    ;;
  *linux*)
    ext=
    LU="$V/core.c $V/descriptor.c $V/hotplug.c $V/io.c $V/strerror.c $V/sync.c $V/os/events_posix.c \
$V/os/threads_posix.c $V/os/linux_usbfs.c $V/os/linux_netlink.c"
    FL="-I$V/config/linux"
    LIBS="-lpthread"
    ;;
  *) echo "unsupported target $t" >&2; exit 1 ;;
  esac
  echo "== $name"
  $ZIG cc -target $t $COMMON $FL $LU src/hdpvr.c src/capture.c $LIBS -o "$OUT/$name/hdpvr-capture$ext"
  rm -f "$OUT/$name"/*.pdb
done
