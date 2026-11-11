# Changelog

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
