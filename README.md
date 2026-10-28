# Black (macOS)
An unofficial native macOS port of the original Xbox version of *Black*.

[![macOS 13+](https://img.shields.io/badge/macOS-13%2B-000000?logo=apple&logoColor=white)](#system-requirements)
[![Intel tested](https://img.shields.io/badge/Intel-tested-brightgreen)](#system-requirements)
[![Apple Silicon untested](https://img.shields.io/badge/Apple%20Silicon-untested-orange)](#system-requirements)
![Vulkan via MoltenVK](https://img.shields.io/badge/Vulkan-MoltenVK-AC162C?logo=vulkan&logoColor=white)

The game's x86 code in `default.xbe` is statically recompiled ahead of time to C++, then built for the Mac together
with the SDK's portable runtime and this repo's host code. Drawing goes through Vulkan 1.1 (MoltenVK on Metal), and windowing, input and audio
through SDL3. No emulator is involved.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the
> game to install or build.

## Installation
1. Build `Black.app` (see [Building](#building)), then move it anywhere, `/Applications` included.
2. Put the game in `~/Library/Application Support/Black`: the USA or Europe release, either as a disc image (`.iso`) or
   as `default.xbe` with its data folders. The app makes that folder on its first run and starts the game as soon as it
   finds the files.
3. Settings (`black.toml`), saves (`hdd`), logs and the shader cache are kept
   in the same folder. A disc image is read in place, never unpacked.

Keep the game out of `Downloads`, `Desktop` and `Documents`: macOS asks permission the first time an app reads those, and
the prompt blocks the game while it waits. `~/Library/Application Support/Black` needs no permission.

## Features
The settings are in the game's own options menu, each page under its own button: **Graphics**, **Controller**,
**General** and **Mouse and Keyboard**. Within a page the settings are grouped under headings, and arrows above
and below the list show there is more to scroll to.

### Graphics
- **Display**
  - **Render Resolution**: 20% to 200% of the window's height (100% by default), instead of the original 640x480.
  - **Field of View**: horizontal FOV from 45 to 140 degrees during gameplay. The game's default is 70.
  - **Viewmodel Field of View**: FOV of the first-person weapon and arms.
  - **FMV Scale**: *Fill* zooms the 4:3 videos to fill the window; *4:3* keeps their original aspect ratio.
  - **Screen Filter**: none, the CRT scanlines the console version shows, or colour filters.
- **Performance**
  - **V-Sync**, on by default.
  - **Max Frame Rate**: the display's refresh rate (default), a fixed value, or unlimited. This replaces the Xbox's
    30 FPS limit.
  - **FPS Counter**, **Dynamic Resolution** with its target frame rate, minimum and maximum resolution and hold time.
- **Quality**
  - **Anisotropic Filtering**, applied to all textures.
  - **Draw Distance**: 2x, 4x or 8x the game's level of detail and draw distances.
  - **Anti-Aliasing**, **Bloom**, **Fog**, **HDR**, **Reload Blur**.

### Controller
Any controller SDL3 supports, with rumble: Xbox, PlayStation and Switch pads, over USB or Bluetooth. Each stick has a
radial dead zone from 0% to 50% of its travel and a sensitivity from 0.1 to 3.0. **Gyroscope** aiming turns the view by
turning a DualShock 4 or DualSense, always or only while firing (macOS 11 or newer for a controller's sensors).

### Mouse and Keyboard
Rebindable controls, each with a second key, mouse sensitivity, invert look, hold-to-aim and walk speed. In play the
mouse is captured as on a PC: no pointer on the screen, and the view keeps turning past the window's edge. Escape opens
the pause menu, Backspace backs out of the game's menus.

### General
**Language**: English, French, German, Italian or Spanish where the disc has them, plus Indonesian for the port's own
text. **Skip Intros**, **Audio Output** (automatic, mono, stereo or 5.1).

### Window
**F11** toggles fullscreen, the window's size is remembered.

## System Requirements
- **macOS** 13 or newer.
- **Mac with an Intel CPU**: this is the configuration the port is built and tested on.
- **GPU** with Metal, and MoltenVK 1.2 or newer (`brew install molten-vk`).
- **Memory**: 4 GB.
- **Storage**: 2 GB for the game files and saves.

> [!WARNING]
> **Apple Silicon (M1/M2/M3/M4) is not tested.** The port has only been run on an Intel Mac, and the steps and paths
> below describe that setup. The sources are the same and nothing in them is x86-only — the recompiled code is plain C++
> and Vulkan goes through MoltenVK on Metal either way — so an arm64 build should compile and may well run, but nobody
> has verified it. Apple Silicon users should expect to debug it themselves; the log is the place to start, and the
> build output lives in `out/build/mac`. Homebrew installs to `/opt/homebrew` there, which `CMakeLists.txt` already
> searches.

### Tested on
MacBook Pro (2019), Intel Core i7-9750H, AMD Radeon Pro 5300M, macOS 15.7: 57 FPS at 1280x720 with the default
settings.

## Building

### 1. Clone the repository
```
git clone https://github.com/hshinosa/Black-macOS.git
```

### 2. Add the game files
Put `default.xbe` from the USA release in `assets/`, or put the disc image (`.iso`) in `assets/` or `iso/`. The build
pulls `default.xbe` out of the image.

### 3. Install the dependencies
- CMake and Ninja: `pip3 install --user cmake ninja`
- [MoltenVK](https://github.com/KhronosGroup/MoltenVK): `brew install molten-vk`
- Python 3, needed only to extract `default.xbe` from a disc image.

### 4. Build
```
./build.sh          # or ./build.sh --run, which also opens the app
```
It builds the recompiler (`tools/CMakeLists.txt` → `out/build/mac-tools/xbe_recomp`), runs it over the xbe
(`generated/game`), then builds `out/build/mac/Black.app` from the root `CMakeLists.txt`. The first build compiles the
whole game and takes 5 to 20 minutes.

After editing host code, `cmake --build out/build/mac -j8` is enough. After editing `config/game.toml` (hooks, symbols,
ABI, partitions) or the xbe, run `./build.sh` again: the recompile step is what reads it.

## Layout
- `generated/game/` is the recompiler's output (`x86_recomp.N.cpp`, `x86_func_mapping.cpp`, `image_symbols.h`). Never edit
  it; change `config/game.toml` or the host code and recompile.
- `src/` is the game's own host: `src/game/` hooks and patches, `src/d3d/` the title's Direct3D 8 entry points and the
  render thread, `src/sound/` DirectSound, `src/common/` the game description, options, menu and translations.
- `macos/` is the platform host: `main.cpp` (window, event loop, settings, folders), `game_files.cpp` (the disc image or
  an extracted install), `vulkan.cpp` (the Vulkan renderer), `input.cpp` and `gamepads.cpp` (keyboard, mouse, pads),
  `audio.cpp`, `ui.cpp` (ImGui), `dynamic_resolution.cpp`, `host.h`.
- `tools/` is the recompiler build and `xdvdfs.py`, which reads a file out of a disc image.

*Black* is a trademark of its respective owners. This project is not affiliated with or endorsed by them.
