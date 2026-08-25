// Cross-process subscriber: consumes book events from a shared-memory ring,
// rebuilds the book and runs the market maker, then reports what it received
// and what it missed.
//
// The correctness claim is the same one the threaded pipeline makes, but across
// a real process boundary: with no gaps, the fill stream must hash identically
// to the in-process run. A gap is reported, never silently absorbed -- a feed
// handler that quietly drops messages produces a book that is wrong in a way
// nothing downstream can detect.
#include "hotpath/book/tape.hpp"
#include "hotpath/core/platform.hpp"
#include "hotpath/ipc/shm_ring.hpp"
#include "hotpath/sim/market_maker.hpp"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace hotpath;
using namespace hotpath::book;
using namespace hotpath::ipc;
using namespace hotpath::sim;

namespace {
struct Digest {
  std::uint64_t h{1469598103934665603ull};
  void mix(std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) { h ^= (v >> (8 * i)) & 0xff; h *= 1099511628211ull; }
  }
  void feed(const Fill& f) noexcept {
    mix(f.ts); mix(f.price); mix(f.qty);
    mix(static_cast<std::uint64_t>(f.side)); mix(f.queue_ahead_at_join);
    mix(f.stale ? 1u : 0u); mix(f.swept ? 1u : 0u);
  }
};
} // namespace

int main(int argc, char** argv) {
  std::string ring_path = "/tmp/hotpath.ring";
  Price lo = 0, hi = 0;
  Qty size = 100;
  std::uint64_t slow_spin = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--ring" && i + 1 < argc) ring_path = argv[++i];
    else if (a == "--lo" && i + 1 < argc) lo = static_cast<Price>(std::strtoul(argv[++i], nullptr, 10));
    else if (a == "--hi" && i + 1 < argc) hi = static_cast<Price>(std::strtoul(argv[++i], nullptr, 10));
    else if (a == "--size" && i + 1 < argc) size = static_cast<Qty>(std::atoi(argv[++i]));
    else if (a == "--slow" && i + 1 < argc) slow_spin = std::strtoull(argv[++i], nullptr, 10);
  }
  if (hi <= lo) { std::fprintf(stderr, "usage: shm_sub --lo P --hi P [--ring PATH] [--slow N]\n"); return 2; }

  request_performance_cores();
  auto ring = ShmRing::open(ring_path);
  const std::uint64_t me = ring.attach_reader();
  std::printf("subscriber %" PRIu64 ": %s (%" PRIu64 " slots)\n", me, ring_path.c_str(), ring.capacity());

  MarketMaker mm(lo, hi, size);
  Digest digest;
  BookEvent e{};
  std::uint64_t next = 0, gap = 0, received = 0, missed = 0, gaps = 0, fills = 0;
  bool started = false;
  auto t0 = std::chrono::steady_clock::now();

  for (;;) {
    const auto st = ring.try_read(next, &e, sizeof e, gap);
    if (st == ShmRing::Status::Ok) {
      if (!started) { t0 = std::chrono::steady_clock::now(); started = true; }
      ++received;
      mm.on_event(e, [&](const Fill& f) { digest.feed(f); ++fills; });
      for (std::uint64_t k = 0; k < slow_spin; ++k) __asm__ __volatile__("" ::: "memory");
    } else if (st == ShmRing::Status::Gap) {
      ++gaps; missed += gap;
    } else {
      const std::uint64_t eof = ring.eof();
      if (eof && next + 1 >= eof) break;
      std::this_thread::yield();
    }
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  std::printf("  received : %" PRIu64 "\n", received);
  std::printf("  missed   : %" PRIu64 " in %" PRIu64 " gap event(s)\n", missed, gaps);
  std::printf("  accounted: %" PRIu64 " of %" PRIu64 " published %s\n",
              received + missed, ring.published(),
              received + missed >= ring.published() ? "[OK]" : "[SHORT]");
  std::printf("  fills    : %" PRIu64 "   digest %016llx%s\n", fills,
              (unsigned long long)digest.h,
              missed ? "   (gapped: not comparable to the in-process run)" : "");
  std::printf("  rate     : %.2f M msg/s\n",
              secs > 0 ? static_cast<double>(received) / secs / 1e6 : 0.0);
  return 0;
}
