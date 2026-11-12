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

## Native mouse look (next)

The mouse still drives a virtual right stick, so the game's dead zone, turn rates and acceleration shape it. 0.2.2 compensates for the dead zone; the real fix is WetRecomp's approach: feed mouse movement straight into the camera's yaw and pitch (1:1, as an angle), with `[[midasm_hook]]`s in the camera code (supported by SDK 0.10.1), and keep the stick emulation for menus and cutscenes.

Finding the camera code in Bourne needs a run with the game: its camera classes (`BnPlayerInput`, `BnCamera`, `BnLookScales`) are native, but their property layout comes from the cooked script packages, and none of the turning code is a named native (`int<Class>exec<Function>`). Plan:
1. Hook the game's controller read (its caller of `XamInputGetState`) to log where the right-stick values are stored.
2. Watch that memory: the code that reads it and writes the camera's rotation (UE3 `FRotator`, 16-bit units) is the target.
3. Hook there: replace the stick-driven yaw/pitch change with the mouse's, behind a `mouse_look` setting (sensitivity, invert), with the stick emulation as fallback.

## Experimental: ReGPU renderer (shelved: renders incorrectly)

[ReGPU](https://github.com/zer0ducksgiven/ReGPU) is a Direct3D 12 Xbox 360 GPU, written for XenonRecomp ports; [Glue2GPU](https://github.com/zer0ducksgiven/Glue2GPU) wraps it as a ReXGlue GPU plugin (both MIT). In Forza Horizon 1 it took 4K from 36 to about 80 FPS, where our 2x render scale is held back by the stock renderer's EDRAM emulation.

- `regpu-plugin/` is Glue2GPU's plugin with its Forza-only parts left out (graphics-screen bridge, ambient occlusion and shadow shader rewrites, shader pre-scan and boot screen, pipeline list). It follows our settings: `--resolution_scale` (1280x720 times 1-3), `--frame_rate` (0: no cap), `--vsync` (display vsync with this renderer) and `--anisotropic_override`.
- `tools\build_regpu.bat` fetches ReGPU (pinned commit) and the FidelityFX SDK's FSR 1 headers (sparse), builds `rexgpu-regpurd.dll` against our SDK in RelWithDebInfo, and copies it and `d3dcompiler_47.dll` beside `tbc.exe`.
- `src/gpu_preload.cpp` now preloads whichever plugin `--gpu_plugin` names, so the two plugins' settings never clash.
- Run: `tbc.exe --gpu_plugin=regpu --game_data_root "..." --frame_rate=60 --show_fps=true`. The first start compiles shaders for a while; later starts use `cache\regpu`.
- Not available with ReGPU: the runtime's ImGui overlays (F3/F4), our FSR/CAS output filters (`--present_effect`) and FXAA. The post-processing toggles still work (they edit the game's own config), as do the FPS counter and the frame rate unlock. ReGPU has its own FSR 1 (`GLUE2GPU_UPSCALER=fsr1`, `GLUE2GPU_QUALITY=1-4`).
- Test order: boots to the menu; menu looks right (colours, text); first level loads and plays; cutscenes; then frame rate at 1x/2x/3x against the stock renderer. To A/B a problem, leave the plugin's default switches off with `set TBC_REGPU_OFF=LEAN_FETCH,TEXTURE_GAMMA` (names without `REGPU_`) or `set TBC_REGPU_OFF=all` (`REGPU_X=0` doesn't work: some switches count as on when merely set).
- **Status: shelved (2026-11-11).** Boots and plays (49 FPS at 1x with the plugin's switches on), but static world geometry and characters come out smeared, speckled and posterised, and the picture is washed out; the HUD and minimap are right. Ruled out, each with no visible change: vertex position precision (`0001-precise-position.patch`, kept: harmless and console-accurate), depth test (`REGPU_ZFUNC_ALWAYS=1`), stencil (`REGPU_NO_STENCIL=1`), mip levels (`REGPU_NO_MIPS=1`), native BC decoding (`REGPU_NO_NATIVE_BC=1`). The plugin's default switches help (all off: 30 FPS and more washed out). Next step if picked up again: ReGPU's debug server (`REGPU_DEBUG=1`, `tools/regpuctl.py capture` and `ps <hash> tint`) to find the draw behind one broken pixel, then its translated shader (`hlsl <hash>`). Suspects left: shader translation (UE3 material shaders) and texture formats or tiling specific to Bourne. Note: a `set` in a prompt stays for every later run in that window; check `logs\tbc_NNN.log` for `tbc-regpu: TBC_REGPU_OFF` before trusting a result.
- If it holds up: a launcher option **Renderer: ReXGlue (default) / ReGPU (experimental)**, the DLL in the installer, and FSR mapped to ReGPU's own.

## Later

- Skip intro logos. Renaming the THX and Sierra / High Moon movies left the game stuck on the first level's loading screen, and the Unreal Engine and PhysX logos aren't separate movie files. Needs a code-level skip instead (Bourne's UE3 build has no `-nostartupmovies` switch).
- Controller feel: stick acceleration off, linear turning, vibration toggle (needs Bourne's input code found).
- PlayStation and keyboard button prompts. Bourne's prompts are a small 3D diamond of four face buttons seen at an angle: three empty rings and the active button, which presses down and shows its symbol (the PS3 version uses the same design with PlayStation symbols). If the button is a 3D model with the symbol as a flat texture, only the symbols need replacing (cross, circle, square, triangle, L1/R1/L2/R2), drawn as our own art, and the press animation carries over. If it's pre-drawn frames, each frame needs redrawing. Step 1 is capturing the game's textures to find out. The swap itself would hook texture uploads, as Table Tennis's SDK patch does, with SDL detecting a PlayStation pad for an Auto setting. Keyboard prompts need keyboard and mouse support first.
- Proper keyboard and mouse (Skullburglar routes mouse look through UE3's PC input; may be compiled out of Bourne's 360 build).
- Ultrawide (low priority; pick up only if players ask). Today a 21:9 screen should show the normal 16:9 picture with black bars, which is fine. First, cheap check: run in a 21:9 window (for example 2560x1080 windowed, or an NVIDIA custom resolution) and confirm it shows bars rather than stretching; if it stretches, make it show bars. Real ultrawide (a wider view with the HUD in place) means patching Bourne's camera and HUD setup and checking every menu and cutscene; both reference ports have it, with stretched menus or side bars in places. No ultrawide display is needed to test: a 21:9 window or a custom resolution behaves the same to the game.
- A native Direct3D renderer like Skullburglar's pre-1.0 one, which replaces calls into the Xbox 360 Direct3D library (Microsoft code built into every 360 game). It ran 275 FPS where the emulated renderer ran 108 on an RTX 3090. Their `tools/d3d_scan.py` finds those functions.
- A UE3 scene renderer like Skullburglar's `n1`: fastest, but tied most closely to each game's engine build.
