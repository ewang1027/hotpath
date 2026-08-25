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

- **A stale number got into the docs anyway.** The memmove volume was measured
  with the 8-byte element size from the *reverted* optimisation and reported
  alongside the 4-byte shipped design, so every "MB memmoved" figure was 2x
  too high. The element *count* was right; the byte conversion was not. Fixed,
  and the diagnostic now lives in a committed tool (`src/tape_stat.cpp`) rather
  than a throwaway script -- which is the exact lesson `simplified_gto_solver`
  taught and which I still managed to repeat.
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

## Phase 5 — CI, determinism, regression gate  [DONE]

- CI on macos-14 (arm64 on purpose) across release / TSan / ASan+UBSan. It
  cannot fetch the 8.25 GB sample, so real-data gates live in
  `scripts/regenerate.sh` and `scripts/regression_gate.sh`; CI runs the
  randomised four-way book differential test, the invariants and the ring
  stress test.
- `scripts/check_determinism.sh`: 20 replays at -O3 plus one at -O0, hashing
  every snapshot of every design. All four tapes match exactly. The
  determinism claim in `METHODOLOGY.md` had been asserted without anything
  enforcing it -- now it is measured.
- `scripts/regression_gate.sh`: full-day parse + four-way crossval + throughput
  within 25% of the recorded baseline. Loose on purpose; a tighter bound would
  fail on laptop scheduling noise rather than on a real regression.

---

## Phase 6 — threaded pipeline  [DONE]

`MarketMaker` extracted so the threaded and single-threaded paths run identical
strategy code, then wired feed / book+strategy / gateway across two SPSC rings.

**Gate: PASS** -- fill streams hash identically to the single-threaded path on
all four symbols.

**The performance result is negative and is reported as such:** pipelining is
1.23-1.52x *slower*, at a near-constant +17 to +20 ns/event, which is the ring
hop. Ring occupancy diagnoses it exactly (feed->strategy >=75% full 99.2% of the
time; strategy->gateway empty 95.3%): the stages are unbalanced, so adding
synchronisation to a critical path that did not shorten cannot pay. Full write-up
in `PERFORMANCE.md`.

Also noted there: the queue-position bookkeeping costs 3-6x book maintenance
itself, because re-quoting re-walks the level's FIFO. Tracking it incrementally
is the obvious next optimisation.

---

## Phase 7 — re-quote latency in the fill model  [DONE]

Closes the limitation the README had been declaring: re-quotes were
instantaneous, so fill counts were an upper bound. `MarketMaker` now takes a
latency, during which the OLD quote keeps resting at its stale price, and
`latency_sweep` measures 0 to 10 ms.

**Gate: PASS** — markout degrades monotonically with latency on all four
symbols, and the zero-latency path reproduces the previously published numbers
bit-for-bit (9,535 fills, 6.4x overstatement, -0.282 bps).

Headline: latency does not simply cost fills. AAPL's fill *count* is U-shaped
and ends 15% ABOVE the zero-latency count, with 50% of filled volume on stale
quotes at 10 ms. A slow maker loses the queue races it wanted and gets filled on
the quotes it was trying to cancel. Microseconds register because executions
arrive in bursts: median gap before an execution is 26 us on AAPL and 6.3 us on
SPY, against ~105 us between events generally.

Also added the `swept` fill path -- our quote being strictly more aggressive
than the order that actually traded, so an aggressor would have hit us first.
Unreachable at zero latency (verified: 0 such fills), which is why it never
showed up before.

### Traps hit

- **A stale number reached the docs.** Memmove volume was measured with the
  8-byte element size from the *reverted* optimisation and printed next to the
  4-byte shipped design, so every "MB memmoved" figure was 2x too high. The
  element count was right; the byte conversion was not. Fixed, and the
  diagnostic now lives in a committed tool (`src/tape_stat.cpp`) instead of a
  throwaway script -- the exact lesson from `simplified_gto_solver`, repeated
  anyway.
- **`market_maker.hpp` was not self-contained**: it used `book::apply` without
  including it and only compiled because every existing TU pulled in `tape.hpp`
  first. A new test exposed it. `apply()` moved to `events.hpp`, where it
  belongs -- it is about applying an event, not about tapes.
- **Landing order bug, caught by a test.** A replacement whose latency elapsed
  between two events was landing only *after* the later event's fills were
  attributed, so a quote we had long since moved kept being swept at its old
  price. Pending replacements now take effect before the event's fills, joining
  the book as it stood when they actually arrived.
- **Hashing a struct's raw bytes is not deterministic when it has padding.**
  The pipeline's fill digest did exactly that; adding two bools to `Fill`
  changed the padding layout and it immediately reported divergence between the
  threaded and single-threaded paths. The digest had never been sound -- it
  agreed by luck. Fields are now hashed individually, and a `static_assert`
  pins `LevelView` (whose byte-wise hash *is* sound) against the same trap.
- The *initial* quote placement costs latency too. That is correct, not an edge
  case to design away, and it is part of why a slow maker spends less time
  quoted -- but it silently made the first version of two tests pass for the
  wrong reason.

---

## Phase 8 — queue position in O(1)  [DONE]

The strategy stage cost 3-6x book maintenance because re-quoting rebuilt an
explicit set of the orders ahead by walking the level's FIFO. A price level is
strict FIFO, so stamping each order with a monotonic insertion sequence turns
"was this ahead of mine?" into one comparison against the stamp captured at
join. **2.90x on AAPL** (104.50 -> 36.05 ns/event), 1.63-2.38x elsewhere,
scaling with how often the touch moves.

**Gate: PASS** -- fill output is bit-identical before and after on every symbol,
so the stamp test is exactly equivalent to the membership set, not an
approximation of it.

Note the earlier rejected version of the same idea: `order_ref < my_join_ref`
is O(1) too, but ITCH order refs are not monotonic (3.0% violations on AAPL) and
it would have silently corrupted every queue position. The book's own insertion
stamp is monotonic by construction.

Knock-on: with the strategy ~2.5x cheaper, the threaded pipeline's absolute
penalty stayed in the same band while every ratio got worse -- exactly what the
fixed-hop-cost model predicted. Recorded in `PERFORMANCE.md` as a passed
prediction rather than quietly re-measured.

### Traps hit

- **Struct layout cost more than the field did.** A 64-bit insertion stamp
  placed after `side` pushed `HybridBook::Order` from 32 to 40 bytes through
  padding and regressed pure book maintenance 10-35% (INTC 13.4 -> 17.5
  ns/event) -- the order pool is the hot path's dominant working set. A 32-bit
  stamp (~100x a full day of adds across the whole US tape) keeps it at 32
  bytes; a `static_assert` pins it. Caught by the regression gate, not by
  reading the code.
- **The absolute-throughput gate was unsupportable and had to be redesigned.**
  Across invocations the same binary on the same tape ranged 12.6-17.5 ns/event
  on INTC, far wider than the within-process CI, so the gate flapped. It now
  gates on the hybrid-vs-map *ratio* measured in the same process, which absorbs
  the same noise on both sides: ratios reproduced within ~14% across runs where
  absolutes swung 30%. This is the same claim class `METHODOLOGY.md` says this
  hardware supports, so the gate and the docs now agree on what a number is
  worth here.

---

## Phase 9 — cross-sectional replication, 25 symbols  [DONE]

Every headline result had been measured on four symbols. Four points can
describe almost any curve, so the study was widened to 25 spanning $7 (SIRI) to
$1,784 (AMZN) and 70K to 2.4M events -- 18,798,869 events total, extracted in
one pass over the same day.

**Gate: PASS** -- all four book designs cross-validate with zero divergences on
all 25 tapes.

What replicated, and what did not:
- Naive fill model overstates volume: **25/25**, but the range widened from
  3.7-6.4x to **3.4-15.6x**. Largest exactly where you fill least (SIRI 15.6x
  on 82 fills; SPY 3.7x on 12,058).
- Passive fills adversely selected: **25/25**.
- Markout worsens with queue depth: **10/13** symbols with >=200 fills in both
  buckets. Real and directional, but weaker than four symbols implied -- now
  reported as such, with the 12 low-sample symbols excluded rather than counted
  as support.
- The intrusive book's failure: **7/25 symbols** lose to `std::map`, up to 1.9x
  (AMZN). The predictor `level churn x depth` has **Pearson r = -0.927**
  (Spearman -0.871) against the intrusive-vs-map ratio.

The most useful thing the wider sample bought was killing a wrong explanation.
With four symbols the losers were the expensive ones and price looked like the
cause (r = -0.837). The ETFs break it: SPY at $321 wins with 699 mean levels
while AAPL at $250 loses with 4,652. Depth is what the sorted vector pays for;
price merely tends to produce depth (r = +0.630). A four-symbol sample would
have shipped the price story.

---

## Phase 10 — bug hunt  [DONE]

A deliberate pass looking for defects the green build was hiding. Five real
bugs, none of which any test was failing on.

1. **`OpenHashMap::insert` rejected in-place updates once the table passed its
   load factor.** The limit was checked on entry rather than at the point of
   occupying a new slot, so updating a key already present returned nullptr --
   which every caller reads as "table full" and drops the write. Now checked
   only when a new slot is actually needed.

2. **A corrupt stream passed the parse gate.** A zero-length frame silently
   ended the parse, `truncated` was not incremented, and nothing verified the
   whole file had been consumed -- so a 116-byte fixture with 2 of 3 messages
   skipped reported `GATE: PASS` after reading 38 bytes. The gate now requires
   `bytes unread == 0` and counts zero-length frames. The real full day does
   consume all 8,251,407,909 bytes, so the published result was genuine; the
   gate simply could not have told the difference.

3. **`SpscRing` accepted a non-power-of-two capacity.** The wrap is a mask, not
   a modulo, so any other value silently aliases slots. `OpenHashMap` already
   rejected this; the ring now does too.

4. **`extract_tape` leaked its routing index and ignored insert failures.**
   Entries were retired on Delete and Replace but never when an order was fully
   executed or fully cancelled, so the index grew all session: 4,270,459 entries
   against a peak of 1,924,078 concurrently-live orders, a 2.2x leak. It stayed
   under the load factor on this day (0.255), but a busier one would overflow --
   and since `insert()`'s return value was discarded, the tapes would simply
   have been missing events with nothing reporting it. Now retired on full
   execution/cancel (index ends the day at **0 entries**, peak load 0.114,
   matching `itch_stat`'s independent "still live at EOD: 0"), and an insert
   failure is fatal.

5. **`BookEvent` wrote uninitialised padding to disk.** The struct is 36 bytes
   of fields padded to 40 and is `fwrite`-ed verbatim, so the same input
   produced byte-different tape files -- 2,356 of 1,512,179 events on AAPL,
   identical in every field. Padding is now explicit and zero-initialised, with
   an `offsetof` assertion so it cannot come back. Tapes are byte-reproducible
   across runs, and field bytes are unchanged from the pre-fix tapes, so every
   published number stands.

### And one hypothesis that was wrong

Probing the queue model's strict-FIFO assumption found it violated on 1.3-4.5%
of executions. The obvious correction -- require the executed order to be ahead
of us before advancing the queue -- drops the fill count to **exactly zero** on
every symbol. We join at the back, so once everything ahead is consumed the only
orders left arrived after we did; the execution that legitimately reaches us is
always one of those. The original unconditional logic was right. Reverted, and
the genuinely suspicious case (an order behind us trading while volume still
rests ahead) is now counted and reported instead: 73 on AAPL against 9,535
fills, 0 on AMZN.

---

## Remaining / not attempted

Stated as gaps in the README rather than hidden: no network path or kernel
bypass (the pipeline is threads and shared memory, not sockets); no self-impact
in the fill model, and its latency is a fixed delay with no jitter; 25 symbols
but **one trading day** -- the cross-section is wide, the time series is n=1.

The highest-value next step is external to the code: run the finished binaries
on bare-metal x86 Linux for an evening to get real p99/p99.9 tail distributions,
which this hardware cannot produce (see `METHODOLOGY.md`).
