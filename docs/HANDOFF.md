# Handoff

Where the project stands, how it's worked on, and what's open. Update it at the end of each
working session. Last updated: 2026-11-18, after the 0.3.3 release.

## Where things are

| What | Where |
|---|---|
| Repository | `thegh0sts/bourne-conspiracy-x360-recomp`, branch `main` |
| Developer's test clone | `C:\Users\kshla\source\repos\tbc-test` |
| Test install (0.3.3) | `F:\Retail Games\tbc4\BourneConspiracyPC` (`game.log` and `install.log` at its root, `tbc.toml` in the build folder `project\out\build\win-amd64-relwithdebinfo`) |
| Launcher settings | `%LOCALAPPDATA%\BourneConspiracyPC\settings.json`; `launcher.json` there names the install the launcher uses |
| Native shader list (pipeline manifest) | `Documents\tbc\cache\native\565507D4.manifest.bin` (3,101 pipelines on 2026-11-18) |
| SDK build output | `C:\rexglue-src\rexglue-sdk\out\install\win-amd64` (`bin\rexgpu-nativerd.dll` is the RelWithDebInfo native renderer) |
| SDK bundles | GitHub release `sdk` ("SDK bundles"): `rexglue-sdk-0.10.1-tbc10-win-amd64.zip`, `...-tbc11-...` |
| Skullburglar (native renderer) | pinned at `67aacb28670077e839f84931dee9b162526bbdb2`; our patches `native-plugin/patches/0001`-`0011`, applied by `tools/build_sdk.bat` after a hard reset of its checkout |

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

## Released

- **0.3.3** (2026-11-18): launcher in German, French, Spanish, Italian; texture filtering (forced
  anisotropy, patch 0009); FXAA on the native renderer (0010); clock on the FPS overlay (0007);
  outlined key glyphs; 1080p render flicker fix (0008); installer adds missing Visual Studio parts;
  frame rate note; MSAA capability log (0011). SDK bundle tbc11.
- **0.3.2** (2026-11-17): keyboard and mouse for shooting, multi and boss takedowns; MMB glyph;
  aim assist checkbox; Quit Game only on the main menu.
- Older: see `CHANGELOG.md`.

## Unreleased (for 0.3.4)

- **Quit Game under Replay Chapter** (`src/menu_quit.cpp`, 2b97da6). With five chapters unlocked
  the chapter list filled exactly the main menu's rows. Now text left of the labels (each chapter's
  city) rules the main menu out. **Needs testing:** Replay Chapter shows no Quit Game; the main menu
  still shows it under Cheats, and Down from Cheats selects it.
- **MSAA in the native renderer:** planned in `docs/native-msaa.md`, prep done (both traces and the
  GPU's capabilities in), no code yet. Waiting for the go-ahead. Key findings: multisample only
  full-width surfaces (picked by pitch); occlusion queries and the shadow mask are half resolution,
  so untouched; full-resolution stencil is only cleared, so no stencil export needed.

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
