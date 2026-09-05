FROM gcc:10

# Debian 11 ships CMake 3.18, on which `ctest --test-dir` runs zero tests and
# exits 0 - a silent green. The version floor is a correctness requirement.
ARG CMAKE_VERSION=3.28.4
RUN apt-get update && apt-get install -y --no-install-recommends \
      ninja-build util-linux ca-certificates curl git \
    && curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
       | tar -xz --strip-components=1 -C /usr/local \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
