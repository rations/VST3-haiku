#!/bin/sh
# make-hpkg.sh — build the `vst3_haiku` .hpkg on a running Haiku (x86_64):
# the vst3jackhost binary plus the SDK example plug-ins. Requires jack (libjack)
# to be installed first (vst3jackhost links -ljack).
#
# NAMku/DRUMku are packaged separately, so this build excludes them.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
VERSION=0.3.0
REVISION=1
STAGE="$HERE/stage"

pkgman install -y cmake ninja gcc make pkgconfig || true
export PKG_CONFIG_PATH="/boot/system/non-packaged/lib/pkgconfig:/boot/system/lib/pkgconfig:$PKG_CONFIG_PATH"

cd "$ROOT"
rm -rf build "$STAGE"
# NAMKU_DIR pointed away so the sibling NAMku is not pulled into this package.
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNAMKU_DIR=/nonexistent
ninja -C build

mkdir -p "$STAGE/bin" "$STAGE/add-ons/media/VST3"

HOST=$(find build -type f -name vst3jackhost | head -1)
[ -n "$HOST" ] || { echo "!! vst3jackhost not built (jack not found?)" >&2; exit 1; }
cp "$HOST" "$STAGE/bin/vst3jackhost"

# Copy every built .vst3 bundle except the ones with their own packages.
find build -type d -name '*.vst3' | while read -r b; do
	case "$(basename "$b")" in
		NAMku.vst3|DRUMku.vst3) continue ;;
	esac
	cp -r "$b" "$STAGE/add-ons/media/VST3/"
done

cp "$HERE/vst3_haiku.PackageInfo" "$STAGE/.PackageInfo"
OUT="$HERE/vst3_haiku-$VERSION-$REVISION-x86_64.hpkg"
package create -C "$STAGE" "$OUT"
echo ">> built $OUT"
