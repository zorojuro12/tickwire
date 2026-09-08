FROM gcc:10

# Debian 11 ships CMake 3.18, on which `ctest --test-dir` runs zero tests and
# exits 0 - a silent green. The version floor is a correctness requirement.
ARG CMAKE_VERSION=3.28.4
# bullseye-security's Packages index and its pool have drifted out of sync
# (a known Debian archive eventual-consistency gap once a package has been
# superseded): the index lists security-suffixed package versions whose
# .deb files 404 from the pool, and pinning individual packages to the
# plain-repo version just pushes the same conflict onto their transitive
# deps. Disabling the security repo for this build sidesteps it entirely --
# every package this image needs is available in plain bullseye/main, and a
# build-time toolchain image has no runtime exposure that security patches
# would meaningfully cover.
RUN sed -i '/bullseye-security/d' /etc/apt/sources.list \
    && apt-get update && apt-get install -y --no-install-recommends \
      ninja-build util-linux ca-certificates curl git \
      libgl1-mesa-dev libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev \
    && curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
       | tar -xz --strip-components=1 -C /usr/local \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
