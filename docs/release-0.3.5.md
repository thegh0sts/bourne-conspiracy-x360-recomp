**Fight attacks on a controller's triggers, a faster native renderer, and Quit Game back on the main menu after playing.** You need your own copy of the game (Xbox 360 disc image); nothing from the game is included.

**Updating?** Click **Download and update** in the launcher (or download `BourneConspiracy.exe` and run it), then click **Reinstall now** on the red notice. The game is rebuilt with the new bundled SDK, `tbc14`. Your settings and saves are kept.

### New
- **Fight attacks on the triggers** (controller; launcher, Controls tab, off by default). Light attack on RT and heavy on LT instead of X and Y, so your right thumb stays on the camera stick in fights. The fight prompts show RT / LT, or R2 / L2 with a PlayStation controller. X still reloads and Y still uses Bourne Instinct outside fights.
- **Faster native renderer**: less CPU work per draw (from the 007 Quantum of Solace port), so busy scenes take less time each frame. Also fixes decal flicker above the original render resolution.
- **Second render thread** (launcher, Graphics tab, on by default): the native renderer prepares the GPU's work on another CPU thread, for a higher frame rate in busy scenes. Untick it if the picture shows glitches.

### Fixes
- **Quit Game was missing from the main menu** after quitting back to it from a level.

Native renderer by the [Skullburglar](https://github.com/nlwstein/skullburglar) project (BSD 3-Clause).

Full list in [CHANGELOG.md](https://github.com/thegh0sts/bourne-conspiracy-x360-recomp/blob/main/CHANGELOG.md).
