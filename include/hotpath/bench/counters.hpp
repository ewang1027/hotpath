#pragma once
#include <cstddef>
#include <cstdint>
#include <new>
#include <unistd.h>

namespace hotpath::bench {

// ---------------------------------------------------------------------------
// Provable invariants.
//
// Because this hardware cannot measure a 300 ns hot path (see core/clock.hpp),
// the strongest claims available to us are the binary ones: the hot path
// allocates zero times, issues zero syscalls, and takes zero locks. Those are
// verifiable regardless of clock resolution -- a counter that reads 0 is a
// proof, not a measurement.
//
// Allocation counting: global operator new/delete are overridden in
// src/alloc_counter.cpp.
// Syscall counting: dyld interposition on the libSystem entry points in
// src/alloc_counter.cpp. Both are opt-in via linking hotpath_instrument,
// so a build we intend to time is never carrying the instrumentation.
// ---------------------------------------------------------------------------

struct Counters {
  std::uint64_t allocations;
  std::uint64_t deallocations;
  std::uint64_t bytes_allocated;
  std::uint64_t syscalls;
};

// Snapshot of the process-wide counters.
[[nodiscard]] Counters read_counters() noexcept;

// A counter that is never wired up reads zero forever, and every invariant
// check built on it "passes" while proving nothing. These are not flags -- each
// performs a real probe on first call (allocate and free; issue a harmless
// close(-1)) and reports whether the counter actually moved.
//
// This is not hypothetical: the syscall counter uses dyld __interpose, which
// is silently ignored when the instrumentation is linked as a static archive.
// It only takes effect from a dylib loaded at launch, which is why
// hotpath_instrument is built SHARED.
// These MUST be inline (i.e. compiled into the caller's image) rather than
// living in the instrumentation dylib. dyld does not apply an image's own
// __interpose entries to calls originating inside that same image -- that is
// how it avoids interposing itself into infinite recursion. A probe compiled
// into libhotpath_instrument.dylib therefore calls the real close() every
// time and reports "inactive" even when interposition is working perfectly
// for every other caller. Found the hard way.
[[nodiscard]] inline bool alloc_counting_active() noexcept {
  static const bool ok = [] {
    const std::uint64_t before = read_counters().allocations;
    void* p = ::operator new(64);
    asm volatile("" : : "r,m"(p) : "memory");
    ::operator delete(p);
    return read_counters().allocations > before;
  }();
  return ok;
}

[[nodiscard]] inline bool syscall_counting_active() noexcept {
  static const bool ok = [] {
    const std::uint64_t before = read_counters().syscalls;
    // Cheapest interposed call with no side effect: fails with EBADF.
    (void)::close(-1);
    return read_counters().syscalls > before;
  }();
  return ok;
}

// Both of the above.
[[nodiscard]] inline bool instrumentation_active() noexcept {
  return alloc_counting_active() && syscall_counting_active();
}

// RAII: snapshot on construction, diff on demand.
class CounterScope {
public:
  CounterScope() noexcept : start_(read_counters()) {}

  [[nodiscard]] Counters delta() const noexcept {
    const Counters now = read_counters();
    return Counters{
        now.allocations      - start_.allocations,
        now.deallocations    - start_.deallocations,
        now.bytes_allocated  - start_.bytes_allocated,
        now.syscalls         - start_.syscalls,
    };
  }

  void reset() noexcept { start_ = read_counters(); }

private:
  Counters start_;
};

} // namespace hotpath::bench
