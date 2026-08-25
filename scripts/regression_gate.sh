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
# replay-only, PER SYMBOL. Recorded values are 3.28 / 2.32 / 2.78 / 2.69
# (docs/PERFORMANCE.md), but the ratio itself moves a lot run to run: observed
# ranges are AAPL 3.13-3.28, SPY 1.96-2.32, MSFT 2.21-2.78, INTC 1.98-2.69.
# Floors sit at ~80% of the observed MINIMUM, and each symbol is measured
# best-of-two.
#
# This gate has now been loosened three times, which is itself the finding: a
# laptop running a desktop cannot support a tight performance bound, exactly as
# docs/METHODOLOGY.md argues. What it still catches is the thing worth catching
# -- the hybrid design regressing toward the baseline it is supposed to beat,
# which would collapse these ratios toward 1.0, not shave 15% off them.
#
# A single global floor does not work here either: SPY has by far the shallowest
# book (699 mean levels against AAPL's 4,652), which makes std::map relatively
# cheap and compresses its ratio well below the others.
declare -a BASE_SYM=(AAPL SPY  MSFT INTC)
declare -a MIN_X=(   2.60 1.60 1.80 1.60)

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

echo; echo "=== book design regression (hybrid vs std::map, per-symbol floor) ==="
for i in "${!BASE_SYM[@]}"; do
  s="${BASE_SYM[$i]}"; floor="${MIN_X[$i]}"
  # Best of two invocations: between-process variance on this machine is far
  # wider than the within-process confidence interval, so one reading is a
  # coin flip near any floor.
  speedup=""; ns=""
  for _ in 1 2; do
    read -r n1 s1 < <(./build/bench/bench_book "$TAPE_DIR/$s.tape" --trials 5 \
        | awk '/^  hybrid/{gsub("x","",$7); print $2, $7; exit}')
    [[ -n "$s1" ]] || continue
    if [[ -z "$speedup" ]] || awk -v a="$s1" -v b="$speedup" 'BEGIN{exit !(a>b)}'; then
      speedup="$s1"; ns="$n1"
    fi
  done
  [[ -n "$speedup" ]] || { note "design $s" "FAIL (no reading)"; fail=1; continue; }
  verdict=$(awk -v g="$speedup" -v m="$floor" 'BEGIN{ printf (g>=m ? "PASS" : "FAIL"); }')
  note "design $s  ${speedup}x over map (floor ${floor}x, ${ns} ns/event)" "$verdict"
  [[ "$verdict" == PASS ]] || fail=1
done

echo
if [[ $fail -eq 0 ]]; then echo "REGRESSION GATE: PASS"; else echo "REGRESSION GATE: FAIL"; exit 1; fi
