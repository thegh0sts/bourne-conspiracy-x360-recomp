**Installer fixes for 0.3.0.** You need your own copy of the game (Xbox 360 disc image); nothing from the game is included.

**Did the 0.3.0 reinstall stop with "'cmake' is not recognized"?** Update to 0.3.1 (**Download and update** in the launcher, or download `BourneConspiracy.exe` and run it), then click **Reinstall now** again. It now finds Visual Studio's CMake itself.

**Already reinstalled with 0.3.0?** Nothing else to do: the game doesn't change, so 0.3.1 won't ask you to reinstall.

### Fixes
- **Install stopped with "'cmake' is not recognized"** even though Visual Studio and its CMake are installed. It only worked on PCs that also had CMake on the system PATH, or when the installer was started from a Visual Studio developer prompt.

### Changed
- **No "Allow required components to be downloaded and installed" checkbox.** If your PC doesn't have Microsoft's C++ build tools yet, the installer downloads and installs them as part of the install (Windows asks for administrator permission). Leaving the box unticked used to stop the install with an error.

Full list in [CHANGELOG.md](https://github.com/thegh0sts/bourne-conspiracy-x360-recomp/blob/main/CHANGELOG.md).
