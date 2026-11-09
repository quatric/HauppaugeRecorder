#!/bin/sh
# Builds the small, static, LGPL ffmpeg that is bundled with HauppaugeRecorder.
# It holds only what the preview uses (src/app_core.c): the HD PVR's MPEG
# transport stream in through a pipe, H.264, AAC and AC-3 decoding, yadif and
# scale, and raw video frames or 16-bit PCM out through a pipe.
#
#   scripts/build-ffmpeg.sh TARGET OUTFILE
#
# TARGET is one of
#   macos-x86_64 macos-arm64                    (Xcode clang; lipo them together)
#   x86_64-windows x86-windows aarch64-windows  ($WIN_PREFIX_<arch>-clang/gcc, e.g. llvm-mingw msvcrt)
#   x86_64-linux x86-linux aarch64-linux arm-linux   (zig cc, static musl)
# nasm is needed for the x86 targets.
set -e
cd "$(dirname "$0")/.."
TARGET=$1
OUTFILE=$2
[ -n "$TARGET" ] && [ -n "$OUTFILE" ] || { echo "usage: $0 TARGET OUTFILE" >&2; exit 1; }
case $OUTFILE in /*) ;; *) OUTFILE=$PWD/$OUTFILE ;; esac

VER=7.1.5
SHA256=de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f
TOP=$PWD/build/ffmpeg-build
SRC=$TOP/ffmpeg-$VER
BLD=$TOP/build-$TARGET
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
ZIG=${ZIG:-zig}

mkdir -p "$TOP"
if [ ! -f "$SRC/configure" ]; then
  curl -fsSL -o "$TOP/ffmpeg-$VER.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
  echo "$SHA256  $TOP/ffmpeg-$VER.tar.xz" | (sha256sum -c - 2>/dev/null || shasum -a 256 -c -) >&2
  tar -C "$TOP" -xJf "$TOP/ffmpeg-$VER.tar.xz"
fi

FEATURES="--disable-everything --disable-autodetect --disable-doc --disable-debug --disable-network
  --disable-ffplay --disable-ffprobe --disable-avdevice --enable-static --disable-shared
  --enable-swscale --enable-swresample
  --enable-protocol=file,pipe
  --enable-demuxer=mpegts
  --enable-parser=h264,aac,ac3
  --enable-decoder=h264,aac,ac3
  --enable-encoder=rawvideo,pcm_s16le
  --enable-muxer=rawvideo,pcm_s16le
  --enable-filter=yadif,scale,fps,format,null,anull,aformat,aresample"

case $TARGET in
macos-*)
  arch=${TARGET#macos-}
  min=10.9; [ "$arch" = arm64 ] && min=11.0
  CROSS="--target-os=darwin --arch=$arch --enable-cross-compile
    --cc=clang --extra-cflags=-arch\ $arch\ -mmacosx-version-min=$min
    --extra-ldflags=-arch\ $arch\ -mmacosx-version-min=$min"
  ;;
*-windows)
  arch=${TARGET%-windows}
  case $arch in x86) tp=i686 ;; *) tp=$arch ;; esac
  eval prefix=\${WIN_PREFIX_$arch:-$tp-w64-mingw32}
  cc=gcc; command -v "$prefix-clang" >/dev/null 2>&1 && cc=clang
  CROSS="--target-os=mingw32 --arch=$arch --enable-cross-compile --cross-prefix=$prefix-
    --cc=$prefix-$cc --extra-cflags=-D_WIN32_WINNT=0x0601 --extra-ldflags=-static"
  ;;
*-linux)
  arch=${TARGET%-linux}
  case $arch in arm) zt=arm-linux-musleabihf; fa=arm ;; x86) zt=x86-linux-musl; fa=x86 ;; *) zt=$arch-linux-musl; fa=$arch ;; esac
  mkdir -p "$BLD"
  # configure wants single-word tools
  for t in cc ar ranlib; do
    printf '#!/bin/sh\nexec %s %s %s "$@"\n' "$ZIG" "$t" "$( [ $t = cc ] && echo "-target $zt")" > "$BLD/zig-$t"
    chmod +x "$BLD/zig-$t"
  done
  CROSS="--target-os=linux --arch=$fa --enable-cross-compile --cc=$BLD/zig-cc --ar=$BLD/zig-ar
    --ranlib=$BLD/zig-ranlib --nm=${NM:-llvm-nm} --strip=${STRIP:-llvm-strip} --extra-ldflags=-static"
  # baseline armhf like the rest of the build (ARMv6 boards too), so no NEON
  [ $arch = arm ] && CROSS="$CROSS --disable-neon"
  ;;
*) echo "unknown target $TARGET" >&2; exit 1 ;;
esac

rm -rf "$BLD/obj"; mkdir -p "$BLD/obj"
cd "$BLD/obj"
# shellcheck disable=SC2086
ARGS=$(echo $FEATURES $CROSS)   # one line; "\ " keeps flags with spaces together
eval "\"$SRC/configure\" $ARGS" > configure.log 2>&1 || { tail -40 configure.log; tail -60 ffbuild/config.log; exit 1; }
EXT=; case $TARGET in *-windows) EXT=.exe ;; esac
make -j"$JOBS" "ffmpeg$EXT" > make.log 2>&1 || { tail -40 make.log; exit 1; }
mkdir -p "$(dirname "$OUTFILE")"
cp "ffmpeg$EXT" "$OUTFILE"
case $TARGET in macos-*)
  # same SDK quirk as the app (see Makefile): CoreMedia recorded as @rpath/libswiftCoreMedia.dylib
  if otool -L "$OUTFILE" | grep -q '@rpath/libswiftCoreMedia.dylib'; then
    install_name_tool -change @rpath/libswiftCoreMedia.dylib \
      /System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia "$OUTFILE" 2>/dev/null
  fi
  codesign --force --sign - "$OUTFILE" ;;
esac
echo "built $OUTFILE"
