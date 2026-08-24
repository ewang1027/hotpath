# Order book design study

All figures from one NASDAQ TotalView-ITCH 5.0 trading day (2019-12-30,
8.25 GB, 268,744,780 messages). Regenerate with `./scripts/regenerate.sh`.

**Read `METHODOLOGY.md` first.** This hardware has a 41.7 ns clock tick, so
nothing here is a per-event latency distribution. Every number below is either
an amortised cost over ~10^6 events (a run spanning hundreds of milliseconds,
where the tick contributes no meaningful error), a ratio between designs on
byte-identical input, or an exact count.

## The four designs

| | structure | price lookup | best bid/ask | keeps queue position |
|---|---|---|---|---|
| **map** | `std::map` price→aggregate, `std::unordered_map` orders | O(log L) tree | `begin()` | no |
| **intrusive** | pooled orders, per-level FIFO, **sorted vector** of level indices | O(log L) binary search | `front()` | yes |
| **flat** | direct-addressed penny grid + bitmap, aggregate only | O(1) arithmetic | bitmap scan | no |
| **hybrid** | direct-addressed grid + **per-level FIFO** over a pooled level store | O(1) arithmetic | bitmap scan | yes |

## Results

`ns/event` is amortised over a full-tape replay, 7 trials, ±95% CI on the mean.
`x` is speedup versus the `map` baseline.

### Book maintenance only

| symbol | events | map | intrusive | flat | hybrid |
|---|---:|---:|---:|---:|---:|
| AAPL | 1,512,179 | 53.8 (1.00x) | 55.5 (0.97x) | **15.5 (3.48x)** | 16.4 (3.28x) |
| SPY | 2,132,141 | 35.8 (1.00x) | 28.0 (1.28x) | **15.0 (2.40x)** | 15.5 (2.32x) |
| MSFT | 1,215,912 | 44.8 (1.00x) | 36.1 (1.24x) | **14.3 (3.14x)** | 16.1 (2.78x) |
| INTC | 749,489 | 35.7 (1.00x) | 26.4 (1.35x) | **12.6 (2.82x)** | 13.2 (2.69x) |

### Replay + top-of-book query on every event

The access pattern that actually matters — a book nobody reads is not a useful
benchmark, and the designs differ more in read cost than in write cost.

| symbol | map | intrusive | flat | hybrid |
|---|---:|---:|---:|---:|
| AAPL | 54.0 (1.00x) | 56.8 (0.95x) | 22.5 (2.40x) | **21.5 (2.51x)** |
| SPY | 36.2 (1.00x) | 30.4 (1.19x) | 21.2 (1.71x) | **19.9 (1.82x)** |
| MSFT | 44.8 (1.00x) | 37.0 (1.21x) | 19.0 (2.36x) | **18.6 (2.40x)** |
| INTC | 36.3 (1.00x) | 28.0 (1.29x) | 16.9 (2.15x) | **16.8 (2.15x)** |

### Allocations during replay

Counted, not estimated (global `operator new` is replaced — see `METHODOLOGY.md`).

| symbol | map | intrusive | flat | hybrid |
|---|---:|---:|---:|---:|
| AAPL | 979,847 (40.0 MB) | **0** | 3,868 | 3,868 |
| SPY | 1,218,265 (48.8 MB) | **0** | 10,354 | 10,354 |
| MSFT | 679,785 (27.6 MB) | **0** | 2,436 | 2,436 |
| INTC | 402,233 (16.3 MB) | **0** | 1,277 | 1,277 |

`map` allocates roughly once per resting order. The grid designs' allocations
are entirely their `std::map` overflow for out-of-window and sub-penny prices;
the dense path allocates nothing.

## Findings

### 1. The textbook intrusive book loses to `std::map` on 7 of 25 symbols

This is the result that surprised me. The pooled, intrusive, zero-allocation
book — the design everyone describes as "the HFT one" — **loses to `std::map`**
on AMZN, GOOGL, TSLA, NFLX, NVDA, AAPL and FB, by as much as **1.9x** (AMZN:
133.2 ns/event against `std::map`'s 70.8).

It is not the binary search. I assumed it was, stored the price inline in the
index vector to make the search cache-friendly, and it got *worse*: 56 → 66
ns/event. Doubling the element size doubled the thing that actually dominates.

The cost is the sorted vector's `memmove` on level create/destroy, and it is
enormous because **real books are deep**:

| symbol | mean live levels | max | level creates/event | elements shifted per create | memmove per replay |
|---|---:|---:|---:|---:|---:|
| AAPL | 4,652 | 5,320 | 0.122 | 1,946 | **2,866 MB** |
| MSFT | 3,463 | 3,813 | 0.040 | 1,334 | 522 MB |
| INTC | 1,576 | 1,723 | 0.014 | 576 | 48 MB |
| SPY | 699 | 756 | 0.051 | 238 | 208 MB |

Regenerate with `./build/src/tape_stat <SYM>.tape`.

### The predictor, across 25 symbols

Four symbols can describe almost any curve, so the study was widened to 25
spanning $7 to $1,784 and 70K to 2.4M events. The predictor is
`level_creates_per_event × mean_depth` — the number of vector elements shifted
per event — and it explains the ranking almost completely:

**Pearson r = −0.927** between `log10(elements shifted per event)` and the
intrusive-vs-`std::map` ratio (Spearman −0.871, n = 25).

| symbol | ~price | mean depth | elements shifted/event | intrusive vs map |
|---|---:|---:|---:|---:|
| AMZN | $1,784 | 4,491 | 1,725 | **0.53x** |
| TSLA | $338 | 2,993 | 1,078 | **0.68x** |
| NVDA | $208 | 2,734 | 694 | **0.82x** |
| GOOGL | $1,280 | 1,750 | 691 | **0.68x** |
| NFLX | $325 | 2,106 | 689 | **0.82x** |
| AAPL | $250 | 4,652 | 568 | **0.97x** |
| FB | $195 | 2,583 | 411 | **0.99x** |
| MSFT | $147 | 3,463 | 139 | 1.26x |
| QQQ | $212 | 2,177 | 137 | 1.19x |
| SPY | $321 | 699 | 36 | 1.27x |
| INTC | $56 | 1,576 | 22 | 1.31x |
| … | | | | |
| GE | $11 | 312 | 1.2 | 1.43x |
| F | $9 | 219 | 1.1 | 1.74x |

The crossover is sharp and sits between ~140 and ~410 elements shifted per
event: MSFT still wins at 139, FB already loses at 411.

### It is depth and churn, not price

Price looks like the driver — the seven losers are the seven most expensive
single names — and it does correlate (r = −0.837). But it is a proxy, and the
ETFs break it:

- **SPY at $321 wins** (1.27x) with a mean depth of just **699 levels**.
- **AAPL at $250 loses** (0.97x) with a mean depth of **4,652**.

A broad-index ETF concentrates enormous liquidity into a tight band around the
touch, so despite a high price its book is shallow. A high-priced single name
accumulates resting orders scattered across thousands of distinct penny levels.
Depth is what the sorted vector pays for; price merely tends to produce depth
(r = +0.630 between log price and depth).

**The generalisation:** a sorted vector is the right level index only when the
book is shallow. Textbook descriptions of this design quietly assume a book tens
of levels deep. Real books here run from 125 (IWM) to 4,652 (AAPL), and the
design fails wherever churn × depth crosses a few hundred elements per event.

### 2. Direct addressing wins, and the hybrid gets it without giving up queue position

The flat grid is 2.5–3.6x the baseline because price lookup is arithmetic and
nothing ever shifts. But an aggregate-only grid throws away *which* orders are
resting at a level, and ITCH fills a level in strict time priority — so that
FIFO order **is** queue position, which the fill model in Phase 4 depends on.

Across all 25 symbols the hybrid beats `std::map` by **1.96x to 3.89x**
(median 2.62x) and beats the intrusive design on **25 of 25**, by up to **7.3x**
(AMZN: 18.2 ns/event against 133.2).

The hybrid keeps the grid's addressing and restores the per-level intrusive
FIFO over a pooled level store. It costs ~3–13% versus flat on maintenance, and
is *faster* than flat once the touch is queried every event on three of four
symbols (checking `slot[hint] >= 0` is a single load; the bitmap variant
extracts a bit first).

Part of that maintenance gap is the monotonic insertion stamp each order
carries — the thing that makes queue position O(1) (see below). It is one extra
store on every add, and adds are 46% of the stream, so it is not free: it costs
the book ~5–8% to save the strategy 2.9x. Worth stating rather than burying,
since the flat design does not pay it and cannot answer the question it buys.

The struct layout matters more than the field does. Placing a 64-bit stamp
after `side` pushes `Order` from 32 to 40 bytes through padding, and that
measured as a **10–35% regression** in pure book maintenance (INTC 13.4 → 17.5
ns/event) — the order pool is the hot path's dominant working set. A 32-bit
stamp holds ~100x a full day of adds across the entire US tape and keeps the
struct at 32 bytes; a `static_assert` pins it there.

### 3. Why the dense grid must be a bounded window

The first design sketch was a dense array over the observed price range. The
data killed it: resting prices span **$0.0001 to $199,999** — far-away limit
orders that sit all day and never trade. A dense array over that range is
**8 GB per side**.

Sizing the window to p0.5–p99.5 of add prices gives 1,659–12,846 penny ticks
(covering ~99.0% of adds), which is cache-resident, with a `std::map` overflow
for the remaining ~1% plus the ~0.005% of adds that are sub-penny. The
cross-validation gate deliberately exercises that overflow path, and the
snapshot merge across grid and overflow is exact rather than
"the overflow never reaches the top ten".

### 4. Correctness: four implementations, zero divergences

Every design produces identical ten-deep snapshots after **every** event across
all four tapes (5,609,721 events), and all four independently drain to exactly
zero resting orders at the close.

This caught a real bug. The intrusive book silently dropped orders when its
level pool was exhausted — `find_or_create_level` returned -1 and `add()` just
returned, leaving the book permanently wrong with no signal. It surfaced only
because a second implementation disagreed, which is not a diagnostic available
in production. Both bounded designs now count rejections and every gate asserts
they are zero.

## Optimisation log

| change | before | after | verdict |
|---|---:|---:|---|
| intrusive: store price inline in level index (8B entries) | 56.1 | 66.0 | **reverted** — falsified the cache hypothesis; element size dominates because memmove does |
| add hybrid design (grid addressing + intrusive FIFO) | 55.4 | 15.9 | kept — 3.36x, and retains queue position |
| hybrid vs flat on top-of-book query (AAPL) | 22.7 | 21.8 | kept — slot load beats bit extraction |
| queue position: FIFO walk → monotonic insertion stamp (AAPL, full strategy) | 104.50 | 36.05 | kept — **2.90x**, bit-identical output |

---

# SPSC ring: memory ordering and false sharing

## Memory ordering — the experiment this hardware makes possible

x86-64 is TSO: it does not reorder store-store or load-load. A ring that
publishes with `memory_order_relaxed` instead of release/acquire therefore
*works on x86 by accident*, and the bug is invisible. arm64 is weakly ordered
and will actually reorder.

Same code, one policy parameter, zero-initialised ring, single lap so an
unpublished slot is distinguishable. Payload is 8 words that must all agree.

| variant | messages | zero slots | torn reads | verdict |
|---|---:|---:|---:|---|
| `release`/`acquire` | 10,485,600 | 0 | 0 | clean |
| `relaxed` | 10,485,600 | **1,463** | **544** | **violations observed** |

The 544 torn reads are the interesting ones: the consumer saw a slot where some
words were the new message and some were still the previous contents. That is
partial store visibility, and it is exactly what the release fence forbids.

Reproduce with `./build/src/litmus_ring --rounds 10`. Note that a *clean*
relaxed run is not evidence of correctness — reordering windows are
microarchitectural, and a quiet CPU may simply not open one. The code is
unsound on arm64 either way.

## False sharing — a negative result

The producer owns `tail_`, the consumer owns `head_`. Textbook advice is to pad
them onto separate cache lines. There is a second, independent mitigation: cache
the opposite index (`cached_head_`/`cached_tail_`) so the steady-state path
never *reads* the other side's atomic at all. Measuring only one of the two
cannot separate them, so all four combinations:

| padding | index cache | ns/msg | ±95% | M msg/s |
|---|---|---:|---:|---:|
| separate lines | cached | 27.39 | 1.53 | 36.51 |
| shared line | cached | 24.41 | 1.14 | 40.97 |
| separate lines | uncached | 72.05 | 0.44 | 13.88 |
| shared line | uncached | 66.59 | 1.16 | 15.02 |

**Index caching is worth 2.63x (+44.7 ns/msg).** Unambiguous — the confidence
intervals are nowhere near overlapping.

**Padding shows no measurable benefit on this machine, and the shared-line
variant is consistently slightly faster** (0.89x with caching on, 0.92x with it
off). That replicates across both caching configurations, so it is not noise in
the way the first, under-powered run was.

Hypothesis for why, stated as a hypothesis: Apple Silicon's performance cores
share an L2, so a contended line bounces within shared cache rather than across
an interconnect, making the coherence cost small. Padding meanwhile spreads the
ring's control state over three cache lines instead of one, so every operation
touches more lines. When coherence is cheap, that extra footprint costs more
than the sharing.

**Do not generalise this to x86.** On a multi-socket box, where a contended line
crosses an interconnect, padding would very likely pay for itself. What this
measurement does support is narrower and still useful: *padding is not free, and
it is the weaker of the two mitigations — the one that matters is not touching
the other core's line in the first place.*

Note the padding constant is **128 bytes**, not the 64 you would use on x86;
`sysctl hw.cachelinesize` reports 128 on Apple Silicon and a test asserts the
compiled constant matches the OS.

## Sanitizers

| build | result |
|---|---|
| ThreadSanitizer, full suite | clean (305,984 assertions) |
| ASan + UBSan, full suite | clean |

One wrinkle worth recording: **ASan replaces global `operator new`/`delete`**,
which overrides ours, so the allocation counter never moves in an ASan build.
The zero-allocation invariant cannot be verified there — the tests detect ASan
and skip those assertions explicitly rather than passing vacuously. Invariants
are gated in the normal build; ASan is for memory errors.

---

# Threaded tick-to-trade pipeline

```
feed thread     ──ring1──▶  strategy thread  ──ring2──▶  gateway thread
walk the tape               book + market maker          consume fills
```

Both paths run the **same** `MarketMaker` code, so comparing them isolates the
threading and nothing else. `./build/src/pipeline <SYM>.tape`.

## Result 1 — the ring handoff preserves semantics exactly

| symbol | single-threaded | pipelined | fill digests |
|---|---:|---:|---|
| AAPL | 9,535 fills | 9,535 fills | identical |
| SPY | 12,058 fills | 12,058 fills | identical |
| MSFT | 9,403 fills | 9,403 fills | identical |
| INTC | 4,434 fills | 4,434 fills | identical |

The fill stream is hashed with FNV-1a in order. Identical digests mean the
lock-free handoff changed nothing about what the strategy did — which is the
property you actually want from a pipeline, and the one that is easy to lose.

## Result 2 — pipelining makes it *slower*, on every symbol

| symbol | single-threaded (ns/event) | 3-stage pipeline | penalty |
|---|---:|---:|---:|
| AAPL | 42.90 ± 3.34 | 55.62 ± 2.98 | **1.30x slower** (+12.7) |
| SPY | 30.47 ± 0.96 | 44.63 ± 1.20 | **1.46x slower** (+14.2) |
| MSFT | 32.08 ± 0.80 | 47.23 ± 2.53 | **1.47x slower** (+15.2) |
| INTC | 29.58 ± 2.20 | 47.94 ± 1.60 | **1.62x slower** (+18.4) |

The penalty is **+13 to +18 ns/event on every symbol** — essentially constant,
because it is the fixed cost of crossing the ring, not a function of the work.
That matches the standalone ring measurement (~25 ns/message) once you account
for the second ring being nearly idle.

And the ratio behaves exactly as a fixed overhead should: **the cheaper the
strategy stage, the worse pipelining looks.** INTC has the lightest per-event
work (29.6 ns) and suffers the worst penalty (1.62x); AAPL has the heaviest
(42.9 ns) and suffers the least (1.30x).

### The model made a prediction, and it held

This table was first measured before the queue-position optimisation below,
when the strategy stage cost 36–90 ns/event. If the penalty really is a fixed
hop cost, then making the strategy ~2.5x cheaper should leave the absolute
penalty alone and make every *ratio* worse. It did:

| symbol | penalty before (strategy 36–90 ns) | penalty after (strategy 30–43 ns) |
|---|---:|---:|
| AAPL | 1.23x (+20.4 ns) | 1.30x (+12.7 ns) |
| SPY | 1.30x (+16.8 ns) | 1.46x (+14.2 ns) |
| MSFT | 1.35x (+19.3 ns) | 1.47x (+15.2 ns) |
| INTC | 1.52x (+18.8 ns) | 1.62x (+18.4 ns) |

Every ratio degraded while the absolute cost stayed in the same band. Faster
work does not make synchronisation cheaper — it makes it a larger share of what
is left.

## Result 3 — the ring occupancy says exactly why

| ring | empty | <25% | <50% | <75% | ≥75% |
|---|---:|---:|---:|---:|---:|
| feed → strategy (AAPL) | 0.0% | 0.3% | 0.3% | 0.3% | **99.2%** |
| strategy → gateway (AAPL) | **94.6%** | 5.4% | 0.0% | 0.0% | 0.0% |

The first ring is full essentially always: the feed thread can produce events far
faster than the strategy can consume them, so it spends its life blocked on a
full ring. The second is empty essentially always: fills are rare (9,535 out of
1.5M events), so the gateway is starved.

**The stages are wildly unbalanced.** Parsing is cheap, the book plus
queue-position bookkeeping is expensive, and order emission is nearly free.
Splitting cheap work away from expensive work across a ring cannot help — you
have added a synchronisation cost to a pipeline whose critical path did not get
shorter.

## What this actually means

This is not an argument against pipelining in trading systems; it is a
measurement of when it pays. A ring hop costs ~20 ns here. It is worth paying
when it buys you something that costs more than 20 ns — isolating a thread that
would otherwise be interrupted, crossing a NUMA or process boundary, decoupling
a stage that can block. It is not worth paying to move 15 ns of book maintenance
onto another core.

The honest read of this repo's own numbers: **for this workload, on this
machine, the single-threaded path is the right design**, and the ring earns its
place as the mechanism for a boundary that has to exist for another reason — not
as a throughput optimisation.

## Queue position in O(1)

The strategy stage originally cost 36–90 ns/event against 13–16 ns for book
maintenance alone — the queue-position bookkeeping was 3–6x the cost of the book
it sat on. The reason was that re-quoting rebuilt an explicit set of the orders
ahead of us by walking the level's FIFO, and levels at the touch are not short.

It does not need a set at all. A price level's order list is strict FIFO, so
stamping each order with a monotonic insertion sequence makes *"was this order
ahead of mine?"* a single comparison against the stamp captured when we joined.
Everything already resting there is below it; everything arriving later is above.

| symbol | before (ns/event) | after | speedup |
|---|---:|---:|---:|
| AAPL | 104.50 | **36.05** | **2.90x** |
| SPY | 73.57 | **30.89** | **2.38x** |
| MSFT | 71.07 | **30.31** | **2.34x** |
| INTC | 49.72 | **30.41** | **1.63x** |

The speedup tracks how often the touch moves — AAPL re-quotes on 10.2% of
events, INTC on 1.4% — because that is how often the walk used to happen.

Fill output is **bit-identical** before and after on every symbol (9,535 fills
and 548,567 shares on AAPL, with matching queue-depth buckets), which is the
point: the stamp comparison is not an approximation of the membership set, it is
exactly equivalent to it.

Note what this did *not* work: an earlier attempt used `order_ref <
my_join_ref` as the same O(1) test. ITCH order reference numbers are not
monotonic — 21,094 non-monotonic adds on AAPL in one day, 3.0% of adds — so it
would have silently corrupted queue position. The book's own insertion stamp is
monotonic by construction.
