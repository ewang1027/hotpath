#include "hotpath/book/flat_book.hpp"
#include "hotpath/book/hybrid_book.hpp"
#include "hotpath/book/intrusive_book.hpp"
#include "hotpath/book/map_book.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <random>
#include <vector>

using namespace hotpath;
using namespace hotpath::book;

namespace {

std::string describe(const Snapshot& s) {
  std::string out = "\n      bids:";
  for (int i = 0; i < s.nbid; ++i)
    out += " [" + std::to_string(s.bid[i].price) + " x" + std::to_string(s.bid[i].qty) +
           " n" + std::to_string(s.bid[i].orders) + "]";
  out += "\n      asks:";
  for (int i = 0; i < s.nask; ++i)
    out += " [" + std::to_string(s.ask[i].price) + " x" + std::to_string(s.ask[i].qty) +
           " n" + std::to_string(s.ask[i].orders) + "]";
  return out;
}

} // namespace

TEST_CASE("book: basic add / best / snapshot", "[book]") {
  MapBook m;
  m.add(1, Side::Buy, 1000000, 100);
  m.add(2, Side::Buy, 1000100, 200);
  m.add(3, Side::Sell, 1000300, 150);

  REQUIRE(m.best_bid() == 1000100);
  REQUIRE(m.best_ask() == 1000300);

  Snapshot s{};
  m.snapshot(s);
  REQUIRE(s.nbid == 2);
  REQUIRE(s.bid[0].price == 1000100);
  REQUIRE(s.bid[0].qty == 200);
  REQUIRE(s.bid[1].price == 1000000);
  REQUIRE(s.nask == 1);
}

TEST_CASE("book: partial execute reduces, full execute removes", "[book]") {
  MapBook m;
  m.add(1, Side::Buy, 1000000, 100);
  m.execute(1, 30);
  Snapshot s{};
  m.snapshot(s);
  REQUIRE(s.nbid == 1);
  REQUIRE(s.bid[0].qty == 70);
  REQUIRE(s.bid[0].orders == 1);

  m.execute(1, 70);
  m.snapshot(s);
  REQUIRE(s.nbid == 0);
  REQUIRE(m.best_bid() == kInvalidPrice);
}

TEST_CASE("book: replace inherits the original side", "[book]") {
  // ITCH's Order Replace carries no side field -- it only means anything
  // relative to the order being replaced. A design that defaults to Buy here
  // silently corrupts every replaced sell order.
  MapBook m;
  m.add(1, Side::Sell, 1000300, 100);
  m.replace(1, 2, 1000400, 50);
  REQUIRE(m.best_ask() == 1000400);
  REQUIRE(m.best_bid() == kInvalidPrice);

  IntrusiveBook ib;
  ib.add(1, Side::Sell, 1000300, 100);
  ib.replace(1, 2, 1000400, 50);
  REQUIRE(ib.best_ask() == 1000400);
  REQUIRE(ib.best_bid() == kInvalidPrice);

  FlatBook fb(900000, 1100000);
  fb.add(1, Side::Sell, 1000300, 100);
  fb.replace(1, 2, 1000400, 50);
  REQUIRE(fb.best_ask() == 1000400);
  REQUIRE(fb.best_bid() == kInvalidPrice);
}

// The real correctness argument: three independent implementations, driven by
// the same randomised workload, must agree on the full top-of-book snapshot
// after every single event. A disagreement pinpoints the exact operation that
// diverged.
TEST_CASE("book: three designs agree under a random workload", "[book][crossval]") {
  constexpr Price kLo = 900000, kHi = 1100000;   // $90 .. $110

  MapBook a;
  IntrusiveBook b(1u << 16, 1u << 16);   // must cover every distinct price, incl. far-away
  FlatBook c(kLo, kHi, 1u << 16);
  HybridBook d(kLo, kHi, 1u << 16, 1u << 16);

  std::mt19937_64 rng(20260824);
  std::vector<OrderId> live;
  OrderId next_id = 1;

  Snapshot sa{}, sb{}, sc{}, sd{};

  for (int step = 0; step < 120000; ++step) {
    const int op = static_cast<int>(rng() % 100);

    if (op < 45 || live.size() < 4) {
      const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
      // Mostly inside the grid, but deliberately 2% outside it and 1% off the
      // penny, so the flat design's overflow path is actually exercised rather
      // than being dead code the gate never reaches.
      Price px;
      const int r = static_cast<int>(rng() % 100);
      if (r < 2)       px = static_cast<Price>(1 + rng() % 500);              // far below the window
      else if (r < 4)  px = static_cast<Price>(2000000 + rng() % 500000);     // far above
      else if (r < 5)  px = static_cast<Price>(kLo + (rng() % 2000) * 100 + 37); // sub-penny
      else             px = static_cast<Price>(kLo + (rng() % 2000) * 100);
      const Qty qty = static_cast<Qty>(1 + rng() % 500);
      const OrderId id = next_id++;
      a.add(id, side, px, qty);
      b.add(id, side, px, qty);
      c.add(id, side, px, qty);
      d.add(id, side, px, qty);
      live.push_back(id);
    } else {
      const std::size_t idx = rng() % live.size();
      const OrderId id = live[idx];
      const int kind = static_cast<int>(rng() % 100);
      if (kind < 30) {
        const Qty q = static_cast<Qty>(1 + rng() % 200);
        a.execute(id, q); b.execute(id, q); c.execute(id, q); d.execute(id, q);
      } else if (kind < 55) {
        const Qty q = static_cast<Qty>(1 + rng() % 200);
        a.cancel(id, q); b.cancel(id, q); c.cancel(id, q); d.cancel(id, q);
      } else if (kind < 85) {
        a.remove(id); b.remove(id); c.remove(id); d.remove(id);
        live[idx] = live.back(); live.pop_back();
      } else {
        const OrderId nid = next_id++;
        const Price px = static_cast<Price>(kLo + (rng() % 2000) * 100);
        const Qty q = static_cast<Qty>(1 + rng() % 500);
        a.replace(id, nid, px, q);
        b.replace(id, nid, px, q);
        c.replace(id, nid, px, q);
        d.replace(id, nid, px, q);
        live[idx] = nid;
      }
    }

    a.snapshot(sa); b.snapshot(sb); c.snapshot(sc); d.snapshot(sd);
    if (!(sa == sb) || !(sa == sc) || !(sa == sd)) {
      INFO("diverged at step " << step);
      INFO("map      " << describe(sa));
      INFO("intrusive" << describe(sb));
      INFO("flat     " << describe(sc));
      INFO("hybrid   " << describe(sd));
      REQUIRE(sa == sb);
      REQUIRE(sa == sc);
      REQUIRE(sa == sd);
    }
  }

  REQUIRE(a.best_bid() == b.best_bid());
  REQUIRE(a.best_bid() == c.best_bid());
  REQUIRE(a.best_ask() == b.best_ask());
  REQUIRE(a.best_ask() == c.best_ask());
  REQUIRE(a.best_bid() == d.best_bid());
  REQUIRE(a.best_ask() == d.best_ask());
  REQUIRE(c.overflow_ops() > 0);   // the overflow path really was exercised
  // If either bounded design ran out of capacity it would silently drop orders,
  // so a divergence above could be a capacity problem masquerading as a logic
  // bug. Assert it is neither.
  REQUIRE(b.rejected() == 0);
  REQUIRE(c.rejected() == 0);
  REQUIRE(d.rejected() == 0);
}
