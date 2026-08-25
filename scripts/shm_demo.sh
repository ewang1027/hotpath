#!/usr/bin/env bash
# Cross-process market data over shared memory.
#
#   ./scripts/shm_demo.sh AAPL [subscriber-slow-spin]
#
# Runs shm_pub and shm_sub as two genuine processes and checks two things:
#   1. the subscriber's fill stream hashes identically to the in-process run,
#      so crossing a process boundary changed nothing;
#   2. every published message is either delivered or counted in a gap -- a
#      feed handler that silently drops messages builds a book that is wrong in
#      a way nothing downstream can detect.
#
# Pass a spin count to handicap the subscriber and watch gaps appear while the
# publisher's rate stays put. That is the design: the producer never blocks.
set -euo pipefail
cd "$(dirname "$0")/.."

SYM="${1:-AAPL}"
SLOW="${2:-0}"
# Real ITCH peaks in the low millions of messages per second. The publisher can
# push ~150 M/s from a memory-mapped tape, which is not a market data rate -- it
# is a memcpy benchmark. Pacing to a realistic rate is what makes the no-gap
# case meaningful rather than a foregone lapping.
PACE="${PACE:-500}"
TAPE_DIR="${HOTPATH_TAPE_DIR:-$HOME/market-data/tapes}"
TAPE="$TAPE_DIR/$SYM.tape"
RING="/tmp/hotpath_${SYM}.ring"
CAP="${CAP:-65536}"

[[ -s "$TAPE" ]] || { echo "missing $TAPE" >&2; exit 1; }

# The subscriber needs the same price window the in-process run uses, or its
# book would be laid out differently and the digests could not be compared.
read -r LO HI < <(./build/src/tape_stat "$TAPE" \
  | awk '/price window/{gsub(/\$/,"",$4); gsub(/\$/,"",$6); printf "%d %d\n", $4*10000+0.5, $6*10000+0.5}')
echo "symbol $SYM   price window [$LO, $HI]   ring capacity $CAP"
echo "publisher pace ${PACE} ns/msg (~$((1000000000/PACE/1000)) k msg/s)   subscriber spin $SLOW"

echo; echo "== in-process reference =="
REF=$(./build/src/pipeline "$TAPE" --trials 1 | awk '/single-threaded *:/{print $3, $6}')
REF_FILLS=$(awk '{print $1}' <<<"$REF"); REF_DIGEST=$(awk '{print $2}' <<<"$REF")
echo "  fills=$REF_FILLS digest=$REF_DIGEST"

echo; echo "== cross-process =="
rm -f "$RING"
./build/src/shm_pub "$TAPE" --ring "$RING" --capacity "$CAP" --wait-readers 1 \
    --pace-ns "$PACE" > /tmp/shm_pub.out 2>&1 &
PUB=$!
# The publisher creates the ring; wait for it to exist before attaching.
for _ in $(seq 1 500); do [[ -s "$RING" ]] && break; sleep 0.01; done
./build/src/shm_sub --ring "$RING" --lo "$LO" --hi "$HI" --slow "$SLOW" > /tmp/shm_sub.out 2>&1 &
SUB=$!
wait $PUB; wait $SUB
cat /tmp/shm_pub.out; cat /tmp/shm_sub.out
rm -f "$RING"

SUB_DIGEST=$(awk '/fills *:/{print $5}' /tmp/shm_sub.out)
MISSED=$(awk '/missed/{print $3}' /tmp/shm_sub.out)
echo
if [[ "$MISSED" != "0" ]]; then
  echo "VERDICT: subscriber was lapped ($MISSED messages) and said so."
  echo "         Digests are not comparable when messages were dropped -- that is the point."
elif [[ "$SUB_DIGEST" == "$REF_DIGEST" ]]; then
  echo "VERDICT: PASS -- fill stream identical across the process boundary ($SUB_DIGEST)"
else
  echo "VERDICT: FAIL -- digest $SUB_DIGEST != in-process $REF_DIGEST"; exit 1
fi
