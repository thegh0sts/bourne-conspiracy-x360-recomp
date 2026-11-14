# Changelog

## 0.2.5 (2026-11-14)

An installer fix for PCs with another Clang installed, Shift and Ctrl working as keyboard binds, and quick-time events on W / A / S / D.

### Updating from 0.2.4
Run the new exe, then click **Reinstall from a disc image...** on the launcher's settings screen. The game is rebuilt and the bundled SDK replaced (now `tbc5`, with the keyboard fix). Your settings and key binds are kept.

### Fixes
- **Install failed on PCs with another Clang on PATH** (seen with a recompilation toolkit's clang 22): every file stopped with "explicit specialization of undeclared template struct 'clock_time_conversion'". That Clang builds for the MinGW target with LLVM's own C++ library, which can't build or link against the SDK. The installer now always builds with Visual Studio's own Clang, named by its full path, and logs which one it used.
- **Shift (sprint) and Ctrl did nothing as keyboard binds.** The SDK's keyboard driver required the exact set of modifier keys a bind names, so a bind that is just Shift could never fire, and while Shift was held W / A / S / D stopped too (SDK patch 0008).

### New
- **Quick-time events on W / A / S / D** (keyboard and mouse). While a QTE prompt is up, W, A, S and D press the button in the same place on its diamond (Y, X, A, B), and Bourne doesn't walk. Only a fresh press counts, so running into a QTE with W held doesn't answer it. Space / E / F / Q still work. Multi-takedowns use a different prompt and don't take W / A / S / D yet. `--qte_wasd=false` turns it off.

## 0.2.4 (2026-11-14)

Experimental ultrawide support with the native renderer, a fix for a crash right after starting, and an installer that checks the game files.

### Updating from 0.2.3
Run the new exe, then click **Reinstall from a disc image...** on the launcher's settings screen. The game is rebuilt and the bundled SDK replaced (now `tbc4`, with a native renderer fix for ultrawide). Your settings and key binds are kept.

### New
- **Ultrawide (experimental, native renderer).** New output resolutions 2560×1080, 3440×1440 and 3840×1600. They set the picture's shape too: the game draws a wider picture (1720×720 for 3440×1440) with a wider field of view rather than a stretched one, and the HUD and menus spread to the edges without stretching. The render resolutions offered follow the output's shape, including one that matches the screen exactly (3440×1440 renders at 3440×1440), so a 16:9 render resolution can't end up on an ultrawide screen or the reverse. The game's video memory layout is adjusted so the shadow map and effect buffers still fit; without that, large areas fell into a false shadow. Pre-rendered videos stay 16:9. Xenos can't fit the wider picture in the console's video memory, so the ultrawide outputs are only offered with the native renderer. `--aspect_ratio` sets the shape directly.

### Fixes
- **Crash about a second after starting** on some installs (0.2.2 and 0.2.3): the game lists its folders at startup, and if a disc extractor had dropped an empty one (the disc's `Engine` folder is empty), the runtime threw "Failed to enumerate directory" instead of reporting the folder missing. The game now creates any missing standard folder before starting.

### Changes
- **The installer checks the game files against the disc** after extracting them, and when it finds them already extracted: every folder must exist (empty ones too) and every file must be there at the disc's size. Missing folders are created and missing or damaged files extracted again; if that fails the install stops and says which files. The two files the launcher edits (Coalesced.ini and Xbox360TOC.txt) are only checked for presence.

## 0.2.3 (2026-11-13)

A new default renderer that keeps high render resolutions at full speed, the console's 2x anti-aliasing turned off to make that possible, and keyboard prompts.

### Updating from 0.2.2
Run the new exe, then click **Reinstall from a disc image...** on the launcher's settings screen. The game has to be rebuilt, and the installer replaces its bundled SDK (now `tbc3`, which includes the native renderer) on that run. Your settings and key binds are kept.

### New
- **Native renderer (default).** Skullburglar's Vulkan renderer draws the game's graphics as ordinary Vulkan rendering instead of imitating the Xbox 360's on-chip framebuffer (EDRAM), the main cost at higher render resolutions. In testing (streets, an alley, interiors, a fight) it ran smoothly at 1x, 2x and 3x render resolution on an RTX 4080 Super. Choose it under **Display > Renderer**. ReXGlue's renderer stays available as **Xenos (Direct3D 12)**; if the native renderer isn't installed, the launcher says so and starts with Xenos.
- **The console's 2x anti-aliasing is off by default.** The Xbox 360 draws the 3D scene with 2x MSAA in two strips (predicated tiling). The native renderer can't draw that, and it also cost Xenos frame rate (frame drops at 2x with it on). The game now draws the scene in one pass. That path was never finished in the engine, so three of its bugs are fixed here: a dark box in the top-left quarter of the screen, black stripes and a split down the middle of the picture, and an over-dark image on Xenos. `--msaa=true` brings the anti-aliasing back (Xenos only).
- **Keyboard prompts (experimental).** With keyboard and mouse on, button prompts show your keys (for example `[F]`) instead of Xbox buttons, and menus show Enter / Backspace. The default layout now puts Enter on A (accept) and Backspace on B (back), with Esc for pause.
- **1920×1080 render resolution.** The native renderer can draw at 1.5x, between the original 720p and 1440p. Xenos only scales by whole numbers, so with Xenos this setting draws at 1440p.
- **Mouse buttons attack in fights** (keyboard and mouse, on by default): left and right mouse are light and heavy attacks in hand-to-hand fights and fire / aim outside them.
- **The log starts with the version**, build time and launch options, so a log someone sends shows which build it came from.

### Changes
- Graphics path and anti-aliasing (FXAA) are Xenos settings and are now labelled that way. The upscaling filter (FSR / CAS) works with both renderers: it scales the finished picture to your screen, so it matters when the render resolution is below the output resolution. **FSR is now the default** for new settings (bilinear looked very pixelated next to it at 720p on a 4K screen); existing settings keep their choice.
- The installer installs third-party licences in a `licenses` folder next to the game.

### Fixes
- SDK patch 0005: the input system is locked against two threads reading controllers at once, which could corrupt memory and crash (seen in 007: Quantum of Solace).
- Running `tbc.exe` without `--gpu_plugin` no longer gives a black window: it uses the native renderer.

### Known issues
- Keyboard prompts are still being tested: some keys may show wrong, and quick-time events show Xbox buttons (press the key bound to that button).
- With the native renderer the launcher's FXAA setting has no effect.
- With the native renderer in fullscreen the FPS counter isn't shown (it is in a window).
- The first time an area is shown, some objects can appear black for a moment while their shaders compile.

### For developers
- `native-plugin/` builds the native renderer from Skullburglar's pre-1.0 sources as `rexgpu-native`; `tools\build_native.bat` builds the SDK with it. Build the installer's SDK with that script, or players only get Xenos.
- New diagnostics: `--msaa_edram_log` (EDRAM placement and resolves), `--resolve_trace` (SDK patch 0006), `--script_trace` (UnrealScript events), `BOTS_DRAW_TRACE` / `BOTS_DUMP_RESOLVES` (native renderer).

### Credits
[Skullburglar](https://github.com/nlwstein/skullburglar) (BSD 3-Clause): the native renderer. Its licence is installed with the game (`licenses\skullburglar-LICENSE.txt`).

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
