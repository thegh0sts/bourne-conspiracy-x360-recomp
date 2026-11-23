# Linux build (experimental)

The Linux version works like the Windows one: the installer reads the player's own disc image and builds the
game on their PC. The difference is where the compiler comes from. On Linux the build runs inside a container
(Podman, or Docker) from a published image, the Steam Runtime 3 "sniper" SDK with clang 21. So players install
nothing but Podman (SteamOS 3.5+ already has it), every PC builds with exactly the toolchain the SDK bundle was
built with, and the game runs on any distro with glibc 2.31 or newer and on the Steam Deck.

| Piece | Where |
|---|---|
| Build image | `tools/linux/Dockerfile`, published as `ghcr.io/thegh0sts/tbc-build:sniper-1` (`tools/linux/publish_image.sh`); the installer's `LINUX_BUILD_IMAGE` names the tag |
| Linux SDK bundle | `tools/linux/build_sdk.sh` → `installer/payload-linux/rexglue-sdk-0.10.1-tbcNN-linux-amd64.zip` (same patches as Windows, plus Skullburglar's Linux fixes 0012, 0013, 0014, 0022) |
| Launcher / installer | `installer/build_installer.sh` → `installer/dist/BourneConspiracy-linux-x86_64` |
| Game | built on the player's PC into `~/.local/share/BourneConspiracyPC/project/out/build/linux-amd64-relwithdebinfo/tbc`; saves in `~/.local/share/tbc` |

What differs from Windows: the native renderer only (Xenos is Direct3D 12 here), no FSR / CAS (FidelityFX is
built for Direct3D 12), no separate FPS window (the native renderer draws its own counter). A menu entry and
desktop shortcut (`bourne-conspiracy.desktop`) start the launcher; `Play.sh` in the install folder starts the
game directly.

## Building it (WSL2 on the Windows PC)

1. **WSL2 and Docker**, once. In PowerShell: `wsl --install -d Ubuntu`, restart, open Ubuntu. Then:
   ```
   sudo apt update && sudo apt install -y docker.io git python3
   sudo usermod -aG docker $USER
   ```
   Close Ubuntu, run `wsl --shutdown` in PowerShell, open Ubuntu again.
2. **The repo**, inside WSL's own disk (much faster than `/mnt/c`):
   ```
   git clone https://github.com/thegh0sts/bourne-conspiracy-x360-recomp ~/tbc
   ```
3. **The build image**, once (and again only when `tools/linux/Dockerfile` changes: then bump the tag in
   `publish_image.sh` and `LINUX_BUILD_IMAGE` together). A GitHub personal access token (classic) with
   `write:packages`, then:
   ```
   echo <token> | docker login ghcr.io -u thegh0sts --password-stdin
   ~/tbc/tools/linux/publish_image.sh
   ```
   On GitHub, make the `tbc-build` package public (Packages, tbc-build, Package settings), so players can pull it.
4. **The SDK bundle** (whenever the SDK or renderer patches change, like `tools\build_native.bat`):
   `~/tbc/tools/linux/build_sdk.sh`
5. **The launcher**: `~/tbc/installer/build_installer.sh`
6. **Compile check**: run `~/tbc/installer/dist/BourneConspiracy-linux-x86_64` (WSLg shows its window) and install
   from the disc image (e.g. `/mnt/c/Users/<you>/...iso`). It builds with Docker exactly as a player's PC would.
   Starting the game in WSL may not work (graphics), but a finished build is the check.
7. **Release** (a beta, as its own pre-release, so the Windows launchers' update check keeps pointing at the
   Windows release):
   ```
   gh release upload sdk installer/payload-linux/rexglue-sdk-0.10.1-tbcNN-linux-amd64.zip
   gh release create vX.Y.Z-linux-beta1 installer/dist/BourneConspiracy-linux-x86_64 --prerelease --title "Linux beta 1" --notes "..."
   ```

## For testers

Download `BourneConspiracy-linux-x86_64`, make it executable (`chmod +x`, or Properties, Permissions), run it.
Bug reports: the launcher's **Save crash report** zips the game log, the install log (the build's output) and the launcher settings onto the desktop.
