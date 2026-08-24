#pragma once
#include "hotpath/book/book.hpp"
#include "hotpath/core/open_hash.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace hotpath::book {

// Design (d): what the measurements actually imply.
//
// The three-way study said two things. The direct-addressed grid wins on book
// maintenance by ~3.5x because price lookup is arithmetic and nothing ever
// shifts. The textbook intrusive book loses even to std::map, because a real
// book runs thousands of levels deep (AAPL: mean 4652, max 5320) and a sorted
// level vector shifts ~1946 elements per level create/destroy -- 5.7 GB of
// memmove per replay.
//
// But the grid throws away per-level order identity, and the intrusive lists
// are exactly what a queue-position model needs: ITCH fills a level in strict
// time priority, so the FIFO order *is* queue position.
//
// So: grid indexing for O(1) price lookup and bitmap best-price, intrusive
// per-level FIFO lists for queue position, one pooled allocation up front.
// Levels live in a shared pool addressed by the grid, so out-of-window and
// sub-penny prices reuse the same level machinery through a std::map.
class HybridBook {
public:
  static constexpr const char* kName = "hybrid";
  static constexpr Price kTick = 100;

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

  HybridBook(Price lo, Price hi,
             std::size_t max_orders_pow2 = 1u << 21,
             std::size_t max_levels = 1u << 20)
      : lo_(lo - (lo % kTick)),
        ticks_(static_cast<std::size_t>((hi - (lo - (lo % kTick))) / kTick) + 1),
        ids_(max_orders_pow2) {
    orders_.resize(max_orders_pow2);
    levels_.resize(max_levels);
    free_orders_.reserve(max_orders_pow2);
    free_levels_.reserve(max_levels);
    for (std::size_t i = max_orders_pow2; i-- > 0;) free_orders_.push_back(static_cast<std::int32_t>(i));
    for (std::size_t i = max_levels; i-- > 0;) free_levels_.push_back(static_cast<std::int32_t>(i));
    for (int s = 0; s < 2; ++s) {
      side_[s].slot.assign(ticks_, -1);
      side_[s].bits.assign((ticks_ + 63) / 64, 0);
    }
  }

  void add(OrderId ref, Side side, Price px, Qty qty) {
    if (qty == 0) return;
    if (free_orders_.empty()) { ++rejected_; return; }
    const std::int32_t li = find_or_create_level(side, px);
    if (li < 0) { ++rejected_; return; }

    const std::int32_t oi = free_orders_.back();
    free_orders_.pop_back();
    Order& o = orders_[static_cast<std::size_t>(oi)];
    o.id = ref; o.qty = qty; o.level = li; o.side = side;

    Level& L = levels_[static_cast<std::size_t>(li)];
    o.prev = L.tail; o.next = -1;
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
    const std::int32_t oi = *slot;
    unlink(oi);
    free_orders_.push_back(oi);
    ids_.erase(ref);
  }

  void replace(OrderId old_ref, OrderId new_ref, Price px, Qty qty) {
    const std::int32_t* slot = ids_.find(old_ref);
    if (!slot) return;
    const std::int32_t oi = *slot;
    const Side side = orders_[static_cast<std::size_t>(oi)].side;
    unlink(oi);
    free_orders_.push_back(oi);
    ids_.erase(old_ref);
    add(new_ref, side, px, qty);
  }

  [[nodiscard]] Price best_bid() const noexcept { return best(Side::Buy); }
  [[nodiscard]] Price best_ask() const noexcept { return best(Side::Sell); }

  void snapshot(Snapshot& s) const noexcept {
    s.nbid = collect(Side::Buy,  s.bid, over_[0].rbegin(), over_[0].rend());
    s.nask = collect(Side::Sell, s.ask, over_[1].begin(),  over_[1].end());
  }

  void clear() {
    ids_.clear();
    free_orders_.clear(); free_levels_.clear();
    for (std::size_t i = orders_.size(); i-- > 0;) free_orders_.push_back(static_cast<std::int32_t>(i));
    for (std::size_t i = levels_.size(); i-- > 0;) free_levels_.push_back(static_cast<std::int32_t>(i));
    for (int s = 0; s < 2; ++s) {
      std::fill(side_[s].slot.begin(), side_[s].slot.end(), -1);
      std::fill(side_[s].bits.begin(), side_[s].bits.end(), 0ull);
      side_[s].hint = -1;
      over_[s].clear();
    }
  }

  [[nodiscard]] std::size_t order_count() const noexcept { return ids_.size(); }
  [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }
  // Levels created outside the dense window (sub-penny, or beyond the price
  // band). These go into a std::map and are the ONLY source of allocation in
  // this design -- the dense path allocates nothing after construction.
  [[nodiscard]] std::uint64_t overflow_levels() const noexcept { return overflow_ops_; }

  // --- queue position support (used by sim/) ---
  [[nodiscard]] const Order* find_order(OrderId ref) const noexcept {
    const std::int32_t* slot = ids_.find(ref);
    return slot ? &orders_[static_cast<std::size_t>(*slot)] : nullptr;
  }
  [[nodiscard]] const Level* level_for(Side side, Price px) const noexcept {
    const std::int32_t li = lookup_level(side, px);
    return li < 0 ? nullptr : &levels_[static_cast<std::size_t>(li)];
  }
  [[nodiscard]] const Order& order_at(std::int32_t i) const noexcept {
    return orders_[static_cast<std::size_t>(i)];
  }
  [[nodiscard]] const Level& level_by_index(std::int32_t i) const noexcept {
    return levels_[static_cast<std::size_t>(i)];
  }
  // Shares resting ahead of `ref` at its own price level, i.e. its queue
  // position. Walks the FIFO from the head, so it is O(orders ahead) -- callers
  // that need it per-event should track it incrementally instead.
  [[nodiscard]] Qty shares_ahead_of(OrderId ref) const noexcept {
    const std::int32_t* slot = ids_.find(ref);
    if (!slot) return 0;
    const Order& target = orders_[static_cast<std::size_t>(*slot)];
    if (target.level < 0) return 0;
    Qty ahead = 0;
    std::int32_t i = levels_[static_cast<std::size_t>(target.level)].head;
    while (i >= 0 && i != *slot) {
      ahead += orders_[static_cast<std::size_t>(i)].qty;
      i = orders_[static_cast<std::size_t>(i)].next;
    }
    return ahead;
  }

private:
  struct SideGrid {
    std::vector<std::int32_t>  slot;   // tick -> level index, -1 = empty
    std::vector<std::uint64_t> bits;
    std::int64_t               hint{-1};
  };

  [[nodiscard]] bool in_grid(Price px) const noexcept {
    return px >= lo_ && px % kTick == 0 &&
           static_cast<std::size_t>((px - lo_) / kTick) < ticks_;
  }
  [[nodiscard]] std::size_t tick_of(Price px) const noexcept {
    return static_cast<std::size_t>((px - lo_) / kTick);
  }
  [[nodiscard]] Price price_of(std::size_t t) const noexcept {
    return lo_ + static_cast<Price>(t) * kTick;
  }

  [[nodiscard]] std::int32_t lookup_level(Side side, Price px) const noexcept {
    const int s = static_cast<int>(side);
    if (in_grid(px)) return side_[s].slot[tick_of(px)];
    auto it = over_[s].find(px);
    return it == over_[s].end() ? -1 : it->second;
  }

  std::int32_t find_or_create_level(Side side, Price px) {
    const int s = static_cast<int>(side);
    const std::int32_t existing = lookup_level(side, px);
    if (existing >= 0) return existing;
    if (free_levels_.empty()) return -1;
    const std::int32_t li = free_levels_.back();
    free_levels_.pop_back();
    Level& L = levels_[static_cast<std::size_t>(li)];
    L.price = px; L.qty = 0; L.orders = 0; L.head = -1; L.tail = -1;

    if (in_grid(px)) {
      const std::size_t t = tick_of(px);
      side_[s].slot[t] = li;
      side_[s].bits[t >> 6] |= (1ull << (t & 63));
      auto& h = side_[s].hint;
      if (h < 0) h = static_cast<std::int64_t>(t);
      else if (side == Side::Buy)  { if (static_cast<std::int64_t>(t) > h) h = static_cast<std::int64_t>(t); }
      else                         { if (static_cast<std::int64_t>(t) < h) h = static_cast<std::int64_t>(t); }
    } else {
      ++overflow_ops_;
      over_[s][px] = li;
    }
    return li;
  }

  void drop_level(Side side, std::int32_t li) {
    const int s = static_cast<int>(side);
    const Price px = levels_[static_cast<std::size_t>(li)].price;
    if (in_grid(px)) {
      const std::size_t t = tick_of(px);
      side_[s].slot[t] = -1;
      side_[s].bits[t >> 6] &= ~(1ull << (t & 63));
      if (side_[s].hint == static_cast<std::int64_t>(t)) side_[s].hint = -1;
    } else {
      over_[s].erase(px);
    }
    free_levels_.push_back(li);
  }

  void unlink(std::int32_t oi) {
    Order& o = orders_[static_cast<std::size_t>(oi)];
    Level& L = levels_[static_cast<std::size_t>(o.level)];
    if (o.prev >= 0) orders_[static_cast<std::size_t>(o.prev)].next = o.next;
    else             L.head = o.next;
    if (o.next >= 0) orders_[static_cast<std::size_t>(o.next)].prev = o.prev;
    else             L.tail = o.prev;
    L.qty -= o.qty;
    const Side side = o.side;
    const std::int32_t li = o.level;
    o.prev = o.next = -1;
    o.level = -1;
    if (--L.orders == 0) drop_level(side, li);
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

  [[nodiscard]] std::int64_t scan_down(const SideGrid& g, std::int64_t from) const noexcept {
    if (from < 0) return -1;
    std::size_t w = static_cast<std::size_t>(from) >> 6;
    std::uint64_t word = g.bits[w] & (~0ull >> (63 - (static_cast<std::size_t>(from) & 63)));
    while (true) {
      if (word) return static_cast<std::int64_t>((w << 6) + (63 - static_cast<std::size_t>(__builtin_clzll(word))));
      if (w == 0) return -1;
      word = g.bits[--w];
    }
  }
  [[nodiscard]] std::int64_t scan_up(const SideGrid& g, std::int64_t from) const noexcept {
    if (from < 0) from = 0;
    if (static_cast<std::size_t>(from) >= ticks_) return -1;
    std::size_t w = static_cast<std::size_t>(from) >> 6;
    std::uint64_t word = g.bits[w] & (~0ull << (static_cast<std::size_t>(from) & 63));
    while (true) {
      if (word) return static_cast<std::int64_t>((w << 6) + static_cast<std::size_t>(__builtin_ctzll(word)));
      if (++w >= g.bits.size()) return -1;
      word = g.bits[w];
    }
  }

  [[nodiscard]] Price grid_best(Side side) const noexcept {
    SideGrid& g = const_cast<SideGrid&>(side_[static_cast<int>(side)]);
    const bool buy = side == Side::Buy;
    if (g.hint < 0) {
      g.hint = buy ? scan_down(g, static_cast<std::int64_t>(ticks_) - 1) : scan_up(g, 0);
    } else if (g.slot[static_cast<std::size_t>(g.hint)] < 0) {
      g.hint = buy ? scan_down(g, g.hint) : scan_up(g, g.hint);
    }
    return g.hint < 0 ? kInvalidPrice : price_of(static_cast<std::size_t>(g.hint));
  }

  [[nodiscard]] Price best(Side side) const noexcept {
    const int s = static_cast<int>(side);
    const Price g = grid_best(side);
    const bool buy = side == Side::Buy;
    Price o = kInvalidPrice;
    if (!over_[s].empty()) o = buy ? over_[s].rbegin()->first : over_[s].begin()->first;
    if (g == kInvalidPrice) return o;
    if (o == kInvalidPrice) return g;
    return buy ? (g > o ? g : o) : (g < o ? g : o);
  }

  template <typename It>
  [[nodiscard]] int collect(Side side, LevelView* out, It oit, It oend) const noexcept {
    const int s = static_cast<int>(side);
    SideGrid& g = const_cast<SideGrid&>(side_[s]);
    const bool buy = side == Side::Buy;
    std::int64_t t = buy ? scan_down(g, static_cast<std::int64_t>(ticks_) - 1) : scan_up(g, 0);

    int n = 0;
    while (n < kSnapshotDepth && (t >= 0 || oit != oend)) {
      const bool have_g = t >= 0, have_o = oit != oend;
      bool take_grid;
      if (have_g && have_o) {
        const Price gp = price_of(static_cast<std::size_t>(t));
        take_grid = buy ? (gp > oit->first) : (gp < oit->first);
      } else take_grid = have_g;

      if (take_grid) {
        const Level& L = levels_[static_cast<std::size_t>(g.slot[static_cast<std::size_t>(t)])];
        out[n++] = LevelView{L.price, L.qty, L.orders};
        t = buy ? scan_down(g, t - 1) : scan_up(g, t + 1);
      } else {
        const Level& L = levels_[static_cast<std::size_t>(oit->second)];
        out[n++] = LevelView{L.price, L.qty, L.orders};
        ++oit;
      }
    }
    return n;
  }

  Price lo_;
  std::size_t ticks_;
  SideGrid side_[2];
  std::map<Price, std::int32_t> over_[2];
  std::vector<Order> orders_;
  std::vector<Level> levels_;
  std::vector<std::int32_t> free_orders_;
  std::vector<std::int32_t> free_levels_;
  OpenHashMap<std::int32_t> ids_;
  std::uint64_t rejected_{0};
  std::uint64_t overflow_ops_{0};
};

} // namespace hotpath::book
