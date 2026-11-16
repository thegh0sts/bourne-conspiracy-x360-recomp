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

## 0.2.1: keyboard and mouse (released, needs testing)

The SDK's keyboard-and-mouse driver is already in our runtime, switched off (`--mnk_mode=true --mnk_mouse=true`). It acts as a virtual Xbox controller: keys and mouse buttons (LMB, RMB, MMB) bind to pad buttons with `--keybind_*` settings, the mouse drives the right stick (`--mnk_sensitivity`), and it merges with a real controller on player 1. The launcher's **Controls** section has a **Keyboard and mouse** switch, mouse sensitivity, and **Edit keys...** for rebinding. Defaults, from the game's controller bindings:

| Action (controller) | Keyboard / mouse |
|---|---|
| Move (left stick) | W A S D |
| Camera (right stick) | Mouse |
| Fire (RT) | Left mouse |
| Aim / targeting (LT) | Right mouse |
| Action / block (A) | Space |
| Takedown, skip cutscene (B) | F |
| Light attack / reload (X) | E, R |
| Heavy attack / Bourne Instinct (Y) | Q |
| Sprint (RB) | Shift |
| Weapon (LB) | G |
| Crouch (L3) | Control |
| Recentre camera / switch shoulder (R3) | Middle mouse |
| Pause (Start) | Escape, Return |
| Back | Tab |

It's controller emulation, so the game's stick dead zone and acceleration still apply to the mouse. Native mouse look through the engine's PC input (Skullburglar's approach) would be a later, bigger step, if Bourne's Xbox 360 build includes that code. Keys can be changed in the launcher (**Edit keys...**).

Camera tuning for the mouse (0.2.1): **Camera turn speed** scales every `[<Mode>LookScales BnLookScales]` turn rate in `Xe-BourneGame.ini` and `[BourneNative.BnPlayerInput] MaxYawRate` (100) in `Xe-BourneInput.ini`; with keyboard and mouse on, `BelowStepMultiplier` goes to 1 (over the shoulder: 0.3 below 95% stick). Confirmed written to the game's config; not yet confirmed that the game reads them at runtime (test: a controller at 300% turn speed should turn visibly faster). If it doesn't, the next options are a mouse-only dead-zone offset in the SDK's keyboard-and-mouse driver (`mnk_input_driver.cpp`, an SDK rebuild) or finding Bourne's look code. Also worth a look: `CameraSensitivity=4` (the in-game option's default) and the running camera's `MaxStickResistance`, which pulls the camera back behind Jason.

## 0.2.7: more responsive mouse look (done)

1. **Less camera smoothing with keyboard and mouse:** the launcher's Mouse camera smoothing setting (default 10% of the
   game's `SmoothTime`; 0.2.5-0.2.6 used 25%).
2. **No lost movement on fast flicks** (stick method): full stick is 6x the game's turn rate, and movement past it is
   carried into the next polls.
3. **Direct mouse look (the default):** the mouse's movement is added straight to the camera's stored angles
   (`src/mouse_look.cpp`), 1:1 with no stick dead zone, turn-rate cap or look acceleration. No code hooks were needed:
   a probe found the three places the camera keeps its orbit (the player input's `CurrentControlRotation`, the orbit
   smoother's `SmoothedOrbitRotation`, and the strategy result's orbit, which some strategies spring back to). The
   stick method stays as the fallback (menus, loading, or Direct unticked).

Left: a frame of latency remains (the game reads the controller once a frame; 60 fps halves it).

## Later

- **Launcher in other languages** (to do). German, French, Spanish and Italian for the launcher and installer (about 220 strings: labels, buttons, tabs, dropdown choices, notes, message boxes, the install screen) through one translation table keyed by the English text, falling back to English when a string is missing. It follows the same Language setting the Achievements tab already uses (one setting for the game, the achievements and the launcher), switching as soon as it changes, with a "Launcher language" override on the Game files tab ("Same as game" by default, then each language named in itself: English, Deutsch, Français, Español, Italiano). The override matters because the game's language is all or nothing: each language has only its own voices and subtitles (German is German voices and German subtitles), so a player who wants the English voices with a German launcher needs it. Label the game setting "Game language (voices, subtitles and text)" so that's clear. On first run, default to the Windows display language when it's one of the five, else English. Settings keep storing the English values (only what's shown is translated), so switching never resets anything. Check the longer German / French text still fits the 980x740 window. Translations would be written without a native-speaker review; a small script should flag English text missing from the table.

- **Linux build (native renderer only, no Direct3D 12).** The SDK builds on Linux (`linux-amd64` presets) and Skullburglar's native Vulkan renderer is cross-platform; Xenos (D3D12) is left out entirely, so the launcher's Renderer setting and the Xenos-only options (graphics path, FXAA) are hidden there. Work: Linux versions of the Windows-only pieces (crash reports `crash_win.cpp`, the Win32 FPS counter window, `gpu_preload.cpp`, parts of frame pacing), a shell version of `tools/build_native.bat` that applies the same SDK and native-plugin patches with Vulkan on and no D3D12, a Linux path in the installer (clang/gcc, no Visual Studio) or prebuilt binaries, and testing on real hardware (a Linux PC or Steam Deck). First check: whether the Windows build already runs under Proton with the native renderer.

- **Native renderer: FPS counter in fullscreen** (done, `fps-fullscreen` branch): the native renderer's present pass draws it into the picture (`native-plugin/patches/0006-fps-counter.patch`); the separate window (`src/fps_overlay.cpp`) is only used with Xenos.
- **Detailed performance overlay** (done, `fps-fullscreen` branch; GPU load is the GPU's busy time from the timestamps, not Windows' counters): the launcher's Show FPS counter becomes Off / FPS / Detailed. Detailed adds frame time (average and worst / 1% low), GPU time per frame (the native renderer's timestamp queries) and GPU load % (Windows' GPU engine counters, as Task Manager), CPU load for the PC and the game (GetSystemTimes / GetProcessTimes), RAM and VRAM (GetProcessMemoryInfo, VK_EXT_memory_budget), and whether the frame waited on the GPU. Drawn by the native present pass with a small bitmap font (5x7 glyphs as bits in the shader); the Xenos window draws the same text with GDI. Sampled once or twice a second, so it costs nothing measurable.

- **Skip intro videos** (shelved, 2026-11-16). Tried replacing the logo movies with short black Bink clips cut from the player's own files (the header and first, black frame repeated; `BIKi`, 1280x720): THXLogo51Mix.bik and SierraHighMoonLogos.bik (THX; mountain, Sierra, High Moon). One frame showed a green flash; eight frames showed black and a new game loaded, but resuming a save hung on the loading screen at times. Blanking BourneLegal_<lang>.bik (copyright text, then the Unreal Engine logo; the startup loading movie) made the game start LoadingScreen at boot and resume hung. There is no PhysX logo movie. If picked up again: find why the loading-screen movie stalls after a shortened movie (the movie player's state or audio tracks; THX has 10), rather than swapping files.
- Controller feel: done. Linear turning is in 0.2.7; a vibration on/off checkbox uses the runtime's `vibration` setting (every rumble the game asks for goes through XamInputSetState). A strength slider would need an SDK patch (the runtime only switches it on or off).
- PlayStation and keyboard button prompts. Bourne's prompts are a small 3D diamond of four face buttons seen at an angle: three empty rings and the active button, which presses down and shows its symbol (the PS3 version uses the same design with PlayStation symbols). If the button is a 3D model with the symbol as a flat texture, only the symbols need replacing (cross, circle, square, triangle, L1/R1/L2/R2), drawn as our own art, and the press animation carries over. If it's pre-drawn frames, each frame needs redrawing. Step 1 is capturing the game's textures to find out. The swap itself would hook texture uploads, as Table Tennis's SDK patch does, with SDL detecting a PlayStation pad for an Auto setting. Keyboard prompts need keyboard and mouse support first.
- Proper keyboard and mouse (Skullburglar routes mouse look through UE3's PC input; may be compiled out of Bourne's 360 build).

## Closed

- **Ultrawide**: done. Ultrawide outputs (21:9 and wider) are a launcher resolution option with the native renderer (experimental), with a wider view rather than bars.
- **Renderer**: done. The native Vulkan renderer (Skullburglar's pre-1.0, `native-plugin/`) is the default; no further renderer projects (a native Direct3D renderer replacing the Xbox 360 Direct3D library, or a UE3 scene renderer like Skullburglar's `n1`) are planned. The shelved ReGPU experiment (zer0ducksgiven's ReGPU through Glue2GPU's plugin: static geometry and characters rendered smeared) was removed from the repo; it's in the history before 2026-11-16.
