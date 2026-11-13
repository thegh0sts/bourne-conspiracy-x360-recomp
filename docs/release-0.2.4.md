**Experimental ultrawide, a fix for a crash right after starting, and an installer that checks the game files.** You need your own copy of the game (Xbox 360 disc image); nothing from the game is included.

**Download `BourneConspiracy.exe`** and run it. Already on 0.2.3? Run the new exe; it opens as the launcher and keeps your settings. Then click **Reinstall from a disc image...** on the settings screen so the game is rebuilt with the new code and SDK.

### Highlights
- **Ultrawide (experimental, native renderer only).** Pick **2560×1080**, **3440×1440** or **3840×1600** as the output resolution. The game draws a wider picture in your screen's exact shape with a wider field of view, not a stretched one. The HUD and menus spread to the edges without stretching. The render resolution list follows the output's shape, including one that matches your screen exactly (3440×1440 renders at 3440×1440). Pre-rendered videos stay 16:9.
- **Fixed: a crash about a second after starting** on some installs, when a disc extractor had left out an empty game folder. The game now creates missing folders before it starts.
- **The installer checks the game files** against the disc after extracting them, re-extracts anything missing or damaged, and says which files if it can't.

### Caveats
- Ultrawide is new. It was tested at 21:9 and in the 3440×1440 shape in a window; 32:9 and a full playthrough haven't been tested. If something looks wrong, please report it with a screenshot.
- Ultrawide needs the native renderer: Xenos can't fit the wider picture in the console's video memory, so the ultrawide resolutions are hidden while Xenos is selected.

Tested on an Intel Core i7-12700KF, 32 GB RAM, NVIDIA GeForce RTX 4080 SUPER, Windows 11.

Native renderer by the [Skullburglar](https://github.com/nlwstein/skullburglar) project (BSD 3-Clause).

Full list in [CHANGELOG.md](https://github.com/thegh0sts/bourne-conspiracy-x360-recomp/blob/main/CHANGELOG.md).
