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

## Phase 2 — order book designs  [DONE]

Four implementations, cross-validated bit-identically on a full trading day.
**Gate: PASS** — zero divergences over 5,609,721 events across AAPL/SPY/MSFT/
INTC, all four draining to exactly zero resting orders at the close. Full study
in `PERFORMANCE.md`.

Headline: the textbook intrusive book is the *slowest* design on AAPL, losing
even to `std::map`, because real books are ~4650 levels deep and its sorted
level vector memmoves 5.7 GB per replay. The hybrid design (grid addressing +
per-level intrusive FIFO) that the measurements implied is 3.3x the baseline and
keeps the time-priority ordering the fill model needs.

### Traps hit

- A randomised differential test caught the intrusive book **silently dropping
  orders** when its level pool filled: `find_or_create_level` returned -1 and
  `add()` just returned. Both bounded designs now count rejections and every
  gate asserts zero.
- The first "optimisation" (price inline in the level index) made things
  *worse* and falsified the cache hypothesis. Recorded as a negative result
  rather than quietly reverted.
- The dense grid cannot span the observed price range: resting orders go from
  $0.0001 to $199,999, which is 8 GB/side. It is a bounded window plus a
  `std::map` overflow, and the cross-validation deliberately exercises the
  overflow path.

---

## Phase 3 — SPSC ring, memory ordering, false sharing  [DONE]

**Gate: PASS.** The `relaxed` variant reproducibly violates its invariant on
arm64 (1,463 zero slots + 544 torn reads per 10.5M messages); the
release/acquire variant is clean and TSan-clean over a 2M-message threaded
stress run.

Findings in `PERFORMANCE.md`: index caching is worth 2.63x; padding is *not*
measurable on this machine and the shared-line layout is slightly faster — an
honest negative result with an Apple-Silicon-specific mechanism, explicitly not
generalised to x86.

### Traps hit

- **`std::aligned_alloc` requires size to be a multiple of the alignment**, but
  C++17's aligned `operator new` must accept any size. A 16-byte allocation at
  128-byte alignment -- i.e. any small cache-line-padded object -- threw a
  spurious `bad_alloc`. Switched to `posix_memalign`; regression test pinned.
- **ASan replaces `operator new`**, so the allocation counter is dead in an ASan
  build and every zero-allocation assertion would pass vacuously. Tests now
  detect ASan and skip those explicitly.

---

## Phase 4 — queue position, fill model, adverse selection  [DONE]

**Gate: PASS.** Markouts are negative on average (adverse selection present, as
expected and reported honestly), and fill rate varies with queue position.
Full results in `ADVERSE-SELECTION.md`.

Headlines: the naive fill model overstates passive volume by 3.7-6.4x; 10s
markouts are negative almost everywhere; markout degrades ~4x with queue depth
on AAPL; SPY shows an order of magnitude less adverse selection than the single
names, which is the expected result for a broad-index ETF recovered from data.

### Traps hit

- **ITCH order reference numbers are NOT monotonically increasing.** Measured
  21,094 non-monotonic adds on AAPL in one day (3.0%). The cheap `ref < join_ref`
  test for "was this order ahead of me" is therefore invalid, and using it would
  have silently corrupted every queue position. Membership is tracked explicitly
  instead. Checked before relying on it, not after.
- Events must be resolved against the book **before** `apply()`: ITCH's
  execute/cancel/delete carry only an order reference, so after the mutation the
  side and price needed to attribute a fill are gone.

---

## Remaining

- Phase 5: CI, plots, and the regression gate.
- Not attempted, and stated as gaps in the README: network path, kernel bypass,
  self-impact in the fill model, more than one trading day.
