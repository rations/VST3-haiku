#!/bin/sh
# build-from-source.sh — build the VST3-haiku SDK, the JACK VST3 host, and the
# example plug-ins, and install the host + example plug-ins. Build jack-port-haiku
# FIRST (vst3jackhost links -ljack).
set -e

HERE=$(cd "$(dirname "$0")" && pwd)

pkgman install -y cmake ninja gcc make pkgconfig
export PKG_CONFIG_PATH="/boot/system/non-packaged/lib/pkgconfig:/boot/system/lib/pkgconfig:$PKG_CONFIG_PATH"

cd "$HERE"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build

# Install the host and the example plug-in bundles into the non-packaged tree.
mkdir -p /boot/system/non-packaged/bin /boot/system/non-packaged/add-ons/vst3
HOST=$(find build -type f -name vst3jackhost | head -1)
[ -n "$HOST" ] && cp "$HOST" /boot/system/non-packaged/bin/vst3jackhost
find build -type d -name '*.vst3' -exec cp -r {} /boot/system/non-packaged/add-ons/vst3/ \;

echo ">> Installed vst3jackhost and example plug-ins under /boot/system/non-packaged."
echo ">> The SDK static libs remain in build/lib/Release for building JackDAW/plug-ins."
