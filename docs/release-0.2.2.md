**Much faster at higher render resolutions, and easier mouse aiming.** You need your own copy of the game (Xbox 360 disc image); nothing from the game is included.

**Download `BourneConspiracy.exe`** and run it. Already on 0.2.1? Run the new exe; it opens as the launcher and keeps your settings. The new **Graphics path** setting works straight away. For the rest of this release, click **Reinstall from a disc image...** on the settings screen so the game is rebuilt with the new code and SDK.

### Highlights
- **Higher render resolutions are about 3x faster.** New **Graphics path** setting under Display. At 2560×1440 the busy street at the start of the first level went from the mid-teens to the mid-40s FPS on an RTX 4080 Super, and at 3840×2160 from about 7 to about 22. The runtime used to copy pixels between render targets about 1,800 times a frame to imitate the Xbox 360's on-chip framebuffer; the ROV path does the same work in the pixel shaders with no copies. **Auto** (default) uses ROV above 720p.
- **Easier mouse aiming:** small, slow mouse movements now move the camera instead of being swallowed by the stick dead zone.
- **Less CPU use** while the game waits on the GPU (the wait loop yields the core instead of spinning).
- **Match the monitor's refresh rate** (`--match_display_refresh=true`, off by default) for original frame pacing at 120 Hz and up.
- **SDK fixes:** jump-table code generation, and input checks in the XMA audio decoder from WetRecomp.

### Caveats
- ROV is new for this game and hasn't been played through yet. If anything looks wrong (lighting, shadows, smoke, glass, the HUD), set **Graphics path** to **Render targets (RTV)** and please report it.
- 3840×2160 is playable but stays below 30 FPS in busy scenes. For a steady 30 or more, use 2560×1440.
- The mouse still drives a virtual controller stick; native mouse look is on the roadmap.

Tested on an Intel Core i7-12700KF, 32 GB RAM, NVIDIA GeForce RTX 4080 SUPER (driver 621.01), Windows 11.

Thanks to [WetRecomp](https://github.com/nikolaygorb/WetRecomp) by nikolaygorb (MIT) for the GPU-wait hook, refresh matching, the stub sweep and the XMA decoder fixes.

Full list in [CHANGELOG.md](https://github.com/thegh0sts/bourne-conspiracy-x360-recomp/blob/main/CHANGELOG.md).
