#!/usr/bin/env bash
# Build and test hotpath on Linux inside a container.
#
#   ./scripts/linux_build.sh            # native arch (arm64 here)
#   ARCH=amd64 ./scripts/linux_build.sh # x86-64 under emulation
#
# Verifies the portability layer against a real Linux, rather than trusting the
# #ifdefs.
#
# The build directory is per-architecture, and separate from the host's. A CMake
# cache is not portable between platforms, and neither are the FetchContent
# dependencies inside it: sharing one directory between arm64 and amd64 runs
# made the x86 link pick up an arm64 libCatch2.a and fail with "file in wrong
# format", which looks exactly like a portability bug and is not one.
set -euo pipefail
cd "$(dirname "$0")/.."

# macOS ships bash 3.2, where expanding an empty array under `set -u` is an
# error, so the platform flag is a plain string rather than an array.
ARCH="${ARCH:-}"
PLATFORM=""
TAG="hotpath-linux"
BUILD="build-linux"
EXTRA=""
if [[ -n "$ARCH" ]]; then
  PLATFORM="--platform=linux/$ARCH"
  TAG="hotpath-linux-$ARCH"
  BUILD="build-linux-$ARCH"
  # Under qemu the emulated g++ segfaults partway through building Catch2, so
  # the test suite cannot be built there. That is an emulation defect, not a
  # portability one -- every hotpath source compiles and every tool runs. Real
  # x86-64 validation happens on native hardware: CI's ubuntu-latest runner and
  # docs/PORTING.md's instructions for a local box.
  EXTRA="-DBUILD_TESTING=OFF"
  echo "note: cross-arch run -- tests are OFF (the emulated compiler crashes on"
  echo "      Catch2). This verifies that hotpath itself compiles and runs for"
  echo "      linux/$ARCH; the suite runs for real on native x86 in CI."
  echo
fi

docker build ${PLATFORM} -t "$TAG" -f docker/Dockerfile.linux docker/ >/dev/null

docker run --rm ${PLATFORM} -v "$PWD:/src" -w /src "$TAG" bash -euo pipefail -c '
  echo "== uname =="; uname -m; echo
  cmake -S . -B '"$BUILD"' -DCMAKE_BUILD_TYPE=Release '"$EXTRA"' >/dev/null
  # Surface warnings, not just errors. This filter used to match "error:" only,
  # which is how three gcc-only diagnostics -- a dead store, a PRIu64 mismatch
  # that is wrong only on LP64, and a volatile deprecation -- stayed invisible
  # on the platform that could see them. _deps is Catch2 and is not ours.
  cmake --build '"$BUILD"' -j"$(nproc)" 2>&1 \
    | grep -E "error:|warning:|Error" | grep -v "_deps/" || true
  echo "== environment =="
  ./'"$BUILD"'/src/hotpath_env
  echo
  if [ -x '"$BUILD"'/tests/hotpath_tests ]; then
    echo "== tests =="
    ctest --test-dir '"$BUILD"' --output-on-failure 2>&1 | tail -5
  else
    echo "== tools built and runnable (tests not built) =="
    ls '"$BUILD"'/src | tr "\n" " "; echo
    ./'"$BUILD"'/src/itch_stat 2>&1 | head -1 || true
    # litmus_ring runs, but its RESULT is meaningless under emulation: qemu does
    # not faithfully reproduce x86 memory ordering, so a clean relaxed run here
    # would be evidence of the emulator rather than of TSO. Real hardware only.
    echo "(litmus_ring built; its memory-ordering result is only meaningful on real x86)"
  fi
'
