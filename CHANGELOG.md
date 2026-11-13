# Changelog

## Unreleased

### New
- **Native renderer, now the default.** Skullburglar's Vulkan renderer (pre-1.0, built by `native-plugin/` with `tools\build_native.bat`) draws the game as ordinary Vulkan rendering instead of emulating the Xbox 360's EDRAM. It renders the whole game correctly at 1x, 2x and 3x. Choose **Renderer** under Display in the launcher; ReXGlue's **Xenos** (Direct3D 12) stays as the second option, and the launcher falls back to it if the native plugin isn't installed. The render resolution reaches the native renderer through `BOTS_GFX_RENDER_SCALE`; graphics path, upscaling and anti-aliasing apply to Xenos only.
- **The game's 2x MSAA is off by default** (`--msaa`, `src/msaa.cpp`). The console draws the 720p HDR scene with 2x MSAA in two strips (predicated tiling), which the native renderer can't draw and which costs Xenos frame rate at higher render resolutions. Running without it needed three fixes to the engine's untested non-tiled path: the shadow mask's half-resolution depth is moved off the scene depth in EDRAM (it caused a dark box in the top-left quarter), the scene renderer's second scene-depth resolve is skipped (it copied the shadow mask's depth instead: stripes, a split down the middle, and a resolve error every frame on Xenos), and no extra depth clear is made at the scene start. `--msaa=true` brings the console's anti-aliasing back.

### Credits
Skullburglar (BSD 3-Clause): the native renderer. Its licence is installed with the game (`licenses\`).

## 0.2.2 (2026-11-12)

Much faster at higher render resolutions, easier mouse aiming, ideas from WetRecomp, and fixes to our SDK build.

### Updating from 0.2.1
Run the new exe. The **Graphics path** setting works straight away. Everything else here is in the game code or the SDK, so click **Reinstall from a disc image...** on the launcher's settings screen to rebuild the game with them. The installer replaces its bundled SDK (now `tbc2`) on that run.

### New
- **Much faster above 720p.** New **Graphics path** setting under Display. The runtime's default way of imitating the Xbox 360's on-chip framebuffer (EDRAM) copied pixels between render targets about 1,800 times a frame. At 2x render resolution those copies took 54 of 68 ms per frame on an RTX 4080 Super. The shader-based path (ROV) needs no copies: 2x went from the mid-teens to the mid-40s FPS, and 3x (4K) from about 7 to about 22 (GPU time per frame 139 ms down to 46 ms). **Auto** (default) uses ROV whenever the render resolution is above 720p and the runtime's default at 1x; RTV and ROV can also be forced (`--render_target_path_d3d12=rtv|rov`).
- **Easier mouse aiming.** Any mouse movement now starts just past the game's stick dead zone, so slow, small movements turn the camera instead of being ignored. The launcher passes the **Stick dead zone** setting to the mouse conversion automatically (`--mnk_mouse_deadzone`); `--mnk_mouse_curve` shapes the response. Needs the SDK with patch 0004.
- **Less CPU while waiting on the GPU.** The game's GPU-wait loop (`sub_822F0108`, the XDK poll) now yields the CPU core between polls instead of spinning it flat out (`--gpu_wait_mode`: 0 original, 1 yield (default), 2 sleep).
- **Match the monitor's refresh rate** (`--match_display_refresh=true`, off by default): with original pacing, the game's vblank follows your monitor (for example 120 Hz instead of 60). The SDK caps it at 240 Hz.
- **Developer tool:** `--dev_debug_runtime=true` lists every code address without a recompiled function (`logs\missed_functions.txt`) and stubs them, logging the first call to each (`logs\stub_sweep.txt`) instead of aborting. Development only.

### SDK fixes (tools\sdk-patches, applied by tools\build_sdk.bat)
- **0001, 0002: jump tables.** The code generator now finds a jump table's index in either operand order and reads past unused (zero) entries. Found while porting 007: Quantum of Solace, where both crashed the game; Bourne picks the fixes up on its next code generation.
- **0003: XMA audio decoder.** Input checks from WetRecomp's SDK: a garbled frame size could make the decoder copy far past its buffer.
- **0004: mouse dead-zone compensation and response curve** (above).

### Credits
WetRecomp by nikolaygorb (MIT): the GPU-wait hook, display refresh matching, the stub sweep and the XMA decoder fixes.

## 0.2.1 (2026-11-11)

Keyboard and mouse, and camera turn speed.

### New
- **Keyboard and mouse** (experimental). Tick **Keyboard and mouse** under Controls in the launcher: W A S D to move, the mouse for the camera, left mouse to fire, right mouse to aim, and keys for every other action. A controller keeps working alongside. The mouse acts as a controller stick, so the game's stick dead zone still applies: a lower **Stick dead zone** makes small mouse movements count.
- **Editable key binds.** **Edit keys...** next to the switch lets you change any action: two keys per action, mouse buttons included, with **Defaults** to go back. A key already in use moves to the new action; F3, F4, F7 and \` stay reserved for the runtime's overlays.
- **Camera turn speed** (Controls, 50-300%). Scales the camera's turn rate in every mode (walking, jogging, sprinting, crouching, aiming) and its top yaw rate, for mouse and controller alike.
- **Full-speed aiming with the mouse.** With **Keyboard and mouse** ticked, aiming over the shoulder no longer has its slow zone (the game turns at 30% speed until the stick is pushed past 95%, which a mouse rarely reaches).
- **Mouse sensitivity** goes up to 1000% (was 300%).

### Known issues
- Mouse camera can still feel sluggish. The turn-speed and aiming changes reach the game's config, but whether Bourne uses those values at runtime is still being confirmed; if a controller turns no faster at 300% turn speed, they're baked into the game data and a code-level fix is needed.
- Skipping the intro logos still isn't possible.

### Under the hood
- **Experimental ReGPU renderer, shelved.** A second, optional renderer: [ReGPU](https://github.com/zer0ducksgiven/ReGPU), a Direct3D 12 Xbox 360 GPU, through an adapted [Glue2GPU](https://github.com/zer0ducksgiven/Glue2GPU) plugin (`regpu-plugin/`, `tools\build_regpu.bat`, `--gpu_plugin=regpu`). Not built or shipped by the installer. It boots and plays but renders world surfaces incorrectly; what was ruled out is in docs/ROADMAP.md. The default renderer is unchanged.
- The graphics plugin preload now follows `--gpu_plugin` (default xenos), so two plugins' settings never clash.
- `.gitattributes` keeps patch files byte-exact.

### Credits
ReGPU and Glue2GPU by zer0ducksgiven (MIT). Licence texts in `licenses/`.

## 0.2.0 (2026-11-10)

Performance, frame rate and image options, built on a patched ReXGlue SDK.

### New
- **60 FPS and higher.** The game was locked to 30 FPS by its own Direct3D setup; a new **Frame rate** option offers 30 (original), 60, and experimental 120, 144 and unlimited. 60 FPS is experimental too: the game was only tuned for 30, so switch back if anything plays too fast or breaks.
- **Faster frame pacing.** The graphics plugin's imitation of the Xbox 360's 60 Hz screen timer slept on every GPU wait. It's now off, and the game's own frame limiter holds the chosen rate. In the busiest scene we know (the street at the start of the first level), 1x went from about 48 to the mid-50s FPS, and the rest of the level holds a solid 60. **Original Xbox 360 frame pacing** remains as an option for troubleshooting.
- **Upscaling filter (FSR / CAS).** AMD FidelityFX filters for a sharper picture at almost no cost. They also soften menu and HUD text (the game draws them into the same image), so **Bilinear (original)** stays the default.
- **VSync option** now controls display vsync. Turning it off allows tearing and can be faster.
- **FPS counter** shows the game's real frame rate in a small window at the top-left. It no longer costs frame rate (the old overlay forced extra repaints).
- **Render resolution** option (1280×720, 2560×1440, 3840×2160) that actually works: the setting was being dropped before. Higher resolutions are demanding: 2x runs in the mid-teens in the busiest scene.
- **Turn off post-processing effects:** motion blur, depth of field, bloom, lens flares and heat haze / distortion can each be switched off in the launcher's Graphics section, for a cleaner picture or a little more performance. They're stored in the game's own config; setting them back to on restores the original files.
- **Open logs** and **Save crash report** in the launcher. The game now writes a crash report and dump to `logs\` if it crashes; the report zip goes to your desktop and contains no game files.
- **Installer includes the SDK.** The installer unpacks a bundled ReXGlue SDK instead of downloading the stock one. Existing installs switch to it on reinstall.

### Fixed
- Windowed mode no longer fills the screen at 1440p or 4K output.
- "Failed to remove temporary directory" message when the launcher closed while the game started.
- Fullscreen setting now applies when unticked.

### Under the hood
- **Patched ReXGlue SDK 0.10.1** (`tools/build_sdk.bat`): AMD FidelityFX enabled, plus nine of Skullburglar's SDK patches (CPU timer that spun a quarter of a core, overlay repaint cost, register-write overhead, background shader compilation, a texture conversion memory bug, and their prerequisites).
- Graphics plugin loaded before command-line settings are read, so its settings (render resolution, vsync, anti-aliasing) apply.
- Leak-scan git hooks block game files, generated code and personal paths from commits (`git config core.hooksPath .githooks`).
- Roadmap in `docs/ROADMAP.md`.

### Credits
Frame pacing approach from the [Table Tennis PC port](https://github.com/davidptrssvea/Table-Tennis-Rockstar-PC) (MIT). SDK patches, crash reporting and the leak scanner from [Skullburglar](https://github.com/nlwstein/skullburglar) (BSD 3-Clause). Licence texts in `licenses/`.

### Known issues
- Skipping the intro logos isn't possible yet: removing the logo movies left the first level stuck loading.
- The opening street of the first level stays in the mid-50s at 1x; higher render resolutions are limited by the emulated graphics.

## 0.1.0 (2026-11-09)

First release: the game recompiled with ReXGlue SDK 0.10.1, playable through the first levels. Installer that builds the game from your own disc image, and a launcher with display, graphics, camera and controller settings.
