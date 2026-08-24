// Memory-model litmus test for the SPSC ring.
//
// This is the experiment the hardware makes possible. x86-64 is TSO: it does
// not reorder store-store or load-load, so a ring that publishes with
// memory_order_relaxed instead of release/acquire works there *by accident* and
// the bug is invisible. arm64 is weakly ordered and will actually reorder, so
// the same code can be shown to break.
//
// Setup: the ring is zero-initialised and the run is a single lap (fewer
// messages than slots), so a slot that has not been published yet is
// distinguishable from one that has. The payload is 8 words that must all be
// equal and non-zero. A consumer that observes the published index before the
// payload writes land sees zeros, or a mix of old and new words -- a torn read.
//
// Under Ordering::AcqRel the release/acquire pair forbids that. Under
// Ordering::Relaxed nothing does.
#include "hotpath/core/platform.hpp"
#include "hotpath/ipc/spsc_ring.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace hotpath;
using namespace hotpath::ipc;

namespace {

struct Payload {
  std::uint64_t w[8];
};

struct Verdict {
  std::uint64_t received{0};
  std::uint64_t zero_slots{0};    // index visible before any payload write
  std::uint64_t torn{0};          // some words new, some stale
  std::uint64_t out_of_order{0};  // sequence went backwards
};

template <Ordering O>
Verdict run_once(std::size_t capacity_pow2, std::uint64_t messages) {
  SpscRing<Payload, O> ring(capacity_pow2);
  Verdict v{};
  std::atomic<bool> go{false};

  std::thread producer([&] {
    request_performance_cores();
    while (!go.load(std::memory_order_acquire)) { /* spin to start together */ }
    for (std::uint64_t i = 1; i <= messages; ++i) {
      Payload p;
      for (auto& w : p.w) w = i;
      while (!ring.try_push(p)) { /* single lap: should never spin */ }
    }
  });

  std::thread consumer([&] {
    request_performance_cores();
    while (!go.load(std::memory_order_acquire)) { }
    std::uint64_t last = 0;
    for (std::uint64_t n = 0; n < messages; ++n) {
      Payload p;
      while (!ring.try_pop(p)) { }
      ++v.received;
      const std::uint64_t first = p.w[0];
      bool uniform = true;
      for (const auto& w : p.w) if (w != first) { uniform = false; break; }
      if (first == 0 && uniform)      ++v.zero_slots;
      else if (!uniform)              ++v.torn;
      else {
        if (first <= last) ++v.out_of_order;
        last = first;
      }
    }
  });

  go.store(true, std::memory_order_release);
  producer.join();
  consumer.join();
  return v;
}

void report(const char* label, const Verdict& v) {
  const std::uint64_t bad = v.zero_slots + v.torn + v.out_of_order;
  std::printf("  %-28s received=%-10" PRIu64 " zero=%-8" PRIu64 " torn=%-8" PRIu64
              " ooo=%-8" PRIu64 " -> %s\n",
              label, v.received, v.zero_slots, v.torn, v.out_of_order,
              bad ? "VIOLATIONS OBSERVED" : "clean");
}

} // namespace

int main(int argc, char** argv) {
  std::size_t cap = 1u << 20;
  std::uint64_t msgs = (1u << 20) - 16;
  int rounds = 20;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--rounds" && i + 1 < argc) rounds = std::atoi(argv[++i]);
    else if (a == "--cap" && i + 1 < argc) cap = std::strtoull(argv[++i], nullptr, 10);
  }
  if (msgs >= cap) msgs = cap - 16;

  std::printf("litmus: SPSC ring memory ordering\n");
  std::printf("arch          : %s\n",
#if defined(__aarch64__)
              "arm64 (weakly ordered -- reordering is architecturally allowed)"
#elif defined(__x86_64__)
              "x86-64 (TSO -- store-store and load-load reordering NOT allowed; "
              "the relaxed variant is expected to pass here even though it is wrong)"
#else
              "unknown"
#endif
  );
  std::printf("capacity      : %zu slots, %" PRIu64 " messages/round (single lap)\n", cap, msgs);
  std::printf("payload       : %zu bytes, 8 words that must agree\n", sizeof(Payload));
  std::printf("rounds        : %d\n\n", rounds);

  Verdict acc_ok{}, acc_bad{};
  for (int r = 0; r < rounds; ++r) {
    const Verdict a = run_once<Ordering::AcqRel>(cap, msgs);
    acc_ok.received += a.received; acc_ok.zero_slots += a.zero_slots;
    acc_ok.torn += a.torn; acc_ok.out_of_order += a.out_of_order;

    const Verdict b = run_once<Ordering::Relaxed>(cap, msgs);
    acc_bad.received += b.received; acc_bad.zero_slots += b.zero_slots;
    acc_bad.torn += b.torn; acc_bad.out_of_order += b.out_of_order;
  }

  std::printf("aggregate over %d rounds:\n", rounds);
  report("release/acquire (correct)", acc_ok);
  report("relaxed (deliberately bad)", acc_bad);

  const std::uint64_t ok_bad = acc_ok.zero_slots + acc_ok.torn + acc_ok.out_of_order;
  const std::uint64_t bad_bad = acc_bad.zero_slots + acc_bad.torn + acc_bad.out_of_order;

  std::printf("\n");
  if (ok_bad != 0) {
    std::printf("FAIL: the release/acquire ring violated its invariant. That is a real bug.\n");
    return 1;
  }
  std::printf("release/acquire: clean, as required.\n");
  if (bad_bad == 0) {
    std::printf("relaxed: no violation observed in this run.\n");
    std::printf("  Note this is NOT evidence the relaxed version is correct -- it is\n");
    std::printf("  architecturally unsound on arm64 regardless of whether a given run\n");
    std::printf("  happens to expose it. Reordering windows are microarchitectural and\n");
    std::printf("  a quiet CPU may simply not open one. Try --rounds with a larger value.\n");
  } else {
    std::printf("relaxed: %" PRIu64 " invariant violations observed.\n", bad_bad);
    std::printf("  This same binary compiled for x86-64 would be expected to report zero,\n");
    std::printf("  because TSO forbids the reordering that arm64 permits. The bug would\n");
    std::printf("  be invisible on an Intel box.\n");
  }
  return 0;
}
