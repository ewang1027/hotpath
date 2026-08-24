#pragma once
#include "hotpath/book/book.hpp"

#include <functional>
#include <map>
#include <unordered_map>

namespace hotpath::book {

// Design (a): the obvious one. std::map of price -> aggregate, std::unordered_map
// of order id -> record.
//
// This is the baseline nearly every candidate writes, and it is here to be
// beaten rather than to win: every level access is a red-black tree descent
// through separately allocated nodes, and every order insert is a malloc. It is
// also the easiest design to be confident is *correct*, which is what makes it
// the reference the other two are validated against.
class MapBook {
public:
  static constexpr const char* kName = "map";

  void add(OrderId ref, Side side, Price px, Qty qty) {
    if (qty == 0) return;
    orders_[ref] = Rec{px, qty, side};
    auto& agg = side == Side::Buy ? bids_[px] : asks_[px];
    agg.qty += qty;
    ++agg.orders;
  }

  void execute(OrderId ref, Qty qty) { reduce(ref, qty); }
  void cancel(OrderId ref, Qty qty)  { reduce(ref, qty); }

  void remove(OrderId ref) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return;
    detach(it->second, it->second.qty);
    orders_.erase(it);
  }

  void replace(OrderId old_ref, OrderId new_ref, Price px, Qty qty) {
    auto it = orders_.find(old_ref);
    if (it == orders_.end()) return;
    const Side side = it->second.side;
    detach(it->second, it->second.qty);
    orders_.erase(it);
    add(new_ref, side, px, qty);
  }

  [[nodiscard]] Price best_bid() const noexcept {
    return bids_.empty() ? kInvalidPrice : bids_.begin()->first;
  }
  [[nodiscard]] Price best_ask() const noexcept {
    return asks_.empty() ? kInvalidPrice : asks_.begin()->first;
  }

  void snapshot(Snapshot& s) const noexcept {
    s.nbid = 0;
    for (auto it = bids_.begin(); it != bids_.end() && s.nbid < kSnapshotDepth; ++it)
      s.bid[s.nbid++] = LevelView{it->first, it->second.qty, it->second.orders};
    s.nask = 0;
    for (auto it = asks_.begin(); it != asks_.end() && s.nask < kSnapshotDepth; ++it)
      s.ask[s.nask++] = LevelView{it->first, it->second.qty, it->second.orders};
  }

  void clear() { orders_.clear(); bids_.clear(); asks_.clear(); }
  [[nodiscard]] std::size_t order_count() const noexcept { return orders_.size(); }
  [[nodiscard]] std::size_t level_count() const noexcept { return bids_.size() + asks_.size(); }

private:
  struct Rec { Price px; Qty qty; Side side; };
  struct Agg { Qty qty{0}; std::uint32_t orders{0}; };

  void detach(const Rec& r, Qty qty) {
    if (r.side == Side::Buy) detach_from(bids_, r.px, qty);
    else                     detach_from(asks_, r.px, qty);
  }

  // bids_ and asks_ have different comparator types, so anything touching
  // "the map for this side" has to be a template rather than a ternary.
  template <typename M>
  static void reduce_level(M& m, Price px, Qty qty) {
    auto it = m.find(px);
    if (it != m.end()) it->second.qty -= qty;
  }

  template <typename M>
  static void detach_from(M& m, Price px, Qty qty) {
    auto it = m.find(px);
    if (it == m.end()) return;
    it->second.qty -= qty;
    if (--it->second.orders == 0) m.erase(it);
  }

  void reduce(OrderId ref, Qty qty) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return;
    Rec& r = it->second;
    // ITCH should never over-execute a resting order, but clamping here means a
    // malformed tape degrades to a delete instead of underflowing an unsigned.
    const Qty take = qty < r.qty ? qty : r.qty;
    if (take == r.qty) {
      detach(r, take);
      orders_.erase(it);
      return;
    }
    r.qty -= take;
    if (r.side == Side::Buy) reduce_level(bids_, r.px, take);
    else                     reduce_level(asks_, r.px, take);
  }

  std::unordered_map<OrderId, Rec> orders_;
  std::map<Price, Agg, std::greater<Price>> bids_;   // descending: begin() is best
  std::map<Price, Agg, std::less<Price>>    asks_;   // ascending:  begin() is best
};

} // namespace hotpath::book
