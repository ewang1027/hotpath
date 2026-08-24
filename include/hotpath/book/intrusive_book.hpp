#pragma once
#include "hotpath/book/book.hpp"
#include "hotpath/core/open_hash.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace hotpath::book {

// Design (b): the textbook low-latency book.
//
//   * orders live in one pre-sized pool with a free list -- never malloc'd
//   * each price level owns an intrusive doubly-linked FIFO of its orders
//   * levels live in their own stable pool; the per-side price ordering is a
//     sorted vector of *indices* into that pool
//
// Why indices into a stable pool rather than a sorted vector of levels: a level
// vector has to shift on every insert and erase, which would move levels in
// memory and invalidate every order's back-pointer. Sorting indices keeps the
// levels themselves pinned, so an order's level reference stays valid for its
// whole life.
//
// The intrusive FIFO is not incidental. ITCH executes against a price level in
// strict time priority, so the list order *is* queue position -- which is what
// the fill model in sim/ needs, and what an aggregate-only book throws away.
class IntrusiveBook {
public:
  static constexpr const char* kName = "intrusive";

  explicit IntrusiveBook(std::size_t max_orders_pow2 = 1u << 20,
                         std::size_t max_levels = 1u << 16)
      : ids_(max_orders_pow2) {
    orders_.resize(max_orders_pow2);
    levels_.resize(max_levels);
    free_orders_.reserve(max_orders_pow2);
    free_levels_.reserve(max_levels);
    // Hand out low indices first so the hot working set stays compact.
    for (std::size_t i = max_orders_pow2; i-- > 0;)
      free_orders_.push_back(static_cast<std::int32_t>(i));
    for (std::size_t i = max_levels; i-- > 0;)
      free_levels_.push_back(static_cast<std::int32_t>(i));
    bid_idx_.reserve(max_levels);
    ask_idx_.reserve(max_levels);
  }

  void add(OrderId ref, Side side, Price px, Qty qty) {
    if (qty == 0) return;
    // Capacity exhaustion must never be silent. Dropping an order here would
    // leave the book permanently and invisibly wrong -- it was caught by the
    // cross-validation gate only because a second design disagreed, which is
    // not a diagnostic you have in production.
    if (free_orders_.empty()) { ++rejected_; return; }
    const std::int32_t li = find_or_create_level(side, px);
    if (li < 0) { ++rejected_; return; }

    const std::int32_t oi = free_orders_.back();
    free_orders_.pop_back();
    Order& o = orders_[static_cast<std::size_t>(oi)];
    o.id = ref; o.qty = qty; o.level = li; o.side = side;

    Level& L = levels_[static_cast<std::size_t>(li)];
    o.prev = L.tail;
    o.next = -1;
    if (L.tail >= 0) orders_[static_cast<std::size_t>(L.tail)].next = oi;
    else             L.head = oi;
    L.tail = oi;
    L.qty += qty;
    ++L.orders;

    if (!ids_.insert(ref, oi)) ++rejected_;
  }

  void execute(OrderId ref, Qty qty) { reduce(ref, qty); }
  void cancel(OrderId ref, Qty qty)  { reduce(ref, qty); }

  void remove(OrderId ref) {
    const std::int32_t* slot = ids_.find(ref);
    if (!slot) return;
    unlink(*slot);
    free_orders_.push_back(*slot);
    ids_.erase(ref);
  }

  void replace(OrderId old_ref, OrderId new_ref, Price px, Qty qty) {
    const std::int32_t* slot = ids_.find(old_ref);
    if (!slot) return;
    const Side side = orders_[static_cast<std::size_t>(*slot)].side;
    unlink(*slot);
    free_orders_.push_back(*slot);
    ids_.erase(old_ref);
    add(new_ref, side, px, qty);
  }

  [[nodiscard]] Price best_bid() const noexcept {
    return bid_idx_.empty() ? kInvalidPrice : levels_[static_cast<std::size_t>(bid_idx_.front())].price;
  }
  [[nodiscard]] Price best_ask() const noexcept {
    return ask_idx_.empty() ? kInvalidPrice : levels_[static_cast<std::size_t>(ask_idx_.front())].price;
  }

  void snapshot(Snapshot& s) const noexcept {
    s.nbid = 0;
    for (std::size_t i = 0; i < bid_idx_.size() && s.nbid < kSnapshotDepth; ++i) {
      const Level& L = levels_[static_cast<std::size_t>(bid_idx_[i])];
      s.bid[s.nbid++] = LevelView{L.price, L.qty, L.orders};
    }
    s.nask = 0;
    for (std::size_t i = 0; i < ask_idx_.size() && s.nask < kSnapshotDepth; ++i) {
      const Level& L = levels_[static_cast<std::size_t>(ask_idx_[i])];
      s.ask[s.nask++] = LevelView{L.price, L.qty, L.orders};
    }
  }

  void clear() {
    ids_.clear();
    bid_idx_.clear();
    ask_idx_.clear();
    free_orders_.clear();
    free_levels_.clear();
    for (std::size_t i = orders_.size(); i-- > 0;) free_orders_.push_back(static_cast<std::int32_t>(i));
    for (std::size_t i = levels_.size(); i-- > 0;) free_levels_.push_back(static_cast<std::int32_t>(i));
  }

  [[nodiscard]] std::size_t order_count() const noexcept { return ids_.size(); }
  [[nodiscard]] std::size_t level_count() const noexcept { return bid_idx_.size() + ask_idx_.size(); }
  // Non-zero means the book is missing orders it was told about: the pools or
  // the id table were too small. Always assert this is zero.
  [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }
  [[nodiscard]] std::size_t levels_in_use() const noexcept {
    return levels_.size() - free_levels_.size();
  }

  // --- exposed for the queue-position model in sim/ ---
  struct Order {
    OrderId      id{0};
    Qty          qty{0};
    std::int32_t prev{-1};
    std::int32_t next{-1};
    std::int32_t level{-1};
    Side         side{Side::Buy};
  };
  struct Level {
    Price         price{0};
    Qty           qty{0};
    std::uint32_t orders{0};
    std::int32_t  head{-1};
    std::int32_t  tail{-1};
  };

  [[nodiscard]] const Order* find_order(OrderId ref) const noexcept {
    const std::int32_t* slot = ids_.find(ref);
    return slot ? &orders_[static_cast<std::size_t>(*slot)] : nullptr;
  }
  [[nodiscard]] const Level& level_at(std::int32_t idx) const noexcept {
    return levels_[static_cast<std::size_t>(idx)];
  }
  [[nodiscard]] const Order& order_at(std::int32_t idx) const noexcept {
    return orders_[static_cast<std::size_t>(idx)];
  }

private:
  // Bids sort descending, asks ascending, so index 0 is always the best.
  [[nodiscard]] std::vector<std::int32_t>& side_idx(Side s) noexcept {
    return s == Side::Buy ? bid_idx_ : ask_idx_;
  }

  // Returns the position where `px` belongs in the side's sorted index vector.
  [[nodiscard]] std::size_t lower_bound_pos(Side s, Price px) const noexcept {
    const std::vector<std::int32_t>& v = s == Side::Buy ? bid_idx_ : ask_idx_;
    std::size_t lo = 0, hi = v.size();
    while (lo < hi) {
      const std::size_t mid = (lo + hi) / 2;
      const Price mp = levels_[static_cast<std::size_t>(v[mid])].price;
      const bool before = s == Side::Buy ? (mp > px) : (mp < px);
      if (before) lo = mid + 1; else hi = mid;
    }
    return lo;
  }

  std::int32_t find_or_create_level(Side s, Price px) {
    std::vector<std::int32_t>& v = side_idx(s);
    const std::size_t pos = lower_bound_pos(s, px);
    if (pos < v.size() && levels_[static_cast<std::size_t>(v[pos])].price == px) return v[pos];
    if (free_levels_.empty()) return -1;
    const std::int32_t li = free_levels_.back();
    free_levels_.pop_back();
    Level& L = levels_[static_cast<std::size_t>(li)];
    L.price = px; L.qty = 0; L.orders = 0; L.head = -1; L.tail = -1;
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(pos), li);
    return li;
  }

  void drop_level(Side s, std::int32_t li) {
    std::vector<std::int32_t>& v = side_idx(s);
    const Price px = levels_[static_cast<std::size_t>(li)].price;
    const std::size_t pos = lower_bound_pos(s, px);
    if (pos < v.size() && v[pos] == li) {
      v.erase(v.begin() + static_cast<std::ptrdiff_t>(pos));
      free_levels_.push_back(li);
    }
  }

  void unlink(std::int32_t oi) {
    Order& o = orders_[static_cast<std::size_t>(oi)];
    Level& L = levels_[static_cast<std::size_t>(o.level)];
    if (o.prev >= 0) orders_[static_cast<std::size_t>(o.prev)].next = o.next;
    else             L.head = o.next;
    if (o.next >= 0) orders_[static_cast<std::size_t>(o.next)].prev = o.prev;
    else             L.tail = o.prev;
    L.qty -= o.qty;
    if (--L.orders == 0) drop_level(o.side, o.level);
    o.prev = o.next = -1;
    o.level = -1;
  }

  void reduce(OrderId ref, Qty qty) {
    const std::int32_t* slot = ids_.find(ref);
    if (!slot) return;
    Order& o = orders_[static_cast<std::size_t>(*slot)];
    const Qty take = qty < o.qty ? qty : o.qty;
    if (take == o.qty) {
      const std::int32_t oi = *slot;
      unlink(oi);
      free_orders_.push_back(oi);
      ids_.erase(ref);
      return;
    }
    o.qty -= take;
    levels_[static_cast<std::size_t>(o.level)].qty -= take;
  }

  std::vector<Order> orders_;
  std::vector<Level> levels_;
  std::vector<std::int32_t> free_orders_;
  std::vector<std::int32_t> free_levels_;
  std::vector<std::int32_t> bid_idx_;
  std::vector<std::int32_t> ask_idx_;
  OpenHashMap<std::int32_t> ids_;
  std::uint64_t rejected_{0};
};

} // namespace hotpath::book
