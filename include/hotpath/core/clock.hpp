#pragma once
#include <cstdint>

#if defined(__APPLE__)
#  include <mach/mach_time.h>
#else
#  include <ctime>
#endif

namespace hotpath {

// ---------------------------------------------------------------------------
// The single most important fact about measurement is what the clock can see,
// and it differs by an order of magnitude between the two platforms this builds
// on.
//
// macOS / Apple Silicon: mach_absolute_time() ticks at 24 MHz --
// mach_timebase_info returns numer=125 denom=3, i.e. 41.667 ns per tick.
// Measured on an M5 Pro, 79% of back-to-back calls return a delta of ZERO.
// Per-event latency in the 100-500 ns range is simply NOT measurable there, so
// this repo reports amortised cost, relative comparisons and clock-free
// invariants instead. See docs/METHODOLOGY.md.
//
// Linux: CLOCK_MONOTONIC is nanosecond-resolution and vDSO-backed, so a tick IS
// a nanosecond and the conversion is the identity. That is the platform on
// which the per-event tail distributions this repo currently declines to
// publish actually become measurable. See docs/PORTING.md.
//
// The interface is identical on both; only the resolution changes, which is
// exactly the point.
// ---------------------------------------------------------------------------

class Timebase {
public:
  static const Timebase& instance() noexcept {
    static const Timebase tb;
    return tb;
  }

  [[nodiscard]] double ns_per_tick() const noexcept { return ns_per_tick_; }
  [[nodiscard]] std::uint32_t numer() const noexcept { return numer_; }
  [[nodiscard]] std::uint32_t denom() const noexcept { return denom_; }

  [[nodiscard]] double ticks_to_ns(std::uint64_t ticks) const noexcept {
    return static_cast<double>(ticks) * ns_per_tick_;
  }

  [[nodiscard]] const char* source() const noexcept {
#if defined(__APPLE__)
    return "mach_absolute_time";
#else
    return "clock_gettime(CLOCK_MONOTONIC)";
#endif
  }

private:
  Timebase() noexcept {
#if defined(__APPLE__)
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    numer_ = info.numer;
    denom_ = info.denom;
    ns_per_tick_ = static_cast<double>(info.numer) / static_cast<double>(info.denom);
#else
    // A tick is already a nanosecond.
    numer_ = 1;
    denom_ = 1;
    ns_per_tick_ = 1.0;
#endif
  }
  std::uint32_t numer_{1};
  std::uint32_t denom_{1};
  double ns_per_tick_{1.0};
};

// Raw tick read. Deliberately not converted to ns at the call site -- the
// conversion is a multiply we do not want inside a measured region.
[[nodiscard]] inline std::uint64_t now_ticks() noexcept {
#if defined(__APPLE__)
  return mach_absolute_time();
#else
  ::timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
         static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

[[nodiscard]] inline double ticks_to_ns(std::uint64_t ticks) noexcept {
  return Timebase::instance().ticks_to_ns(ticks);
}

} // namespace hotpath
