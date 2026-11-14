**Installer fix for PCs with another Clang installed, Shift and Ctrl as keyboard binds, and quick-time events on W / A / S / D.** You need your own copy of the game (Xbox 360 disc image); nothing from the game is included.

**Download `BourneConspiracy.exe`** and run it. Already on 0.2.4? Run the new exe; it opens as the launcher and keeps your settings. Then click **Reinstall from a disc image...** on the settings screen so the game is rebuilt with the new code and SDK.

### Fixes
- **Install failed with "explicit specialization of undeclared template struct 'clock_time_conversion'"** on PCs where another tool had put its own Clang on PATH. The installer now always builds with Visual Studio's own Clang.
- **Shift (sprint) and Ctrl now work as keyboard binds.** Before, a bind that was just Shift or Ctrl never fired.

### New
- **Quick-time events on W / A / S / D** with keyboard and mouse: W, A, S and D press the button in the same place on the prompt's diamond (Y, X, A, B). Space / E / F / Q still work.

### Known issues
- Multi-takedown prompts don't take W / A / S / D yet; use Space / E / F / Q there.
- The prompt diamond still shows Xbox buttons.

Native renderer by the [Skullburglar](https://github.com/nlwstein/skullburglar) project (BSD 3-Clause).

Full list in [CHANGELOG.md](https://github.com/thegh0sts/bourne-conspiracy-x360-recomp/blob/main/CHANGELOG.md).
