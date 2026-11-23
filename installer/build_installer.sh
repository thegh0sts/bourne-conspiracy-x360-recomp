#!/bin/sh
# Linux counterpart of build_installer.bat: builds dist/BourneConspiracy-linux-x86_64 (launcher + installer, one
# file) from tbc_launcher.py and the project files, in the build image (../tools/linux/Dockerfile), so it runs on
# glibc 2.31 and newer (the Steam Deck included).
#
#   installer/build_installer.sh
#
# Run from the repo in WSL2 (or any Linux) with Docker, after ../tools/linux/build_sdk.sh has put the Linux SDK
# bundle in payload-linux/ (without one, the installer would download the stock SDK, which has no native renderer).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$HERE")
IMAGE=${IMAGE:-ghcr.io/thegh0sts/tbc-build:sniper-1}
if [ "$1" != "--in-container" ]; then
  exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$REPO:$REPO" -w "$HERE" "$IMAGE" \
    sh "$HERE/build_installer.sh" --in-container
fi

cd "$HERE"
echo "Collecting project files into payload/project ..."
rm -rf payload build dist
mkdir -p payload/project/src payload/project/generated payload/project/assets/keys/qte payload/project/assets/ps \
  payload/licenses payload/sdk
for f in tbc_manifest.toml default_functions.toml default_functions_scan.toml hooks.toml CMakeLists.txt CMakePresets.json; do
  cp "../$f" payload/project/
done
cp ../src/* payload/project/src/
cp ../generated/rexglue.cmake payload/project/generated/
cp ../assets/keys/*.png payload/project/assets/keys/ 2>/dev/null || true
cp ../assets/keys/qte/*.png payload/project/assets/keys/qte/ 2>/dev/null || true
cp ../assets/ps/*.png payload/project/assets/ps/ 2>/dev/null || true
cp ../tools/tbc_config.py tbc_config.py
cp ../licenses/*.txt payload/licenses/
cp ../LICENSE payload/licenses/bourne-conspiracy-x360-recomp-LICENSE.txt 2>/dev/null || true
# The Linux SDK bundle (../tools/linux/build_sdk.sh); the newest if there are several.
BUNDLE=$(ls -t payload-linux/rexglue-sdk-*-linux-amd64.zip 2>/dev/null | head -n 1)
if [ -n "$BUNDLE" ]; then
  echo "Using the SDK bundle $BUNDLE"
  cp "$BUNDLE" payload/sdk/
else
  echo "No Linux SDK bundle in payload-linux/: the installer will download the stock ReXGlue SDK."
fi

echo "Building the launcher ..."
pyinstaller --noconfirm --onefile --windowed --name BourneConspiracy-linux-x86_64 \
  --add-data "payload:payload" --add-data "icon.ico:." tbc_launcher.py
echo
echo "Done: $HERE/dist/BourneConspiracy-linux-x86_64"
