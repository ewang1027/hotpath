#pragma once
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
// Membership ("was that order ahead of me?") is decided by comparing the
// order's monotonic insertion stamp against the stamp at the moment we joined.
// A price level's order list is strict FIFO, so everything resting in it when
// we arrived has a strictly smaller stamp, and everything that arrives later
// has a larger one. That makes the test O(1) with no state.
//
// Two earlier approaches were rejected:
//   * `order_ref < my_join_ref` -- ITCH order reference numbers are NOT
//     monotonically increasing (21,094 non-monotonic adds on AAPL in one day,
//     3.0% of adds), so this silently corrupts queue position.
//   * an explicit hash set of the orders ahead -- exact, but it had to be
//     rebuilt by walking the level's FIFO on every re-quote, which measured as
//     3-6x the cost of maintaining the book itself.
class QueuePosition {
public:
  // Join the back of a level currently holding `resting` shares. `join_seq` is
  // the book's next insertion stamp: everything already at the level is below it.
  void join(Qty resting, std::uint32_t join_seq) noexcept {
    ahead_ = resting;
    join_seq_ = join_seq;
    joined_ = true;
  }

  void leave() noexcept { joined_ = false; ahead_ = 0; }
  [[nodiscard]] bool joined() const noexcept { return joined_; }
  [[nodiscard]] Qty ahead() const noexcept { return ahead_; }

  [[nodiscard]] bool is_ahead(std::uint32_t seq) const noexcept { return seq < join_seq_; }

  // An execution happened at our price level. Returns how many shares of OURS
  // were filled: the exchange consumes the queue from the front, so we are only
  // reached once cumulative executed volume exceeds the volume ahead of us.
  [[nodiscard]] Qty on_execution(Qty executed, Qty our_remaining) noexcept {
    if (executed <= ahead_) { ahead_ -= executed; return 0; }
    const Qty through = executed - ahead_;
    ahead_ = 0;
    return through < our_remaining ? through : our_remaining;
  }

  // An order ahead of us was cancelled (partially) -- the queue shortens with
  // no trade. This is the path a naive fill model ignores entirely.
  void on_cancel(std::uint32_t seq, Qty cancelled) noexcept {
    if (!is_ahead(seq)) return;                 // behind us: no effect
    ahead_ -= cancelled < ahead_ ? cancelled : ahead_;
  }

  // An order ahead of us was deleted outright; `remaining` is what it still had.
  void on_delete(std::uint32_t seq, Qty remaining) noexcept {
    if (!is_ahead(seq)) return;
    ahead_ -= remaining < ahead_ ? remaining : ahead_;
  }

private:
  Qty           ahead_{0};
  std::uint32_t join_seq_{0};
  bool          joined_{false};
};

} // namespace hotpath::sim
