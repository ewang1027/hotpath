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
| AAPL | 1,512,179 | 53.4 (1.00x) | 55.4 (0.96x) | **15.0 (3.57x)** | 15.9 (3.36x) |
| SPY | 2,132,141 | 37.2 (1.00x) | 28.8 (1.29x) | 14.7 (2.53x) | **14.6 (2.55x)** |
| MSFT | 1,215,912 | 43.7 (1.00x) | 35.5 (1.23x) | **13.7 (3.20x)** | 14.6 (3.00x) |
| INTC | 749,489 | 35.2 (1.00x) | 26.5 (1.32x) | **12.6 (2.80x)** | 12.9 (2.73x) |

### Replay + top-of-book query on every event

The access pattern that actually matters — a book nobody reads is not a useful
benchmark, and the designs differ more in read cost than in write cost.

| symbol | map | intrusive | flat | hybrid |
|---|---:|---:|---:|---:|
| AAPL | 54.5 (1.00x) | 59.8 (0.91x) | 22.7 (2.40x) | **21.8 (2.50x)** |
| SPY | 35.5 (1.00x) | 28.9 (1.23x) | 18.8 (1.89x) | **17.2 (2.06x)** |
| MSFT | 44.0 (1.00x) | 36.1 (1.22x) | 18.9 (2.33x) | **17.6 (2.50x)** |
| INTC | 35.1 (1.00x) | 27.5 (1.28x) | **16.5 (2.13x)** | 17.6 (2.00x) |

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

### 1. The textbook intrusive book is the *worst* design on the most active symbol

This is the result that surprised me. The pooled, intrusive, zero-allocation
book — the design everyone describes as "the HFT one" — **loses to `std::map`
on AAPL** (0.96x, and 0.91x once you query the touch).

It is not the binary search. I assumed it was, stored the price inline in the
index vector to make the search cache-friendly, and it got *worse*: 56 → 66
ns/event. Doubling the element size doubled the thing that actually dominates.

The cost is the sorted vector's `memmove` on level create/destroy, and it is
enormous because **real books are deep**:

| symbol | mean live levels | max | level creates/event | elements shifted per create | memmove per replay |
|---|---:|---:|---:|---:|---:|
| AAPL | 4,652 | 5,320 | 0.122 | 1,946 | **5,732 MB** |
| MSFT | 3,463 | 3,813 | 0.040 | 1,334 | 1,043 MB |
| INTC | 1,576 | 1,723 | 0.014 | 576 | 96 MB |
| SPY | 699 | 756 | 0.051 | 238 | 416 MB |

The predictor is `level_creates_per_event × mean_depth`, and the ranking follows
it monotonically:

| symbol | predictor | memmove/event | intrusive vs map |
|---|---:|---:|---:|
| AAPL | 568 | 3.79 KB | 0.96x |
| MSFT | 139 | 0.86 KB | 1.23x |
| SPY | 36 | 0.20 KB | 1.29x |
| INTC | 22 | 0.13 KB | 1.32x |

AAPL moves 3.79 KB of memory *per event* purely to keep a vector sorted. That
is why the design crosses over from winning by ~30% to losing.

**The generalisation:** a sorted vector is the right level index only when the
book is shallow. Textbook descriptions of this design quietly assume a book tens
of levels deep. A real one is thousands, because of resting orders far from the
touch that never trade (see finding 3).

### 2. Direct addressing wins, and the hybrid gets it without giving up queue position

The flat grid is 2.5–3.6x the baseline because price lookup is arithmetic and
nothing ever shifts. But an aggregate-only grid throws away *which* orders are
resting at a level, and ITCH fills a level in strict time priority — so that
FIFO order **is** queue position, which the fill model in Phase 4 depends on.

The hybrid keeps the grid's addressing and restores the per-level intrusive
FIFO over a pooled level store. It costs ~0–6% versus flat on maintenance, and
is *faster* than flat on three of four symbols once the touch is queried every
event (checking `slot[hint] >= 0` is a single load; the bitmap variant extracts
a bit first). So the queue-position capability is effectively free.

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
