#!/bin/sh
# Cross-builds hdpvr-capture (and, for Windows, the hdpvr-driver installer) for
# Windows and Linux. libusb is vendored, so nothing else is needed.
#   ZIG=/path/to/zig scripts/build-all.sh [target ...]
# Targets are Zig triples; the default is the full list below.
#
# Windows 7 needs binaries linked against msvcrt.dll (Zig links the Universal
# CRT, which Windows 7 lacks). Point these at MinGW-w64 GCC/Clang compilers
# that use msvcrt (llvm-mingw's msvcrt release or Debian's gcc-mingw-w64) to
# use them instead:
#   WIN_CC_x86_64=x86_64-w64-mingw32-clang WIN_CC_x86=i686-w64-mingw32-clang
#   WIN_CC_aarch64=aarch64-w64-mingw32-clang  (optional, for Windows on Arm)
# LIBUSBK_DIR is the unpacked libusbK-3.1.0.0-bin folder (scripts/fetch-libusbk.sh).
# Its signed driver is only the fallback for a Windows without winusb.inf;
# without it the builds still work everywhere WinUSB does.
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
COMMON="-O2 -Wall -Wextra -Wno-missing-field-initializers -std=gnu11 -Isrc -I$V"
SRC="src/hdpvr.c src/app_core.c src/decoder.c src/sink.c"

for t in $TARGETS; do
  name=$(echo "$t" | sed 's/\.2\.31$//')
  mkdir -p "$OUT/$name"
  CC="$ZIG cc -target $t"
  case $t in
  *windows*)
    os=windows; ext=.exe
    LU="$V/core.c $V/descriptor.c $V/hotplug.c $V/io.c $V/strerror.c $V/sync.c $V/os/events_windows.c \
$V/os/threads_windows.c $V/os/windows_common.c $V/os/windows_usbdk.c $V/os/windows_winusb.c"
    FL="-I$V/config/windows -U_WIN32_WINNT -D_WIN32_WINNT=0x0601"
    OSSRC="src/audio_windows.c"
    LIBS="-ladvapi32 -lsetupapi -lole32 -loleaut32 -lwinmm -luuid"
    GUI="src/gui_win32.c"; GUILIBS="-lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -luser32 -ldwmapi -Wl,--subsystem,windows"
    case $t in
    x86_64-*)  wcc=$WIN_CC_x86_64;  karch=amd64; ff=x86_64-windows ;;
    x86-*)     wcc=$WIN_CC_x86;     karch=x86;   ff=x86-windows ;;
    aarch64-*) wcc=$WIN_CC_aarch64; karch=;      ff=aarch64-windows ;;
    esac
    # the icon (from Hauppauge Capture): Zig compiles .rc itself, MinGW needs windres
    RES=resources/windows/app.rc
    if [ -n "$wcc" ]; then
      CC="$wcc"; LIBS="$LIBS -static -lpthread"
      RES=build/app-$name.o
      mkdir -p build
      "$(echo "$wcc" | sed -E 's/-(clang|gcc).*$//')-windres" -I resources/windows -O coff \
          resources/windows/app.rc -o "$RES"
    elif [ -n "$karch" ]; then
      echo "warning: $name built with Zig links the Universal CRT and will not start on a stock Windows 7" >&2
    fi
    ;;
  *linux*)
    os=linux; ext=; RES=; GUI=
    LU="$V/core.c $V/descriptor.c $V/hotplug.c $V/io.c $V/strerror.c $V/sync.c $V/os/events_posix.c \
$V/os/threads_posix.c $V/os/linux_usbfs.c $V/os/linux_netlink.c"
    FL="-I$V/config/linux"
    OSSRC="src/audio_alsa.c"
    LIBS="-lpthread -lm -ldl"
    GUILIBS=
    # one static musl ffmpeg per CPU serves the glibc and musl builds alike
    case $t in x86_64-*) ff=x86_64-linux ;; x86-*) ff=x86-linux ;; aarch64-*) ff=aarch64-linux ;; arm-*) ff=arm-linux ;; esac
    ;;
  *) echo "unsupported target $t" >&2; exit 1 ;;
  esac
  echo "== $name ($CC)"
  $CC $COMMON $FL $LU src/hdpvr.c src/capture.c $RES $LIBS -o "$OUT/$name/hdpvr-capture$ext"
  if [ -n "$GUI" ] && [ -f "$GUI" ]; then
    $CC $COMMON $FL $LU $SRC $OSSRC $GUI $RES $GUILIBS $LIBS -o "$OUT/$name/HauppaugeRecorder$ext"
  fi
  if [ $os = windows ]; then
    # Zadig-style driver installer, plus the libusbK fallback driver
    $CC $COMMON $FL src/driver_win32.c $RES -lsetupapi -lcfgmgr32 -lcrypt32 -lwintrust -lshell32 -ladvapi32 -lole32 \
        $( [ -n "$wcc" ] && echo -static ) -o "$OUT/$name/hdpvr-driver.exe"
    if [ -n "$karch" ]; then
      if [ -n "$LIBUSBK_DIR" ]; then
        mkdir -p "$OUT/$name/driver/libusbK" "$OUT/$name/licenses"
        cp "$LIBUSBK_DIR/bin/sys/$karch/libusbK.sys" "$LIBUSBK_DIR/bin/sys/$karch/WdfCoInstaller01009.dll" \
           "$OUT/$name/driver/libusbK/"
        cp "$LIBUSBK_DIR/bin/dll/$karch/libusbK.dll" "$OUT/$name/"
        cp resources/windows/libusbK-LICENSE-bsd.txt "$OUT/$name/licenses/libusbK-LICENSE.txt"
      else
        echo "note: LIBUSBK_DIR not set; $name uses WinUSB only" >&2
      fi
    fi
  fi
  if [ "${BUNDLE_FFMPEG:-1}" = 1 ] && [ -n "$GUI" ]; then
    ffbin=build/ffmpeg-build/out/$ff/ffmpeg$ext
    [ -f "$ffbin" ] || scripts/build-ffmpeg.sh "$ff" "$ffbin"
    cp "$ffbin" "$OUT/$name/ffmpeg$ext"
    mkdir -p "$OUT/$name/licenses"
    cp resources/licenses/* "$OUT/$name/licenses/"
  fi
  rm -f "$OUT/$name"/*.pdb
done
