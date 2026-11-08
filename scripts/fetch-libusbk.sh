#!/bin/sh
# Downloads the signed libusbK 3.1.0.0 driver (BSD licence) used for Windows 7/8.0
# and unpacks it to $1 (default build/libusbK). Prints the unpacked folder.
set -e
DEST=${1:-build/libusbK}
URL=https://github.com/mcuee/libusbk/releases/download/V3.1.0.0/libusbK-3.1.0.0-bin.7z
SHA256=38605d8d5a86f408a4b7bec60f6d4a096050eee72f89a63a8d5be125252d3fe7
mkdir -p "$DEST"
curl -fsSL -o "$DEST/libusbK-bin.7z" "$URL"
echo "$SHA256  $DEST/libusbK-bin.7z" | (sha256sum -c - 2>/dev/null || shasum -a 256 -c -) >&2
7z x -y -o"$DEST" "$DEST/libusbK-bin.7z" >/dev/null
echo "$DEST/libusbK-3.1.0.0-bin"
