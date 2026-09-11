#!/usr/bin/env bash
set -euo pipefail
set -o pipefail

echo "==> plain"
cmake -S . -B build/plain -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/plain -j8
ctest --test-dir build/plain --output-on-failure

echo "==> asan"
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTW_SANITIZER=address,undefined
cmake --build build/asan -j8
ctest --test-dir build/asan --output-on-failure

echo "==> tsan"
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTW_SANITIZER=thread
cmake --build build/tsan -j8
setarch -R ctest --test-dir build/tsan --output-on-failure

echo "==> toolchain"
bash scripts/verify-toolchain.sh
