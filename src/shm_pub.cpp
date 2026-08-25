// Cross-process market-data publisher: replays a tape into a shared-memory ring.
//
// Pairs with shm_sub. Together they demonstrate the boundary that actually
// justifies a ring: the in-process pipeline measured SLOWER than single-threaded
// (docs/PERFORMANCE.md), because a ~15 ns hop bought nothing. A process boundary
// is not an optimisation you chose, and shared memory is how you cross it
// without copying through the kernel.
#include "hotpath/book/tape.hpp"
#include "hotpath/core/platform.hpp"
#include "hotpath/ipc/shm_ring.hpp"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace hotpath;
using namespace hotpath::book;
using namespace hotpath::ipc;

int main(int argc, char** argv) {
  std::string tape_path, ring_path = "/tmp/hotpath.ring";
  std::uint64_t capacity = 1u << 16;
  std::uint64_t wait_readers = 0;
  std::uint64_t pace_ns = 0;      // 0 = as fast as possible
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--ring" && i + 1 < argc) ring_path = argv[++i];
    else if (a == "--capacity" && i + 1 < argc) capacity = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--wait-readers" && i + 1 < argc) wait_readers = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--pace-ns" && i + 1 < argc) pace_ns = std::strtoull(argv[++i], nullptr, 10);
    else tape_path = a;
  }
  if (tape_path.empty()) {
    std::fprintf(stderr, "usage: shm_pub <SYM.tape> [--ring PATH] [--capacity N] "
                         "[--wait-readers N] [--pace-ns N]\n");
    return 2;
  }

  request_performance_cores();
  Tape tape(tape_path);
  auto ring = ShmRing::create(ring_path, capacity, sizeof(BookEvent));

  std::printf("publisher: %s\n", ring_path.c_str());
  std::printf("  events   : %zu\n", tape.size());
  std::printf("  ring     : %" PRIu64 " slots x %u bytes\n", ring.capacity(), ring.slot_bytes());
  if (wait_readers) {
    std::printf("  waiting for %" PRIu64 " subscriber(s)...\n", wait_readers);
    while (ring.readers() < wait_readers) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  const BookEvent* ev = tape.events();
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < tape.size(); ++i) {
    ring.publish(&ev[i], sizeof(BookEvent));
    if (pace_ns) {
      const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(pace_ns);
      while (std::chrono::steady_clock::now() < until) { /* spin: sleep is far too coarse */ }
    }
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  ring.mark_eof();

  std::printf("  published: %" PRIu64 " in %.3f s (%.2f M msg/s, %.1f ns/msg)\n",
              ring.published(), secs,
              static_cast<double>(tape.size()) / secs / 1e6,
              secs * 1e9 / static_cast<double>(tape.size()));
  std::printf("  the publisher never waited for a subscriber: this ring overwrites.\n");
  return 0;
}
