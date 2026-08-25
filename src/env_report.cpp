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
              cache_line_matches_os() ? "[exact]"
              : cache_line_covers_os() ? "[covers it -- over-padded, harmless]"
                                       : "[UNDER-PADDED -- false sharing remains]");
  std::printf("page size              : %zu bytes\n", info.page_size);
  std::printf("performance cores      : %zu\n", info.perf_cores);
  std::printf("efficiency cores       : %zu\n", info.efficiency_cores);
  std::printf("L1d / L2               : %zu / %zu bytes\n", info.l1d_bytes, info.l2_bytes);
  std::printf("affinity               : %s\n", affinity_mechanism());
  std::printf("\n-- timebase --\n");
  std::printf("clock source           : %s\n", tb.source());
  std::printf("nominal tick           : %.4f ns (numer=%u denom=%u)\n",
              tb.ns_per_tick(), tb.numer(), tb.denom());

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
  // The nominal tick is not the binding constraint -- what the clock actually
  // resolves is. On macOS/arm64 the two agree at 41.7 ns. On Linux the nominal
  // tick is 1 ns but a virtualised clocksource can still land far above that,
  // so report the measured figure and let it speak.
  const double measured = ticks_to_ns(best);
  std::printf("\n=> effective resolution is %.1f ns (nominal %.1f).\n",
              measured, tb.ns_per_tick());
  if (measured > 10.0) {
    std::printf("   Per-event latency below that is NOT measurable here; this repo\n");
    std::printf("   reports amortised cost, ratios and clock-free invariants instead.\n");
    std::printf("   See docs/METHODOLOGY.md.\n");
  } else {
    std::printf("   Fine enough for per-event tail distributions -- the measurement\n");
    std::printf("   docs/METHODOLOGY.md declines to make on Apple Silicon.\n");
  }

  std::printf("\n-- instrumentation --\n");
  std::printf("counters linked        : %s\n",
              bench::instrumentation_active() ? "yes" : "no");
  const auto c = bench::read_counters();
  std::printf("startup allocations    : %llu (%llu bytes)\n",
              (unsigned long long)c.allocations, (unsigned long long)c.bytes_allocated);
  std::printf("startup syscalls       : %llu\n", (unsigned long long)c.syscalls);
  std::printf("affinity request       : %s (%s)\n",
              request_performance_cores() ? "ok" : "failed", affinity_mechanism());
  return 0;
}
