# Roadmap

Ideas adopted from two other ReXGlue ports, in the order we're doing them:

- [Skullburglar](https://github.com/nlwstein/skullburglar), a port of *50 Cent: Blood on the Sand* (also Unreal Engine 3). BSD 3-Clause licence.
- [Table Tennis PC](https://github.com/davidptrssvea/Table-Tennis-Rockstar-PC), a port of *Rockstar Games presents Table Tennis*. MIT licence.

Code taken or adapted from either keeps their copyright notice in the file header, and their licence texts are in `licenses/`.

## Done (needs testing)

| # | What | From | Where |
|---|---|---|---|
| 1 | Guest vsync off plus a host frame limiter, and a tearing option. With guest vsync on, the graphics plugin sleeps on every GPU wait (Table Tennis measured 30-48 FPS instead of 60). | Table Tennis | `src/fps_unlock.cpp`, `src/frame_pacing.cpp`, launcher |
| 2 | FPS counter as a separate native window instead of an ImGui overlay. ImGui overlays make the SDK repaint every display refresh (Skullburglar measured about 20% of the frame rate). | Both | `src/fps_overlay.cpp` |
| 5 | "Open logs" and "Save crash report" in the launcher. `tbc.exe` writes `logs\crash-<pid>.txt` and `.dmp` on a crash. | Both | `src/crash_win.cpp`, launcher |
| 6 | Leak scan and git hooks that block commits of game files, generated code, build outputs and personal paths. | Skullburglar | `tools/leak_scan.py`, `.githooks/` |

Turn the hooks on once per clone: `git config core.hooksPath .githooks`

## Next: build the ReXGlue SDK ourselves (item 11)

We use the prebuilt SDK v0.10.1. Building it from source lets us apply fixes that only exist as SDK patches. Skullburglar's patches are written for v0.10.0, but this subset applies cleanly to v0.10.1, in this order (checked with `git apply`):

| Patch | What it does for us |
|---|---|
| 0002 debug-audio-dump-fps-log | Prerequisite for 0029 (env-gated debug output, off by default) |
| 0004 framerate-limit-and-vk-warning-dedupe | `framerate_limit` setting inside the SDK |
| 0006 perf-micro-and-mac-fullscreen | Register-write bookkeeping was 3-4% of the GPU thread |
| 0008 async-pipelines-background-precompile | Background shader compilation, no compile stutter |
| 0009 diagnostics-present-stats-vmem-ring | Present statistics; prerequisite for 0030 |
| 0026 texture-copyswap-16in32-count | Memory bug fix in texture conversion |
| 0029 timer-queue-precise-sleep | A timer spun about a quarter of a CPU core for nothing |
| 0030 imgui-passive-overlays-skip-continuous-repaint | Overlays no longer force continuous repaints |

v0.10.1 already has Skullburglar's 0001 (sync fences). Table Tennis's SDK patch adds FSR and CAS sharpening filters and button-prompt texture hooks; it's written for v0.10.0 and hasn't been checked against v0.10.1.

Build steps (Windows, x64 Native Tools prompt; needs LLVM/Clang, CMake and Ninja):

```
git clone --recursive --branch v0.10.1 https://github.com/rexglue/rexglue-sdk C:\rexglue-src
cd /d C:\rexglue-src
git apply <path to skullburglar>\patches\rexglue-sdk\0002-...patch   (then 0004, 0006, 0008, 0009, 0026, 0029, 0030)
cmake --preset win-amd64
cmake --build out\build\win-amd64 --config RelWithDebInfo --target install --parallel
cmake --build out\build\win-amd64 --config Release --target install --parallel
```

The SDK installs to `C:\rexglue-src\out\install\win-amd64`. Point this project at it with `-DCMAKE_PREFIX_PATH=C:/rexglue-src/out/install/win-amd64` and rebuild.

For players, the installer would then download our patched SDK build (a zip on this repo's Releases page, checked against a fixed SHA-256, as the Table Tennis installer does) instead of the stock one.

## Later

- Skip intro logos. Renaming the THX and Sierra / High Moon movies left the game stuck on the first level's loading screen, and the Unreal Engine and PhysX logos aren't separate movie files. Needs a code-level skip instead (Bourne's UE3 build has no `-nostartupmovies` switch).
- Controller feel: stick acceleration off, linear turning, vibration toggle (needs Bourne's input code found).
- PlayStation and keyboard button prompts (texture replacement; Table Tennis's SDK patch).
- Proper keyboard and mouse (Skullburglar routes mouse look through UE3's PC input; may be compiled out of Bourne's 360 build).
- Ultrawide.
- A native Direct3D renderer like Skullburglar's pre-1.0 one, which replaces calls into the Xbox 360 Direct3D library (Microsoft code built into every 360 game). It ran 275 FPS where the emulated renderer ran 108 on an RTX 3090. Their `tools/d3d_scan.py` finds those functions.
- A UE3 scene renderer like Skullburglar's `n1`: fastest, but tied most closely to each game's engine build.
