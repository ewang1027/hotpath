#pragma once
#include "hotpath/book/events.hpp"
#include "hotpath/book/hybrid_book.hpp"
#include "hotpath/sim/queue_model.hpp"
#include "hotpath/sim/signals.hpp"

#include <cstdint>

namespace hotpath::sim {

using book::BookEvent;
using book::EventType;
using book::HybridBook;

struct Fill {
  Ts    ts;
  Price price;
  Qty   qty;
  Side  side;
  Qty   queue_ahead_at_join;
  // True when the fill landed on a quote we had already decided to move but
  // whose replacement had not yet taken effect -- i.e. a stale quote. These are
  // the fills latency actually costs you, and they are tracked separately
  // because their markout is the whole point of the latency sweep.
  bool  stale;
  // True when we were filled at a strictly better price than the resting order
  // that actually traded: an aggressor sweeping the book would have hit our
  // more aggressive quote first. Queue position is irrelevant in that case.
  bool  swept;
};

// Queue-position-aware passive market maker: joins the touch on both sides and
// re-quotes when the touch moves, losing queue position each time it does.
//
// Extracted so the single-threaded driver and the threaded pipeline run the
// *same* strategy code. If they ran separate copies, "the pipeline produces
// identical fills" would only prove the two copies agreed, not that crossing a
// lock-free ring preserved the semantics.
// Which fill rule to apply. Both share every line of the quoting logic, so a
// comparison between them isolates the fill model and nothing else -- if the
// naive model lived in its own driver, differences in re-quoting or latency
// handling would silently contaminate the result.
enum class FillModel { QueueAware, Naive };

class MarketMaker {
public:
  // latency_ns models the round trip between deciding to move a quote and that
  // move taking effect. Until it lands, the OLD quote is still resting in the
  // market at its old price -- which is exactly how a slow participant gets
  // picked off. Zero reproduces the instantaneous-requote behaviour.
  MarketMaker(Price lo, Price hi, Qty quote_size, Ts latency_ns = 0,
              FillModel fill_model = FillModel::QueueAware,
              std::size_t max_orders_pow2 = 1u << 21,
              std::size_t max_levels = 1u << 20)
      : book_(lo, hi, max_orders_pow2, max_levels),
        quote_size_(quote_size), latency_(latency_ns), fill_model_(fill_model) {}

  // Enable or disable quoting per side. A side switched off is cancelled, which
  // like every other quote change takes `latency_` to take effect. Called by
  // the driver from a signal computed on the PREVIOUS event -- you can only act
  // on information you have already seen.
  void set_quoting(bool allow_bid, bool allow_ask) noexcept {
    allow_bid_ = allow_bid;
    allow_ask_ = allow_ask;
  }

  // Top of book as the maker currently sees it, for the driver's signal.
  [[nodiscard]] Touch touch() const noexcept {
    const Price bb = book_.best_bid(), ba = book_.best_ask();
    if (bb == kInvalidPrice || ba == kInvalidPrice || bb >= ba) return Touch{};
    const auto* lb = book_.level_for(Side::Buy, bb);
    const auto* la = book_.level_for(Side::Sell, ba);
    if (!lb || !la) return Touch{};
    return Touch{bb, ba, lb->qty, la->qty};
  }

  // Applies one event and invokes cb(const Fill&) for each fill it produces.
  // Allocation-free: the book pools are pre-sized and fills go straight to the
  // callback rather than into a container.
  template <typename OnFill>
  void on_event(const BookEvent& e, OnFill&& cb) {
    // A replacement whose latency elapsed between the previous event and this
    // one had already landed by the time this event happened, so it must take
    // effect BEFORE this event's fills are attributed -- otherwise a quote we
    // have long since moved keeps getting swept at its old price. It joins the
    // book as it stood before this event, which is where it actually arrived.
    {
      const Price pbb = book_.best_bid();
      const Price pba = book_.best_ask();
      if (pbb != kInvalidPrice && pba != kInvalidPrice && pbb < pba) {
        land_if_due(bid_, pos_bid_, Side::Buy, pbb, e.ts);
        land_if_due(ask_, pos_ask_, Side::Sell, pba, e.ts);
      }
    }

    // Resolve the affected order BEFORE mutating: ITCH's execute/cancel/delete
    // carry only an order reference, so after apply() the side and price needed
    // to attribute a fill are gone.
    bool have_pre = false;
    Side pre_side = Side::Buy;
    Price pre_px = 0;
    std::uint32_t pre_seq = 0;
    Qty pre_qty = 0;
    if (e.type != EventType::Add) {
      if (const auto* o = book_.find_order(e.order_ref)) {
        if (o->level >= 0) {
          have_pre = true;
          pre_side = o->side;
          pre_px = book_.level_by_index(o->level).price;
          pre_seq = o->seq;
          pre_qty = o->qty;
        }
      }
    }

    book::apply(book_, e);

    // Fills are resolved against the quote that was actually resting when the
    // event happened -- before any in-flight replacement lands.
    if (have_pre) {
      step_side(bid_, pos_bid_, Side::Buy, pre_side, pre_px, pre_seq, pre_qty, e, cb);
      step_side(ask_, pos_ask_, Side::Sell, pre_side, pre_px, pre_seq, pre_qty, e, cb);
    }

    const Price bb = book_.best_bid();
    const Price ba = book_.best_ask();
    if (bb == kInvalidPrice || ba == kInvalidPrice || bb >= ba) return;

    decide(bid_, Side::Buy, bb, e.ts, allow_bid_);
    decide(ask_, Side::Sell, ba, e.ts, allow_ask_);

    // With zero latency the decision lands within the same event, reproducing
    // instantaneous re-quoting exactly.
    land_if_due(bid_, pos_bid_, Side::Buy, bb, e.ts);
    land_if_due(ask_, pos_ask_, Side::Sell, ba, e.ts);

    mid_ = (static_cast<double>(bb) + static_cast<double>(ba)) / 2.0;
    have_mid_ = true;
  }

  [[nodiscard]] const HybridBook& book() const noexcept { return book_; }
  [[nodiscard]] bool have_mid() const noexcept { return have_mid_; }
  [[nodiscard]] double mid() const noexcept { return mid_; }
  [[nodiscard]] std::uint64_t requotes() const noexcept { return requotes_; }
  // Diagnostic: how often the strict-FIFO assumption behind the queue model was
  // observably violated. See QueuePosition::behind_while_queued().
  [[nodiscard]] std::uint64_t behind_while_queued() const noexcept {
    return pos_bid_.behind_while_queued() + pos_ask_.behind_while_queued();
  }

private:
  struct Quote {
    bool  active{false};
    Price px{0};
    Qty   remaining{0};
    Qty   ahead_at_join{0};
    bool  in_flight{false};   // a replacement has been decided but not landed
    Ts    lands_at{0};
    bool  want_active{true};  // what the in-flight change is trying to achieve
  };

  // Is our quote strictly more aggressive than the order that just traded? A
  // bid above the traded price (or an ask below it) would have been taken first
  // by the same aggressor, so queue position does not apply -- we are in front
  // of the entire level that did trade. This is the path a stale quote gets
  // run over on, and it is unreachable when the quote is always at the touch.
  [[nodiscard]] static bool strictly_better(Side side, Price ours, Price traded) noexcept {
    return side == Side::Buy ? ours > traded : ours < traded;
  }

  template <typename OnFill>
  void step_side(Quote& q, QueuePosition& pos, Side side, Side pre_side, Price pre_px,
                 std::uint32_t pre_seq, Qty pre_qty, const BookEvent& e, OnFill& cb) {
    if (!q.active || pre_side != side) return;

    if (e.type == EventType::Execute && strictly_better(side, q.px, pre_px)) {
      const Qty f = e.shares < q.remaining ? e.shares : q.remaining;
      if (f) {
        cb(Fill{e.ts, q.px, f, side, q.ahead_at_join, q.in_flight, true});
        q.remaining -= f;
        if (q.remaining == 0) { q.active = false; pos.leave(); }
      }
      return;
    }

    if (pre_px != q.px) return;
    switch (e.type) {
      case EventType::Execute: {
        // The ONLY difference between the two models.
        const Qty f = fill_model_ == FillModel::Naive
                          ? (e.shares < q.remaining ? e.shares : q.remaining)
                          : pos.on_execution(pre_seq, e.shares, q.remaining);
        if (f) {
          cb(Fill{e.ts, q.px, f, side, q.ahead_at_join, q.in_flight, false});
          q.remaining -= f;
          if (q.remaining == 0) { q.active = false; pos.leave(); }
        }
        break;
      }
      case EventType::Cancel:  pos.on_cancel(pre_seq, e.shares); break;
      // A replace deletes the original outright, so both remove the order's
      // full remaining size from the queue ahead of us.
      case EventType::Delete:
      case EventType::Replace: pos.on_delete(pre_seq, pre_qty); break;
      default: break;
    }
  }

  // Decide to move: the change is now in flight and lands `latency_` later.
  // Crucially the old quote keeps resting at its old price in the meantime --
  // including when the change is a cancellation, which is what makes pulling a
  // quote on a signal cost something rather than being free.
  void decide(Quote& q, Side, Price target, Ts now, bool allow) {
    if (q.in_flight) return;
    if (q.active == allow && (!q.active || q.px == target)) return;
    q.in_flight = true;
    q.want_active = allow;
    q.lands_at = now + latency_;
  }

  void land_if_due(Quote& q, QueuePosition& pos, Side side, Price target, Ts now) {
    if (!q.in_flight || now < q.lands_at) return;
    q.in_flight = false;
    if (!q.want_active) { q.active = false; pos.leave(); return; }
    join(q, pos, side, target);
  }

  void join(Quote& q, QueuePosition& pos, Side side, Price target) {
    // Joining sends us to the BACK of the level's queue. Carrying queue
    // position across a re-quote would be cheating, and it is most of why
    // passive fills are hard. Under latency it is worse still: everything that
    // arrived while we were in flight is now ahead of us.
    const auto* L = book_.level_for(side, target);
    const Qty resting = L ? L->qty : 0;
    // O(1): capture the book's next insertion stamp instead of enumerating the
    // orders ahead. Everything already resting here is below it by FIFO.
    pos.join(resting, book_.next_seq());
    q.active = true; q.px = target; q.remaining = quote_size_; q.ahead_at_join = resting;
    ++requotes_;
  }

  HybridBook book_;
  Qty quote_size_;
  Ts  latency_{0};
  FillModel fill_model_{FillModel::QueueAware};
  bool allow_bid_{true};
  bool allow_ask_{true};
  Quote bid_, ask_;
  QueuePosition pos_bid_, pos_ask_;
  double mid_{0.0};
  bool have_mid_{false};
  std::uint64_t requotes_{0};
};

} // namespace hotpath::sim
