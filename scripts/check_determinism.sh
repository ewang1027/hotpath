#!/usr/bin/env bash
# Verifies the bit-exact determinism claim in docs/METHODOLOGY.md.
#
# Replays every tape N times in the optimised build and once in a -O0 build,
# hashing every snapshot of every book design in order. All digests must match:
# same input, same work, regardless of optimisation level or run.
set -euo pipefail
cd "$(dirname "$0")/.."

TAPE_DIR="${HOTPATH_TAPE_DIR:-$HOME/market-data/tapes}"
RUNS="${RUNS:-20}"
SYMS=(AAPL SPY MSFT INTC)

cmake --build build >/dev/null
[[ -d build-debug ]] || cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build build-debug >/dev/null

fail=0
for s in "${SYMS[@]}"; do
  ref=""
  for ((i=0; i<RUNS; i++)); do
    d=$(./build/src/book_crossval "$TAPE_DIR/$s.tape" | awk '/^digest/{print $3}')
    if [[ -z "$ref" ]]; then ref="$d"
    elif [[ "$d" != "$ref" ]]; then
      echo "$s: NONDETERMINISM at -O3 run $i: $d != $ref"; fail=1; break
    fi
  done
  d0=$(./build-debug/src/book_crossval "$TAPE_DIR/$s.tape" | awk '/^digest/{print $3}')
  if [[ "$d0" != "$ref" ]]; then
    echo "$s: -O0 digest $d0 != -O3 digest $ref"; fail=1
  else
    printf '%-6s %s  (%d runs at -O3 + 1 at -O0, all identical)\n' "$s" "$ref" "$RUNS"
  fi
done

echo
if [[ $fail -eq 0 ]]; then echo "DETERMINISM: PASS"; else echo "DETERMINISM: FAIL"; exit 1; fi
