**Keyboard and mouse: move speed on the scroll wheel, new key prompts and takedown diamond, and a controller fix.** You need your own copy of the game (Xbox 360 disc image); nothing from the game is included.

**Updating?** Click **Download and update** in the launcher (or download `BourneConspiracy.exe` and run it), then click **Reinstall now** on the red notice. The game is rebuilt with the new bundled SDK, `tbc13`. Your settings and saves are kept.

### New
- **Move speed on the scroll wheel** (keyboard and mouse). W / A / S / D set the direction and the scroll wheel the speed: up faster, down slower, 10% a notch from 20% to 100%. Sprinting is always full speed. Before, the keys always ran Bourne at full speed, so he took a step or two to stop. In the first mission, after the movement tutorial, a hint tells you about it.
- **On-screen messages** (native renderer): each scroll wheel notch shows the move speed for a moment.
- **New key prompts**: flat, light grey keys sized to sit in the line of text, including the right mouse button and the scroll wheel, which were missing.
- **WASD takedown diamond**: quick-time events with keyboard and mouse show W / A / S / D keys in the diamond's places.

### Fixes
- **A controller's X and Y did nothing in fights** after the game had been played with keyboard and mouse and then started without the launcher (Play.bat). The keyboard-and-mouse changes to the game's config now live in their own copy, which the game only reads in keyboard-and-mouse mode.
- **Quit Game showed under Replay Chapter's list** once five chapters were unlocked.
- **Native renderer: some textures too dark or too bright** (a brightness factor read from the wrong place), found in another port.

Native renderer by the [Skullburglar](https://github.com/nlwstein/skullburglar) project (BSD 3-Clause).

Full list in [CHANGELOG.md](https://github.com/thegh0sts/bourne-conspiracy-x360-recomp/blob/main/CHANGELOG.md).
