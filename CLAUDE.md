# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An unofficial macOS port of the original Xbox game *Black*, made with the
[XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) SDK. The game's x86 code in `default.xbe` is statically
recompiled to C++ ahead of time. That C++ is then built for the Mac together with the SDK's portable runtime and this
repo's host code. Drawing goes through Vulkan 1.1 (MoltenVK on Metal), and windowing, input and audio through SDL3. The
repo ships no game assets: `assets/`, `iso/` and `generated/` hold the user's own files or files derived from them, and
all three are gitignored.

## Build

Everything runs on the Mac. Requirements: CMake and Ninja (`pip3 install --user cmake ninja`), MoltenVK
(`brew install molten-vk`) and Python 3, the last only to pull the xbe out of an ISO.

The build is developed and tested on an **Intel** Mac (i7-9750H, Radeon Pro 5300M, macOS 15.7). Apple Silicon is
untested: the sources are the same and the recompiled code is plain C++, so an arm64 build should work, but nobody has
verified it, and CI keeps to an Intel runner (`macos-13`) for that reason.

- `./build.sh` runs the whole pipeline:
  1. Clones SDL `release-3.4.16` into `third_party/SDL` (shallow).
  2. Gets `assets/default.xbe`. If it is missing, `python3 tools/xdvdfs.py <iso> default.xbe assets/default.xbe`
     extracts it from `assets/*.iso` or `iso/*.iso`.
  3. Builds the recompiler (`tools/CMakeLists.txt` → `out/build/mac-tools/xbe_recomp`).
  4. Runs `xbe_recomp config/game.toml --xdk <sdk>/xdk` → `generated/game/`.
  5. Builds `out/build/mac/Black.app` (`cmake -S . -B out/build/mac`, then Ninja).
  6. Puts the disc image in `~/Library/Application Support/Black` (a clone, which costs no space) if the folder has no
     game yet.
- `./build.sh --run` also opens the app; `cmake --build out/build/mac -j8` rebuilds the app alone after host changes.
- After editing `config/game.toml` (hooks, symbols, ABI, partitions), the recompile step has to run again: `./build.sh`.
- The first build compiles 64 partitions of recompiled code and takes 5 to 20 minutes.
- `-DXBE_GENERATED=<dir> -DXBE_IMAGE=<file>` builds recompiled code from somewhere other than `generated/game`.

There are no tests or linters in this repo.

## CI/CD

There is none. The app is built on a Mac with `./build.sh`; nothing is published for download, so no workflow needs the
game's xbe or a release key.

## Where the SDK comes from

`CMakeLists.txt` looks for the SDK in this order: `sdk/` in the repo, then `../XboxXBE-Recomp` beside the repo. If
neither exists, it FetchContent's the SDK at a pinned commit, the one this port was made with. A sibling checkout lets
you read or change the runtime (`<sdk>/runtime/{app,core,kernel,xdk,gpu,audio}`), whose headers (`app/app.h`,
`kernel/kernel.h`, `xdk/hle.h`, `core/input.h`, ...) the host code includes.

## Architecture

The root `CMakeLists.txt` builds one macOS application, `Black.app`, made from:

- **`generated/game/`**: recompiler output (`x86_recomp.N.cpp` for N < `partitions`, `x86_func_mapping.cpp`,
  `image_symbols.h`). Compiled with `-O2 -w`. Never edit it by hand. Change `config/game.toml` or the host code and
  recompile instead.
- **The SDK runtime** (`<sdk>/runtime/*/*.cpp`), without the Windows-only parts and the parts this repo replaces. The
  regex filter in `CMakeLists.txt` drops `app/`, `core/input.cpp`, `gpu/d3d12`, `gpu/render_thread`, `audio/xaudio2`,
  `xdk/dsound`, `xdk/d3d8` and `xdk/xnet`.
- **`src/`**, the game-specific host, organized as `src/<area>/*.cpp` and globbed by CMake:
  - `src/game/`: game hooks and patches (FOV, draw distance, intro skip, menus, HUD, mouse, prompts).
  - `src/d3d/`: the title's Direct3D 8 (XDK 5849) entry points. `render_thread.cpp` records D3D calls on the game's
    render thread and replays them on a dedicated Vulkan thread. Vertex, texture and surface memory is read at draw
    time, the way the console GPU reads it, so XDK fences, busy checks and locks wait on that replay.
  - `src/sound/`: DirectSound reimplemented (buffers, streams, ADPCM decode, mixer, reverb).
  - `src/common/`: `game_info.cpp` defines `app::kGame`, the struct that describes the game to the runtime (title,
    settings file, option bindings, load/save/init, pad and rumble filters). Also here: the settings rows the game's
    own options menu shows (`menu.cpp`), `options.h` (`game::Options`, saved to `black.toml`), translations, and
    `europe.patch`.
- **`macos/`**, the platform host:
  - `main.cpp` runs SDL's window and event loop and implements the `app::` and `host::` interfaces the runtime and the
    game use, plus the settings file and the folders. `ExeDir()` is `~/Library/Application Support/Black`.
  - `vulkan.cpp` is the Vulkan backend and `vk_shader.cpp` compiles the SDK's HLSL translations to SPIR-V with glslang.
    Both take their surface from SDL, which makes the Metal view, so instance and surface creation hop to the main
    thread through `host::OnMainThread` — AppKit allows nothing else.
  - `game_files.cpp` finds the game: a disc image first, then an extracted install with `GlobData.bin`, then a lone
    `default.xbe`; it checks the xbe's SHA1 against `kImageSha1` and turns a Europe xbe into the US one with
    `src/common/europe.patch` (CMake writes the patch out as a C array, since a Mach-O section cannot `incbin` it).
  - `input.cpp` is the keyboard and mouse: bindings, the menu cursor, the mouse look, the name entry. `gamepads.cpp` is
    SDL controllers. `audio.cpp`, `dynamic_resolution.cpp`, `game_host.cpp`, `host.h`.
  - `compat/crt.h` is force-included into all host sources: the `memfd_create` and `pthread_setname_np` shims macOS
    needs. `compat/x86intrin.h` forwards to the real header on x86.
- **`tools/`** builds the recompiler (`xbe_recomp`) and carries `xdvdfs.py`.

### How recompiled code reaches the host

`config/game.toml` is the recompiler's input:
- `[xbe]`: path, entry point, XDK version, output dir, `partitions` (CMake reads this number to list the generated
  sources), and code sections.
- `[symbols]` / `[data]`: addresses of library functions and globals in the image, 0 where the image has none. Host code
  refers to them through `SYM(Name)` from `image_symbols.h`.
- `[abi]`: register-argument conventions for link-time-code-generated XDK functions.
- `[[hook]]`: entries `at = <x86 address>`, `call = "<Name>"`. The recompiler emits a call at that address to a host
  function `void Name(X86Context& ctx, uint8_t* base)`, mostly defined in `src/game/hooks.cpp`. To add a hook, add the
  `[[hook]]` entry, define the function, and recompile.

Host code reads and writes guest memory through the kernel helpers (`HostPtr(va)`, `R32`, ...) and guest registers
through `X86Context`.

### Supported game versions

The build is recompiled from the **USA** `default.xbe`. At runtime a Europe xbe is turned into the US one with
`src/common/europe.patch`. Code that hardcodes addresses assumes the US image.

## Conventions

- Native code is C++20 and is compiled with `-fsigned-char -fno-strict-aliasing`, matching x86 semantics.
- The app is a normal bundle with no entitlements; it reads only its own data folder, so no macOS privacy prompt stands
  between the game and its files.
- Comment style is terse, lowercase, and explains *why*, as in the existing files.