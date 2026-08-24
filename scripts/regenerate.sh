#!/usr/bin/env bash
# Regenerates every number quoted in docs/. Run this after any change that could
# move them, and update the docs from its output.
#
# The one lesson carried over from simplified_gto_solver: throwaway benchmark
# scripts that never get committed leave stale numbers in the docs that nobody
# notices.
set -euo pipefail
cd "$(dirname "$0")/.."

DATA_DIR="${HOTPATH_DATA_DIR:-$HOME/market-data/itch}"
TAPE_DIR="${HOTPATH_TAPE_DIR:-$HOME/market-data/tapes}"
DAY="${1:-12302019}"
RAW="$DATA_DIR/${DAY}.NASDAQ_ITCH50"
SYMS=(AAPL SPY MSFT INTC)
TRIALS="${TRIALS:-7}"

[[ -s "$RAW" ]] || { echo "missing $RAW -- run scripts/fetch_data.sh $DAY" >&2; exit 1; }
cmake --build build >/dev/null

echo "===== environment (docs/METHODOLOGY.md) ====="
./build/src/hotpath_env

echo; echo "===== full-day parse gate (docs/BUILDLOG.md) ====="
./build/src/itch_stat "$RAW"

echo; echo "===== tape extraction ====="
mkdir -p "$TAPE_DIR"
args=(); for s in "${SYMS[@]}"; do args+=(--symbol "$s"); done
./build/src/extract_tape "$RAW" "${args[@]}" --out "$TAPE_DIR"

echo; echo "===== cross-validation gate (docs/PERFORMANCE.md finding 4) ====="
for s in "${SYMS[@]}"; do ./build/src/book_crossval "$TAPE_DIR/$s.tape"; done

echo; echo "===== design study (docs/PERFORMANCE.md) ====="
for s in "${SYMS[@]}"; do ./build/bench/bench_book "$TAPE_DIR/$s.tape" --trials "$TRIALS"; done

echo; echo "===== tape structure: depth, churn, inter-event timing ====="
for s in "${SYMS[@]}"; do ./build/src/tape_stat "$TAPE_DIR/$s.tape"; done

echo; echo "===== SPSC ring: memory ordering + false sharing ====="
./build/src/litmus_ring --rounds 10
./build/bench/bench_ring --trials 12 --messages 20000000

echo; echo "===== fills and adverse selection (docs/ADVERSE-SELECTION.md) ====="
for s in "${SYMS[@]}"; do ./build/src/sim_mm "$TAPE_DIR/$s.tape" --size 100; done

echo; echo "===== latency sweep (docs/ADVERSE-SELECTION.md result 4) ====="
for s in "${SYMS[@]}"; do ./build/src/latency_sweep "$TAPE_DIR/$s.tape" --size 100; done

echo; echo "===== threaded pipeline (docs/PERFORMANCE.md) ====="
for s in "${SYMS[@]}"; do ./build/src/pipeline "$TAPE_DIR/$s.tape" --trials 5; done

echo; echo "===== determinism ====="
RUNS=5 ./scripts/check_determinism.sh

echo; echo "===== cross-sectional study, every tape (docs/PERFORMANCE.md finding 1) ====="
# The core four symbols above carry the detailed reports; this widens the design
# study across every extracted tape, which is what the r=-0.927 predictor and the
# "7 of 25 lose to std::map" claim rest on. Extract more tapes first with:
#   ./build/src/extract_tape <raw> --symbol X --symbol Y ... --out "$TAPE_DIR"
./scripts/symbol_sweep.sh | tee /tmp/hotpath_sweep.csv

echo; echo "===== figures (docs/img) ====="
HOTPATH_SWEEP_CSV=/tmp/hotpath_sweep.csv ./scripts/make_plots.py
