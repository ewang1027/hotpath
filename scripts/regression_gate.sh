#!/usr/bin/env bash
# Local regression gate. Needs the real ITCH data, so it is not part of CI.
#
# Gates on things that are stable enough to threshold:
#   - the full-day parse gate (framing + referential integrity + invariants)
#   - four-way book cross-validation on every tape
#   - zero allocations in the pooled book designs
#   - throughput within a tolerance of the recorded baseline
#
# Throughput thresholds are deliberately loose (default 25%): this machine is a
# laptop running a desktop, and a tighter bound would fail on scheduling noise
# rather than on a real regression.
set -euo pipefail
cd "$(dirname "$0")/.."

TAPE_DIR="${HOTPATH_TAPE_DIR:-$HOME/market-data/tapes}"
DATA_DIR="${HOTPATH_DATA_DIR:-$HOME/market-data/itch}"
DAY="${1:-12302019}"
TOLERANCE="${TOLERANCE:-25}"

# Baseline ns/event for the hybrid design, replay-only (docs/PERFORMANCE.md).
declare -a BASE_SYM=(AAPL SPY MSFT INTC)
declare -a BASE_NS=(15.9 14.6 14.6 12.9)

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

echo; echo "=== throughput regression (hybrid, replay-only, +/-${TOLERANCE}%) ==="
for i in "${!BASE_SYM[@]}"; do
  s="${BASE_SYM[$i]}"; base="${BASE_NS[$i]}"
  got=$(./build/bench/bench_book "$TAPE_DIR/$s.tape" --trials 5 \
        | awk '/^  hybrid/{print $2; exit}')
  [[ -n "$got" ]] || { note "throughput $s" "FAIL (no reading)"; fail=1; continue; }
  verdict=$(awk -v g="$got" -v b="$base" -v t="$TOLERANCE" \
    'BEGIN{ lim=b*(1+t/100); printf (g<=lim ? "PASS" : "FAIL"); }')
  note "throughput $s  ${got} ns/event (base ${base})" "$verdict"
  [[ "$verdict" == PASS ]] || fail=1
done

echo
if [[ $fail -eq 0 ]]; then echo "REGRESSION GATE: PASS"; else echo "REGRESSION GATE: FAIL"; exit 1; fi
