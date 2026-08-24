# Measurement methodology

## The short version

This project was built on an Apple M5 Pro (arm64, macOS). **That machine cannot
measure the latency of its own hot path.** This document says what it can
measure instead, and why every number in `PERFORMANCE.md` is one of those things.

Regenerate every figure below with `./build/src/hotpath_env`.

## What the hardware actually is

```
cache line             : 128 bytes          (x86-64 is 64 -- see below)
page size              : 16384 bytes        (x86-64 is typically 4096)
performance cores      : 5
efficiency cores       : 10
L1d / L2               : 65536 / 8388608 bytes
mach_timebase_info     : numer=125 denom=3
resolution             : 41.6667 ns/tick (24.000 MHz)
back-to-back reads     : 79.2% return a delta of ZERO (158488/200000)
smallest non-zero delta: 1 tick (41.67 ns)
```

## Why per-event latency is not reported

The userspace clock ticks once every **41.667 ns**. A competent tick-to-trade
path is a few hundred nanoseconds end to end, and a single ITCH message parse is
tens of nanoseconds. Timing one message therefore yields 0 ticks or 1 tick — the
"distribution" you would plot is the quantiser's, not the code's. Measured
directly: **79.2% of back-to-back `mach_absolute_time()` calls return a delta of
exactly zero.**

macOS removes the rest of the usual toolkit as well:

| Technique | Linux | macOS / Apple Silicon |
|---|---|---|
| Cycle counter | `rdtsc` (~0.3 ns) | `CNTVCT_EL0` at 24 MHz (41.7 ns) |
| Core isolation | `isolcpus`, `nohz_full` | none |
| Thread pinning | `sched_setaffinity` | `THREAD_AFFINITY_POLICY` is accepted and **ignored** |
| Hardware counters | `perf stat` | Instruments only, no CLI equivalent |
| Huge pages | `madvise(MADV_HUGEPAGE)` | not controllable |

The only scheduling lever available is the QoS class, which *hints* the thread
toward the P-core cluster (`pthread_set_qos_class_self_np`). It is a hint, not a
guarantee, and the machine is running a desktop the whole time.

**So this repo publishes no p50/p99/p99.9 tick-to-trade figure.** Any such number
produced on this hardware would be fabricated, and the fabrication would be
obvious to anyone who has done this work.

## What is claimed instead

### 1. Invariants — proofs, not measurements

A counter that reads zero is a proof regardless of clock resolution. Three
properties are enforced by tests that fail CI:

- **Zero heap allocations in steady state.** Global `operator new`/`delete` are
  replaced and counted (including the aligned C++17 overloads — every
  cache-line-padded type in this repo is over-aligned and would otherwise
  allocate through a path the counter never sees).
- **Zero syscalls in steady state.** Counted by dyld `__interpose` on the
  libSystem entry points. The input is `mmap`ed once at startup, so the parse
  loop performs no I/O at all.
- **Bit-exact determinism.** Identical input produces an identical output hash
  across repeated runs and across `-O0`/`-O3`.

> **The counters verify themselves.** `alloc_counting_active()` and
> `syscall_counting_active()` each perform a real probe on first call and report
> whether the counter actually moved. This is not ceremony: dyld silently
> ignores `__interpose` from a *static* archive, which left the syscall counter
> reading zero forever while every invariant assertion passed vacuously. The
> instrumentation is built `SHARED` for that reason. The probes must also be
> `inline` in the header, because dyld does not apply an image's own
> interposition to calls originating inside that same image.

### 2. Throughput and amortised cost

Over 10^8 messages a run spans seconds, so a 41.7 ns tick contributes no
meaningful error to the total. `total_time / N` is sound; the per-event
*distribution* is not.

### 3. Relative comparisons on identical input

The order-book design study reports A-vs-B on the same message stream. Even
where the absolute number carries quantisation error, the *ratio* is valid — both
sides pay the same error. This is the form most of `PERFORMANCE.md` takes.

### 4. Batch-level stalls

Per-1000-message batch times still carry signal at 41.7 ns resolution, because
the events worth catching — an allocation, a rehash, a page fault — cost
microseconds. A fat batch is visible even when a fast one is unmeasurable.

## What would change on x86 Linux

Running the same code on a bare-metal Linux box with `isolcpus`, `nohz_full`,
huge pages and `rdtsc` would make genuine p99/p99.9 tick-to-trade distributions
available, and `perf stat` would replace inference about cache behaviour with
measured miss counts. The code is portable to that; only `kCacheLine`
(128 -> 64) and the clock source would change. This is a known gap, stated
rather than papered over.
