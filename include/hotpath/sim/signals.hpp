#pragma once
#include "hotpath/core/types.hpp"

#include <cmath>
#include <cstdint>

namespace hotpath::sim {

// Top of book at one instant. Everything below is derived from this and the
// previous one -- no history, no allocation.
struct Touch {
  Price bid_px{0}, ask_px{0};
  Qty   bid_qty{0}, ask_qty{0};

  [[nodiscard]] bool valid() const noexcept {
    return bid_px != 0 && ask_px != 0 && bid_px < ask_px &&
           (bid_qty + ask_qty) > 0;
  }
  [[nodiscard]] double mid() const noexcept {
    return 0.5 * (static_cast<double>(bid_px) + static_cast<double>(ask_px));
  }
  [[nodiscard]] double spread() const noexcept {
    return static_cast<double>(ask_px) - static_cast<double>(bid_px);
  }

  // Queue imbalance in [-1, +1]. Positive = more resting size on the bid.
  [[nodiscard]] double imbalance() const noexcept {
    const double b = bid_qty, a = ask_qty;
    const double t = b + a;
    return t > 0 ? (b - a) / t : 0.0;
  }

  // Stoikov's microprice: the mid weighted by the OPPOSITE side's size, so a
  // heavy bid pulls fair value toward the ask. It is a better estimate of where
  // the next trade prints than the mid, because the mid ignores the fact that a
  // 10x-heavier bid is far more likely to hold than to break.
  [[nodiscard]] double microprice() const noexcept {
    const double b = bid_qty, a = ask_qty;
    const double t = b + a;
    if (t <= 0) return mid();
    return (b * static_cast<double>(ask_px) + a * static_cast<double>(bid_px)) / t;
  }

  // Microprice minus mid, in units of half-spread: a scale-free version of the
  // same signal that is comparable across symbols and across spread regimes.
  [[nodiscard]] double micro_tilt() const noexcept {
    const double hs = 0.5 * spread();
    return hs > 0 ? (microprice() - mid()) / hs : 0.0;
  }
};

// Order Flow Imbalance (Cont, Kukanov & Stoikov 2014).
//
// Counts net signed size arriving at the touch. A bid that improves adds its
// size; a bid that is pulled subtracts the size it had. The ask contributes
// with the opposite sign. Unlike raw trade counts it captures pressure from
// quoting, which on this data is where almost all the action is -- adds and
// deletes are 85% of messages while executions are 1.7%.
//
// Accumulated as an exponentially-weighted sum with a time constant rather than
// a fixed window, so it is O(1) with no history buffer.
class OrderFlowImbalance {
public:
  explicit OrderFlowImbalance(double tau_ns = 1e9) : tau_ns_(tau_ns) {}

  void update(const Touch& t, Ts ts) noexcept {
    if (!have_prev_) {
      prev_ = t; last_ts_ = ts; have_prev_ = true;
      depth_ = 0.5 * (static_cast<double>(t.bid_qty) + static_cast<double>(t.ask_qty));
      return;
    }
    const double e = increment(prev_, t);

    // Decay whatever has accumulated, then add this event's contribution.
    const double dt = ts >= last_ts_ ? static_cast<double>(ts - last_ts_) : 0.0;
    const double decay = std::exp(-dt / tau_ns_);
    ofi_ = ofi_ * decay + e;

    // Depth is tracked the same way so the normalised signal is comparable
    // across symbols: 1000 shares of pressure means something different in SIRI
    // than in AMZN.
    const double d = 0.5 * (static_cast<double>(t.bid_qty) + static_cast<double>(t.ask_qty));
    depth_ = depth_ * decay + d * (1.0 - decay);

    prev_ = t;
    last_ts_ = ts;
  }

  [[nodiscard]] double raw() const noexcept { return ofi_; }
  [[nodiscard]] double normalized() const noexcept {
    return depth_ > 1.0 ? ofi_ / depth_ : 0.0;
  }
  [[nodiscard]] bool ready() const noexcept { return have_prev_; }

  // One event's contribution. Signs follow Cont et al: positive is buy pressure.
  static double increment(const Touch& p, const Touch& c) noexcept {
    double e = 0.0;
    if (c.bid_px >= p.bid_px) e += static_cast<double>(c.bid_qty);
    if (c.bid_px <= p.bid_px) e -= static_cast<double>(p.bid_qty);
    if (c.ask_px <= p.ask_px) e -= static_cast<double>(c.ask_qty);
    if (c.ask_px >= p.ask_px) e += static_cast<double>(p.ask_qty);
    return e;
  }

private:
  double tau_ns_;
  Touch  prev_{};
  bool   have_prev_{false};
  double ofi_{0.0};
  double depth_{0.0};
  Ts     last_ts_{0};
};

} // namespace hotpath::sim
