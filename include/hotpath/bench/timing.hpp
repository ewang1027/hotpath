#pragma once
#include "hotpath/core/clock.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace hotpath::bench {

// ---------------------------------------------------------------------------
// What we can honestly measure with a 41.667 ns tick.
//
// Not: per-message latency percentiles. A single message costs well under one
// tick, so a per-message timer reads 0 or 1 ticks and the "distribution" is
// pure quantisation noise.
//
// Instead:
//  - BatchTimer      : time N messages at once; amortised cost = total/N.
//                      With N large enough that the batch spans thousands of
//                      ticks, quantisation error becomes negligible.
//  - Batch stalls    : the *spread* across batches still carries signal. An
//                      allocation, a rehash or a page fault costs microseconds
//                      and shows up as a fat batch even at this resolution.
//  - Trials          : repeat the whole run; report mean +/- CI across trials
//                      so a design comparison is a statistical claim.
// ---------------------------------------------------------------------------

class BatchTimer {
public:
  void start() noexcept { t0_ = now_ticks(); }
  // Returns elapsed ticks for this batch and records it.
  std::uint64_t stop() noexcept {
    const std::uint64_t d = now_ticks() - t0_;
    batches_.push_back(d);
    return d;
  }
  void reserve(std::size_t n) { batches_.reserve(n); }
  void clear() noexcept { batches_.clear(); }

  [[nodiscard]] const std::vector<std::uint64_t>& batches() const noexcept { return batches_; }
  [[nodiscard]] std::uint64_t total_ticks() const noexcept {
    std::uint64_t s = 0;
    for (auto b : batches_) s += b;
    return s;
  }
  [[nodiscard]] double total_ns() const noexcept { return ticks_to_ns(total_ticks()); }

private:
  std::uint64_t t0_{0};
  std::vector<std::uint64_t> batches_;
};

struct Stats {
  double mean;
  double stddev;
  double min;
  double max;
  double p50;
  double p99;
  std::size_t n;

  // 95% CI half-width on the mean, normal approximation.
  [[nodiscard]] double ci95() const noexcept {
    return n > 1 ? 1.96 * stddev / std::sqrt(static_cast<double>(n)) : 0.0;
  }
};

inline Stats summarize(std::vector<double> xs) {
  Stats s{};
  s.n = xs.size();
  if (xs.empty()) return s;
  std::sort(xs.begin(), xs.end());
  double sum = 0.0;
  for (double x : xs) sum += x;
  s.mean = sum / static_cast<double>(xs.size());
  double acc = 0.0;
  for (double x : xs) acc += (x - s.mean) * (x - s.mean);
  s.stddev = xs.size() > 1 ? std::sqrt(acc / static_cast<double>(xs.size() - 1)) : 0.0;
  s.min = xs.front();
  s.max = xs.back();
  auto pct = [&](double p) {
    const double idx = p * static_cast<double>(xs.size() - 1);
    const auto lo = static_cast<std::size_t>(idx);
    const auto hi = std::min(lo + 1, xs.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return xs[lo] * (1.0 - frac) + xs[hi] * frac;
  };
  s.p50 = pct(0.50);
  s.p99 = pct(0.99);
  return s;
}

// Throughput over a long run. This IS reliable: over 10^8 messages the run
// spans many seconds, so a 41 ns tick contributes no meaningful error.
struct Throughput {
  std::uint64_t messages;
  double seconds;
  [[nodiscard]] double msgs_per_sec() const noexcept {
    return seconds > 0.0 ? static_cast<double>(messages) / seconds : 0.0;
  }
  [[nodiscard]] double ns_per_msg() const noexcept {
    return messages ? seconds * 1e9 / static_cast<double>(messages) : 0.0;
  }
};

// Keep the optimiser from deleting the work we are trying to time.
template <typename T>
inline void do_not_optimize(T const& value) noexcept {
  asm volatile("" : : "r,m"(value) : "memory");
}

} // namespace hotpath::bench
