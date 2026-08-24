// Order book design study.
//
// What is measured, and why it is measurable on this hardware: each trial
// replays the entire tape (10^6 events) so the run spans hundreds of
// milliseconds. A 41.7 ns clock tick contributes no meaningful error to a
// total that large, and the designs are compared on byte-identical input, so
// the ratio between them is sound even where the absolute figure is not.
// Per-event latency distributions are NOT reported -- see docs/METHODOLOGY.md.
#include "hotpath/bench/counters.hpp"
#include "hotpath/bench/timing.hpp"
#include "hotpath/book/flat_book.hpp"
#include "hotpath/book/hybrid_book.hpp"
#include "hotpath/book/intrusive_book.hpp"
#include "hotpath/book/map_book.hpp"
#include "hotpath/book/tape.hpp"
#include "hotpath/core/platform.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <memory>
#include <vector>

using namespace hotpath;
using namespace hotpath::book;
using namespace hotpath::bench;

namespace {

struct Result {
  std::string name;
  Stats ns_per_event;     // across trials
  double msgs_per_sec;
  std::uint64_t allocations;
  std::uint64_t bytes;
};

// `query_top` models a strategy that reads the touch after every update, which
// is the access pattern that actually matters -- a book nobody looks at is not
// a useful benchmark, and the designs differ far more in read cost than in
// write cost.
template <typename MakeBook>
Result run(const std::string& name, const Tape& tape, int trials, bool query_top,
           MakeBook make) {
  const BookEvent* ev = tape.events();
  const std::size_t n = tape.size();

  {   // warm the page cache and the branch predictors; discard
    auto b = make();
    for (std::size_t i = 0; i < n; ++i) apply(*b, ev[i]);
  }

  std::vector<double> per_trial;
  per_trial.reserve(static_cast<std::size_t>(trials));
  std::uint64_t allocs = 0, bytes = 0;

  for (int t = 0; t < trials; ++t) {
    auto b = make();                      // construction excluded from timing
    CounterScope cs;
    const std::uint64_t t0 = now_ticks();
    if (query_top) {
      Price sink = 0;
      for (std::size_t i = 0; i < n; ++i) {
        apply(*b, ev[i]);
        sink += b->best_bid() ^ b->best_ask();
      }
      do_not_optimize(sink);
    } else {
      for (std::size_t i = 0; i < n; ++i) apply(*b, ev[i]);
    }
    const double ns = ticks_to_ns(now_ticks() - t0);
    const auto d = cs.delta();
    if (t == 0) { allocs = d.allocations; bytes = d.bytes_allocated; }
    per_trial.push_back(ns / static_cast<double>(n));
  }

  Result r;
  r.name = name;
  r.ns_per_event = summarize(per_trial);
  r.msgs_per_sec = 1e9 / r.ns_per_event.mean;
  r.allocations = allocs;
  r.bytes = bytes;
  return r;
}

void print_table(const char* title, const std::vector<Result>& rs, std::size_t events) {
  std::printf("\n%s\n", title);
  std::printf("  %-11s %10s %9s %12s %14s %12s\n",
              "design", "ns/event", "+/-95%", "M events/s", "allocations", "alloc bytes");
  const double base = rs.empty() ? 1.0 : rs.front().ns_per_event.mean;
  for (const auto& r : rs) {
    std::printf("  %-11s %10.1f %9.2f %12.2f %14" PRIu64 " %12" PRIu64 "   %.2fx\n",
                r.name.c_str(), r.ns_per_event.mean, r.ns_per_event.ci95(),
                r.msgs_per_sec / 1e6, r.allocations, r.bytes,
                base / r.ns_per_event.mean);
  }
  std::printf("  (%zu events per trial, %zu trials)\n", events, rs.empty() ? 0 : rs.front().ns_per_event.n);
}

} // namespace

int main(int argc, char** argv) {
  std::string path;
  int trials = 5;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--trials" && i + 1 < argc) trials = std::atoi(argv[++i]);
    else path = a;
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: bench_book <SYM.tape> [--trials N]\n");
    return 2;
  }

  request_performance_cores();
  Tape tape(path);
  const auto win = tape.price_window();

  std::printf("tape        : %s\n", path.c_str());
  std::printf("events      : %zu\n", tape.size());
  std::printf("flat window : $%.2f .. $%.2f (%zu ticks, %.2f%% of adds inside)\n",
              win.lo / 1e4, win.hi / 1e4,
              static_cast<std::size_t>((win.hi - win.lo) / FlatBook::kTick) + 1,
              100.0 * win.covered);

  for (int mode = 0; mode < 2; ++mode) {
    const bool q = mode == 1;
    std::vector<Result> rs;
    rs.push_back(run("map", tape, trials, q,
                     [] { return std::make_unique<MapBook>(); }));
    rs.push_back(run("intrusive", tape, trials, q,
                     [] { return std::make_unique<IntrusiveBook>(1u << 21, 1u << 20); }));
    rs.push_back(run("flat", tape, trials, q,
                     [&win] { return std::make_unique<FlatBook>(win.lo, win.hi, 1u << 21); }));
    rs.push_back(run("hybrid", tape, trials, q,
                     [&win] { return std::make_unique<HybridBook>(win.lo, win.hi, 1u << 21, 1u << 20); }));
    print_table(q ? "-- replay + top-of-book query every event --"
                  : "-- replay only (book maintenance) --",
                rs, tape.size());
  }
  return 0;
}
