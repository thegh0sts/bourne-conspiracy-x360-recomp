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

We use the prebuilt SDK v0.10.1. Building it from source gets us two things the prebuilt one doesn't have.

### FSR and CAS output filters

The SDK's v0.10.1 source already contains AMD FidelityFX support for the final picture: **FSR** (upscales the finished 720p image to the window or screen resolution, with sharpening) and **CAS** (sharpening only), plus experimental FSR 2 and 3 modes. It's off in the prebuilt download, which is why `--present_effect` only accepts `bilinear` there. Building with `-DREXGLUE_ENABLE_FIDELITYFX=ON` turns it on (CMake fetches AMD's FidelityFX SDK during the build; on Windows it uses the Direct3D 12 backend).

These filters only work on the finished image, so they cost a fraction of a millisecond, unlike a higher render resolution (2x render ran in the mid-teens in our busiest scene). Rendering at 1x with FSR to 1440p or 4K output is the practical way to a sharper picture at close to 60 FPS. This is what the Table Tennis PC port does (through its own SDK patch).

Once the SDK is built this way, the launcher gets an **Upscaling filter** option (Off / FSR / CAS) that passes `--present_effect fsr` or `cas`. The option has to wait for that SDK: the prebuilt one rejects any value but `bilinear`.

### Performance patches

Building from source also lets us apply fixes that only exist as SDK patches. Skullburglar's patches are written for v0.10.0, but this subset applies cleanly to v0.10.1, in this order (checked with `git apply`):

| Patch | What it does for us |
|---|---|
| 0002 debug-audio-dump-fps-log | Prerequisite for 0029 (env-gated debug output, off by default) |
| 0003 vulkan-hoist-shared-memory-uploads | A Vulkan change (unused on Windows), but it carries header declarations that 0006 and 0008 need |
| 0004 framerate-limit-and-vk-warning-dedupe | `framerate_limit` setting inside the SDK |
| 0006 perf-micro-and-mac-fullscreen | Register-write bookkeeping was 3-4% of the GPU thread |
| 0008 async-pipelines-background-precompile | Background shader compilation, no compile stutter |
| 0009 diagnostics-present-stats-vmem-ring | Present statistics; prerequisite for 0030 |
| 0026 texture-copyswap-16in32-count | Memory bug fix in texture conversion |
| 0029 timer-queue-precise-sleep | A timer spun about a quarter of a CPU core for nothing |
| 0030 imgui-passive-overlays-skip-continuous-repaint | Overlays no longer force continuous repaints |

v0.10.1 already has Skullburglar's 0001 (sync fences). Table Tennis's SDK patch (button-prompt texture hooks, its own FSR and CAS wiring, networking) is written for v0.10.0 and hasn't been checked against v0.10.1.

### Building it

`tools\build_sdk.bat` does all of it: clones SDK v0.10.1 and Skullburglar (pinned to the commit the patch list was checked against), applies the patches above, and builds and installs the SDK with FidelityFX on. Run it from an x64 Native Tools prompt:

```
tools\build_sdk.bat C:\rexglue-src
```

The SDK installs to `C:\rexglue-src\rexglue-sdk\out\install\win-amd64`. Then reconfigure this project against it and rebuild:

```
cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH=C:/rexglue-src/rexglue-sdk/out/install/win-amd64
cmake --build out\build\win-amd64-relwithdebinfo
```

For players, the installer would then download our patched SDK build (a zip on this repo's Releases page, checked against a fixed SHA-256, as the Table Tennis installer does) instead of the stock one.

## Later

- Skip intro logos. Renaming the THX and Sierra / High Moon movies left the game stuck on the first level's loading screen, and the Unreal Engine and PhysX logos aren't separate movie files. Needs a code-level skip instead (Bourne's UE3 build has no `-nostartupmovies` switch).
- Controller feel: stick acceleration off, linear turning, vibration toggle (needs Bourne's input code found).
- PlayStation and keyboard button prompts (texture replacement; Table Tennis's SDK patch).
- Proper keyboard and mouse (Skullburglar routes mouse look through UE3's PC input; may be compiled out of Bourne's 360 build).
- Ultrawide.
- A native Direct3D renderer like Skullburglar's pre-1.0 one, which replaces calls into the Xbox 360 Direct3D library (Microsoft code built into every 360 game). It ran 275 FPS where the emulated renderer ran 108 on an RTX 3090. Their `tools/d3d_scan.py` finds those functions.
- A UE3 scene renderer like Skullburglar's `n1`: fastest, but tied most closely to each game's engine build.
