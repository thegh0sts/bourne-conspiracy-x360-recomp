#!/bin/sh
# Run inside a git checkout (build_sdk.bat runs it in every SDK submodule).
#
# Without Windows symlink support, Git for Windows checks a symbolic link out as a small text
# file holding the link's target path. libmspack, which the SDK uses for XEX decompression,
# keeps its sources behind such links, so the compiler sees "../../libmspack/mspack/lzxd.c"
# instead of C code. This replaces each such stub with a copy of the file it points to.
# Safe to run again: a replaced file no longer names an existing path.
git ls-files -s | while read -r mode obj stage path; do
  [ "$mode" = "120000" ] || continue
  [ -f "$path" ] || continue
  target=$(cat "$path")
  src="$(dirname "$path")/$target"
  if [ -f "$src" ]; then
    cp "$src" "$path" && echo "  linked file restored: $path"
  fi
done
