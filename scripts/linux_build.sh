#!/usr/bin/env bash
# Build and test hotpath on Linux inside a container.
#
#   ./scripts/linux_build.sh            # native arch (arm64 here)
#   ARCH=amd64 ./scripts/linux_build.sh # x86-64 under emulation
#
# Verifies the portability layer against a real Linux, rather than trusting the
# #ifdefs. Note the build directory is kept separate from the host's: a CMake
# cache is not portable between platforms and reusing one produces confusing
# failures.
set -euo pipefail
cd "$(dirname "$0")/.."

# macOS ships bash 3.2, where expanding an empty array under `set -u` is an
# error, so the platform flag is a plain string rather than an array.
ARCH="${ARCH:-}"
PLATFORM=""
TAG="hotpath-linux"
if [[ -n "$ARCH" ]]; then PLATFORM="--platform=linux/$ARCH"; TAG="hotpath-linux-$ARCH"; fi

docker build ${PLATFORM} -t "$TAG" -f docker/Dockerfile.linux docker/ >/dev/null

docker run --rm ${PLATFORM} -v "$PWD:/src" -w /src "$TAG" bash -euo pipefail -c '
  echo "== uname =="; uname -m; echo
  cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build-linux -j"$(nproc)" 2>&1 | grep -E "error|warning: unused" || true
  echo "== environment =="
  ./build-linux/src/hotpath_env
  echo
  echo "== tests =="
  ctest --test-dir build-linux --output-on-failure 2>&1 | tail -5
'
