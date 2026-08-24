#pragma once
#include "hotpath/book/events.hpp"
#include "hotpath/book/hybrid_book.hpp"
#include "hotpath/sim/queue_model.hpp"

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
};

// Queue-position-aware passive market maker: joins the touch on both sides and
// re-quotes when the touch moves, losing queue position each time it does.
//
// Extracted so the single-threaded driver and the threaded pipeline run the
// *same* strategy code. If they ran separate copies, "the pipeline produces
// identical fills" would only prove the two copies agreed, not that crossing a
// lock-free ring preserved the semantics.
class MarketMaker {
public:
  MarketMaker(Price lo, Price hi, Qty quote_size,
              std::size_t max_orders_pow2 = 1u << 21,
              std::size_t max_levels = 1u << 20)
      : book_(lo, hi, max_orders_pow2, max_levels), quote_size_(quote_size) {}

  // Applies one event and invokes cb(const Fill&) for each fill it produces.
  // Allocation-free: the book pools are pre-sized and fills go straight to the
  // callback rather than into a container.
  template <typename OnFill>
  void on_event(const BookEvent& e, OnFill&& cb) {
    // Resolve the affected order BEFORE mutating: ITCH's execute/cancel/delete
    // carry only an order reference, so after apply() the side and price needed
    // to attribute a fill are gone.
    bool have_pre = false;
    Side pre_side = Side::Buy;
    Price pre_px = 0;
    if (e.type != EventType::Add) {
      if (const auto* o = book_.find_order(e.order_ref)) {
        if (o->level >= 0) {
          have_pre = true;
          pre_side = o->side;
          pre_px = book_.level_by_index(o->level).price;
        }
      }
    }

    book::apply(book_, e);

    if (have_pre) {
      step_side(bid_, pos_bid_, Side::Buy, pre_side, pre_px, e, cb);
      step_side(ask_, pos_ask_, Side::Sell, pre_side, pre_px, e, cb);
    }

    const Price bb = book_.best_bid();
    const Price ba = book_.best_ask();
    if (bb == kInvalidPrice || ba == kInvalidPrice || bb >= ba) return;

    requote(bid_, pos_bid_, Side::Buy, bb);
    requote(ask_, pos_ask_, Side::Sell, ba);
    mid_ = (static_cast<double>(bb) + static_cast<double>(ba)) / 2.0;
    have_mid_ = true;
  }

  [[nodiscard]] const HybridBook& book() const noexcept { return book_; }
  [[nodiscard]] bool have_mid() const noexcept { return have_mid_; }
  [[nodiscard]] double mid() const noexcept { return mid_; }
  [[nodiscard]] std::uint64_t requotes() const noexcept { return requotes_; }

private:
  struct Quote {
    bool  active{false};
    Price px{0};
    Qty   remaining{0};
    Qty   ahead_at_join{0};
  };

  template <typename OnFill>
  void step_side(Quote& q, QueuePosition& pos, Side side, Side pre_side, Price pre_px,
                 const BookEvent& e, OnFill& cb) {
    if (!q.active || pre_side != side || pre_px != q.px) return;
    switch (e.type) {
      case EventType::Execute: {
        const Qty f = pos.on_execution(e.order_ref, e.shares, q.remaining);
        if (f) {
          cb(Fill{e.ts, q.px, f, side, q.ahead_at_join});
          q.remaining -= f;
          if (q.remaining == 0) { q.active = false; pos.leave(); }
        }
        break;
      }
      case EventType::Cancel:  pos.on_cancel(e.order_ref, e.shares); break;
      case EventType::Delete:  pos.on_delete(e.order_ref); break;
      case EventType::Replace: pos.on_delete(e.order_ref); break;
      default: break;
    }
  }

  void requote(Quote& q, QueuePosition& pos, Side side, Price target) {
    if (q.active && q.px == target) return;
    // Re-quoting sends us to the BACK of the new level's queue. Carrying queue
    // position across a re-quote would be cheating, and it is most of why
    // passive fills are hard.
    const auto* L = book_.level_for(side, target);
    const Qty resting = L ? L->qty : 0;
    pos.join(resting);
    if (L) {
      for (std::int32_t oi = L->head; oi >= 0; oi = book_.order_at(oi).next)
        pos.note_ahead(book_.order_at(oi).id, book_.order_at(oi).qty);
    }
    q.active = true; q.px = target; q.remaining = quote_size_; q.ahead_at_join = resting;
    ++requotes_;
  }

  HybridBook book_;
  Qty quote_size_;
  Quote bid_, ask_;
  QueuePosition pos_bid_, pos_ask_;
  double mid_{0.0};
  bool have_mid_{false};
  std::uint64_t requotes_{0};
};

} // namespace hotpath::sim
