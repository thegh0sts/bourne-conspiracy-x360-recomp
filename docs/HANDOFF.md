# Handoff

Where the project stands, how it's worked on, and what's open. Update it at the end of each
working session. Last updated: 2026-11-18, after the 0.3.3 release.

## Where things are

| What | Where |
|---|---|
| Repository | `thegh0sts/bourne-conspiracy-x360-recomp`, branch `main` |
| Developer's test clone | `%USERPROFILE%\source\repos\tbc-test` |
| Test install (0.3.3) | `F:\Retail Games\tbc4\BourneConspiracyPC` (`game.log` and `install.log` at its root, `tbc.toml` in the build folder `project\out\build\win-amd64-relwithdebinfo`) |
| Launcher settings | `%LOCALAPPDATA%\BourneConspiracyPC\settings.json`; `launcher.json` there names the install the launcher uses |
| Native shader list (pipeline manifest) | `Documents\tbc\cache\native\565507D4.manifest.bin` (3,101 pipelines on 2026-11-18) |
| SDK build output | `C:\rexglue-src\rexglue-sdk\out\install\win-amd64` (`bin\rexgpu-nativerd.dll` is the RelWithDebInfo native renderer) |
| SDK bundles | GitHub release `sdk` ("SDK bundles"): `rexglue-sdk-0.10.1-tbc10-win-amd64.zip`, `...-tbc11-...`; next: `...-tbc12-...` (not uploaded yet) |
| Skullburglar (native renderer) | pinned at `67aacb28670077e839f84931dee9b162526bbdb2`; our patches `native-plugin/patches/0001`-`0012`, applied by `tools/build_sdk.bat` after a hard reset of its checkout |

## How work is done

- **The developer builds and releases on Windows**, from the command line. Give exact commands.
  The developer picks version numbers.
- **Build the renderer and SDK:** `tools\build_native.bat` in an *x64 Native Tools Command Prompt
  for VS* (also rebuilds tbc in the clone).
- **Build the installer:** `cd installer`, `set "SDK_ZIP="`, `build_installer.bat` → `installer\dist\BourneConspiracy.exe`;
  the SDK is packed as `installer\payload\sdk\<SDK_BUNDLE>`. Check `dir payload\sdk` shows the
  current bundle name (a stale `build_installer.bat` packed tbc11 contents as tbc10 once).
- **Release:**
  ```
  gh release upload sdk installer\payload\sdk\<bundle>.zip        (only when SDK_BUNDLE changes)
  gh release create vX.Y.Z installer\dist\BourneConspiracy.exe --target main --title "vX.Y.Z" --notes-file docs\release-X.Y.Z.md
  ```
- **Version bump:** `VERSION` and `GAME_VERSION` in `installer/tbc_launcher.py`; `project(... VERSION)`
  and `TBC_RELEASE` in `CMakeLists.txt`; date the `CHANGELOG.md` section; write `docs/release-X.Y.Z.md`
  (same shape as the previous one). A new `GAME_VERSION` or `SDK_BUNDLE` makes installs show
  **Reinstall now**.
- **Bump `SDK_BUNDLE`** (`installer/build_installer.bat`, `-tbcNN`) whenever the SDK or renderer
  patches change, or existing installs keep the old renderer.
- **Testing one change without a reinstall:** copy the changed `src/*.cpp` into
  `<install>\project\src\` and run `cmake --build out\build\win-amd64-relwithdebinfo` in
  `<install>\project`; or copy `rexgpu-nativerd.dll` next to `tbc.exe`.
- **Launcher from source:** `python installer\tbc_launcher.py` in the clone.
- **Launcher translations:** every new UI string goes in `installer/launcher_text.py` (`TEXT`:
  English → de, fr, es, it); `python tools/check_launcher_text.py` must say `0 missing`. The
  Launcher Text sheet artifact (https://claude.ai/artifact/KuGyrTcRuCvhzF3hTGybyS) mirrors it.
- **Renderer traces:** the launcher discards the renderer's stderr, so run `tbc.exe` directly with
  `BOTS_DRAW_TRACE=1` (and `BOTS_TRACE_RENDERING=1`), `2> trace.txt`. About 100 MB a minute;
  take the tail with `Get-Content -Tail`.
- Commit messages end with the Co-Authored-By and Claude-Session lines.

## History

The project started on 2026-11-09 (243 commits by 0.3.3). `CHANGELOG.md` has every detail; this is
the story in short.

| Release | Date | What it brought |
|---|---|---|
| 0.1.0 | 11-09 | The game recompiled with ReXGlue SDK 0.10.1, playable through the first levels. Installer that builds the game from the player's own disc image; launcher with display, graphics, camera and controller settings. |
| 0.2.0 | 11-10 | Our own patched SDK build. 60 FPS and higher, frame pacing (from the Table Tennis PC port), FSR/CAS upscaling, VSync, FPS counter, render resolution, post-processing toggles, SDK bundled in the installer, leak-scan git hooks. |
| 0.2.1 | 11-11 | Keyboard and mouse, editable key binds, camera turn speed, mouse sensitivity. ReGPU renderer tried and shelved. |
| 0.2.2 | 11-12 | Much faster above 720p, easier mouse aiming, GPU-wait hook and refresh-rate matching (from WetRecomp), SDK fixes (jump tables, XMA audio, mouse curve). |
| 0.2.3 | 11-13 | **Native Vulkan renderer** (Skullburglar's, `native-plugin/`) as the default; the game's own 2x MSAA turned off (`src/msaa.cpp`) to make that possible; first keyboard prompts; 1920x1080 render resolution; mouse buttons attack in fights. |
| 0.2.4 / -1 | 11-14 | Ultrawide (experimental, native renderer); crash after starting fixed; installer checks the game files; fix for PCs with another Clang; Shift and Ctrl as binds; QTEs on W/A/S/D. |
| 0.2.5 | 11-14 | Keyboard and mouse finished: mouse look, the player's keys in every prompt, keys on HUD badges and QTEs, fixed key layout, launcher update check and rebuild notice. |
| 0.2.6 | 11-15 | PlayStation button prompts, controller swapping mid-game, crash fix for non-English Documents folders. |
| 0.2.7 | 11-15 | More responsive mouse look (direct 1:1, smoothing setting, aiming sensitivity); look further up and down; no auto-recentre; linear stick turning; arrow keys in menus. |
| 0.2.8 | 11-16 | Launcher settings in tabs. |
| 0.3.0 | 11-16 | Quit Game on the main menu; detailed performance overlay and achievement pop-ups drawn by the native renderer (so they show in fullscreen); brightness; vibration strength; Achievements and Saves tabs; game language; prompts in every language. SDK bundle tbc10. |
| 0.3.1 | 11-17 | Installer fixes ("cmake is not recognized"). |
| 0.3.2 | 11-17 | Shooting, multi and boss takedowns on keys; MMB glyph; aim assist checkbox; Quit Game only on the main menu. Known issue: high-fps scripting stalls. |
| 0.3.3 | 11-18 | Launcher in German, French, Spanish, Italian; texture filtering (forced anisotropy, patch 0009); FXAA on the native renderer (0010); clock on the FPS overlay (0007); outlined key glyphs; 1080p render flicker fix (0008); installer adds missing Visual Studio parts; frame rate note; MSAA capability log (0011). SDK bundle tbc11. |

Credits carried in the releases: Skullburglar by nlwstein (native renderer, SDK patches, crash
reporting, leak scanner; BSD 3-Clause), WetRecomp by nikolaygorb (MIT), Table Tennis PC (MIT),
ReGPU and Glue2GPU by zer0ducksgiven (MIT; shelved).

## Unreleased (for 0.3.4)

- **Quit Game under Replay Chapter** (`src/menu_quit.cpp`, 2b97da6). With five chapters unlocked
  the chapter list filled exactly the main menu's rows. Now text left of the labels (each chapter's
  city) rules the main menu out. **Needs testing:** Replay Chapter shows no Quit Game; the main menu
  still shows it under Cheats, and Down from Cheats selects it.
- **MSAA in the native renderer:** planned in `docs/native-msaa.md`, prep done (both traces and the
  GPU's capabilities in), no code yet. Waiting for the go-ahead. Key findings: multisample only
  full-width surfaces (picked by pitch); occlusion queries and the shadow mask are half resolution,
  so untouched; full-resolution stencil is only cleared, so no stencil export needed.

- **Native renderer patch 0012 (tiled frames)**, shared with the Eat Lead port (same SDK build in
  `C:\rexglue-src`). No change for Bourne by default; a test with `--msaa=true` on the native renderer
  would show whether the game's own 2x MSAA strips now work. Keep the `--msaa=false` workarounds and the
  host MSAA plan either way.

- **SDK patch 0012, texture exponent bias** (2026-11-20, from the 007 port): the SPIR-V translator read
  exp_adjust from fetch constant word 4 (inside the LOD bias) instead of word 3. In 007 it made the menus
  invisible on the native renderer. Bourne: SDK tag `tbc12` in `tools/build_sdk.bat` (distinct from Eat
  Lead's own12/own13 so switching projects re-patches the shared SDK), bundle `tbc12`. **To do:** run
  `tools\build_native.bat`, delete the files in `Documents\tbc\cache\native` except `565507D4.manifest.bin`
  (cached pipelines keep the old shader translations; the manifest is only the list to rebuild), and compare
  the native picture with 0.3.3 (look for textures that change brightness). Then build the
  installer and `gh release upload sdk installer\payload\sdk\rexglue-sdk-0.10.1-tbc12-win-amd64.zip` with 0.3.4.

## Open issues and waiting items

| Item | State | Next step |
|---|---|---|
| High-fps scripting stalls (Solomon's warehouse intro, boat wave 2) | Intermittent above 60 FPS | A stuck capture at uncapped with the probe build (`script_trace_filter = "ReenableDamage\|StartBlock\|Takedown"`); then pick a fix (watchdog retry, cap during scripted scenes, fixed 60 Hz logic) |
| Aim assist "still on" (user report) | The checkbox only sets `MaxStrafeSticky=0` (strafe sticky); the snap onto targets when aiming isn't in the config | Ask the reporter: snap when aiming, or camera following while strafing? |
| Movement drift after releasing W/A/S/D, worse when aiming | Input path has no smoothing; likely the game's strafe/stop animation blend-out | Developer compares with a controller while aiming; then a walk modifier key, or measure and shorten the blend |
| Shipped shader list (first-time stutter) | Manifest grows as the developer plays | At the end of a playthrough, ship `565507D4.manifest.bin` as `565507D4.master.bin` |
| Installer "build tools not found" (user report) | Fixed in 0.3.3 (likely cause: existing Visual Studio missing Clang/CMake) | Confirm with the reporter's `install.log` if it recurs |
| Glyph opacity on the QTE diamond | Faded keys at 45% opacity; may look grey with the new outline | Adjust in `src/key_glyphs.cpp` if it looks muddy |
| Full playthrough with keyboard and mouse | Missions 2 and 3 confirmed | Report any prompt whose keys don't respond, with the time |

## Ideas not started

"Collect logs" button and a pre-install check in the launcher; QTE assist (hold instead of mash,
longer window); controller remapping; HUD and subtitle options; alpha-to-coverage after MSAA;
Linux and Steam Deck build; Discord Rich Presence; save backup. Shelved: SMAA (FXAA first),
real frame generation (no motion vectors; driver frame generation recommended instead), Quit Game
in the pause menu, skip intro videos.

## Technical notes

Things learnt the hard way, worth knowing before changing these areas.

**Game objects and QTEs** (`src/qte.cpp`)
- `GObjObjects` is at `0x83655774` (a search for it once settled on the wrong array). `Controller.Pawn` is at +0x1B0.
- QTEs are Kismet `SeqAct_BnQuickAction` nodes handled by `OnBnQuickAction`; the input window is at +0xEC.
- Multi-takedowns use `BnTakedownBranch` (+0x88 play time, +0x98 prompt start). The first takedown
  of each kind in a session, and every one after the boat, needed rescans: `ScanBranches` rescans
  every 250 ms for 8 s after a takedown event.
- Shooting takedowns (`PlayerOverTheShoulder.Takedown`) use `BnTakedownSequencePlayer`: +0x5C is
  non-zero only during one, +0x74 is the stage (0, 2, 4).
- Readable-page cache is per scan only (`ScanBegin`); a stale cache made keys stop working.

**High-fps stalls** (open)
- Solomon's warehouse intro: `SeqAct_PlayFightTakedown_16107` fires but the takedown branch never
  starts (no `ReenableDamage`). Boat finale `Support_Ship_Wave_2`: `SeqAct_Teleport_15892` (enemy onto
  the boat after `PlayAnimation_4`) and `SeqAct_Teleport_11930` (player) never fire uncapped.
- Launcher 120/144/Unlimited set `MaxSmoothedFrameRate` to 1000. Ctrl+F8 (`--frame_rate_test_key`)
  switches between 60 and the set rate; F9 clashed with the screen recorder.

**Renderer**
- `Host(v) = uint32_t(float(v)*scale_+0.5f)`. The SDK's viewport bounds are integers; scaling them
  without rounding left the last row undrawn at render scale 1.5 (the 1080p flicker, patch 0008).
- 1x render resolution can't be 1080p: 1920x1080 needs about 16.6 MB of EDRAM, over the 360's 10 MB.
- The launcher's FSR is `--present_effect fsr`, i.e. FSR 1 (EASU + RCAS). The temporal FSR path
  (`fsr2`/`fsr3`) isn't used. ReXGlue builds FidelityFX without frame generation, and frame generation
  couldn't work anyway: no motion vectors, and the HUD is baked into the image.
- A frame (see `docs/native-msaa.md`): z-prepass, half-resolution occlusion queries and shadow mask,
  three shadow cascades, main colour pass (HDR, fmt 3), decal buffer, translucents, bloom, tone map
  and HUD. Everything at full width shares colour base `0x2D0` (at 720p) and depth base 0.
- The pipeline manifest's record size must not change, or every player's shader list starts over.

**Game config** (`Coalesced.ini`, edited by the launcher through `tools/tbc_config.py`)
- Aim assist checkbox sets `Hm_Engine.HmTargetCirclePositionAdjuster.MaxStrafeSticky` (0.9 → 0).
  The snap onto targets when aiming isn't in the config.
- Movement and strafing are animation-driven (`BnPlayerPawnAnimRates`, `StrafeRateZoomedOut=1.4`);
  there's no movement ramp or dead zone in the config.

**Launcher and installer**
- Translations: the key "Save" has two meanings, separated by `CONTEXT`; CSV imports had leading
  apostrophes once.
- `build_installer.bat` deletes `payload` each run; without `SDK_ZIP` it packs
  `C:\rexglue-src\rexglue-sdk\out\install\win-amd64`.
- The installer finds Visual Studio with `vswhere` (2022+, MSVC x64 + Clang + CMake); missing parts
  are added with the Visual Studio Installer's `modify` (0.3.3). Before that, running the Build Tools
  bootstrapper over an existing install changed nothing.
- In `cmd`, `%PATH%` in one line expands before `vcvars64.bat` runs: tool folders go first on PATH
  in the starting environment, never as `set "PATH=...;%PATH%"` in the command.

## Code map (main pieces)

| File | What it does |
|---|---|
| `src/qte.cpp` | QTEs and takedowns on keys (`OnBnQuickAction`, `BnTakedownBranch`, `BnTakedownSequencePlayer` for shooting takedowns), `--qte_probe` |
| `src/keyboard_prompts.cpp`, `src/key_glyphs.cpp`, `assets/keys/` | Key glyphs in prompts, the QTE diamond |
| `src/mouse_look.cpp` | Mouse look and aiming sensitivity |
| `src/menu_quit.cpp` | Quit Game on the main menu |
| `src/msaa.cpp` | Turns the game's own 2x MSAA off (`--msaa=false`) and clears depth before the main pass |
| `src/fps_unlock.cpp`, `src/frame_pacing.cpp` | Frame rate above 60; Ctrl+F8 test key |
| `src/script_trace.cpp` | `--script_trace_kismet` (level script events) |
| `installer/tbc_launcher.py`, `installer/tbc_installer.py`, `installer/launcher_text.py` | Launcher, installer, translations |
| `native-plugin/patches/` | Our changes to Skullburglar's renderer |
| `tools/tbc_config.py` | Reads and edits the game's `Coalesced.ini` (the launcher's game settings) |
