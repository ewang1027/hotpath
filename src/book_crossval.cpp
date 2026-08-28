// Phase 2 gate: three independent order book implementations must produce
// bit-identical state at EVERY event across a real trading day.
//
// This is the correctness argument the whole project rests on. Any single book
// implementation can be confidently wrong; three that disagree tell you exactly
// which event diverged, and three that agree over ~10^8 operations are unlikely
// to be wrong in the same way.
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

using namespace hotpath;
using namespace hotpath::book;

namespace {

// FNV-1a over every snapshot of every design, in order. Two runs that produce
// the same digest performed bit-identical work; a build flag or a container
// iteration order that leaked nondeterminism into the replay would change it.
struct Digest {
  std::uint64_t h{1469598103934665603ull};
  void feed(const void* p, std::size_t n) noexcept {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
  }
  void feed(const Snapshot& s) noexcept {
    feed(&s.nbid, sizeof s.nbid);
    feed(&s.nask, sizeof s.nask);
    feed(s.bid, sizeof(LevelView) * static_cast<std::size_t>(s.nbid));
    feed(s.ask, sizeof(LevelView) * static_cast<std::size_t>(s.nask));
  }
};

void dump(const char* tag, const Snapshot& s) {
  std::printf("  %-10s bids:", tag);
  for (int i = 0; i < s.nbid; ++i)
    std::printf(" [%u x%u n%u]", s.bid[i].price, s.bid[i].qty, s.bid[i].orders);
  std::printf("\n  %-10s asks:", "");
  for (int i = 0; i < s.nask; ++i)
    std::printf(" [%u x%u n%u]", s.ask[i].price, s.ask[i].qty, s.ask[i].orders);
  std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
  std::string path;
  std::uint64_t check_every = 1;   // 1 = compare after every single event
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--every" && i + 1 < argc) check_every = std::strtoull(argv[++i], nullptr, 10);
    else path = a;
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: book_crossval <SYM.tape> [--every N]\n");
    return 2;
  }

  request_performance_cores();
  Tape tape(path);
  const auto win = tape.price_window();

  std::printf("tape       : %s\n", path.c_str());
  std::printf("events     : %zu\n", tape.size());
  std::printf("flat window: $%.4f .. $%.4f  (%zu penny ticks, covers %.3f%% of adds)\n",
              win.lo / 1e4, win.hi / 1e4,
              static_cast<std::size_t>((win.hi - win.lo) / FlatBook::kTick) + 1,
              100.0 * win.covered);

  MapBook a;
  IntrusiveBook b(1u << 21, 1u << 20);
  FlatBook c(win.lo, win.hi, 1u << 21);
  HybridBook d(win.lo, win.hi, 1u << 21, 1u << 20);

  Snapshot sa{}, sb{}, sc{}, sd{};
  Digest digest;
  const BookEvent* ev = tape.events();
  std::uint64_t diverged = 0;

  for (std::size_t i = 0; i < tape.size(); ++i) {
    apply(a, ev[i]);
    apply(b, ev[i]);
    apply(c, ev[i]);
    apply(d, ev[i]);

    if (i % check_every) continue;
    a.snapshot(sa); b.snapshot(sb); c.snapshot(sc); d.snapshot(sd);
    digest.feed(sa); digest.feed(sb); digest.feed(sc); digest.feed(sd);
    if (!(sa == sb) || !(sa == sc) || !(sa == sd)) {
      if (++diverged <= 3) {
        std::printf("\nDIVERGENCE at event %zu (type=%d ref=%" PRIu64 " px=%u sh=%u)\n",
                    i, static_cast<int>(ev[i].type), ev[i].order_ref, ev[i].price, ev[i].shares);
        dump("map", sa);
        dump("intrusive", sb);
        dump("flat", sc);
        dump("hybrid", sd);
      }
    }
  }

  std::printf("\n-- results --\n");
  std::printf("digest          : %016llx\n", (unsigned long long)digest.h);
  std::printf("comparisons     : %" PRIu64 "\n",
              (static_cast<std::uint64_t>(tape.size()) + check_every - 1) / check_every);
  std::printf("divergences     : %" PRIu64 "\n", diverged);
  std::printf("final orders    : map=%zu intrusive=%zu flat=%zu hybrid=%zu\n",
              a.order_count(), b.order_count(), c.order_count(), d.order_count());
  std::printf("intrusive levels: %zu in use, rejected=%" PRIu64 "\n",
              b.levels_in_use(), b.rejected());
  std::printf("flat grid ops   : %" PRIu64 "  overflow ops: %" PRIu64 " (%.4f%%), rejected=%" PRIu64 "\n",
              c.grid_ops(), c.overflow_ops(),
              100.0 * static_cast<double>(c.overflow_ops()) /
                  static_cast<double>(c.grid_ops() + c.overflow_ops() ? c.grid_ops() + c.overflow_ops() : 1),
              c.rejected());

  const bool pass = diverged == 0 && b.rejected() == 0 && c.rejected() == 0 &&
                    d.rejected() == 0 &&
                    a.order_count() == b.order_count() &&
                    a.order_count() == c.order_count() &&
                    a.order_count() == d.order_count();
  std::printf("\nGATE: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
