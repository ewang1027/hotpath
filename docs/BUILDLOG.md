# Build log

Resume-from-cold notes. Newest phase last. Every phase has a pass/fail gate;
"it runs without crashing" is not a gate.

## Resuming in one minute

```bash
brew install cmake ninja                      # if missing
./scripts/fetch_data.sh 12302019              # ~3.5 GB gz -> ~12 GB raw
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/src/hotpath_env                       # machine facts -> METHODOLOGY.md
./build/tests/hotpath_tests                   # unit + differential tests
./build/src/itch_stat ~/market-data/itch/12302019.NASDAQ_ITCH50
```

Data lives **outside** the repo (`~/market-data/itch` by default, override with
`HOTPATH_DATA_DIR`) so a 12 GB file can never be accidentally committed.

---

## Phase 0-1 — measurement harness + ITCH 5.0 parser  [DONE]

Built the ruler before the thing being measured, because the ruler turned out to
be the binding constraint (see `METHODOLOGY.md`).

**Gate: PASS** on a 3 GB real prefix of 12302019 (97,526,600 messages):
zero unknown message types, zero length mismatches, zero orphaned order
references of any kind, zero allocations and zero syscalls in the steady-state
loop. (`truncated tail: 1` is expected on a deliberately chopped dev slice; the
full-day run is the real gate.)

**Measured:** 33.3 M msg/s, 30.0 ns/msg amortised, 0.95 GiB/s ingest.

### Message mix on a real trading day (12302019, first 3 GB)

| Type | Count | Share |
|---|---:|---:|
| A AddOrder | 42,419,420 | 43.50% |
| D OrderDelete | 40,506,051 | 41.53% |
| U OrderReplace | 8,493,207 | 8.71% |
| E OrderExecuted | 1,666,224 | 1.71% |
| X OrderCancel | 1,600,047 | 1.64% |
| F AddOrderMPID | 1,100,208 | 1.13% |
| I NOII | 1,079,430 | 1.11% |
| P TradeNonCross | 379,302 | 0.39% |
| others | < 0.3% each | |

**The finding that shapes every later design decision:** adds and deletes are
85% of the stream, executions are 1.7%. Orders overwhelmingly get cancelled, not
traded. So order *deletion* is the hot operation, which is why the order map
uses backward-shift deletion rather than tombstones — a tombstoned table would
degrade toward a linear scan over a full day at this delete rate. It is also
why the Day 5-6 fill model matters: if you assume a resting order fills whenever
its price trades, you are wrong about 98% of the flow.

Peak concurrently-live orders: 1,759,699 (over 3 GB; expect more over a full
day). Table is currently 2^24 slots -> load factor 0.105. Retune once the
full-day peak is known.

### Traps hit

- **dyld `__interpose` is silently ignored from a static archive.** The syscall
  counter read zero forever while every invariant assertion passed vacuously.
  Fix: build `hotpath_instrument` as `SHARED`. Guard: `syscall_counting_active()`
  performs a real probe.
- **dyld does not apply an image's own interposition to its own calls.** The
  probe therefore has to be `inline` in the header so it compiles into the
  caller's image; as an out-of-line function inside the instrumentation dylib it
  reported "inactive" even when interposition was working for everyone else.
- **Nasdaq's server drops multi-GB transfers** (curl exit 56). `fetch_data.sh`
  resumes with `-C -` and retries until the byte count matches Content-Length,
  then verifies the published md5 before spending 12 GB of disk on `gunzip`.
- Strict warnings must be attached to our targets, not set globally, or
  FetchContent'd Catch2 buries our diagnostics in its own.

---

## Phase 2 — order book, three designs  [NEXT]

Three implementations behind one interface, cross-validated bit-identically on a
full trading day. See the plan for the design list.
