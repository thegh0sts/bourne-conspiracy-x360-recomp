# Launcher and installer

`BourneConspiracy.exe` is a single program for players:

- **Not installed yet:** shows the installer. The player picks their own Xbox 360 disc image and it builds the game on their PC (reads the ISO, checks it's The Bourne Conspiracy, finds or installs the C++ build tools, downloads ReXGlue SDK 0.10.1, builds `tbc.exe`, creates a desktop shortcut to the launcher).
- **Installed:** shows the settings screen and a Play button.

Nothing from the game ships with it. Running the installer again resumes where it stopped.

## Settings

| Setting | How it's applied |
|---|---|
| Fullscreen, output resolution, render resolution, anti-aliasing | ReXGlue command-line settings (`--fullscreen`, `--resolution`, `--resolution_scale`, `--swap_post_effect`) |
| VSync | Display vsync. Unticked passes `--d3d12_allow_variable_refresh_rate_and_tearing=true`: frames may tear, but a present can no longer stall the graphics queue the emulation uses (the Table Tennis port measured 47 FPS instead of 60 without it). |
| Original Xbox 360 frame pacing | Off by default: the launcher passes `--vsync=false`, which turns off the plugin's imitation of the Xbox 360 vblank wait (it slept on every GPU wait), and `tbc.exe`'s own frame limiter holds the frame rate. Ticked passes `--vsync=true` and the limiter stays off. For troubleshooting. |
| Render resolution | 1280×720 (original), 2560×1440 or 3840×2160: the size the game draws at (`--resolution_scale` 1/2/3). Output resolution only sets the final picture size. Scaling is expensive in this runtime: on an RTX 4080 Super one heavy street scene ran about 48 FPS at 1x, about 20 at 2x and under 10 at 3x. For 60 FPS, use 1x with FXAA. |
| Frame rate | `--frame_rate` (30, 60, 120, 144 or 0 for unlimited), handled by `src/fps_unlock.cpp`: it changes the game's Direct3D present interval (two vblanks, 30 FPS, originally) and sets the frame limiter in `src/frame_pacing.cpp`. For 120/144/unlimited the launcher also raises `[Engine.GameEngine] MaxSmoothedFrameRate`, the engine's own limiter. |
| Show FPS counter | `--show_fps=true`: the game's frame rate in a small separate window over the game's top-left corner (`src/fps_overlay.cpp`). It isn't an ImGui overlay, because those make the SDK repaint every display refresh. F3 opens the SDK's full debug overlay. |
| Motion blur, depth of field, bloom, lens flares, heat haze / distortion | Game config `[SystemSettings]` (`MotionBlur`, `DepthOfField`, `Bloom`, `LensFlares`, `Distortion`) |
| Field of view | Scales every camera mode's `Fov=` value in the camera config |
| Stick dead zone | Game config `[HM_Engine.HmPlayerInput] DeadZone` (original 0.25) |


**Open logs** opens the folder with `game.log`. **Save crash report** zips `game.log`, the newest `logs\crash-<pid>.txt` and `.dmp` that `tbc.exe` writes on a crash, and the launcher settings onto the desktop, for attaching to an issue. No game files go in it.

On Play, the launcher rebuilds `Coalesced.ini` from the original (kept as `Coalesced.ini.bak`) with the chosen settings, updates `Xbox360TOC.txt` to match, and starts the game. With every setting at default the original files are put back. Settings are saved in `%LOCALAPPDATA%\BourneConspiracyPC\settings.json`; the install location in `launcher.json` next to it.

## Building

Needs Python 3 on PATH. From this folder:

```
build_installer.bat
```

It gathers the project files into `payload/`, copies in `..\tools\tbc_config.py`, installs PyInstaller and writes `dist\BourneConspiracy.exe`. Rebuild whenever the function lists change.

To run without packaging: `python tbc_launcher.py` (after running `build_installer.bat` once, so `payload/` and `tbc_config.py` exist).

## Files

- `tbc_launcher.py` - entry point: settings screen, applies settings, starts the game.
- `tbc_installer.py` - install logic and install screen.
- `tbc_config.py` - copied from `../tools` at build time: reads and writes the game's packed config.
- `icon.ico` - app, window and shortcut icon.

Project repo: https://github.com/thegh0sts/bourne-conspiracy-x360-recomp
