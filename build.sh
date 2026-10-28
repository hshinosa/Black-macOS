#!/bin/bash
# Black for macOS: recompile the XBE, then build Black.app.  ./build.sh [--run]
#
# Two steps: the recompiler (tools/) turns assets/default.xbe into generated/game, then CMake builds Black.app from that,
# the SDK's portable runtime, the game's own sources in src and the host in macos. Drawing goes through Vulkan on Metal,
# by way of MoltenVK; window, input and sound through SDL3.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(pwd)

SDK=${SDK:-$( [ -d sdk ] && echo sdk || echo ../XboxXBE-Recomp )}
ISO=${ISO:-$HOME/Downloads/Black.iso}
DATA="$HOME/Library/Application Support/Black"
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}
export PATH="$HOME/Library/Python/3.13/bin:/usr/local/bin:/opt/homebrew/bin:$PATH"

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }

command -v cmake >/dev/null || { echo "cmake is missing: pip3 install --user cmake ninja"; exit 1; }
command -v ninja >/dev/null || { echo "ninja is missing: pip3 install --user cmake ninja"; exit 1; }
[ -f "$SDK/cmake/XbeRecomp.cmake" ] || { echo "the SDK is missing: clone TGP482/XboxXBE-Recomp to $SDK"; exit 1; }

# SDL3, cloned once into third_party/
if [ ! -f third_party/SDL/CMakeLists.txt ]; then
    say "cloning SDL3"
    git clone --depth 1 --branch release-3.4.16 https://github.com/libsdl-org/SDL.git third_party/SDL
fi

# the game: default.xbe from the disc image, which the build needs for the sha1 it checks at run time
if [ ! -f assets/default.xbe ]; then
    [ -f "$ISO" ] || { echo "no assets/default.xbe and no disc image at $ISO (set ISO=)"; exit 1; }
    say "default.xbe from $ISO"
    mkdir -p assets
    python3 -I tools/xdvdfs.py "$ISO" default.xbe assets/default.xbe
fi

# the recompiler, native, built once
if [ ! -x out/build/mac-tools/xbe_recomp ]; then
    say "building the recompiler"
    cmake -S tools -B out/build/mac-tools -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build out/build/mac-tools -j "$JOBS"
fi

say "recompiling default.xbe into generated/game"
mkdir -p generated/game
./out/build/mac-tools/xbe_recomp config/game.toml --xdk "$SDK/xdk" 2>&1 | grep -v "^unimplemented" || true

say "building Black.app"
cmake -S . -B out/build/mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/local >/dev/null
cmake --build out/build/mac -j "$JOBS"

# the game files: the disc image, a clone (instant, shares the blocks) or a link, in the folder the app reads
mkdir -p "$DATA"
if [ ! -e "$DATA/Black.iso" ] && [ ! -e "$DATA/default.xbe" ] && [ -f "$ISO" ]; then
    say "putting $ISO in $DATA"
    cp -c "$ISO" "$DATA/Black.iso" 2>/dev/null || cp "$ISO" "$DATA/Black.iso"
fi

say "done: out/build/mac/Black.app, game files in $DATA"
if [ "${1:-}" = "--run" ]; then
    open out/build/mac/Black.app
fi
exit 0