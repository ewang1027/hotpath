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

echo "== lint (-Wall, warnings are errors) =="
verilator --lint-only -Wall -Wno-fatal rtl/itch_parse.sv
echo "clean"

echo; echo "== build =="
verilator --cc rtl/itch_parse.sv --exe ../rtl/tb_itch_parse.cpp \
  -CFLAGS "-std=c++20 -O2 -I$PWD/include -I$PWD/tests" \
  --Mdir build-rtl --build -o tb_itch_parse >/dev/null

echo; echo "== co-simulate =="
if [[ "$BYTES" != "0" && -s "$RAW" ]]; then
  ./build-rtl/tb_itch_parse --file "$RAW" --bytes "$BYTES"
else
  ./build-rtl/tb_itch_parse
fi
