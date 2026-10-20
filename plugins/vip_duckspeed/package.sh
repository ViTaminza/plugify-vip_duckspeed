#!/bin/bash
# Assembles build/package/vip_duckspeed: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/vip_duckspeed"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/vip_duckspeed/libvip_duckspeed.so" "$OUT/bin/"
cp "$ROOT/build/plugins/vip_duckspeed/vip_duckspeed.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
echo "package: $OUT"
