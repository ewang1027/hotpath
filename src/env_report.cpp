// Prints the machine facts that the measurement methodology depends on.
// Run this first on any new machine: docs/METHODOLOGY.md quotes its output,
// and scripts/regenerate.sh re-runs it so those numbers cannot go stale.
#include "hotpath/core/cache.hpp"
#include "hotpath/core/clock.hpp"
#include "hotpath/core/platform.hpp"
#include "hotpath/bench/counters.hpp"

#include <cstdio>

int main() {
  using namespace hotpath;
  const auto info = PlatformInfo::query();
  const auto& tb = Timebase::instance();

  std::printf("== hotpath environment report ==\n\n");
  std::printf("cache line (OS)        : %zu bytes\n", info.cache_line);
  std::printf("cache line (compiled)  : %zu bytes  %s\n", kCacheLine,
              cache_line_matches_os() ? "[match]" : "[MISMATCH -- padding is wrong]");
  std::printf("page size              : %zu bytes\n", info.page_size);
  std::printf("performance cores      : %zu\n", info.perf_cores);
  std::printf("efficiency cores       : %zu\n", info.efficiency_cores);
  std::printf("L1d / L2               : %zu / %zu bytes\n", info.l1d_bytes, info.l2_bytes);
  std::printf("\n-- timebase --\n");
  std::printf("mach_timebase_info     : numer=%u denom=%u\n", tb.numer(), tb.denom());
  std::printf("resolution             : %.4f ns/tick (%.3f MHz)\n",
              tb.ns_per_tick(), 1000.0 / tb.ns_per_tick());

  // Demonstrate the quantisation problem rather than merely asserting it.
  constexpr int kProbe = 200000;
  int zeros = 0;
  std::uint64_t best = UINT64_MAX;
  for (int i = 0; i < kProbe; ++i) {
    const std::uint64_t a = now_ticks();
    const std::uint64_t b = now_ticks();
    const std::uint64_t d = b - a;
    if (d == 0) ++zeros;
    else if (d < best) best = d;
  }
  std::printf("back-to-back reads     : %.1f%% return a delta of ZERO (%d/%d)\n",
              100.0 * zeros / kProbe, zeros, kProbe);
  std::printf("smallest non-zero delta: %llu ticks (%.2f ns)\n",
              (unsigned long long)best, ticks_to_ns(best));
  std::printf("\n=> per-event latency below ~%.0f ns is NOT measurable here.\n",
              tb.ns_per_tick());
  std::printf("   See docs/METHODOLOGY.md for what this repo claims instead.\n");

  std::printf("\n-- instrumentation --\n");
  std::printf("counters linked        : %s\n",
              bench::instrumentation_active() ? "yes" : "no");
  const auto c = bench::read_counters();
  std::printf("startup allocations    : %llu (%llu bytes)\n",
              (unsigned long long)c.allocations, (unsigned long long)c.bytes_allocated);
  std::printf("startup syscalls       : %llu\n", (unsigned long long)c.syscalls);
  std::printf("qos -> performance     : %s\n",
              request_performance_cores() ? "requested" : "failed");
  return 0;
}
