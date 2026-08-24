#pragma once
#include "hotpath/core/open_hash.hpp"
#include "hotpath/core/types.hpp"

#include <cstdint>

namespace hotpath::sim {

// Queue position for one hypothetical resting order.
//
// This is the part almost every candidate backtest gets wrong. The naive model
// says: if the price traded, I filled. That is wrong for the overwhelming
// majority of passive flow, because you are behind a queue and the exchange
// fills that queue in strict time priority.
//
// Two things reduce the volume ahead of you:
//   1. executions ahead of you  -- the obvious one
//   2. CANCELS ahead of you     -- the one naive models omit
//
// (2) matters enormously here: on this trading day adds and deletes are 85% of
// all messages while executions are 1.7%. Most of the queue in front of you
// disappears by being cancelled, not by trading. A model that only advances the
// queue on executions will conclude you almost never reach the front; a model
// that fills whenever the price trades will conclude you always do. Both are
// wrong in opposite directions.
//
// Membership ("was that order ahead of me?") is tracked explicitly rather than
// inferred from the order reference number. ITCH order refs are NOT
// monotonically increasing -- measured 21,094 non-monotonic adds on AAPL in one
// day (3.0% of adds) -- so `ref < my_join_ref` is not a valid proxy.
class QueuePosition {
public:
  explicit QueuePosition(std::size_t ahead_capacity_pow2 = 1u << 12)
      : ahead_orders_(ahead_capacity_pow2) {}

  // Join the back of the queue at a level currently holding `resting` shares
  // across the given order refs.
  void join(Qty resting) noexcept {
    ahead_ = resting;
    ahead_orders_.clear();
    joined_ = true;
  }

  // Record one order that is ahead of us (called for each order resting at the
  // level at join time).
  void note_ahead(OrderId ref, Qty qty) noexcept {
    if (qty) ahead_orders_.insert(ref, qty);
  }

  void leave() noexcept { joined_ = false; ahead_ = 0; ahead_orders_.clear(); }
  [[nodiscard]] bool joined() const noexcept { return joined_; }
  [[nodiscard]] Qty ahead() const noexcept { return ahead_; }

  // An execution happened at our price level. Returns how many shares of OURS
  // were filled: the exchange consumes the queue from the front, so we are only
  // reached once cumulative executed volume exceeds the volume ahead of us.
  [[nodiscard]] Qty on_execution(OrderId ref, Qty executed, Qty our_remaining) noexcept {
    if (Qty* q = ahead_orders_.find(ref)) {
      const Qty d = executed < *q ? executed : *q;
      *q -= d;
      if (*q == 0) ahead_orders_.erase(ref);
    }
    if (executed <= ahead_) { ahead_ -= executed; return 0; }
    const Qty through = executed - ahead_;
    ahead_ = 0;
    return through < our_remaining ? through : our_remaining;
  }

  // An order ahead of us was cancelled (partially) -- the queue shortens with
  // no trade. This is the path a naive fill model ignores entirely.
  void on_cancel(OrderId ref, Qty cancelled) noexcept {
    Qty* q = ahead_orders_.find(ref);
    if (!q) return;                       // behind us: no effect on our position
    const Qty d = cancelled < *q ? cancelled : *q;
    *q -= d;
    ahead_ -= d < ahead_ ? d : ahead_;
    if (*q == 0) ahead_orders_.erase(ref);
  }

  // An order ahead of us was deleted outright.
  void on_delete(OrderId ref) noexcept {
    Qty* q = ahead_orders_.find(ref);
    if (!q) return;
    ahead_ -= *q < ahead_ ? *q : ahead_;
    ahead_orders_.erase(ref);
  }

private:
  OpenHashMap<Qty> ahead_orders_;
  Qty  ahead_{0};
  bool joined_{false};
};

} // namespace hotpath::sim
