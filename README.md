# The Bourne Conspiracy - Xbox 360 Recompilation

A native Windows port of *Robert Ludlum's The Bourne Conspiracy* (Xbox 360, title ID `565507D4`), built by statically recompiling the original game code to C++ with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). It is not an emulator: the game code is translated ahead of time and compiled into a normal Windows executable.

**This repository contains no game files.** You need your own legally obtained copy of the game.

## Status

Early, but playable: boots through the intro videos and main menu into gameplay, tested through the first levels.

## Requirements

- Windows 10/11, x64
- Visual Studio 2026 (or 2022) with **Desktop development with C++** and **C++ Clang tools for Windows**
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk/releases) **v0.10.1**, extracted to e.g. `C:\rexglue-sdk`
- Your own Xbox 360 disc image of the game

## Setup

1. Extract the game from your ISO with [extract-xiso](https://github.com/XboxDev/extract-xiso):
   ```
   extract-xiso -s -d "<path>\tbc-game" "<path>\game.iso"
   ```
2. Place the extracted folder so it matches the paths in `tbc_manifest.toml` (by default `../bourne-conspiracy-x360-port/tbc-game` relative to this repo), or edit `game_root` and `file_path` in the manifest to point at it.

## Build

From an **x64 Native Tools Command Prompt for VS**, in this folder:

```
cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH=C:/rexglue-sdk
cmake --build out\build\win-amd64-relwithdebinfo
```

The first build runs ReXGlue code generation (about 2 minutes) and then compiles the generated code, which takes a while.

## Run

Double-click `run_tbc.bat`, or:

```
out\build\win-amd64-relwithdebinfo\tbc.exe --game_data_root "<path>\tbc-game" --gpu_plugin xenos --log_level debug --log_file tbc_log.txt
```

## How the port works

ReXGlue's analyzer misses some functions, mostly small virtual-function stubs that are only ever called through pointers. These are added by hand:

- `default_functions.toml` - function starts found from codegen errors and runtime crashes.
- `default_functions_scan.toml` - function starts found by scanning the decrypted executable: addresses stored in the game's data (virtual function tables) and code left uncovered between known functions. Every entry is checked to follow a return or jump instruction (or alignment padding), so it can't split an existing function.

If the game crashes with `Call to invalid or unregistered function at guest address 0x...`, that address is another missed function: add it to `default_functions.toml` and rebuild.

## Known issues

- Function `0x825EADC8` is reported as 1.27 MB, which suggests several functions merged into one. Not yet investigated.
- FPS is currently capped at 30fps. This is being investigated.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and the Xenia project, whose work this is built on.
- Function recovery, installer and tooling developed with help from Claude (Anthropic).

## Disclaimer

Not affiliated with or endorsed by Microsoft, Activision, High Moon Studios or the Ludlum estate. All trademarks belong to their respective owners.
