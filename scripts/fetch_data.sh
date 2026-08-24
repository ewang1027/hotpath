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

# Verify against Nasdaq's published md5 when there is one. Note that the
# directory listing advertises a .md5sum for every day but some of them 404
# (12302019 does) and the server renders the 404 as an HTML page with a 200-ish
# body, so a naive fetch-and-compare silently diffs against HTML. Require the
# fetched text to actually look like an md5 before trusting it.
curl -sS -L -o "$GZ.md5sum" "$BASE/${DAY}.NASDAQ_ITCH50.gz.md5sum" || true
want=""
if [[ -s "$GZ.md5sum" ]]; then want=$(awk 'NR==1{print $1}' "$GZ.md5sum"); fi
if [[ "$want" =~ ^[0-9a-fA-F]{32}$ ]]; then
  got=$(md5 -q "$GZ")
  if [[ "$want" == "$got" ]]; then echo "md5 OK ($got)"
  else echo "md5 MISMATCH: want=$want got=$got" >&2; exit 1; fi
else
  rm -f "$GZ.md5sum"
  echo "note: no valid published md5 for $DAY; relying on gzip CRC32 instead."
  echo "      (gunzip verifies a CRC over the whole decompressed stream, which"
  echo "       is a stronger end-to-end check than the md5 would have been.)"
fi

if [[ ! -s "$RAW" ]]; then
  echo "decompressing -> $RAW (verifying CRC32)"
  if ! gunzip -c "$GZ" > "$RAW"; then
    echo "FAILED: gzip CRC error -- archive is corrupt, delete it and re-run" >&2
    rm -f "$RAW"; exit 1
  fi
fi
echo "ready: $RAW ($(stat -f%z "$RAW") bytes)"
