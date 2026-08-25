#pragma once
#include "hotpath/book/book.hpp"
#include "hotpath/core/open_hash.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <vector>

namespace hotpath::book {

// Design (c): direct-addressed price grid with a bitmap index.
//
// Price maps to an array slot arithmetically, so add/cancel at a known price is
// O(1) with no search at all, and best bid/ask is a bitmap scan that usually
// terminates in the first word.
//
// The obvious version of this design does not survive contact with real data.
// Measured over one trading day, resting prices span $0.0001 to $199,999 --
// far-away limit orders that sit all day and never trade. A dense array over
// the observed range would be 8 GB per side. Roughly 0.005% of adds are also
// sub-penny, so a penny grid cannot represent them exactly.
//
// So the grid is a bounded *window* -- sized to cover the bulk of real activity
// and small enough to stay cache-resident -- with a std::map overflow for
// everything outside it or off the penny. That hybrid is what production
// systems actually do, and the overflow rate is reported rather than assumed
// negligible.
class FlatBook {
public:
  static constexpr const char* kName = "flat";
  static constexpr Price kTick = 100;   // $0.01 in ITCH's 1/10000 units

  // lo/hi are inclusive price bounds for the dense window.
  FlatBook(Price lo, Price hi, std::size_t max_orders_pow2 = 1u << 20)
      : lo_(lo - (lo % kTick)),
        // hi < lo would underflow this unsigned subtraction into ~42M ticks and
        // silently allocate hundreds of MB of grid rather than failing.
        ticks_(hi < lo - (lo % kTick)
                   ? throw std::invalid_argument("FlatBook: price window is inverted")
                   : static_cast<std::size_t>((hi - (lo - (lo % kTick))) / kTick) + 1),
        ids_(max_orders_pow2) {
    for (int s = 0; s < 2; ++s) {
      g_[s].qty.assign(ticks_, 0);
      g_[s].cnt.assign(ticks_, 0);
      g_[s].bits.assign((ticks_ + 63) / 64, 0);
    }
  }

  void add(OrderId ref, Side side, Price px, Qty qty) {
    if (qty == 0) return;
    // Same rule as the intrusive design: a full id table drops the order, and
    // a dropped order is a silently wrong book. Count it.
    if (!ids_.insert(ref, Rec{px, qty, side})) { ++rejected_; return; }
    deposit(side, px, qty, +1);
  }

  void execute(OrderId ref, Qty qty) { reduce(ref, qty); }
  void cancel(OrderId ref, Qty qty)  { reduce(ref, qty); }

  void remove(OrderId ref) {
    const Rec* r = ids_.find(ref);
    if (!r) return;
    deposit(r->side, r->px, r->qty, -1);
    ids_.erase(ref);
  }

  void replace(OrderId old_ref, OrderId new_ref, Price px, Qty qty) {
    const Rec* r = ids_.find(old_ref);
    if (!r) return;
    const Side side = r->side;
    deposit(side, r->px, r->qty, -1);
    ids_.erase(old_ref);
    add(new_ref, side, px, qty);
  }

  [[nodiscard]] Price best_bid() const noexcept {
    const Price g = grid_best(Side::Buy);
    const Price o = over_[0].empty() ? kInvalidPrice : over_[0].rbegin()->first;
    if (g == kInvalidPrice) return o;
    if (o == kInvalidPrice) return g;
    return g > o ? g : o;
  }
  [[nodiscard]] Price best_ask() const noexcept {
    const Price g = grid_best(Side::Sell);
    const Price o = over_[1].empty() ? kInvalidPrice : over_[1].begin()->first;
    if (g == kInvalidPrice) return o;
    if (o == kInvalidPrice) return g;
    return g < o ? g : o;
  }

  void snapshot(Snapshot& s) const noexcept {
    // Overflow maps are ascending for both sides, so bids walk them backwards.
    s.nbid = collect(Side::Buy,  s.bid, over_[0].rbegin(), over_[0].rend());
    s.nask = collect(Side::Sell, s.ask, over_[1].begin(),  over_[1].end());
  }

  void clear() {
    ids_.clear();
    for (int s = 0; s < 2; ++s) {
      std::fill(g_[s].qty.begin(), g_[s].qty.end(), 0u);
      std::fill(g_[s].cnt.begin(), g_[s].cnt.end(), 0u);
      std::fill(g_[s].bits.begin(), g_[s].bits.end(), 0ull);
      g_[s].hint = -1;
      over_[s].clear();
    }
  }

  [[nodiscard]] std::size_t order_count() const noexcept { return ids_.size(); }
  [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }
  [[nodiscard]] std::uint64_t overflow_ops() const noexcept { return overflow_ops_; }
  [[nodiscard]] std::uint64_t grid_ops() const noexcept { return grid_ops_; }
  [[nodiscard]] std::size_t grid_ticks() const noexcept { return ticks_; }

private:
  struct Rec { Price px; Qty qty; Side side; };
  struct Agg { Qty qty{0}; std::uint32_t orders{0}; };

  struct Grid {
    std::vector<Qty>           qty;
    std::vector<std::uint32_t> cnt;
    std::vector<std::uint64_t> bits;
    std::int64_t               hint{-1};   // cached best tick, -1 = unknown
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

  // delta is +1 for an arriving order, -1 for a departing one; qty is signed by
  // the caller's intent.
  void deposit(Side side, Price px, Qty qty, int delta) {
    const int s = static_cast<int>(side);
    if (!in_grid(px)) {
      ++overflow_ops_;
      auto& m = over_[s];
      auto it = m.find(px);
      if (delta > 0) {
        Agg& a = (it == m.end()) ? m[px] : it->second;
        a.qty += qty; ++a.orders;
      } else if (it != m.end()) {
        it->second.qty -= qty;
        if (--it->second.orders == 0) m.erase(it);
      }
      return;
    }
    ++grid_ops_;
    Grid& g = g_[s];
    const std::size_t t = tick_of(px);
    if (delta > 0) {
      if (g.cnt[t]++ == 0) set_bit(g, t);
      g.qty[t] += qty;
      if (g.hint < 0) g.hint = static_cast<std::int64_t>(t);
      else if (side == Side::Buy) { if (static_cast<std::int64_t>(t) > g.hint) g.hint = static_cast<std::int64_t>(t); }
      else                        { if (static_cast<std::int64_t>(t) < g.hint) g.hint = static_cast<std::int64_t>(t); }
    } else {
      g.qty[t] -= qty;
      if (--g.cnt[t] == 0) {
        clear_bit(g, t);
        if (g.hint == static_cast<std::int64_t>(t)) g.hint = -1;  // force a rescan
      }
    }
  }

  void reduce(OrderId ref, Qty qty) {
    Rec* r = ids_.find(ref);
    if (!r) return;
    const Qty take = qty < r->qty ? qty : r->qty;
    if (take == r->qty) { deposit(r->side, r->px, take, -1); ids_.erase(ref); return; }
    r->qty -= take;
    if (in_grid(r->px)) g_[static_cast<int>(r->side)].qty[tick_of(r->px)] -= take;
    else { auto it = over_[static_cast<int>(r->side)].find(r->px);
           if (it != over_[static_cast<int>(r->side)].end()) it->second.qty -= take; }
  }

  static void set_bit(Grid& g, std::size_t t) noexcept   { g.bits[t >> 6] |=  (1ull << (t & 63)); }
  static void clear_bit(Grid& g, std::size_t t) noexcept { g.bits[t >> 6] &= ~(1ull << (t & 63)); }
  [[nodiscard]] static bool test_bit(const Grid& g, std::size_t t) noexcept {
    return (g.bits[t >> 6] >> (t & 63)) & 1ull;
  }

  // Highest set tick at or below `from` (-1 if none).
  [[nodiscard]] std::int64_t scan_down(const Grid& g, std::int64_t from) const noexcept {
    if (from < 0) return -1;
    std::size_t w = static_cast<std::size_t>(from) >> 6;
    std::uint64_t word = g.bits[w] & (~0ull >> (63 - (static_cast<std::size_t>(from) & 63)));
    while (true) {
      if (word) return static_cast<std::int64_t>((w << 6) + (63 - static_cast<std::size_t>(__builtin_clzll(word))));
      if (w == 0) return -1;
      word = g.bits[--w];
    }
  }
  // Lowest set tick at or above `from` (-1 if none).
  [[nodiscard]] std::int64_t scan_up(const Grid& g, std::int64_t from) const noexcept {
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
    Grid& g = const_cast<Grid&>(g_[static_cast<int>(side)]);
    if (g.hint < 0) {
      g.hint = side == Side::Buy ? scan_down(g, static_cast<std::int64_t>(ticks_) - 1)
                                 : scan_up(g, 0);
      if (g.hint < 0) return kInvalidPrice;
    } else if (!test_bit(g, static_cast<std::size_t>(g.hint))) {
      g.hint = side == Side::Buy ? scan_down(g, g.hint) : scan_up(g, g.hint);
      if (g.hint < 0) return kInvalidPrice;
    }
    return price_of(static_cast<std::size_t>(g.hint));
  }

  // Merge the dense grid and the overflow map in price order. The overflow
  // almost never reaches the top ten, but "almost never" is not a correctness
  // argument -- the cross-validation gate compares this against std::map at
  // every event, so the merge has to be exact.
  template <typename It>
  [[nodiscard]] int collect(Side side, LevelView* out, It oit, It oend) const noexcept {
    const int s = static_cast<int>(side);
    Grid& g = const_cast<Grid&>(g_[s]);
    const bool buy = side == Side::Buy;

    std::int64_t t = buy ? scan_down(g, static_cast<std::int64_t>(ticks_) - 1) : scan_up(g, 0);
    int n = 0;
    while (n < kSnapshotDepth && (t >= 0 || oit != oend)) {
      const bool have_g = t >= 0;
      const bool have_o = oit != oend;
      bool take_grid;
      if (have_g && have_o) {
        const Price gp = price_of(static_cast<std::size_t>(t));
        take_grid = buy ? (gp > oit->first) : (gp < oit->first);
      } else {
        take_grid = have_g;
      }
      if (take_grid) {
        const std::size_t tt = static_cast<std::size_t>(t);
        out[n++] = LevelView{price_of(tt), g.qty[tt], g.cnt[tt]};
        t = buy ? scan_down(g, t - 1) : scan_up(g, t + 1);
      } else {
        out[n++] = LevelView{oit->first, oit->second.qty, oit->second.orders};
        ++oit;
      }
    }
    return n;
  }

  Price lo_;
  std::size_t ticks_;
  Grid g_[2];
  // Both overflow maps are ascending; the bid side simply iterates in reverse.
  // Using two different comparator types here would make them distinct types
  // and defeat indexing them by side.
  std::map<Price, Agg> over_[2];
  OpenHashMap<Rec> ids_;
  std::uint64_t overflow_ops_{0};
  std::uint64_t grid_ops_{0};
  std::uint64_t rejected_{0};
};

} // namespace hotpath::book
