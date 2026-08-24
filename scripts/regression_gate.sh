#!/usr/bin/env bash
# Local regression gate. Needs the real ITCH data, so it is not part of CI.
#
# Gates on things that are stable enough to threshold:
#   - the full-day parse gate (framing + referential integrity + invariants)
#   - four-way book cross-validation on every tape
#   - zero allocations in the pooled book designs
#   - the hybrid book's SPEEDUP over the std::map baseline
#
# Note what is gated and what is not. An absolute ns/event bound is not
# supportable here: across invocations the same binary on the same tape has
# ranged 12.6-17.5 ns/event on INTC, far wider than the within-process
# confidence interval, because this is a laptop running a desktop. Gating on it
# produces a flapping build, not a signal.
#
# The RATIO between two designs measured in the same process is stable, because
# both sides absorb the same thermal and scheduling noise. That is also exactly
# the class of claim docs/METHODOLOGY.md says this hardware supports, so the
# gate and the documentation agree on what a number here is worth.
set -euo pipefail
cd "$(dirname "$0")/.."

TAPE_DIR="${HOTPATH_TAPE_DIR:-$HOME/market-data/tapes}"
DATA_DIR="${HOTPATH_DATA_DIR:-$HOME/market-data/itch}"
DAY="${1:-12302019}"


# Minimum acceptable speedup of the hybrid book over the std::map baseline,
# replay-only. Recorded values are 3.28 / 2.32 / 2.78 / 2.69 (docs/PERFORMANCE.md),
# so a floor of 2.0 leaves real margin while still catching a design regression.
declare -a BASE_SYM=(AAPL SPY MSFT INTC)
MIN_SPEEDUP="${MIN_SPEEDUP:-2.0}"

fail=0
note() { printf '%-46s %s\n' "$1" "$2"; }

cmake --build build >/dev/null

echo "=== full-day parse gate ==="
if ./build/src/itch_stat "$DATA_DIR/${DAY}.NASDAQ_ITCH50" >/tmp/gate_parse.txt 2>&1; then
  note "full-day parse" "PASS"
else
  note "full-day parse" "FAIL"; tail -20 /tmp/gate_parse.txt; fail=1
fi

echo; echo "=== four-way cross-validation ==="
for s in "${BASE_SYM[@]}"; do
  if ./build/src/book_crossval "$TAPE_DIR/$s.tape" >/tmp/gate_cv.txt 2>&1; then
    note "crossval $s" "PASS"
  else
    note "crossval $s" "FAIL"; tail -20 /tmp/gate_cv.txt; fail=1
  fi
done

echo; echo "=== book design regression (hybrid vs std::map, floor ${MIN_SPEEDUP}x) ==="
for s in "${BASE_SYM[@]}"; do
  read -r ns speedup < <(./build/bench/bench_book "$TAPE_DIR/$s.tape" --trials 5 \
      | awk '/^  hybrid/{gsub("x","",$7); print $2, $7; exit}')
  [[ -n "$speedup" ]] || { note "design $s" "FAIL (no reading)"; fail=1; continue; }
  verdict=$(awk -v g="$speedup" -v m="$MIN_SPEEDUP" 'BEGIN{ printf (g>=m ? "PASS" : "FAIL"); }')
  note "design $s  ${speedup}x over map  (${ns} ns/event)" "$verdict"
  [[ "$verdict" == PASS ]] || fail=1
done

echo
if [[ $fail -eq 0 ]]; then echo "REGRESSION GATE: PASS"; else echo "REGRESSION GATE: FAIL"; exit 1; fi
