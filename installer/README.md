# Launcher and installer

`BourneConspiracy.exe` is a single program for players:

- **Not installed yet:** shows the installer. The player picks their own Xbox 360 disc image and it builds the game on their PC (reads the ISO, checks it's The Bourne Conspiracy, finds or installs the C++ build tools, downloads ReXGlue SDK 0.10.1, builds `tbc.exe`, creates a desktop shortcut to the launcher).
- **Installed:** shows the settings screen and a Play button.

Nothing from the game ships with it. Running the installer again resumes where it stopped.

## Settings

| Setting | How it's applied |
|---|---|
| Fullscreen, output resolution, render resolution, anti-aliasing, VSync | ReXGlue command-line settings (`--fullscreen`, `--resolution`, `--resolution_scale`, `--swap_post_effect`, `--vsync`) |
| Render resolution | 1280×720 (original), 2560×1440 or 3840×2160: the size the game draws at (`--resolution_scale` 1/2/3). Output resolution only sets the final picture size. |
| Frame rate | `--frame_rate` (30, 60 or 0), handled by `src/fps_unlock.cpp`: the game asks Direct3D for a present interval of two vblanks (30 FPS) and this changes it to one (60) or none. For 120/144/unlimited the launcher also raises `[Engine.GameEngine] MaxSmoothedFrameRate`, the engine's own limiter. Needs a rebuilt `tbc.exe`. |
| Show FPS counter | `--show_fps=true`, a small counter drawn by `src/tbc_app.h` (builds from before this option need rebuilding). F3 opens the SDK's full debug overlay in any build. |
| Motion blur, depth of field, bloom | Game config `[SystemSettings]` |
| Field of view | Scales every camera mode's `Fov=` value in the camera config |
| Stick dead zone | Game config `[HM_Engine.HmPlayerInput] DeadZone` (original 0.25) |


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
