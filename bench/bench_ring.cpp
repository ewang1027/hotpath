// False-sharing experiment on the SPSC ring.
//
// The producer owns tail_ and the consumer owns head_. If those two atomics
// share a cache line, every publish invalidates the line the other core is
// spinning on, and the line ping-pongs between cores on every single message.
// Padding them onto separate lines removes that.
//
// This effect is large -- hundreds of nanoseconds of coherence traffic per
// message -- so it is cleanly measurable even with a 41.7 ns clock, and the
// comparison is a ratio on identical work. Note the padding here is 128 bytes,
// not the 64 you would use on x86: `sysctl hw.cachelinesize` reports 128 on
// Apple Silicon, and padding to 64 would leave the two atomics sharing a line.
#include "hotpath/bench/timing.hpp"
#include "hotpath/core/cache.hpp"
#include "hotpath/core/platform.hpp"
#include "hotpath/ipc/spsc_ring.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace hotpath;
using namespace hotpath::ipc;
using namespace hotpath::bench;

namespace {

struct Msg { std::uint64_t seq; std::uint64_t payload; };

template <Padding P, Caching C>
double run(std::size_t cap, std::uint64_t messages) {
  SpscRing<Msg, Ordering::AcqRel, P, C> ring(cap);
  std::atomic<bool> go{false};
  std::uint64_t sink = 0;

  std::thread prod([&] {
    request_performance_cores();
    while (!go.load(std::memory_order_acquire)) {}
    for (std::uint64_t i = 1; i <= messages; ++i) {
      Msg m{i, i * 2654435761ull};
      while (!ring.try_push(m)) {}
    }
  });
  std::thread cons([&] {
    request_performance_cores();
    while (!go.load(std::memory_order_acquire)) {}
    Msg m{};
    for (std::uint64_t n = 0; n < messages; ++n) {
      while (!ring.try_pop(m)) {}
      sink += m.payload;
    }
  });

  go.store(true, std::memory_order_release);
  const std::uint64_t t0 = now_ticks();
  prod.join();
  cons.join();
  const double ns = ticks_to_ns(now_ticks() - t0);
  do_not_optimize(sink);
  return ns / static_cast<double>(messages);
}

} // namespace

int main(int argc, char** argv) {
  std::size_t cap = 1u << 14;
  std::uint64_t messages = 20'000'000;
  int trials = 5;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--trials" && i + 1 < argc) trials = std::atoi(argv[++i]);
    else if (a == "--messages" && i + 1 < argc) messages = std::strtoull(argv[++i], nullptr, 10);
  }

  const auto info = PlatformInfo::query();
  std::printf("cache line (OS/compiled) : %zu / %zu %s\n", info.cache_line, kCacheLine,
              cache_line_covers_os() ? "" : "  *** UNDER-PADDED ***");
  std::printf("ring capacity            : %zu slots of %zu bytes\n", cap, sizeof(Msg));
  std::printf("messages per trial       : %" PRIu64 "\n", messages);
  std::printf("trials                   : %d\n\n", trials);

  // Four combinations, because padding and index-caching are two independent
  // mitigations for the same problem and only measuring both separates them.
  std::vector<double> pc, kc, pu, ku;
  for (int t = 0; t < trials; ++t) {
    pc.push_back(run<Padding::Padded, Caching::Cached>(cap, messages));
    kc.push_back(run<Padding::Packed, Caching::Cached>(cap, messages));
    pu.push_back(run<Padding::Padded, Caching::Uncached>(cap, messages));
    ku.push_back(run<Padding::Packed, Caching::Uncached>(cap, messages));
  }
  const Stats spc = summarize(pc), skc = summarize(kc);
  const Stats spu = summarize(pu), sku = summarize(ku);

  std::printf("  %-24s %-12s %10s %9s %12s\n", "padding", "index cache", "ns/msg", "+/-95%", "M msg/s");
  auto row = [](const char* pad, const char* cache, const Stats& s) {
    std::printf("  %-24s %-12s %10.2f %9.2f %12.2f\n", pad, cache, s.mean, s.ci95(), 1e3 / s.mean);
  };
  row("separate lines", "cached", spc);
  row("shared line", "cached", skc);
  row("separate lines", "uncached", spu);
  row("shared line", "uncached", sku);

  std::printf("\n  false sharing cost, index caching ON  : %.2fx (%+.1f ns/msg)\n",
              skc.mean / spc.mean, skc.mean - spc.mean);
  std::printf("  false sharing cost, index caching OFF : %.2fx (%+.1f ns/msg)\n",
              sku.mean / spu.mean, sku.mean - spu.mean);
  std::printf("  cost of dropping index caching (padded): %.2fx (%+.1f ns/msg)\n",
              spu.mean / spc.mean, spu.mean - spc.mean);
  return 0;
}
