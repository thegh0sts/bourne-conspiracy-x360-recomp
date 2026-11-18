**The launcher in German, French, Spanish and Italian, sharper textures and FXAA with the native renderer, and an installer fix.** You need your own copy of the game (Xbox 360 disc image); nothing from the game is included.

**Updating?** Click **Download and update** in the launcher (or download `BourneConspiracy.exe` and run it), then click **Reinstall now** on the red notice. The game is rebuilt with the new bundled SDK, `tbc11`. Your settings and saves are kept.

### New
- **Launcher language**: English, Deutsch, Français, Español or Italiano, at the top right of the launcher and the install screen. By default it follows the game's language. The translations were made with AI and haven't been checked by native speakers yet; corrections are welcome.
- **Texture filtering** (Graphics tab, native renderer): anisotropic 16x by default, or 8x, 4x or Game's own. Floors, roads and walls stay sharp at an angle; the HUD and text are left as they were.
- **FXAA with the native renderer** (Display tab, Anti-aliasing: Off, FXAA or FXAA strong). Before, only Xenos had it.
- **Clock under the FPS counter** (native renderer), to match a video recording to the game log.
- **Key glyphs redrawn with a black outline**, so they're readable on light backgrounds.

### Fixes
- **Installer: "Build Tools finished installing but couldn't be found".** With Visual Studio 2022 or newer already installed but missing Clang or CMake, the installer couldn't add them. It now adds the missing parts to your Visual Studio (Windows asks for administrator permission).
- **Black flicker at the 1920×1080 render resolution** with the native renderer.

### Changed
- **Frame rate note** on the Display tab: above 60 FPS is experimental. For smoother motion, keep 60 and turn on your graphics driver's frame generation (NVIDIA Smooth Motion, AMD Fluid Motion Frames); the README has the steps.

### Known issue
- **Above 60 FPS (experimental), scripted moments can stall** in the second level, some of the time: Solomon's intro in the warehouse may never start, and on the boat one enemy of the second wave may stay on the support ship. Load the last checkpoint, or play at 60.

Native renderer by the [Skullburglar](https://github.com/nlwstein/skullburglar) project (BSD 3-Clause).

Full list in [CHANGELOG.md](https://github.com/thegh0sts/bourne-conspiracy-x360-recomp/blob/main/CHANGELOG.md).
