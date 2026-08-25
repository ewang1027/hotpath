# Running this on Linux (and why it is worth doing)

This repo was developed on macOS / Apple Silicon, and `METHODOLOGY.md` spends
most of its length explaining what that platform *cannot* measure: a 41.7 ns
clock tick, no core pinning, no `perf`. Linux fixes all three. The port is
small, and one experiment in particular only becomes possible with a second
machine.

## What builds where

| target | status |
|---|---|
| macOS / arm64 | developed here |
| Linux / arm64 | **verified** — 66/66 tests pass, `./scripts/linux_build.sh` |
| Linux / x86-64 | **verified in CI** (`ubuntu-latest` is x86-64); locally, all 14 tools build and run under emulation |
| WSL2 on Windows | Linux, so yes |
| Native Windows + MSVC | no, and not worth it (see below) |

## The porting layer

Four files carry every platform difference, all behind `#ifdef __APPLE__`:

| file | macOS | Linux |
|---|---|---|
| `core/clock.hpp` | `mach_absolute_time`, 41.7 ns/tick | `clock_gettime(CLOCK_MONOTONIC)`, 1 ns/tick |
| `core/platform.hpp` | `sysctlbyname`; QoS *hint* toward P-cores | `sysconf`/sysfs; `sched_setaffinity`, a hard binding |
| `core/cache.hpp` | 128-byte lines | 64-byte lines |
| `src/alloc_counter.cpp` | dyld `__interpose` | symbol preemption + `dlsym(RTLD_NEXT)` |

Everything else — every `mmap`, the shared-memory ring, `std::atomic_ref`, all
the `__builtin_*` intrinsics, Catch2, Verilator — is already portable.

One check changed semantics in the process. `cache_line_matches_os()` became
`cache_line_covers_os()`, asserting `kCacheLine >= os_line` rather than
equality: over-padding wastes a little space and is harmless, under-padding
leaves the structures sharing a line, which is the actual bug. That distinction
only shows up once the same source builds for 64- and 128-byte lines.

## The experiment that needs your machine

`litmus_ring` currently ends with a claim it cannot check:

> This same binary compiled for x86-64 would be expected to report zero,
> because TSO forbids the reordering that arm64 permits. The bug would be
> invisible on an Intel box.

On Apple Silicon the deliberately-broken `memory_order_relaxed` ring reports
**1,463 premature reads and 544 torn reads per 10.5M messages**. x86-64 is
Total Store Order: it does not reorder store-store or load-load, so the same
source should report **exactly zero** — not "fewer", zero.

Same code, same test, opposite result, for a reason that is a property of the
ISA rather than of the program. That is a far stronger statement than either
machine can make alone, and it is the single most valuable thing to run on an
x86 box.

**Emulation cannot answer this.** qemu-user does not faithfully reproduce x86
memory ordering, so a clean result under emulation would be evidence of the
emulator, not of TSO. It needs real x86 hardware.

Two other results are worth re-running for the same reason:

- **False sharing.** On Apple Silicon, padding the SPSC ring's indices apart
  measured *no benefit at all* (`PERFORMANCE.md`), which I attributed to the
  P-cores sharing an L2. x86 cores have private L2s and 64-byte lines, so if
  that hypothesis is right, padding should start to matter there. A confirmed
  prediction beats a plausible story.
- **Tail latency.** With a 1 ns clock and `sched_setaffinity`, the p99/p99.9
  tick-to-trade distributions this repo declines to publish become measurable.

## On Windows

**Use WSL2.** It is Linux, so the port above is all that is needed.

- Keep the data on the Linux filesystem (`~/`), not `/mnt/c/`. The 9p bridge to
  the Windows filesystem would dominate every measurement in this repo.
- `.wslconfig` accepts `kernelCommandLine`, so `isolcpus` and `nohz_full` are
  available if you want them.
- WSL2 is a Hyper-V VM. Tail latency will include hypervisor scheduling — better
  than macOS, short of bare metal — and PMU access for `perf` is mostly absent.

**Native Windows with MSVC is not worth it.** It would need `CreateFileMapping`
in place of every `mmap`, `_byteswap_*` and `_BitScanReverse64` in place of the
compiler builtins, `_aligned_malloc` for the over-aligned allocations, and the
syscall counter has no equivalent at all short of Detours. Real work, for
nothing WSL2 does not already give you.

**The GPU is irrelevant.** Order book maintenance is pointer-chasing through a
linked structure with data-dependent branches and serial state — close to the
most GPU-hostile shape a workload can have. There is no CUDA here and adding
some would not help. The only thing a bigger CPU buys is parallelism across
symbols in the 25-symbol sweeps, which are embarrassingly parallel.

## Running it

```bash
# In WSL2 (or any Linux):
sudo apt install build-essential cmake ninja-build git
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build

./scripts/x86_report.sh          # the cross-platform comparison
```

`scripts/x86_report.sh` runs exactly the measurements whose answers should
differ between the two platforms, and prints the macOS figures beside them so
the comparison is immediate.

## Verifying without a second machine

`./scripts/linux_build.sh` builds and tests in a container. On arm64 that is
native and complete: **66/66 tests pass**, with the environment report showing
64-byte lines, 4 KB pages, a nanosecond clock and hard affinity.

`ARCH=amd64 ./scripts/linux_build.sh` cross-builds for x86-64 under emulation.
Two limits are worth stating, because both look like portability failures and
neither is:

- **The emulated compiler crashes on Catch2.** `g++` segfaults partway through
  building it under qemu, so the test suite cannot be built there. Every
  hotpath source compiles and all 14 tools run; the suite is built with
  `-DBUILD_TESTING=OFF` and validated for real on native x86 in CI.
- **Memory ordering is not reproducible under emulation.** qemu does not
  faithfully model x86's TSO, so a clean `litmus_ring` result there would be
  evidence of the emulator, not of the ISA. That experiment needs real
  hardware — which is the whole reason it is worth running on yours.

Build directories are per-architecture. Sharing one between arm64 and amd64
runs made the x86 link pick up an arm64 `libCatch2.a` and fail with "file in
wrong format", which looks exactly like a portability bug and is not one.
