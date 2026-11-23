# The Linux build environment for The Bourne Conspiracy: the Linux SDK bundle (build_sdk.sh), the Linux launcher
# (../../installer/build_installer.sh) and, on players' PCs, the game itself (the installer runs its build in this
# image with Podman or Docker). Published as ghcr.io/thegh0sts/tbc-build:sniper-<n> by publish_image.sh; the
# installer's LINUX_BUILD_IMAGE names the tag it uses.
#
# Steam Runtime 3 "sniper" SDK (Debian 11 base, glibc 2.31): what's built here runs on any distro with glibc 2.31
# or newer and on the Steam Deck. From Skullburglar's docker/linux-steamrt/Dockerfile
# (https://github.com/nlwstein/skullburglar, BSD 3-Clause): sniper's own clang is 11, so clang 21 comes from
# apt.llvm.org; C++23 headers come from sniper's gcc-14, which ships only static libraries, so libstdc++ is linked
# statically. Added here: a CMake new enough for the SDK (3.25+), and Tk and PyInstaller for the launcher.
FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest

ENV DEBIAN_FRONTEND=noninteractive

RUN wget -qO /etc/apt/trusted.gpg.d/apt.llvm.org.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
 && echo "deb http://apt.llvm.org/bullseye/ llvm-toolchain-bullseye-21 main" > /etc/apt/sources.list.d/llvm.list \
 && apt-get update && apt-get install -y --no-install-recommends clang-21 lld-21 llvm-21 \
    curl git xz-utils zip unzip ninja-build python3 python3-pip python3-tk python3-venv \
 && rm -rf /var/lib/apt/lists/*

# clang/clang++ wrappers: gcc-14's headers and static libstdc++ first, then gcc-10's dir for libgcc_s/libgcc_eh.
RUN G=/usr/lib/gcc-14/lib/gcc/x86_64-linux-gnu/14 \
 && for t in clang clang++; do \
      printf '#!/bin/sh\nexec /usr/bin/%s-21 --gcc-install-dir=%s -L%s -L/usr/lib/gcc/x86_64-linux-gnu/10 -static-libstdc++ -fuse-ld=lld -pthread -Wno-unused-command-line-argument "$@"\n' \
        "$t" "$G" "$G" > /usr/local/bin/$t && chmod +x /usr/local/bin/$t; \
    done \
 && ln -sf /usr/bin/ld.lld-21 /usr/local/bin/ld.lld \
 && for t in llvm-ar llvm-ranlib llvm-nm llvm-objcopy llvm-strip; do ln -sf /usr/bin/$t-21 /usr/local/bin/$t; done

# CMake from Kitware (sniper's is older than the SDK's minimum, 3.25).
ARG CMAKE_VERSION=3.31.6
RUN curl -fsSL https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz \
    | tar -xz -C /opt \
 && ln -sf /opt/cmake-${CMAKE_VERSION}-linux-x86_64/bin/cmake /usr/local/bin/cmake \
 && ln -sf /opt/cmake-${CMAKE_VERSION}-linux-x86_64/bin/ctest /usr/local/bin/ctest

# PyInstaller for the Linux launcher (installer/build_installer.sh), in its own venv.
RUN python3 -m venv /opt/pyinstaller && /opt/pyinstaller/bin/pip install --no-cache-dir pyinstaller \
 && ln -sf /opt/pyinstaller/bin/pyinstaller /usr/local/bin/pyinstaller

ENV CC=/usr/local/bin/clang CXX=/usr/local/bin/clang++
