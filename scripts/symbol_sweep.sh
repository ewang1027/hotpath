#!/usr/bin/env bash
# Runs the design study and structural stats across every tape and emits CSV.
#
# Exists because the headline design finding -- that the textbook intrusive book
# loses on the most active symbol -- was originally measured on four symbols.
# Four points can describe almost any curve. This widens it.
set -euo pipefail
cd "$(dirname "$0")/.."
TAPE_DIR="${HOTPATH_TAPE_DIR:-$HOME/market-data/tapes}"
TRIALS="${TRIALS:-5}"

echo "symbol,events,mean_levels,creates_per_event,shifted_per_event,kb_memmove_per_event,map_ns,intrusive_ns,flat_ns,hybrid_ns,intrusive_vs_map,hybrid_vs_map"
for f in "$TAPE_DIR"/*.tape; do
  s=$(basename "$f" .tape)
  ts=$(./build/src/tape_stat "$f")
  ev=$(awk '/^events /{print $3}' <<<"$ts")
  ml=$(awk '/mean live levels/{print $5}' <<<"$ts")
  cpe=$(awk '/level creates/{gsub(/[()]/,"",$5); print $5}' <<<"$ts")
  kb=$(awk '/bytes memmoved/{gsub(/[()]/,"",$7); print $7}' <<<"$ts")
  spe=$(awk -v c="$cpe" -v m="$ml" 'BEGIN{printf "%.1f", c*m}')

  bb=$(./build/bench/bench_book "$f" --trials "$TRIALS")
  # first table only = replay-only (book maintenance)
  read -r mn <<<"$(awk '/^  map /{print $2; exit}' <<<"$bb")"
  read -r in <<<"$(awk '/^  intrusive /{print $2; exit}' <<<"$bb")"
  read -r fl <<<"$(awk '/^  flat /{print $2; exit}' <<<"$bb")"
  read -r hy <<<"$(awk '/^  hybrid /{print $2; exit}' <<<"$bb")"
  ivm=$(awk -v a="$mn" -v b="$in" 'BEGIN{printf "%.3f", a/b}')
  hvm=$(awk -v a="$mn" -v b="$hy" 'BEGIN{printf "%.3f", a/b}')
  echo "$s,$ev,$ml,$cpe,$spe,$kb,$mn,$in,$fl,$hy,$ivm,$hvm"
done
