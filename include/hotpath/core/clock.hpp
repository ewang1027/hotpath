#pragma once
#include <cstdint>
#include <mach/mach_time.h>

namespace hotpath {

// ---------------------------------------------------------------------------
// The single most important fact about measurement on this machine.
//
// mach_absolute_time() on Apple Silicon ticks at 24 MHz -- mach_timebase_info
// returns numer=125 denom=3, i.e. 41.667 ns per tick. Measured on an M5 Pro,
// 90.7% of back-to-back mach_absolute_time() calls return a delta of ZERO.
//
// Consequence: per-event latency in the 100-500 ns range is NOT measurable
// here. Anything this repo reports is either (a) an amortized cost over many
// events, (b) a relative comparison on identical input, or (c) a provable
// invariant that needs no clock at all. See docs/METHODOLOGY.md.
// ---------------------------------------------------------------------------

class Timebase {
public:
  static const Timebase& instance() noexcept {
    static const Timebase tb;
    return tb;
  }

  [[nodiscard]] double ns_per_tick() const noexcept { return ns_per_tick_; }
  [[nodiscard]] std::uint32_t numer() const noexcept { return info_.numer; }
  [[nodiscard]] std::uint32_t denom() const noexcept { return info_.denom; }

  [[nodiscard]] double ticks_to_ns(std::uint64_t ticks) const noexcept {
    return static_cast<double>(ticks) * ns_per_tick_;
  }

private:
  Timebase() noexcept {
    mach_timebase_info(&info_);
    ns_per_tick_ = static_cast<double>(info_.numer) / static_cast<double>(info_.denom);
  }
  mach_timebase_info_data_t info_{};
  double ns_per_tick_{1.0};
};

// Raw tick read. Deliberately not converted to ns at the call site -- the
// conversion is a multiply we do not want inside a measured region.
[[nodiscard]] inline std::uint64_t now_ticks() noexcept {
  return mach_absolute_time();
}

[[nodiscard]] inline double ticks_to_ns(std::uint64_t ticks) noexcept {
  return Timebase::instance().ticks_to_ns(ticks);
}

} // namespace hotpath
