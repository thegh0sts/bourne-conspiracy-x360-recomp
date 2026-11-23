#!/bin/sh
# Builds the Linux build image (Dockerfile here) and pushes it to the GitHub container registry, where the
# installer pulls it from (LINUX_BUILD_IMAGE in installer/tbc_installer.py). Run once, and again only when the
# Dockerfile changes: then bump TAG here and LINUX_BUILD_IMAGE together, so old installers keep their image.
#
# Needs Docker (WSL2: Docker Desktop with WSL integration, or docker.io inside Ubuntu) and a GitHub login for
# ghcr.io: a personal access token (classic) with write:packages, then
#   echo <token> | docker login ghcr.io -u thegh0sts --password-stdin
# After the first push, make the package public on GitHub (Packages, tbc-build, Package settings), so players
# can pull it without logging in.
set -e
TAG=${TAG:-ghcr.io/thegh0sts/tbc-build:sniper-1}
cd "$(dirname "$0")"
docker build -t "$TAG" .
docker push "$TAG"
echo "Pushed $TAG"
