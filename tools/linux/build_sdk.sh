#!/bin/sh
# Builds the Linux SDK bundle: the ReXGlue SDK v0.10.1 with Skullburglar's patches (the Windows set from
# tools/build_sdk.bat plus Skullburglar's Linux fixes), ours (tools/sdk-patches) and Skullburglar's native renderer
# with our patches (native-plugin/patches), zipped as the installer packs it. Linux counterpart of
# tools/build_native.bat, run in the build image (Dockerfile here) so the bundle matches what players build with.
#
#   tools/linux/build_sdk.sh [work folder]        (default ~/rexglue-src-linux)
#
# Run from the repo in WSL2 (or any Linux) with Docker. Needs the image: publish_image.sh, or set IMAGE to a
# local tag. The zip ends up in installer/payload-linux/ (installer/build_installer.sh packs it); upload it to the
# "sdk" release next to the Windows bundle.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${1:-$HOME/rexglue-src-linux}
IMAGE=${IMAGE:-ghcr.io/thegh0sts/tbc-build:sniper-1}
mkdir -p "$WORK"
docker run --rm -it --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$REPO:$REPO" -v "$WORK:$WORK" -w "$REPO" "$IMAGE" \
  sh "$REPO/tools/linux/build_sdk_in_container.sh" "$REPO" "$WORK"
