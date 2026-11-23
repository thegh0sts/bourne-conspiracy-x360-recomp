#!/bin/sh
# Inside the build image (build_sdk.sh starts it): $1 = this repo, $2 = the work folder.
set -e
REPO=$1
WORK=$2
SDK=$WORK/rexglue-sdk
SKULL=$WORK/skullburglar
BAT=$REPO/tools/build_sdk.bat
# The same SDK version, Skullburglar commit and patch list as Windows (tools/build_sdk.bat), read from there so
# they can't drift apart.
SDK_TAG=$(sed -n 's/^set "SDK_TAG=\(.*\)"\r*$/\1/p' "$BAT")
SKULL_COMMIT=$(sed -n 's/^set "SKULL_COMMIT=\(.*\)"\r*$/\1/p' "$BAT")
PATCHES=$(sed -n 's/^set "PATCHES=\(.*\)"\r*$/\1/p' "$BAT")
# Skullburglar's Linux fixes, which a Windows build doesn't need: write watches without parsing /proc/self/maps,
# RenderDoc only when attached, waiting on several objects without spinning, unlinking guest memory.
LINUX_PATCHES="0012 0013 0014 0022"
# The bundle's name: the Windows one's (installer/build_installer.bat SDK_BUNDLE), for Linux.
BUNDLE=$(sed -n 's/^set "SDK_BUNDLE=\(.*\)"\r*$/\1/p' "$REPO/installer/build_installer.bat" | sed 's/win-amd64/linux-amd64/')
echo "SDK $SDK_TAG, Skullburglar $SKULL_COMMIT, patches $PATCHES + $LINUX_PATCHES, bundle $BUNDLE"

echo "=== 1/4 Getting the SDK source ($SDK_TAG)"
if [ ! -d "$SDK/.git" ]; then
  git clone --recursive --branch "$SDK_TAG" https://github.com/rexglue/rexglue-sdk "$SDK"
fi
git -C "$SDK" reset --hard --quiet
git -C "$SDK" clean -fdq -- include src cmake
git -C "$SDK" submodule update --init --recursive

echo "=== 2/4 Getting Skullburglar ($SKULL_COMMIT) and the native renderer's patches"
if [ ! -d "$SKULL/.git" ]; then
  git clone --filter=blob:none https://github.com/nlwstein/skullburglar "$SKULL"
fi
git -C "$SKULL" -c advice.detachedHead=false checkout --quiet --force "$SKULL_COMMIT"
git -C "$SKULL" clean -fdq -- src
for f in "$REPO"/native-plugin/patches/*.patch; do
  git -C "$SKULL" apply "$f"
  echo "$(basename "$f") applied to the native renderer"
done

echo "=== 3/4 Applying the SDK patches"
for p in $PATCHES $LINUX_PATCHES; do
  f=$(ls "$SKULL"/patches/rexglue-sdk/"$p"-*.patch)
  git -C "$SDK" apply "$f"
  echo "$p applied"
done
for f in "$REPO"/tools/sdk-patches/*.patch; do
  git -C "$SDK" apply "$f"
  echo "$(basename "$f") applied"
done

echo "=== 4/4 Building and installing (RelWithDebInfo and Release)"
cd "$SDK"
cmake --preset linux-amd64 -DREXGLUE_USE_VULKAN=ON -DREXGLUE_EXTRA_GPU_PLUGINS="$REPO/native-plugin" \
  -DTBC_SKULL_DIR="$SKULL" -DTBC_N1FFI_LIBRARY=
cmake --build out/build/linux-amd64 --config RelWithDebInfo --target install --parallel
cmake --build out/build/linux-amd64 --config Release --target install --parallel

OUT=$REPO/installer/payload-linux
mkdir -p "$OUT"
python3 - "$SDK/out/install/linux-amd64" "$OUT/$BUNDLE" <<'PY'
import os, sys, zipfile
src, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(src):
        for f in files:
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, src))  # keeps the Unix permissions (bin/rexglue stays executable)
print("Wrote", out)
PY
echo "Done: $OUT/$BUNDLE"
