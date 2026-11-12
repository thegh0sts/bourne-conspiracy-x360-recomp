# The Bourne Conspiracy - Xbox 360 Recompilation

A native Windows port of *Robert Ludlum's The Bourne Conspiracy* (Xbox 360, title ID `565507D4`), built by statically recompiling the original game code to C++ with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). It is not an emulator: the game code is translated ahead of time and compiled into a normal Windows executable.

**This repository contains no game files.** You need your own legally obtained copy of the game.

## Status

Early, but playable: boots through the intro videos and main menu into gameplay, tested through the first levels.

## Requirements

- Windows 10/11, x64
- Visual Studio 2026 (or 2022) with **Desktop development with C++** and **C++ Clang tools for Windows**
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk/releases) **v0.10.1**, extracted to e.g. `C:\rexglue-sdk`
- Your own Xbox 360 disc image of the game

## Setup

1. Extract the game from your ISO with [extract-xiso](https://github.com/XboxDev/extract-xiso):
   ```
   extract-xiso -s -d "<path>\tbc-game" "<path>\game.iso"
   ```
2. Place the extracted folder so it matches the paths in `tbc_manifest.toml` (by default `../bourne-conspiracy-x360-port/tbc-game` relative to this repo), or edit `game_root` and `file_path` in the manifest to point at it.

## Build

From an **x64 Native Tools Command Prompt for VS**, in this folder:

```
cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH=C:/rexglue-sdk
cmake --build out\build\win-amd64-relwithdebinfo
```

The first build runs ReXGlue code generation (about 2 minutes) and then compiles the generated code, which takes a while.

## Run

Double-click `run_tbc.bat`, or:

```
out\build\win-amd64-relwithdebinfo\tbc.exe --game_data_root "<path>\tbc-game" --gpu_plugin xenos --log_level debug --log_file tbc_log.txt
```

## How the port works

ReXGlue's analyzer misses some functions, mostly small virtual-function stubs that are only ever called through pointers. These are added by hand:

- `default_functions.toml` - function starts found from codegen errors and runtime crashes.
- `default_functions_scan.toml` - function starts found by scanning the decrypted executable: addresses stored in the game's data (virtual function tables) and code left uncovered between known functions. Every entry is checked to follow a return or jump instruction (or alignment padding), so it can't split an existing function.

If the game crashes with `Call to invalid or unregistered function at guest address 0x...`, that address is another missed function: add it to `default_functions.toml` and rebuild.

## Performance and 60 FPS

The original game runs at 30 FPS: it asks Direct3D to wait two screen refreshes per frame. `src/fps_unlock.cpp` can change that, set with the launcher's **Frame rate** option or `--frame_rate=60` (or `--frame_rate=0` for no limit; the launcher then also raises the engine's own cap, `MaxSmoothedFrameRate`).

The launcher also turns off the graphics plugin's imitation of the Xbox 360 vblank wait (`--vsync=false`), which slept on every GPU wait and cost a lot of frame rate, and lets `tbc.exe`'s own frame limiter (`src/frame_pacing.cpp`) hold the chosen rate instead. This approach comes from the Table Tennis PC port. Together with an FPS counter that no longer forces extra repaints, it took the busiest scene from about 48 to the mid-50s FPS at 1x.

**60 FPS is experimental.** Keep these caveats in mind:

- The game was only ever tuned and tested at 30 FPS. Gameplay speed, physics, animation and cutscene timing at 60 FPS have only been lightly tested, through the first level. If anything looks too fast, too slow or broken, switch back to 30.
- Above 60 FPS (120, 144, unlimited) is untested beyond starting the game.
- Heavy scenes don't hold 60. The runtime's imitation of the Xbox 360 graphics hardware is expensive, so even a fast graphics card dips into the 40s in busy areas. Expect an uneven frame rate rather than a steady 60.
- Higher render resolutions still cost more than in PC games. With the **Graphics path** on Auto or ROV, 2560×1440 reaches the mid-40s in the busy street. 3840×2160 hasn't been measured with ROV yet.

### Tested on

| | |
|---|---|
| CPU | Intel Core i7-12700KF |
| RAM | 32 GB DDR4 |
| GPU | NVIDIA GeForce RTX 4080 SUPER (driver 621.01) |
| OS | Windows 11 |
| Build | ReXGlue SDK v0.10.1, RelWithDebInfo |

Frame rate at 60 FPS in the busy street at the start of the first level:

| Render resolution | FPS now | FPS before the pacing change | GPU load (before) |
|---|---|---|---|
| 1280×720 (1x) | mid-50s | about 48 (dips to about 40 as the level opens) | about 85% |
| 2560×1440 (2x) | mid-40s with ROV (mid-teens with RTV) | about 15-25 | close to 100% |
| 3840×2160 (3x) | not measured | under 10 | close to 100% |

"Before" was measured with the old pacing and the old FPS counter overlay; "now" with the new pacing, the native FPS counter and Original Xbox 360 frame pacing unticked. At 2x the graphics card is the limit, which pacing can't help.

**Why higher resolutions were slow.** The Xbox 360 draws into 10 MB of fast on-chip memory (EDRAM) and reuses pieces of it for different images within a frame. The runtime's default way of imitating it on NVIDIA and AMD (`rtv`) gives each image its own render target and copies pixels between them whenever the game reuses a piece. Bourne does that about 1,800 times a frame. A PIX capture of the first street put those copies at 12 ms of a 22.5 ms frame at 1x, and at 54 of 68 ms at 2x, where each copy has four times the pixels. The other path (`rov`) keeps EDRAM as one buffer and blends in the pixel shaders, so it needs no copies. At 2x that took the street from the mid-teens to the mid-40s FPS. The launcher's **Graphics path** setting picks it automatically above 720p (Auto). This replaces an earlier note here that found no difference between the two paths; the PIX capture and the 2x test above show otherwise.

For a smooth 60, 1280×720 with FXAA is still the safest choice.

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md) for what's planned, including ideas from the [Skullburglar](https://github.com/nlwstein/skullburglar) and [Table Tennis PC](https://github.com/davidptrssvea/Table-Tennis-Rockstar-PC) ports.

## Contributing

Turn on the leak-scan hooks once per clone, so game files, generated code and personal paths can't be committed:

```
git config core.hooksPath .githooks
```

## Known issues

- Function `0x825EADC8` is reported as 1.27 MB, which suggests several functions merged into one. Not yet investigated.
- 60 FPS and higher render resolutions are limited by graphics performance (see above). ROV makes higher resolutions much faster but hasn't been played through yet; if something looks wrong, set **Graphics path** to RTV.
- Keyboard and mouse is controller emulation: the mouse drives the right stick, so the camera can feel heavier than a PC game. Use a low stick dead zone and the camera turn speed option.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and the Xenia project, whose work this is built on.
- [Skullburglar](https://github.com/nlwstein/skullburglar) (BSD 3-Clause): crash reports and the leak scanner are adapted from its code, and its notes on SDK performance pointed at the FPS counter cost.
- [Table Tennis PC](https://github.com/davidptrssvea/Table-Tennis-Rockstar-PC) (MIT): the frame pacing approach (guest vsync off, host frame limiter, tearing).
- [WetRecomp](https://github.com/nikolaygorb/WetRecomp) by nikolaygorb (MIT): the GPU-wait hook, display refresh matching, the stub-sweep debug tool and the XMA audio decoder fixes (SDK patch 0003).
- [ReGPU](https://github.com/zer0ducksgiven/ReGPU) and [Glue2GPU](https://github.com/zer0ducksgiven/Glue2GPU) (MIT): the experimental Direct3D 12 renderer and its ReXGlue plugin (`regpu-plugin/`).
- Function recovery, installer and tooling developed with help from Claude (Anthropic).

## Disclaimer

Not affiliated with or endorsed by Microsoft, Activision, High Moon Studios or the Ludlum estate. All trademarks belong to their respective owners.
