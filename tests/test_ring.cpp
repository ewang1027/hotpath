#include "hotpath/core/cache.hpp"
#include "hotpath/core/platform.hpp"
#include "hotpath/ipc/spsc_ring.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace hotpath;
using namespace hotpath::ipc;

TEST_CASE("ring: compile-time cache line matches the OS", "[ring]") {
  // Every alignas() in the ring pads to kCacheLine. If that constant disagrees
  // with the hardware, the padding is to the wrong boundary and the
  // false-sharing experiment silently measures nothing.
  REQUIRE(cache_line_matches_os());
}

TEST_CASE("ring: single-threaded push/pop semantics", "[ring]") {
  SpscRing<int> r(8);
  int v = 0;
  REQUIRE_FALSE(r.try_pop(v));          // empty
  REQUIRE(r.try_push(1));
  REQUIRE(r.try_push(2));
  REQUIRE(r.try_pop(v)); REQUIRE(v == 1);
  REQUIRE(r.try_pop(v)); REQUIRE(v == 2);
  REQUIRE_FALSE(r.try_pop(v));
}

TEST_CASE("ring: reports full rather than overwriting", "[ring]") {
  SpscRing<int> r(4);                    // 4 slots, one reserved => 3 usable
  REQUIRE(r.try_push(1));
  REQUIRE(r.try_push(2));
  REQUIRE(r.try_push(3));
  REQUIRE_FALSE(r.try_push(4));          // must refuse, not clobber
  int v = 0;
  REQUIRE(r.try_pop(v)); REQUIRE(v == 1);
  REQUIRE(r.try_push(4));                // space freed
}

// Threaded FIFO + no-loss check. Run under ThreadSanitizer in CI; TSan
// understands release/acquire, so a missing fence here is reported as a data
// race rather than having to be caught statistically.
TEST_CASE("ring: concurrent producer/consumer preserves order and loses nothing",
          "[ring][thread]") {
  constexpr std::uint64_t kN = 2'000'000;
  SpscRing<std::uint64_t> r(1u << 12);   // deliberately smaller than kN: wraps many times
  std::atomic<bool> ok{true};

  std::thread prod([&] {
    for (std::uint64_t i = 1; i <= kN; ++i) while (!r.try_push(i)) {}
  });
  std::thread cons([&] {
    std::uint64_t expect = 1, v = 0;
    for (std::uint64_t n = 0; n < kN; ++n) {
      while (!r.try_pop(v)) {}
      if (v != expect++) { ok.store(false); return; }
    }
  });
  prod.join();
  cons.join();
  REQUIRE(ok.load());
  REQUIRE(r.size_approx() == 0);
}

TEST_CASE("ring: wraps correctly across many laps", "[ring]") {
  SpscRing<std::uint64_t> r(4);
  std::uint64_t v = 0;
  for (std::uint64_t i = 1; i <= 10000; ++i) {
    REQUIRE(r.try_push(i));
    REQUIRE(r.try_pop(v));
    REQUIRE(v == i);
  }
}

// Regression: over-aligned allocations whose size is not a multiple of the
// alignment. std::aligned_alloc rejects those, so an operator new built on it
// throws bad_alloc for any small cache-line-padded object. Caught by the
// 4-slot ring above; pinned here so it cannot come back.
TEST_CASE("aligned operator new accepts sizes that are not multiples of the alignment",
          "[ring][invariant]") {
  for (std::size_t n : {1u, 8u, 16u, 17u, 127u, 129u}) {
    void* p = ::operator new(n, std::align_val_t{kCacheLine});
    REQUIRE(p != nullptr);
    REQUIRE(reinterpret_cast<std::uintptr_t>(p) % kCacheLine == 0);
    ::operator delete(p, std::align_val_t{kCacheLine});
  }
}
