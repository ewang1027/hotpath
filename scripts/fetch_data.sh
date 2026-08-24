#!/usr/bin/env bash
# Download + decompress a NASDAQ TotalView-ITCH 5.0 sample day.
#
# Nasdaq serves these over a flaky connection that drops mid-transfer (curl 56)
# on multi-GB files, so this resumes with -C - and retries until the byte count
# matches Content-Length rather than trusting a single invocation.
set -uo pipefail

DAY="${1:-12302019}"
DATA_DIR="${HOTPATH_DATA_DIR:-$HOME/market-data/itch}"
BASE="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
GZ="$DATA_DIR/${DAY}.NASDAQ_ITCH50.gz"
RAW="$DATA_DIR/${DAY}.NASDAQ_ITCH50"

mkdir -p "$DATA_DIR"

expected=$(curl -sSI -L "$BASE/${DAY}.NASDAQ_ITCH50.gz" \
           | awk 'tolower($1)=="content-length:"{print $2}' | tr -d '\r' | tail -1)
if [[ -z "$expected" ]]; then echo "could not determine remote size" >&2; exit 1; fi
echo "remote size: $expected bytes"

for attempt in $(seq 1 100); do
  have=$(stat -f%z "$GZ" 2>/dev/null || echo 0)
  if [[ "$have" -ge "$expected" ]]; then echo "download complete ($have bytes)"; break; fi
  echo "attempt $attempt: have $have / $expected"
  curl -sS -L -C - --connect-timeout 20 --speed-limit 1024 --speed-time 60 \
       -o "$GZ" "$BASE/${DAY}.NASDAQ_ITCH50.gz" || true
done

have=$(stat -f%z "$GZ" 2>/dev/null || echo 0)
if [[ "$have" -lt "$expected" ]]; then echo "FAILED: incomplete after retries" >&2; exit 1; fi

# Verify against Nasdaq's published md5 before spending 12 GB of disk on it.
curl -sS -L -o "$GZ.md5sum" "$BASE/${DAY}.NASDAQ_ITCH50.gz.md5sum" || true
if [[ -s "$GZ.md5sum" ]]; then
  want=$(awk '{print $1}' "$GZ.md5sum")
  got=$(md5 -q "$GZ")
  if [[ "$want" == "$got" ]]; then echo "md5 OK ($got)"
  else echo "md5 MISMATCH: want=$want got=$got" >&2; exit 1; fi
fi

if [[ ! -s "$RAW" ]]; then
  echo "decompressing -> $RAW"
  gunzip -c "$GZ" > "$RAW"
fi
echo "ready: $RAW ($(stat -f%z "$RAW") bytes)"
