**Keyboard and mouse, finished: mouse look, your keys in every prompt, W / A / S / D for every QTE, and an update check.** You need your own copy of the game (Xbox 360 disc image); nothing from the game is included.

**Download `BourneConspiracy.exe`** and run it. Already on 0.2.4 or 0.2.4-1? Run the new exe; it opens as the launcher and keeps your settings. Then click **Reinstall from a disc image...** on the settings screen and pick your disc image, so the game is rebuilt with the new code and SDK. **This is needed for every release**: nothing in this one takes effect until the game is reinstalled.

### New
- **Mouse look**: the camera turns by as much as the mouse moves, at any frame rate. No more dead zone, slow start or speed-up after half a second. Set the speed with the launcher's mouse sensitivity slider.
- **Your keys in the prompts**: key images instead of Xbox buttons in text prompts, on the door / interact badge, the takedown meter and the QTE prompts.
- **Every QTE on W / A / S / D**, multi-takedowns and boss takedowns included. The QTE prompt shows the key in the button's place on the diamond.
- **Update check** in the launcher: it tells you when a new release is out (and there's a "Check for updates" link).

### Changes
- **The keyboard layout is fixed** (no more key editor): the prompts and key images are drawn for it. "Key binds" in the launcher shows it; also [here](https://github.com/thegh0sts/bourne-conspiracy-x360-recomp/blob/main/docs/keyboard-and-mouse.md).
- **Fights are always on the mouse buttons** with keyboard and mouse: left = light attack, right = heavy attack.
- "Camera turn speed" is now "Stick turn speed", for a controller and the arrow keys.

### Fixes
- The title screen showed Enter but only took Esc; Enter and Space start the game now.
- "Hold LT to pop out" and other trigger, bumper, stick and d-pad prompts kept the Xbox button.

### Known issues
- Looking up and down stops at the game's own camera limits (about 20 degrees on foot), as on the Xbox 360.

Native renderer by the [Skullburglar](https://github.com/nlwstein/skullburglar) project (BSD 3-Clause).

Full list in [CHANGELOG.md](https://github.com/thegh0sts/bourne-conspiracy-x360-recomp/blob/main/CHANGELOG.md).
