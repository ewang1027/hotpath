// Threaded tick-to-trade pipeline.
//
//   feed thread      : walk the tape, publish market-data events   -> ring1
//   strategy thread  : maintain the book, run the maker, emit fills -> ring2
//   gateway thread   : consume fills (stands in for an order gateway)
//
// Two questions, both answered by measurement rather than assumed:
//
//   1. Does crossing the ring preserve semantics exactly? The pipeline and the
//      single-threaded path run the SAME MarketMaker code, and their fill
//      streams are hashed and compared. Identical digests mean the lock-free
//      handoff changed nothing.
//
//   2. Is pipelining actually faster? A ring hop costs ~25 ns/message (measured
//      in bench_ring) and the book+strategy stage costs ~16-20 ns/event, so
//      there is a real possibility the hop costs more than the work it
//      parallelises. Reported either way.
#include "hotpath/bench/counters.hpp"
#include "hotpath/bench/timing.hpp"
#include "hotpath/book/tape.hpp"
#include "hotpath/core/platform.hpp"
#include "hotpath/ipc/spsc_ring.hpp"
#include "hotpath/sim/market_maker.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace hotpath;
using namespace hotpath::book;
using namespace hotpath::sim;
using namespace hotpath::bench;
using namespace hotpath::ipc;

namespace {

struct FeedMsg { BookEvent ev; std::uint32_t eof; };
struct FillMsg { Fill fill;    std::uint32_t eof; };

// FNV-1a over the fill stream, in order.
struct Digest {
  std::uint64_t h{1469598103934665603ull};
  void feed(const Fill& f) noexcept {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&f);
    for (std::size_t i = 0; i < sizeof(Fill); ++i) { h ^= b[i]; h *= 1099511628211ull; }
  }
};

struct Occupancy {
  std::uint64_t bucket[5]{};   // 0%, <25%, <50%, <75%, >=75%
  std::uint64_t samples{0};
  void sample(std::size_t used, std::size_t cap) noexcept {
    ++samples;
    if (used == 0) { ++bucket[0]; return; }
    const double f = static_cast<double>(used) / static_cast<double>(cap);
    if (f < 0.25) ++bucket[1];
    else if (f < 0.50) ++bucket[2];
    else if (f < 0.75) ++bucket[3];
    else ++bucket[4];
  }
  void print(const char* name) const {
    if (!samples) return;
    std::printf("  %-10s", name);
    const char* lbl[5] = {"empty", "<25%", "<50%", "<75%", ">=75%"};
    for (int i = 0; i < 5; ++i)
      std::printf(" %s=%.1f%%", lbl[i], 100.0 * static_cast<double>(bucket[i]) /
                                            static_cast<double>(samples));
    std::printf("\n");
  }
};

struct Run {
  double ns_per_event;
  std::uint64_t fills;
  std::uint64_t digest;
  std::uint64_t allocations;
  std::uint64_t overflow_levels;   // the only legitimate allocation source
};

Run run_single(const Tape& tape, Price lo, Price hi, Qty size) {
  MarketMaker mm(lo, hi, size);
  Digest d;
  std::uint64_t fills = 0;
  const BookEvent* ev = tape.events();

  CounterScope cs;
  const std::uint64_t t0 = now_ticks();
  for (std::size_t i = 0; i < tape.size(); ++i)
    mm.on_event(ev[i], [&](const Fill& f) { d.feed(f); ++fills; });
  const double ns = ticks_to_ns(now_ticks() - t0);
  const auto delta = cs.delta();
  return Run{ns / static_cast<double>(tape.size()), fills, d.h, delta.allocations,
             mm.book().overflow_levels()};
}

Run run_pipelined(const Tape& tape, Price lo, Price hi, Qty size,
                  std::size_t ring_pow2, Occupancy* occ1, Occupancy* occ2) {
  SpscRing<FeedMsg> ring1(ring_pow2);
  SpscRing<FillMsg> ring2(ring_pow2);
  std::atomic<bool> go{false};
  std::atomic<std::uint64_t> alloc_strategy{0};
  std::atomic<std::uint64_t> overflow_strategy{0};
  Digest digest;
  std::uint64_t fills = 0;

  const BookEvent* ev = tape.events();
  const std::size_t n = tape.size();

  std::thread feed([&] {
    request_performance_cores();
    while (!go.load(std::memory_order_acquire)) {}
    for (std::size_t i = 0; i < n; ++i) {
      FeedMsg m{ev[i], 0};
      while (!ring1.try_push(m)) {}
    }
    FeedMsg eof{}; eof.eof = 1;
    while (!ring1.try_push(eof)) {}
  });

  std::thread strategy([&] {
    request_performance_cores();
    MarketMaker mm(lo, hi, size);
    while (!go.load(std::memory_order_acquire)) {}
    CounterScope cs;
    std::uint64_t seen = 0;
    for (;;) {
      FeedMsg m{};
      while (!ring1.try_pop(m)) {}
      if (m.eof) break;
      mm.on_event(m.ev, [&](const Fill& f) {
        FillMsg fm{f, 0};
        while (!ring2.try_push(fm)) {}
      });
      if (occ1 && ((++seen & 0x3FF) == 0)) occ1->sample(ring1.size_approx(), ring1.capacity());
    }
    alloc_strategy.store(cs.delta().allocations, std::memory_order_relaxed);
    overflow_strategy.store(mm.book().overflow_levels(), std::memory_order_relaxed);
    FillMsg eof{}; eof.eof = 1;
    while (!ring2.try_push(eof)) {}
  });

  std::thread gateway([&] {
    request_performance_cores();
    while (!go.load(std::memory_order_acquire)) {}
    std::uint64_t seen = 0;
    for (;;) {
      FillMsg m{};
      while (!ring2.try_pop(m)) {}
      if (m.eof) break;
      digest.feed(m.fill);
      ++fills;
      if (occ2 && ((++seen & 0x3F) == 0)) occ2->sample(ring2.size_approx(), ring2.capacity());
    }
  });

  go.store(true, std::memory_order_release);
  const std::uint64_t t0 = now_ticks();
  feed.join(); strategy.join(); gateway.join();
  const double ns = ticks_to_ns(now_ticks() - t0);
  return Run{ns / static_cast<double>(n), fills, digest.h,
             alloc_strategy.load(std::memory_order_relaxed),
             overflow_strategy.load(std::memory_order_relaxed)};
}

} // namespace

int main(int argc, char** argv) {
  std::string path;
  Qty size = 100;
  int trials = 5;
  std::size_t ring_pow2 = 1u << 14;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--size" && i + 1 < argc) size = static_cast<Qty>(std::atoi(argv[++i]));
    else if (a == "--trials" && i + 1 < argc) trials = std::atoi(argv[++i]);
    else if (a == "--ring" && i + 1 < argc) ring_pow2 = std::strtoull(argv[++i], nullptr, 10);
    else path = a;
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: pipeline <SYM.tape> [--size N] [--trials N] [--ring SLOTS]\n");
    return 2;
  }

  request_performance_cores();
  Tape tape(path);
  const auto win = tape.price_window();

  std::printf("tape        : %s\n", path.c_str());
  std::printf("events      : %zu\n", tape.size());
  std::printf("ring        : %zu slots (feed msg %zu B, fill msg %zu B)\n",
              ring_pow2, sizeof(FeedMsg), sizeof(FillMsg));
  std::printf("trials      : %d\n", trials);

  std::vector<double> st, pt;
  Run rs{}, rp{};
  Occupancy occ1{}, occ2{};
  for (int t = 0; t < trials; ++t) {
    rs = run_single(tape, win.lo, win.hi, size);
    st.push_back(rs.ns_per_event);
    rp = run_pipelined(tape, win.lo, win.hi, size, ring_pow2,
                       t == 0 ? &occ1 : nullptr, t == 0 ? &occ2 : nullptr);
    pt.push_back(rp.ns_per_event);
  }
  const Stats ss = summarize(st), sp = summarize(pt);

  std::printf("\n-- semantics --\n");
  std::printf("  single-threaded : %" PRIu64 " fills, digest %016llx\n",
              rs.fills, (unsigned long long)rs.digest);
  std::printf("  pipelined       : %" PRIu64 " fills, digest %016llx\n",
              rp.fills, (unsigned long long)rp.digest);
  const bool same = rs.fills == rp.fills && rs.digest == rp.digest;
  std::printf("  fill streams    : %s\n",
              same ? "IDENTICAL -- the ring handoff preserved semantics exactly"
                   : "*** DIVERGED ***");

  // The dense grid path allocates nothing. The std::map overflow -- for
  // sub-penny prices and orders outside the price band -- does, once per
  // overflow level created. Reporting a raw non-zero count as a violation would
  // be misleading, so the invariant is stated against the expected source.
  std::printf("\n-- steady-state invariants --\n");
  auto alloc_line = [](const char* what, std::uint64_t allocs, std::uint64_t overflow) {
    const bool ok = allocs <= overflow;
    std::printf("  %-30s %" PRIu64 " allocations, %" PRIu64 " overflow levels -> %s\n",
                what, allocs, overflow,
                ok ? "[OK: dense path allocation-free]" : "[VIOLATION: unexplained allocation]");
    return ok;
  };
  const bool inv_a = alloc_line("single-threaded", rs.allocations, rs.overflow_levels);
  const bool inv_b = alloc_line("strategy thread", rp.allocations, rp.overflow_levels);

  std::printf("\n-- throughput --\n");
  std::printf("  %-18s %10s %9s %12s\n", "configuration", "ns/event", "+/-95%", "M events/s");
  std::printf("  %-18s %10.2f %9.2f %12.2f\n", "single-threaded", ss.mean, ss.ci95(), 1e3 / ss.mean);
  std::printf("  %-18s %10.2f %9.2f %12.2f\n", "3-stage pipeline", sp.mean, sp.ci95(), 1e3 / sp.mean);
  std::printf("\n  pipelining is %.2fx %s (%+.1f ns/event)\n",
              sp.mean > ss.mean ? sp.mean / ss.mean : ss.mean / sp.mean,
              sp.mean > ss.mean ? "SLOWER" : "faster", sp.mean - ss.mean);

  std::printf("\n-- ring occupancy (identifies the bottleneck stage) --\n");
  occ1.print("feed->strat");
  occ2.print("strat->gw");
  std::printf("  A ring that sits empty means its producer is the bottleneck;\n");
  std::printf("  a ring that sits full means its consumer is.\n");
  return (same && inv_a && inv_b) ? 0 : 1;
}
