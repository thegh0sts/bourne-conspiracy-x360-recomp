**PlayStation button prompts, controller swapping mid-game, and a fix for the game closing at startup on PCs whose Documents folder has non-English letters.** You need your own copy of the game (Xbox 360 disc image); nothing from the game is included.

**Download `BourneConspiracy.exe`** and run it, or use **Download and update** in the 0.2.5 launcher. Then click **Reinstall now** on the red notice (or **Reinstall from a disc image...**) and pick your disc image, so the game is rebuilt with the new code and SDK. **This is needed for every release**: nothing in this one takes effect until the game is reinstalled. Your settings are kept.

### New
- **PlayStation button prompts**: with a DualShock or DualSense connected, the prompts show cross / circle / square / triangle, L1 / R1 / L2 / R2 and Share / Options instead of the Xbox buttons, in text prompts, on the door / takedown badges and on the QTE prompts. They follow the connected pad and switch as soon as you change pads.

### Changes
- **With keyboard and mouse on, a controller is ignored.** That mode moves the fight attacks onto the mouse buttons and speeds up the camera for mouse look, so a controller alongside it played wrongly. Untick Keyboard and mouse to play with a controller.
- **Launcher:** Graphics path and Anti-aliasing are greyed out unless the renderer is Xenos, the only renderer they apply to.

### Fixes
- **Swapping controllers mid-game works.** A pad plugged in before the old one was unplugged did nothing; every connected controller now plays as player one.
- **The game no longer closes a few seconds after starting when the Windows Documents folder has non-English letters** (for example an Arabic or Cyrillic Documents folder moved by OneDrive).

### Known issues
- While two controllers are connected, the prompts follow the one connected first; they switch when it's unplugged.
- Looking up and down stops at the game's own camera limits (about 20 degrees on foot), as on the Xbox 360.

Native renderer by the [Skullburglar](https://github.com/nlwstein/skullburglar) project (BSD 3-Clause).

Full list in [CHANGELOG.md](https://github.com/thegh0sts/bourne-conspiracy-x360-recomp/blob/main/CHANGELOG.md).
