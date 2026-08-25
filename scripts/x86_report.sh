#!/usr/bin/env bash
# The measurements whose answers should DIFFER between Apple Silicon and x86.
#
# Run this on an x86-64 Linux box (WSL2 is fine) and compare against the macOS
# figures printed alongside. See docs/PORTING.md for why each one matters.
set -euo pipefail
cd "$(dirname "$0")/.."

[[ -x build/src/hotpath_env ]] || { echo "build first: cmake -S . -B build && cmake --build build" >&2; exit 1; }

echo "############ 1. the clock ############"
echo "macOS/arm64 reference: 41.7 ns effective resolution, 90% of back-to-back"
echo "reads return a delta of zero. That is why this repo publishes no p99."
echo
./build/src/hotpath_env

echo
echo "############ 2. memory ordering -- the headline ############"
echo "macOS/arm64 reference: the relaxed ring reports ~1463 premature + ~544 torn"
echo "reads per 10.5M messages. x86-64 is Total Store Order and should report"
echo "EXACTLY ZERO -- not fewer, zero. Same source, opposite result, because of"
echo "the ISA rather than the program."
echo
./build/src/litmus_ring --rounds 10

echo
echo "############ 3. false sharing ############"
echo "macOS/arm64 reference: padding the ring indices apart showed NO measurable"
echo "benefit (0.89-0.92x), hypothesised to be the shared L2 on Apple's P-core"
echo "cluster. x86 cores have private L2s, so padding should start to matter."
echo
./build/bench/bench_ring --trials 12 --messages 20000000

echo
echo "############ 4. correctness, unchanged everywhere ############"
ctest --test-dir build 2>&1 | tail -3
