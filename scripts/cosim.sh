#!/usr/bin/env bash
# Build and run the RTL/C++ co-simulation.
#
#   ./scripts/cosim.sh                 # synthetic streams only
#   ./scripts/cosim.sh 60000000        # plus this many bytes of real ITCH
#
# Needs Verilator (brew install verilator). It is not part of the normal build
# because it is the only thing in the repo with that dependency, and a C++
# project that will not configure without an RTL simulator installed is a
# nuisance.
set -euo pipefail
cd "$(dirname "$0")/.."

BYTES="${1:-0}"
DATA_DIR="${HOTPATH_DATA_DIR:-$HOME/market-data/itch}"
DAY="${DAY:-12302019}"
RAW="$DATA_DIR/${DAY}.NASDAQ_ITCH50"

command -v verilator >/dev/null || { echo "verilator not found: brew install verilator" >&2; exit 1; }

echo "== lint (-Wall) =="
verilator --lint-only -Wall -DHOTPATH_ASSERT rtl/itch_parse.sv
echo "clean"

echo; echo "== build =="
# Rebuild from scratch whenever a source is newer than the binary. Verilator's
# incremental build did not always pick up an edited .sv here, which is worse
# than slow: a validation run that silently exercises the previous RTL will
# happily report PASS on code that no longer exists.
if [[ ! -x build-rtl/tb_itch_parse ]] \
   || [[ -n "$(find rtl -newer build-rtl/tb_itch_parse -name '*.sv' -o -newer build-rtl/tb_itch_parse -name '*.cpp' 2>/dev/null)" ]]; then
  rm -rf build-rtl
fi
verilator --cc rtl/itch_parse.sv --exe ../rtl/tb_itch_parse.cpp --assert -DHOTPATH_ASSERT \
  -CFLAGS "-std=c++20 -O2 -I$PWD/include -I$PWD/tests" \
  --Mdir build-rtl --build -o tb_itch_parse >/dev/null

echo; echo "== co-simulate =="
FUZZ="${FUZZ:-5000}"
if [[ "$BYTES" != "0" && -s "$RAW" ]]; then
  ./build-rtl/tb_itch_parse --fuzz "$FUZZ" --file "$RAW" --bytes "$BYTES"
else
  ./build-rtl/tb_itch_parse --fuzz "$FUZZ"
fi
