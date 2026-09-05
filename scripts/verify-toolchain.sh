#!/usr/bin/env bash
set -euo pipefail

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

# Check 1: g++ major version must be exactly 10.
gxx_major=$(g++ -dumpversion | cut -d. -f1)
[ "$gxx_major" = "10" ] || fail "g++ major version is $gxx_major, expected 10"
echo "OK: g++ major version is 10"

# Check 2: a C++20 probe compiles and reports the three feature macros P0 depends on.
probe_dir=$(mktemp -d)
trap 'rm -rf "$probe_dir"' EXIT

cat > "$probe_dir/probe.cpp" <<'EOF'
#include <version>
#if !defined(__cpp_lib_jthread) || !defined(__cpp_lib_span) || !defined(__cpp_concepts)
#error "missing required C++20 feature macro"
#endif
int main() { return 0; }
EOF
g++ -std=c++20 -o "$probe_dir/probe" "$probe_dir/probe.cpp" \
  || fail "C++20 probe failed to compile (missing __cpp_lib_jthread, __cpp_lib_span, or __cpp_concepts)"
echo "OK: C++20 probe compiles with __cpp_lib_jthread, __cpp_lib_span, __cpp_concepts"

# Check 3: CMake must be at least 3.21 (Debian's packaged 3.18 silently runs zero tests).
cmake_version=$(cmake --version | head -1 | awk '{print $3}')
cmake_major=$(echo "$cmake_version" | cut -d. -f1)
cmake_minor=$(echo "$cmake_version" | cut -d. -f2)
if [ "$cmake_major" -lt 3 ] || { [ "$cmake_major" -eq 3 ] && [ "$cmake_minor" -lt 21 ]; }; then
  fail "cmake version $cmake_version is below the 3.21 floor"
fi
echo "OK: cmake version $cmake_version >= 3.21"

# Check 4: setarch must be available (required to run TSan binaries).
command -v setarch >/dev/null 2>&1 || fail "setarch not found on PATH"
echo "OK: setarch is available"
